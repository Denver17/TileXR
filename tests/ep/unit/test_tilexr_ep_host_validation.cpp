#include <cstdint>
#include <iostream>

#include "comm_args.h"
#include "ep_host.h"
#include "tilexr_types.h"

int TileXRGetCommArgsDev(TileXRCommPtr, GM_ADDR &commArgsPtr)
{
    commArgsPtr = nullptr;
    return TileXR::TILEXR_ERROR_NOT_INITIALIZED;
}

int TileXRGetCommArgsHost(TileXRCommPtr, TileXR::CommArgs *&commArgsPtr)
{
    commArgsPtr = nullptr;
    return TileXR::TILEXR_ERROR_NOT_INITIALIZED;
}

namespace {

int g_failures = 0;

void CheckInt(const char *label, int actual, int expected)
{
    if (actual != expected) {
        std::cerr << label << " actual=" << actual << " expected=" << expected << std::endl;
        ++g_failures;
    }
}

TileXREp::EpDispatchParams ValidParams()
{
    static uint16_t x[32] = {};
    static int32_t expertIds[8] = {};
    static uint16_t expandXOut[64] = {};
    static int64_t expertTokenNumsOut[4] = {};
    static int32_t epRecvCountsOut[2] = {};
    static int32_t assistInfoForCombineOut[32] = {};

    TileXREp::EpDispatchParams params {};
    params.x = x;
    params.expertIds = expertIds;
    params.comm = reinterpret_cast<TileXRCommPtr>(0x1000);
    params.bs = 4;
    params.h = 8;
    params.topK = 2;
    params.moeExpertNum = 8;
    params.expandXOut = expandXOut;
    params.expertTokenNumsOut = expertTokenNumsOut;
    params.epRecvCountsOut = epRecvCountsOut;
    params.assistInfoForCombineOut = assistInfoForCombineOut;
    params.dtype = TileXR::TILEXR_DATA_TYPE_FP16;
    params.stream = reinterpret_cast<aclrtStream>(0x2000);
    return params;
}

TileXREp::EpDispatchParams ValidV2Params()
{
    static bool xActiveMask[4] = {true, false, true, true};
    TileXREp::EpDispatchParams params = ValidParams();
    params.xActiveMask = xActiveMask;
    params.activeMaskType = TileXREp::TILEXR_EP_ACTIVE_MASK_TOKEN;
    params.epWorldSize = 2;
    params.epRankId = 0;
    params.tpWorldSize = 1;
    params.tpRankId = 0;
    params.expertShardType = 0;
    params.sharedExpertNum = 0;
    params.sharedExpertRankNum = 0;
    params.quantMode = 0;
    params.globalBs = 8;
    params.expertTokenNumsType = 1;
    return params;
}

TileXREp::EpDispatchParams ValidStaticQuantParams()
{
    TileXREp::EpDispatchParams params = ValidV2Params();
    static int8_t expandXOutI8[64] = {};
    static float scales[1] = {1.0f};
    params.scales = scales;
    params.expandXOut = expandXOutI8;
    params.quantMode = 1;
    params.dtype = TileXR::TILEXR_DATA_TYPE_INT8;
    return params;
}

TileXREp::EpDispatchParams ValidPerTokenDynamicQuantParams()
{
    TileXREp::EpDispatchParams params = ValidV2Params();
    static int8_t expandXOutI8[64] = {};
    static float dynamicScalesOut[8] = {};
    params.expandXOut = expandXOutI8;
    params.dynamicScalesOut = dynamicScalesOut;
    params.quantMode = 2;
    params.dtype = TileXR::TILEXR_DATA_TYPE_INT8;
    return params;
}

TileXREp::EpDispatchParams ValidMemoryMxfp8Params()
{
    TileXREp::EpDispatchParams params = ValidV2Params();
    static uint8_t expandXOutFp8[64] = {};
    static uint8_t dynamicScalesOut[16] = {};
    params.expandXOut = expandXOutFp8;
    params.dynamicScalesOut = dynamicScalesOut;
    params.quantMode = 4;
    params.expandXOutDtype = TileXR::TILEXR_DATA_TYPE_FP8E4M3;
    return params;
}

TileXREp::EpCombineParams ValidCombineParams()
{
    static uint16_t expertOut[64] = {};
    static int32_t assistInfoForCombine[32] = {};
    static int32_t epRecvCounts[2] = {};
    static uint16_t yOut[32] = {};

    TileXREp::EpCombineParams params {};
    params.expertOut = expertOut;
    params.assistInfoForCombine = assistInfoForCombine;
    params.epRecvCounts = epRecvCounts;
    params.comm = reinterpret_cast<TileXRCommPtr>(0x1000);
    params.bs = 4;
    params.h = 8;
    params.topK = 2;
    params.moeExpertNum = 8;
    params.yOut = yOut;
    params.dtype = TileXR::TILEXR_DATA_TYPE_FP16;
    params.stream = reinterpret_cast<aclrtStream>(0x2000);
    return params;
}

TileXREp::EpCombineParams ValidMemoryCombineMxfp8Params(int64_t quantMode)
{
    TileXREp::EpCombineParams params = ValidCombineParams();
    static float expertScales[8] = {1.0f, 0.5f, 1.0f, 0.5f, 1.0f, 0.5f, 1.0f, 0.5f};
    params.expertScales = expertScales;
    params.quantMode = quantMode;
    params.globalBs = 8;
    return params;
}

TileXR::CommArgs ValidCommArgs()
{
    TileXR::CommArgs args {};
    args.rank = 0;
    args.localRank = 0;
    args.rankSize = 2;
    args.localRankSize = args.rankSize;
    args.peerMems[0] = reinterpret_cast<GM_ADDR>(0x10000000);
    args.peerMems[1] = reinterpret_cast<GM_ADDR>(0x20000000);
    args.peerMemBytes = TileXREp::kEpMemoryRequiredPeerBytes;
    return args;
}

TileXR::CommArgs CrossNodeCommArgs()
{
    TileXR::CommArgs args {};
    args.rank = 0;
    args.localRank = 0;
    args.rankSize = 8;
    args.localRankSize = 4;
    for (int rank = 0; rank < args.rankSize; ++rank) {
        args.peerMems[rank] = reinterpret_cast<GM_ADDR>(0x10000000 + rank * 0x10000000);
    }
    args.peerMemBytes = TileXREp::kEpMemoryRequiredPeerBytes;
    return args;
}

TileXR::CommArgs CrossNodeUdmaCommArgs()
{
    TileXR::CommArgs args = CrossNodeCommArgs();
    args.extraFlag |= TileXR::ExtraFlag::UDMA;
    args.udmaInfoPtr = reinterpret_cast<GM_ADDR>(0x30000000);
    args.udmaRegistryPtr = reinterpret_cast<GM_ADDR>(0x40000000);
    return args;
}

int ValidateMemoryDispatch(
    const TileXREp::EpDispatchParams &params, const TileXR::CommArgs &commArgs)
{
    TileXREp::EpMemoryDispatchReferenceConfig config {};
    return TileXREp::TileXREpValidateDispatchMemoryConfig(params, commArgs, 48, &config);
}

void TestBasicValidation()
{
    TileXREp::EpDispatchParams params = ValidParams();
    CheckInt("valid basic params", TileXREp::TileXREpValidateBasicDispatchParams(params), TileXR::TILEXR_SUCCESS);

    params = ValidParams();
    params.x = nullptr;
    CheckInt("null x", TileXREp::TileXREpValidateBasicDispatchParams(params),
        TileXR::TILEXR_ERROR_PARA_CHECK_FAIL);

    params = ValidParams();
    params.comm = nullptr;
    CheckInt("null comm", TileXREp::TileXREpValidateBasicDispatchParams(params),
        TileXR::TILEXR_ERROR_PARA_CHECK_FAIL);

    params = ValidParams();
    params.bs = 0;
    CheckInt("zero bs", TileXREp::TileXREpValidateBasicDispatchParams(params),
        TileXR::TILEXR_ERROR_PARA_CHECK_FAIL);

    params = ValidParams();
    params.dtype = TileXR::TILEXR_DATA_TYPE_FP32;
    CheckInt("unsupported fp32", TileXREp::TileXREpValidateBasicDispatchParams(params),
        TileXR::TILEXR_ERROR_PARA_CHECK_FAIL);
}

void TestCommValidation()
{
    TileXREp::EpDispatchParams params = ValidParams();
    TileXR::CommArgs commArgs = ValidCommArgs();
    TileXREp::EpWindowConfig window {};
    CheckInt("valid dispatch config", TileXREp::TileXREpValidateDispatchConfig(params, commArgs, &window),
        TileXR::TILEXR_SUCCESS);

    commArgs = ValidCommArgs();
    commArgs.rankSize = 0;
    CheckInt("rank size zero", TileXREp::TileXREpValidateDispatchConfig(params, commArgs, &window),
        TileXR::TILEXR_ERROR_PARA_CHECK_FAIL);

    commArgs = ValidCommArgs();
    commArgs.peerMems[1] = nullptr;
    CheckInt("missing peer mem", TileXREp::TileXREpValidateDispatchConfig(params, commArgs, &window),
        TileXR::TILEXR_ERROR_NOT_INITIALIZED);

    commArgs = CrossNodeCommArgs();
    CheckInt("cross-node dispatch needs udma registry",
        TileXREp::TileXREpValidateDispatchConfig(params, commArgs, &window),
        TileXR::TILEXR_ERROR_NOT_INITIALIZED);

    commArgs = CrossNodeCommArgs();
    CheckInt("cross-node memory dispatch config", ValidateMemoryDispatch(params, commArgs),
        TileXR::TILEXR_SUCCESS);

    commArgs.peerMemBytes = TileXREp::kEpMemoryRequiredPeerBytes - 1;
    CheckInt("memory dispatch rejects undersized peer memory", ValidateMemoryDispatch(params, commArgs),
        TileXR::TILEXR_ERROR_PARA_CHECK_FAIL);

    commArgs = CrossNodeCommArgs();
    commArgs.peerMems[1] = nullptr;
    CheckInt("cross-node memory dispatch missing peer mem", ValidateMemoryDispatch(params, commArgs),
        TileXR::TILEXR_ERROR_NOT_INITIALIZED);

    params = ValidV2Params();
    commArgs = ValidCommArgs();
    params.tpWorldSize = 2;
    params.epWorldSize = 1;
    static int32_t memoryTpRecvCountsOut[2] = {};
    params.tpRecvCountsOut = memoryTpRecvCountsOut;
    CheckInt("memory dispatch rejects tp", ValidateMemoryDispatch(params, commArgs),
        TileXR::TILEXR_ERROR_NOT_SUPPORT);

    params = ValidStaticQuantParams();
    commArgs = ValidCommArgs();
    CheckInt("memory dispatch rejects static quant", ValidateMemoryDispatch(params, commArgs),
        TileXR::TILEXR_ERROR_NOT_SUPPORT);

    params = ValidMemoryMxfp8Params();
    commArgs = ValidCommArgs();
    CheckInt("memory dispatch supports mxfp8 e4m3", ValidateMemoryDispatch(params, commArgs),
        TileXR::TILEXR_SUCCESS);

    params = ValidMemoryMxfp8Params();
    params.expandXOutDtype = TileXR::TILEXR_DATA_TYPE_FP8E5M2;
    CheckInt("memory dispatch supports mxfp8 e5m2", ValidateMemoryDispatch(params, commArgs),
        TileXR::TILEXR_SUCCESS);

    params = ValidMemoryMxfp8Params();
    params.dynamicScalesOut = nullptr;
    CheckInt("memory mxfp8 requires dynamic scales output", ValidateMemoryDispatch(params, commArgs),
        TileXR::TILEXR_ERROR_PARA_CHECK_FAIL);

    params = ValidMemoryMxfp8Params();
    params.expandXOutDtype = TileXR::TILEXR_DATA_TYPE_INT8;
    CheckInt("memory mxfp8 requires fp8 output dtype", ValidateMemoryDispatch(params, commArgs),
        TileXR::TILEXR_ERROR_PARA_CHECK_FAIL);

    params = ValidV2Params();
    params.epWorldSize = 4;
    params.globalBs = 16;
    params.sharedExpertNum = 1;
    params.sharedExpertRankNum = 1;
    params.moeExpertNum = 6;
    commArgs = ValidCommArgs();
    commArgs.rankSize = 4;
    commArgs.localRankSize = 4;
    commArgs.peerMems[2] = reinterpret_cast<GM_ADDR>(0x30000000);
    commArgs.peerMems[3] = reinterpret_cast<GM_ADDR>(0x40000000);
    CheckInt("memory dispatch supports shared experts", ValidateMemoryDispatch(params, commArgs),
        TileXR::TILEXR_SUCCESS);

    params = ValidV2Params();
    params.xActiveMask = nullptr;
    params.activeMaskType = TileXREp::TILEXR_EP_ACTIVE_MASK_TOKEN;
    commArgs = ValidCommArgs();
    CheckInt("memory token mask requires pointer", ValidateMemoryDispatch(params, commArgs),
        TileXR::TILEXR_ERROR_PARA_CHECK_FAIL);

    params = ValidV2Params();
    params.activeMaskType = TileXREp::TILEXR_EP_ACTIVE_MASK_NONE;
    commArgs = ValidCommArgs();
    CheckInt("memory none mask rejects pointer", ValidateMemoryDispatch(params, commArgs),
        TileXR::TILEXR_ERROR_PARA_CHECK_FAIL);

    params = ValidV2Params();
    static bool expertMask[8] = {true, true, false, true, true, false, true, true};
    params.xActiveMask = expertMask;
    params.activeMaskType = TileXREp::TILEXR_EP_ACTIVE_MASK_EXPERT;
    commArgs = ValidCommArgs();
    CheckInt("memory expert mask rejected", ValidateMemoryDispatch(params, commArgs),
        TileXR::TILEXR_ERROR_NOT_SUPPORT);

    params = ValidV2Params();
    params.epWorldSize = 8;
    params.globalBs = 32;
    params.sharedExpertNum = 2;
    params.sharedExpertRankNum = 4;
    params.moeExpertNum = 8;
    commArgs = ValidCommArgs();
    commArgs.rankSize = 8;
    commArgs.localRankSize = 8;
    for (int rank = 0; rank < commArgs.rankSize; ++rank) {
        commArgs.peerMems[rank] = reinterpret_cast<GM_ADDR>(0x10000000 + rank * 0x01000000);
    }
    CheckInt("memory supports multiple ranks per shared expert",
        ValidateMemoryDispatch(params, commArgs), TileXR::TILEXR_SUCCESS);

    params.sharedExpertRankNum = 3;
    CheckInt("memory rejects uneven shared expert groups",
        ValidateMemoryDispatch(params, commArgs),
        TileXR::TILEXR_ERROR_PARA_CHECK_FAIL);

    commArgs = CrossNodeUdmaCommArgs();
    params = ValidParams();
    static uint8_t workspace[1024] = {};
    params.workspace = workspace;
    CheckInt("cross-node dispatch config", TileXREp::TileXREpValidateDispatchConfig(params, commArgs, &window),
        TileXR::TILEXR_SUCCESS);

    commArgs = ValidCommArgs();
    params = ValidParams();
    params.moeExpertNum = 7;
    CheckInt("non-divisible experts", TileXREp::TileXREpValidateDispatchConfig(params, commArgs, &window),
        TileXR::TILEXR_ERROR_PARA_CHECK_FAIL);
}

void TestCombineValidation()
{
    TileXREp::EpCombineParams params = ValidCombineParams();
    CheckInt("valid combine basic params", TileXREp::TileXREpValidateBasicCombineParams(params),
        TileXR::TILEXR_SUCCESS);

    params = ValidCombineParams();
    params.expertOut = nullptr;
    CheckInt("combine null expertOut", TileXREp::TileXREpValidateBasicCombineParams(params),
        TileXR::TILEXR_ERROR_PARA_CHECK_FAIL);

    params = ValidCombineParams();
    params.assistInfoForCombine = nullptr;
    CheckInt("combine null assist", TileXREp::TileXREpValidateBasicCombineParams(params),
        TileXR::TILEXR_ERROR_PARA_CHECK_FAIL);

    params = ValidCombineParams();
    params.epRecvCounts = nullptr;
    CheckInt("combine null recv counts", TileXREp::TileXREpValidateBasicCombineParams(params),
        TileXR::TILEXR_ERROR_PARA_CHECK_FAIL);

    params = ValidCombineParams();
    params.yOut = nullptr;
    CheckInt("combine null yOut", TileXREp::TileXREpValidateBasicCombineParams(params),
        TileXR::TILEXR_ERROR_PARA_CHECK_FAIL);

    params = ValidCombineParams();
    TileXR::CommArgs commArgs = ValidCommArgs();
    TileXREp::EpWindowConfig window {};
    CheckInt("valid combine config", TileXREp::TileXREpValidateCombineConfig(params, commArgs, &window),
        TileXR::TILEXR_SUCCESS);

    commArgs = ValidCommArgs();
    commArgs.peerMems[1] = nullptr;
    CheckInt("combine missing peer mem", TileXREp::TileXREpValidateCombineConfig(params, commArgs, &window),
        TileXR::TILEXR_ERROR_NOT_INITIALIZED);

    commArgs = CrossNodeCommArgs();
    CheckInt("cross-node combine needs udma registry", TileXREp::TileXREpValidateCombineConfig(params, commArgs,
        &window), TileXR::TILEXR_ERROR_NOT_INITIALIZED);

    commArgs = CrossNodeUdmaCommArgs();
    CheckInt("cross-node combine needs workspace", TileXREp::TileXREpValidateCombineConfig(params, commArgs, &window),
        TileXR::TILEXR_ERROR_NOT_INITIALIZED);

    params.workspace = reinterpret_cast<void *>(0x50000000);
    CheckInt("cross-node combine config", TileXREp::TileXREpValidateCombineConfig(params, commArgs, &window),
        TileXR::TILEXR_SUCCESS);

    TileXREp::EpMemoryCombineReferenceConfig memoryConfig {};
    params = ValidMemoryCombineMxfp8Params(3);
    commArgs = ValidCommArgs();
    CheckInt("memory combine supports mxfp8 e5m2",
        TileXREp::TileXREpValidateCombineMemoryConfig(params, commArgs, 48, &memoryConfig),
        TileXR::TILEXR_SUCCESS);

    commArgs.peerMemBytes = TileXREp::kEpMemoryRequiredPeerBytes - 1;
    CheckInt("memory combine rejects undersized peer memory",
        TileXREp::TileXREpValidateCombineMemoryConfig(params, commArgs, 48, &memoryConfig),
        TileXR::TILEXR_ERROR_PARA_CHECK_FAIL);
    commArgs = ValidCommArgs();

    params = ValidMemoryCombineMxfp8Params(4);
    CheckInt("memory combine supports mxfp8 e4m3",
        TileXREp::TileXREpValidateCombineMemoryConfig(params, commArgs, 48, &memoryConfig),
        TileXR::TILEXR_SUCCESS);

    params.expertScales = nullptr;
    CheckInt("memory combine mxfp8 requires expert scales",
        TileXREp::TileXREpValidateCombineMemoryConfig(params, commArgs, 48, &memoryConfig),
        TileXR::TILEXR_ERROR_PARA_CHECK_FAIL);

    params = ValidMemoryCombineMxfp8Params(2);
    CheckInt("memory combine rejects int8 communication quantization",
        TileXREp::TileXREpValidateCombineMemoryConfig(params, commArgs, 48, &memoryConfig),
        TileXR::TILEXR_ERROR_NOT_SUPPORT);
}

void TestV2CapabilityValidation()
{
    TileXREp::EpDispatchParams params = ValidV2Params();
    TileXR::CommArgs commArgs = ValidCommArgs();
    CheckInt("valid v2 params", TileXREp::TileXREpValidateDispatchV2Config(params, commArgs), TileXR::TILEXR_SUCCESS);

    params = ValidV2Params();
    params.tpWorldSize = 0;
    CheckInt("v2 no tp world zero", TileXREp::TileXREpValidateDispatchV2Config(params, commArgs),
        TileXR::TILEXR_SUCCESS);

    params = ValidV2Params();
    static int32_t tpRecvCountsOut[2] = {};
    params.tpRecvCountsOut = tpRecvCountsOut;
    CheckInt("v2 supports tp recv counts output for tp size one",
        TileXREp::TileXREpValidateDispatchV2Config(params, commArgs), TileXR::TILEXR_SUCCESS);

    params = ValidV2Params();
    params.expertTokenNumsType = 0;
    CheckInt("v2 supports prefix-sum expert token counts", TileXREp::TileXREpValidateDispatchV2Config(params, commArgs),
        TileXR::TILEXR_SUCCESS);

    params = ValidV2Params();
    params.expertTokenNumsType = 2;
    CheckInt("v2 rejects unsupported expert token count type", TileXREp::TileXREpValidateDispatchV2Config(params,
        commArgs), TileXR::TILEXR_ERROR_NOT_SUPPORT);

    params = ValidV2Params();
    params.quantMode = 1;
    CheckInt("v2 static quant requires int8 output", TileXREp::TileXREpValidateDispatchV2Config(params, commArgs),
        TileXR::TILEXR_ERROR_PARA_CHECK_FAIL);

    params = ValidStaticQuantParams();
    CheckInt("v2 supports static quant int8", TileXREp::TileXREpValidateDispatchV2Config(params, commArgs),
        TileXR::TILEXR_SUCCESS);

    params = ValidStaticQuantParams();
    params.scales = nullptr;
    CheckInt("v2 static quant requires scales", TileXREp::TileXREpValidateDispatchV2Config(params, commArgs),
        TileXR::TILEXR_ERROR_PARA_CHECK_FAIL);

    params = ValidPerTokenDynamicQuantParams();
    CheckInt("v2 supports per-token dynamic quant int8",
        TileXREp::TileXREpValidateDispatchV2Config(params, commArgs), TileXR::TILEXR_SUCCESS);

    params = ValidPerTokenDynamicQuantParams();
    params.dynamicScalesOut = nullptr;
    CheckInt("v2 per-token dynamic quant requires dynamic scales output",
        TileXREp::TileXREpValidateDispatchV2Config(params, commArgs), TileXR::TILEXR_ERROR_PARA_CHECK_FAIL);

    params = ValidPerTokenDynamicQuantParams();
    params.scales = reinterpret_cast<void *>(0x5000);
    CheckInt("v2 per-token dynamic quant does not take input scales",
        TileXREp::TileXREpValidateDispatchV2Config(params, commArgs), TileXR::TILEXR_ERROR_PARA_CHECK_FAIL);

    params = ValidV2Params();
    params.quantMode = 3;
    CheckInt("v2 rejects unsupported quant modes after per-token dynamic quant",
        TileXREp::TileXREpValidateDispatchV2Config(params, commArgs), TileXR::TILEXR_ERROR_NOT_SUPPORT);

    params = ValidV2Params();
    params.tpWorldSize = 2;
    params.epWorldSize = 1;
    static int32_t tpRecvCountsOutForTp[2] = {};
    params.tpRecvCountsOut = tpRecvCountsOutForTp;
    CheckInt("v2 supports same-node tp size two", TileXREp::TileXREpValidateDispatchV2Config(params, commArgs),
        TileXR::TILEXR_SUCCESS);

    params = ValidV2Params();
    params.tpWorldSize = 2;
    params.epWorldSize = 1;
    params.tpRankId = 2;
    params.tpRecvCountsOut = tpRecvCountsOutForTp;
    CheckInt("v2 rejects invalid tp rank", TileXREp::TileXREpValidateDispatchV2Config(params, commArgs),
        TileXR::TILEXR_ERROR_PARA_CHECK_FAIL);

    params = ValidV2Params();
    params.tpWorldSize = 2;
    params.epWorldSize = 1;
    params.tpRecvCountsOut = nullptr;
    CheckInt("v2 tp size two requires tp recv counts", TileXREp::TileXREpValidateDispatchV2Config(params, commArgs),
        TileXR::TILEXR_ERROR_PARA_CHECK_FAIL);

    params = ValidV2Params();
    static int32_t tpRecvCountsOutForTp4[4] = {};
    params.tpWorldSize = 4;
    params.epWorldSize = 1;
    params.globalBs = 16;
    params.tpRecvCountsOut = tpRecvCountsOutForTp4;
    commArgs.rankSize = 4;
    commArgs.localRankSize = 4;
    commArgs.peerMems[2] = reinterpret_cast<GM_ADDR>(0x30000000);
    commArgs.peerMems[3] = reinterpret_cast<GM_ADDR>(0x40000000);
    CheckInt("v2 supports same-node tp size four", TileXREp::TileXREpValidateDispatchV2Config(params, commArgs),
        TileXR::TILEXR_SUCCESS);

    params = ValidV2Params();
    params.sharedExpertNum = 1;
    CheckInt("v2 rejects shared expert", TileXREp::TileXREpValidateDispatchV2Config(params, commArgs),
        TileXR::TILEXR_ERROR_NOT_SUPPORT);

    params = ValidV2Params();
    params.epWorldSize = 4;
    params.globalBs = 16;
    params.sharedExpertNum = 1;
    params.sharedExpertRankNum = 1;
    params.moeExpertNum = 6;
    commArgs.rankSize = 4;
    CheckInt("v2 supports shared expert on leading ep ranks",
        TileXREp::TileXREpValidateDispatchV2Config(params, commArgs), TileXR::TILEXR_SUCCESS);

    params = ValidV2Params();
    static int32_t tpRecvCountsOutForSharedTp[8] = {};
    params.epWorldSize = 4;
    params.globalBs = 32;
    params.tpWorldSize = 2;
    params.tpRankId = 0;
    params.tpRecvCountsOut = tpRecvCountsOutForSharedTp;
    params.sharedExpertNum = 1;
    params.sharedExpertRankNum = 1;
    params.moeExpertNum = 6;
    commArgs.rankSize = 8;
    commArgs.localRankSize = 8;
    for (int rank = 0; rank < commArgs.rankSize; ++rank) {
        commArgs.peerMems[rank] = reinterpret_cast<GM_ADDR>(0x10000000 + rank * 0x10000000);
    }
    CheckInt("v2 supports tp with shared expert",
        TileXREp::TileXREpValidateDispatchV2Config(params, commArgs), TileXR::TILEXR_SUCCESS);

    params = ValidV2Params();
    params.expertShardType = 1;
    CheckInt("v2 rejects shard type", TileXREp::TileXREpValidateDispatchV2Config(params, commArgs),
        TileXR::TILEXR_ERROR_NOT_SUPPORT);

    params = ValidV2Params();
    params.epWorldSize = 4;
    CheckInt("v2 rejects ep world mismatch", TileXREp::TileXREpValidateDispatchV2Config(params, commArgs),
        TileXR::TILEXR_ERROR_PARA_CHECK_FAIL);

    params = ValidV2Params();
    params.epRankId = 1;
    CheckInt("v2 rejects ep rank mismatch", TileXREp::TileXREpValidateDispatchV2Config(params, commArgs),
        TileXR::TILEXR_ERROR_PARA_CHECK_FAIL);
}

} // namespace

int main()
{
    TestBasicValidation();
    TestCommValidation();
    TestCombineValidation();
    TestV2CapabilityValidation();
    return g_failures == 0 ? 0 : 1;
}
