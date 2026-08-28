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
constexpr uint32_t kEpMemoryAllCoreCountBsThreshold = 128U;
constexpr uint32_t kEpMemoryCumsumStatusPerCore = 32U;

constexpr uint64_t kEpMemoryRoundStrideBytes =
    static_cast<uint64_t>(TileXR::IPC_DATA_OFFSET + TileXR::IPC_BUFF_MAX_SIZE);
constexpr uint64_t kEpMemoryRequiredPeerBytes =
    static_cast<uint64_t>(TileXR::PING_PONG_SIZE) * kEpMemoryRoundStrideBytes;

constexpr int64_t kEpMemoryDispatchProfileBuffer = 0;
constexpr int64_t kEpMemoryCombineProfileBuffer = 1;
constexpr uint64_t kEpMemoryDispatchDfxOffset = 768ULL * 1024ULL;
constexpr uint64_t kEpMemoryDispatchDfxStride = 512ULL;
constexpr uint64_t kEpMemoryDispatchCumsumOffset = 868ULL * 1024ULL;
constexpr uint64_t kEpMemoryDispatchCumsumFlagOffset = 876ULL * 1024ULL;

static_assert(kEpMemoryDispatchProfileBuffer != kEpMemoryCombineProfileBuffer,
    "EP memory Dispatch and Combine profiling must use different buffers");
static_assert(kEpMemoryDispatchProfileBuffer >= 0 &&
        kEpMemoryDispatchProfileBuffer < TileXR::PING_PONG_SIZE &&
        kEpMemoryCombineProfileBuffer >= 0 &&
        kEpMemoryCombineProfileBuffer < TileXR::PING_PONG_SIZE,
    "EP memory profiling buffer index exceeds communicator buffers");
static_assert(kEpMemoryDispatchDfxOffset +
        static_cast<uint64_t>(kEpMemoryMaxVectorCoreNum) * kEpMemoryDispatchDfxStride <=
        kEpMemoryDispatchCumsumOffset,
    "EP memory DFX records must not overlap cumsum state");
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
