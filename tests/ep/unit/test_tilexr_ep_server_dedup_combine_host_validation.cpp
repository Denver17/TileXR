#include "server_dedup_combine_test_fixture.h"
using namespace TileXREp;
using namespace TileXR;
using ServerDedupTest::Check;
namespace {
CommArgs g_args {};
CommArgs *g_host = &g_args;
GM_ADDR g_dev = reinterpret_cast<GM_ADDR>(0xB00000);
bool g_a5 = true;
int g_hostRet = TILEXR_SUCCESS, g_devRet = TILEXR_SUCCESS, g_launchRet = TILEXR_SUCCESS;
int g_hostCalls = 0, g_devCalls = 0, g_launchCalls = 0, g_magicCalls = 0;
EpMemoryServerDedupCombineParams g_seen {};
EpHostLaunchContext g_context {};
EpMemoryServerDedupCombineLayout g_layout {};
void Rejected(const EpMemoryServerDedupCombineParams &params, int expected)
{
    const int launches = g_launchCalls;
    Check(CombineTest::Invoke(params) == expected, "error code propagated");
    Check(g_launchCalls == launches && g_magicCalls == 0, "rejection cannot launch or consume magic");
}
}
int TileXRGetCommArgsHost(TileXRCommPtr comm, CommArgs *&args)
{
    Check(comm == CombineTest::ValidParams().combine.comm, "Host comm forwarded");
    ++g_hostCalls;
    args = g_host;
    return g_hostRet;
}
int TileXRGetCommArgsDev(TileXRCommPtr comm, GM_ADDR &args)
{
    Check(comm == CombineTest::ValidParams().combine.comm, "device comm forwarded");
    ++g_devCalls;
    args = g_dev;
    return g_devRet;
}
int TileXRCommNextMagic(TileXRCommPtr, int64_t *) { ++g_magicCalls; return TILEXR_SUCCESS; }
namespace TileXREp {
bool TileXREpIsA5Soc() { return g_a5; }
int TileXREpLaunchCombineMemoryServerDedupKernel(const EpMemoryServerDedupCombineParams &params,
    const EpHostLaunchContext &context, const EpMemoryServerDedupCombineLayout &layout)
{
    ++g_launchCalls;
    g_seen = params;
    g_context = context;
    g_layout = layout;
    return g_launchRet;
}
}
int main()
{
    CombineTest::InitComm(g_args);
    auto valid = CombineTest::ValidParams();
    Check(CombineTest::Invoke(valid) == TILEXR_SUCCESS && g_launchCalls == 1, "valid call reaches launch fake");
    const auto &p = valid.combine;
    const auto &s = g_seen.combine;
    Check(s.expertOut == p.expertOut && s.assistInfoForCombine == p.assistInfoForCombine &&
        s.epRecvCounts == p.epRecvCounts && s.expertScales == p.expertScales && s.yOut == p.yOut &&
        s.comm == p.comm && s.stream == p.stream && g_seen.tokenType == valid.tokenType &&
        g_seen.relayReadIndex == valid.relayReadIndex && g_seen.sourceMask == valid.sourceMask,
        "all original tensor pointers preserved, including full prefix start");
    Check(s.bs == 4 && s.h == 13 && s.topK == 6 && s.moeExpertNum == 192 && s.epWorldSize == 64 &&
        s.epRankId == 9 && s.globalBs == 256 && s.tpWorldSize == 1 && s.tpRankId == 0 &&
        s.expertShardType == 0 && s.sharedExpertNum == 0 && s.sharedExpertRankNum == 0 && s.quantMode == 0 &&
        s.dtype == TILEXR_DATA_TYPE_BFP16 && s.activeMaskType == 0 && s.xActiveMask == nullptr &&
        s.sharedExpertX == nullptr && g_context.hostArgs == &g_args && g_context.devArgs == g_dev &&
        g_layout.capacityRows == 768 && g_layout.requiredWindowBytes == 1454080,
        "scalars and validated context forwarded");
    for (int i = 0; i < 39; ++i) {
        auto invalid = valid;
        auto &v = invalid.combine;
        int error = TILEXR_ERROR_PARA_CHECK_FAIL;
        switch (i) {
            case 0: v.expertOut = nullptr; break;
            case 1: v.assistInfoForCombine = nullptr; break;
            case 2: v.epRecvCounts = nullptr; break;
            case 3: v.expertScales = nullptr; break;
            case 4: v.yOut = nullptr; break;
            case 5: invalid.tokenType = nullptr; break;
            case 6: invalid.relayReadIndex = nullptr; break;
            case 7: invalid.sourceMask = nullptr; break;
            case 8: v.comm = nullptr; break;
            case 9: v.stream = nullptr; break;
            case 10: v.expertOut = reinterpret_cast<void *>(0x100001); break;
            case 11: v.yOut = reinterpret_cast<void *>(0x500001); break;
            case 12: v.assistInfoForCombine = reinterpret_cast<int32_t *>(0x200002); break;
            case 13: v.epRecvCounts = reinterpret_cast<int32_t *>(0x300002); break;
            case 14: v.expertScales = reinterpret_cast<float *>(0x400002); break;
            case 15: invalid.tokenType = reinterpret_cast<uint32_t *>(0x600002); break;
            case 16: invalid.relayReadIndex = reinterpret_cast<void *>(0x700002); break;
            case 17: invalid.sourceMask = reinterpret_cast<uint32_t *>(0x800002); break;
            case 18: v.bs = 0; break;
            case 19: v.h = -1; break;
            case 20: v.topK = 33; break;
            case 21: v.moeExpertNum = 193; break;
            case 22: v.epRankId = 8; break;
            case 23: v.epWorldSize = 16; break;
            case 24: v.globalBs = 255; break;
            case 25: v.globalBs = -1; break;
            case 26: v.bs = int64_t(UINT32_MAX) + 1; break;
            case 27: v.h = int64_t(UINT32_MAX) + 1; break;
            case 28: v.quantMode = 4; error = TILEXR_ERROR_NOT_SUPPORT; break;
            case 29: v.quantMode = 256; error = TILEXR_ERROR_NOT_SUPPORT; break;
            case 30: v.dtype = TILEXR_DATA_TYPE_FP32; error = TILEXR_ERROR_NOT_SUPPORT; break;
            case 31: v.xActiveMask = reinterpret_cast<bool *>(0x1000); error = TILEXR_ERROR_NOT_SUPPORT; break;
            case 32: v.activeMaskType = 1; error = TILEXR_ERROR_NOT_SUPPORT; break;
            case 33: v.sharedExpertX = reinterpret_cast<void *>(0x1000); error = TILEXR_ERROR_NOT_SUPPORT; break;
            case 34: v.sharedExpertNum = 1; error = TILEXR_ERROR_NOT_SUPPORT; break;
            case 35: v.sharedExpertRankNum = 1; error = TILEXR_ERROR_NOT_SUPPORT; break;
            case 36: v.tpWorldSize = 2; error = TILEXR_ERROR_NOT_SUPPORT; break;
            case 37: v.tpRankId = 1; error = TILEXR_ERROR_NOT_SUPPORT; break;
            case 38: v.expertShardType = 1; error = TILEXR_ERROR_NOT_SUPPORT; break;
        }
        Rejected(invalid, error);
    }
    // Each tensor's byte extent must fit the address space without reading it.
    for (int i = 0; i < 8; ++i) {
        auto invalid = valid;
        void *nearEnd = reinterpret_cast<void *>(UINTPTR_MAX - 3U);
        switch (i) {
            case 0: invalid.combine.expertOut = nearEnd; break;
            case 1: invalid.combine.yOut = nearEnd; break;
            case 2: invalid.combine.assistInfoForCombine = static_cast<int32_t *>(nearEnd); break;
            case 3: invalid.combine.epRecvCounts = static_cast<int32_t *>(nearEnd); break;
            case 4: invalid.combine.expertScales = static_cast<float *>(nearEnd); break;
            case 5: invalid.tokenType = static_cast<uint32_t *>(nearEnd); break;
            case 6: invalid.relayReadIndex = nearEnd; break;
            case 7: invalid.sourceMask = static_cast<uint32_t *>(nearEnd); break;
        }
        Rejected(invalid, TILEXR_ERROR_PARA_CHECK_FAIL);
    }
    // Same fixed prefix is insufficient once the kernel writes the real stage.
    const uint64_t fullBytes = g_args.peerMemBytes;
    auto unsupportedLane = valid;
    unsupportedLane.combine.h = 100000;
    Rejected(unsupportedLane, TILEXR_ERROR_NOT_SUPPORT);
    // M4 now uses a 240KiB budget on A5. Its old 190KiB boundary no longer
    // limits this shape; M5's unchanged workspace becomes the limiting stage.
    g_args.peerMemBytes = 8ULL * 1024U * 1024U * 1024U;
    auto laneBoundary = valid;
    laneBoundary.combine.h = 10145;
    Check(CombineTest::Invoke(laneBoundary) == TILEXR_SUCCESS, "old M4 UB boundary now fits");
    laneBoundary.combine.h = 11008;
    Check(CombineTest::Invoke(laneBoundary) == TILEXR_SUCCESS, "last complete M5 lane fits UB");
    laneBoundary.combine.h = 11009;
    Rejected(laneBoundary, TILEXR_ERROR_NOT_SUPPORT);
    laneBoundary.combine.topK = 32;
    laneBoundary.combine.h = 8896;
    Check(CombineTest::Invoke(laneBoundary) == TILEXR_SUCCESS, "K32 fits M5 probe and aggregate buffers");
    laneBoundary.combine.h = 8897;
    Rejected(laneBoundary, TILEXR_ERROR_NOT_SUPPORT);
    auto oversizedMetadata = valid;
    oversizedMetadata.combine.bs = UINT32_MAX;
    oversizedMetadata.combine.h = 1;
    oversizedMetadata.combine.topK = 1;
    oversizedMetadata.combine.globalBs = 0;
    Rejected(oversizedMetadata, TILEXR_ERROR_NOT_SUPPORT);
    g_args.peerMemBytes = 2ULL * (1048576ULL + TILEXR_FLAG_BUFF_BYTES);
    Rejected(valid, TILEXR_ERROR_PARA_CHECK_FAIL);
    g_args.peerMemBytes = fullBytes;
    Check(CombineTest::Invoke(valid) == TILEXR_SUCCESS, "exact full data capacity accepted");
    const int hosts = g_hostCalls;
    g_a5 = false;
    Rejected(valid, TILEXR_ERROR_NOT_SUPPORT);
    Check(g_hostCalls == hosts, "non A5 rejected before comm query");
    g_a5 = true;
    auto noStream = valid; noStream.combine.stream = nullptr;
    Rejected(noStream, TILEXR_ERROR_PARA_CHECK_FAIL);
    Check(g_hostCalls == hosts, "basic rejection before comm query");
    const int devices = g_devCalls;
    g_hostRet = TILEXR_ERROR_NOT_SUPPORT;
    Rejected(valid, TILEXR_ERROR_NOT_SUPPORT);
    g_hostRet = TILEXR_SUCCESS; g_host = nullptr;
    Rejected(valid, TILEXR_ERROR_NOT_INITIALIZED);
    g_host = &g_args;
    for (int i = 0; i < 8; ++i) {
        CombineTest::InitComm(g_args);
        int error = TILEXR_ERROR_PARA_CHECK_FAIL;
        switch (i) {
            case 0: g_args.rank = -1; break;
            case 1: g_args.rank = g_args.rankSize; break;
            case 2: g_args.peerMemBytes -= 1024; break;
            case 3: g_args.peerMemBytes += 512; break;
            case 4: g_args.peerMemBytes = 0; break;
            case 5: g_args.peerMems[63] = nullptr; error = TILEXR_ERROR_NOT_INITIALIZED; break;
            case 6: g_args.peerMems[63] = reinterpret_cast<GM_ADDR>(0x1000001); break;
            case 7: g_args.peerMems[63] = reinterpret_cast<GM_ADDR>(UINTPTR_MAX - 511U); break;
        }
        Rejected(valid, error);
    }
    Check(g_devCalls == devices, "bad Host context/layout never requests device args");
    CombineTest::InitComm(g_args);
    g_devRet = TILEXR_ERROR_NOT_SUPPORT; Rejected(valid, TILEXR_ERROR_NOT_SUPPORT);
    g_devRet = TILEXR_SUCCESS; g_dev = nullptr; Rejected(valid, TILEXR_ERROR_NOT_INITIALIZED);
    g_dev = reinterpret_cast<GM_ADDR>(0xB00000);
    for (auto dtype : {TILEXR_DATA_TYPE_FP16, TILEXR_DATA_TYPE_BFP16}) {
        auto inferred = valid;
        inferred.combine.dtype = dtype;
        inferred.combine.epWorldSize = inferred.combine.epRankId = inferred.combine.globalBs = 0;
        inferred.combine.tpWorldSize = 0;
        Check(CombineTest::Invoke(inferred) == TILEXR_SUCCESS && g_seen.combine.dtype == dtype,
            "supported dtypes and existing zero sentinels");
    }
    g_launchRet = TILEXR_ERROR_NOT_SUPPORT;
    const int launches = g_launchCalls;
    Check(CombineTest::Invoke(valid) == TILEXR_ERROR_NOT_SUPPORT && g_launchCalls == launches + 1,
        "launch error propagated");
    Check(g_magicCalls == 0, "Host path never advances magic");
    return CombineTest::Finish("host_validation");
}
