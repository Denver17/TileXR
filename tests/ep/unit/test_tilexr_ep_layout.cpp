#include <cmath>
#include <cstdint>
#include <iostream>
#include <type_traits>
#include <vector>

#include "comm_args.h"
#include "ep_layout.h"
#include "ep_memory_layout.h"
#include "mxfp8_golden.h"
#include "tilexr_types.h"

namespace {

int g_failures = 0;

static_assert(std::is_same<std::underlying_type<TileXREpDemo::Mxfp8Format>::type, uint64_t>::value,
    "Mxfp8Format must use uint64_t as its underlying type");

void CheckInt64(const char *label, int64_t actual, int64_t expected)
{
    if (actual != expected) {
        std::cerr << label << " actual=" << actual << " expected=" << expected << std::endl;
        ++g_failures;
    }
}

void CheckInt(const char *label, int actual, int expected)
{
    if (actual != expected) {
        std::cerr << label << " actual=" << actual << " expected=" << expected << std::endl;
        ++g_failures;
    }
}

void CheckBool(const char *label, bool actual, bool expected)
{
    if (actual != expected) {
        std::cerr << label << " actual=" << actual << " expected=" << expected << std::endl;
        ++g_failures;
    }
}

void TestExpertMapping()
{
    CheckInt("expert 0 dst", TileXREp::TileXREpDstRank(0, 4), 0);
    CheckInt("expert 0 local", TileXREp::TileXREpLocalExpert(0, 4), 0);
    CheckInt("expert 3 dst", TileXREp::TileXREpDstRank(3, 4), 0);
    CheckInt("expert 3 local", TileXREp::TileXREpLocalExpert(3, 4), 3);
    CheckInt("expert 4 dst", TileXREp::TileXREpDstRank(4, 4), 1);
    CheckInt("expert 4 local", TileXREp::TileXREpLocalExpert(4, 4), 0);
    CheckInt("expert 7 dst", TileXREp::TileXREpDstRank(7, 4), 1);
    CheckInt("expert 7 local", TileXREp::TileXREpLocalExpert(7, 4), 3);
    CheckInt("negative expert dst", TileXREp::TileXREpDstRank(-1, 4), TileXR::TILEXR_INVALID_VALUE);
    CheckInt("zero local expert dst", TileXREp::TileXREpDstRank(1, 0), TileXR::TILEXR_INVALID_VALUE);
}

void TestDataTypes()
{
    CheckBool("fp16 supported", TileXREp::TileXREpIsSupportedDataType(TileXR::TILEXR_DATA_TYPE_FP16), true);
    CheckBool("bf16 supported", TileXREp::TileXREpIsSupportedDataType(TileXR::TILEXR_DATA_TYPE_BFP16), true);
    CheckBool("fp32 unsupported", TileXREp::TileXREpIsSupportedDataType(TileXR::TILEXR_DATA_TYPE_FP32), false);
    CheckInt64("fp16 bytes", TileXREp::TileXREpDataTypeSize(TileXR::TILEXR_DATA_TYPE_FP16), 2);
    CheckInt64("bf16 bytes", TileXREp::TileXREpDataTypeSize(TileXR::TILEXR_DATA_TYPE_BFP16), 2);
    CheckInt64("int32 bytes invalid", TileXREp::TileXREpDataTypeSize(TileXR::TILEXR_DATA_TYPE_INT32),
        TileXR::TILEXR_INVALID_VALUE);
}

void TestWindowConfig()
{
    TileXREp::EpWindowConfig config {};
    const int ret = TileXREp::TileXREpBuildWindowConfig(
        2, 4, 8, 2, 8, TileXR::TILEXR_DATA_TYPE_FP16, &config);
    CheckInt("valid config ret", ret, TileXR::TILEXR_SUCCESS);
    CheckInt64("rank size", config.rankSize, 2);
    CheckInt64("bs", config.bs, 4);
    CheckInt64("hidden size", config.h, 8);
    CheckInt64("topk", config.topK, 2);
    CheckInt64("moe experts", config.moeExpertNum, 8);
    CheckInt64("local experts", config.localExpertNum, 4);
    CheckInt64("dtype bytes", config.dtypeBytes, 2);
    CheckInt64("max routes", config.maxRoutesPerSrc, 8);
    CheckInt64("row bytes", config.rowBytes, 16);
    CheckInt64("payload bytes", config.payloadBytesPerSlot, 128);
    CheckInt64("assist bytes", config.assistBytesPerSlot, 128);
    CheckInt64("slot bytes", config.slotBytes, 320);
    CheckInt64("total bytes", config.totalBytes, 704);
}

void TestRejectsInvalidConfig()
{
    TileXREp::EpWindowConfig config {};
    CheckInt("null out", TileXREp::TileXREpBuildWindowConfig(
        2, 4, 8, 2, 8, TileXR::TILEXR_DATA_TYPE_FP16, nullptr),
        TileXR::TILEXR_ERROR_PARA_CHECK_FAIL);
    CheckInt("non-divisible experts", TileXREp::TileXREpBuildWindowConfig(
        2, 4, 8, 2, 7, TileXR::TILEXR_DATA_TYPE_FP16, &config),
        TileXR::TILEXR_ERROR_PARA_CHECK_FAIL);
    CheckInt("unsupported dtype", TileXREp::TileXREpBuildWindowConfig(
        2, 4, 8, 2, 8, TileXR::TILEXR_DATA_TYPE_FP32, &config),
        TileXR::TILEXR_ERROR_PARA_CHECK_FAIL);
    CheckInt("rank size too large", TileXREp::TileXREpBuildWindowConfig(
        TileXR::TILEXR_MAX_RANK_SIZE + 1, 4, 8, 2, TileXR::TILEXR_MAX_RANK_SIZE + 1,
        TileXR::TILEXR_DATA_TYPE_FP16, &config),
        TileXR::TILEXR_ERROR_PARA_CHECK_FAIL);
    CheckInt("oversized window", TileXREp::TileXREpBuildWindowConfig(
        2, 1024 * 1024, 64, 8, 8, TileXR::TILEXR_DATA_TYPE_FP16, &config),
        TileXR::TILEXR_ERROR_PARA_CHECK_FAIL);
}

void CheckUint8(const char *label, uint8_t actual, uint8_t expected)
{
    if (actual != expected) {
        std::cerr << label << " actual=0x" << std::hex << static_cast<int>(actual)
                  << " expected=0x" << static_cast<int>(expected) << std::dec << std::endl;
        ++g_failures;
    }
}

void CheckFloat(const char *label, float actual, float expected)
{
    if (std::fabs(actual - expected) > 1.0e-6f) {
        std::cerr << label << " actual=" << actual << " expected=" << expected << std::endl;
        ++g_failures;
    }
}

void TestMxfp8Golden()
{
    std::vector<float> e4Input(32, 0.0f);
    e4Input[0] = 1.0f;
    e4Input[1] = -1.0f;
    e4Input[2] = 448.0f;
    e4Input[3] = -448.0f;
    e4Input[4] = std::ldexp(1.0f, -9);
    e4Input[5] = std::ldexp(1.0f, -10);
    e4Input[6] = 3.0f * std::ldexp(1.0f, -10);
    const TileXREpDemo::Mxfp8Tensor e4 = TileXREpDemo::QuantizeMxfp8(
        e4Input, 1, e4Input.size(), TileXREpDemo::Mxfp8Format::E4M3);
    CheckInt64("mxfp8 e4 scale count", static_cast<int64_t>(e4.scaleCountPerRow), 2);
    CheckUint8("mxfp8 e4 one", e4.elements[0], 0x38);
    CheckUint8("mxfp8 e4 minus one", e4.elements[1], 0xb8);
    CheckUint8("mxfp8 e4 max", e4.elements[2], 0x7e);
    CheckUint8("mxfp8 e4 minus max", e4.elements[3], 0xfe);
    CheckUint8("mxfp8 e4 min subnormal", e4.elements[4], 0x01);
    CheckUint8("mxfp8 e4 tie to even zero", e4.elements[5], 0x00);
    CheckUint8("mxfp8 e4 tie to even two", e4.elements[6], 0x02);
    CheckUint8("mxfp8 e4 scale", e4.scales[0], 0x7f);
    CheckUint8("mxfp8 e4 padded scale", e4.scales[1], 0x00);

    std::vector<float> e5Input(32, 0.0f);
    e5Input[0] = 1.0f;
    e5Input[1] = -1.0f;
    e5Input[2] = 57344.0f;
    e5Input[3] = -57344.0f;
    e5Input[4] = std::ldexp(1.0f, -16);
    e5Input[5] = std::ldexp(1.0f, -17);
    e5Input[6] = 3.0f * std::ldexp(1.0f, -17);
    const TileXREpDemo::Mxfp8Tensor e5 = TileXREpDemo::QuantizeMxfp8(
        e5Input, 1, e5Input.size(), TileXREpDemo::Mxfp8Format::E5M2);
    CheckUint8("mxfp8 e5 one", e5.elements[0], 0x3c);
    CheckUint8("mxfp8 e5 minus one", e5.elements[1], 0xbc);
    CheckUint8("mxfp8 e5 max", e5.elements[2], 0x7b);
    CheckUint8("mxfp8 e5 minus max", e5.elements[3], 0xfb);
    CheckUint8("mxfp8 e5 min subnormal", e5.elements[4], 0x01);
    CheckUint8("mxfp8 e5 tie to even zero", e5.elements[5], 0x00);
    CheckUint8("mxfp8 e5 tie to even two", e5.elements[6], 0x02);
    CheckUint8("mxfp8 e5 scale", e5.scales[0], 0x7f);

    std::vector<float> blockInput(33, 1.0f);
    blockInput[32] = 2.0f;
    const TileXREpDemo::Mxfp8Tensor blocks = TileXREpDemo::QuantizeMxfp8(
        blockInput, 1, blockInput.size(), TileXREpDemo::Mxfp8Format::E4M3);
    CheckInt64("mxfp8 two block scale count", static_cast<int64_t>(blocks.scaleCountPerRow), 2);
    CheckUint8("mxfp8 first block scale", blocks.scales[0], 0x77);
    CheckUint8("mxfp8 second block scale", blocks.scales[1], 0x78);

    CheckFloat("mxfp8 e4 decode one",
        TileXREpDemo::DecodeFp8(0x38, TileXREpDemo::Mxfp8Format::E4M3), 1.0f);
    CheckFloat("mxfp8 e4 decode max",
        TileXREpDemo::DecodeFp8(0x7e, TileXREpDemo::Mxfp8Format::E4M3), 448.0f);
    CheckFloat("mxfp8 e5 decode min subnormal",
        TileXREpDemo::DecodeFp8(0x01, TileXREpDemo::Mxfp8Format::E5M2), std::ldexp(1.0f, -16));

    const std::vector<float> roundTrip = TileXREpDemo::DequantizeMxfp8(
        blocks, 1, blockInput.size(), TileXREpDemo::Mxfp8Format::E4M3);
    CheckInt64("mxfp8 round trip size", static_cast<int64_t>(roundTrip.size()), 33);
    CheckFloat("mxfp8 first block round trip", roundTrip[0], 1.0f);
    CheckFloat("mxfp8 second block round trip", roundTrip[32], 2.0f);
}

void TestMemoryCoreAllocation()
{
    CheckInt("small BS threshold", TileXREp::kEpMemorySmallBsThreshold, 48);
    CheckBool("47 is small BS", 47U < TileXREp::kEpMemorySmallBsThreshold, true);
    CheckBool("48 is not small BS", 48U < TileXREp::kEpMemorySmallBsThreshold, false);
    CheckInt("8 experts count cores", TileXREp::TileXREpMemoryCountCoreNum(8, 8, 48), 1);
    CheckInt("32 experts count cores", TileXREp::TileXREpMemoryCountCoreNum(32, 32, 48), 2);
    CheckInt("count cores capped at eight", TileXREp::TileXREpMemoryCountCoreNum(256, 128, 48), 8);
    CheckInt("count cores capped by receive states", TileXREp::TileXREpMemoryCountCoreNum(128, 2, 48), 2);
    CheckInt("one block rejected", TileXREp::TileXREpMemoryCountCoreNum(8, 8, 1), 0);
}

void TestMemoryCommunicatorRoundLayout()
{
    CheckInt64("memory round stride", TileXREp::kEpMemoryRoundStrideBytes,
        TileXR::IPC_DATA_OFFSET + TileXR::IPC_BUFF_MAX_SIZE);
    CheckInt64("memory required peer bytes", TileXREp::kEpMemoryRequiredPeerBytes,
        2 * (TileXR::IPC_DATA_OFFSET + TileXR::IPC_BUFF_MAX_SIZE));
    CheckInt64("memory magic one round", TileXREp::TileXREpMemoryRoundIndex(1), 1);
    CheckInt64("memory magic two round", TileXREp::TileXREpMemoryRoundIndex(2), 0);
    CheckInt64("memory magic three offset", TileXREp::TileXREpMemoryRoundDataOffset(3),
        TileXREp::kEpMemoryRoundStrideBytes + TileXR::IPC_DATA_OFFSET);
    CheckInt64("memory magic four offset", TileXREp::TileXREpMemoryRoundDataOffset(4),
        TileXR::IPC_DATA_OFFSET);
}

void TestMemoryWindowConfig()
{
    TileXREp::EpMemoryDispatchReferenceConfig config {};
    const int ret = TileXREp::TileXREpBuildMemoryDispatchReferenceConfig(
        2, 0, 4, 8, 2, 8, 0, 0, 8, TileXR::TILEXR_DATA_TYPE_FP16,
        TileXR::TILEXR_DATA_TYPE_FP16, 0, 48, &config);
    CheckInt("memory config ret", ret, TileXR::TILEXR_SUCCESS);
    CheckInt64("memory local experts", config.localExpertNum, 4);
    CheckInt64("memory receive states", config.rscvStatusNum, 8);
    CheckInt("memory block dim", config.blockDim, 48);
    CheckInt("memory all-to-all cores", config.aivUsedAllToAll, 47);
    CheckInt("memory count cores", config.aivUsedCumSum, 1);
    CheckInt("memory moe cores", config.moeUsedAivNum, 47);
    CheckInt("memory shared cores", config.sharedUsedAivNum, 0);
    CheckInt64("memory token slot", config.hCommuSize, 512);
    CheckInt64("memory expert segment", config.expertPerSizeOnWin, 2048);
    CheckInt64("memory combine reserve", config.combineReserveBytes, 4096);
    CheckInt64("memory workspace", config.workspaceBytes, 1536);
    CheckInt64("memory round payload", config.roundPayloadBytes, 1070596096);

    TileXREp::EpMemoryDispatchReferenceConfig smallBsConfig {};
    CheckInt("small BS config ret",
        TileXREp::TileXREpBuildMemoryDispatchReferenceConfig(
            64, 0, 47, 8, 7, 64, 0, 0, 3008, TileXR::TILEXR_DATA_TYPE_FP16,
            TileXR::TILEXR_DATA_TYPE_FP16, 0, 48, &smallBsConfig),
        TileXR::TILEXR_SUCCESS);
    CheckInt("small BS uses all-core count allocation", smallBsConfig.aivUsedCumSum, 2);

    TileXREp::EpMemoryDispatchReferenceConfig largeBsConfig {};
    CheckInt("large BS boundary config ret",
        TileXREp::TileXREpBuildMemoryDispatchReferenceConfig(
            64, 0, 48, 8, 2, 64, 0, 0, 3072, TileXR::TILEXR_DATA_TYPE_FP16,
            TileXR::TILEXR_DATA_TYPE_FP16, 0, 48, &largeBsConfig),
        TileXR::TILEXR_SUCCESS);
    CheckInt("large BS disables all-core count allocation", largeBsConfig.aivUsedCumSum, 4);

    const int sharedRet = TileXREp::TileXREpBuildMemoryDispatchReferenceConfig(
        8, 0, 4, 8, 2, 8, 2, 4, 32, TileXR::TILEXR_DATA_TYPE_FP16,
        TileXR::TILEXR_DATA_TYPE_FP16, 0, 48, &config);
    CheckInt("shared memory config ret", sharedRet, TileXR::TILEXR_SUCCESS);
    CheckInt64("shared rank local experts", config.localExpertNum, 1);
    CheckInt64("shared rank receive states", config.rscvStatusNum, 8);
    CheckInt("shared dispatch cores", config.sharedUsedAivNum, 23);
    CheckInt("shared moe cores", config.moeUsedAivNum, 24);
    CheckInt64("shared combine reserve", config.combineReserveBytes, 8192);

    TileXREp::EpMemoryDispatchReferenceConfig sharedRankConfig {};
    TileXREp::EpMemoryDispatchReferenceConfig moeRankConfig {};
    CheckInt("shared role config ret", TileXREp::TileXREpBuildMemoryDispatchReferenceConfig(
        8, 0, 4, 8, 2, 8, 2, 4, 32, TileXR::TILEXR_DATA_TYPE_FP16,
        TileXR::TILEXR_DATA_TYPE_FP16, 0, 48, &sharedRankConfig),
        TileXR::TILEXR_SUCCESS);
    CheckInt("moe role config ret", TileXREp::TileXREpBuildMemoryDispatchReferenceConfig(
        8, 4, 4, 8, 2, 8, 2, 4, 32, TileXR::TILEXR_DATA_TYPE_FP16,
        TileXR::TILEXR_DATA_TYPE_FP16, 0, 48, &moeRankConfig),
        TileXR::TILEXR_SUCCESS);
    CheckInt64("shared rank workspace reservation", sharedRankConfig.workspaceBytes, 1536);
    CheckInt64("moe rank workspace reservation", moeRankConfig.workspaceBytes, 3072);
    CheckInt64("shared and moe round payload match", sharedRankConfig.roundPayloadBytes,
        moeRankConfig.roundPayloadBytes);

    const int mxfp8Ret = TileXREp::TileXREpBuildMemoryDispatchReferenceConfig(
        2, 0, 4, 33, 2, 8, 0, 0, 8, TileXR::TILEXR_DATA_TYPE_FP16,
        TileXR::TILEXR_DATA_TYPE_FP8E4M3, 4, 48, &config);
    CheckInt("mxfp8 memory config ret", mxfp8Ret, TileXR::TILEXR_SUCCESS);
    CheckInt64("mxfp8 payload bytes", config.hOutSize, 33);
    CheckInt64("mxfp8 scale bytes", config.scaleOutBytes, 2);
    CheckInt64("mxfp8 triple offset", config.tokenQuantAlignBytes, 288);
    CheckInt64("mxfp8 token slot", config.hCommuSize, 512);

    TileXREp::EpMemoryCombineReferenceConfig combineConfig {};
    const int combineMxfp8Ret = TileXREp::TileXREpBuildMemoryCombineReferenceConfig(
        8, 0, 4, 1024, 2, 8, 0, 0, 32, TileXR::TILEXR_DATA_TYPE_FP16, 4, 48,
        &combineConfig);
    CheckInt("combine mxfp8 memory config ret", combineMxfp8Ret, TileXR::TILEXR_SUCCESS);
    CheckInt64("combine mxfp8 blocks", combineConfig.blockCntPerToken, 3);
    CheckInt64("combine mxfp8 packed row", combineConfig.packedRowBytes, 1536);
    CheckInt("combine mxfp8 receive buffers", combineConfig.receiveBufferNum, 2);
    CheckInt64("combine mxfp8 receive UB", combineConfig.receiveUbBytes, 20288);
    CheckInt64("combine mxfp8 send UB", combineConfig.sendUbBytes, 5344);

    TileXREp::EpMemoryDispatchReferenceConfig dispatchMxfp8Config {};
    const int dispatchMxfp8Ret = TileXREp::TileXREpBuildMemoryDispatchReferenceConfig(
        8, 0, 4, 1024, 2, 8, 0, 0, 32, TileXR::TILEXR_DATA_TYPE_FP16,
        TileXR::TILEXR_DATA_TYPE_FP8E4M3, 4, 48, &dispatchMxfp8Config);
    CheckInt("dispatch mxfp8 UB config ret", dispatchMxfp8Ret, TileXR::TILEXR_SUCCESS);
    CheckInt64("dispatch mxfp8 all-to-all UB", dispatchMxfp8Config.allToAllUbBytes, 12416);

    TileXREp::EpMemoryDispatchReferenceConfig dispatchNonQuantConfig {};
    const int dispatchNonQuantRet = TileXREp::TileXREpBuildMemoryDispatchReferenceConfig(
        8, 0, 4, 1024, 2, 8, 0, 0, 32, TileXR::TILEXR_DATA_TYPE_FP16,
        TileXR::TILEXR_DATA_TYPE_FP16, 0, 48, &dispatchNonQuantConfig);
    CheckInt("dispatch non-quant UB config ret", dispatchNonQuantRet, TileXR::TILEXR_SUCCESS);
    CheckInt64("dispatch non-quant all-to-all UB", dispatchNonQuantConfig.allToAllUbBytes, 12384);

    TileXREp::EpMemoryCombineReferenceConfig combineNonQuantConfig {};
    const int combineNonQuantRet = TileXREp::TileXREpBuildMemoryCombineReferenceConfig(
        8, 0, 4, 1024, 2, 8, 0, 0, 32, TileXR::TILEXR_DATA_TYPE_FP16, 0, 48,
        &combineNonQuantConfig);
    CheckInt("combine non-quant UB config ret", combineNonQuantRet, TileXR::TILEXR_SUCCESS);
    CheckInt64("combine non-quant send UB", combineNonQuantConfig.sendUbBytes, 5088);

    TileXREp::EpMemoryCombineReferenceConfig combineSharedMxfp8Config {};
    const int combineSharedMxfp8Ret = TileXREp::TileXREpBuildMemoryCombineReferenceConfig(
        2, 0, 4, 1024, 2, 1, 1, 1, 8, TileXR::TILEXR_DATA_TYPE_FP16, 4, 48,
        &combineSharedMxfp8Config);
    CheckInt("combine shared mxfp8 UB config ret", combineSharedMxfp8Ret, TileXR::TILEXR_SUCCESS);
    CheckInt64("combine shared mxfp8 receive UB", combineSharedMxfp8Config.receiveUbBytes, 20544);

    TileXREp::EpMemoryCombineReferenceConfig combineLargeMxfp8Config {};
    const int combineLargeMxfp8Ret = TileXREp::TileXREpBuildMemoryCombineReferenceConfig(
        16, 0, 256, 7168, 16, 64, 0, 0, 4096, TileXR::TILEXR_DATA_TYPE_FP16, 4, 200,
        &combineLargeMxfp8Config);
    CheckInt("combine large mxfp8 UB config ret", combineLargeMxfp8Ret, TileXR::TILEXR_SUCCESS);
    CheckInt64("combine large mxfp8 receive UB", combineLargeMxfp8Config.receiveUbBytes, 142368);

    const int combineInvalidQuantRet = TileXREp::TileXREpBuildMemoryCombineReferenceConfig(
        8, 0, 4, 1024, 2, 8, 0, 0, 32, TileXR::TILEXR_DATA_TYPE_FP16, 2, 48,
        &combineConfig);
    CheckInt("combine memory rejects int8 quant", combineInvalidQuantRet,
        TileXR::TILEXR_ERROR_PARA_CHECK_FAIL);

    TileXREp::EpMemoryDispatchReferenceConfig largeDispatchConfig {};
    const int largeDispatchRet = TileXREp::TileXREpBuildMemoryDispatchReferenceConfig(
        16, 0, 256, 3584, 16, 64, 0, 0, 4096, TileXR::TILEXR_DATA_TYPE_BFP16,
        TileXR::TILEXR_DATA_TYPE_BFP16, 0, 200, &largeDispatchConfig);
    CheckInt("large bf16 memory dispatch config ret", largeDispatchRet, TileXR::TILEXR_SUCCESS);
    CheckInt64("large bf16 memory dispatch token slot", largeDispatchConfig.hCommuSize, 7680);
    CheckInt64("large bf16 memory dispatch payload", largeDispatchConfig.combineReserveBytes +
        largeDispatchConfig.rscvStatusNum * largeDispatchConfig.expertPerSizeOnWin, 157286400);

    TileXREp::EpMemoryDispatchReferenceConfig fullRoundDispatchConfig {};
    const int fullRoundDispatchRet = TileXREp::TileXREpBuildMemoryDispatchReferenceConfig(
        16, 0, 256, 3584, 16, 128, 0, 0, 4096, TileXR::TILEXR_DATA_TYPE_BFP16,
        TileXR::TILEXR_DATA_TYPE_BFP16, 0, 200, &fullRoundDispatchConfig);
    CheckInt("dispatch accepts payload larger than old half", fullRoundDispatchRet, TileXR::TILEXR_SUCCESS);
    CheckInt64("dispatch full round payload usage", fullRoundDispatchConfig.combineReserveBytes +
        fullRoundDispatchConfig.rscvStatusNum * fullRoundDispatchConfig.expertPerSizeOnWin, 283115520);

    TileXREp::EpMemoryDispatchReferenceConfig dispatch64pNonQuantConfig {};
    const int dispatch64pNonQuantRet = TileXREp::TileXREpBuildMemoryDispatchReferenceConfig(
        64, 0, 94, 7168, 6, 384, 0, 0, 6016, TileXR::TILEXR_DATA_TYPE_BFP16,
        TileXR::TILEXR_DATA_TYPE_BFP16, 0, 64, &dispatch64pNonQuantConfig);
    CheckInt("64p non-quant memory dispatch config ret", dispatch64pNonQuantRet, TileXR::TILEXR_SUCCESS);
    CheckInt64("64p non-quant memory dispatch token slot", dispatch64pNonQuantConfig.hCommuSize, 15360);
    CheckInt64("64p non-quant memory dispatch payload",
        dispatch64pNonQuantConfig.combineReserveBytes +
            dispatch64pNonQuantConfig.rscvStatusNum * dispatch64pNonQuantConfig.expertPerSizeOnWin,
        563097600);

    TileXREp::EpMemoryCombineReferenceConfig largeCombineConfig {};
    const int largeCombineRet = TileXREp::TileXREpBuildMemoryCombineReferenceConfig(
        16, 0, 256, 3584, 16, 64, 0, 0, 4096, TileXR::TILEXR_DATA_TYPE_BFP16, 0, 200,
        &largeCombineConfig);
    CheckInt("large bf16 memory combine config ret", largeCombineRet, TileXR::TILEXR_SUCCESS);
    CheckInt64("large bf16 memory combine reserve", largeCombineConfig.combineReserveBytes, 31457280);

    TileXREp::EpMemoryCombineReferenceConfig fullRoundCombineConfig {};
    const int fullRoundCombineRet = TileXREp::TileXREpBuildMemoryCombineReferenceConfig(
        16, 0, 4096, 3584, 16, 64, 0, 0, 65536, TileXR::TILEXR_DATA_TYPE_BFP16, 0, 200,
        &fullRoundCombineConfig);
    CheckInt("combine accepts reserve larger than old half", fullRoundCombineRet, TileXR::TILEXR_SUCCESS);
    CheckInt64("combine full round reserve", fullRoundCombineConfig.combineReserveBytes, 503316480);
}

void TestMemoryWindowRejectsInvalidConfig()
{
    TileXREp::EpMemoryDispatchReferenceConfig config {};
    CheckInt("memory null out", TileXREp::TileXREpBuildMemoryDispatchReferenceConfig(
        2, 0, 4, 8, 2, 8, 0, 0, 8, TileXR::TILEXR_DATA_TYPE_FP16,
        TileXR::TILEXR_DATA_TYPE_FP16, 0, 48, nullptr),
        TileXR::TILEXR_ERROR_PARA_CHECK_FAIL);
    CheckInt("memory one block", TileXREp::TileXREpBuildMemoryDispatchReferenceConfig(
        2, 0, 4, 8, 2, 8, 0, 0, 8, TileXR::TILEXR_DATA_TYPE_FP16,
        TileXR::TILEXR_DATA_TYPE_FP16, 0, 1, &config),
        TileXR::TILEXR_ERROR_PARA_CHECK_FAIL);
    CheckInt("memory too many blocks", TileXREp::TileXREpBuildMemoryDispatchReferenceConfig(
        2, 0, 4, 8, 2, 8, 0, 0, 8, TileXR::TILEXR_DATA_TYPE_FP16,
        TileXR::TILEXR_DATA_TYPE_FP16, 0, 201, &config),
        TileXR::TILEXR_ERROR_PARA_CHECK_FAIL);
    CheckInt("memory int8 rejected", TileXREp::TileXREpBuildMemoryDispatchReferenceConfig(
        2, 0, 4, 8, 2, 8, 0, 0, 8, TileXR::TILEXR_DATA_TYPE_INT8,
        TileXR::TILEXR_DATA_TYPE_INT8, 0, 48, &config),
        TileXR::TILEXR_ERROR_PARA_CHECK_FAIL);
    CheckInt("memory uneven shared groups", TileXREp::TileXREpBuildMemoryDispatchReferenceConfig(
        8, 0, 4, 8, 2, 8, 2, 3, 32, TileXR::TILEXR_DATA_TYPE_FP16,
        TileXR::TILEXR_DATA_TYPE_FP16, 0, 48, &config),
        TileXR::TILEXR_ERROR_PARA_CHECK_FAIL);
    CheckInt("memory oversized", TileXREp::TileXREpBuildMemoryDispatchReferenceConfig(
        8, 4, 4096, 7168, 8, 64, 0, 0, 32768,
        TileXR::TILEXR_DATA_TYPE_FP16, TileXR::TILEXR_DATA_TYPE_FP16, 0, 48, &config),
        TileXR::TILEXR_ERROR_PARA_CHECK_FAIL);
    CheckInt("memory oversized ub", TileXREp::TileXREpBuildMemoryDispatchReferenceConfig(
        2, 0, 1, 60000, 1, 2, 0, 0, 2,
        TileXR::TILEXR_DATA_TYPE_FP16, TileXR::TILEXR_DATA_TYPE_FP16, 0, 48, &config),
        TileXR::TILEXR_ERROR_PARA_CHECK_FAIL);

    TileXREp::EpMemoryCombineReferenceConfig combineConfig {};
    CheckInt("combine aligned polling buffers exceed UB",
        TileXREp::TileXREpBuildMemoryCombineReferenceConfig(
            2, 0, 193377, 1, 1, 2, 0, 0, 386754,
            TileXR::TILEXR_DATA_TYPE_FP16, 0, 48, &combineConfig),
        TileXR::TILEXR_ERROR_PARA_CHECK_FAIL);
}

} // namespace

int main()
{
    TestMxfp8Golden();
    TestExpertMapping();
    TestDataTypes();
    TestWindowConfig();
    TestRejectsInvalidConfig();
    TestMemoryCoreAllocation();
    TestMemoryCommunicatorRoundLayout();
    TestMemoryWindowConfig();
    TestMemoryWindowRejectsInvalidConfig();
    return g_failures == 0 ? 0 : 1;
}
