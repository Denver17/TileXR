#ifndef TILEXR_EP_MEMORY_SERVER_DEDUP_COUNT_LAYOUT_H
#define TILEXR_EP_MEMORY_SERVER_DEDUP_COUNT_LAYOUT_H

#include "comm_args.h"
#include "ep_memory_server_dedup_buffer_layout.h"
#include "tilexr_types.h"

#include "ep_memory_server_dedup_mode.h"

namespace TileXREp {

constexpr uint32_t kEpServerDedupCoreNum = 64U;
#ifndef TILEXR_EP_SERVER_DEDUP_SEND_CORE_NUM
#define TILEXR_EP_SERVER_DEDUP_SEND_CORE_NUM 32U
#endif
constexpr uint32_t kEpServerDedupSendCoreNum = TILEXR_EP_SERVER_DEDUP_SEND_CORE_NUM;
static_assert(
    kEpServerDedupSendCoreNum >= 16U && kEpServerDedupSendCoreNum <= 48U && kEpServerDedupSendCoreNum % 4U == 0U,
    "TILEXR_EP_SERVER_DEDUP_SEND_CORE_NUM must be 16..48 in steps of 4");
constexpr uint32_t kEpServerDedupRecvCoreNum = kEpServerDedupCoreNum - kEpServerDedupSendCoreNum;
#ifndef TILEXR_EP_SERVER_DEDUP_COUNT_CORE_NUM
#define TILEXR_EP_SERVER_DEDUP_COUNT_CORE_NUM 32U
#endif
constexpr uint32_t kEpServerDedupCountCoreNum = TILEXR_EP_SERVER_DEDUP_COUNT_CORE_NUM;
static_assert(kEpServerDedupCountCoreNum >= 1U && kEpServerDedupCountCoreNum <= kEpServerDedupCoreNum,
    "TILEXR_EP_SERVER_DEDUP_COUNT_CORE_NUM must be 1..64");
constexpr uint32_t kEpServerDedupFirstCountCore = kEpServerDedupCoreNum - kEpServerDedupCountCoreNum;
constexpr uint32_t kEpServerDedupMaxExperts = 1024U;
constexpr uint32_t kEpServerDedupCountRecordBytes = 32U;
constexpr uint32_t kEpServerDedupCountGmStride = 512U;
constexpr uint32_t kEpServerDedupCountUbBytes = 190U * 1024U;
static_assert(kEpServerDedupCountGmStride % kEpServerDedupCountRecordBytes == 0U &&
        TILEXR_EP_SERVER_DEDUP_DISPATCH_COUNT_OFFSET % kEpServerDedupCountGmStride == 0U,
    "GM count slots must be aligned whole records");
static_assert(
    uint64_t(kEpServerDedupMaxExperts) * kEpServerDedupCountGmStride <= TILEXR_EP_SERVER_DEDUP_DISPATCH_COUNT_BYTES,
    "Count records exceed their fixed region");
static_assert(uint64_t(kEpServerDedupRecvCoreNum) * kEpServerDedupRecvCoreNum * kEpServerDedupCountRecordBytes <=
            TILEXR_EP_SERVER_DEDUP_DISPATCH_CUMSUM_BYTES &&
        TILEXR_EP_SERVER_DEDUP_DISPATCH_CUMSUM_BYTES <= TILEXR_EP_SERVER_DEDUP_DISPATCH_READY_BYTES,
    "Receiver matrices exceed their fixed regions");
static_assert(uint64_t(kEpServerDedupMaxExperts) * sizeof(int32_t) * kEpServerDedupRecvCoreNum * 2U <=
        TILEXR_EP_SERVER_DEDUP_DISPATCH_PREFIX_BYTES,
    "Prefix region must include one full spare footprint");

#if TILEXR_ASCENDC_AICORE_COMPILE
#define TILEXR_EP_DEDUP_INLINE FORCE_INLINE_AICORE
#else
#define TILEXR_EP_DEDUP_INLINE inline
#endif

// Shared Host/kernel budget for the asynchronous records retained after SendCnt.
// Only trailing Count cores with a nonempty expert partition reserve records.
TILEXR_EP_DEDUP_INLINE uint32_t TileXREpServerDedupCountReservedBytes(uint32_t experts, uint32_t core)
{
    if (core < kEpServerDedupFirstCountCore || core - kEpServerDedupFirstCountCore >= experts) {
        return 0U;
    }
    const uint32_t records = (experts + kEpServerDedupCountCoreNum - 1U) / kEpServerDedupCountCoreNum;
    return (records + 7U) / 8U * 8U * kEpServerDedupCountRecordBytes;
}

// Matrix offsets are relative to cntOffset; cntOffset, counterOffset and
// stateBytes are relative to the selected window. Leading padding is retained.
struct EpServerDedupCountLayout {
    uint64_t cntOffset;
    uint64_t cumsumOffset;
    uint64_t cumsumFlagOffset;
    uint64_t prefixOffset;
    uint64_t prefixStride;
    uint64_t counterOffset; // Retired counter: preserve its reserved layout, never read/write it.
    uint64_t stateBytes;
};

// All ranks use the same compiled core split and serialize communicator calls
// on one stream. Count publication (including zeros) protects two-call reuse.
// Host validates equal aligned halves and a runtime flag reserve in each half.
TILEXR_EP_DEDUP_INLINE uint64_t TileXREpServerDedupWindowOffset(int64_t magic, uint64_t peerMemBytes)
{
    return (uint64_t(magic) % 2U) * (peerMemBytes / 2U);
}

TILEXR_EP_DEDUP_INLINE EpServerDedupCountLayout TileXREpServerDedupCountLayout(uint32_t experts)
{
    EpServerDedupCountLayout layout {};
    layout.cntOffset = TILEXR_EP_SERVER_DEDUP_DISPATCH_CONTROL_OFFSET + TILEXR_EP_SERVER_DEDUP_DISPATCH_COUNT_OFFSET;
    layout.cumsumOffset = TILEXR_EP_SERVER_DEDUP_DISPATCH_CUMSUM_OFFSET - TILEXR_EP_SERVER_DEDUP_DISPATCH_COUNT_OFFSET;
    layout.prefixOffset = TILEXR_EP_SERVER_DEDUP_DISPATCH_PREFIX_OFFSET - TILEXR_EP_SERVER_DEDUP_DISPATCH_COUNT_OFFSET;
    layout.prefixStride = (uint64_t(experts) * sizeof(int32_t) + 31U) / 32U * 32U;
    layout.cumsumFlagOffset =
        TILEXR_EP_SERVER_DEDUP_DISPATCH_READY_OFFSET - TILEXR_EP_SERVER_DEDUP_DISPATCH_COUNT_OFFSET;
    layout.counterOffset =
        TILEXR_EP_SERVER_DEDUP_DISPATCH_CONTROL_OFFSET + TILEXR_EP_SERVER_DEDUP_DISPATCH_COUNTER_OFFSET;
    layout.stateBytes = layout.counterOffset + TILEXR_EP_SERVER_DEDUP_DISPATCH_COUNTER_BYTES;
    return layout;
}

// Peak sendCnt allocation: four route-sized UB buffers plus count records/histogram.
// Sender/receive scratch overlays those four buffers and the histogram after
// filtering; the records remain reserved until the asynchronous writes retire.
TILEXR_EP_DEDUP_INLINE uint64_t TileXREpServerDedupCountSendUbBytes(
    uint64_t routes, uint32_t experts, uint32_t countCores)
{
    const uint64_t routeBytes = (routes * sizeof(int32_t) + 255U) / 256U * 256U;
    const uint32_t records = (experts + countCores - 1U) / countCores;
    const uint32_t alignedRecords = (records + 7U) / 8U * 8U;
    return 4U * routeBytes + uint64_t(alignedRecords) * (kEpServerDedupCountRecordBytes + sizeof(int32_t));
}

#undef TILEXR_EP_DEDUP_INLINE
} // namespace TileXREp
#endif
