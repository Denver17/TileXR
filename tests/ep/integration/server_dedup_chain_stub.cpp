// 仅由 test_server_dedup_chain.py 链接到真实 consumer；不进入生产安装包。
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
#include "../demo/tilexr_ep_expert_ids.h"
#include "memory_boundary_test.h"
#include "tilexr_profiling_runtime.h"
#include "server_dedup_dispatch_reference.h"

namespace {
int deviceId = 0;
int rankId = -1;
TileXR::CommArgs args;
std::map<void *, void *> allocations;
std::map<std::string, uint64_t> calls;
std::vector<void *> firstBuffers;
std::map<uintptr_t, std::vector<uint8_t>> packets;
std::array<uint8_t, 64> boundaryTail {};
bool boundaryMode = false;
alignas(32) std::array<uint8_t, 32> localPeer {};
TileXR::TileXRProfilingRuntimeConfig profileConfig;
std::vector<uint64_t> profileTimes;
std::vector<uint32_t> profileEvents, profileOccupied, profileDropped;
uint32_t profileStride = 0;
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
    return Call("sync");
}
aclError aclrtMalloc(void **data, size_t bytes, aclrtMemMallocPolicy)
{
    const int result = Call("malloc");
    if (result != 0) { return result; }
    void *raw = std::malloc(bytes + 31);
    if (raw == nullptr) { return 93; }
    *data = reinterpret_cast<void *>((reinterpret_cast<uintptr_t>(raw) + 31) & ~uintptr_t(31));
    allocations[*data] = raw;
    return 0;
}
aclError aclrtFree(void *data) { std::free(allocations.at(data)); allocations.erase(data); return Call("free"); }
aclError aclrtMemset(void *data, size_t size, int32_t value, size_t bytes)
{
    if (bytes > size) return 92;
    std::memset(data, value, bytes);
    return Call("memset");
}
aclError aclrtMemcpy(void *destination, size_t, const void *source, size_t bytes, aclrtMemcpyKind)
{
    if (rankId >= 0 && source == args.peerMems[rankId] + TileXREp::TileXREpServerDedupRoundOffset(args.peerMemBytes)
        && bytes == uint64_t(args.rankSize) * 32U) {
        auto *epochs = static_cast<int64_t *>(destination);
        std::fill_n(epochs, bytes / sizeof(int64_t), int64_t(calls["launch"]));
        if (std::getenv("TEST_ROUND_FAULT") != nullptr) --epochs[3];
        return Call("round-read");
    }
    if (boundaryMode && source == args.peerMems[rankId] + TileXRMemoryBoundaryTest::kOffset - 32 && bytes == 64) {
        std::memcpy(destination, boundaryTail.data(), bytes);
        return Call("boundary-host-read");
    }
    const uintptr_t address = reinterpret_cast<uintptr_t>(source);
    const uintptr_t peer = rankId < 0 ? 0 : reinterpret_cast<uintptr_t>(args.peerMems[rankId]);
    if (peer && address >= peer && address - peer < args.peerMemBytes) {
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
    return Call("comm-init");
}
int TileXRCommInitRankMemoryDomainWithSize(int domain, int dataMiB, int size, int rank, TileXRCommPtr *comm)
{
    if (dataMiB != 4092) return 92;
    const int ret = TileXRCommInitRankMemoryDomain(domain, size, rank, comm);
    if (ret != 0) return ret;
    boundaryMode = true;
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
    return Call("comm-destroy");
}
int TileXRMoeEpDispatchMemoryServerDedup(void *x, int32_t *ids, void *scales, bool *mask,
    int64_t maskType, void *weights, TileXRCommPtr comm, int64_t bs, int64_t h, int64_t topK,
    int64_t experts, int64_t world, int64_t rank, int64_t tpWorld, int64_t tpRank,
    int64_t shard, int64_t shared, int64_t sharedRanks, int64_t quant, int64_t globalBs,
    int64_t countType, void *expandX, void *dynamic, int32_t *assist, int64_t *counts,
    int32_t *sendCounts, int32_t *tpCounts, void *expandScales, uint32_t *assistEx,
    uint32_t *sourceMask, uint32_t *group, float *weightsOut, TileXR::TileXRDataType dtype,
    TileXR::TileXRDataType outDtype, aclrtStream stream)
{
    if (x == nullptr || ids == nullptr || comm != &args || bs <= 0 || h <= 0 || topK <= 0 || topK > 32 ||
        experts != world * 2 || rank != rankId || tpWorld != 1 || tpRank != 0 || shard != 0 ||
        shared != 0 || sharedRanks != 0 || (quant != 0 && quant != 4) || globalBs != world * bs || (countType != 0 && countType != 1) ||
        maskType != 0 || scales != nullptr || mask != nullptr || weights == nullptr || (quant == 4 ? dynamic == nullptr : dynamic != nullptr) ||
        assist != nullptr || tpCounts != nullptr || expandScales != nullptr || expandX == nullptr ||
        counts == nullptr || sendCounts == nullptr || assistEx == nullptr || sourceMask == nullptr ||
        group == nullptr || weightsOut == nullptr || weightsOut == weights || stream != reinterpret_cast<void *>(0x20) || (quant == 0 ? outDtype != TileXR::TILEXR_DATA_TYPE_RESERVED :
            (outDtype != TileXR::TILEXR_DATA_TYPE_FP8E4M3 && outDtype != TileXR::TILEXR_DATA_TYPE_FP8E5M2)) ||
        (dtype != TileXR::TILEXR_DATA_TYPE_FP16 && dtype != TileXR::TILEXR_DATA_TYPE_BFP16)) { return 92; }
    std::vector<void *> current {x, ids, expandX, counts, sendCounts, assistEx, sourceMask, group, weights, weightsOut};
    if (dynamic) current.push_back(dynamic);
    if (calls["launch"] == 0) { firstBuffers = current; }
    if (current != firstBuffers || allocations.size() != current.size() || calls["comm-init"] != 1 ||
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
        const Shape shape {uint32_t(world), uint32_t(experts), uint32_t(bs), uint32_t(h), uint32_t(topK),
            dtype == TileXR::TILEXR_DATA_TYPE_BFP16, quant == 4, outDtype == TileXR::TILEXR_DATA_TYPE_FP8E4M3};
        const auto layout = TileXREp::TileXREpServerDedupDataLayout(shape.bs, shape.h, shape.k, shape.experts, shape.quantized);
        const char *packetFault = std::getenv("TEST_PACKET_FAULT");
        const std::string failure = packetFault == nullptr ? "" : packetFault;
        const uint64_t hiddenStride = uint64_t(shape.h) * (shape.quantized ? 1U : 2U);
        const uint64_t groupStride = (4U + 8U * (shape.k - 1U) + 31U) / 32U * 32U;
        using GroupKey = std::tuple<uint32_t, uint32_t, uint32_t, uint32_t>;
        std::map<GroupKey, uint32_t> groupIndices;
        for (uint32_t relay = 0; relay < shape.ranks; ++relay) {
            const auto relayRows = DispatchRows(shape, relay);
            uint32_t count = 0;
            for (const auto &row : relayRows) if (row.FirstHop()) ++count;
            uint32_t ordinal = 0;
            for (const auto &row : relayRows) if (row.FirstHop()) {
                // Deliberately change the valid, nonmonotonic t assignment each
                // generation; the consumer must validate associations, not order.
                const uint32_t t = (count - 1U - ordinal++ + uint32_t(calls["launch"])) % (8U * shape.bs);
                groupIndices[{row.source, row.token, row.packet.members.front().k, relay}] = t;
            }
        }
        std::fill(sourceMask, sourceMask + bs, 0);
        const auto sourcePackets = SourcePackets(Ids(shape, rank), Weights(shape, rank), rank, world, experts, topK);
        for (const auto &packet : sourcePackets) {
            const auto &first = packet.members.front();
            sourceMask[packet.token] |= uint32_t(1) << first.k;
            weightsOut[packet.token * topK + first.k] = packet.members.size() > 1U ? 1.0f : first.weight;
        }
        const auto outputRows = DispatchRows(shape, rank);
        packets.clear();
        uint32_t previousT = UINT32_MAX;
        for (size_t index = 0; index < outputRows.size(); ++index) {
            const auto &row = outputRows[index];
            const auto payload = Payload(shape, row.source, row.packet, layout);
            auto *hidden = static_cast<uint8_t *>(expandX) + index * hiddenStride;
            std::memcpy(hidden, payload.data(), hiddenStride);
            if (quant == 4) std::memcpy(static_cast<uint8_t *>(dynamic) + index * layout.scaleBytes,
                payload.data() + layout.quantCount, layout.scaleBytes);
            uint32_t t = row.Direct() ? UINT32_MAX : groupIndices.at({row.source, row.token, row.packet.members.front().k, row.relay});
            if (row.FirstHop() && failure == "duplicate-t") {
                if (previousT != UINT32_MAX) t = previousT;
                previousT = t;
            }
            auto metadata = DispatchAssist(row, t);
            if (failure == "assist-source") metadata[0] ^= 1U;
            if (failure == "assist-token") metadata[1] ^= 1U;
            if (failure == "assist-k") metadata[2] ^= 1U;
            if (failure == "assist-weight") metadata[3] ^= 1U;
            if (failure == "assist-role") metadata[4] ^= 1U;
            if (failure == "assist-count") metadata[5] ^= 1U;
            if (failure == "assist-relay" && !row.Direct()) metadata[6] ^= 1U;
            if (failure == "assist-t" && !row.Direct()) metadata[7] = 8U * shape.bs;
            if (failure == "terminal-t" && !row.Direct() && !row.FirstHop()) metadata[7] = (t + 1U) % (8U * shape.bs);
            if (failure != "missing-output") std::memcpy(assistEx + index * 8U, metadata.data(), 32U);
            if (row.FirstHop()) {
                auto *groupRow = reinterpret_cast<uint32_t *>(reinterpret_cast<uint8_t *>(group) + t * groupStride);
                groupRow[0] = uint32_t(index);
                for (uint32_t member = 1; member < row.packet.members.size(); ++member) {
                    const auto &route = row.packet.members[member];
                    groupRow[1U + (member - 1U) * 2U] = t * shape.k + route.k;
                    groupRow[2U + (member - 1U) * 2U] = FloatBits(route.weight);
                }
                if (failure == "group-row") groupRow[0] ^= 1U;
                if (failure == "group-slot") groupRow[1] ^= 1U;
                if (failure == "group-weight") groupRow[2] ^= 1U;
            }
            auto wire = Pack(payload);
            for (uint64_t block = 0; block < layout.blockCount; ++block)
                std::memset(wire.data() + block * 512U + 480U, 0, 32U);
            if (failure == "flag-first") wire[480U] = 1U;
            if (failure == "flag-last") wire[wire.size() - 1U] = 1U;
            packets[reinterpret_cast<uintptr_t>(args.peerMems[rank]) + DispatchSlot(shape, rank, row, layout)] = wire;
        }
        if (!outputRows.empty()) {
            if (failure == "hidden") static_cast<uint8_t *>(expandX)[0] ^= 1U;
            if (failure == "scale" && quant == 4) static_cast<uint8_t *>(dynamic)[0] ^= 1U;
            if (failure == "row-order" && outputRows.size() > 1U)
                std::swap_ranges(assistEx, assistEx + 8U, assistEx + 8U);
        }
        if (failure == "mask") sourceMask[0] ^= 1U;
        if (failure == "input-weight") static_cast<float *>(weights)[0] += 1.0f;
        if (failure == "source-weight") std::fill(weightsOut, weightsOut + bs * topK, 0.0f);
        if (failure == "counter") packets[reinterpret_cast<uintptr_t>(args.peerMems[rank]) +
            TileXREp::TileXREpServerDedupCountLayout(shape.experts).counterOffset] = {1U, 0U, 0U, 0U};
        const char *fault = std::getenv("TEST_COUNT_FAULT");
        if (fault && std::strcmp(fault, "unwritten") == 0) return 0;
        std::fill(sendCounts, sendCounts + experts, 0);
        const int64_t localExperts = experts / world;
        for (int64_t source = 0; source < world; ++source) {
            TileXREpDemo::ExpertIdsConfig config {bs, topK, experts, localExperts, world, source, 2026};
            for (int32_t expert : TileXREpDemo::BuildBalancedShuffleExpertIds(config))
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
