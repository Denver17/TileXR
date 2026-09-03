#include "ep_host.h"

#include "comm_args.h"
#include "ep_window.h"
#include "tilexr_types.h"

namespace TileXREp {
namespace {

bool TileXREpUsesCrossNodeComm(const TileXR::CommArgs &commArgs)
{
    return commArgs.localRankSize > 0 && commArgs.localRankSize < commArgs.rankSize;
}

int64_t TileXREpEffectiveTpWorldSize(int64_t tpWorldSize)
{
    return tpWorldSize == 0 ? 1 : tpWorldSize;
}

int64_t TileXREpExpertRankSize(const TileXR::CommArgs &commArgs, int64_t tpWorldSize)
{
    const int64_t effectiveTpWorldSize = TileXREpEffectiveTpWorldSize(tpWorldSize);
    return effectiveTpWorldSize <= 0 ? 0 : commArgs.rankSize / effectiveTpWorldSize;
}

} // namespace

int TileXREpValidateBasicDispatchParams(const EpDispatchParams &params)
{
    if (params.x == nullptr || params.expertIds == nullptr || params.comm == nullptr || params.expandXOut == nullptr ||
        params.expertTokenNumsOut == nullptr || params.epRecvCountsOut == nullptr ||
        params.assistInfoForCombineOut == nullptr || params.stream == nullptr || params.bs <= 0 || params.h <= 0 ||
        params.topK <= 0 || params.moeExpertNum <= 0 || !TileXREpIsSupportedDataType(params.dtype)) {
        return TileXR::TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    return TileXR::TILEXR_SUCCESS;
}

int TileXREpValidateDispatchCapabilities(const EpDispatchParams &params, const TileXR::CommArgs &commArgs)
{
    if (params.expertShardType != 0) {
        return TileXR::TILEXR_ERROR_NOT_SUPPORT;
    }
    if (params.quantMode != 0 && params.quantMode != 1 && params.quantMode != 2) {
        return TileXR::TILEXR_ERROR_NOT_SUPPORT;
    }
    if (params.quantMode == 1 &&
        (params.scales == nullptr || params.dtype != TileXR::TILEXR_DATA_TYPE_INT8 ||
            params.dynamicScalesOut != nullptr || params.expandScalesOut != nullptr)) {
        return TileXR::TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    if (params.quantMode == 2 &&
        (params.scales != nullptr || params.dtype != TileXR::TILEXR_DATA_TYPE_INT8 ||
            params.dynamicScalesOut == nullptr || params.expandScalesOut != nullptr)) {
        return TileXR::TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    if (params.quantMode == 0 && !TileXREpIsSupportedDataType(params.dtype)) {
        return TileXR::TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    const int64_t effectiveTpWorldSize = TileXREpEffectiveTpWorldSize(params.tpWorldSize);
    if (params.tpRankId < 0 || params.tpRankId >= effectiveTpWorldSize) {
        return TileXR::TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    if (effectiveTpWorldSize == 1 && params.tpRankId != 0) {
        return TileXR::TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    if (effectiveTpWorldSize > 1) {
        if (params.tpRecvCountsOut == nullptr || commArgs.rankSize % effectiveTpWorldSize != 0 ||
            commArgs.rank % effectiveTpWorldSize != params.tpRankId) {
            return TileXR::TILEXR_ERROR_PARA_CHECK_FAIL;
        }
        const int64_t tpGroupStartRank = (commArgs.rank / effectiveTpWorldSize) * effectiveTpWorldSize;
        const int64_t tpGroupEndRank = tpGroupStartRank + effectiveTpWorldSize;
        if (tpGroupStartRank < 0 || tpGroupEndRank > commArgs.rankSize || commArgs.localRankSize <= 1) {
            return TileXR::TILEXR_ERROR_NOT_SUPPORT;
        }
        for (int64_t tpPeerRank = tpGroupStartRank; tpPeerRank < tpGroupEndRank; ++tpPeerRank) {
            if (commArgs.rank / commArgs.localRankSize != tpPeerRank / commArgs.localRankSize) {
                return TileXR::TILEXR_ERROR_NOT_SUPPORT;
            }
        }
    }
    if (params.sharedExpertNum < 0 || params.sharedExpertRankNum < 0) {
        return TileXR::TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    if ((params.sharedExpertNum == 0) != (params.sharedExpertRankNum == 0)) {
        return TileXR::TILEXR_ERROR_NOT_SUPPORT;
    }
    if (params.sharedExpertNum != params.sharedExpertRankNum) {
        return TileXR::TILEXR_ERROR_NOT_SUPPORT;
    }
    const int64_t expertRankSize = TileXREpExpertRankSize(commArgs, params.tpWorldSize);
    if (params.sharedExpertRankNum > expertRankSize) {
        return TileXR::TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    const int64_t moeRankNum = expertRankSize - params.sharedExpertRankNum;
    if (moeRankNum <= 0 || params.moeExpertNum % moeRankNum != 0) {
        return TileXR::TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    if (params.expertTokenNumsType != kEpExpertTokenNumsTypePrefixSum &&
        params.expertTokenNumsType != kEpExpertTokenNumsTypeCount) {
        return TileXR::TILEXR_ERROR_NOT_SUPPORT;
    }
    const int64_t expectedEpRankId = effectiveTpWorldSize == 1 ? commArgs.rank : commArgs.rank / effectiveTpWorldSize;
    if ((params.epWorldSize != 0 && params.epWorldSize != expertRankSize) ||
        (params.epRankId != 0 && params.epRankId != expectedEpRankId) ||
        (params.globalBs != 0 && params.globalBs != params.bs * commArgs.rankSize)) {
        return TileXR::TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    return TileXR::TILEXR_SUCCESS;
}

int TileXREpValidateDispatchConfig(const EpDispatchParams &params, const TileXR::CommArgs &commArgs,
    EpWindowConfig *window)
{
    if (window == nullptr || commArgs.rankSize <= 0 || commArgs.rankSize > TileXR::TILEXR_MAX_RANK_SIZE ||
        commArgs.rank < 0 || commArgs.rank >= commArgs.rankSize) {
        return TileXR::TILEXR_ERROR_PARA_CHECK_FAIL;
    }

    for (int rank = 0; rank < commArgs.rankSize; ++rank) {
        if (commArgs.peerMems[rank] == nullptr) {
            return TileXR::TILEXR_ERROR_NOT_INITIALIZED;
        }
    }

    if (TileXREpUsesCrossNodeComm(commArgs) &&
        (params.workspace == nullptr || (commArgs.extraFlag & TileXR::ExtraFlag::UDMA) == 0 ||
            commArgs.udmaInfoPtr == nullptr || commArgs.udmaRegistryPtr == nullptr)) {
        return TileXR::TILEXR_ERROR_NOT_INITIALIZED;
    }

    int ret = TileXREpValidateDispatchCapabilities(params, commArgs);
    if (ret != TileXR::TILEXR_SUCCESS) {
        return ret;
    }

    const int64_t payloadExtraBytes = params.quantMode == 2 ? static_cast<int64_t>(sizeof(float)) : 0;
    ret = TileXREpBuildDispatchWindowConfigForExpertRanks(commArgs.rankSize,
        TileXREpExpertRankSize(commArgs, params.tpWorldSize), params.bs, params.h, params.topK,
        params.moeExpertNum, params.sharedExpertNum, params.sharedExpertRankNum, params.dtype, payloadExtraBytes,
        window);
    if (ret != TileXR::TILEXR_SUCCESS) {
        return ret;
    }
    const int64_t effectiveTpWorldSize = TileXREpEffectiveTpWorldSize(params.tpWorldSize);
    if (effectiveTpWorldSize > 1 &&
        TileXREpAlignUp(window->totalBytes, kEpWindowAlignmentBytes) * (effectiveTpWorldSize + 2) >
            TileXR::IPC_BUFF_MAX_SIZE) {
        return TileXR::TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    return TileXR::TILEXR_SUCCESS;
}

int TileXREpValidateBasicCombineParams(const EpCombineParams &params)
{
    if (params.expertOut == nullptr || params.assistInfoForCombine == nullptr || params.epRecvCounts == nullptr ||
        params.comm == nullptr || params.yOut == nullptr || params.stream == nullptr || params.bs <= 0 ||
        params.h <= 0 || params.topK <= 0 || params.moeExpertNum <= 0 || !TileXREpIsSupportedDataType(params.dtype)) {
        return TileXR::TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    return TileXR::TILEXR_SUCCESS;
}

int TileXREpValidateCombineConfig(const EpCombineParams &params, const TileXR::CommArgs &commArgs,
    EpWindowConfig *window)
{
    if (window == nullptr || commArgs.rankSize <= 0 || commArgs.rankSize > TileXR::TILEXR_MAX_RANK_SIZE ||
        commArgs.rank < 0 || commArgs.rank >= commArgs.rankSize) {
        return TileXR::TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    if (TileXREpUsesCrossNodeComm(commArgs) &&
        (params.workspace == nullptr || (commArgs.extraFlag & TileXR::ExtraFlag::UDMA) == 0 ||
            commArgs.udmaInfoPtr == nullptr || commArgs.udmaRegistryPtr == nullptr)) {
        return TileXR::TILEXR_ERROR_NOT_INITIALIZED;
    }

    for (int rank = 0; rank < commArgs.rankSize; ++rank) {
        if (commArgs.peerMems[rank] == nullptr) {
            return TileXR::TILEXR_ERROR_NOT_INITIALIZED;
        }
    }

    return TileXREpBuildWindowConfig(commArgs.rankSize, params.bs, params.h, params.topK, params.moeExpertNum,
        params.dtype, window);
}

} // namespace TileXREp
