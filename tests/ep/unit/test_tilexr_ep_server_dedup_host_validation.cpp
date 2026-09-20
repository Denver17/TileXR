#include <limits>
#include <cstdlib>

#include "ep_kernel_launch.h"
#include "ep_memory_server_dedup_launch.h"
#include "ep_memory_server_dedup_profile.h"
#include "server_dedup_test_checks.h"

namespace {
using namespace TileXR;
using namespace TileXREp;
using ServerDedupTest::Check;

bool g_a5 = true;
int g_hostRet = TILEXR_SUCCESS;
int g_devRet = TILEXR_SUCCESS;
int g_submitRet = TILEXR_SUCCESS;
int g_hostCalls = 0;
int g_devCalls = 0;
int g_submitCalls = 0;
int g_magicCalls = 0;
TileXRCommPtr g_seenComm = nullptr;
CommArgs g_commArgs {};
CommArgs *g_hostArgs = &g_commArgs;
GM_ADDR g_devArgs = reinterpret_cast<GM_ADDR>(0xA000);
EpMemoryServerDedupParams g_submitted {};
EpMemoryServerDedupLayout g_layout {};
EpHostLaunchContext g_context {};

EpMemoryServerDedupParams ValidParams()
{
    EpMemoryServerDedupParams params {};
    auto &p = params.dispatch;
    p.x = reinterpret_cast<void *>(0x1000);
    p.expertIds = reinterpret_cast<int32_t *>(0x2000);
    p.expertScales = reinterpret_cast<void *>(0x3000);
    p.comm = reinterpret_cast<TileXRCommPtr>(0x4000);
    p.stream = reinterpret_cast<aclrtStream>(0x5000);
    p.expandXOut = reinterpret_cast<void *>(0x6000);
    p.expertTokenNumsOut = reinterpret_cast<int64_t *>(0x7000);
    p.epRecvCountsOut = reinterpret_cast<int32_t *>(0x8000);
    p.bs = 4;
    p.h = 13;
    p.topK = 6;
    p.moeExpertNum = 192;
    p.epWorldSize = 64;
    p.epRankId = 9;
    p.tpWorldSize = 1;
    p.globalBs = 256;
    p.expertTokenNumsType = 1;
    p.dtype = TILEXR_DATA_TYPE_BFP16;
    p.expandXOutDtype = TILEXR_DATA_TYPE_BFP16;
    params.tokenTypeOut = reinterpret_cast<uint32_t *>(0xB004);
    params.destinationIndexOut = reinterpret_cast<uint32_t *>(0x9004);
    params.sourceMaskOut = reinterpret_cast<uint32_t *>(0xC004);
    params.relayReadIndexOut = reinterpret_cast<uint32_t *>(0xD004);
    params.expertScalesOut = reinterpret_cast<float *>(0xF000);
    return params;
}

int Invoke(const EpMemoryServerDedupParams &params)
{
    const auto &p = params.dispatch;
    return TileXRMoeEpDispatchMemoryServerDedup(p.x, p.expertIds, p.scales, p.xActiveMask,
        p.activeMaskType, p.expertScales, p.comm, p.bs, p.h, p.topK, p.moeExpertNum,
        p.epWorldSize, p.epRankId, p.tpWorldSize, p.tpRankId, p.expertShardType,
        p.sharedExpertNum, p.sharedExpertRankNum, p.quantMode, p.globalBs, p.expertTokenNumsType,
        p.expandXOut, p.dynamicScalesOut, p.expertTokenNumsOut,
        p.epRecvCountsOut, p.tpRecvCountsOut, p.expandScalesOut, params.tokenTypeOut, params.destinationIndexOut,
        params.relayReadIndexOut, params.sourceMaskOut, params.expertScalesOut, p.dtype, p.expandXOutDtype, p.stream);
}

void CheckRejected(const EpMemoryServerDedupParams &params, int expected)
{
    const int submits = g_submitCalls;
    Check(Invoke(params) == expected, "invalid call returns expected error");
    Check(g_submitCalls == submits && g_magicCalls == 0, "rejected call does not submit or consume magic");
}

} // namespace

int TileXRGetCommArgsHost(TileXRCommPtr comm, TileXR::CommArgs *&args)
{
    ++g_hostCalls;
    g_seenComm = comm;
    args = g_hostArgs;
    return g_hostRet;
}

int TileXRGetCommArgsDev(TileXRCommPtr comm, GM_ADDR &args)
{
    ++g_devCalls;
    g_seenComm = comm;
    args = g_devArgs;
    return g_devRet;
}

int TileXRCommNextMagic(TileXRCommPtr, int64_t *)
{
    ++g_magicCalls;
    return TileXR::TILEXR_SUCCESS;
}

namespace TileXREp {
bool TileXREpIsA5Soc() { return g_a5; }

int TileXREpLaunchDispatchMemoryServerDedupKernel(const EpMemoryServerDedupParams &params,
    const EpHostLaunchContext &context, const EpMemoryServerDedupLayout &layout)
{
    ++g_submitCalls;
    g_submitted = params;
    g_context = context;
    g_layout = layout;
    return g_submitRet;
}
} // namespace TileXREp

int main()
{
    g_commArgs.rankSize = 64;
    g_commArgs.peerMemBytes = kEpServerDedupPeerBytes;
    g_commArgs.rank = 9;
    for (auto &peer : g_commArgs.peerMems) {
        peer = reinterpret_cast<GM_ADDR>(0xE000);
    }
    auto valid = ValidParams();
    Check(Invoke(valid) == TILEXR_SUCCESS && g_submitCalls == 1, "valid call reaches submission fake");
    const auto &p = valid.dispatch;
    const auto &captured = g_submitted.dispatch;
    Check(g_seenComm == p.comm && captured.comm == p.comm && captured.stream == p.stream, "comm and stream forwarded");
    Check(captured.x == p.x && captured.expertIds == p.expertIds && captured.expertScales == p.expertScales,
        "input and weight pointers forwarded without reading them");
    Check(captured.expandXOut == p.expandXOut && captured.expertTokenNumsOut == p.expertTokenNumsOut &&
        captured.epRecvCountsOut == p.epRecvCountsOut, "original output pointers forwarded");
    Check(g_submitted.tokenTypeOut == valid.tokenTypeOut &&
        g_submitted.destinationIndexOut == valid.destinationIndexOut &&
        g_submitted.sourceMaskOut == valid.sourceMaskOut &&
        g_submitted.relayReadIndexOut == valid.relayReadIndexOut && g_submitted.expertScalesOut == valid.expertScalesOut,
        "metadata and independent weight output retain their order");
    Check(captured.bs == 4 && captured.h == 13 && captured.topK == 6 && captured.moeExpertNum == 192 &&
        captured.globalBs == 256 && captured.epRankId == 9 && captured.epWorldSize == 64 &&
        captured.tpWorldSize == 1 && captured.expertTokenNumsType == 1, "shape and count settings forwarded");
    Check(captured.dtype == TILEXR_DATA_TYPE_BFP16 && captured.expandXOutDtype == TILEXR_DATA_TYPE_BFP16,
        "input and output dtype forwarded");
    Check(g_context.hostArgs == &g_commArgs && g_context.devArgs == g_devArgs &&
        g_layout.capacityRows == 768 && g_layout.tokenTypeBytes == 3072 && g_layout.destinationIndexBytes == 9216 &&
        g_layout.relayReadIndexRowStrideBytes == 48 && g_layout.relayReadIndexBytes == 36864,
        "validated context and bounded output layout forwarded");


    for (int test = 0; test < 45; ++test) {
        auto params = valid;
        auto &v = params.dispatch;
        int error = TILEXR_ERROR_PARA_CHECK_FAIL;
        switch (test) {
            case 0: v.x = nullptr; break;
            case 1: v.expertIds = nullptr; break;
            case 2: v.comm = nullptr; break;
            case 3: v.stream = nullptr; break;
            case 4: v.expandXOut = nullptr; break;
            case 5: v.expertTokenNumsOut = nullptr; break;
            case 6: v.epRecvCountsOut = nullptr; break;
            case 7: params.tokenTypeOut = nullptr; break;
            case 8: params.sourceMaskOut = nullptr; break;
            case 9: params.relayReadIndexOut = nullptr; break;
            case 10: params.tokenTypeOut = reinterpret_cast<uint32_t *>(0xB001); break;
            case 11: params.relayReadIndexOut = reinterpret_cast<uint32_t *>(0xD001); break;
            case 12: params.sourceMaskOut = reinterpret_cast<uint32_t *>(0xC001); break;
            case 13: v.bs = 0; break;
            case 14: v.h = 0; break;
            case 15: v.topK = 33; break;
            case 16: v.globalBs = 255; break;
            case 17: v.globalBs = -1; break;
            case 18: v.epRankId = 8; break;
            case 19: v.epWorldSize = 16; break;
            case 20: v.moeExpertNum = 1088; break;
            case 21: v.moeExpertNum = 193; break;
            case 22: v.bs = std::numeric_limits<int64_t>::max(); break;
            case 23: v.expertTokenNumsType = 2; break;
            case 24: v.expandXOutDtype = TILEXR_DATA_TYPE_FP16; break;
            case 25: v.expertScales = reinterpret_cast<void *>(0x3001); break;
            case 26: v.expertTokenNumsOut = reinterpret_cast<int64_t *>(0x7004); break;
            case 27: v.h = std::numeric_limits<int64_t>::max(); break;
            case 28: v.expertScales = nullptr; break;
            case 29: params.expertScalesOut = nullptr; break;
            case 30: params.expertScalesOut = reinterpret_cast<float *>(0xF001); break;
            case 31: v.quantMode = 4; v.dynamicScalesOut = reinterpret_cast<void *>(0x11000); break;
            case 32: v.dynamicScalesOut = reinterpret_cast<void *>(0x11000); break;
            case 33: params.expertScalesOut = static_cast<float *>(v.expertScales); break;
            case 34: params.expertScalesOut = static_cast<float *>(v.expertScales) + 1; break;
            case 35: params.expertScalesOut = reinterpret_cast<float *>(UINTPTR_MAX - 3U); break;
            // Reject values before the launch builder narrows its tiling fields.
            case 36: v.bs = int64_t(UINT32_MAX) + 1; break;
            case 37: v.h = int64_t(UINT32_MAX) + 1; break;
            case 38: v.moeExpertNum = int64_t(UINT16_MAX) + 1; break;
            case 39: v.topK = 256 + 6; break;
            case 40: v.expertTokenNumsType = 256 + 1; break;
            case 41: v.quantMode = 256; error = TILEXR_ERROR_NOT_SUPPORT; break;
            case 42: v.dtype = static_cast<TileXRDataType>(256 + TILEXR_DATA_TYPE_BFP16);
                error = TILEXR_ERROR_NOT_SUPPORT; break;
            case 43: params.destinationIndexOut = nullptr; break;
            case 44: params.destinationIndexOut = reinterpret_cast<uint32_t *>(0x9001); break;
        }
        CheckRejected(params, error);
    }

    for (auto dtype : {TILEXR_DATA_TYPE_FP16, TILEXR_DATA_TYPE_BFP16}) {
        for (auto outType : {TILEXR_DATA_TYPE_FP8E4M3, TILEXR_DATA_TYPE_FP8E5M2}) {
            auto quantized = valid;
            quantized.dispatch.dtype = dtype;
            quantized.dispatch.quantMode = 4;
            quantized.dispatch.expandXOutDtype = outType;
            quantized.dispatch.dynamicScalesOut = reinterpret_cast<void *>(0x11000);
            Check(Invoke(quantized) == TILEXR_SUCCESS && g_layout.dynamicScalesBytes > 0 &&
                g_submitted.dispatch.dynamicScalesOut == quantized.dispatch.dynamicScalesOut,
                "both input and FP8 types pass the existing quantization ABI");
            quantized.dispatch.dynamicScalesOut = nullptr;
            CheckRejected(quantized, TILEXR_ERROR_PARA_CHECK_FAIL);
        }
    }
    for (int test = 0; test < 12; ++test) {
        auto params = valid;
        auto &v = params.dispatch;
        switch (test) {
            case 0: v.activeMaskType = TILEXR_EP_ACTIVE_MASK_TOKEN; break;
            case 1: v.xActiveMask = reinterpret_cast<bool *>(0x1000); break;
            case 2: v.sharedExpertNum = 1; break;
            case 3: v.sharedExpertRankNum = 1; break;
            case 4: v.tpWorldSize = 2; break;
            case 5: v.tpRankId = 1; break;
            case 6: v.expertShardType = 1; break;
            case 7: v.quantMode = 3; break;
            case 8: v.dtype = TILEXR_DATA_TYPE_FP32; break;
            case 9: v.scales = reinterpret_cast<void *>(0x1000); break;
            case 10: v.expandScalesOut = reinterpret_cast<void *>(0x1000); break;
            case 11: v.tpRecvCountsOut = reinterpret_cast<int32_t *>(0x1000); break;
        }
        CheckRejected(params, TILEXR_ERROR_NOT_SUPPORT);
    }

    auto defaults = valid;
    defaults.dispatch.dtype = TILEXR_DATA_TYPE_FP16;
    defaults.dispatch.expandXOutDtype = TILEXR_DATA_TYPE_RESERVED;
    defaults.dispatch.globalBs = defaults.dispatch.epWorldSize = defaults.dispatch.epRankId = 0;
    defaults.dispatch.tpWorldSize = defaults.dispatch.expertTokenNumsType = 0;
    Check(Invoke(defaults) == TILEXR_SUCCESS && g_layout.globalBs == 256,
        "FP16/defaults and 4B-aligned five-table outputs accepted");

    g_a5 = false;
    const int hostCalls = g_hostCalls;
    const int devCalls = g_devCalls;
    CheckRejected(EpMemoryServerDedupParams {}, TILEXR_ERROR_NOT_SUPPORT);
    Check(g_hostCalls == hostCalls && g_devCalls == devCalls, "non-A5 rejects before comm/runtime state");
    g_a5 = true;
    g_hostRet = TILEXR_ERROR_INTERNAL;
    CheckRejected(valid, TILEXR_ERROR_INTERNAL);
    g_hostRet = TILEXR_SUCCESS;
    g_hostArgs = nullptr;
    CheckRejected(valid, TILEXR_ERROR_NOT_INITIALIZED);
    g_hostArgs = &g_commArgs;
    g_commArgs.peerMems[63] = nullptr;
    CheckRejected(valid, TILEXR_ERROR_NOT_INITIALIZED);
    g_commArgs.peerMems[63] = reinterpret_cast<GM_ADDR>(0xE000);
    g_commArgs.peerMemBytes = kEpServerDedupPeerBytes - 1U;
    CheckRejected(valid, TILEXR_ERROR_PARA_CHECK_FAIL);
    g_commArgs.peerMemBytes = kEpServerDedupPeerBytes;
    Check(Invoke(valid) == TILEXR_SUCCESS, "exact 8 GiB domain covers both windows and runtime flags");
    g_commArgs.peerMems[63] += 1;
    CheckRejected(valid, TILEXR_ERROR_PARA_CHECK_FAIL);
    g_commArgs.peerMems[63] -= 1;
    g_devRet = TILEXR_ERROR_MKIRT;
    CheckRejected(valid, TILEXR_ERROR_MKIRT);
    g_devRet = TILEXR_SUCCESS;
    g_devArgs = nullptr;
    CheckRejected(valid, TILEXR_ERROR_NOT_INITIALIZED);
    g_devArgs = reinterpret_cast<GM_ADDR>(0xA000);
    g_submitRet = TILEXR_ERROR_MKIRT;
    Check(Invoke(valid) == TILEXR_ERROR_MKIRT, "submission error propagated");
    Check(g_magicCalls == 0, "Host checkpoint never allocates a launch magic");
#if defined(TILEXR_EP_SERVER_DEDUP_ENABLE_PROFILING) && TILEXR_EP_SERVER_DEDUP_ENABLE_PROFILING
    return ServerDedupTest::Finish("host_profile_validation");
#else
    return ServerDedupTest::Finish("host_validation");
#endif
}
