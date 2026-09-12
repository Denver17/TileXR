#ifndef TILEXR_EP_MEMORY_SERVER_DEDUP_COUNT_LAYOUT_H
#define TILEXR_EP_MEMORY_SERVER_DEDUP_COUNT_LAYOUT_H

#include "comm_args.h"

namespace TileXREp {

constexpr uint32_t kEpServerDedupCoreNum = 64U;
constexpr uint32_t kEpServerDedupSendCoreNum = 48U;
constexpr uint32_t kEpServerDedupRecvCoreNum = 16U;
constexpr uint32_t kEpServerDedupMaxExperts = 1024U;
constexpr uint32_t kEpServerDedupCountRecordBytes = 32U;
constexpr uint32_t kEpServerDedupCountUbBytes = 190U * 1024U;
constexpr uint64_t kEpServerDedupProfileReservedBytes = 12800U;

#if TILEXR_ASCENDC_AICORE_COMPILE
#define TILEXR_EP_DEDUP_INLINE FORCE_INLINE_AICORE
#else
#define TILEXR_EP_DEDUP_INLINE inline
#endif

// R07-01: one state region. Matrix offsets are relative to cntOffset; counter
// and stateBytes are relative to peerMems[rank]. Profiling capacity is always reserved.
struct EpServerDedupCountLayout {
    uint64_t cntOffset;
    uint64_t cumsumOffset;
    uint64_t cumsumFlagOffset;
    uint64_t prefixOffset;
    uint64_t prefixStride;
    uint64_t counterOffset;
    uint64_t stateBytes;
};

TILEXR_EP_DEDUP_INLINE EpServerDedupCountLayout TileXREpServerDedupCountLayout(uint32_t experts)
{
    EpServerDedupCountLayout layout {};
    const uint64_t matrixBytes = uint64_t(kEpServerDedupRecvCoreNum) *
        kEpServerDedupRecvCoreNum * kEpServerDedupCountRecordBytes;
    layout.cntOffset = kEpServerDedupProfileReservedBytes;
    layout.cumsumOffset = uint64_t(experts) * kEpServerDedupCountRecordBytes;
    layout.prefixOffset = layout.cumsumOffset + matrixBytes;
    layout.prefixStride = (uint64_t(experts) * sizeof(int32_t) + 31U) / 32U * 32U;
    layout.cumsumFlagOffset = layout.prefixOffset + kEpServerDedupRecvCoreNum * layout.prefixStride;
    const uint64_t bytes = layout.cntOffset + layout.cumsumFlagOffset + matrixBytes;
    layout.counterOffset = (bytes + 1023U) / 1024U * 1024U;
    layout.stateBytes = layout.counterOffset + 1024U;
    return layout;
}

// Peak sendCnt allocation: four route-sized UB buffers plus count records/histogram.
// Receive/cumsum uses a separate allocation lifetime after the send writes drain.
TILEXR_EP_DEDUP_INLINE uint64_t TileXREpServerDedupCountSendUbBytes(
    uint64_t routes, uint32_t experts, uint32_t countCores)
{
    const uint64_t routeBytes = (routes * sizeof(int32_t) + 255U) / 256U * 256U;
    const uint32_t records = (experts + countCores - 1U) / countCores;
    const uint32_t alignedRecords = (records + 7U) / 8U * 8U;
    return 4U * routeBytes + uint64_t(alignedRecords) * (32U + sizeof(int32_t));
}

#undef TILEXR_EP_DEDUP_INLINE
} // namespace TileXREp
#endif
