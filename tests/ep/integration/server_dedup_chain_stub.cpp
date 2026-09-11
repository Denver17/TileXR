// 仅由 test_server_dedup_chain.py 链接到真实 consumer；不进入生产安装包。
#include <cstdlib>
#include <array>
#include <cstring>
#include <fstream>
#include <map>
#include <string>
#include "acl/acl.h"
#include "tilexr_ep.h"
#include "ep_memory_server_dedup_profile.h"
#include "memory_boundary_test.h"

namespace {
int deviceId = 0;
int rankId = -1;
TileXR::CommArgs args;
std::map<void *, void *> allocations;
std::map<std::string, uint64_t> calls;
std::array<void *, 8> firstBuffers {};
std::array<uint8_t, 64> boundaryTail {};
bool boundaryMode = false;
std::array<TileXREp::EpServerDedupProfileRecord, TileXREp::kEpServerDedupProfileMaxCores> profile {};
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

void WriteProfile()
{
    using namespace TileXREp;
    const char *overrideValue = std::getenv("TILEXR_EP_DISPATCH_MEMORY_SERVER_DEDUP_BLOCK_DIM");
    const uint32_t cores = overrideValue == nullptr || *overrideValue == '\0' ? 4 :
        static_cast<uint32_t>(std::strtoul(overrideValue, nullptr, 10));
    if (cores == 0 || cores > profile.size()) { return; }
    for (uint32_t core = 0; core < cores; ++core) {
        const uint64_t start = 1000U + 100U * calls["launch"] + core;
        profile[core] = {kEpServerDedupProfileMarker, kEpServerDedupProfileVersion,
            sizeof(EpServerDedupProfileRecord), static_cast<uint32_t>(rankId), core, cores, 0,
            calls["launch"], start, start + 10U, start + 20U, 0};
    }
    const char *fault = std::getenv("TEST_PROFILE_FAULT");
    if (fault == nullptr) { return; }
    auto &record = profile[cores - 1];
    const std::string field(fault);
    if (field == "marker") { record.marker = 0; }
    else if (field == "version") { ++record.version; }
    else if (field == "size") { record.recordBytes = 0; }
    else if (field == "rank") { ++record.rank; }
    else if (field == "core") { record.core = cores; }
    else if (field == "block_dim") { ++record.blockDim; }
    else if (field == "generation") { --record.generation; }
    else if (field == "start") { record.start = 0; }
    else if (field == "checked") { record.checked = record.start - 1; }
    else if (field == "end") { record.end = record.checked - 1; }
    else if (field == "missing") { record = {}; }
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
    if (boundaryMode && source == args.peerMems[rankId] + TileXRMemoryBoundaryTest::kOffset - 32 && bytes == 64) {
        std::memcpy(destination, boundaryTail.data(), bytes);
        return Call("boundary-host-read");
    }
    std::memcpy(destination, source, bytes);
    if (source == profile.data()) { return Call("profile-read"); }
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
    args.peerMems[rank] = reinterpret_cast<GM_ADDR>(profile.data());
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
    uint32_t *sourceMask, uint32_t *group, TileXR::TileXRDataType dtype,
    TileXR::TileXRDataType outDtype, aclrtStream stream)
{
    if (x == nullptr || ids == nullptr || comm != &args || bs != 2 || h != 16 || topK != 3 ||
        experts != world * 2 || rank != rankId || tpWorld != 1 || tpRank != 0 || shard != 0 ||
        shared != 0 || sharedRanks != 0 || quant != 0 || globalBs != world * bs || countType != 0 ||
        maskType != 0 || scales != nullptr || mask != nullptr || weights != nullptr || dynamic != nullptr ||
        assist != nullptr || tpCounts != nullptr || expandScales != nullptr || expandX == nullptr ||
        counts == nullptr || sendCounts == nullptr || assistEx == nullptr || sourceMask == nullptr ||
        group == nullptr || stream != reinterpret_cast<void *>(0x20) || outDtype != TileXR::TILEXR_DATA_TYPE_RESERVED ||
        (dtype != TileXR::TILEXR_DATA_TYPE_FP16 && dtype != TileXR::TILEXR_DATA_TYPE_BFP16)) { return 92; }
    const std::array<void *, 8> current {{x, ids, expandX, counts, sendCounts, assistEx, sourceMask, group}};
    if (calls["launch"] == 0) { firstBuffers = current; }
    if (current != firstBuffers || allocations.size() != current.size() || calls["comm-init"] != 1 ||
        calls["create-stream"] != 1) { return 92; }
    for (auto buffer : current) { if (allocations.count(buffer) != 1) { return 92; } }
    const int result = Call("launch");
    const char *mode = std::getenv("TILEXR_VALIDATION_RUN_MODE");
    if (result == 0 && mode != nullptr && std::strcmp(mode, "breakdown") == 0) { WriteProfile(); }
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
