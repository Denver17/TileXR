// 仅由 test_server_dedup_chain.py 链接到真实 consumer；不进入生产安装包。
#include <cstdlib>
#include <array>
#include <cmath>
#include <cstring>
#include <fstream>
#include <map>
#include <string>
#include "acl/acl.h"
#include "tilexr_ep.h"
#include "../demo/tilexr_ep_expert_ids.h"
#include "memory_boundary_test.h"
#include "ep_memory_server_dedup_profile.h"
#include "server_dedup_source_send_fixture.h"

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
std::array<TileXREp::EpServerDedupProfileRecord, TileXREp::kEpServerDedupCoreNum * 2> countProfile {};
std::vector<uint8_t> sendProfile;
uint64_t sendProfileOffset = 0;
bool profileReady = false;
int Call(const char *name)
{
    const uint64_t count = ++calls[name];
    const char *directory = std::getenv("TEST_TRACE_DIR");
    if (directory != nullptr) {
        const char *rank = std::getenv("TEST_RANK");
        std::ofstream log(std::string(directory) + "/" + (rank == nullptr ? "0" : rank), std::ios::app);
        log << name << '\n';
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
    if (profileReady && source == args.peerMems[rankId] && bytes == sizeof(countProfile)) {
        std::memcpy(destination, countProfile.data(), bytes);
        return Call("breakdown-read");
    }
    if (profileReady && source == args.peerMems[rankId] + sendProfileOffset && bytes == sendProfile.size()) {
        std::memcpy(destination, sendProfile.data(), bytes);
        return Call("send-breakdown-read");
    }
    if (boundaryMode && source == args.peerMems[rankId] + TileXRMemoryBoundaryTest::kOffset - 32 && bytes == 64) {
        std::memcpy(destination, boundaryTail.data(), bytes);
        return Call("boundary-host-read");
    }
    const uintptr_t address = reinterpret_cast<uintptr_t>(source);
    const uintptr_t peer = rankId < 0 ? 0 : reinterpret_cast<uintptr_t>(args.peerMems[rankId]);
    if (peer && address >= peer && address - peer < args.peerMemBytes) {
        const auto found = packets.find(address);
        if (found != packets.end() && bytes == found->second.size())
            std::memcpy(destination, found->second.data(), bytes);
        else {
            const auto flags = packets.find(address - 480U);
            if (bytes == 32U && flags != packets.end()) std::memcpy(destination, flags->second.data() + 480U, bytes);
            else std::memset(destination, 0, bytes);
        }
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
int TileXRCommDestroy(TileXRCommPtr) { return Call("comm-destroy"); }
int TileXRMoeEpDispatchMemoryServerDedup(void *x, int32_t *ids, void *scales, bool *mask,
    int64_t maskType, void *weights, TileXRCommPtr comm, int64_t bs, int64_t h, int64_t topK,
    int64_t experts, int64_t world, int64_t rank, int64_t tpWorld, int64_t tpRank,
    int64_t shard, int64_t shared, int64_t sharedRanks, int64_t quant, int64_t globalBs,
    int64_t countType, void *expandX, void *dynamic, int32_t *assist, int64_t *counts,
    int32_t *sendCounts, int32_t *tpCounts, void *expandScales, uint32_t *assistEx,
    uint32_t *sourceMask, uint32_t *group, float *weightsOut, TileXR::TileXRDataType dtype,
    TileXR::TileXRDataType outDtype, aclrtStream stream)
{
    if (x == nullptr || ids == nullptr || comm != &args || bs != 2 || h != 16 || topK != 3 ||
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
        const char *mode = std::getenv("TILEXR_VALIDATION_RUN_MODE");
        if (mode && std::strcmp(mode, "breakdown") == 0) {
            using namespace TileXREp;
            profileReady = true;
            for (uint32_t core = 0; core < kEpServerDedupCoreNum * 2U; ++core) {
                auto &p = countProfile[core];
                p = EpServerDedupProfileRecord {};
                p.marker = kEpServerDedupProfileMarker; p.version = kEpServerDedupProfileVersion;
                p.recordBytes = sizeof(p); p.rank = uint32_t(rank); p.core = core / 2U; p.segment = core % 2U;
                p.blockDim = kEpServerDedupCoreNum; p.generation = calls["launch"];
                for (uint32_t i = 0; i < 4U; ++i)
                    p.time[i] = calls["launch"] * 1000 + core / 2U + (p.segment * 4U + i) * 10;
            }
            const char *fault = std::getenv("TEST_PROFILE_FAULT");
            const auto detailLayout = TileXREpServerDedupSendProfileLayout(args.peerMemBytes, uint32_t(bs), uint32_t(topK));
            sendProfile.assign(detailLayout.bytes, 0);
            sendProfileOffset = detailLayout.offset;
            const ServerDedupReference::Shape profileShape {uint32_t(world), uint32_t(experts), uint32_t(bs), uint32_t(h),
                uint32_t(topK), dtype == TileXR::TILEXR_DATA_TYPE_BFP16, quant == 4, outDtype == TileXR::TILEXR_DATA_TYPE_FP8E4M3};
            const auto sent = ServerDedupReference::SourcePackets(ServerDedupReference::Ids(profileShape, rank),
                ServerDedupReference::Weights(profileShape, rank), rank, world, experts, topK);
            for (uint32_t core = 0; core < kEpServerDedupSendCoreNum; ++core) {
                EpServerDedupSendProfileHeader header {kEpServerDedupProfileMarker, kEpServerDedupProfileVersion,
                    sizeof(EpServerDedupSendProfilePacket), uint32_t(rank), core, calls["launch"], 0, 0};
                const uint32_t count = bs / 48U + (core < bs % 48U);
                const uint32_t start = core * (bs / 48U) + std::min(core, uint32_t(bs % 48U));
                std::vector<EpServerDedupSendProfilePacket> issued;
                for (bool firstHop : {true, false}) for (const auto &packet : sent) {
                    if (packet.token < start || packet.token >= start + count || (packet.members.size() > 1U) != firstHop) continue;
                    issued.push_back({countProfile[core * 2U].time[2] + 1U, packet.token, packet.members[0].k});
                    if (firstHop) ++header.firstHopCount;
                }
                header.packetCount = uint32_t(issued.size());
                if (core == 0U && fault) {
                    if (std::strcmp(fault, "issue-header") == 0) --header.generation;
                    if (std::strcmp(fault, "issue-count") == 0) header.packetCount = detailLayout.capacity + 1U;
                    if (std::strcmp(fault, "issue-time") == 0 && !issued.empty()) issued[0].issue = 1U;
                }
                auto *base = sendProfile.data() + core * detailLayout.coreStride;
                std::memcpy(base, &header, sizeof(header));
                if (!issued.empty()) std::memcpy(base + sizeof(header), issued.data(), issued.size() * sizeof(issued[0]));
            }
            if (fault) {
                auto &p = countProfile[0];
                if (std::strcmp(fault, "missing") == 0) p.marker = 0;
                if (std::strcmp(fault, "stale") == 0) --p.generation;
                if (std::strcmp(fault, "legacy") == 0) p.version = 1;
                if (std::strcmp(fault, "core") == 0) p.core = 1;
                if (std::strcmp(fault, "time") == 0) p.time[3] = p.time[2] - 1;
            }
        }
        using namespace ServerDedupReference;
        const Shape shape {uint32_t(world), uint32_t(experts), uint32_t(bs), uint32_t(h), uint32_t(topK),
            dtype == TileXR::TILEXR_DATA_TYPE_BFP16, quant == 4, outDtype == TileXR::TILEXR_DATA_TYPE_FP8E4M3};
        const auto layout = TileXREp::TileXREpServerDedupDataLayout(shape.bs, shape.h, shape.k, shape.experts, shape.quantized);
        const char *packetFault = std::getenv("TEST_PACKET_FAULT");
        const std::string packetFailure = packetFault == nullptr ? "" : packetFault;
        std::fill(sourceMask, sourceMask + bs, 0);
        for (uint32_t source = 0; source < shape.ranks; ++source) {
            const auto expected = SourcePackets(Ids(shape, source), Weights(shape, source), source, shape.ranks, shape.experts, shape.k);
            for (const auto &packet : expected) {
                const auto &first = packet.members[0];
                if (source == uint32_t(rank)) {
                    sourceMask[packet.token] |= uint32_t(1) << first.k;
                    weightsOut[packet.token * topK + first.k] = packet.members.size() > 1U ? 1.0f : first.weight;
                    if (packet.members.size() == 1U && packetFailure == "direct-weight")
                        weightsOut[packet.token * topK + first.k] = 0.0f;
                }
                const uint32_t local = shape.experts / shape.ranks;
                if (first.expert / local != uint32_t(rank)) continue;
                auto offset = TileXREp::TileXREpServerDedupDispatchOffset(layout, source, rank,
                    first.expert % local, first.q, shape.ranks, local, shape.bs);
                auto payload = Payload(shape, source, packet, layout);
                if (packet.members.size() == 1U) {
                    if (packetFailure == "direct-missing") continue;
                    if (packetFailure == "direct-hidden") payload[0] ^= 1U;
                    if (packetFailure == "direct-tail") Put(payload, layout.tailOffset + 16U, uint32_t(1));
                    if (packetFailure == "direct-q") offset += layout.tokenStride;
                }
                packets[reinterpret_cast<uintptr_t>(args.peerMems[rank]) + offset] = Pack(payload);
            }
        }
        if (packetFault) {
            const std::string fault(packetFault);
            if (fault == "mask") sourceMask[0] ^= 1U;
            if (fault == "input-weight") static_cast<float *>(weights)[0] += 1.0f;
            if (fault == "source-weight") std::fill(weightsOut, weightsOut + bs * topK, 0.0f);
            for (auto &packet : packets) {
                uint64_t offset = 0;
                if (fault == "flag") offset = packet.second.size() - 4U;
                if (fault == "tail-q") offset = (layout.tailOffset + 36U) / 480U * 512U + (layout.tailOffset + 36U) % 480U;
                if (fault == "header-weight") offset = (layout.tailOffset + 12U) / 480U * 512U + (layout.tailOffset + 12U) % 480U;
                if (fault == "flag" || fault == "tail-q" || fault == "header-weight" || fault == "hidden") packet.second[offset] ^= 1U;
            }
        }
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
