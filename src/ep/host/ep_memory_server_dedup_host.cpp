#include "ep_memory_server_dedup_host.h"

#include "ep_memory_server_dedup_protocol.h"
#include "ep_memory_server_dedup_profile.h"

namespace TileXREp {
namespace {

bool IsAligned(const void *pointer, uint32_t alignment)
{
    return pointer != nullptr && reinterpret_cast<uintptr_t>(pointer) % alignment == 0;
}

} // namespace

int TileXREpValidateBasicMemoryServerDedupParams(const EpMemoryServerDedupParams &params)
{
    const auto &p = params.dispatch;
    if (p.comm == nullptr || p.stream == nullptr || p.bs <= 0 || p.h <= 0 ||
        p.topK <= 0 || p.topK > kEpServerDedupMaxTopK || p.moeExpertNum <= 0 ||
        !IsAligned(p.x, 2U) || !IsAligned(p.expertIds, 4U) || !IsAligned(p.expandXOut, 2U) ||
        !IsAligned(p.expertTokenNumsOut, 8U) || !IsAligned(p.epRecvCountsOut, 4U) ||
        !IsAligned(params.assistExOut, 32U) || !IsAligned(params.sourceMaskOut, 4U) ||
        !IsAligned(params.groupOut, 32U) ||
        !IsAligned(p.expertScales, 4U) || !IsAligned(params.expertScalesOut, 4U)) {
        return TileXR::TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    if ((p.dtype != TileXR::TILEXR_DATA_TYPE_FP16 && p.dtype != TileXR::TILEXR_DATA_TYPE_BFP16) ||
        (p.quantMode != 0 && p.quantMode != 4) || p.activeMaskType != TILEXR_EP_ACTIVE_MASK_NONE || p.xActiveMask != nullptr ||
        p.sharedExpertNum != 0 || p.sharedExpertRankNum != 0 ||
        (p.tpWorldSize != 0 && p.tpWorldSize != 1) || p.tpRankId != 0 || p.expertShardType != 0 ||
        p.scales != nullptr || p.tpRecvCountsOut != nullptr ||
        p.expandScalesOut != nullptr) {
        return TileXR::TILEXR_ERROR_NOT_SUPPORT;
    }
    const bool validOutput = p.quantMode == 4 ?
        ((p.expandXOutDtype == TileXR::TILEXR_DATA_TYPE_FP8E4M3 || p.expandXOutDtype == TileXR::TILEXR_DATA_TYPE_FP8E5M2) &&
         p.dynamicScalesOut != nullptr) :
        ((p.expandXOutDtype == TileXR::TILEXR_DATA_TYPE_RESERVED || p.expandXOutDtype == p.dtype) && p.dynamicScalesOut == nullptr);
    if (!validOutput || p.expertTokenNumsType < kEpExpertTokenNumsTypePrefixSum ||
        p.expertTokenNumsType > kEpExpertTokenNumsTypeCount) {
        return TileXR::TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    return TileXR::TILEXR_SUCCESS;
}

int TileXREpValidateMemoryServerDedupConfig(const EpMemoryServerDedupParams &params,
    const TileXR::CommArgs &commArgs, EpMemoryServerDedupLayout *layout)
{
    if (layout == nullptr) {
        return TileXR::TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    *layout = EpMemoryServerDedupLayout {};
    const auto &p = params.dispatch;
    if (commArgs.rank < 0 || commArgs.rank >= commArgs.rankSize ||
        (p.epWorldSize != 0 && p.epWorldSize != commArgs.rankSize) ||
        (p.epRankId != 0 && p.epRankId != commArgs.rank)) {
        return TileXR::TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    EpMemoryServerDedupLayout next {};
    const int ret = TileXREpBuildMemoryServerDedupLayout(commArgs.rankSize, p.bs, p.h,
        p.topK, p.moeExpertNum, &next, p.quantMode == 4);
    if (ret != TileXR::TILEXR_SUCCESS) {
        return ret;
    }
    const uintptr_t weights = reinterpret_cast<uintptr_t>(p.expertScales);
    const uintptr_t outputWeights = reinterpret_cast<uintptr_t>(params.expertScalesOut);
    if (next.weightBytes > UINTPTR_MAX - weights || next.weightBytes > UINTPTR_MAX - outputWeights ||
        (weights < outputWeights + next.weightBytes && outputWeights < weights + next.weightBytes)) {
        return TileXR::TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    if (p.globalBs != 0 && (p.globalBs < 0 || static_cast<uint64_t>(p.globalBs) != next.globalBs)) {
        return TileXR::TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    // The communicator's final 4 MiB are flags, not payload capacity.
    constexpr uint64_t flagBytes = 4ULL * 1024 * 1024;
    if (commArgs.peerMemBytes < flagBytes || next.data.requiredBytes > commArgs.peerMemBytes - flagBytes) {
        return TileXR::TILEXR_ERROR_PARA_CHECK_FAIL;
    }
#if defined(TILEXR_EP_SERVER_DEDUP_ENABLE_PROFILING) && TILEXR_EP_SERVER_DEDUP_ENABLE_PROFILING
    if (next.data.requiredBytes > TileXREpServerDedupSendProfileLayout(
        commArgs.peerMemBytes, uint32_t(p.bs), uint32_t(p.topK)).offset) {
        return TileXR::TILEXR_ERROR_PARA_CHECK_FAIL;
    }
#endif
    for (int rank = 0; rank < commArgs.rankSize; ++rank) {
        if (commArgs.peerMems[rank] == nullptr) {
            return TileXR::TILEXR_ERROR_NOT_INITIALIZED;
        }
        if (!IsAligned(commArgs.peerMems[rank], 32U)) {
            return TileXR::TILEXR_ERROR_PARA_CHECK_FAIL;
        }
    }
    *layout = next;
    return TileXR::TILEXR_SUCCESS;
}

} // namespace TileXREp
