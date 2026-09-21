// 链接真实 consumer 的 Host 生命周期/C3 dry-run/C4/C5 串联测试；不链接生产 Host 库。
#include <cstdlib>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <map>
#include <string>
#include <thread>
#include <tuple>
#include "acl/acl.h"
#include "tilexr_ep.h"
#include "ep_memory_server_dedup_combine_host.h"
#include "ep_memory_server_dedup_combine_config.h"
#include "../demo/tilexr_ep_expert_ids.h"
#include "memory_boundary_test.h"
#include "combine_memory_boundary_test.h"
#include "tilexr_profiling_runtime.h"
#include "server_dedup_dispatch_reference.h"

namespace {
int deviceId = 0;
int rankId = -1;
TileXR::CommArgs args;
std::map<void *, void *> allocations;
std::map<const void *, size_t> allocationBytes;
std::map<std::string, uint64_t> calls;
std::vector<void *> firstBuffers;
std::vector<const void *> firstCombineBuffers;
uint64_t sharedMagic = 0;
uint64_t lastDispatchMagic = 0;
bool IsM3()
{
    const char *checkpoint = std::getenv("TILEXR_VALIDATION_CHECKPOINT");
    return checkpoint && std::strcmp(checkpoint, "ep-sdedup-combine-p2-m3") == 0;
}
bool IsM12()
{
    const char *checkpoint = std::getenv("TILEXR_VALIDATION_CHECKPOINT");
    return IsM3() || (checkpoint && std::strcmp(checkpoint, "ep-sdedup-combine-p2-m12") == 0);
}
bool IsDiagnostic()
{
    const char *value = std::getenv("TILEXR_VALIDATION_COMBINE_OBSERVATION");
    return IsM12() && value && std::strcmp(value, "per-round") == 0;
}
std::map<uintptr_t, std::vector<uint8_t>> m12Memory;
uint8_t *M12Address(uintptr_t address, size_t bytes)
{
    auto found = m12Memory.upper_bound(address);
    if (found == m12Memory.begin()) return nullptr;
    --found;
    if (address - found->first > found->second.size() || bytes > found->second.size() - (address - found->first)) return nullptr;
    return found->second.data() + address - found->first;
}
bool IsLocalPeer(uintptr_t address)
{
    if (rankId < 0) return false;
    const auto base = reinterpret_cast<uintptr_t>(args.peerMems[rankId]);
    return address >= base && address - base < args.peerMemBytes;
}
bool IsMultiBatch()
{
    const char *checkpoint = std::getenv("TILEXR_VALIDATION_CHECKPOINT");
    const char *probe = std::getenv("TILEXR_VALIDATION_BOUNDARY_PROBE");
    return IsM12() || (checkpoint && (std::strcmp(checkpoint, "ep-sdedup-combine-p1-chain-multi") == 0 ||
        std::strcmp(checkpoint, "ep-sdedup-combine-p1-profile") == 0 ||
        (std::strcmp(checkpoint, "ep-sdedup-combine-p1-memory-boundary") == 0 &&
         (!probe || std::strcmp(probe, "0") == 0))));
}
bool BatchPending()
{
    if (IsDiagnostic()) return calls["sync"] < calls["combine-launch"];
    return IsMultiBatch() && sharedMagic != 0 && calls["sync"] == 0;
}
std::map<uintptr_t, std::vector<uint8_t>> packets;
std::array<uint8_t, 64> boundaryTail {};
bool boundaryMode = false;
bool combineBoundaryMode = false;
std::map<uint64_t, uint8_t> combineBoundaryBytes;
alignas(512) std::array<uint8_t, 32> localPeer {};
TileXR::TileXRProfilingRuntimeConfig profileConfig;
std::vector<uint64_t> profileTimes;
std::vector<uint32_t> profileEvents, profileOccupied, profileDropped;
uint32_t profileStride = 0;
TileXR::TileXRProfilingRuntimeConfig combineProfileConfig;
std::vector<uint64_t> combineProfileTimes;
std::vector<uint32_t> combineProfileEvents, combineProfileOccupied, combineProfileDropped;
int Call(const char *name)
{
    const uint64_t count = ++calls[name];
    const char *directory = std::getenv("TEST_TRACE_DIR");
    if (directory != nullptr) {
        const char *rank = std::getenv("TEST_RANK");
        std::ofstream log(std::string(directory) + "/" + (rank == nullptr ? "0" : rank), std::ios::app);
        log << name << '\n';
    }
    const char *pauseStage = std::getenv("TEST_PAUSE_STAGE");
    const char *resumeFile = std::getenv("TEST_RESUME_FILE");
    if (pauseStage != nullptr && resumeFile != nullptr && std::strcmp(pauseStage, name) == 0) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
        while (!std::ifstream(resumeFile).good()) {
            if (std::chrono::steady_clock::now() >= deadline) return 92;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }
    const char *failure = std::getenv("TEST_FAIL_STAGE");
    const char *failCall = std::getenv("TEST_FAIL_CALL");
    return failure != nullptr && std::strcmp(failure, name) == 0 &&
        (failCall == nullptr || count == std::strtoull(failCall, nullptr, 10)) ? 91 : 0;
}


}

extern "C" {
aclError aclInit(const char *) { return Call("init"); }
aclError aclFinalize() { return Call("finalize"); }
aclError aclrtSetDevice(int32_t id) { deviceId = id; return Call("set-device"); }
aclError aclrtResetDevice(int32_t) { return Call("reset-device"); }
aclError aclrtGetDevice(int32_t *id) { *id = deviceId; return Call("get-device"); }
aclError aclrtGetDeviceInfo(uint32_t device, aclrtDevAttr attr, int64_t *value)
{
    if (device != static_cast<uint32_t>(deviceId) || attr != ACL_DEV_ATTR_VECTOR_CORE_NUM) { return 92; }
    const char *configured = std::getenv("TEST_VECTOR_CORES");
    *value = configured == nullptr ? 4 : std::strtoll(configured, nullptr, 10);
    return Call("device-info");
}
const char *aclrtGetSocName() { return std::getenv("TEST_SOC") == nullptr ? "Ascend950" : std::getenv("TEST_SOC"); }
aclError aclrtCreateStream(aclrtStream *stream) { *stream = reinterpret_cast<void *>(0x20); return Call("create-stream"); }
aclError aclrtDestroyStream(aclrtStream) { return Call("destroy-stream"); }
aclError aclrtSynchronizeStream(aclrtStream stream)
{
    if (stream != reinterpret_cast<void *>(0x20)) { return 92; }
    if (IsMultiBatch() && !IsDiagnostic()) {
        const char *mode = std::getenv("TILEXR_VALIDATION_OPERATOR_MODE");
        const bool dispatch = mode == nullptr || std::strcmp(mode, "combine") != 0;
        const bool combine = mode != nullptr && std::strcmp(mode, "dispatch") != 0;
        const char *loops = std::getenv("TILEXR_VALIDATION_LOOPS");
        const char *warmup = std::getenv("TILEXR_VALIDATION_WARMUP");
        const uint64_t total = (loops ? std::strtoull(loops, nullptr, 10) : 1U) +
            (warmup ? std::strtoull(warmup, nullptr, 10) : 0U);
        if (calls["sync"] != 0 || calls["launch"] != (dispatch ? total : 0U) ||
            calls["combine-launch"] != (combine ? total : 0U)) return 92;
    }
    return Call("sync");
}
aclError aclrtMalloc(void **data, size_t bytes, aclrtMemMallocPolicy)
{
    if (BatchPending()) return Call("mid-batch-allocation") == 0 ? 92 : 91;
    const int result = Call("malloc");
    if (result != 0) { return result; }
    void *raw = std::malloc(bytes + 31);
    if (raw == nullptr) { return 93; }
    *data = reinterpret_cast<void *>((reinterpret_cast<uintptr_t>(raw) + 31) & ~uintptr_t(31));
    allocations[*data] = raw;
    allocationBytes[*data] = bytes;
    return 0;
}
aclError aclrtFree(void *data) { std::free(allocations.at(data)); allocations.erase(data); allocationBytes.erase(data); return Call("free"); }
aclError aclrtMemset(void *data, size_t size, int32_t value, size_t bytes)
{
    if (BatchPending()) return Call("mid-batch-memset") == 0 ? 92 : 91;
    if (bytes > size) return 92;
    if (IsM12() && IsLocalPeer(reinterpret_cast<uintptr_t>(data))) {
        m12Memory[reinterpret_cast<uintptr_t>(data)].assign(bytes, static_cast<uint8_t>(value));
        return Call("m12-window-seed");
    }
    std::memset(data, value, bytes);
    return Call("memset");
}
aclError aclrtMemcpy(void *destination, size_t, const void *source, size_t bytes, aclrtMemcpyKind)
{
    if (BatchPending()) return Call("mid-batch-transfer") == 0 ? 92 : 91;
    if (IsM12() && IsLocalPeer(reinterpret_cast<uintptr_t>(destination))) {
        if (auto data = M12Address(reinterpret_cast<uintptr_t>(destination), bytes)) {
            std::memcpy(data, source, bytes);
            return Call("m12-window-seed");
        }
        auto &region = m12Memory[reinterpret_cast<uintptr_t>(destination)];
        region.assign(static_cast<const uint8_t *>(source), static_cast<const uint8_t *>(source) + bytes);
        return Call("m12-window-seed");
    }
    if (IsM12()) {
        if (auto data = M12Address(reinterpret_cast<uintptr_t>(source), bytes)) {
            std::memcpy(destination, data, bytes);
            return Call("m12-window-read");
        }
    }
    if (boundaryMode && source == args.peerMems[rankId] + TileXRMemoryBoundaryTest::kOffset - 32 && bytes == 64) {
        std::memcpy(destination, boundaryTail.data(), bytes);
        return Call("boundary-host-read");
    }
    const uintptr_t address = reinterpret_cast<uintptr_t>(source);
    const uintptr_t peer = rankId < 0 ? 0 : reinterpret_cast<uintptr_t>(args.peerMems[rankId]);
    if (combineBoundaryMode && peer && address >= peer && address - peer < args.peerMemBytes) {
        using namespace TileXRCombineMemoryBoundaryTest;
        const uint64_t offset = address - peer;
        if (!LegalRange(offset, bytes) || bytes != kProbeBytes) return 92;
        bool allowed = false;
        for (uint32_t window = 0; window < kWindowCount; ++window)
            for (uint32_t index = 0; index < kProbesPerWindow; ++index) {
                const uint64_t point = ProbeOffset(window, index);
                allowed = allowed || offset == point || offset == point - kProbeBytes ||
                    (LegalRange(point + kProbeBytes, kProbeBytes) && offset == point + kProbeBytes);
            }
        if (!allowed) return 92;
        const int ret = Call("combine-boundary-host-read");
        if (ret != 0) return ret;
        const char *fault = std::getenv("TEST_COMBINE_BOUNDARY_FAULT");
        if (fault && std::strcmp(fault, "host-no-read") == 0) return 0;
        for (size_t byte = 0; byte < bytes; ++byte)
            static_cast<uint8_t *>(destination)[byte] = combineBoundaryBytes[offset + byte];
        return 0;
    }
    if (peer && address >= peer && address - peer < args.peerMemBytes) {
        if (!boundaryMode) {
            const uint64_t bank = lastDispatchMagic % 2U;
            const uint64_t expectedBank = std::getenv("TEST_WINDOW_FAULT") ? 1U - bank : bank;
            if ((address - peer) / (args.peerMemBytes / 2U) != expectedBank)
                return Call("wrong-bank-read") == 0 ? 92 : 91;
        }
        auto found = packets.upper_bound(address);
        if (found != packets.begin()) --found;
        if (found != packets.end() && address >= found->first && address - found->first <= found->second.size() &&
            bytes <= found->second.size() - (address - found->first))
            std::memcpy(destination, found->second.data() + address - found->first, bytes);
        else std::memset(destination, 0, bytes);
        return Call("packet-read");
    }
    std::memcpy(destination, source, bytes);
    if (source == &args && std::getenv("TEST_DEVICE_MAPPING_MISMATCH") != nullptr) {
        static_cast<TileXR::CommArgs *>(destination)->peerMems[3] += 32;
    }
    return Call("memcpy");
}
int TileXRCommInitRankMemoryDomain(int domain, int size, int rank, TileXRCommPtr *comm)
{
    rankId = rank;
    args = TileXR::CommArgs {};
    args.rank = rank; args.rankSize = size; args.localRank = rank % 8; args.localRankSize = 8;
    args.commDomain = domain;
    args.extraFlag |= TileXR::ExtraFlag::MEMORY_ONLY;
    args.peerMemBytes = uint64_t(TileXR::TILEXR_MEMORY_DOMAIN_BUFFER_SIZE) * 1024 * 1024 + TileXR::TILEXR_FLAG_BUFF_BYTES;
    for (int i = 0; i < size; ++i) { args.peerMems[i] = reinterpret_cast<GM_ADDR>(uintptr_t(0x100000 + i * 32)); }
    args.peerMems[rank] = reinterpret_cast<GM_ADDR>(localPeer.data());
    if (std::getenv("TEST_MISSING_PEER") != nullptr) { args.peerMems[3] = nullptr; }
    if (std::getenv("TEST_SMALL_PEER") != nullptr) { args.peerMemBytes = 64; }
    *comm = &args;
    std::string error;
    if (!TileXR::LoadTileXRProfilingRuntimeConfig(rank, deviceId, domain, &profileConfig, &error)) return 94;
    if (profileConfig.enabled) {
        // Fixture events are small; export them through the real runtime writer
        // using a compact equivalent buffer, rather than reserving device GM.
        if (profileConfig.recordsPerCore != 65536U) return 95;
        profileConfig.latestLaunch = true;
        profileStride = 16U;
        profileTimes.resize(uint64_t(profileStride) * 64U);
        profileEvents.resize(profileTimes.size());
        profileOccupied.resize(64U);
        profileDropped.resize(profileOccupied.size());
    }
    if (!TileXR::LoadTileXRCombineProfilingRuntimeConfig(rank, deviceId, domain, &combineProfileConfig, &error)) return 94;
    if (combineProfileConfig.enabled) {
        if (combineProfileConfig.recordsPerCore != 65536U ||
            (profileConfig.enabled && profileConfig.recordsPath == combineProfileConfig.recordsPath)) return 95;
        combineProfileConfig.latestLaunch = true;
        combineProfileConfig.recordsPerCore = IsM12() ? 32U : 16U;
        combineProfileTimes.resize(combineProfileConfig.recordsPerCore * 64U);
        combineProfileEvents.resize(combineProfileTimes.size());
        combineProfileOccupied.resize(64U);
        combineProfileDropped.resize(64U);
    }
    return Call("comm-init");
}
int TileXRCommInitRankMemoryDomainWithSize(int domain, int dataMiB, int size, int rank, TileXRCommPtr *comm)
{
    const char *checkpoint = std::getenv("TILEXR_VALIDATION_CHECKPOINT");
    boundaryMode = checkpoint && std::strcmp(checkpoint, "ep-sdedup-p1-memory-boundary") == 0;
    const char *probe = std::getenv("TILEXR_VALIDATION_BOUNDARY_PROBE");
    combineBoundaryMode = checkpoint && std::strcmp(checkpoint, "ep-sdedup-combine-p1-memory-boundary") == 0 &&
        probe && std::strcmp(probe, "1") == 0;
    if (dataMiB != (boundaryMode ? 4092 : 8188)) return 92;
    const int ret = TileXRCommInitRankMemoryDomain(domain, size, rank, comm);
    if (ret != 0) return ret;
    args.peerMemBytes = uint64_t(dataMiB) * 1024 * 1024 + TileXR::TILEXR_FLAG_BUFF_BYTES;
    for (int i = 0; i < size; ++i) {
        args.peerMems[i] = reinterpret_cast<GM_ADDR>(UINT64_C(0x1000000000) + uint64_t(i) * (UINT64_C(1) << 33));
    }
    if (std::getenv("TEST_MISSING_PEER") != nullptr) args.peerMems[3] = nullptr;
    if (std::getenv("TEST_SMALL_PEER") != nullptr) args.peerMemBytes -= 32;
    if (std::getenv("TEST_LARGE_PEER") != nullptr) args.peerMemBytes += 32;
    const char *fault = std::getenv("TEST_BOUNDARY_FAULT");
    if (fault != nullptr && std::strcmp(fault, "capacity") == 0) args.peerMemBytes -= 32;
    if (fault != nullptr && std::strcmp(fault, "initial") == 0) boundaryTail[63] = 1;
    const char *combineFault = std::getenv("TEST_COMBINE_BOUNDARY_FAULT");
    if (combineBoundaryMode && combineFault && std::strcmp(combineFault, "initial") == 0)
        combineBoundaryBytes[TileXRCombineMemoryBoundaryTest::ProbeOffset(1, 3)] = 1;
    return Call("comm-init-sized");
}
int TileXRGetCommArgsHost(TileXRCommPtr, TileXR::CommArgs *&host) { host = &args; return Call("host-args"); }
int TileXRGetCommArgsDev(TileXRCommPtr, GM_ADDR &device) { device = reinterpret_cast<GM_ADDR>(&args); return Call("device-args"); }
int TileXRCommDestroy(TileXRCommPtr)
{
    if (profileConfig.enabled) {
        const char *fault = std::getenv("TEST_PROFILE_FAULT");
        if (!fault || std::strcmp(fault, "missing") != 0) {
            std::string error;
            profileConfig.recordsPerCore = profileStride;
            if (fault && std::strcmp(fault, "capture") == 0) ++profileConfig.captureId[0];
            if (!TileXR::WriteTileXRProfilingRecords(profileConfig, rankId, profileOccupied,
                    profileDropped, profileTimes, profileEvents, &error)) return 94;
        }
        const int result = Call("profiling-export");
        if (result != 0) return result;
    }
    if (combineProfileConfig.enabled) {
        const char *fault = std::getenv("TEST_COMBINE_PROFILE_FAULT");
        if (!fault || std::strcmp(fault, "missing") != 0) {
            std::string error;
            if (fault && std::strcmp(fault, "capture") == 0) ++combineProfileConfig.captureId[0];
            if (!TileXR::WriteTileXRProfilingRecords(combineProfileConfig, rankId, combineProfileOccupied,
                    combineProfileDropped, combineProfileTimes, combineProfileEvents, &error)) return 94;
        }
        const int result = Call("combine-profiling-export");
        if (result != 0) return result;
    }
    return Call("comm-destroy");
}
int TileXRMoeEpDispatchMemoryServerDedup(void *x, int32_t *ids, void *scales, bool *mask,
    int64_t maskType, void *weights, TileXRCommPtr comm, int64_t bs, int64_t h, int64_t topK,
    int64_t experts, int64_t world, int64_t rank, int64_t tpWorld, int64_t tpRank,
    int64_t shard, int64_t shared, int64_t sharedRanks, int64_t quant, int64_t globalBs,
    int64_t countType, void *expandX, void *dynamic, int64_t *counts,
    int32_t *sendCounts, int32_t *tpCounts, void *expandScales, uint32_t *tokenType,
    uint32_t *destinationIndex, uint32_t *relayReadIndex, uint32_t *sourceMask, float *weightsOut, TileXR::TileXRDataType dtype,
    TileXR::TileXRDataType outDtype, aclrtStream stream)
{
    if (x == nullptr || ids == nullptr || comm != &args || bs <= 0 || h <= 0 || topK <= 0 || topK > 32 ||
        experts <= 0 || world <= 0 || experts % world != 0 || rank != rankId || tpWorld != 1 || tpRank != 0 || shard != 0 ||
        shared != 0 || sharedRanks != 0 || (quant != 0 && quant != 4) || globalBs != world * bs || (countType != 0 && countType != 1) ||
        maskType != 0 || scales != nullptr || mask != nullptr || weights == nullptr || (quant == 4 ? dynamic == nullptr : dynamic != nullptr) ||
        tpCounts != nullptr || expandScales != nullptr || expandX == nullptr ||
        counts == nullptr || sendCounts == nullptr || tokenType == nullptr || destinationIndex == nullptr || sourceMask == nullptr ||
        relayReadIndex == nullptr || weightsOut == nullptr || weightsOut == weights || stream != reinterpret_cast<void *>(0x20) || (quant == 0 ? outDtype != TileXR::TILEXR_DATA_TYPE_RESERVED :
            (outDtype != TileXR::TILEXR_DATA_TYPE_FP8E4M3 && outDtype != TileXR::TILEXR_DATA_TYPE_FP8E5M2)) ||
        (dtype != TileXR::TILEXR_DATA_TYPE_FP16 && dtype != TileXR::TILEXR_DATA_TYPE_BFP16)) { return 92; }
    std::vector<void *> current {x, ids, expandX, counts, sendCounts, tokenType, destinationIndex, relayReadIndex, sourceMask, weights, weightsOut};
    if (dynamic) current.push_back(dynamic);
    if (calls["launch"] == 0) { firstBuffers = current; }
    const char *mode = std::getenv("TILEXR_VALIDATION_OPERATOR_MODE");
    const bool both = mode && std::strcmp(mode, "both") == 0;
    if (IsMultiBatch() && both && calls["launch"] != calls["combine-launch"]) return 92;
    const size_t extraBuffers = both ? (quant == 4 ? 2U : 1U) : 0U;
    if (current != firstBuffers || allocations.size() != current.size() + extraBuffers || calls["comm-init"] != 1 ||
        calls["create-stream"] != 1 || calls["sync"] != 0) { return 92; }
    for (auto buffer : current) { if (allocations.count(buffer) != 1) { return 92; } }
    const auto *routeWeights = static_cast<const float *>(weights);
    for (int64_t token = 0; token < bs; ++token) {
        double sum = 0.0;
        for (int64_t k = 0; k < topK; ++k) {
            const float weight = routeWeights[token * topK + k];
            if (!std::isfinite(weight) || weight < 0.0f || weight > 1.0f) { return 92; }
            sum += weight;
        }
        if (std::abs(sum - 1.0) > 1e-6) { return 92; }
    }
    const int result = Call("launch");
    if (result == 0) {
        lastDispatchMagic = ++sharedMagic;
        if (profileConfig.enabled) {
            const uint64_t launch = calls["launch"] - 1U;
            const uint64_t origin = (launch + 1U) * 100000U;
            const char *fault = std::getenv("TEST_PROFILE_FAULT");
            for (uint32_t core = 0; core < 64U; ++core) {
                const size_t base = uint64_t(core) * profileStride;
                const uint32_t ids[] = {0x53000001U, 0x53000002U, 0x53000204U,
                    0x53000206U, 0x53000207U, 0x53000205U, 0x53000300U, 0x53000009U};
                // The initial capture is deliberately longer. Its stale tail
                // remains in GM; the real standard exporter must ignore it.
                const uint32_t count = launch == 0U ? 16U : 8U;
                for (uint32_t i = 0; i < count; ++i) {
                    profileTimes[base + i] = origin + core + i * 100U;
                    profileEvents[base + i] = i + 1U == count ? ids[7U] : ids[std::min(i, 6U)];
                }
                profileOccupied[core] = count;
                profileDropped[core] = 0U;
                if (fault && core == 63U && (std::strcmp(fault, "overflow") == 0 ||
                        (std::strcmp(fault, "previous-overflow") == 0 && launch == 0U))) {
                    profileDropped[core] = 2U;
                    profileEvents[base + count - 1U] = 0U;
                }
            }
        }
        using namespace ServerDedupReference;
        Shape shape {uint32_t(world), uint32_t(experts), uint32_t(bs), uint32_t(h), uint32_t(topK),
            dtype == TileXR::TILEXR_DATA_TYPE_BFP16, quant == 4, outDtype == TileXR::TILEXR_DATA_TYPE_FP8E4M3};
        if (const char *mode = std::getenv("TILEXR_VALIDATION_ROUTE_MODE"))
            shape.routeMode = TileXREpDemo::ParseRouteMode(mode);
        if (const char *seed = std::getenv("TILEXR_VALIDATION_SEED"))
            shape.seed = static_cast<uint32_t>(std::strtoull(seed, nullptr, 10));
        const auto layout = TileXREp::TileXREpServerDedupDataLayout(shape.bs, shape.h, shape.k, shape.experts, shape.quantized);
        const char *packetFault = std::getenv("TEST_PACKET_FAULT");
        const std::string failure = packetFault == nullptr ? "" : packetFault;
        const uint64_t hiddenStride = uint64_t(shape.h) * (shape.quantized ? 1U : 2U);
        std::fill(sourceMask, sourceMask + bs, 0);
        const auto sourcePackets = SourcePackets(Ids(shape, rank), Weights(shape, rank), rank, world, experts, topK);
        for (const auto &packet : sourcePackets) {
            const auto &first = packet.members.front();
            sourceMask[packet.token] |= uint32_t(1) << first.k;
            weightsOut[packet.token * topK + first.k] = packet.members.size() > 1U ? 1.0f : first.weight;
        }
        const auto outputRows = DispatchRows(shape, rank);
        packets.clear();
        for (size_t index = 0; index < outputRows.size(); ++index) {
            const auto &row = outputRows[index];
            const auto payload = Payload(shape, row.source, row.packet, layout);
            auto *hidden = static_cast<uint8_t *>(expandX) + index * hiddenStride;
            std::memcpy(hidden, payload.data(), hiddenStride);
            if (quant == 4) std::memcpy(static_cast<uint8_t *>(dynamic) + index * layout.scaleBytes,
                payload.data() + layout.quantCount, layout.scaleBytes);
            uint32_t type = TokenType(row);
            auto destination = DestinationIndex(shape, row);
            if (failure == "destination-source") destination[0] ^= 1U;
            if (failure == "destination-token") destination[1] ^= 1U;
            if (failure == "destination-k") destination[2] ^= 1U;
            if (failure == "token-type") type = type % 3U + 1U;
            if (failure == "terminal-slot" && type == 3U) destination[0] ^= 1U;
            if (failure == "terminal-sentinel" && type == 3U) destination[1] = 0U;
            if (failure != "missing-output") {
                tokenType[index] = type;
                std::copy(destination.begin(), destination.end(), destinationIndex + index * 3U);
            }
            if (row.FirstHop()) {
                auto words = RelayReadIndex(row);
                if (failure == "relay-count") words[0] += 1U;
                if (failure == "relay-self-weight") words[1] ^= 1U;
                if (failure == "relay-expert") words[2] ^= 1U;
                if (failure == "relay-q") words[2] ^= 1U << 16U;
                if (failure == "relay-weight") words[3] ^= 1U;
                if (failure == "relay-missing") --words[0];
                if (failure == "relay-duplicate" && words.size() >= 6U) {
                    words[4] = words[2]; words[5] = words[3];
                }
                std::copy(words.begin(), words.end(), relayReadIndex + index * shape.k * 2U);
            }
            auto wire = Pack(payload);
            for (uint64_t block = 0; block < layout.blockCount; ++block)
                std::memset(wire.data() + block * 512U + 480U, 0, 32U);
            if (failure == "flag-first") wire[480U] = 1U;
            if (failure == "flag-last") wire[wire.size() - 1U] = 1U;
            packets[reinterpret_cast<uintptr_t>(args.peerMems[rank]) +
                TileXREp::TileXREpServerDedupWindowOffset(int64_t(lastDispatchMagic), args.peerMemBytes) + DispatchSlot(shape, rank, row, layout)] = wire;
        }
        if (!outputRows.empty()) {
            if (failure == "hidden") static_cast<uint8_t *>(expandX)[0] ^= 1U;
            if (failure == "scale" && quant == 4) static_cast<uint8_t *>(dynamic)[0] ^= 1U;
            if (failure == "row-order" && outputRows.size() > 1U)
                std::swap_ranges(destinationIndex, destinationIndex + 3U, destinationIndex + 3U);
        }
        if (failure == "mask") sourceMask[0] ^= 1U;
        if (failure == "input-weight") static_cast<float *>(weights)[0] += 1.0f;
        if (failure == "source-weight") std::fill(weightsOut, weightsOut + bs * topK, 0.0f);
        const char *fault = std::getenv("TEST_COUNT_FAULT");
        if (fault && std::strcmp(fault, "unwritten") == 0) return 0;
        std::fill(sendCounts, sendCounts + experts, 0);
        const int64_t localExperts = experts / world;
        for (int64_t source = 0; source < world; ++source) {
            for (int32_t expert : Ids(shape, source))
                if (expert / localExperts == rank) ++sendCounts[(expert % localExperts) * world + source];
        }
        int32_t sum = 0, last = 0;
        for (int64_t i = 0; i < experts; ++i) {
            sum += sendCounts[i]; sendCounts[i] = sum;
            if ((i + 1) % world == 0) { counts[i / world] = countType ? sum - last : sum; last = sum; }
        }
        if (fault && std::strcmp(fault, "prefix") == 0) ++sendCounts[0];
        if (fault && std::strcmp(fault, "expert") == 0) ++counts[0];
    }
    return result;
}

int TileXRMoeEpCombineMemoryServerDedup(void *expertOut, int32_t *assist, int32_t *sendCounts,
    float *weights, bool *mask, int64_t maskType, void *sharedX, TileXRCommPtr comm,
    int64_t bs, int64_t h, int64_t topK, int64_t experts, int64_t world, int64_t rank,
    int64_t tpWorld, int64_t tpRank, int64_t shard, int64_t shared, int64_t sharedRanks,
    int64_t quant, int64_t globalBs, void *yOut, TileXR::TileXRDataType dtype, aclrtStream stream,
    const uint32_t *tokenType, const void *relay, const uint32_t *sourceMask)
{
    const char *mode = std::getenv("TILEXR_VALIDATION_OPERATOR_MODE");
    const bool both = mode && std::strcmp(mode, "both") == 0;
    if ((!both && (!mode || std::strcmp(mode, "combine") != 0)) || comm != &args || rank != rankId ||
        stream != reinterpret_cast<void *>(0x20) || tpWorld != 1 || tpRank != 0 || shard != 0 ||
        shared != 0 || sharedRanks != 0 || quant != 0 || globalBs != world * bs ||
        mask || maskType != 0 || sharedX || (dtype != TileXR::TILEXR_DATA_TYPE_BFP16 &&
        dtype != TileXR::TILEXR_DATA_TYPE_FP16) || calls["comm-init"] != 1 || calls["create-stream"] != 1) return 92;
    if (IsMultiBatch()) {
        if (calls["launch"] != (both ? calls["combine-launch"] + 1U : 0U) || (!IsDiagnostic() && calls["sync"] != 0)) return 92;
    } else if (calls["launch"] != (both ? 1U : 0U) || calls["sync"] != (both ? 1U : 0U) ||
               calls["combine-launch"] != 0) return 92;
    const char *configuredDtype = std::getenv("TILEXR_VALIDATION_DTYPE");
    if (!configuredDtype || (dtype == TileXR::TILEXR_DATA_TYPE_BFP16 ?
            std::strcmp(configuredDtype, "bf16") != 0 : std::strcmp(configuredDtype, "fp16") != 0)) return 92;
    TileXREp::EpMemoryServerDedupCombineLayout layout;
    if (TileXREp::TileXREpBuildMemoryServerDedupCombineLayout(world, bs, h, topK, experts, &layout) != 0) return 92;
    const std::pair<const void *, uint64_t> buffers[] = {
        {expertOut, layout.expertOutBytes}, {assist, layout.assistInfoBytes},
        {sendCounts, layout.sendCountsBytes}, {weights, layout.expertScalesBytes},
        {tokenType, layout.tokenTypeBytes}, {relay, layout.relayReadIndexBytes},
        {sourceMask, layout.sourceMaskBytes}, {yOut, layout.yOutBytes}};
    std::vector<const void *> current;
    for (const auto &buffer : buffers) current.push_back(buffer.first);
    if (calls["combine-launch"] == 0) firstCombineBuffers = current;
    if (current != firstCombineBuffers) return 92;
    for (const auto &buffer : buffers) {
        const auto found = allocationBytes.find(buffer.first);
        if (found == allocationBytes.end() || found->second < buffer.second) return 92;
    }
    for (size_t i = 0; i < sizeof(buffers) / sizeof(buffers[0]); ++i)
        for (size_t j = i + 1; j < sizeof(buffers) / sizeof(buffers[0]); ++j)
            if (buffers[i].first == buffers[j].first) return 92;
    int32_t previous = 0;
    for (int64_t index = 0; index < experts; ++index) {
        if (sendCounts[index] < previous || uint64_t(sendCounts[index]) > layout.capacityRows) return 92;
        previous = sendCounts[index];
    }
    if (both) {
        const bool dispatchQuantized = firstBuffers.size() == 12U;
        if (assist != firstBuffers[6] || sendCounts != firstBuffers[4] || weights != firstBuffers[10] ||
            tokenType != firstBuffers[5] || relay != firstBuffers[7] || sourceMask != firstBuffers[8] ||
            (dispatchQuantized ? expertOut == firstBuffers[2] : expertOut != firstBuffers[2])) return 92;
        for (const auto *data : firstBuffers) {
            if (allocationBytes.count(data) != 1 || yOut == data ||
                (dispatchQuantized && expertOut == data)) return 92;
        }
        if (allocations.size() != firstBuffers.size() + (dispatchQuantized ? 2U : 1U)) return 92;
    } else if (allocations.size() != 8U) return 92;
    if (!IsM12() || !std::getenv("TILEXR_VALIDATION_COMBINE_FIXTURE") ||
        std::strcmp(std::getenv("TILEXR_VALIDATION_COMBINE_FIXTURE"), "balanced") == 0) {
        using namespace ServerDedupReference;
        Shape shape {uint32_t(world), uint32_t(experts), uint32_t(bs), uint32_t(h), uint32_t(topK),
            dtype == TileXR::TILEXR_DATA_TYPE_BFP16, false, true};
        if (const char *route = std::getenv("TILEXR_VALIDATION_ROUTE_MODE"))
            shape.routeMode = TileXREpDemo::ParseRouteMode(route);
        if (!both) shape.routeMode = TileXREpDemo::RouteMode::Balance;
        if (const char *seed = std::getenv("TILEXR_VALIDATION_SEED"))
            shape.seed = static_cast<uint32_t>(std::strtoull(seed, nullptr, 10));
        std::vector<int32_t> counts(experts, 0);
        const int64_t localExperts = experts / world;
        for (int64_t source = 0; source < world; ++source)
            for (int32_t expert : Ids(shape, source))
                if (expert / localExperts == rank) ++counts[(expert % localExperts) * world + source];
        int32_t prefix = 0;
        for (int64_t index = 0; index < experts; ++index) {
            prefix += counts[index];
            if (sendCounts[index] != prefix) return 92;
        }
    }
    const int result = Call("combine-launch");
    if (result != 0) return result;
    ++sharedMagic;
    if (IsM12()) {
        const uintptr_t base = reinterpret_cast<uintptr_t>(args.peerMems[rankId]) +
            (sharedMagic % 2U) * (args.peerMemBytes / 2U);
        const char *fault = std::getenv("TEST_M12_FAULT");
        const auto isFault = [&](const char *name) { return fault && std::strcmp(fault, name) == 0; };
        const uint64_t resultBytes = bs * topK * ((h * 2U + 479U) / 480U * 512U);
        const uint64_t stride = (h * 2U + 511U) / 512U * 512U;
        const uintptr_t stage = base + TILEXR_EP_SERVER_DEDUP_SHARED_DATA_OFFSET + resultBytes;
        uint32_t packed = 0;
        for (int32_t row = 0; row < sendCounts[experts - 1]; ++row) {
            if (tokenType[row] != 3U) continue;
            const uint32_t slot = assist[uint64_t(row) * 3U];
            auto target = M12Address(stage + uint64_t(slot) * stride, stride);
            if (!target) return 92;
            if (!isFault("missing-copy")) {
                const uint32_t sourceRow = isFault("packed-index") ? packed : static_cast<uint32_t>(row);
                std::memcpy(target, static_cast<uint8_t *>(expertOut) + uint64_t(sourceRow) * h * 2U, h * 2U);
            }
            if (isFault("stage-tail")) target[h * 2U - 1U] ^= 1U;
            if (isFault("padding") && stride > uint64_t(h) * 2U) target[h * 2U] = 0;
            ++packed;
        }
        if (IsM3()) {
            // Host transport substitute reconstructs incoming peer sends. This
            // does not execute or prove the device packing/communication path.
            using namespace ServerDedupReference;
            Shape shape {uint32_t(world), uint32_t(experts), uint32_t(bs), uint32_t(h), uint32_t(topK),
                dtype == TileXR::TILEXR_DATA_TYPE_BFP16, false, true};
            if (const char *route = std::getenv("TILEXR_VALIDATION_ROUTE_MODE"))
                shape.routeMode = TileXREpDemo::ParseRouteMode(route);
            if (!both) shape.routeMode = TileXREpDemo::RouteMode::Balance;
            if (const char *seed = std::getenv("TILEXR_VALIDATION_SEED"))
                shape.seed = static_cast<uint32_t>(std::strtoull(seed, nullptr, 10));
            const bool generated = !both || firstBuffers.size() == 12U;
            const uint64_t packetBytes = ((h * 2U + 479U) / 480U) * 512U;
            const char *m3Fault = std::getenv("TEST_M3_FAULT");
            const auto faultIs = [&](const char *name) { return m3Fault && std::strcmp(m3Fault, name) == 0; };
            bool corrupted = false;
            for (uint32_t producer = 0; producer < uint32_t(world); ++producer) {
                const auto rows = DispatchRows(shape, producer);
                for (size_t rowIndex = 0; rowIndex < rows.size(); ++rowIndex) {
                    const auto &row = rows[rowIndex];
                    if (!row.Direct() || row.source != uint32_t(rank)) continue;
                    const uint64_t offset = (row.token * topK + row.k) * packetBytes;
                    auto target = M12Address(base + TILEXR_EP_SERVER_DEDUP_SHARED_DATA_OFFSET + offset, packetBytes);
                    if (!target) return 92;
                    if (faultIs("missing-packet")) continue;
                    std::vector<uint8_t> payload(packetBytes / 512U * 480U, 0xabU);
                    for (uint32_t col = 0; col < uint32_t(h); ++col) {
                        const uint16_t one = shape.bf16 ? 0x3f80U : 0x3c00U;
                        const uint16_t bits = generated ? static_cast<uint16_t>(one +
                            (rowIndex * 17U + col * 3U + producer * 11U) % 113U) :
                            HiddenBits(shape, row.source, row.token, col);
                        std::memcpy(payload.data() + col * 2U, &bits, 2U);
                    }
                    const auto packet = Pack(payload);
                    std::memcpy(target, packet.data(), packet.size());
                    if (!corrupted) {
                        if (faultIs("payload")) target[0] ^= 1U;
                        if (faultIs("tail")) target[(h * 2U - 1U) / 480U * 512U + (h * 2U - 1U) % 480U] ^= 1U;
                        if (faultIs("flag")) target[packetBytes - 32U] ^= 1U;
                        corrupted = true;
                    }
                }
            }
            if (faultIs("unwritten")) {
                for (uint64_t offset = 0; offset < resultBytes; offset += packetBytes) {
                    auto target = M12Address(base + TILEXR_EP_SERVER_DEDUP_SHARED_DATA_OFFSET + offset, packetBytes);
                    if (target[480U] == 0 && target[481U] == 0 && target[482U] == 0 && target[483U] == 0) {
                        target[0] ^= 1U;
                        break;
                    }
                }
            }
        }
        auto control = M12Address(base + TILEXR_EP_SERVER_DEDUP_COMBINE_CONTROL_OFFSET,
            TILEXR_EP_SERVER_DEDUP_COMBINE_CONTROL_BYTES);
        if (!control) return 92;
        std::memset(control, 0, TILEXR_EP_SERVER_DEDUP_COMBINE_PREPARE_DONE_BYTES);
        std::memset(control + TILEXR_EP_SERVER_DEDUP_COMBINE_SERVER_READY_OFFSET, 0,
            TileXREp::kEpServerDedupCombineRecvCoreNum * 512U);
        if (isFault("prepare")) control[0] = 1;
        if (isFault("ready")) control[TILEXR_EP_SERVER_DEDUP_COMBINE_SERVER_READY_OFFSET] = 1;
        if (isFault("neighbor")) control[TILEXR_EP_SERVER_DEDUP_COMBINE_SERVER_READY_OFFSET +
            TileXREp::kEpServerDedupCombineRecvCoreNum * 512U] = 0;
        if (isFault("reserved")) control[TILEXR_EP_SERVER_DEDUP_COMBINE_RESERVED_OFFSET] = 0;
        if (isFault("result")) *M12Address(base + TILEXR_EP_SERVER_DEDUP_SHARED_DATA_OFFSET, 1) = 0;
        if (isFault("inactive-window")) {
            const uintptr_t other = reinterpret_cast<uintptr_t>(args.peerMems[rankId]) +
                (1U - sharedMagic % 2U) * (args.peerMemBytes / 2U);
            *M12Address(other + TILEXR_EP_SERVER_DEDUP_COMBINE_CONTROL_OFFSET +
                TILEXR_EP_SERVER_DEDUP_COMBINE_RESERVED_OFFSET, 1) = 0;
        }
    }
    if (combineProfileConfig.enabled) {
        const uint64_t origin = calls["combine-launch"] * 100000U;
        const char *profileFault = std::getenv("TEST_COMBINE_PROFILE_FAULT");
        for (uint32_t core = 0; core < 64U; ++core) {
            const uint32_t base = core * combineProfileConfig.recordsPerCore;
            combineProfileTimes[base] = origin + core;
            combineProfileTimes[base + 1U] = origin + core + 50000U;
            combineProfileEvents[base] = 0x3001U;
            combineProfileEvents[base + 1U] = 0x3002U;
            combineProfileOccupied[core] = 8U;
            combineProfileDropped[core] = 0;
            if (IsM12()) {
                std::vector<uint32_t> events {0x3001,0x3003,0x3004,0x3005,0x3006,0x3007,0x3008,0x3009,0x300a};
                if (core >= 56U) events.insert(events.end(), {0x300b,0x300c,0x300d,0x300e});
                if (core >= TileXREp::kEpServerDedupCombineSendCoreNum)
                    events.insert(events.end(), {0x300f,0x3010,0x3011,0x3012});
                events.insert(events.end(), {0x3013,0x3014,0x3015,0x3016,0x3002});
                for (size_t index = 0; index < events.size(); ++index) {
                    combineProfileEvents[base + index] = events[index];
                    combineProfileTimes[base + index] = origin + core + index * 100U;
                }
                if (IsM3()) {
                    events = {0x3001, 0x3003, 0x3004, 0x3017, 0x3018};
                    if (core < TileXREp::kEpServerDedupCombineSendCoreNum)
                        events.insert(events.end(), {0x301b, 0x301c});
                    else events.insert(events.end(), {0x3019, 0x301a});
                    events.insert(events.end(), {0x3013, 0x3014, 0x3002});
                    for (size_t index = 0; index < events.size(); ++index) {
                        combineProfileEvents[base + index] = events[index];
                        combineProfileTimes[base + index] = origin + core + index * 100U;
                    }
                }
                // The real DirectProfilingWriter publishes an 8-record-aligned
                // occupied span with a zero tail, not its logical event count.
                const uint32_t occupied = static_cast<uint32_t>((events.size() + 7U) / 8U * 8U);
                for (uint32_t index = events.size(); index < occupied; ++index) {
                    combineProfileTimes[base + index] = 0U;
                    combineProfileEvents[base + index] = 0U;
                }
                combineProfileOccupied[core] = occupied;
            }
            if (profileFault && std::strcmp(profileFault, "overflow") == 0) combineProfileDropped[core] = 1U;
            if (profileFault && std::strcmp(profileFault, "end") == 0) combineProfileEvents[base + 1U] = 0;
        }
    }
    const char *fault = std::getenv("TEST_COMBINE_FAULT");
    const char *faultCall = std::getenv("TEST_COMBINE_FAULT_CALL");
    if (fault && (!faultCall || calls["combine-launch"] == std::strtoull(faultCall, nullptr, 10))) {
        const std::pair<const char *, const void *> targets[] = {
            {"y_out", yOut}, {"expert_out", expertOut}, {"send_counts", sendCounts}, {"assist", assist},
            {"token_type", tokenType}, {"relay", relay}, {"source_mask", sourceMask}, {"weights", weights}};
        for (const auto &target : targets) {
            const bool tail = std::string(fault) == std::string(target.first) + "_tail";
            if (std::strcmp(fault, target.first) == 0 || tail) {
                const size_t offset = tail ? allocationBytes.at(target.second) - 1U : 0U;
                static_cast<uint8_t *>(const_cast<void *>(target.second))[offset] ^= 1U;
            }
        }
    }
    return 0;
}
}

int TileXRMemoryBoundaryTest::Launch(void *commArgs, void *result, aclrtStream stream)
{
    if (commArgs != &args || stream != reinterpret_cast<void *>(0x20) || !boundaryMode ||
        args.peerMemBytes != kPeerBytes) return 92;
    const int ret = Call("boundary-launch");
    if (ret != 0) return ret;
    const char *configured = std::getenv("TEST_BOUNDARY_FAULT");
    const std::string fault = configured == nullptr ? "" : configured;
    if (fault == "no-launch") return 0;
    auto *output = static_cast<uint8_t *>(result);
    std::memcpy(output, boundaryTail.data() + 32, 32);
    if (fault != "no-write") std::memset(boundaryTail.data() + 32, 0xFF, 32);
    if (fault != "no-read") std::memcpy(output + 32, boundaryTail.data() + 32, 32);
    if (fault == "guard") boundaryTail[31] = 0xFF;
    return 0;
}

int TileXRCombineMemoryBoundaryTest::Launch(void *commArgs, void *result,
    uint32_t window, uint32_t index, aclrtStream stream)
{
    if (!combineBoundaryMode || commArgs != &args || stream != reinterpret_cast<void *>(0x20) ||
        !ValidProbe(window, index) || args.peerMemBytes != kPeerBytes ||
        allocations.size() != 1 || allocationBytes.count(result) != 1 || allocationBytes.at(result) != kResultBytes ||
        calls["launch"] || calls["combine-launch"] || sharedMagic != 0 ||
        calls["sync"] != window * kProbesPerWindow + index) return 92;
    const int ret = Call("combine-boundary-launch");
    if (ret != 0) return ret;
    const char *configured = std::getenv("TEST_COMBINE_BOUNDARY_FAULT");
    const std::string fault = configured ? configured : "";
    if (fault == "no-launch") return 0;
    const uint64_t offset = ProbeOffset(window, index);
    auto *output = static_cast<uint8_t *>(result);
    if (fault != "no-initial-read")
        for (uint32_t byte = 0; byte < kProbeBytes; ++byte) output[byte] = combineBoundaryBytes[offset + byte];
    const uint32_t pattern = ProbePattern(window, index);
    std::array<uint8_t, kProbeBytes> value {};
    for (uint32_t word = 0; word < kProbeBytes / sizeof(pattern); ++word)
        std::memcpy(value.data() + word * sizeof(pattern), &pattern, sizeof(pattern));
    const auto write = [&](uint64_t destination) {
        for (uint32_t byte = 0; byte < kProbeBytes; ++byte) combineBoundaryBytes[destination + byte] = value[byte];
    };
    if (fault != "no-write") write(fault == "wrong-window" ? ProbeOffset(window ^ 1U, index) : offset);
    if (fault != "no-readback")
        for (uint32_t byte = 0; byte < kProbeBytes; ++byte) output[kProbeBytes + byte] = combineBoundaryBytes[offset + byte];
    if (fault == "guard-before") combineBoundaryBytes[offset - 1U] = 1;
    if (fault == "guard-after" && LegalRange(offset + kProbeBytes, kProbeBytes))
        combineBoundaryBytes[offset + kProbeBytes] = 1;
    if (fault == "alias" && window == 1U) write(ProbeOffset(0, index));
    // A late write into an already-checked guard must be caught by the final sweep.
    if (fault == "old-guard" && window == 1U && index == 3U)
        combineBoundaryBytes[ProbeOffset(0, 0) - 1U] = 1;
    return 0;
}
