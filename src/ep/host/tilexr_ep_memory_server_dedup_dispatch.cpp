#include "tilexr_ep.h"

#include "ep_kernel_launch.h"
#include "ep_memory_server_dedup_launch.h"

namespace {

int DispatchServerDedup(const TileXREp::EpMemoryServerDedupParams &params)
{
    if (!TileXREp::TileXREpIsA5Soc()) {
        return TileXR::TILEXR_ERROR_NOT_SUPPORT;
    }
    int ret = TileXREp::TileXREpValidateBasicMemoryServerDedupParams(params);
    if (ret != TileXR::TILEXR_SUCCESS) {
        return ret;
    }
    TileXREp::EpHostLaunchContext context {};
    ret = TileXRGetCommArgsHost(params.dispatch.comm, context.hostArgs);
    if (ret != TileXR::TILEXR_SUCCESS) {
        return ret;
    }
    if (context.hostArgs == nullptr) {
        return TileXR::TILEXR_ERROR_NOT_INITIALIZED;
    }
    TileXREp::EpMemoryServerDedupLayout layout {};
    ret = TileXREp::TileXREpValidateMemoryServerDedupConfig(params, *context.hostArgs, &layout);
    if (ret != TileXR::TILEXR_SUCCESS) {
        return ret;
    }
    ret = TileXRGetCommArgsDev(params.dispatch.comm, context.devArgs);
    if (ret != TileXR::TILEXR_SUCCESS) {
        return ret;
    }
    if (context.devArgs == nullptr) {
        return TileXR::TILEXR_ERROR_NOT_INITIALIZED;
    }
    return TileXREp::TileXREpLaunchDispatchMemoryServerDedupKernel(params, context, layout);
}

} // namespace

int TileXRMoeEpDispatchMemoryServerDedup(void *x, int32_t *expertIds, void *scales, bool *xActiveMask,
    int64_t activeMaskType, void *expertScales, TileXRCommPtr comm, int64_t bs, int64_t h, int64_t topK,
    int64_t moeExpertNum, int64_t epWorldSize, int64_t epRankId, int64_t tpWorldSize, int64_t tpRankId,
    int64_t expertShardType, int64_t sharedExpertNum, int64_t sharedExpertRankNum, int64_t quantMode,
    int64_t globalBs, int64_t expertTokenNumsType, void *expandXOut, void *dynamicScalesOut,
    int32_t *assistInfoForCombineOut, int64_t *expertTokenNumsOut, int32_t *sendCountsOut,
    int32_t *tpRecvCountsOut, void *expandScalesOut, uint32_t *assistExOut, uint32_t *sourceMaskOut,
    uint32_t *groupOut, float *expertScalesOut, TileXR::TileXRDataType dtype, TileXR::TileXRDataType expandXOutDtype,
    aclrtStream stream)
{
    TileXREp::EpMemoryServerDedupParams params {};
    auto &p = params.dispatch;
    p.x = x;
    p.expertIds = expertIds;
    p.scales = scales;
    p.xActiveMask = xActiveMask;
    p.activeMaskType = activeMaskType;
    p.expertScales = expertScales;
    p.comm = comm;
    p.bs = bs;
    p.h = h;
    p.topK = topK;
    p.moeExpertNum = moeExpertNum;
    p.epWorldSize = epWorldSize;
    p.epRankId = epRankId;
    p.tpWorldSize = tpWorldSize;
    p.tpRankId = tpRankId;
    p.expertShardType = expertShardType;
    p.sharedExpertNum = sharedExpertNum;
    p.sharedExpertRankNum = sharedExpertRankNum;
    p.quantMode = quantMode;
    p.globalBs = globalBs;
    p.expertTokenNumsType = expertTokenNumsType;
    p.expandXOut = expandXOut;
    p.dynamicScalesOut = dynamicScalesOut;
    p.assistInfoForCombineOut = assistInfoForCombineOut;
    p.expertTokenNumsOut = expertTokenNumsOut;
    p.epRecvCountsOut = sendCountsOut;
    p.tpRecvCountsOut = tpRecvCountsOut;
    p.expandScalesOut = expandScalesOut;
    p.dtype = dtype;
    p.expandXOutDtype = expandXOutDtype;
    p.stream = stream;
    params.assistExOut = assistExOut;
    params.sourceMaskOut = sourceMaskOut;
    params.groupOut = groupOut;
    params.expertScalesOut = expertScalesOut;
    return DispatchServerDedup(params);
}
