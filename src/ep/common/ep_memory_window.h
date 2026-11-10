/*
 * 定义 Memory EP 的 peer window 分区和 magic 对应的轮次地址；Host 校验容量，Device 按同一布局访问。
 */
#ifndef TILEXR_EP_COMMON_EP_MEMORY_WINDOW_H
#define TILEXR_EP_COMMON_EP_MEMORY_WINDOW_H

#include <cstdint>

#include "comm_args.h"

namespace TileXREp {

constexpr int64_t kEpMemoryStateWindowBytes = 1024 * 1024;
constexpr int64_t kEpMemoryWindowAlignmentBytes = 32;
constexpr int64_t kEpMemorySplitBlockBytes = 512;
constexpr int64_t kEpMemorySplitPayloadBytes = 480;
constexpr int64_t kEpMemoryFullUbBytes = 190 * 1024;
constexpr uint32_t kEpMemoryMaxCountCoreNum = 8;
constexpr uint32_t kEpMemoryMaxVectorCoreNum = 200;
constexpr uint32_t kEpMemoryAssistInfoWidth = 3U;
constexpr uint32_t kEpMemorySmallBsThreshold = 48U;
constexpr uint32_t kEpMemoryDoubleGroupStripeRankNum = 32U;
constexpr uint32_t kEpMemoryDoubleGroupRoundNum = 2U;
constexpr uint32_t kEpMemoryDoubleGroupWorldSize64 = 64U;
constexpr uint32_t kEpMemoryDoubleGroupWorldSize128 = 128U;
constexpr uint32_t kEpMemoryCumsumStatusPerCore = 32U;

constexpr uint64_t kEpMemoryRoundStrideBytes =
    static_cast<uint64_t>(TileXR::IPC_DATA_OFFSET + TileXR::IPC_BUFF_MAX_SIZE);
constexpr uint64_t kEpMemoryRequiredPeerBytes =
    static_cast<uint64_t>(TileXR::PING_PONG_SIZE) * kEpMemoryRoundStrideBytes;

// Receive-count workspace grows backward from this unchanged state-window boundary.
constexpr uint64_t kEpMemoryRecvCountWorkspaceEnd = 768ULL * 1024ULL;
constexpr uint64_t kEpMemoryDispatchCumsumOffset = 868ULL * 1024ULL;
constexpr uint64_t kEpMemoryDispatchCumsumFlagOffset = 876ULL * 1024ULL;

static_assert(kEpMemoryDispatchCumsumOffset +
        static_cast<uint64_t>(kEpMemoryMaxCountCoreNum) * kEpMemoryMaxCountCoreNum *
            kEpMemoryWindowAlignmentBytes <=
        kEpMemoryDispatchCumsumFlagOffset,
    "EP memory cumsum records must not overlap cumsum flags");
static_assert(kEpMemoryDispatchCumsumFlagOffset +
        static_cast<uint64_t>(kEpMemoryMaxVectorCoreNum) * kEpMemoryMaxCountCoreNum *
            kEpMemoryWindowAlignmentBytes <=
        static_cast<uint64_t>(kEpMemoryStateWindowBytes),
    "EP memory cumsum flags must fit in the state window");

#if TILEXR_ASCENDC_AICORE_COMPILE
#define TILEXR_EP_MEMORY_INLINE FORCE_INLINE_AICORE
#else
#define TILEXR_EP_MEMORY_INLINE inline
#endif

TILEXR_EP_MEMORY_INLINE uint64_t TileXREpMemoryRoundIndex(int64_t magic)
{
    return static_cast<uint64_t>(magic) % static_cast<uint64_t>(TileXR::PING_PONG_SIZE);
}

TILEXR_EP_MEMORY_INLINE uint64_t TileXREpMemoryRoundOffset(int64_t magic)
{
    return TileXREpMemoryRoundIndex(magic) * kEpMemoryRoundStrideBytes;
}

TILEXR_EP_MEMORY_INLINE uint64_t TileXREpMemoryRoundDataOffset(int64_t magic)
{
    return TileXREpMemoryRoundOffset(magic) + static_cast<uint64_t>(TileXR::IPC_DATA_OFFSET);
}

TILEXR_EP_MEMORY_INLINE GM_ADDR TileXREpMemoryRoundBase(GM_ADDR peerMem, int64_t magic)
{
    return peerMem + TileXREpMemoryRoundOffset(magic);
}

TILEXR_EP_MEMORY_INLINE GM_ADDR TileXREpMemoryRoundDataBase(GM_ADDR peerMem, int64_t magic)
{
    return peerMem + TileXREpMemoryRoundDataOffset(magic);
}

#undef TILEXR_EP_MEMORY_INLINE

} // namespace TileXREp

#endif // TILEXR_EP_COMMON_EP_MEMORY_WINDOW_H
