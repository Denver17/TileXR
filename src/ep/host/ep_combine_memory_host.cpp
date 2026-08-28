#include "ep_host.h"

#include <cstdint>
#include <limits>

#include "comm_args.h"
#include "ep_memory_layout.h"
#include "tilexr_types.h"

namespace TileXREp {
namespace {

bool ResolveGlobalBs(const EpCombineParams &params, const TileXR::CommArgs &commArgs, int64_t *globalBs)
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

int ValidateMask(const EpCombineParams &params)
{
    if (params.activeMaskType == TILEXR_EP_ACTIVE_MASK_NONE) {
        return params.xActiveMask == nullptr ? TileXR::TILEXR_SUCCESS : TileXR::TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    if (params.activeMaskType == TILEXR_EP_ACTIVE_MASK_TOKEN) {
        return params.xActiveMask != nullptr ? TileXR::TILEXR_SUCCESS : TileXR::TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    if (params.activeMaskType == TILEXR_EP_ACTIVE_MASK_EXPERT) {
        return TileXR::TILEXR_ERROR_NOT_SUPPORT;
    }
    return TileXR::TILEXR_ERROR_PARA_CHECK_FAIL;
}

} // namespace

int TileXREpValidateCombineMemoryConfig(const EpCombineParams &params, const TileXR::CommArgs &commArgs,
    uint32_t blockDim, EpMemoryCombineReferenceConfig *config)
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

    const int64_t effectiveTpWorldSize = params.tpWorldSize == 0 ? 1 : params.tpWorldSize;
    const bool useMxfp8 = params.quantMode == 3 || params.quantMode == 4;
    if (effectiveTpWorldSize != 1 || params.tpRankId != 0 ||
        (params.quantMode != 0 && !useMxfp8)) {
        return TileXR::TILEXR_ERROR_NOT_SUPPORT;
    }
    if (useMxfp8 && params.expertScales == nullptr) {
        return TileXR::TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    if (params.expertShardType != 0 ||
        (params.epWorldSize != 0 && params.epWorldSize != commArgs.rankSize) ||
        (params.epRankId != 0 && params.epRankId != commArgs.rank)) {
        return TileXR::TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    const int maskRet = ValidateMask(params);
    if (maskRet != TileXR::TILEXR_SUCCESS) {
        return maskRet;
    }

    int64_t globalBs = 0;
    if (!ResolveGlobalBs(params, commArgs, &globalBs)) {
        return TileXR::TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    return TileXREpBuildMemoryCombineReferenceConfig(commArgs.rankSize, commArgs.rank, params.bs, params.h,
        params.topK, params.moeExpertNum, params.sharedExpertNum, params.sharedExpertRankNum, globalBs,
        params.dtype, params.quantMode, blockDim, config);
}

int TileXREpPrepareMemoryCombineLaunchContext(const EpCombineParams &params, EpHostLaunchContext *context)
{
    if (context == nullptr) {
        return TileXR::TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    *context = EpHostLaunchContext {};
    int ret = TileXRGetCommArgsHost(params.comm, context->hostArgs);
    if (ret != TileXR::TILEXR_SUCCESS || context->hostArgs == nullptr) {
        *context = EpHostLaunchContext {};
        return ret == TileXR::TILEXR_SUCCESS ? TileXR::TILEXR_ERROR_NOT_INITIALIZED : ret;
    }
    ret = TileXRGetCommArgsDev(params.comm, context->devArgs);
    if (ret != TileXR::TILEXR_SUCCESS || context->devArgs == nullptr) {
        *context = EpHostLaunchContext {};
        return ret == TileXR::TILEXR_SUCCESS ? TileXR::TILEXR_ERROR_NOT_INITIALIZED : ret;
    }
    return TileXR::TILEXR_SUCCESS;
}

} // namespace TileXREp
