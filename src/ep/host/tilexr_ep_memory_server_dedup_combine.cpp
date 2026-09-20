#include "ep_memory_server_dedup_combine_host.h"
#include "ep_kernel_launch.h"

namespace {
int CombineServerDedup(const TileXREp::EpMemoryServerDedupCombineParams &params)
{
    if (!TileXREp::TileXREpIsA5Soc()) { return TileXR::TILEXR_ERROR_NOT_SUPPORT; }
    int ret = TileXREp::TileXREpValidateBasicMemoryServerDedupCombineParams(params);
    if (ret != TileXR::TILEXR_SUCCESS) { return ret; }
    TileXREp::EpHostLaunchContext context {};
    ret = TileXRGetCommArgsHost(params.combine.comm, context.hostArgs);
    if (ret != TileXR::TILEXR_SUCCESS) { return ret; }
    if (context.hostArgs == nullptr) { return TileXR::TILEXR_ERROR_NOT_INITIALIZED; }
    TileXREp::EpMemoryServerDedupCombineLayout layout {};
    ret = TileXREp::TileXREpValidateMemoryServerDedupCombineConfig(params, *context.hostArgs, &layout);
    if (ret != TileXR::TILEXR_SUCCESS) { return ret; }
    ret = TileXRGetCommArgsDev(params.combine.comm, context.devArgs);
    if (ret != TileXR::TILEXR_SUCCESS) { return ret; }
    if (context.devArgs == nullptr) { return TileXR::TILEXR_ERROR_NOT_INITIALIZED; }
    return TileXREp::TileXREpLaunchCombineMemoryServerDedupKernel(params, context, layout);
}
} // namespace

int TileXRMoeEpCombineMemoryServerDedup(void *expertOut, int32_t *assistInfoForCombine, int32_t *sendCounts,
    float *expertScales, bool *xActiveMask, int64_t activeMaskType, void *sharedExpertX,
    TileXRCommPtr comm, int64_t bs, int64_t h, int64_t topK, int64_t moeExpertNum,
    int64_t epWorldSize, int64_t epRankId, int64_t tpWorldSize, int64_t tpRankId,
    int64_t expertShardType, int64_t sharedExpertNum, int64_t sharedExpertRankNum,
    int64_t quantMode, int64_t globalBs, void *yOut, TileXR::TileXRDataType dtype, aclrtStream stream,
    const uint32_t *tokenType, const void *relayReadIndex, const uint32_t *sourceMask)
{
    TileXREp::EpMemoryServerDedupCombineParams params {};
    auto &p = params.combine;
    p.expertOut = expertOut;
    p.assistInfoForCombine = assistInfoForCombine;
    p.expertScales = expertScales;
    p.xActiveMask = xActiveMask;
    p.activeMaskType = activeMaskType;
    p.sharedExpertX = sharedExpertX;
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
    p.yOut = yOut;
    p.dtype = dtype;
    p.stream = stream;
    p.epRecvCounts = sendCounts; // Preserve the complete inclusive-prefix array.
    params.tokenType = tokenType;
    params.relayReadIndex = relayReadIndex;
    params.sourceMask = sourceMask;
    return CombineServerDedup(params);
}
