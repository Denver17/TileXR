/*
 * 用独立路由 oracle 核对双 rank 的 compact Dispatch 输出，并交给生产 Combine。
 * 比较最大预分配、Host 实际大小参考和 Device pool；Host 计时与供 msprof 对齐的
 * kernel 序号分开记录，各阶段准备后执行 Host AllGather。
 * 环绕用例保留上一轮 Allocation 至本轮完成，登记 Combine stream 后释放引用并等待退休。
 * 可选 SDK capture 绑定 Pool service stream，在 Pending 完成后采集，避免阻塞 Host 扩容。
 */
#include "tilexr_elastic_ep.h"
#include "device/tilexr_elastic_memory_profile.h"
#include "tilexr_ep.h"
#include "tilexr/profiling.h"
#include <acl/acl.h>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <exception>
#include <stdexcept>
#include <string>
#include <vector>
#include <chrono>
#include <thread>
#include <iomanip>
#include <memory>
#include <algorithm>

// Existing internal Host control channel from src/comm/comm_wrap.cpp; no Device work.
int TileXRCommHostAllGatherTimeout(TileXRCommPtr comm, const void *sendBuf, size_t sendBytes,
    void *recvBuf, uint32_t timeoutMs);

namespace {
void Check(int result, const char *what)
{
    if (result != 0) {
        throw std::runtime_error(std::string(what) + ": " + std::to_string(result));
    }
}
void Require(bool condition, const char *what)
{
    if (!condition) {
        throw std::runtime_error(what);
    }
}
uint16_t Bf16(float value)
{
    uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return static_cast<uint16_t>((bits + 0x7fffU + ((bits >> 16) & 1U)) >> 16);
}
int Route(int source, int token, int k, bool zero)
{
    return zero ? k : (token * 2 + k + source) % 4;
}
float Value(int source, int token)
{
    return float(source * 16 + token + 1);
}
struct Buffer {
    explicit Buffer(size_t size) : bytes(size)
    {
        Check(aclrtMalloc(&data, bytes, ACL_MEM_MALLOC_NORMAL_ONLY), "aclrtMalloc");
    }
    ~Buffer()
    {
        if (data != nullptr && !std::uncaught_exception()) {
            (void)aclrtFree(data);
        }
    }
    void *data = nullptr;
    size_t bytes;
    Buffer(const Buffer &) = delete;
    Buffer &operator=(const Buffer &) = delete;
};
template <class T> void Upload(Buffer &buffer, const std::vector<T> &values)
{
    Check(aclrtMemcpy(buffer.data, buffer.bytes, values.data(), values.size() * sizeof(T),
        ACL_MEMCPY_HOST_TO_DEVICE), "upload");
}
template <class T> std::vector<T> Download(const void *data, size_t count)
{
    std::vector<T> result(count);
    if (count != 0) {
        Check(aclrtMemcpy(result.data(), count * sizeof(T), data, count * sizeof(T),
            ACL_MEMCPY_DEVICE_TO_HOST), "download");
    }
    return result;
}
struct PerfCase {
    const char *outputMode;
    const char *phase;
    const char *sampleKind;
    int sample;
    bool trimAfter = false;
    uint64_t *baselinePeak = nullptr;
    bool kernelPerf = false;
    int ordinal = 0;
    Buffer *reusableBaseline = nullptr;
    std::shared_ptr<TileXR::Elastic::Allocation> *retainedOutput = nullptr;
};
void StageBarrier(TileXRCommPtr comm, const PerfCase *perf, int stage)
{
    if (perf == nullptr || !perf->kernelPerf) return;
    const uint64_t token = uint64_t(perf->ordinal) * 2 + stage;
    uint64_t gathered[2] = {};
    Check(TileXRCommHostAllGatherTimeout(comm, &token, sizeof(token), gathered, 10000),
        "kernel_perf stage barrier");
    Require(gathered[0] == token && gathered[1] == token, "kernel_perf stages diverged");
}
void CheckProfile(const TileXR::Profiling::Status &status, const char *operation)
{
    if (!status) throw std::runtime_error(std::string(operation) + ": " + status.message);
}
TileXR::Profiling::CaptureConfig ProfileConfig(int rank, int ordinal)
{
    TileXR::Profiling::CaptureConfig config;
    config.rank = rank;
    config.recordsPerStream = 64;
    const char *seed = std::getenv("TILEXR_ELASTIC_CAPTURE_ID");
    Require(seed != nullptr && std::strlen(seed) == 32, "capture requires shared fresh 32-hex UUID seed");
    const std::string digits = "0123456789abcdef";
    bool nonzero = false;
    for (size_t i = 0; i < 16; ++i) {
        const auto hi = digits.find(seed[i * 2]);
        const auto lo = digits.find(seed[i * 2 + 1]);
        Require(hi != std::string::npos && lo != std::string::npos, "invalid capture UUID seed");
        config.captureId[i] = uint8_t(hi * 16 + lo);
        nonzero = nonzero || config.captureId[i] != 0;
    }
    Require(nonzero, "capture UUID seed must be nonzero");
    for (unsigned i = 0; i < 4; ++i) config.captureId[12 + i] ^= uint8_t(uint32_t(ordinal) >> (i * 8));
    Require(std::any_of(config.captureId.begin(), config.captureId.end(), [](uint8_t x) { return x != 0; }),
        "derived capture UUID must be nonzero");
    return config;
}
uint64_t OracleRows(int rank, int bs, bool zero)
{
    uint64_t rows = 0;
    for (int source = 0; source < 2; ++source) {
        for (int token = 0; token < bs; ++token) {
            for (int k = 0; k < 2; ++k) {
                if (Route(source, token, k, zero) / 2 == rank) {
                    ++rows;
                }
            }
        }
    }
    return rows;
}
using Clock = std::chrono::steady_clock;
double Micros(Clock::time_point begin, Clock::time_point end)
{
    return std::chrono::duration<double, std::micro>(end - begin).count();
}
TileXR::Elastic::Stats RetireCompleted(TileXR::Elastic::Pool &pool, bool retainedOutput = false)
{
    const auto deadline = Clock::now() + std::chrono::seconds(10);
    for (;;) {
        const auto stats = pool.Collect();
        if ((retainedOutput || stats.liveBytes == 0) && stats.pendingRetireBytes == 0) {
            return stats;
        }
        Require(Clock::now() < deadline, "completed output did not retire");
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}
void RunRound(TileXR::Elastic::Pool &pool, TileXRCommPtr comm, aclrtStream stream,
    int rank, int bs, bool zero, bool fail, const PerfCase *perf = nullptr, int h = 128)
{
    std::vector<uint16_t> x(size_t(bs) * h);
    std::vector<int32_t> ids(size_t(bs) * 2);
    for (int token = 0; token < bs; ++token) {
        for (int column = 0; column < h; ++column) {
            x[size_t(token) * h + column] = Bf16(Value(rank, token));
        }
        for (int k = 0; k < 2; ++k) {
            ids[size_t(token) * 2 + k] = Route(rank, token, k, zero);
        }
    }
    Buffer input(x.size() * 2), routes(ids.size() * 4), counts(4 * 4), experts(2 * 8);
    Upload(input, x);
    Upload(routes, ids);
    TileXR::Elastic::CompactParams params;
    params.comm = comm;
    params.x = input.data;
    params.expertIds = static_cast<int32_t *>(routes.data);
    params.expertTokenNums = static_cast<int64_t *>(experts.data);
    params.sendCounts = static_cast<int32_t *>(counts.data);
    params.bs = bs;
    params.h = h;
    params.topK = 2;
    params.experts = 4;
    const bool devicePool = perf == nullptr || std::strcmp(perf->outputMode, "device_pool") == 0;
    // This Host oracle is a comparison-only allocation reference. Production compact
    // Dispatch receives no size from it and independently computes rows on Device.
    const uint64_t oracleRows = OracleRows(rank, bs, zero);
    const uint64_t maxRows = uint64_t(2) * bs * 2;
    const uint64_t maxOutputBytes = TileXR::Elastic::CompactAssistOffset(maxRows, h) + maxRows * 12;
    const uint64_t actualOutputBytes = TileXR::Elastic::CompactAssistOffset(oracleRows, h) + oracleRows * 12;
    const auto before = pool.Snapshot();
    std::shared_ptr<TileXR::Profiling::AclCapture> capture;
    const char *profileDir = std::getenv("TILEXR_ELASTIC_PROFILE_DIR");
    const bool sampleProfile = profileDir != nullptr && profileDir[0] != '\0' && devicePool &&
        perf != nullptr && perf->kernelPerf && std::strcmp(perf->sampleKind, "measurement") == 0;
    std::shared_ptr<TileXR::Elastic::Pending> pending;
    std::shared_ptr<TileXR::Elastic::Allocation> allocation;
    std::unique_ptr<Buffer> baseline;
    void *hiddenData = nullptr;
    int32_t *assistData = nullptr;
    uint64_t outputBytes = 0;
    const auto begin = Clock::now();
    if (!devicePool) {
        const uint64_t capacityRows = std::strcmp(perf->outputMode, "maximum_capacity") == 0 ? maxRows : oracleRows;
        outputBytes = TileXR::Elastic::CompactAssistOffset(capacityRows, h) + capacityRows * 12;
        if (perf->reusableBaseline != nullptr) {
            Require(perf->reusableBaseline->bytes == outputBytes, "preallocated output size mismatch");
            hiddenData = perf->reusableBaseline->data;
        } else {
            baseline.reset(new Buffer(outputBytes));
            hiddenData = baseline->data;
        }
        assistData = reinterpret_cast<int32_t *>(static_cast<uint8_t *>(hiddenData) +
            TileXR::Elastic::CompactAssistOffset(capacityRows, h));
    }
    const auto allocated = Clock::now();
    Clock::time_point submitted;
    StageBarrier(comm, perf, 0);
    if (devicePool) {
        if (sampleProfile) {
            capture = std::make_shared<TileXR::Profiling::AclCapture>();
            auto profileConfig = ProfileConfig(rank, perf->ordinal);
            pending = TileXR::Elastic::DispatchCompactProfiled(pool, params, stream,
                [capture, profileConfig](aclrtStream serviceStream, uint32_t lanes) mutable {
                    profileConfig.logicalStreamCount = lanes;
                    CheckProfile(capture->Initialize(profileConfig, serviceStream), "initialize capture");
                    return reinterpret_cast<uint64_t>(capture->DeviceArgs());
                });
        } else {
            pending = TileXR::Elastic::DispatchCompact(pool, params, stream);
        }
        submitted = Clock::now();
    }
    try {
        if (devicePool) {
            allocation = pending->Wait();
            hiddenData = allocation->Data();
            outputBytes = allocation->Bytes();
            assistData = allocation->Rows() == 0 ? nullptr : reinterpret_cast<int32_t *>(
                static_cast<uint8_t *>(hiddenData) + TileXR::Elastic::CompactAssistOffset(allocation->Rows(), h));
        } else {
            Check(TileXRMoeEpDispatchMemory(params.x, params.expertIds, nullptr, nullptr, 0,
                nullptr, comm, bs, h, 2, 4, 2, rank, 1, 0, 0, 0, 0, 0, int64_t(bs) * 2,
                1, hiddenData, nullptr, assistData, params.expertTokenNums, params.sendCounts,
                nullptr, nullptr, params.dtype, params.dtype, stream), "baseline DispatchMemory");
            submitted = Clock::now();
            Check(aclrtSynchronizeStream(stream), "baseline Dispatch completion");
        }
    } catch (const std::exception &error) {
        if (!fail) {
            throw;
        }
        std::cout << "PASS expected capacity failure rank=" << rank << " error=" << error.what() << std::endl;
        // Pending confirms kernel completion before reporting a capacity error.
        // Do not reuse this communicator after its failed communication call.
        std::_Exit(0);
    }
    if (capture) {
        // Wait above confirms the native kernel has completed and the Host growth loop
        // has run. Collection here cannot stall that loop behind its own resident kernel.
        CheckProfile(capture->Collect(), "collect capture");
        const auto *snapshot = capture->Snapshot();
        Require(snapshot != nullptr && snapshot->streams.size() == capture->Config().logicalStreamCount,
            "missing profiling lanes");
        for (size_t lane = 0; lane < snapshot->streams.size(); ++lane) {
            const auto &record = snapshot->streams[lane];
            Require(record.id == ((uint32_t(rank) << 16) | uint32_t(lane)) && record.complete && record.dropped == 0 &&
                record.records.size() >= 2 && record.records.front().eventId == 1 && record.records.back().eventId == TileXR::Elastic::Profile::KernelEnd + 1,
                "invalid/incomplete/overflowed profiling lane");
        }
        const std::string path = std::string(profileDir) + "/capture-" + std::to_string(perf->ordinal) +
            "-rank" + std::to_string(rank) + ".records.bin";
        CheckProfile(capture->Export(path), "export capture");
        std::cout << "PROFILE rank=" << rank << " ordinal=" << perf->ordinal
            << " lanes=" << snapshot->streams.size() << " file=" << path << std::endl;
        CheckProfile(capture->Close(), "close capture");
    }
    const auto dispatched = Clock::now();
    const auto atCompletion = pool.Snapshot();
    Require(!fail, "capacity injection unexpectedly succeeded");
    std::vector<uint16_t> expectedHidden;
    std::vector<int32_t> expectedAssist, expectedCounts;
    std::vector<int64_t> expectedExperts;
    for (int expert = rank * 2; expert < rank * 2 + 2; ++expert) {
        int64_t expertRows = 0;
        for (int source = 0; source < 2; ++source) {
            for (int token = 0; token < bs; ++token) {
                for (int k = 0; k < 2; ++k) {
                    if (Route(source, token, k, zero) != expert) {
                        continue;
                    }
                    expectedHidden.insert(expectedHidden.end(), h, Bf16(Value(source, token)));
                    expectedAssist.insert(expectedAssist.end(), {source, token, k});
                    ++expertRows;
                }
            }
            expectedCounts.push_back(static_cast<int32_t>(expectedAssist.size() / 3));
        }
        expectedExperts.push_back(expertRows);
    }
    const uint64_t rows = expectedAssist.size() / 3;
    Require(rows == oracleRows, "independent row oracles disagree");
    if (devicePool) {
        Require(allocation->Rows() == rows, "actual rows mismatch");
        Require(allocation->Bytes() == actualOutputBytes, "bundle is not actual-sized");
    }
    Require(Download<uint16_t>(hiddenData, expectedHidden.size()) == expectedHidden, "hidden mismatch");
    Require(Download<int32_t>(assistData, expectedAssist.size()) == expectedAssist, "assist mismatch");
    Require(Download<int32_t>(counts.data, 4) == expectedCounts, "counts mismatch");
    Require(Download<int64_t>(experts.data, 2) == expectedExperts, "expert counts mismatch");
    double combineUs = 0;
    double combineSubmitUs = 0;
    if (!zero) {
        std::vector<float> weights(size_t(bs) * 2, 0.5f);
        Buffer weightsDevice(weights.size() * 4), output(size_t(bs) * h * 2);
        Upload(weightsDevice, weights);
        if (devicePool) {
            allocation->RecordStream(stream);
        }
        StageBarrier(comm, perf, 1);
        const auto combineBegin = Clock::now();
        Check(TileXRMoeEpCombineMemory(hiddenData, assistData, static_cast<int32_t *>(counts.data),
            static_cast<float *>(weightsDevice.data), nullptr, 0, nullptr, comm, bs, h, 2, 4,
            2, rank, 1, 0, 0, 0, 0, 0, int64_t(bs) * 2, output.data,
            TileXR::TILEXR_DATA_TYPE_BFP16, stream), "CombineMemory");
        const auto combineSubmitted = Clock::now();
        Check(aclrtSynchronizeStream(stream), "Combine completion");
        combineUs = Micros(combineBegin, Clock::now());
        combineSubmitUs = Micros(combineBegin, combineSubmitted);
        Require(Download<uint16_t>(output.data, x.size()) == x, "Dispatch->Combine mismatch");
    }
    const auto identity = allocation ? allocation->Identity() : TileXR::Elastic::Handle{};
    const bool ringRetained = perf != nullptr && perf->retainedOutput != nullptr;
    bool ringWrap = false;
    if (ringRetained && *perf->retainedOutput) {
        const auto &previous = *perf->retainedOutput;
        const auto previousIdentity = previous->Identity();
        Require(previous->Bytes() == actualOutputBytes && previous->Rows() == rows,
            "retained output layout changed");
        Require(identity.generation > previousIdentity.generation && identity.segmentId == previousIdentity.segmentId,
            "retained ring changed segment or reused generation");
        Require(identity.offset + identity.bytes <= previousIdentity.offset ||
            previousIdentity.offset + previousIdentity.bytes <= identity.offset,
            "new output overlaps retained output");
        ringWrap = identity.offset < previousIdentity.offset;
        Require(Download<uint16_t>(previous->Data(), expectedHidden.size()) == expectedHidden,
            "retained hidden was overwritten");
        const void *previousAssist = static_cast<const uint8_t *>(previous->Data()) +
            TileXR::Elastic::CompactAssistOffset(previous->Rows(), h);
        Require(Download<int32_t>(previousAssist, expectedAssist.size()) == expectedAssist,
            "retained assist was overwritten");
    }
    const auto releaseBegin = Clock::now();
    if (ringRetained) *perf->retainedOutput = allocation;
    allocation.reset();
    pending.reset();
    baseline.reset();
    auto stats = perf == nullptr ? pool.Collect() : RetireCompleted(pool, ringRetained);
    if (ringWrap) Require(stats.paddingBytes > 0, "ring wrap did not retain suffix padding");
    const double releaseUs = Micros(releaseBegin, Clock::now());
    uint64_t trimmedBytes = 0;
    double trimUs = 0;
    if (perf != nullptr && perf->trimAfter && devicePool) {
        const uint64_t priorCommitted = stats.committedBytes;
        const auto trimBegin = Clock::now();
        stats = pool.Trim(true);
        trimUs = Micros(trimBegin, Clock::now());
        trimmedBytes = priorCommitted - stats.committedBytes;
        Require(stats.committedBytes == 0, "perf shrink did not return all retired output HBM");
    }
    if (perf == nullptr) {
        std::cout << "PASS rank=" << rank << " bs=" << bs << " rows=" << rows
            << " committed=" << stats.committedBytes << " growth=" << stats.growthCount << std::endl;
        return;
    }
    if (!devicePool && perf->baselinePeak != nullptr) {
        *perf->baselinePeak = std::max(*perf->baselinePeak, outputBytes);
    }
    const uint64_t growthDelta = stats.growthCount - before.growthCount;
    if (devicePool && (std::strcmp(perf->sampleKind, "measurement") == 0 ||
        std::strcmp(perf->phase, "hot") == 0)) {
        Require(growthDelta == 0, "warmed perf call performed Host HBM growth");
    }
    if (devicePool && std::strcmp(perf->sampleKind, "transition") == 0 &&
        (std::strcmp(perf->phase, "cold") == 0 || std::strcmp(perf->phase, "growth") == 0)) {
        Require(growthDelta == 1, "cold/growth transition did not perform exactly one HBM growth");
    }
    TileXR::CommArgs *commArgs = nullptr;
    Check(TileXRGetCommArgsHost(comm, commArgs), "perf communicator accounting");
    const double dispatchUs = Micros(begin, dispatched);
    const uint64_t payloadBytes = rows * h * 2;
    const uint64_t copyBytes = payloadBytes + rows * 12;
    if (perf->kernelPerf) {
        if (devicePool) {
            Require(growthDelta == (perf->ordinal == 0 ? 1U : 0U),
                "kernel_perf must grow once in first warmup only");
        }
        // Ordinals count only the named Dispatch/Combine kernels within this process;
        // copies, events and allocator service work are not assigned kernel ordinals.
        std::cout << "{\"schema\":\"tilexr.elastic.kernel_perf.v1\",\"rank\":" << rank
            << ",\"mode\":\"" << perf->outputMode << "\",\"phase\":\"" << perf->phase
            << "\",\"sample_kind\":\"" << perf->sampleKind << "\",\"sample\":" << perf->sample
            << ",\"ordinal\":" << perf->ordinal
            << ",\"kernel_ordinal\":" << perf->ordinal
            << ",\"warmup\":" << (std::strcmp(perf->sampleKind, "warmup") == 0 ? "true" : "false")
            << ",\"dispatch_kernel_ordinal\":" << perf->ordinal
            << ",\"combine_kernel_ordinal\":" << perf->ordinal
            << ",\"backend\":\"native_memory\",\"native_status\":\"PASS\",\"dtype\":\"bf16\""
            << ",\"dispatch_kernel_version\":\"" << (devicePool ? "tilexr_ep_dispatch_compact_kernel" : "tilexr_ep_dispatch_memory_kernel")
            << "\",\"combine_kernel_version\":\"tilexr_ep_combine_memory_kernel\""
            << ",\"timing_scope\":\"external_msprof_device_task_duration\",\"kernel_duration_us\":null"
            << ",\"diagnostic_state\":\"inherited_same_process_and_build\",\"completion_sync\":true"
            << ",\"stage_barrier\":\"host_allgather_before_each_stage\""
            << ",\"baseline_output_preallocated\":" << (devicePool ? "false" : "true")
            << ",\"host_actual_size_is_oracle_reference\":" << (std::strcmp(perf->outputMode, "host_actual_reference") == 0 ? "true" : "false")
            << ",\"world_size\":2,\"bs\":" << bs << ",\"h\":" << h << ",\"top_k\":2,\"experts\":4"
            << ",\"actual_rows\":" << rows << ",\"padded_rows\":" << rows
            << ",\"payload_bytes\":" << payloadBytes << ",\"assist_bytes\":" << rows * 12
            << ",\"effective_output_copy_bytes\":" << copyBytes << ",\"control_count_bytes\":32"
            << ",\"combine_logical_input_bytes\":" << payloadBytes
            << ",\"combine_output_bytes\":" << uint64_t(bs) * h * 2
            << ",\"staging_peer_window_bytes\":" << commArgs->peerMemBytes
            << ",\"max_capacity_rows\":" << maxRows << ",\"max_capacity_output_bytes\":" << maxOutputBytes
            << ",\"actual_bundle_bytes\":" << actualOutputBytes << ",\"allocated_output_bytes\":" << outputBytes
            << ",\"committed_at_completion\":" << (devicePool ? atCompletion.committedBytes : outputBytes)
            << ",\"committed_after_round\":" << (devicePool ? stats.committedBytes : outputBytes)
            << ",\"peak_committed\":" << (devicePool ? stats.peakCommittedBytes : outputBytes)
            << ",\"pool_metadata_bytes\":" << (devicePool ? stats.metadataBytes : 0)
            << ",\"ring_retained\":" << (ringRetained ? "true" : "false")
            << ",\"ring_wrap\":" << (ringWrap ? "true" : "false")
            << ",\"allocation_segment_id\":" << identity.segmentId
            << ",\"allocation_generation\":" << identity.generation
            << ",\"allocation_offset\":" << identity.offset
            << ",\"padding_bytes\":" << stats.paddingBytes
            << ",\"padding_at_completion\":" << atCompletion.paddingBytes
            << ",\"blocked_free_bytes\":" << stats.blockedFreeBytes
            << ",\"growth_before\":" << before.growthCount
            << ",\"growth_after\":" << stats.growthCount
            << ",\"growth_delta\":" << growthDelta << ",\"trimmed_bytes\":0}"
            << std::endl;
        return;
    }
    std::cout << std::fixed << std::setprecision(3)
        << "{\"schema\":\"tilexr.elastic.perf.v1\",\"rank\":" << rank
        << ",\"mode\":\"" << perf->outputMode << "\",\"phase\":\"" << perf->phase
        << "\",\"sample_kind\":\"" << perf->sampleKind << "\",\"sample\":" << perf->sample
        << ",\"backend\":\"native_memory\",\"native_status\":\"PASS\",\"dtype\":\"bf16\""
        << ",\"dispatch_kernel_version\":\"" << (devicePool ? "tilexr_ep_dispatch_compact_kernel" : "tilexr_ep_dispatch_memory_kernel")
        << "\",\"combine_kernel_version\":\"tilexr_ep_combine_memory_kernel\""
        << ",\"timing_scope\":\"host_output_allocation_plus_submit_and_completion_wait_not_pure_kernel\""
        << ",\"host_oracle_compute_in_timing\":false,\"diagnostic_state\":\"inherited_same_process_and_build\""
        << ",\"host_actual_size_is_oracle_reference\":" << (std::strcmp(perf->outputMode, "host_actual_reference") == 0 ? "true" : "false")
        << ",\"world_size\":2,\"bs\":" << bs << ",\"h\":" << h << ",\"top_k\":2,\"experts\":4"
        << ",\"actual_rows\":" << rows << ",\"padded_rows\":" << rows
        << ",\"payload_bytes\":" << payloadBytes << ",\"assist_bytes\":" << rows * 12
        << ",\"effective_output_copy_bytes\":" << copyBytes << ",\"control_count_bytes\":32"
        << ",\"staging_peer_window_bytes\":" << commArgs->peerMemBytes
        << ",\"max_capacity_rows\":" << maxRows << ",\"max_capacity_output_bytes\":" << maxOutputBytes
        << ",\"actual_bundle_bytes\":" << actualOutputBytes << ",\"allocated_output_bytes\":" << outputBytes
        << ",\"committed_at_completion\":" << (devicePool ? atCompletion.committedBytes : outputBytes)
        << ",\"committed_after_release\":" << stats.committedBytes
        << ",\"peak_committed\":" << (devicePool ? stats.peakCommittedBytes : *perf->baselinePeak)
        << ",\"pool_metadata_bytes\":" << (devicePool ? stats.metadataBytes : 0)
        << ",\"growth_delta\":" << growthDelta << ",\"trimmed_bytes\":" << trimmedBytes
        << ",\"host_output_allocation_us\":" << (devicePool ? 0 : Micros(begin, allocated))
        << ",\"dispatch_submit_cpu_us\":" << Micros(allocated, submitted)
        << ",\"dispatch_call_and_wait_us\":" << Micros(allocated, dispatched)
        << ",\"dispatch_completion_wall_us\":" << dispatchUs
        << ",\"dispatch_latency\":\"" << dispatchUs << " us\""
        << ",\"combine_submit_cpu_us\":" << combineSubmitUs
        << ",\"combine_completion_wall_us\":" << combineUs
        << ",\"combine_latency\":\"" << combineUs << " us\""
        << ",\"combine_logical_input_bytes\":" << payloadBytes
        << ",\"combine_output_bytes\":" << uint64_t(bs) * h * 2
        << ",\"combine_effective_output_GB_per_s\":" << (combineUs > 0 ? double(uint64_t(bs) * h * 2) / combineUs / 1000 : 0)
        << ",\"output_release_wait_us\":" << releaseUs << ",\"trim_us\":" << trimUs
        << ",\"effective_copy_GB_per_s\":" << (dispatchUs > 0 ? double(copyBytes) / dispatchUs / 1000 : 0)
        << ",\"bandwidth_scope\":\"logical_final_output_bytes_over_host_completion_wall_not_link_bandwidth\"}"
        << std::endl;
}
void RunPerf(TileXR::Elastic::Pool &pool, TileXRCommPtr comm, aclrtStream stream, int rank,
             const std::string &selectedMode)
{
    const char *phases[] = {"cold", "hot", "growth", "shrink"};
    const int sizes[] = {16, 16, 64, 16};
    for (const char *mode : {"maximum_capacity", "host_actual_reference", "device_pool"}) {
        if (!selectedMode.empty() && selectedMode != mode) continue;
        uint64_t baselinePeak = 0;
        for (int phase = 0; phase < 4; ++phase) {
            PerfCase sample {mode, phases[phase], "transition", -1, phase == 3, &baselinePeak};
            RunRound(pool, comm, stream, rank, sizes[phase], false, false, &sample);
            sample.trimAfter = false;
            sample.sampleKind = "warmup";
            for (int i = 0; i < 3; ++i) {
                sample.sample = i;
                RunRound(pool, comm, stream, rank, sizes[phase], false, false, &sample);
            }
            sample.sampleKind = "measurement";
            for (int i = 0; i < 10; ++i) {
                sample.sample = i;
                RunRound(pool, comm, stream, rank, sizes[phase], false, false, &sample);
            }
        }
    }
}
void RunKernelPerf(TileXR::Elastic::Pool &pool, TileXRCommPtr comm, aclrtStream stream,
    int rank, const std::string &mode, int bs, int h, int warmup, int samples, bool ringRetained = false)
{
    // Agree on capture identity before either rank launches a measured invocation.
    // Rank and lane remain separate fields; the UUID identifies the shared capture.
    struct CaptureAgreement { uint64_t enabled; uint8_t id[16]; } local = {}, paired[2] = {};
    const char *profileDir = std::getenv("TILEXR_ELASTIC_PROFILE_DIR");
    local.enabled = mode == "device_pool" && profileDir != nullptr && profileDir[0] != '\0';
    if (local.enabled) {
        const auto config = ProfileConfig(rank, 0);
        std::copy(config.captureId.begin(), config.captureId.end(), local.id);
    }
    Check(TileXRCommHostAllGatherTimeout(comm, &local, sizeof(local), paired, 10000),
        "kernel_perf capture agreement");
    Require(paired[0].enabled == paired[1].enabled &&
        std::memcmp(paired[0].id, paired[1].id, sizeof(local.id)) == 0,
        "kernel_perf ranks disagree on capture enable/UUID");
    std::unique_ptr<Buffer> baseline;
    if (mode != "device_pool") {
        const uint64_t capacityRows = mode == "maximum_capacity" ? uint64_t(bs) * 4 : OracleRows(rank, bs, false);
        baseline.reset(new Buffer(TileXR::Elastic::CompactAssistOffset(capacityRows, h) + capacityRows * 12));
    }
    uint64_t baselinePeak = 0;
    uint64_t measuredGrowth = 0;
    std::shared_ptr<TileXR::Elastic::Allocation> retained;
    for (int ordinal = 0; ordinal < warmup + samples; ++ordinal) {
        if (ordinal == warmup) {
            measuredGrowth = pool.Snapshot().growthCount;
        }
        PerfCase sample {mode.c_str(), ordinal == 0 ? "cold" : "hot",
            ordinal < warmup ? "warmup" : "measurement", ordinal < warmup ? ordinal : ordinal - warmup,
            false, &baselinePeak, true, ordinal, baseline.get(), ringRetained ? &retained : nullptr};
        RunRound(pool, comm, stream, rank, bs, false, false, &sample, h);
    }
    Require(pool.Snapshot().growthCount == measuredGrowth,
        "kernel_perf measurement region performed HBM growth");
    if (ringRetained) {
        retained.reset();
        RetireCompleted(pool);
    }
}
}
int main(int argc, char **argv)
{
    try {
        Require(argc >= 3, "usage: test_compact_ep RANK DEVICE [normal|zero|fail|perf] [PERF_MODE]; "
            "or RANK DEVICE [kernel_perf|kernel_ring_perf] MODE BS H WARMUP SAMPLES");
        const int rank = std::stoi(argv[1]);
        const int device = std::stoi(argv[2]);
        const std::string mode = argc > 3 ? argv[3] : "normal";
        const bool ringPerf = mode == "kernel_ring_perf";
        const bool kernelPerf = mode == "kernel_perf" || ringPerf;
        Require(rank == 0 || rank == 1, "two ranks required");
        Require(mode == "normal" || mode == "zero" || mode == "fail" || mode == "perf" ||
            kernelPerf, "invalid mode");
        const std::string perfMode = argc > 4 ? argv[4] : "";
        Require(perfMode.empty() || ((mode == "perf" || kernelPerf) && (perfMode == "maximum_capacity" ||
            perfMode == "host_actual_reference" || perfMode == "device_pool")), "invalid perf mode");
        int kernelBs = 16, kernelH = 128, warmup = 5, samples = 50;
        if (kernelPerf) {
            Require(argc == 9 && !perfMode.empty(), "kernel_perf requires MODE BS H WARMUP SAMPLES");
            Require(!ringPerf || perfMode == "device_pool", "kernel_ring_perf requires device_pool");
            kernelBs = std::stoi(argv[5]);
            kernelH = std::stoi(argv[6]);
            warmup = std::stoi(argv[7]);
            samples = std::stoi(argv[8]);
            Require(kernelBs >= 1 && kernelBs <= 256 && kernelH >= 128 && kernelH <= 4096 && kernelH % 16 == 0,
                "kernel_perf supports BS 1..256 and H 128..4096 in multiples of 16");
            Require(warmup >= 1 && warmup <= 1000 && samples >= 1 && samples <= 10000,
                "kernel_perf requires 1..1000 warmup and 1..10000 measurement rounds");
        }
        Check(aclInit(nullptr), "aclInit");
        Check(aclrtSetDevice(device), "set device");
        aclrtStream stream = nullptr;
        Check(aclrtCreateStream(&stream), "create stream");
        TileXRCommPtr comm = nullptr;
        Check(TileXRCommInitRankLocal(2, rank, &comm), "create communicator");
        TileXR::Elastic::Config config;
        config.segmentBytes = mode == "fail" && rank == 0 ? 64 : 4096;
        config.maxCommittedBytes = mode == "fail" && rank == 0 ? 64 : 4ULL * 1024 * 1024;
        if (kernelPerf) {
            config.maxCommittedBytes = 128ULL * 1024 * 1024;
            if (ringPerf) {
                const uint64_t rows = OracleRows(rank, kernelBs, false);
                const uint64_t bundle = TileXR::Elastic::CompactAssistOffset(rows, kernelH) + rows * 12;
                const uint64_t stride = (bundle + 63) & ~uint64_t(63);
                // Two live outputs fit, while a third needs a nonempty wrap with suffix padding.
                config.segmentBytes = 2 * stride + ((stride / 2) & ~uint64_t(63));
                Require(config.segmentBytes > 2 * stride && config.segmentBytes < 3 * stride,
                    "ring fixture requires two outputs and partial suffix headroom");
            }
        }
        config.maxCachedFreeBytes = config.maxCommittedBytes;
        auto pool = TileXR::Elastic::Pool::Create(config);
        Require(pool->Snapshot().committedBytes == 0, "initial output HBM must be zero");
        if (kernelPerf) {
            RunKernelPerf(*pool, comm, stream, rank, perfMode, kernelBs, kernelH, warmup, samples, ringPerf);
        } else if (mode == "perf") {
            RunPerf(*pool, comm, stream, rank, perfMode);
        } else if (mode == "normal") {
            for (int bs : {16, 16, 64, 16}) {
                RunRound(*pool, comm, stream, rank, bs, false, false);
            }
        } else {
            RunRound(*pool, comm, stream, rank, 16, mode == "zero", mode == "fail");
        }
        const auto trimmed = pool->Trim(true);
        Require(trimmed.committedBytes == 0, "trim did not return output HBM");
        pool->Close();
        pool.reset();
        Check(TileXRCommDestroy(comm), "destroy communicator");
        Check(aclrtDestroyStream(stream), "destroy stream");
        std::cout << "PASS complete rank=" << rank << " mode=" << mode << std::endl;
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "FAIL " << error.what() << std::endl;
        // Failure may leave device accesses in flight; avoid destructor cleanup/reset.
        std::_Exit(1);
    }
}
