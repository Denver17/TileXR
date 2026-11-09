/*
 * 功能：创建诊断 session，校验 collective 调用，执行 clock 采样、模型交换、文件写入和观测收集。
 * 调用关系：实现 tilexr_straggler.h 的会话接口，调用 clock kernel、drift 拟合和记录分析模块。
 * 资源与生命周期：session 借用 communicator 和 stream，持有采样/记录及可选 RDMA buffer。
 *   Destroy 等待已记录的 stream 并尝试释放资源；失败时保留 session 供重试。
 * Purpose: Create diagnostic sessions, validate collective calls, sample clocks, exchange/save models and collect observations.
 * Calls: Implement tilexr_straggler.h session APIs using the clock kernel, drift fitter and record analyzer.
 * Lifetime: Sessions borrow communicators/streams and own sample, record and optional RDMA buffers.
 *   Destroy waits for recorded streams and attempts cleanup; failures retain the session for retry.
 */
#include "straggler_session.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <sstream>
#include <sys/stat.h>
#include <sys/types.h>

#include "acl/acl_rt.h"
#include "straggler_analysis.h"
#include "tilexr_api.h"

namespace TileXRDiagnostics {
namespace Host {
namespace {

std::mutex g_sessionStateMutex;
std::unordered_map<TileXRCommPtr, StragglerSession *> g_commOwners;

enum class ControlOperation : uint32_t {
    CLOCK_PREFLIGHT = 1,
    CLOCK_READY = 2,
    CLOCK_RESULT = 3,
    COLLECT_PREFLIGHT = 4,
    CLOCK_RDMA_ALLOCATED = 5,
    CLOCK_RDMA_PREPARED = 6,
    CLOCK_DESTROY = 7,
    CLOCK_DESTROY_RELEASED = 8,
    CLOCK_PERSISTED = 9,
    CLOCK_MODEL_READY = 10,
};

struct ControlEnvelope {
    uint64_t magic = 0x5458525354524147ull;
    uint64_t sequence = 0;
    uint64_t launchId = 0;
    uint64_t configHash = 0;
    int32_t status = TileXR::TILEXR_SUCCESS;
    uint32_t operation = 0;
    uint32_t rank = 0;
    uint32_t generation = 0;
    uint64_t reserved0 = 0;
    uint64_t reserved1 = 0;
};

static_assert(sizeof(ControlEnvelope) == 64, "straggler control envelope ABI changed");
static_assert(TileXR::TILEXR_CLOCK_RDMA_VIEWS_OFFSET +
    TILEXR_STRAGGLER_MAX_RANKS * sizeof(TileXRRDMADeviceView) <= TileXR::TILEXR_CLOCK_RDMA_STAGING_OFFSET,
    "clock views overlap send staging");
static_assert(TileXR::TILEXR_CLOCK_RDMA_STAGING_OFFSET + sizeof(TileXR::TileXRClockSyncProtocolSlot) <=
    TileXR::TILEXR_CLOCK_RDMA_PROFILE_OFFSET, "clock staging overlaps profiling");
static_assert(TileXR::TILEXR_CLOCK_RDMA_PROFILE_OFFSET + sizeof(TileXR::TileXRClockRdmaProfile) <=
    TileXR::TILEXR_CLOCK_RDMA_ASYNC_STAGING_OFFSET, "clock profiling overlaps deferred staging");
static_assert(TileXR::TILEXR_CLOCK_RDMA_ASYNC_STAGING_OFFSET < TileXR::TILEXR_CLOCK_SYNC_REGION_OFFSET,
    "clock deferred staging overlaps protocol mailboxes");

aclError MallocDevice(void **ptr, size_t bytes)
{
    return aclrtMalloc(ptr, bytes, ACL_MEM_MALLOC_HUGE_FIRST);
}

aclError FreeDevice(void *ptr)
{
    return aclrtFree(ptr);
}

aclError MemsetDeviceAsync(void *ptr, size_t bytes, int value, size_t count, aclrtStream stream)
{
    return aclrtMemsetAsync(ptr, bytes, value, count, stream);
}

aclError CopyHostToDeviceAsync(void *dst, size_t dstBytes, const void *src, size_t bytes,
                               aclrtStream stream)
{
    return aclrtMemcpyAsync(dst, dstBytes, src, bytes, ACL_MEMCPY_HOST_TO_DEVICE, stream);
}

aclError CopyDeviceToHost(void *dst, size_t dstBytes, const void *src, size_t bytes)
{
    return aclrtMemcpy(dst, dstBytes, src, bytes, ACL_MEMCPY_DEVICE_TO_HOST);
}

aclError SynchronizeStream(aclrtStream stream)
{
    return aclrtSynchronizeStream(stream);
}

uint64_t SteadyNowNs();

const StragglerRuntimeHooks kDefaultHooks {
    MallocDevice,
    FreeDevice,
    MemsetDeviceAsync,
    CopyHostToDeviceAsync,
    CopyDeviceToHost,
    SynchronizeStream,
    TileXRGetCommArgsHost,
    TileXRGetCommArgsDev,
    TileXRCommNextMagic,
    TileXRCommHostAllGatherTimeout,
    LaunchClockSyncKernel,
    SteadyNowNs,
    aclGetRecentErrMsg,
    TileXRRDMAQuery,
    TileXRRDMARegister,
    TileXRRDMAUnregister,
    TileXRRDMAGetDeviceView,
    TileXRRDMAShutdown,
};

const StragglerRuntimeHooks *g_hooks = &kDefaultHooks;

uint64_t SteadyNowNs()
{
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

uint64_t ConfigHash(const TileXRStragglerConfig &config)
{
    uint64_t hash = 1469598103934665603ull;
    auto mix = [&](uint64_t value) {
        for (uint32_t index = 0; index < 8; ++index) {
            hash ^= (value >> (index * 8u)) & 0xffu;
            hash *= 1099511628211ull;
        }
    };
    uint64_t relative = 0;
    uint64_t mad = 0;
    std::memcpy(&relative, &config.relativeThreshold, sizeof(relative));
    std::memcpy(&mad, &config.madThreshold, sizeof(mad));
    mix(config.clockSyncSamples);
    mix(config.clockSyncWarmupSamples);
    mix(config.recordCapacity);
    mix(relative);
    mix(mad);
    mix(config.absoluteThresholdNs);
    mix(config.timeoutMs);
    mix(config.maxCalibrationAgeMs);
    return hash;
}

int GatherControl(StragglerSession &session, ControlOperation operation, uint64_t sequence,
                  uint64_t launchId, uint32_t generation, int localStatus)
{
    if (session.hostArgs == nullptr || session.hostArgs->rankSize <= 0) {
        return TileXR::TILEXR_ERROR_NOT_INITIALIZED;
    }
    ControlEnvelope local {};
    local.sequence = sequence;
    local.launchId = launchId;
    local.configHash = ConfigHash(session.config);
    local.configHash = (local.configHash ^ session.clockDrift.windowUs) * 1099511628211ull;
    local.configHash = (local.configHash ^ session.clockDrift.tracking) * 1099511628211ull;
    local.status = localStatus;
    local.operation = static_cast<uint32_t>(operation);
    local.rank = static_cast<uint32_t>(session.hostArgs->rank);
    local.generation = generation;
    local.reserved0 = (session.clockUsesRdma ? 1u : 0u) | (session.clockRdmaDeferred ? 2u : 0u) |
        (session.clockRdmaCompact ? 4u : 0u);
    local.reserved1 = session.clockRdmaFailed ? 1u : 0u;
    std::vector<ControlEnvelope> envelopes(static_cast<size_t>(session.hostArgs->rankSize));
    const int gatherStatus = g_hooks->hostAllGather(
        session.comm, &local, sizeof(local), envelopes.data(), session.config.timeoutMs);
    if (gatherStatus != TileXR::TILEXR_SUCCESS) {
        return gatherStatus;
    }
    int domainStatus = TileXR::TILEXR_SUCCESS;
    bool abortRdma = false;
    for (int rank = 0; rank < session.hostArgs->rankSize; ++rank) {
        const ControlEnvelope &item = envelopes[static_cast<size_t>(rank)];
        if (item.magic != local.magic || item.operation != local.operation ||
            item.sequence != sequence || item.generation != generation ||
            item.configHash != local.configHash ||
            item.rank != static_cast<uint32_t>(rank) || item.reserved0 != local.reserved0) {
            return TileXR::TILEXR_ERROR_INTERNAL;
        }
        if (domainStatus == TileXR::TILEXR_SUCCESS && item.status != TileXR::TILEXR_SUCCESS) {
            domainStatus = item.status;
        }
        abortRdma = abortRdma || item.reserved1 != 0;
    }
    if (domainStatus != TileXR::TILEXR_SUCCESS) {
        return domainStatus;
    }
    for (const ControlEnvelope &item : envelopes) {
        if (item.launchId != launchId) {
            return TileXR::TILEXR_ERROR_INTERNAL;
        }
    }
    if (operation == ControlOperation::CLOCK_DESTROY) {
        session.clockRdmaFailed = abortRdma;
    }
    return domainStatus;
}

int PrepareClockRdma(StragglerSession &session, aclrtStream stream,
                     uint64_t sequence, uint32_t generation)
{
    if (!session.clockUsesRdma) {
        return TileXR::TILEXR_SUCCESS;
    }
    session.clockRdmaParticipating = true;
    int status = TileXR::TILEXR_SUCCESS;
    if (!session.clockRdmaReady) {
        if (session.clockRdmaMemory == nullptr &&
            g_hooks->mallocDevice(&session.clockRdmaMemory, TileXR::TILEXR_CLOCK_RDMA_BYTES) !=
                ACL_SUCCESS) {
            status = TileXR::TILEXR_ERROR_INTERNAL;
        }
        if (session.clockRdmaMemory == nullptr) {
            status = TileXR::TILEXR_ERROR_INTERNAL;
        }
        if (status == TileXR::TILEXR_SUCCESS) {
            const aclError clear = g_hooks->memsetDeviceAsync(session.clockRdmaMemory,
                TileXR::TILEXR_CLOCK_RDMA_BYTES, 0, TileXR::TILEXR_CLOCK_RDMA_BYTES, stream);
            const aclError sync = g_hooks->synchronizeStream(stream);
            if (clear != ACL_SUCCESS || sync != ACL_SUCCESS) {
                status = TileXR::TILEXR_ERROR_INTERNAL;
            }
        }
        int ret = GatherControl(session, ControlOperation::CLOCK_RDMA_ALLOCATED,
            sequence, 0, generation, status);
        if (ret != TileXR::TILEXR_SUCCESS) {
            return ret;
        }
        // Collective registration includes the complete allocation, never an IPC alias.
        status = g_hooks->rdmaRegister(session.comm, session.clockRdmaMemory,
            TileXR::TILEXR_CLOCK_RDMA_BYTES, &session.clockRdmaHandle);
        if (status != TileXR::TILEXR_SUCCESS) {
            TileXRRDMAInfo info {};
            session.clockRdmaFailed = g_hooks->rdmaQuery(session.comm, &info) != TileXR::TILEXR_SUCCESS ||
                info.failed != 0;
            return status;
        }
        session.clockRdmaViews.resize(static_cast<size_t>(session.hostArgs->rankSize));
        for (int peer = 0; peer < session.hostArgs->rankSize; ++peer) {
            if (peer == session.hostArgs->rank) {
                continue;
            }
            status = g_hooks->rdmaGetView(session.comm, session.clockRdmaHandle, peer,
                &session.clockRdmaViews[peer]);
            if (status != TileXR::TILEXR_SUCCESS) {
                break;
            }
        }
        const size_t bytes = session.clockRdmaViews.size() * sizeof(TileXRRDMADeviceView);
        if (status == TileXR::TILEXR_SUCCESS) {
            const uint64_t deferred = (session.clockRdmaDeferred ? 1u : 0u) |
                (session.clockRdmaCompact ? 2u : 0u);
            const aclError modeCopy = g_hooks->copyHostToDeviceAsync(session.clockRdmaMemory,
                sizeof(deferred), &deferred, sizeof(deferred), stream);
            void *views = static_cast<uint8_t *>(session.clockRdmaMemory) +
                TileXR::TILEXR_CLOCK_RDMA_VIEWS_OFFSET;
            const aclError copy = g_hooks->copyHostToDeviceAsync(views, bytes,
                session.clockRdmaViews.data(), bytes, stream);
            const aclError sync = g_hooks->synchronizeStream(stream);
            if (modeCopy != ACL_SUCCESS || copy != ACL_SUCCESS || sync != ACL_SUCCESS) {
                status = TileXR::TILEXR_ERROR_INTERNAL;
            }
        }
        ret = GatherControl(session, ControlOperation::CLOCK_RDMA_PREPARED,
            sequence, 0, generation, status);
        if (ret != TileXR::TILEXR_SUCCESS) {
            return ret;
        }
        session.clockRdmaReady = true;
    }
    // One serialized AIV owns each QP. Reserve room for this whole clock round
    // and its invalid stop entry; this clock path deliberately refuses wrap.
    const uint64_t samples = session.config.clockSyncSamples + session.config.clockSyncWarmupSamples;
    const uint64_t sourceSlots = samples * 3u * (session.hostArgs->rankSize - 1u);
    if (session.clockRdmaDeferred && sourceSlots >
        (TileXR::TILEXR_CLOCK_SYNC_REGION_OFFSET - TileXR::TILEXR_CLOCK_RDMA_ASYNC_STAGING_OFFSET) / 64u) {
        return TileXR::TILEXR_ERROR_NOT_SUPPORT;
    }
    for (int peer = 0; peer < session.hostArgs->rankSize; ++peer) {
        if (peer == session.hostArgs->rank || (session.hostArgs->rank != 0 && peer != 0)) {
            continue;
        }
        const auto &view = session.clockRdmaViews[peer];
        TileXRRDMADeviceState state {};
        if (g_hooks->copyDeviceToHost(&state, sizeof(state),
                reinterpret_cast<const void *>(view.queue.state), sizeof(state)) != ACL_SUCCESS) {
            return TileXR::TILEXR_ERROR_INTERNAL;
        }
        const uint64_t sends = samples * (session.hostArgs->rank == 0 ?
            (session.clockRdmaCompact ? 2u : 3u) : (session.clockRdmaCompact ? 1u : 2u));
        if (state.error != 0 || state.producer != state.completed ||
            state.reserved[0] != 0 ||
            (session.clockRdmaDeferred && sends > view.queue.maxSendWr) ||
            state.producer >= view.queue.sqDepth || sends >= view.queue.sqDepth - state.producer ||
            state.cqConsumer >= view.queue.cqDepth || sends >= view.queue.cqDepth - state.cqConsumer) {
            return TileXR::TILEXR_ERROR_NOT_SUPPORT;
        }
    }
    return TileXR::TILEXR_SUCCESS;
}

void TrackOutstandingStream(StragglerSession &session, aclrtStream stream)
{
    if (stream != nullptr && std::find(session.outstandingStreams.begin(),
            session.outstandingStreams.end(), stream) == session.outstandingStreams.end()) {
        session.outstandingStreams.push_back(stream);
    }
}

void UntrackStreamIfIdle(StragglerSession &session, aclrtStream stream)
{
    if (stream == nullptr || std::find(session.recordStreams.begin(), session.recordStreams.end(), stream) !=
            session.recordStreams.end()) {
        return;
    }
    session.outstandingStreams.erase(std::remove(session.outstandingStreams.begin(),
        session.outstandingStreams.end(), stream), session.outstandingStreams.end());
}

bool ValidateOwner(const StragglerSession &session)
{
    return session.ownerThread == std::thread::id() || session.ownerThread == std::this_thread::get_id();
}

uint64_t ResolveCyclesPerSecond(const TileXR::CommArgs &args)
{
    if ((args.extraFlag & (TileXR::ExtraFlag::PERF_CYCLE_A5 |
                          TileXR::ExtraFlag::PERF_CYCLE_1GHZ)) != 0) {
        return 1000000000ull;
    }
    return 50000000ull;
}

bool ValidateConfig(const TileXRStragglerConfig &config)
{
    return config.clockSyncSamples >= TileXR::TILEXR_CLOCK_SYNC_MIN_SAMPLES &&
        config.clockSyncSamples <= TileXR::TILEXR_CLOCK_SYNC_MAX_SAMPLES &&
        config.clockSyncWarmupSamples <= TileXR::TILEXR_CLOCK_SYNC_MAX_WARMUP_SAMPLES &&
        config.clockSyncSamples <= std::numeric_limits<uint32_t>::max() -
            config.clockSyncWarmupSamples &&
        config.recordCapacity > 0 && config.recordCapacity <= 65536u &&
        config.relativeThreshold > 0.0 && config.relativeThreshold < 10.0 &&
        config.madThreshold > 0.0 && config.madThreshold < 100.0 &&
        config.absoluteThresholdNs > 0 && config.timeoutMs > 0 && config.timeoutMs <= 60000u &&
        config.maxCalibrationAgeMs > 0;
}

bool EnsureDirectory(const std::string &path)
{
    if (path.empty()) {
        return true;
    }
    std::string current;
    size_t position = 0;
    if (path[0] == '/') {
        current = "/";
        position = 1;
    }
    while (position <= path.size()) {
        const size_t next = path.find('/', position);
        const std::string component = path.substr(
            position, next == std::string::npos ? std::string::npos : next - position);
        position = next == std::string::npos ? path.size() + 1 : next + 1;
        if (component.empty()) {
            continue;
        }
        if (!current.empty() && current[current.size() - 1] != '/') {
            current += '/';
        }
        current += component;
        struct stat state {};
        if (stat(current.c_str(), &state) == 0) {
            if (!S_ISDIR(state.st_mode)) {
                return false;
            }
            continue;
        }
        if (errno != ENOENT || (mkdir(current.c_str(), 0755) != 0 && errno != EEXIST)) {
            return false;
        }
    }
    return true;
}

int UpdateClockModels(StragglerSession &session,
                      const std::vector<TileXR::TileXRClockSyncDeviceSample> &samples,
                      uint64_t sequence, uint64_t magic, uint32_t generation)
{
    if (session.clockDrift.windowUs == 0) { return TileXR::TILEXR_SUCCESS; }
    auto candidate = session.clockDrift;
    UpdateClockDrift(candidate, samples,
        session.cyclesPerSecond, session.hostArgs->rank, generation);
    int status = TileXR::TILEXR_SUCCESS;
    if (candidate.tracking) {
        const bool invalidProbe = session.clockDrift.closed &&
            candidate.model.state == TILEXR_CLOCK_DRIFT_READY &&
            session.hostArgs->rank != 0 && !candidate.probeValid;
        status = GatherControl(session,
            ControlOperation::CLOCK_MODEL_READY, sequence, magic, generation,
            invalidProbe ? TileXR::TILEXR_ERROR_INTERNAL : TileXR::TILEXR_SUCCESS);
        if (status != TileXR::TILEXR_SUCCESS) { return status; }
    }
    std::vector<TileXRClockDriftModel> models(session.hostArgs->rankSize);
    status = g_hooks->hostAllGather(session.comm, &candidate.model, sizeof(TileXRClockDriftModel),
        models.data(), session.config.timeoutMs);
    if (status != TileXR::TILEXR_SUCCESS) { return status; }
    for (size_t rank = 0; rank < models.size(); ++rank) {
        const auto &model = models[rank];
        if (model.version != 1 || model.rank != static_cast<int32_t>(rank) ||
            model.requestedWindowUs != session.clockDrift.windowUs ||
            model.cyclesPerSecond != session.cyclesPerSecond ||
            model.state < TILEXR_CLOCK_DRIFT_COLLECTING || model.state > TILEXR_CLOCK_DRIFT_UNSTABLE) {
            return TileXR::TILEXR_ERROR_INTERNAL;
        }
    }
    session.clockDrift = std::move(candidate);
    session.clockDriftModels.swap(models);
    return TileXR::TILEXR_SUCCESS;
}

int WriteClockJson(const StragglerSession &session,
                    const std::vector<TileXR::TileXRClockSyncDeviceSample> &samples)
{
    if (session.outputDir.empty()) {
        return TileXR::TILEXR_SUCCESS;
    }
    if (!EnsureDirectory(session.outputDir)) {
        return TileXR::TILEXR_ERROR_INTERNAL;
    }
    std::ostringstream path;
    path << session.outputDir << "/clock_sync_generation" << session.clock.generation << ".json";
    std::ostringstream out;
    out << std::setprecision(17);
    out << "{\n  \"schema\": \"tilexr_clock_sync.v1\",\n"
        << "  \"generation\": " << session.clock.generation << ",\n"
        << "  \"reference_rank\": 0,\n"
        << "  \"rank_size\": " << session.clock.rankSize << ",\n"
        << "  \"warmup_sample_count\": " << session.clock.warmupSampleCount << ",\n"
        << "  \"sample_count\": " << session.clock.sampleCount << ",\n"
        << "  \"cycles_per_second\": " << session.clock.cyclesPerSecond << ",\n"
        << "  \"frequency_source_id\": " << session.clock.frequencySource << ",\n"
        << "  \"frequency_source\": \"cann_9_1_product_rule\",\n"
        << "  \"precision_cycles\": " << session.clock.precisionCycles << ",\n"
        << "  \"precision_ns\": " << session.clock.precisionNs << ",\n"
        << "  \"precision_semantics\": \"max_rank_half_min_effective_rtt\",\n"
        << "  \"offset_model_assumption\": \"symmetric_effective_one_way_delays\",\n"
        << "  \"path_symmetry_verified\": false,\n"
        << "  \"measured_residual_ns\": null,\n"
        << "  \"ranks\": [\n";
    for (uint32_t rank = 0; rank < session.clock.rankSize; ++rank) {
        const TileXRClockRankResult &item = session.clock.ranks[rank];
        out << "    {\"rank\": " << rank
            << ", \"status\": " << item.status
            << ", \"offset_cycles\": " << item.offsetCycles
            << ", \"offset_ns\": " << item.offsetNs
            << ", \"min_rtt_cycles\": " << item.minRoundTripCycles
            << ", \"median_rtt_cycles\": " << item.medianRoundTripCycles
            << ", \"error_bound_cycles\": " << item.errorBoundCycles
            << ", \"error_bound_ns\": " << item.errorBoundNs
            << ", \"valid_samples\": " << item.validSamples
            << ", \"rejected_samples\": " << item.rejectedSamples << "}"
            << (rank + 1 == session.clock.rankSize ? "\n" : ",\n");
    }
    out << "  ]";
    if (session.clockDrift.tracking) {
        out << ",\n  \"tracking\": {\"initial_window_us\": " << session.clockDrift.windowUs
            << ", \"maintenance_interval_ms\": 60000, \"trigger_ns\": 1000, \"rate_gain\": 0.25"
            << ", \"initial_generation\": " << session.clockDrift.initialGeneration
            << ", \"local_model_updated\": " << (session.clockDrift.updated ? "true" : "false")
            << ", \"local_probe_valid\": " << (session.clockDrift.probeValid ? "true" : "false")
            << ", \"local_preupdate_residual_ns\": " << session.clockDrift.residualNs << "},\n"
            << "  \"drift_models\": [\n";
        for (size_t rank = 0; rank < session.clockDriftModels.size(); ++rank) {
            const auto &model = session.clockDriftModels[rank];
            out << "    {\"rank\": " << rank << ", \"state\": " << model.state
                << ", \"anchor_cycle\": " << model.anchorCycle
                << ", \"offset_cycles\": " << model.offsetCycles
                << ", \"rate_correction\": " << model.rateCorrection
                << ", \"model_generation\": " << model.trainedAtGeneration
                << ", \"initial_fit_samples\": " << model.sampleCount
                << ", \"initial_fit_span_cycles\": " << model.spanCycles << "}"
                << (rank+1 == session.clockDriftModels.size() ? "\n" : ",\n");
        }
        out << "  ],\n  \"local_samples\": [\n";
        for (size_t i = 0; i < samples.size(); ++i) {
            const auto &s = samples[i];
            out << "    {\"sequence\": " << s.sequence << ", \"status\": " << s.status
                << ", \"t0\": " << s.t0 << ", \"t1\": " << s.t1
                << ", \"t2\": " << s.t2 << ", \"t3\": " << s.t3 << "}"
                << (i+1 == samples.size() ? "\n" : ",\n");
        }
        out << "  ]";
    }
    out << "\n}\n";
    const std::string snapshot = out.str();
    auto write = [&](const std::string &name) {
        std::ofstream file(name.c_str(),std::ios::out | std::ios::trunc);
        file << snapshot;
        file.close();
        return !file.fail();
    };
    if (!write(path.str())) { return TileXR::TILEXR_ERROR_INTERNAL; }
    if (session.clockDrift.tracking) {
        const std::string latest = session.outputDir+"/clock_drift_latest.json";
        if (!write(latest+".tmp") || std::rename((latest+".tmp").c_str(),latest.c_str()) != 0) {
            return TileXR::TILEXR_ERROR_INTERNAL;
        }
    }
    return TileXR::TILEXR_SUCCESS;
}

int WriteReportJson(const StragglerSession &session, const TileXRStragglerReport &report)
{
    if (session.outputDir.empty()) {
        return TileXR::TILEXR_SUCCESS;
    }
    if (!EnsureDirectory(session.outputDir)) {
        return TileXR::TILEXR_ERROR_INTERNAL;
    }
    std::ostringstream path;
    path << session.outputDir << "/launch" << report.launchId << ".json";
    std::ofstream out(path.str().c_str(), std::ios::out | std::ios::trunc);
    if (!out.is_open()) {
        return TileXR::TILEXR_ERROR_INTERNAL;
    }
    out << std::setprecision(10)
        << "{\n  \"schema\": \"tilexr_straggler.v1\",\n"
        << "  \"launch_id\": " << report.launchId << ",\n"
        << "  \"component_kind\": " << report.componentKind << ",\n"
        << "  \"operation_kind\": " << report.operationKind << ",\n"
        << "  \"stage_id\": " << report.stageId << ",\n"
        << "  \"operation_id\": " << report.operationId << ",\n"
        << "  \"message_bytes\": " << report.messageBytes << ",\n"
        << "  \"rank_size\": " << report.rankSize << ",\n"
        << "  \"calibration_generation\": " << report.calibrationGeneration << ",\n"
        << "  \"clock_precision_ns\": " << report.clockPrecisionNs << ",\n"
        << "  \"calibration_age_ns\": " << report.calibrationAgeNs << ",\n"
        << "  \"relative_threshold\": " << report.relativeThreshold << ",\n"
        << "  \"mad_threshold\": " << report.madThreshold << ",\n"
        << "  \"absolute_threshold_ns\": " << report.absoluteThresholdNs << ",\n"
        << "  \"median_duration_ns\": " << report.medianDurationNs << ",\n"
        << "  \"mad_duration_ns\": " << report.madDurationNs << ",\n"
        << "  \"median_pre_communication_ns\": " << report.medianPreCommunicationNs << ",\n"
        << "  \"mad_pre_communication_ns\": " << report.madPreCommunicationNs << ",\n"
        << "  \"complete\": " << (report.complete ? "true" : "false") << ",\n"
        << "  \"dropped_records\": " << report.droppedRecords << ",\n"
        << "  \"ranks\": [\n";
    for (uint32_t rank = 0; rank < report.rankSize; ++rank) {
        const TileXRStragglerRankObservation &item = report.ranks[rank];
        out << "    {\"rank\": " << rank
            << ", \"status\": " << item.status
            << ", \"classification\": " << item.classification
            << ", \"reason_flags\": " << item.reasonFlags
            << ", \"local_start_cycle\": " << item.localStartCycle
            << ", \"local_ready_cycle\": " << item.localReadyCycle
            << ", \"local_end_cycle\": " << item.localEndCycle
            << ", \"aligned_start_ns\": " << item.alignedStartNs
            << ", \"aligned_ready_ns\": " << item.alignedReadyNs
            << ", \"aligned_end_ns\": " << item.alignedEndNs
            << ", \"duration_ns\": " << item.durationNs
            << ", \"pre_communication_ns\": " << item.preCommunicationNs
            << ", \"duration_delta_ns\": " << item.durationDeltaNs
            << ", \"pre_communication_delta_ns\": " << item.preCommunicationDeltaNs
            << ", \"start_delta_ns\": " << item.startDeltaNs
            << ", \"completion_delta_ns\": " << item.completionDeltaNs
            << ", \"relative_delta\": " << item.relativeDelta
            << ", \"robust_score\": " << item.robustScore
            << ", \"start_robust_score\": " << item.startRobustScore
            << ", \"completion_robust_score\": " << item.completionRobustScore
            << ", \"pre_communication_robust_score\": " << item.preCommunicationRobustScore
            << ", \"block_count\": " << item.blockCount << "}"
            << (rank + 1 == report.rankSize ? "\n" : ",\n");
    }
    out << "  ]\n}\n";
    return out.good() ? TileXR::TILEXR_SUCCESS : TileXR::TILEXR_ERROR_INTERNAL;
}

} // namespace

StragglerSession *FindStragglerSession(TileXRCommPtr comm)
{
    std::lock_guard<std::mutex> guard(g_sessionStateMutex);
    const auto owner = g_commOwners.find(comm);
    return owner == g_commOwners.end() ? nullptr : owner->second;
}

void SetStragglerRuntimeHooksForTest(const StragglerRuntimeHooks *hooks)
{
    g_hooks = hooks == nullptr ? &kDefaultHooks : hooks;
}

void SetStragglerTestDelayForHost(StragglerSession *session, int rank, uint64_t cycles)
{
    if (session != nullptr) {
        session->testDelayRank = rank;
        session->testDelayCycles = cycles;
    }
}

int BeginObservation(StragglerSession *session, const TileXRStragglerOperation *operation,
                     aclrtStream stream, TileXRStragglerObservation *observation)
{
    if (observation != nullptr) {
        *observation = TileXRStragglerObservation {};
    }
    if (session == nullptr || operation == nullptr || observation == nullptr || stream == nullptr ||
        operation->componentKind == TILEXR_STRAGGLER_COMPONENT_UNKNOWN ||
        session->deviceRecords == nullptr || session->config.recordCapacity == 0) {
        return TileXR::TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    std::lock_guard<std::mutex> guard(session->stateMutex);
    if (!ValidateOwner(*session)) {
        session->observationStatus = TileXR::TILEXR_ERROR_PARA_CHECK_FAIL;
        return session->observationStatus;
    }
    if (session->localLaunchId == std::numeric_limits<uint64_t>::max()) {
        ++session->droppedRecords;
        session->observationStatus = TileXR::TILEXR_ERROR_INTERNAL;
        return session->observationStatus;
    }
    const uint64_t pending = session->localLaunchId + 1u - session->nextCollectId;
    if (pending >= session->config.recordCapacity) {
        ++session->droppedRecords;
        session->observationStatus = TileXR::TILEXR_ERROR_INTERNAL;
        return session->observationStatus;
    }
    observation->detector = session;
    observation->launchId = ++session->localLaunchId;
    observation->messageBytes = operation->messageBytes;
    observation->operationId = operation->operationId;
    observation->generation = session->generation;
    observation->componentKind = operation->componentKind;
    observation->operationKind = operation->operationKind;
    observation->stageId = operation->stageId;
    observation->rank = static_cast<uint32_t>(session->hostArgs->rank);
    observation->delayCycles = session->testDelayRank == session->hostArgs->rank ?
        session->testDelayCycles : 0;
    const uint64_t slot = (observation->launchId - 1u) % session->config.recordCapacity;
    observation->deviceRecord = static_cast<uint8_t *>(session->deviceRecords) +
        TileXR::StragglerTimingRecordOffset(static_cast<uint32_t>(slot));
    TileXR::TileXRStragglerDeviceControl &control =
        session->recordControls[static_cast<uint32_t>(slot)];
    control = TileXR::TileXRStragglerDeviceControl {};
    control.launchId = observation->launchId;
    control.operationId = observation->operationId;
    control.messageBytes = observation->messageBytes;
    control.delayCycles = observation->delayCycles;
    control.generation = observation->generation;
    control.componentKind = observation->componentKind;
    control.operationKind = observation->operationKind;
    control.stageId = observation->stageId;
    control.rank = observation->rank;
    void *deviceControl = static_cast<uint8_t *>(session->deviceRecords) +
        TileXR::StragglerDeviceControlOffset(static_cast<uint32_t>(slot));
    session->recordStatuses[static_cast<uint32_t>(slot)] = TileXR::TILEXR_SUCCESS;
    session->recordStreams[static_cast<uint32_t>(slot)] = stream;
    TrackOutstandingStream(*session, stream);
    if (g_hooks->copyHostToDeviceAsync(deviceControl, sizeof(control), &control,
            sizeof(control), stream) != ACL_SUCCESS) {
        session->recordStatuses[static_cast<uint32_t>(slot)] = TileXR::TILEXR_ERROR_INTERNAL;
        session->observationStatus = TileXR::TILEXR_ERROR_INTERNAL;
        return TileXR::TILEXR_ERROR_INTERNAL;
    }
    return TileXR::TILEXR_SUCCESS;
}

int EndObservation(StragglerSession *session, const TileXRStragglerObservation *observation,
                   aclrtStream stream, int launchStatus)
{
    if (session == nullptr || observation == nullptr || observation->detector != session ||
        observation->launchId == 0 || stream == nullptr) {
        return TileXR::TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    std::lock_guard<std::mutex> guard(session->stateMutex);
    if (!ValidateOwner(*session)) {
        return TileXR::TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    const uint64_t slot = (observation->launchId - 1u) % session->config.recordCapacity;
    session->recordStatuses[static_cast<uint32_t>(slot)] = launchStatus;
    if (session->recordStreams[static_cast<uint32_t>(slot)] == nullptr) {
        session->recordStreams[static_cast<uint32_t>(slot)] = stream;
    }
    TrackOutstandingStream(*session, stream);
    return TileXR::TILEXR_SUCCESS;
}

} // namespace Host
} // namespace TileXRDiagnostics

extern "C" int TileXRStragglerConfigDefault(TileXRStragglerConfig *config)
{
    if (config == nullptr) {
        return TileXR::TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    *config = TileXRStragglerConfig {};
    config->clockSyncSamples = 128;
    config->clockSyncWarmupSamples = 32;
    config->recordCapacity = 64;
    config->relativeThreshold = 0.2;
    config->madThreshold = 3.0;
    config->absoluteThresholdNs = 5000;
    config->timeoutMs = 5000;
    config->maxCalibrationAgeMs = 60000;
    return TileXR::TILEXR_SUCCESS;
}

extern "C" int TileXRStragglerCreate(TileXRCommPtr comm,
                                                const TileXRStragglerConfig *config,
                                                TileXRStraggler *detector)
{
    if (detector != nullptr) {
        *detector = nullptr;
    }
    if (comm == nullptr || config == nullptr || detector == nullptr ||
        !TileXRDiagnostics::Host::ValidateConfig(*config)) {
        return TileXR::TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    std::unique_ptr<TileXRDiagnostics::Host::StragglerSession> session(
        new (std::nothrow) TileXRDiagnostics::Host::StragglerSession);
    if (!session) {
        return TileXR::TILEXR_ERROR_INTERNAL;
    }
    session->comm = comm;
    session->config = *config;
    const char *tracking = std::getenv("TILEXR_CLOCK_DRIFT_TRACK");
    if (tracking != nullptr && std::strcmp(tracking,"0") != 0 && std::strcmp(tracking,"1") != 0) {
        return TileXR::TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    session->clockDrift.tracking = tracking != nullptr && std::strcmp(tracking,"1") == 0;
    if (session->clockDrift.tracking) {
        if (config->outputDir == nullptr || *config->outputDir == '\0' || config->clockSyncSamples < 64) {
            return TileXR::TILEXR_ERROR_PARA_CHECK_FAIL;
        }
        session->clockDrift.windowUs = 10000000;
    }
    const char *driftWindow = std::getenv("TILEXR_CLOCK_DRIFT_WINDOW_US");
    if (driftWindow != nullptr) {
        uint64_t value = 0;
        if (*driftWindow == '\0') { return TileXR::TILEXR_ERROR_PARA_CHECK_FAIL; }
        for (const char *p = driftWindow; *p != '\0'; ++p) {
            if (*p < '0' || *p > '9' || value > 6000000u) {
                return TileXR::TILEXR_ERROR_PARA_CHECK_FAIL;
            }
            value = value*10u + static_cast<uint64_t>(*p-'0');
            if (value > 60000000u) { return TileXR::TILEXR_ERROR_PARA_CHECK_FAIL; }
        }
        session->clockDrift.windowUs = value;
    }
    if (session->clockDrift.tracking && session->clockDrift.windowUs != 10000000) {
        return TileXR::TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    const char *clockTransport = std::getenv("TILEXR_CLOCK_TRANSPORT");
    if (clockTransport != nullptr && std::strcmp(clockTransport, "mte") != 0 &&
        std::strcmp(clockTransport, "rdma") != 0) {
        return TileXR::TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    session->clockUsesRdma = clockTransport != nullptr && std::strcmp(clockTransport, "rdma") == 0;
    const char *deferred = std::getenv("TILEXR_CLOCK_RDMA_DEFER_CQE");
    session->clockRdmaDeferred = session->clockUsesRdma && deferred != nullptr &&
        std::strcmp(deferred, "1") == 0;
    const char *compact = std::getenv("TILEXR_CLOCK_RDMA_COMPACT");
    session->clockRdmaCompact = compact != nullptr && std::strcmp(compact, "1") == 0;
    if (session->clockRdmaCompact && !session->clockRdmaDeferred) {
        return TileXR::TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    int ret = TileXRDiagnostics::Host::g_hooks->getCommArgsHost(comm, session->hostArgs);
    if (ret != TileXR::TILEXR_SUCCESS || session->hostArgs == nullptr || session->hostArgs->rankSize <= 0 ||
        session->hostArgs->rankSize > TILEXR_STRAGGLER_MAX_RANKS) {
        return ret == TileXR::TILEXR_SUCCESS ? TileXR::TILEXR_ERROR_NOT_INITIALIZED : ret;
    }
    ret = TileXRDiagnostics::Host::g_hooks->getCommArgsDev(comm, session->devArgs);
    if (ret != TileXR::TILEXR_SUCCESS || session->devArgs == nullptr) {
        return ret == TileXR::TILEXR_SUCCESS ? TileXR::TILEXR_ERROR_NOT_INITIALIZED : ret;
    }
    if (config->outputDir != nullptr) {
        std::ostringstream rankOutput;
        rankOutput << config->outputDir << "/rank" << session->hostArgs->rank;
        session->outputDir = rankOutput.str();
        session->config.outputDir = session->outputDir.c_str();
    }
    session->cyclesPerSecond = TileXRDiagnostics::Host::ResolveCyclesPerSecond(*session->hostArgs);
    session->clockDriftModels.resize(static_cast<size_t>(session->hostArgs->rankSize));
    for (size_t rank = 0; rank < session->clockDriftModels.size(); ++rank) {
        auto &model = session->clockDriftModels[rank];
        model.version = 1;
        model.rank = static_cast<int32_t>(rank);
        model.cyclesPerSecond = session->cyclesPerSecond;
        model.requestedWindowUs = session->clockDrift.windowUs;
        model.state = session->clockDrift.windowUs == 0 ? TILEXR_CLOCK_DRIFT_DISABLED :
            TILEXR_CLOCK_DRIFT_COLLECTING;
    }
    session->deviceSamplesBytes = static_cast<size_t>(config->clockSyncSamples) *
        sizeof(TileXR::TileXRClockSyncDeviceSample);
    session->deviceRecordsBytes = static_cast<size_t>(config->recordCapacity) *
        sizeof(TileXR::TileXRStragglerDeviceSlot);
    session->recordControls.assign(config->recordCapacity, TileXR::TileXRStragglerDeviceControl {});
    session->recordStatuses.assign(config->recordCapacity, TileXR::TILEXR_SUCCESS);
    session->recordStreams.assign(config->recordCapacity, nullptr);
    if (TileXRDiagnostics::Host::g_hooks->mallocDevice(&session->deviceSamples,
            session->deviceSamplesBytes + sizeof(TileXR::TileXRClockSyncDeviceSample)) != ACL_SUCCESS ||
        session->deviceSamples == nullptr) {
        return TileXR::TILEXR_ERROR_INTERNAL;
    }
    if (TileXRDiagnostics::Host::g_hooks->mallocDevice(&session->deviceRecords,
            session->deviceRecordsBytes) != ACL_SUCCESS || session->deviceRecords == nullptr) {
        TileXRDiagnostics::Host::g_hooks->freeDevice(session->deviceSamples);
        return TileXR::TILEXR_ERROR_INTERNAL;
    }
    session->ownerThread = std::this_thread::get_id();
    {
        std::lock_guard<std::mutex> guard(TileXRDiagnostics::Host::g_sessionStateMutex);
        if (TileXRDiagnostics::Host::g_commOwners.count(comm) != 0) {
            TileXRDiagnostics::Host::g_hooks->freeDevice(session->deviceRecords);
            TileXRDiagnostics::Host::g_hooks->freeDevice(session->deviceSamples);
            return TileXR::TILEXR_ERROR_PARA_CHECK_FAIL;
        }
        TileXRDiagnostics::Host::g_commOwners[comm] = session.get();
    }
    *detector = session.release();
    return TileXR::TILEXR_SUCCESS;
}

extern "C" int TileXRStragglerDestroy(TileXRStraggler detector)
{
    TileXRDiagnostics::Host::StragglerSession *session =
        static_cast<TileXRDiagnostics::Host::StragglerSession *>(detector);
    if (session == nullptr) {
        return TileXR::TILEXR_SUCCESS;
    }
    {
        std::lock_guard<std::mutex> guard(TileXRDiagnostics::Host::g_sessionStateMutex);
        if (!TileXRDiagnostics::Host::ValidateOwner(*session)) {
            return TileXR::TILEXR_ERROR_PARA_CHECK_FAIL;
        }
    }
    int quiescenceStatus = TileXR::TILEXR_SUCCESS;
    {
        std::lock_guard<std::mutex> stateGuard(session->stateMutex);
        for (aclrtStream outstanding : session->outstandingStreams) {
            if (TileXRDiagnostics::Host::g_hooks->synchronizeStream(outstanding) != ACL_SUCCESS) {
                quiescenceStatus = TileXR::TILEXR_ERROR_INTERNAL;
            }
        }
        if (session->clockRdmaParticipating) {
            const int ret = TileXRDiagnostics::Host::GatherControl(*session,
                TileXRDiagnostics::Host::ControlOperation::CLOCK_DESTROY,
                session->controlSequence, 0, session->generation, quiescenceStatus);
            if (ret != TileXR::TILEXR_SUCCESS) {
                return ret;
            }
        } else if (quiescenceStatus != TileXR::TILEXR_SUCCESS) {
            return quiescenceStatus;
        }
        session->outstandingStreams.clear();
        std::fill(session->recordStreams.begin(), session->recordStreams.end(), nullptr);
    }
    bool failed = false;
    if (session->clockRdmaFailed) {
        // Stream quiescence does not cancel NIC requests. Close all QPs and
        // release MRs collectively before freeing any retained clock backing.
        const int ret = TileXRDiagnostics::Host::g_hooks->rdmaShutdown(session->comm);
        if (ret != TileXR::TILEXR_SUCCESS) {
            return ret;
        }
        session->clockRdmaHandle = 0;
        session->clockRdmaFailed = false;
    }
    if (session->clockRdmaHandle != 0) {
        const int ret = TileXRDiagnostics::Host::g_hooks->rdmaUnregister(
            session->comm, session->clockRdmaHandle);
        if (ret != TileXR::TILEXR_SUCCESS) {
            return ret;
        }
        session->clockRdmaHandle = 0;
    }
    if (session->clockRdmaMemory != nullptr) {
        if (TileXRDiagnostics::Host::g_hooks->freeDevice(session->clockRdmaMemory) == ACL_SUCCESS) {
            session->clockRdmaMemory = nullptr;
        } else {
            failed = true;
        }
    }
    if (session->deviceSamples != nullptr) {
        if (TileXRDiagnostics::Host::g_hooks->freeDevice(session->deviceSamples) == ACL_SUCCESS) {
            session->deviceSamples = nullptr;
        } else {
            failed = true;
        }
    }
    if (session->deviceRecords != nullptr) {
        if (TileXRDiagnostics::Host::g_hooks->freeDevice(session->deviceRecords) == ACL_SUCCESS) {
            session->deviceRecords = nullptr;
        } else {
            failed = true;
        }
    }
    if (session->clockRdmaParticipating) {
        const int ret = TileXRDiagnostics::Host::GatherControl(*session,
            TileXRDiagnostics::Host::ControlOperation::CLOCK_DESTROY_RELEASED,
            session->controlSequence, 0, session->generation,
            failed ? TileXR::TILEXR_ERROR_INTERNAL : TileXR::TILEXR_SUCCESS);
        if (ret != TileXR::TILEXR_SUCCESS) {
            return ret;
        }
    } else if (failed) {
        return TileXR::TILEXR_ERROR_INTERNAL;
    }
    {
        std::lock_guard<std::mutex> guard(TileXRDiagnostics::Host::g_sessionStateMutex);
        TileXRDiagnostics::Host::g_commOwners.erase(session->comm);
    }
    delete session;
    return TileXR::TILEXR_SUCCESS;
}

extern "C" int TileXRStragglerObservationBegin(
    TileXRStraggler detector, const TileXRStragglerOperation *operation,
    aclrtStream stream, TileXRStragglerObservation *observation)
{
    return TileXRDiagnostics::Host::BeginObservation(
        static_cast<TileXRDiagnostics::Host::StragglerSession *>(detector),
        operation, stream, observation);
}

extern "C" int TileXRStragglerObservationBeginForComm(
    TileXRCommPtr comm, const TileXRStragglerOperation *operation,
    aclrtStream stream, TileXRStragglerObservation *observation)
{
    if (observation != nullptr) {
        *observation = TileXRStragglerObservation {};
    }
    if (comm == nullptr || operation == nullptr || observation == nullptr || stream == nullptr) {
        return TileXR::TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    TileXRDiagnostics::Host::StragglerSession *session =
        TileXRDiagnostics::Host::FindStragglerSession(comm);
    if (session == nullptr) {
        return TileXR::TILEXR_ERROR_NOT_FOUND;
    }
    return TileXRDiagnostics::Host::BeginObservation(session, operation, stream, observation);
}

extern "C" int TileXRStragglerObservationEnd(
    TileXRStraggler detector, const TileXRStragglerObservation *observation,
    aclrtStream stream, int launchStatus)
{
    return TileXRDiagnostics::Host::EndObservation(
        static_cast<TileXRDiagnostics::Host::StragglerSession *>(detector),
        observation, stream, launchStatus);
}

extern "C" int TileXRClockSynchronize(TileXRStraggler detector,
                                                 aclrtStream stream,
                                                 TileXRClockSyncResult *result)
{
    TileXRDiagnostics::Host::StragglerSession *session =
        static_cast<TileXRDiagnostics::Host::StragglerSession *>(detector);
    if (session == nullptr || stream == nullptr || result == nullptr) {
        return TileXR::TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    *result = TileXRClockSyncResult {};
    if (!TileXRDiagnostics::Host::ValidateOwner(*session)) {
        return TileXR::TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    std::lock_guard<std::mutex> stateGuard(session->stateMutex);
    int localStatus = TileXR::TILEXR_SUCCESS;
    if (session->clockStream != nullptr) {
        const aclrtStream pendingClockStream = session->clockStream;
        if (TileXRDiagnostics::Host::g_hooks->synchronizeStream(pendingClockStream) == ACL_SUCCESS) {
            session->clockStream = nullptr;
            TileXRDiagnostics::Host::UntrackStreamIfIdle(*session, pendingClockStream);
        } else {
            localStatus = TileXR::TILEXR_ERROR_INTERNAL;
        }
    }
    if (localStatus == TileXR::TILEXR_SUCCESS &&
        (session->hostArgs == nullptr || session->deviceSamples == nullptr)) {
        localStatus = TileXR::TILEXR_ERROR_NOT_INITIALIZED;
    } else if (localStatus == TileXR::TILEXR_SUCCESS &&
               (!session->outstandingStreams.empty() ||
               session->nextCollectId <= session->localLaunchId ||
               session->generation == std::numeric_limits<uint32_t>::max())) {
        localStatus = TileXR::TILEXR_ERROR_PARA_CHECK_FAIL;
    } else if (localStatus == TileXR::TILEXR_SUCCESS && session->clockUsesRdma) {
        TileXRRDMAInfo info {};
        if (session->clockRdmaFailed || session->clockRdmaSetupStatus != TileXR::TILEXR_SUCCESS ||
            TileXRDiagnostics::Host::g_hooks->rdmaQuery == nullptr ||
            TileXRDiagnostics::Host::g_hooks->rdmaRegister == nullptr ||
            TileXRDiagnostics::Host::g_hooks->rdmaGetView == nullptr ||
            TileXRDiagnostics::Host::g_hooks->rdmaUnregister == nullptr ||
            TileXRDiagnostics::Host::g_hooks->rdmaShutdown == nullptr ||
            TileXRDiagnostics::Host::g_hooks->rdmaQuery(session->comm, &info) != TileXR::TILEXR_SUCCESS ||
            !info.connected || !info.deviceDirect || info.failed) {
            localStatus = TileXR::TILEXR_ERROR_NOT_SUPPORT;
        }
    } else if (localStatus == TileXR::TILEXR_SUCCESS) {
        for (int rank = 0; rank < session->hostArgs->rankSize; ++rank) {
            if (session->hostArgs->peerMems[rank] == nullptr) {
                localStatus = TileXR::TILEXR_ERROR_NOT_SUPPORT;
                break;
            }
        }
    }
    const long double timeout = static_cast<long double>(session->cyclesPerSecond) *
        static_cast<long double>(session->config.timeoutMs) / 1000.0L;
    if (timeout <= 0.0L || timeout > static_cast<long double>(std::numeric_limits<uint64_t>::max())) {
        localStatus = TileXR::TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    const uint64_t sequence = ++session->controlSequence;
    const uint32_t generation = session->generation + 1u;
    int ret = TileXRDiagnostics::Host::GatherControl(*session,
        TileXRDiagnostics::Host::ControlOperation::CLOCK_PREFLIGHT, sequence,
        0, generation, localStatus);
    if (ret != TileXR::TILEXR_SUCCESS) {
        return ret;
    }
    int64_t magic = 0;
    session->clockStream = stream;
    TileXRDiagnostics::Host::TrackOutstandingStream(*session, stream);
    localStatus = TileXRDiagnostics::Host::PrepareClockRdma(*session, stream, sequence, generation);
    if (localStatus == TileXR::TILEXR_SUCCESS) {
        localStatus = TileXRDiagnostics::Host::g_hooks->nextMagic(session->comm, &magic);
    }
    if (localStatus == TileXR::TILEXR_SUCCESS &&
        TileXRDiagnostics::Host::g_hooks->memsetDeviceAsync(session->deviceSamples,
            session->deviceSamplesBytes + sizeof(TileXR::TileXRClockSyncDeviceSample), 0,
            session->deviceSamplesBytes + sizeof(TileXR::TileXRClockSyncDeviceSample), stream) != ACL_SUCCESS) {
        localStatus = TileXR::TILEXR_ERROR_INTERNAL;
    } else if (localStatus == TileXR::TILEXR_SUCCESS) {
        session->clockStream = stream;
        TileXRDiagnostics::Host::TrackOutstandingStream(*session, stream);
    }
    ret = TileXRDiagnostics::Host::GatherControl(*session,
        TileXRDiagnostics::Host::ControlOperation::CLOCK_READY, sequence,
        static_cast<uint64_t>(magic), generation, localStatus);
    if (ret != TileXR::TILEXR_SUCCESS) {
        if (session->clockUsesRdma) {
            session->clockRdmaSetupStatus = ret;
        }
        if (localStatus == TileXR::TILEXR_SUCCESS &&
            TileXRDiagnostics::Host::g_hooks->synchronizeStream(stream) == ACL_SUCCESS) {
            session->clockStream = nullptr;
            TileXRDiagnostics::Host::UntrackStreamIfIdle(*session, stream);
        }
        return ret;
    }
    localStatus = TileXRDiagnostics::Host::g_hooks->launchClockSync(
        *session, session->config.clockSyncWarmupSamples, session->config.clockSyncSamples,
        magic, static_cast<uint64_t>(timeout), stream);
    const aclError synchronizeStatus = TileXRDiagnostics::Host::g_hooks->synchronizeStream(stream);
    const bool streamSynchronized = synchronizeStatus == ACL_SUCCESS;
    if (streamSynchronized) {
        session->clockStream = nullptr;
        TileXRDiagnostics::Host::UntrackStreamIfIdle(*session, stream);
    }
    std::vector<TileXR::TileXRClockSyncDeviceSample> samples(session->config.clockSyncSamples);
    TileXRClockRankResult local {};
    local.rank = session->hostArgs->rank;
    aclError copyStatus = ACL_SUCCESS;
    if (localStatus == TileXR::TILEXR_SUCCESS && streamSynchronized) {
        copyStatus = TileXRDiagnostics::Host::g_hooks->copyDeviceToHost(samples.data(),
            session->deviceSamplesBytes, session->deviceSamples, session->deviceSamplesBytes);
    }
    if (localStatus != TileXR::TILEXR_SUCCESS || !streamSynchronized || copyStatus != ACL_SUCCESS) {
        const char *recentError = TileXRDiagnostics::Host::g_hooks->getRecentError();
        std::cerr << "TileXR clock sync runtime boundary failed rank=" << session->hostArgs->rank
                  << ", synchronizeStatus=" << synchronizeStatus
                  << ", copyStatus=" << copyStatus
                  << ", recentError=" << (recentError == nullptr ? "" : recentError)
                  << std::endl;
        local.status = localStatus != TileXR::TILEXR_SUCCESS ? localStatus : TileXR::TILEXR_ERROR_INTERNAL;
    } else {
        if (!TileXRDiagnostics::Host::EstimateClockRank(
                samples, session->cyclesPerSecond, session->hostArgs->rank, local)) {
            const size_t diagnosticCount = std::min<size_t>(samples.size(), 4u);
            for (size_t index = 0; index < diagnosticCount; ++index) {
                const TileXR::TileXRClockSyncDeviceSample &sample = samples[index];
                std::cerr << "TileXR clock sync invalid sample rank=" << session->hostArgs->rank
                          << ", index=" << index
                          << ", status=" << sample.status
                          << ", sequence=" << sample.sequence
                          << ", t0=" << sample.t0
                          << ", t1=" << sample.t1
                          << ", t2=" << sample.t2
                          << ", t3=" << sample.t3 << ", detail=" << sample.reserved << std::endl;
            }
        }
    }
    ret = TileXRDiagnostics::Host::GatherControl(*session,
        TileXRDiagnostics::Host::ControlOperation::CLOCK_RESULT, sequence,
        static_cast<uint64_t>(magic), generation, local.status);
    if (ret != TileXR::TILEXR_SUCCESS) {
        if (session->clockUsesRdma) {
            session->clockRdmaFailed = true;
        }
        return ret;
    }
    TileXRClockSyncResult clock {};
    clock.version = TileXR::TILEXR_STRAGGLER_ABI_VERSION;
    clock.rankSize = static_cast<uint32_t>(session->hostArgs->rankSize);
    clock.sampleCount = session->config.clockSyncSamples;
    clock.generation = generation;
    clock.frequencySource = TILEXR_CLOCK_FREQUENCY_CANN_PRODUCT_RULE;
    clock.warmupSampleCount = session->config.clockSyncWarmupSamples;
    clock.cyclesPerSecond = session->cyclesPerSecond;
    ret = TileXRDiagnostics::Host::g_hooks->hostAllGather(
        session->comm, &local, sizeof(local), clock.ranks, session->config.timeoutMs);
    if (ret != TileXR::TILEXR_SUCCESS) {
        return ret;
    }
    for (uint32_t rank = 0; rank < clock.rankSize; ++rank) {
        if (clock.ranks[rank].rank != static_cast<int32_t>(rank) ||
            clock.ranks[rank].status != TileXR::TILEXR_SUCCESS) {
            std::cerr << "TileXR clock sync gathered result mismatch localRank="
                      << session->hostArgs->rank << ", expectedRank=" << rank
                      << ", gatheredRank=" << clock.ranks[rank].rank
                      << ", gatheredStatus=" << clock.ranks[rank].status
                      << ", localStatus=" << local.status
                      << ", localValidSamples=" << local.validSamples << std::endl;
            return TileXR::TILEXR_ERROR_INTERNAL;
        }
        clock.precisionCycles = std::max(clock.precisionCycles, clock.ranks[rank].errorBoundCycles);
        clock.precisionNs = std::max(clock.precisionNs, clock.ranks[rank].errorBoundNs);
    }
    ret = TileXRDiagnostics::Host::UpdateClockModels(*session, samples, sequence,
        static_cast<uint64_t>(magic), generation);
    if (ret != TileXR::TILEXR_SUCCESS) { return ret; }
    *result = clock;
    session->clock = clock;
    session->generation = generation;
    const uint64_t now = TileXRDiagnostics::Host::g_hooks->steadyNowNs();
    const bool driftReady = session->clockDrift.windowUs != 0 &&
        std::all_of(session->clockDriftModels.begin(),session->clockDriftModels.end(),
            [](const TileXRClockDriftModel &model) { return model.state == TILEXR_CLOCK_DRIFT_READY; });
    if (driftReady && session->clockDriftFrozenHostNs == 0) {
        session->clockDriftFrozenHostNs = now;
    }
    // Tracking probes validate the current model; its anchor still changes only
    // on accepted updates. Frozen mode keeps the original model age.
    session->calibrationHostNs = session->clockDrift.tracking ? now :
        (driftReady ? session->clockDriftFrozenHostNs : now);
    ret = TileXRDiagnostics::Host::WriteClockJson(*session,samples);
    if (session->clockDrift.tracking) {
        ret = TileXRDiagnostics::Host::GatherControl(*session,
            TileXRDiagnostics::Host::ControlOperation::CLOCK_PERSISTED,sequence,
            static_cast<uint64_t>(magic),generation,ret);
    }
    return ret;
}

extern "C" int TileXRClockGetDriftModel(TileXRStraggler detector, int rank,
                                         TileXRClockDriftModel *model)
{
    auto *session = static_cast<TileXRDiagnostics::Host::StragglerSession *>(detector);
    if (session == nullptr || model == nullptr || rank < 0 ||
        !TileXRDiagnostics::Host::ValidateOwner(*session)) {
        return TileXR::TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    std::lock_guard<std::mutex> guard(session->stateMutex);
    if (static_cast<size_t>(rank) >= session->clockDriftModels.size()) {
        return TileXR::TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    *model = session->clockDriftModels[static_cast<size_t>(rank)];
    return TileXR::TILEXR_SUCCESS;
}

extern "C" int TileXRStragglerCollect(TileXRStraggler detector,
                                                aclrtStream stream,
                                                TileXRStragglerReport *report)
{
    TileXRDiagnostics::Host::StragglerSession *session =
        static_cast<TileXRDiagnostics::Host::StragglerSession *>(detector);
    if (session == nullptr || stream == nullptr || report == nullptr) {
        return TileXR::TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    *report = TileXRStragglerReport {};
    if (!TileXRDiagnostics::Host::ValidateOwner(*session)) {
        return TileXR::TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    std::lock_guard<std::mutex> stateGuard(session->stateMutex);
    int localStatus = TileXR::TILEXR_SUCCESS;
    if (session->clock.version != TileXR::TILEXR_STRAGGLER_ABI_VERSION) {
        localStatus = TileXR::TILEXR_ERROR_NOT_INITIALIZED;
    } else if (session->nextCollectId > session->localLaunchId) {
        if (session->observationStatus != TileXR::TILEXR_SUCCESS) {
            localStatus = session->observationStatus;
            session->observationStatus = TileXR::TILEXR_SUCCESS;
        } else {
            localStatus = TileXR::TILEXR_ERROR_NOT_FOUND;
        }
    }
    const uint64_t ageNs = TileXRDiagnostics::Host::g_hooks->steadyNowNs() - session->calibrationHostNs;
    if (localStatus == TileXR::TILEXR_SUCCESS &&
        ageNs > static_cast<uint64_t>(session->config.maxCalibrationAgeMs) * 1000000ull) {
        localStatus = TileXR::TILEXR_ERROR_TIMEOUT;
    }
    const uint64_t sequence = ++session->controlSequence;
    int ret = TileXRDiagnostics::Host::GatherControl(*session,
        TileXRDiagnostics::Host::ControlOperation::COLLECT_PREFLIGHT, sequence,
        session->nextCollectId, session->clock.generation, localStatus);
    if (ret != TileXR::TILEXR_SUCCESS) {
        return ret;
    }
    const uint64_t slot = (session->nextCollectId - 1u) % session->config.recordCapacity;
    aclrtStream recordStream = session->recordStreams[static_cast<uint32_t>(slot)];
    const bool streamSynchronized = recordStream == nullptr ||
        TileXRDiagnostics::Host::g_hooks->synchronizeStream(recordStream) == ACL_SUCCESS;
    if (streamSynchronized && recordStream != nullptr) {
        session->recordStreams[static_cast<uint32_t>(slot)] = nullptr;
        TileXRDiagnostics::Host::UntrackStreamIfIdle(*session, recordStream);
    }
    const void *deviceRecord = static_cast<const uint8_t *>(session->deviceRecords) +
        TileXR::StragglerTimingRecordOffset(static_cast<uint32_t>(slot));
    TileXR::TileXRStragglerTimingRecord local {};
    const int recordStatus = session->recordStatuses[static_cast<uint32_t>(slot)];
    if (!streamSynchronized) {
        local.status = TileXR::TILEXR_TIMING_RECORD_STATUS_STARTED;
        local.rank = static_cast<uint32_t>(session->hostArgs->rank);
        local.launchId = session->nextCollectId;
        local.generation = session->clock.generation;
    } else if (TileXRDiagnostics::Host::g_hooks->copyDeviceToHost(&local, sizeof(local),
                   deviceRecord, sizeof(local)) != ACL_SUCCESS ||
               recordStatus != TileXR::TILEXR_SUCCESS) {
        local.status = TileXR::TILEXR_TIMING_RECORD_STATUS_ERROR;
        local.rank = static_cast<uint32_t>(session->hostArgs->rank);
        local.launchId = session->nextCollectId;
        local.generation = session->clock.generation;
    }
    std::vector<TileXR::TileXRStragglerTimingRecord> records(session->hostArgs->rankSize);
    ret = TileXRDiagnostics::Host::g_hooks->hostAllGather(
        session->comm, &local, sizeof(local), records.data(), session->config.timeoutMs);
    if (ret != TileXR::TILEXR_SUCCESS) {
        return ret;
    }
    ret = TileXRDiagnostics::Host::ClassifyStragglerRecords(
        session->config, session->clock, records, *report, session->clockDriftModels.data());
    report->droppedRecords = session->droppedRecords;
    report->calibrationAgeNs = ageNs;
    const bool pendingProducer = std::find_if(records.begin(), records.end(),
        [](const TileXR::TileXRStragglerTimingRecord &record) {
            return record.status == TileXR::TILEXR_TIMING_RECORD_STATUS_STARTED;
        }) != records.end();
    if (ret != TileXR::TILEXR_SUCCESS) {
        TileXRDiagnostics::Host::WriteReportJson(*session, *report);
        if (pendingProducer) {
            return ret;
        }
    }
    session->recordStatuses[static_cast<uint32_t>(slot)] = TileXR::TILEXR_SUCCESS;
    ++session->nextCollectId;
    return ret != TileXR::TILEXR_SUCCESS ? ret :
        TileXRDiagnostics::Host::WriteReportJson(*session, *report);
}
