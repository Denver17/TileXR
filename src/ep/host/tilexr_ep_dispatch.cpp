#include "tilexr_ep.h"

#include "ep_host.h"
#include "ep_kernel_launch.h"

namespace {

int LaunchEpDispatch(const TileXREp::EpDispatchParams &params)
{
    if (!TileXREp::TileXREpIsA5Soc()) {
        return TileXR::TILEXR_ERROR_NOT_SUPPORT;
    }
    int ret = TileXREp::TileXREpValidateBasicDispatchParams(params);
    if (ret != TileXR::TILEXR_SUCCESS) {
        return ret;
    }

    TileXREp::EpHostLaunchContext context {};
    ret = TileXREp::TileXREpPrepareLaunchContext(params, &context);
    if (ret != TileXR::TILEXR_SUCCESS) {
        return ret;
    }

    return TileXREp::TileXREpLaunchDispatchKernel(params, context);
}

int LaunchEpCombine(const TileXREp::EpCombineParams &params)
{
    if (!TileXREp::TileXREpIsA5Soc()) {
        return TileXR::TILEXR_ERROR_NOT_SUPPORT;
    }
    int ret = TileXREp::TileXREpValidateBasicCombineParams(params);
    if (ret != TileXR::TILEXR_SUCCESS) {
        return ret;
    }

    TileXREp::EpHostLaunchContext context {};
    ret = TileXREp::TileXREpPrepareCombineLaunchContext(params, &context);
    if (ret != TileXR::TILEXR_SUCCESS) {
        return ret;
    }

    return TileXREp::TileXREpLaunchCombineKernel(params, context);
}

} // namespace

int TileXRMoeEpDispatch(void *x, int32_t *expertIds, void *scales, bool *xActiveMask, void *expertScales,
    TileXRCommPtr comm, int64_t bs, int64_t h, int64_t topK, int64_t moeExpertNum, int64_t epWorldSize,
    int64_t epRankId, int64_t tpWorldSize, int64_t tpRankId, int64_t expertShardType, int64_t sharedExpertNum,
    int64_t sharedExpertRankNum, int64_t quantMode, int64_t globalBs, int64_t expertTokenNumsType, void *expandXOut,
    void *dynamicScalesOut, int32_t *assistInfoForCombineOut, int64_t *expertTokenNumsOut, int32_t *epRecvCountsOut,
    int32_t *tpRecvCountsOut, void *expandScalesOut, void *workspace, TileXR::TileXRDataType dtype,
    aclrtStream stream)
{
    TileXREp::EpDispatchParams params {};
    params.x = x;
    params.expertIds = expertIds;
    params.scales = scales;
    params.xActiveMask = xActiveMask;
    params.expertScales = expertScales;
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
    params.expertTokenNumsType = expertTokenNumsType;
    params.expandXOut = expandXOut;
    params.dynamicScalesOut = dynamicScalesOut;
    params.assistInfoForCombineOut = assistInfoForCombineOut;
    params.expertTokenNumsOut = expertTokenNumsOut;
    params.epRecvCountsOut = epRecvCountsOut;
    params.tpRecvCountsOut = tpRecvCountsOut;
    params.expandScalesOut = expandScalesOut;
    params.workspace = workspace;
    params.dtype = dtype;
    params.stream = stream;

    return LaunchEpDispatch(params);
}

int TileXRMoeEpCombine(void *expertOut, int32_t *assistInfoForCombine, int32_t *epRecvCounts,
    TileXRCommPtr comm, int64_t bs, int64_t h, int64_t topK, int64_t moeExpertNum,
    void *yOut, void *workspace, TileXR::TileXRDataType dtype, aclrtStream stream)
{
    TileXREp::EpCombineParams params {};
    params.expertOut = expertOut;
    params.assistInfoForCombine = assistInfoForCombine;
    params.epRecvCounts = epRecvCounts;
    params.comm = comm;
    params.bs = bs;
    params.h = h;
    params.topK = topK;
    params.moeExpertNum = moeExpertNum;
    params.yOut = yOut;
    params.workspace = workspace;
    params.dtype = dtype;
    params.stream = stream;

    return LaunchEpCombine(params);
}
