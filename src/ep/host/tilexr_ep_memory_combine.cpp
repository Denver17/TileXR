#include "tilexr_ep.h"

#ifndef TILEXR_EP_MEMORY_SUPPORTED
#define TILEXR_EP_MEMORY_SUPPORTED 1
#endif

#if TILEXR_EP_MEMORY_SUPPORTED
#include "ep_host.h"
#include "ep_kernel_launch.h"
#endif
#include "tilexr_types.h"

#if TILEXR_EP_MEMORY_SUPPORTED
namespace {

int LaunchEpCombineMemory(const TileXREp::EpCombineParams &params)
{
    int ret = TileXREp::TileXREpValidateBasicCombineParams(params);
    if (ret != TileXR::TILEXR_SUCCESS) {
        return ret;
    }
    TileXREp::EpHostLaunchContext context {};
    ret = TileXREp::TileXREpPrepareMemoryCombineLaunchContext(params, &context);
    if (ret != TileXR::TILEXR_SUCCESS) {
        return ret;
    }
    return TileXREp::TileXREpLaunchCombineMemoryKernel(params, context);
}

} // namespace
#endif

int TileXRMoeEpCombineMemory(void *expertOut, int32_t *assistInfoForCombine, int32_t *sendCounts,
    float *expertScales, bool *xActiveMask, int64_t activeMaskType, void *sharedExpertX,
    TileXRCommPtr comm, int64_t bs, int64_t h, int64_t topK, int64_t moeExpertNum,
    int64_t epWorldSize, int64_t epRankId, int64_t tpWorldSize, int64_t tpRankId,
    int64_t expertShardType, int64_t sharedExpertNum, int64_t sharedExpertRankNum,
    int64_t quantMode, int64_t globalBs, void *yOut, TileXR::TileXRDataType dtype, aclrtStream stream)
{
#if !TILEXR_EP_MEMORY_SUPPORTED
    return TileXR::TILEXR_ERROR_NOT_SUPPORT;
#else
    TileXREp::EpCombineParams params {};
    params.expertOut = expertOut;
    params.assistInfoForCombine = assistInfoForCombine;
    params.epRecvCounts = sendCounts;
    params.expertScales = expertScales;
    params.xActiveMask = xActiveMask;
    params.activeMaskType = activeMaskType;
    params.sharedExpertX = sharedExpertX;
    params.comm = comm;
    params.bs = bs;
    params.h = h;
    params.topK = topK;
    params.moeExpertNum = moeExpertNum;
    params.epWorldSize = epWorldSize;
    params.epRankId = epRankId;
    params.tpWorldSize = tpWorldSize;
    params.tpRankId = tpRankId;
    params.expertShardType = expertShardType;
    params.sharedExpertNum = sharedExpertNum;
    params.sharedExpertRankNum = sharedExpertRankNum;
    params.quantMode = quantMode;
    params.globalBs = globalBs;
    params.yOut = yOut;
    params.workspace = nullptr;
    params.dtype = dtype;
    params.stream = stream;
    return LaunchEpCombineMemory(params);
#endif
}
