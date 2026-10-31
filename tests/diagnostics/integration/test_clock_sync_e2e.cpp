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
#include "tilexr_straggler.h"

namespace {

struct Options {
    int rankSize = 4;
    int rank = 0;
    int deviceId = -1;
    int rounds = 20;
    std::string outputDir = "clock_sync_e2e";
    bool socketMode = false;
};

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
        if (index + 1 >= argc) {
            return false;
        }
        const std::string argument = argv[index];
        const char *valueText = argv[++index];
        if (argument == "--output-dir") {
            options.outputDir = valueText;
            continue;
        }
        if (argument == "--comm-mode") {
            if (std::string(valueText) == "socket") {
                options.socketMode = true;
            } else if (std::string(valueText) != "local") {
                return false;
            }
            continue;
        }
        int64_t value = 0;
        if (!ParseInteger(valueText, 0, std::numeric_limits<int32_t>::max(), value)) {
            return false;
        }
        if (argument == "--rank-size") {
            options.rankSize = static_cast<int>(value);
        } else if (argument == "--rank") {
            options.rank = static_cast<int>(value);
        } else if (argument == "--device-id") {
            options.deviceId = static_cast<int>(value);
        } else if (argument == "--rounds") {
            options.rounds = static_cast<int>(value);
        } else {
            return false;
        }
    }
    return options.rankSize > 0 && options.rankSize <= TILEXR_STRAGGLER_MAX_RANKS &&
        options.rank >= 0 && options.rank < options.rankSize && options.deviceId >= 0 &&
        options.rounds > 0 && !options.outputDir.empty();
}

uint64_t PercentileNearestRank(std::vector<uint64_t> values, uint32_t numerator,
                               uint32_t denominator)
{
    std::sort(values.begin(), values.end());
    const size_t rank = (values.size() * numerator + denominator - 1u) / denominator;
    return values[rank == 0 ? 0 : rank - 1u];
}

bool WriteSummary(const Options &options, const std::vector<TileXRClockSyncResult> &results)
{
    std::vector<uint64_t> precisions;
    for (const TileXRClockSyncResult &result : results) {
        precisions.push_back(result.precisionNs);
    }
    std::ostringstream path;
    path << options.outputDir << "/clock_rank" << options.rank << ".json";
    std::ofstream output(path.str().c_str(), std::ios::out | std::ios::trunc);
    if (!output.is_open()) {
        return false;
    }
    const TileXRClockSyncResult &last = results.back();
    output << "{\n"
           << "  \"schema\": \"tilexr_clock_sync_e2e.v1\",\n"
           << "  \"rank\": " << options.rank << ",\n"
           << "  \"rank_size\": " << options.rankSize << ",\n"
           << "  \"rounds\": " << results.size() << ",\n"
           << "  \"warmup_samples_per_round\": " << last.warmupSampleCount << ",\n"
           << "  \"measured_samples_per_round\": " << last.sampleCount << ",\n"
           << "  \"cycles_per_second\": " << last.cyclesPerSecond << ",\n"
           << "  \"precision_p50_ns\": " << PercentileNearestRank(precisions, 50, 100) << ",\n"
           << "  \"precision_p95_ns\": " << PercentileNearestRank(precisions, 95, 100) << ",\n"
           << "  \"precision_max_ns\": " << *std::max_element(precisions.begin(), precisions.end()) << ",\n"
           << "  \"round_precisions_ns\": [";
    for (size_t index = 0; index < precisions.size(); ++index) {
        output << precisions[index] << (index + 1u == precisions.size() ? "" : ", ");
    }
    output << "],\n"
           << "  \"last_generation\": " << last.generation << ",\n"
           << "  \"last_rank_bounds_ns\": [";
    for (uint32_t rank = 0; rank < last.rankSize; ++rank) {
        output << last.ranks[rank].errorBoundNs << (rank + 1u == last.rankSize ? "" : ", ");
    }
    output << "]\n}\n";
    return output.good();
}

} // namespace

int main(int argc, char **argv)
{
    Options options;
    if (!ParseOptions(argc, argv, options)) {
        std::cerr << "Usage: " << argv[0]
                  << " --rank-size N --rank R --device-id D --rounds N"
                  << " --output-dir DIR --comm-mode local|socket" << std::endl;
        return 2;
    }

    TileXRCommPtr comm = nullptr;
    TileXRStraggler detector = nullptr;
    aclrtStream stream = nullptr;
    bool initialized = aclInit(nullptr) == ACL_SUCCESS;
    bool deviceSet = initialized && aclrtSetDevice(options.deviceId) == ACL_SUCCESS;
    bool ok = deviceSet && aclrtCreateStream(&stream) == ACL_SUCCESS;
    if (ok && options.socketMode) {
        TileXRUniqueId uniqueId {};
        ok = TileXRGetUniqueId(&uniqueId, 0) == TileXR::TILEXR_SUCCESS &&
            TileXRCommInitRank(uniqueId, options.rankSize, options.rank, &comm) ==
                TileXR::TILEXR_SUCCESS;
    } else if (ok) {
        ok = TileXRCommInitRankLocal(options.rankSize, options.rank, &comm) ==
            TileXR::TILEXR_SUCCESS;
    }

    TileXRStragglerConfig config {};
    if (ok) {
        ok = TileXRStragglerConfigDefault(&config) == TileXR::TILEXR_SUCCESS;
        config.outputDir = nullptr;
        config.maxCalibrationAgeMs = 300000;
        ok = ok && TileXRStragglerCreate(comm, &config, &detector) == TileXR::TILEXR_SUCCESS;
    }

    std::vector<TileXRClockSyncResult> results;
    for (int round = 0; ok && round < options.rounds; ++round) {
        TileXRClockSyncResult result {};
        ok = TileXRClockSynchronize(detector, stream, &result) == TileXR::TILEXR_SUCCESS &&
            result.rankSize == static_cast<uint32_t>(options.rankSize) && result.precisionNs > 0;
        if (ok) {
            results.push_back(result);
        }
    }
    if (ok) {
        ok = WriteSummary(options, results);
    }

    const int destroyStatus = TileXRStragglerDestroy(detector);
    if (comm != nullptr) {
        TileXRCommDestroy(comm);
    }
    if (stream != nullptr) {
        aclrtDestroyStream(stream);
    }
    if (deviceSet) {
        aclrtResetDevice(options.deviceId);
    }
    if (initialized) {
        aclFinalize();
    }
    ok = ok && destroyStatus == TileXR::TILEXR_SUCCESS;
    if (!ok) {
        std::cerr << "[rank " << options.rank << "] clock sync e2e failed" << std::endl;
        return 1;
    }
    const TileXRClockSyncResult &last = results.back();
    std::cout << "[rank " << options.rank << "] PASS clock sync e2e precision_ns="
              << last.precisionNs << " generation=" << last.generation << std::endl;
    return 0;
}
