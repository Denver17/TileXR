#include "ep_host.h"

#include <cstdint>
#include <limits>

#include "comm_args.h"
#include "ep_memory_layout.h"
#include "tilexr_types.h"

namespace TileXREp {
namespace {

int64_t TileXREpEffectiveTpWorldSize(int64_t tpWorldSize)
{
    return tpWorldSize == 0 ? 1 : tpWorldSize;
}

TileXR::TileXRDataType TileXREpMemoryExpandXOutDtype(const EpDispatchParams &params)
{
    return params.expandXOutDtype == TileXR::TILEXR_DATA_TYPE_RESERVED ? params.dtype : params.expandXOutDtype;
}

bool TileXREpMemoryGlobalBs(const EpDispatchParams &params, const TileXR::CommArgs &commArgs, int64_t *globalBs)
{
    if (globalBs == nullptr || params.bs <= 0 || commArgs.rankSize <= 0 ||
        params.bs > std::numeric_limits<int64_t>::max() / commArgs.rankSize) {
        return false;
    }
    const int64_t expected = params.bs * commArgs.rankSize;
    if (params.globalBs != 0 && params.globalBs != expected) {
        return false;
    }
    *globalBs = expected;
    return true;
}

int TileXREpValidateMemoryMask(const EpDispatchParams &params)
{
    if (params.activeMaskType == TILEXR_EP_ACTIVE_MASK_EXPERT) {
        return TileXR::TILEXR_ERROR_NOT_SUPPORT;
    }
    if (params.activeMaskType == TILEXR_EP_ACTIVE_MASK_NONE) {
        return params.xActiveMask == nullptr ? TileXR::TILEXR_SUCCESS : TileXR::TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    if (params.activeMaskType == TILEXR_EP_ACTIVE_MASK_TOKEN) {
        return params.xActiveMask != nullptr ? TileXR::TILEXR_SUCCESS : TileXR::TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    return TileXR::TILEXR_ERROR_PARA_CHECK_FAIL;
}

} // namespace

int TileXREpValidateDispatchMemoryConfig(const EpDispatchParams &params, const TileXR::CommArgs &commArgs,
    uint32_t blockDim, EpMemoryDispatchReferenceConfig *config)
{
    if (config == nullptr || commArgs.rankSize <= 0 || commArgs.rankSize > TileXR::TILEXR_MAX_RANK_SIZE ||
        commArgs.rank < 0 || commArgs.rank >= commArgs.rankSize ||
        commArgs.peerMemBytes < kEpMemoryRequiredPeerBytes) {
        return TileXR::TILEXR_ERROR_PARA_CHECK_FAIL;
    }

    for (int rank = 0; rank < commArgs.rankSize; ++rank) {
        if (commArgs.peerMems[rank] == nullptr) {
            return TileXR::TILEXR_ERROR_NOT_INITIALIZED;
        }
    }

    const int64_t effectiveTpWorldSize = TileXREpEffectiveTpWorldSize(params.tpWorldSize);
    if (effectiveTpWorldSize != 1 || params.tpRankId != 0 || params.tpRecvCountsOut != nullptr ||
        params.scales != nullptr || params.expertScales != nullptr || params.expandScalesOut != nullptr) {
        return TileXR::TILEXR_ERROR_NOT_SUPPORT;
    }
    const TileXR::TileXRDataType expandXOutDtype = TileXREpMemoryExpandXOutDtype(params);
    if (params.quantMode == 0) {
        if (params.dynamicScalesOut != nullptr || expandXOutDtype != params.dtype) {
            return TileXR::TILEXR_ERROR_PARA_CHECK_FAIL;
        }
    } else if (params.quantMode == 4) {
        if (params.dynamicScalesOut == nullptr ||
            (expandXOutDtype != TileXR::TILEXR_DATA_TYPE_FP8E4M3 &&
                expandXOutDtype != TileXR::TILEXR_DATA_TYPE_FP8E5M2)) {
            return TileXR::TILEXR_ERROR_PARA_CHECK_FAIL;
        }
    } else {
        return TileXR::TILEXR_ERROR_NOT_SUPPORT;
    }
    if (params.expertShardType != 0 || params.expertTokenNumsType < kEpExpertTokenNumsTypePrefixSum ||
        params.expertTokenNumsType > kEpExpertTokenNumsTypeCount ||
        (params.epWorldSize != 0 && params.epWorldSize != commArgs.rankSize) ||
        (params.epRankId != 0 && params.epRankId != commArgs.rank)) {
        return TileXR::TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    const int maskRet = TileXREpValidateMemoryMask(params);
    if (maskRet != TileXR::TILEXR_SUCCESS) {
        return maskRet;
    }
    int64_t globalBs = 0;
    if (!TileXREpMemoryGlobalBs(params, commArgs, &globalBs)) {
        return TileXR::TILEXR_ERROR_PARA_CHECK_FAIL;
    }

    const int ret = TileXREpBuildMemoryDispatchReferenceConfig(commArgs.rankSize, commArgs.rank, params.bs,
        params.h, params.topK, params.moeExpertNum, params.sharedExpertNum, params.sharedExpertRankNum,
        globalBs, params.dtype, expandXOutDtype, params.quantMode, blockDim, config);
    if (ret != TileXR::TILEXR_SUCCESS) {
        return ret;
    }
    return TileXR::TILEXR_SUCCESS;
}

int TileXREpPrepareMemoryLaunchContext(const EpDispatchParams &params, EpHostLaunchContext *context)
{
    if (context == nullptr) {
        return TileXR::TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    *context = EpHostLaunchContext {};

    int ret = TileXRGetCommArgsHost(params.comm, context->hostArgs);
    if (ret != TileXR::TILEXR_SUCCESS) {
        return ret;
    }
    if (context->hostArgs == nullptr) {
        *context = EpHostLaunchContext {};
        return TileXR::TILEXR_ERROR_NOT_INITIALIZED;
    }

    ret = TileXRGetCommArgsDev(params.comm, context->devArgs);
    if (ret != TileXR::TILEXR_SUCCESS) {
        *context = EpHostLaunchContext {};
        return ret;
    }
    if (context->devArgs == nullptr) {
        *context = EpHostLaunchContext {};
        return TileXR::TILEXR_ERROR_NOT_INITIALIZED;
    }

    return TileXR::TILEXR_SUCCESS;
}

} // namespace TileXREp
