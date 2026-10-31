#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

#include "acl/acl.h"
#include "straggler_session.h"
#include "tilexr_straggler.h"
#include "tilexr_collectives.h"

namespace {

struct Options {
    int rankSize = 4;
    int rank = 0;
    int firstNpu = 0;
    int deviceId = -1;
    int clockRounds = 20;
    int measuredRounds = 3;
    int64_t smallCount = 262144;
    int64_t largeCount = 4194304;
    uint64_t delayCycles = 5000000;
    std::string outputDir = "collective_straggler_e2e";
    bool socketMode = false;
    bool clockOnly = false;
};

struct ScenarioResult {
    std::string name;
    int expectedSlowRank = -1;
    std::vector<TileXRStragglerReport> reports;
};

bool CheckAcl(int rank, const char *step, aclError status)
{
    if (status == ACL_SUCCESS) {
        return true;
    }
    std::cerr << "[rank " << rank << "] " << step << " failed, status=" << status << std::endl;
    return false;
}

bool CheckTileXR(int rank, const char *step, int status)
{
    if (status == TileXR::TILEXR_SUCCESS) {
        return true;
    }
    std::cerr << "[rank " << rank << "] " << step << " failed, status=" << status << std::endl;
    return false;
}

bool ParseInteger(const char *text, int64_t minimum, int64_t maximum, int64_t &value)
{
    if (text == nullptr || text[0] == '\0') {
        return false;
    }
    char *end = nullptr;
    const long long parsed = std::strtoll(text, &end, 10);
    if (end == text || *end != '\0' || parsed < minimum || parsed > maximum) {
        return false;
    }
    value = static_cast<int64_t>(parsed);
    return true;
}

bool ParseOptions(int argc, char **argv, Options &options)
{
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        if (index + 1 >= argc) {
            return false;
        }
        const char *text = argv[++index];
        int64_t value = 0;
        if (argument == "--output-dir") {
            options.outputDir = text;
            continue;
        }
        if (argument == "--comm-mode") {
            if (std::string(text) == "socket") {
                options.socketMode = true;
            } else if (std::string(text) != "local") {
                return false;
            }
            continue;
        }
        if (!ParseInteger(text, 0, std::numeric_limits<int32_t>::max(), value)) {
            return false;
        }
        if (argument == "--rank-size") {
            options.rankSize = static_cast<int>(value);
        } else if (argument == "--rank") {
            options.rank = static_cast<int>(value);
        } else if (argument == "--first-npu") {
            options.firstNpu = static_cast<int>(value);
        } else if (argument == "--device-id") {
            options.deviceId = static_cast<int>(value);
        } else if (argument == "--clock-rounds") {
            options.clockRounds = static_cast<int>(value);
        } else if (argument == "--measured-rounds") {
            options.measuredRounds = static_cast<int>(value);
        } else if (argument == "--small-count") {
            options.smallCount = value;
        } else if (argument == "--large-count") {
            options.largeCount = value;
        } else if (argument == "--delay-cycles") {
            options.delayCycles = static_cast<uint64_t>(value);
        } else if (argument == "--clock-only") {
            options.clockOnly = value != 0;
        } else {
            return false;
        }
    }
    return options.rankSize > 0 && options.rankSize <= TILEXR_STRAGGLER_MAX_RANKS &&
        options.rank >= 0 && options.rank < options.rankSize && options.firstNpu >= 0 &&
        options.deviceId >= -1 && options.clockRounds >= 20 && options.measuredRounds >= 1 &&
        options.smallCount > 0 && options.largeCount >= options.smallCount &&
        options.delayCycles > 0 && !options.outputDir.empty() &&
        (options.clockOnly || options.rankSize == 4);
}

uint64_t PercentileNearestRank(std::vector<uint64_t> values, uint32_t numerator, uint32_t denominator)
{
    if (values.empty() || denominator == 0) {
        return 0;
    }
    std::sort(values.begin(), values.end());
    const size_t rank = (values.size() * numerator + denominator - 1) / denominator;
    return values[rank == 0 ? 0 : rank - 1];
}

bool CheckAllReduceOutput(const Options &options, const std::vector<int32_t> &output, int iteration)
{
    int64_t expected = 0;
    for (int rank = 0; rank < options.rankSize; ++rank) {
        expected += static_cast<int64_t>(rank + 1) * 1000 + iteration;
    }
    for (size_t index = 0; index < output.size(); ++index) {
        if (output[index] != expected) {
            std::cerr << "[rank " << options.rank << "] AllReduce mismatch at " << index
                      << ", expected=" << expected << ", actual=" << output[index] << std::endl;
            return false;
        }
    }
    return true;
}

bool RunAllReduce(const Options &options, TileXRCommPtr comm, aclrtStream stream,
                  int32_t *deviceInput, int32_t *deviceOutput, int64_t count, int iteration)
{
    std::vector<int32_t> input(static_cast<size_t>(count),
        static_cast<int32_t>((options.rank + 1) * 1000 + iteration));
    std::vector<int32_t> output(static_cast<size_t>(count), 0);
    const size_t bytes = static_cast<size_t>(count) * sizeof(int32_t);
    return CheckAcl(options.rank, "copy AllReduce input",
               aclrtMemcpy(deviceInput, bytes, input.data(), bytes, ACL_MEMCPY_HOST_TO_DEVICE)) &&
        CheckTileXR(options.rank, "TileXRAllReduce",
               TileXRAllReduce(deviceInput, deviceOutput, count, TileXR::TILEXR_DATA_TYPE_INT32,
                               TileXR::TILEXR_REDUCE_SUM, comm, stream)) &&
        CheckAcl(options.rank, "synchronize AllReduce", aclrtSynchronizeStream(stream)) &&
        CheckAcl(options.rank, "copy AllReduce output",
               aclrtMemcpy(output.data(), bytes, deviceOutput, bytes, ACL_MEMCPY_DEVICE_TO_HOST)) &&
        CheckAllReduceOutput(options, output, iteration);
}

bool ValidateReport(const Options &options, const TileXRStragglerReport &report,
                    int expectedSlowRank, const std::string &scenario)
{
    if (report.complete == 0 || report.rankSize != static_cast<uint32_t>(options.rankSize) ||
        report.componentKind != TILEXR_STRAGGLER_COMPONENT_COLLECTIVES ||
        report.operationKind != static_cast<uint32_t>(TileXR::TileXRType::ALL_REDUCE)) {
        std::cerr << "[rank " << options.rank << "] incomplete report for " << scenario << std::endl;
        return false;
    }
    for (int rank = 0; rank < options.rankSize; ++rank) {
        const uint32_t classification = report.ranks[rank].classification;
        const uint32_t expected = rank == expectedSlowRank ? TILEXR_STRAGGLER_SLOW : TILEXR_STRAGGLER_NORMAL;
        if (classification != expected) {
            std::cerr << "[rank " << options.rank << "] " << scenario << " rank " << rank
                      << " classification=" << classification << ", expected=" << expected
                      << ", duration_ns=" << report.ranks[rank].durationNs
                      << ", pre_communication_ns=" << report.ranks[rank].preCommunicationNs
                      << std::endl;
            return false;
        }
    }
    return true;
}

bool ObserveAllReduce(const Options &options, TileXRStraggler detector,
                      TileXRCommPtr comm, aclrtStream stream, int32_t *deviceInput,
                      int32_t *deviceOutput, int64_t count, int iteration,
                      int expectedSlowRank, const std::string &scenario,
                      TileXRStragglerReport &report)
{
    return RunAllReduce(options, comm, stream, deviceInput, deviceOutput, count, iteration) &&
        CheckTileXR(options.rank, "TileXRStragglerCollect",
            TileXRStragglerCollect(detector, stream, &report)) &&
        ValidateReport(options, report, expectedSlowRank, scenario);
}

bool WriteSummary(const Options &options, const std::vector<TileXRClockSyncResult> &clocks,
                  const std::vector<ScenarioResult> &scenarios)
{
    if (options.rank != 0 || clocks.empty()) {
        return true;
    }
    std::vector<uint64_t> precision;
    for (const TileXRClockSyncResult &clock : clocks) {
        precision.push_back(clock.precisionNs);
    }
    std::ofstream output((options.outputDir + "/summary.json").c_str(),
                         std::ios::out | std::ios::trunc);
    if (!output.is_open()) {
        return false;
    }
    const TileXRClockSyncResult &lastClock = clocks.back();
    output << "{\n"
           << "  \"schema\": \"tilexr_collective_straggler_e2e.v1\",\n"
           << "  \"rank_size\": " << options.rankSize << ",\n"
           << "  \"npu_ids\": [";
    for (int rank = 0; rank < options.rankSize; ++rank) {
        output << options.firstNpu + rank << (rank + 1 == options.rankSize ? "" : ", ");
    }
    output << "],\n"
           << "  \"clock_rounds\": " << clocks.size() << ",\n"
           << "  \"clock_warmup_samples_per_round\": " << lastClock.warmupSampleCount << ",\n"
           << "  \"clock_samples_per_round\": " << lastClock.sampleCount << ",\n"
           << "  \"allreduce_correct\": true,\n"
           << "  \"cycles_per_second\": " << lastClock.cyclesPerSecond << ",\n"
           << "  \"frequency_source\": \"cann_9_1_product_rule\",\n"
           << "  \"precision_p50_ns\": " << PercentileNearestRank(precision, 50, 100) << ",\n"
           << "  \"precision_p95_ns\": " << PercentileNearestRank(precision, 95, 100) << ",\n"
           << "  \"precision_max_ns\": " << *std::max_element(precision.begin(), precision.end()) << ",\n"
           << "  \"clock_ranks\": [\n";
    for (int rank = 0; rank < options.rankSize; ++rank) {
        const TileXRClockRankResult &item = lastClock.ranks[rank];
        output << "    {\"rank\": " << rank
               << ", \"offset_ns\": " << item.offsetNs
               << ", \"min_rtt_ns\": " << item.minRoundTripNs
               << ", \"median_rtt_ns\": " << item.medianRoundTripNs
               << ", \"error_bound_ns\": " << item.errorBoundNs << "}"
               << (rank + 1 == options.rankSize ? "\n" : ",\n");
    }
    output << "  ],\n  \"scenarios\": [\n";
    for (size_t scenarioIndex = 0; scenarioIndex < scenarios.size(); ++scenarioIndex) {
        const ScenarioResult &scenario = scenarios[scenarioIndex];
        output << "    {\"name\": \"" << scenario.name << "\", \"expected_slow_rank\": "
               << scenario.expectedSlowRank << ", \"reports\": [";
        for (size_t reportIndex = 0; reportIndex < scenario.reports.size(); ++reportIndex) {
            const TileXRStragglerReport &report = scenario.reports[reportIndex];
            output << "{\"launch_id\": " << report.launchId << ", \"median_duration_ns\": "
                   << report.medianDurationNs << ", \"classifications\": [";
            for (uint32_t rank = 0; rank < report.rankSize; ++rank) {
                output << report.ranks[rank].classification
                       << (rank + 1 == report.rankSize ? "" : ", ");
            }
            output << "], \"reason_flags\": [";
            for (uint32_t rank = 0; rank < report.rankSize; ++rank) {
                output << report.ranks[rank].reasonFlags
                       << (rank + 1 == report.rankSize ? "" : ", ");
            }
            output << "], \"pre_communication_ns\": [";
            for (uint32_t rank = 0; rank < report.rankSize; ++rank) {
                output << report.ranks[rank].preCommunicationNs
                       << (rank + 1 == report.rankSize ? "" : ", ");
            }
            output << "]}" << (reportIndex + 1 == scenario.reports.size() ? "" : ", ");
        }
        output << "]}" << (scenarioIndex + 1 == scenarios.size() ? "\n" : ",\n");
    }
    output << "  ]\n}\n";
    return output.good();
}

void Cleanup(TileXRStraggler detector, TileXRCommPtr comm, aclrtStream stream,
             int deviceId, bool deviceSet, int32_t *deviceInput, int32_t *deviceOutput)
{
    TileXRStragglerDestroy(detector);
    if (deviceInput != nullptr) {
        aclrtFree(deviceInput);
    }
    if (deviceOutput != nullptr) {
        aclrtFree(deviceOutput);
    }
    if (comm != nullptr) {
        TileXRCommDestroy(comm);
    }
    if (stream != nullptr) {
        aclrtDestroyStream(stream);
    }
    if (deviceSet) {
        aclrtResetDevice(deviceId);
    }
    aclFinalize();
}

} // namespace

int main(int argc, char **argv)
{
    Options options;
    if (!ParseOptions(argc, argv, options)) {
        std::cerr << "Usage: " << argv[0]
                  << " --rank-size N --rank R --first-npu 0 --output-dir DIR"
                  << " [--clock-rounds 20 --measured-rounds 3 --small-count N --large-count N"
                  << " --delay-cycles N --device-id D --comm-mode local|socket --clock-only 0|1]"
                  << std::endl;
        return 2;
    }

    const int deviceId = options.deviceId >= 0 ? options.deviceId : options.firstNpu + options.rank;
    TileXRCommPtr comm = nullptr;
    aclrtStream stream = nullptr;
    TileXRStraggler detector = nullptr;
    int32_t *deviceInput = nullptr;
    int32_t *deviceOutput = nullptr;
    bool deviceSet = false;
    bool ok = CheckAcl(options.rank, "aclInit", aclInit(nullptr));
    if (ok) {
        ok = CheckAcl(options.rank, "aclrtSetDevice", aclrtSetDevice(deviceId));
        deviceSet = ok;
    }
    if (ok) {
        ok = CheckAcl(options.rank, "aclrtCreateStream", aclrtCreateStream(&stream));
    }
    if (ok && options.socketMode) {
        TileXRUniqueId uniqueId {};
        ok = CheckTileXR(options.rank, "TileXRGetUniqueId", TileXRGetUniqueId(&uniqueId, 0)) &&
            CheckTileXR(options.rank, "TileXRCommInitRank",
                TileXRCommInitRank(uniqueId, options.rankSize, options.rank, &comm));
    } else if (ok) {
        ok = CheckTileXR(options.rank, "TileXRCommInitRankLocal",
            TileXRCommInitRankLocal(options.rankSize, options.rank, &comm));
    }

    TileXRStragglerConfig config {};
    if (ok) {
        ok = CheckTileXR(options.rank, "TileXRStragglerConfigDefault",
            TileXRStragglerConfigDefault(&config));
        config.outputDir = options.outputDir.c_str();
        config.absoluteThresholdNs = 1000000;
        config.maxCalibrationAgeMs = 300000;
        ok = ok && CheckTileXR(options.rank, "TileXRStragglerCreate",
            TileXRStragglerCreate(comm, &config, &detector));
    }

    std::vector<TileXRClockSyncResult> clocks;
    for (int round = 0; ok && round < options.clockRounds; ++round) {
        TileXRClockSyncResult clock {};
        ok = CheckTileXR(options.rank, "TileXRClockSynchronize",
            TileXRClockSynchronize(detector, stream, &clock));
        if (ok) {
            clocks.push_back(clock);
        }
    }

    if (options.clockOnly) {
        const std::vector<ScenarioResult> scenarios;
        if (ok) {
            ok = WriteSummary(options, clocks, scenarios);
        }
        Cleanup(detector, comm, stream, deviceId, deviceSet, deviceInput, deviceOutput);
        if (ok) {
            std::cout << "[rank " << options.rank << "] PASS collective clock sync e2e" << std::endl;
        }
        return ok ? 0 : 1;
    }

    const size_t maxBytes = static_cast<size_t>(options.largeCount) * sizeof(int32_t);
    if (ok) {
        ok = CheckAcl(options.rank, "allocate AllReduce input",
                aclrtMalloc(reinterpret_cast<void **>(&deviceInput), maxBytes, ACL_MEM_MALLOC_HUGE_FIRST)) &&
            CheckAcl(options.rank, "allocate AllReduce output",
                aclrtMalloc(reinterpret_cast<void **>(&deviceOutput), maxBytes, ACL_MEM_MALLOC_HUGE_FIRST));
    }

    for (int warmup = 0; ok && warmup < 3; ++warmup) {
        TileXRStragglerReport warmupReport {};
        ok = ObserveAllReduce(options, detector, comm, stream, deviceInput, deviceOutput,
            options.largeCount, -100 - warmup, -1, "normal_warmup", warmupReport);
    }
    std::vector<ScenarioResult> scenarios;
    ScenarioResult normal;
    normal.name = "normal_small_large";
    normal.expectedSlowRank = -1;
    if (ok) {
        TileXRStragglerReport report {};
        ok = ObserveAllReduce(options, detector, comm, stream, deviceInput, deviceOutput,
            options.smallCount, 1, -1, normal.name, report);
        if (ok) {
            normal.reports.push_back(report);
        }
    }
    if (ok) {
        TileXRStragglerReport report {};
        ok = ObserveAllReduce(options, detector, comm, stream, deviceInput, deviceOutput,
            options.largeCount, 2, -1, normal.name, report);
        if (ok) {
            normal.reports.push_back(report);
        }
    }
    scenarios.push_back(normal);

    const int slowRanks[] = {1, 2};
    for (int scenarioIndex = 0; ok && scenarioIndex < 2; ++scenarioIndex) {
        const int slowRank = slowRanks[scenarioIndex];
        std::ostringstream name;
        name << "injected_rank" << slowRank;
        ScenarioResult injected;
        injected.name = name.str();
        injected.expectedSlowRank = slowRank;
        TileXRDiagnostics::Host::SetStragglerTestDelayForHost(
            static_cast<TileXRDiagnostics::Host::StragglerSession *>(detector),
            slowRank, options.delayCycles);

        TileXRStragglerReport warmupReport {};
        ok = ObserveAllReduce(options, detector, comm, stream, deviceInput, deviceOutput,
            options.largeCount, 10 + scenarioIndex * 10, slowRank,
            injected.name + "_warmup", warmupReport);
        for (int round = 0; ok && round < options.measuredRounds; ++round) {
            TileXRStragglerReport report {};
            ok = ObserveAllReduce(options, detector, comm, stream, deviceInput, deviceOutput,
                options.largeCount, 11 + scenarioIndex * 10 + round, slowRank,
                injected.name, report);
            if (ok) {
                injected.reports.push_back(report);
            }
        }
        scenarios.push_back(injected);
    }
    TileXRDiagnostics::Host::SetStragglerTestDelayForHost(
        static_cast<TileXRDiagnostics::Host::StragglerSession *>(detector), -1, 0);

    if (ok) {
        ok = WriteSummary(options, clocks, scenarios);
    }
    Cleanup(detector, comm, stream, deviceId, deviceSet, deviceInput, deviceOutput);
    if (ok) {
        std::cout << "[rank " << options.rank << "] PASS straggler e2e" << std::endl;
    }
    return ok ? 0 : 1;
}
