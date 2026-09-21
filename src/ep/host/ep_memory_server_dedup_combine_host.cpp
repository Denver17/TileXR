#include "ep_memory_server_dedup_combine_host.h"
#include "ep_memory_server_dedup_protocol.h"

namespace TileXREp {
namespace {
bool Aligned(const void *p, uint32_t alignment)
{
    return p != nullptr && reinterpret_cast<uintptr_t>(p) % alignment == 0;
}
bool AddressRange(const void *p, uint64_t bytes)
{
    return bytes <= UINTPTR_MAX - reinterpret_cast<uintptr_t>(p);
}
} // namespace

int TileXREpValidateBasicMemoryServerDedupCombineParams(const EpMemoryServerDedupCombineParams &params)
{
    const auto &p = params.combine;
    if (p.comm == nullptr || p.stream == nullptr || p.bs <= 0 || p.h <= 0 || p.topK <= 0 ||
        p.topK > kEpServerDedupMaxTopK || p.moeExpertNum <= 0 || !Aligned(p.expertOut, 2) || !Aligned(p.yOut, 2) ||
        !Aligned(p.assistInfoForCombine, 4) || !Aligned(p.epRecvCounts, 4) ||
        !Aligned(p.expertScales, 4) || !Aligned(params.tokenType, 4) ||
        !Aligned(params.relayReadIndex, 4) || !Aligned(params.sourceMask, 4)) {
        return TileXR::TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    if ((p.dtype != TileXR::TILEXR_DATA_TYPE_FP16 && p.dtype != TileXR::TILEXR_DATA_TYPE_BFP16) ||
        p.quantMode != 0 || p.activeMaskType != TILEXR_EP_ACTIVE_MASK_NONE || p.xActiveMask != nullptr ||
        p.sharedExpertX != nullptr || p.sharedExpertNum != 0 || p.sharedExpertRankNum != 0 ||
        (p.tpWorldSize != 0 && p.tpWorldSize != 1) || p.tpRankId != 0 || p.expertShardType != 0) {
        return TileXR::TILEXR_ERROR_NOT_SUPPORT;
    }
    return TileXR::TILEXR_SUCCESS;
}

int TileXREpValidateMemoryServerDedupCombineConfig(const EpMemoryServerDedupCombineParams &params,
    const TileXR::CommArgs &args, EpMemoryServerDedupCombineLayout *layout)
{
    if (layout == nullptr) { return TileXR::TILEXR_ERROR_PARA_CHECK_FAIL; }
    *layout = {};
    const auto &p = params.combine;
    if (args.rank < 0 || args.rank >= args.rankSize ||
        (p.epWorldSize != 0 && p.epWorldSize != args.rankSize) ||
        (p.epRankId != 0 && p.epRankId != args.rank)) {
        return TileXR::TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    EpMemoryServerDedupCombineLayout next {};
    int ret = TileXREpBuildMemoryServerDedupCombineLayout(args.rankSize, p.bs, p.h, p.topK, p.moeExpertNum, &next);
    if (ret != TileXR::TILEXR_SUCCESS) { return ret; }
    // TokenNum stays device-side. Reserve for every supported count that can
    // fit the caller's tensor capacity, rather than reading count data on Host.
    const uint32_t maxTokens = static_cast<uint32_t>(next.capacityRows < kEpServerDedupCombineMaxTokenNum ?
        next.capacityRows : kEpServerDedupCombineMaxTokenNum);
    if (EpCombinePayloadLaneCount(maxTokens, static_cast<uint32_t>(p.h)) == 0U ||
        EpCombineM4InputLaneCount(maxTokens, static_cast<uint32_t>(p.h), static_cast<uint32_t>(p.topK)) == 0U ||
        EpCombineM4OutputLaneCount(maxTokens, static_cast<uint32_t>(p.h), static_cast<uint32_t>(p.topK)) == 0U ||
        EpCombineM5InputLaneCount(static_cast<uint32_t>(p.bs), static_cast<uint32_t>(p.h), static_cast<uint32_t>(p.topK)) == 0U ||
        EpCombineM5OutputLaneCount(static_cast<uint32_t>(p.bs), static_cast<uint32_t>(p.h), static_cast<uint32_t>(p.topK)) == 0U) {
        return TileXR::TILEXR_ERROR_NOT_SUPPORT;
    }
    if ((p.globalBs != 0 && (p.globalBs < 0 || uint64_t(p.globalBs) != next.globalBs)) ||
        !AddressRange(p.expertOut, next.expertOutBytes) || !AddressRange(p.yOut, next.yOutBytes) ||
        !AddressRange(p.assistInfoForCombine, next.assistInfoBytes) ||
        !AddressRange(p.epRecvCounts, next.sendCountsBytes) || !AddressRange(p.expertScales, next.expertScalesBytes) ||
        !AddressRange(params.tokenType, next.tokenTypeBytes) ||
        !AddressRange(params.relayReadIndex, next.relayReadIndexBytes) || !AddressRange(params.sourceMask, next.sourceMaskBytes)) {
        return TileXR::TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    const uint64_t stride = args.peerMemBytes / 2U;
    if (args.peerMemBytes % 1024U != 0 || stride < TileXR::TILEXR_FLAG_BUFF_BYTES ||
        next.requiredWindowBytes > stride - TileXR::TILEXR_FLAG_BUFF_BYTES) {
        return TileXR::TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    for (int rank = 0; rank < args.rankSize; ++rank) {
        if (args.peerMems[rank] == nullptr) { return TileXR::TILEXR_ERROR_NOT_INITIALIZED; }
        if (!Aligned(args.peerMems[rank], 512) || !AddressRange(args.peerMems[rank], args.peerMemBytes)) {
            return TileXR::TILEXR_ERROR_PARA_CHECK_FAIL;
        }
    }
    *layout = next;
    return TileXR::TILEXR_SUCCESS;
}
} // namespace TileXREp
