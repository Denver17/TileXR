/*
 * 功能：执行多 rank clock 校准并导出采样和模型，可选检查 sleep_at 或采集后续 RDMA kernel。
 * 调用关系：runner 为各 rank 启动本程序；程序调用 diagnostics API，输出供 analyzer 读取。
 * 资源与生命周期：程序创建 communicator、stream 和 detector，结束时调用销毁接口；
 *   detector 销毁失败时返回，不继续销毁 communicator 和 stream。
 */
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "acl/acl.h"
#include "tilexr_straggler.h"
#include "straggler_session.h"
#include "sleep_at_rdma_profile.h"

namespace {

struct Options {
    int rankSize = 4;
    int rank = 0;
    int deviceId = -1;
    int rounds = 20;
    int samples = 128;
    int warmup = 32;
    bool freezeCalibration = false;
    bool sleepAtTest = false;
    bool sleepAtRdmaProfile = false;
    int roundGapMs = 0;
    int maintenanceIntervalMs = 60000;
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
        } else if (argument == "--samples") {
            options.samples = static_cast<int>(value);
        } else if (argument == "--warmup") {
            options.warmup = static_cast<int>(value);
        } else if (argument == "--freeze-calibration") {
            if (value > 1) { return false; }
            options.freezeCalibration = value != 0;
        } else if (argument == "--sleep-at-test") {
            if (value > 1) { return false; }
            options.sleepAtTest = value != 0;
        } else if (argument == "--sleep-at-rdma-profile") {
            if (value > 1) { return false; }
            options.sleepAtRdmaProfile = value != 0;
        } else if (argument == "--round-gap-ms") {
            if (value > 10000) { return false; }
            options.roundGapMs = static_cast<int>(value);
        } else if (argument == "--maintenance-interval-ms") {
            if (value < 1 || value > 3600000) { return false; }
            options.maintenanceIntervalMs = static_cast<int>(value);
        } else {
            return false;
        }
    }
    return options.rankSize > 0 && options.rankSize <= TILEXR_STRAGGLER_MAX_RANKS &&
        options.rank >= 0 && options.rank < options.rankSize && options.deviceId >= 0 &&
        options.rounds > 0 && options.samples >= 4 && options.samples <= 256 &&
        options.warmup <= 256 && !options.outputDir.empty() &&
        (!options.freezeCalibration || options.rounds >= 2) &&
        (!options.sleepAtRdmaProfile || options.rankSize == 2);
}

bool WriteFrozenCalibration(const Options &options, const TileXRClockSyncResult &clock)
{
    std::ofstream output(options.outputDir + "/frozen_calibration_rank" +
        std::to_string(options.rank) + ".json");
    output << "{\n  \"schema\": \"tilexr_frozen_clock_calibration.v1\",\n"
           << "  \"calibration_round\": 0,\n  \"validation_first_round\": 1,\n"
           << "  \"cycles_per_second\": " << clock.cyclesPerSecond << ",\n"
           << "  \"generation\": " << clock.generation << ",\n"
           << "  \"correction\": \"root_cycles = local_cycles + offset_cycles\",\n"
           << "  \"offset_model_assumption\": \"symmetric_effective_one_way_delays\",\n"
           << "  \"path_symmetry_verified\": false,\n  \"offset_cycles\": [";
    for (uint32_t rank = 0; rank < clock.rankSize; ++rank) {
        output << clock.ranks[rank].offsetCycles << (rank + 1 == clock.rankSize ? "" : ", ");
    }
    output << "]\n}\n";
    return output.good();
}

uint64_t PercentileNearestRank(std::vector<uint64_t> values, uint32_t numerator,
                               uint32_t denominator)
{
    std::sort(values.begin(), values.end());
    const size_t rank = (values.size() * numerator + denominator - 1u) / denominator;
    return values[rank == 0 ? 0 : rank - 1u];
}

bool AgreeValidation(TileXRCommPtr comm, int rankSize, bool localOk)
{
    if (comm == nullptr) {
        return false;
    }
    const int status = localOk ? TileXR::TILEXR_SUCCESS : TileXR::TILEXR_ERROR_INTERNAL;
    std::vector<int> statuses(static_cast<size_t>(rankSize));
    return TileXRCommHostAllGatherTimeout(comm, &status, sizeof(status), statuses.data(), 5000) ==
        TileXR::TILEXR_SUCCESS && std::all_of(statuses.begin(), statuses.end(),
            [](int item) { return item == TileXR::TILEXR_SUCCESS; });
}

bool WriteRdmaProfile(TileXRDiagnostics::Host::StragglerSession &session,
                      int round, int rank, uint64_t generation, uint64_t frequency,
                      std::ostream &send, std::ostream &receive)
{
    if (!session.clockUsesRdma) { return true; }
    TileXR::TileXRClockRdmaProfile profile {};
    const auto *source = static_cast<const uint8_t *>(session.clockRdmaMemory) +
        TileXR::TILEXR_CLOCK_RDMA_PROFILE_OFFSET;
    if (aclrtMemcpy(&profile, sizeof(profile), source, sizeof(profile),
                    ACL_MEMCPY_DEVICE_TO_HOST) != ACL_SUCCESS) { return false; }
    if (profile.magic == 0) { return true; } // Normal, uninstrumented kernel.
    if (profile.magic != TileXR::TILEXR_CLOCK_RDMA_PROFILE_MAGIC ||
        profile.bytes != sizeof(profile) || profile.generation != generation) { return false; }
    const char *stages[] = {"send_total", "staging", "view_load", "validate_load_state",
        "reserve_store_state", "stop_encode_publish", "wqe_encode", "wqe_publish",
        "doorbell_issue", "doorbell_to_cqe", "cqe_reads_subset", "cqe_decode_subset",
        "retire_cqdb_state", "return_tail"};
    if (profile.reserved[0] != 0) {
        stages[9] = "deferred_cqe_not_polled";
        stages[10] = "deferred_cqe_read_not_applicable";
        stages[11] = "deferred_cqe_decode_not_applicable";
        stages[12] = "deferred_retire_not_applicable";
    }
    for (uint32_t state = 0; state < 5; ++state) {
        const auto &record = profile.send[state];
        if (record.sends != 0) {
            if (record.failures != 0) { return false; }
            for (uint32_t stage = 0; stage < TileXR::TILEXR_CLOCK_RDMA_PROFILE_STAGES; ++stage) {
                send << round << ',' << rank << ',' << state + 1 << ',' << stages[stage] << ','
                    << record.sends << ',' << record.polls << ',' << record.pending << ','
                    << record.failures << ',' << record.sum[stage] << ',' << record.minimum[stage]
                    << ',' << record.maximum[stage] << ',' << frequency << '\n';
            }
        }
        const auto &poll = profile.receive[state];
        if (poll.calls != 0) {
            receive << round << ',' << rank << ',' << state + 1 << ',' << poll.calls << ','
                << poll.matches << ',' << poll.invalidateCycles << ',' << poll.readCycles
                << ',' << frequency << ',' << poll.windowMatches << ',' << poll.matchWindowCycles
                << ',' << poll.minMatchWindowCycles << ',' << poll.maxMatchWindowCycles << '\n';
        }
    }
    if (profile.reserved[0] != 0) {
        send << round << ',' << rank << ",0,round_cq_drain,1,0,0,0,"
            << profile.reserved[1] << ',' << profile.reserved[1] << ','
            << profile.reserved[1] << ',' << frequency << '\n';
    }
    return send.good() && receive.good();
}

bool CheckSleepAt(const Options &options, TileXRCommPtr comm,
                    TileXRStraggler detector, aclrtStream stream)
{
    using namespace TileXRDiagnostics::Host;
    auto &session = *static_cast<StragglerSession *>(detector);
    TileXRClockDriftModel model {};
    bool ok = TileXRClockGetDriftModel(detector,options.rank,&model) == 0 &&
        model.state == TILEXR_CLOCK_DRIFT_READY;
    if (!AgreeValidation(comm,options.rankSize,ok)) { return false; }
    // The timing record is written by the clock kernel on every rank.
    TileXR::TileXRClockSyncDeviceSample timing {};
    const auto *timingAddress = static_cast<const uint8_t *>(session.deviceSamples) +
        options.samples*sizeof(timing);
    ok = aclrtMemcpy(&timing,sizeof(timing),timingAddress,sizeof(timing),
        ACL_MEMCPY_DEVICE_TO_HOST) == ACL_SUCCESS && timing.t3 != 0;
    std::vector<uint64_t> rootCycles(static_cast<size_t>(options.rankSize));
    ok = ok && TileXRCommHostAllGatherTimeout(comm,&timing.t3,sizeof(timing.t3),
        rootCycles.data(),5000) == 0;
    if (!AgreeValidation(comm,options.rankSize,ok)) { return false; }
    std::ofstream evidence(options.outputDir + "/sleep_at_rank" +
        std::to_string(options.rank) + ".jsonl");
    // Probe through the existing clock entry: its timing.t0 is a device cycle
    // read after the wait on this same stream. This adds no production outputs.
    for (int test = 0; test < 3; ++test) {
        // First warms registration with an expired target. Then two future
        // waits are queued without synchronization. Finally retry a past target.
        const int64_t target = test == 1 ? static_cast<int64_t>(rootCycles[0] +
            model.cyclesPerSecond/5) : static_cast<int64_t>(rootCycles[0]);
        const int64_t finalTarget = target + (test == 1 ? model.cyclesPerSecond/50 : 0);
        uint64_t deadline = 0;
        ok = ClockSleepDeadline(model,finalTarget,&deadline) == 0;
        int64_t magic = 0;
        ok = ok && TileXRCommNextMagic(comm,&magic) == 0;
        if (!AgreeValidation(comm,options.rankSize,ok)) { return false; }
        const auto start = std::chrono::steady_clock::now();
        ok = TileXRClockSleepAt(detector,target,stream) == 0;
        if (test == 1) { ok = ok && TileXRClockSleepAt(detector,finalTarget,stream) == 0; }
        const auto submitted = std::chrono::steady_clock::now();
        ok = ok && LaunchClockSyncKernel(session,0,4,magic,
            model.cyclesPerSecond*2,stream) == 0;
        // Drain even after a launch error before reporting it to peer ranks.
        const bool drained = aclrtSynchronizeStream(stream) == ACL_SUCCESS;
        ok = ok && drained;
        const auto finished = std::chrono::steady_clock::now();
        const auto *probeTiming = static_cast<const uint8_t *>(session.deviceSamples) + 4*sizeof(timing);
        ok = ok && aclrtMemcpy(&timing,sizeof(timing),probeTiming,sizeof(timing),
            ACL_MEMCPY_DEVICE_TO_HOST) == ACL_SUCCESS &&
            timing.status == TileXR::TILEXR_CLOCK_SYNC_SAMPLE_SUCCESS && timing.t0 >= deadline;
        int64_t aligned = 0;
        ok = ok && TileXRClockCorrectCycle(&model,timing.t0,&aligned) == 0 && aligned >= finalTarget;
        const auto enqueueNs = std::chrono::duration_cast<std::chrono::nanoseconds>(submitted-start).count();
        const auto totalNs = std::chrono::duration_cast<std::chrono::nanoseconds>(finished-start).count();
        // 220ms future wait leaves a broad margin for Host jitter while still
        // detecting a blocking Host implementation or a missing device wait.
        ok = ok && (test != 1 || (enqueueNs < 100000000 && totalNs >= 100000000)) &&
            (test == 1 || totalNs < 100000000);
        evidence << "{\"case\":" << test << ",\"rank\":" << options.rank
            << ",\"target\":" << finalTarget << ",\"local_deadline\":" << deadline
            << ",\"next_kernel_start\":" << timing.t0 << ",\"corrected_start\":" << aligned
            << ",\"enqueue_ns\":" << enqueueNs << ",\"total_ns\":" << totalNs
            << ",\"model_generation\":" << model.trainedAtGeneration
            << ",\"pass\":" << (ok ? "true" : "false") << "}\n";
        evidence.flush();
        std::cout << "[rank " << options.rank << "] sleep_at case=" << test
            << " pass=" << ok << " enqueue_ns=" << enqueueNs << " total_ns=" << totalNs << '\n';
        if (!AgreeValidation(comm,options.rankSize,ok && evidence.good())) { return false; }
        ok = TileXRCommHostAllGatherTimeout(comm,&timing.t3,sizeof(timing.t3),
            rootCycles.data(),5000) == 0;
        if (!AgreeValidation(comm,options.rankSize,ok)) { return false; }
    }
    return true;
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
           << "  \"precision_semantics\": \"max_rank_half_min_effective_rtt\",\n"
           << "  \"offset_model_assumption\": \"symmetric_effective_one_way_delays\",\n"
           << "  \"path_symmetry_verified\": false,\n"
           << "  \"measured_residual_ns\": null,\n"
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
    if (std::getenv("RDMA_BENCH_CPU_ONLY") != nullptr) {
        std::cerr << "CPU-only mode forbids clock NPU execution" << std::endl;
        return 2;
    }
    Options options;
    if (!ParseOptions(argc, argv, options)) {
        std::cerr << "Usage: " << argv[0]
                  << " --rank-size N --rank R --device-id D --rounds N"
                  << " --samples N --warmup N --output-dir DIR --comm-mode local|socket"
                  << " --freeze-calibration 0|1 --round-gap-ms N --sleep-at-test 0|1"
                  << " --sleep-at-rdma-profile 0|1" << std::endl;
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
        const char *tracking = std::getenv("TILEXR_CLOCK_DRIFT_TRACK");
        if (tracking != nullptr && std::string(tracking) == "1") {
            config.outputDir = options.outputDir.c_str();
        }
        config.clockSyncSamples = static_cast<uint32_t>(options.samples);
        config.clockSyncWarmupSamples = static_cast<uint32_t>(options.warmup);
        config.maxCalibrationAgeMs = 300000;
        ok = ok && TileXRStragglerCreate(comm, &config, &detector) == TileXR::TILEXR_SUCCESS;
    }

    std::vector<TileXRClockSyncResult> results;
    TileXRClockSyncResult frozenClock {};
    uint64_t frozenCalibrationHostNs = 0;
    std::ofstream metrics(options.outputDir + "/metrics_rank" + std::to_string(options.rank) + ".csv");
    std::ofstream raw(options.outputDir + "/samples_rank" + std::to_string(options.rank) + ".csv");
    std::ofstream queues(options.outputDir + "/queues_rank" + std::to_string(options.rank) + ".csv");
    std::ofstream drift(options.outputDir + "/drift_models_rank" + std::to_string(options.rank) + ".csv");
    std::ofstream corrected(options.outputDir + "/corrected_samples_rank" + std::to_string(options.rank) + ".csv");
    std::ofstream sendProfile(options.outputDir + "/send_profile_rank" + std::to_string(options.rank) + ".csv");
    std::ofstream pollProfile(options.outputDir + "/poll_profile_rank" + std::to_string(options.rank) + ".csv");
    metrics << "round,rank,peer,host_ns,kernel_cycles,cycles_per_second,min_rtt_ns,median_rtt_ns,error_bound_ns,valid_samples,rejected_samples,offset_ns\n";
    raw << "round,rank,sequence,status,t0,t1,t2,t3,rtt_cycles,applied_offset_cycles\n";
    queues << "round,rank,peer,sq_depth,cq_depth,producer,completed,cq_consumer,error,syndrome,expected,qpn,cqn,sq_entry_bytes,cq_entry_bytes,max_send_wr,max_send_sge,db_mode,queue_abi\n";
    drift << "round,rank,peer,version,state,anchor_cycle,offset_cycles,rate_correction,span_cycles,cycles_per_second,sample_count,rejected_samples,fit_rms_cycles,rate_standard_error,requested_window_us,trained_at_generation\n";
    corrected << "round,rank,sequence,state,trained_at_generation,corrected_t0,corrected_t3\n";
    drift << std::setprecision(17);
    sendProfile << "round,rank,state,stage,sends,polls,pending,failures,cycles_sum,cycles_min,cycles_max,cycles_per_second\n";
    pollProfile << "round,rank,state,calls,matches,invalidate_cycles,read_cycles,cycles_per_second,"
        "window_matches,match_window_cycles,min_match_window_cycles,max_match_window_cycles\n";
    ok = ok && metrics.good() && raw.good() && queues.good() && drift.good() && corrected.good() &&
        sendProfile.good() && pollProfile.good();
    ok = AgreeValidation(comm, options.rankSize, ok);
    for (int round = 0; ok && round < options.rounds; ++round) {
        if (round != 0) {
            const auto *session = static_cast<TileXRDiagnostics::Host::StragglerSession *>(detector);
            const bool maintained = session->clockDrift.tracking &&
                std::all_of(session->clockDriftModels.begin(),session->clockDriftModels.end(),
                    [](const TileXRClockDriftModel &m) { return m.state == TILEXR_CLOCK_DRIFT_READY; });
            std::this_thread::sleep_for(std::chrono::milliseconds(
                maintained ? options.maintenanceIntervalMs : options.roundGapMs));
        }
        TileXRClockSyncResult result {};
        const auto start = std::chrono::steady_clock::now();
        const int status = TileXRClockSynchronize(detector, stream, &result);
        const auto hostNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - start).count();
        ok = status == TileXR::TILEXR_SUCCESS &&
            result.rankSize == static_cast<uint32_t>(options.rankSize) && result.precisionNs > 0;
        if (ok) {
            auto *session = static_cast<TileXRDiagnostics::Host::StragglerSession *>(detector);
            if (options.freezeCalibration) {
                if (round == 0) {
                    frozenClock = result;
                    frozenCalibrationHostNs = session->calibrationHostNs;
                    ok = WriteFrozenCalibration(options, frozenClock);
                } else {
                    // Reuse the sampler, but never use its candidate estimate as a new
                    // correction. Keep the first calibration fixed for held-out validation.
                    session->clock = frozenClock;
                    session->calibrationHostNs = frozenCalibrationHostNs;
                }
            }
            std::vector<TileXR::TileXRClockSyncDeviceSample> samples(options.samples + 1);
            ok = ok && aclrtMemcpy(samples.data(), samples.size() * sizeof(samples[0]),
                session->deviceSamples, samples.size() * sizeof(samples[0]),
                ACL_MEMCPY_DEVICE_TO_HOST) == ACL_SUCCESS;
            const auto &timing = samples.back();
            ok = ok && timing.sequence == static_cast<uint32_t>(options.samples) &&
                timing.status == TileXR::TILEXR_CLOCK_SYNC_SAMPLE_SUCCESS && timing.t3 > timing.t0;
            ok = ok && WriteRdmaProfile(*session, round, options.rank, timing.reserved,
                result.cyclesPerSecond, sendProfile, pollProfile);
            if (session->clockUsesRdma) {
                for (int peer = 0; ok && peer < options.rankSize; ++peer) {
                    if (peer == options.rank || (options.rank != 0 && peer != 0)) {
                        continue;
                    }
                    const auto &view = session->clockRdmaViews[peer];
                    TileXRRDMADeviceState state {};
                    const uint64_t expected = static_cast<uint64_t>(round + 1) *
                        (options.samples + options.warmup) * (options.rank == 0 ?
                            (session->clockRdmaCompact ? 2u : 3u) :
                            (session->clockRdmaCompact ? 1u : 2u));
                    ok = aclrtMemcpy(&state, sizeof(state), reinterpret_cast<const void *>(view.queue.state),
                        sizeof(state), ACL_MEMCPY_DEVICE_TO_HOST) == ACL_SUCCESS;
                    queues << round << ',' << options.rank << ',' << peer << ',' << view.queue.sqDepth << ','
                        << view.queue.cqDepth << ',' << state.producer << ',' << state.completed << ','
                        << state.cqConsumer << ',' << state.error << ',' << state.syndrome << ',' << expected
                        << ',' << view.queue.qpn << ',' << view.queue.cqn
                        << ',' << view.queue.sqEntryBytes << ',' << view.queue.cqEntryBytes
                        << ',' << view.queue.maxSendWr << ',' << view.queue.maxSendSge
                        << ',' << view.queue.dbMode << ',' << view.queue.queueAbi << '\n';
                    ok = ok && state.error == 0 && state.producer == expected &&
                        state.completed == expected && state.cqConsumer == expected;
                }
            }
            for (uint32_t peer = 0; ok && peer < result.rankSize; ++peer) {
                const auto &item = result.ranks[peer];
                metrics << round << ',' << options.rank << ',' << peer << ',' << hostNs << ','
                    << timing.t3 - timing.t0 << ',' << result.cyclesPerSecond << ','
                    << item.minRoundTripNs << ',' << item.medianRoundTripNs << ','
                    << item.errorBoundNs << ',' << item.validSamples << ','
                    << item.rejectedSamples << ',' << item.offsetNs << '\n';
                TileXRClockDriftModel model {};
                ok = TileXRClockGetDriftModel(detector,static_cast<int>(peer),&model) == TileXR::TILEXR_SUCCESS;
                drift << round << ',' << options.rank << ',' << peer << ',' << model.version << ','
                    << model.state << ',' << model.anchorCycle << ',' << model.offsetCycles << ','
                    << model.rateCorrection << ',' << model.spanCycles << ',' << model.cyclesPerSecond << ','
                    << model.sampleCount << ',' << model.rejectedSamples << ',' << model.fitRmsCycles << ','
                    << model.rateStandardError << ',' << model.requestedWindowUs << ','
                    << model.trainedAtGeneration << '\n';
            }
            TileXRClockDriftModel localModel {};
            ok = ok && TileXRClockGetDriftModel(detector,options.rank,&localModel) == TileXR::TILEXR_SUCCESS;
            for (int index = 0; ok && index < options.samples; ++index) {
                const auto &item = samples[index];
                raw << round << ',' << options.rank << ',' << item.sequence << ',' << item.status
                    << ',' << item.t0 << ',' << item.t1 << ',' << item.t2 << ',' << item.t3
                    << ',' << item.roundTripCycles << ','
                    << (options.freezeCalibration ? frozenClock : result).ranks[options.rank].offsetCycles
                    << '\n';
                int64_t alignedT0 = 0, alignedT3 = 0;
                if (localModel.state == TILEXR_CLOCK_DRIFT_READY && options.rank != 0) {
                    ok = TileXRClockCorrectCycle(&localModel,item.t0,&alignedT0) == TileXR::TILEXR_SUCCESS &&
                        TileXRClockCorrectCycle(&localModel,item.t3,&alignedT3) == TileXR::TILEXR_SUCCESS;
                }
                corrected << round << ',' << options.rank << ',' << item.sequence << ',' << localModel.state
                    << ',' << localModel.trainedAtGeneration << ',' << alignedT0 << ',' << alignedT3 << '\n';
            }
            results.push_back(result);
            std::cout << "[rank " << options.rank << "] round=" << round << " status=" << status
                << " host_ns=" << hostNs << " kernel_cycles=" << timing.t3 - timing.t0 << std::endl;
        } else {
            std::cerr << "[rank " << options.rank << "] round=" << round << " status=" << status << std::endl;
        }
        metrics.flush();
        raw.flush();
        queues.flush();
        drift.flush();
        corrected.flush();
        sendProfile.flush();
        pollProfile.flush();
        ok = AgreeValidation(comm, options.rankSize, ok && metrics.good() && raw.good() && queues.good() &&
            drift.good() && corrected.good() &&
            sendProfile.good() && pollProfile.good());
    }
    if (ok && options.sleepAtTest) {
        ok = CheckSleepAt(options, comm, detector, stream);
    }
    if (ok && options.sleepAtRdmaProfile) {
        ok = RunSleepAtRdmaProfile(comm, detector, stream, options.rank, options.deviceId, options.outputDir);
    }
    if (ok) {
        ok = WriteSummary(options, results);
    }
    if (comm != nullptr) {
        ok = AgreeValidation(comm, options.rankSize, ok);
    }

    const int destroyStatus = TileXRStragglerDestroy(detector);
    if (destroyStatus != TileXR::TILEXR_SUCCESS) {
        std::cerr << "[rank " << options.rank << "] clock destroy failed, status=" << destroyStatus
            << "; retaining resources until process exit" << std::endl;
        return 1;
    }
    if (comm != nullptr) {
        ok = TileXRCommDestroy(comm) == TileXR::TILEXR_SUCCESS && ok;
    }
    if (stream != nullptr) {
        ok = aclrtDestroyStream(stream) == ACL_SUCCESS && ok;
    }
    if (initialized) {
        ok = aclFinalize() == ACL_SUCCESS && ok;
    }
    ok = ok && destroyStatus == TileXR::TILEXR_SUCCESS;
    if (!ok) {
        std::cerr << "[rank " << options.rank << "] clock sync e2e failed" << std::endl;
        return 1;
    }
    const TileXRClockSyncResult &last = results.back();
    std::cout << "[rank " << options.rank << "] PASS clock sync e2e model_bound_ns="
              << last.precisionNs << " generation=" << last.generation << std::endl;
    return 0;
}
