#ifndef TILEXR_EP_MEMORY_SERVER_DEDUP_COMBINE_CONFIG_H
#define TILEXR_EP_MEMORY_SERVER_DEDUP_COMBINE_CONFIG_H

#include <cstdint>

// Independent of Dispatch. Rebuild Host and kernel together on all ranks.
#ifndef TILEXR_EP_SERVER_DEDUP_COMBINE_SEND_CORE_NUM
#define TILEXR_EP_SERVER_DEDUP_COMBINE_SEND_CORE_NUM 32U
#endif

namespace TileXREp {
constexpr uint32_t kEpServerDedupCombineSendCoreNum = TILEXR_EP_SERVER_DEDUP_COMBINE_SEND_CORE_NUM;
static_assert(kEpServerDedupCombineSendCoreNum >= 16U && kEpServerDedupCombineSendCoreNum <= 56U &&
    kEpServerDedupCombineSendCoreNum % 8U == 0U,
    "TILEXR_EP_SERVER_DEDUP_COMBINE_SEND_CORE_NUM must be 16, 24, 32, 40, 48 or 56");
constexpr uint32_t kEpServerDedupCombineRecvCoreNum = 64U - kEpServerDedupCombineSendCoreNum;
constexpr uint32_t kEpServerDedupCombineMaxTokenNum = 10000U;
// P2.1 keeps all resources distinct: three row arrays, compare mask, one
// 512-row table-2 cache, per-level detector/work/result/zero buffers and flags.
constexpr uint32_t kEpServerDedupCombineUbBytes = 190U * 1024U;
constexpr uint32_t kEpServerDedupCombineTableRows = 512U;
constexpr uint32_t kEpServerDedupCombineFixedUbBytes =
    kEpServerDedupCombineTableRows * 12U + 256U +
    2304U + 512U + 32U + 2048U +
    512U + 256U + 32U + 512U + kEpServerDedupCombineRecvCoreNum * 32U;
#if defined(__CCE__) && defined(__CCE_IS_AICORE__)
#define TILEXR_EP_COMBINE_CONSTEXPR __aicore__ inline constexpr
#else
#define TILEXR_EP_COMBINE_CONSTEXPR constexpr
#endif
TILEXR_EP_COMBINE_CONSTEXPR uint32_t EpCombineAlign(uint32_t n, uint32_t a)
{
    return (n + a - 1U) / a * a;
}
TILEXR_EP_COMBINE_CONSTEXPR uint32_t EpCombineRowUbBytes(uint32_t tokens)
{
    return 3U * EpCombineAlign(tokens == 0U ? 4U : tokens * 4U, 256U) +
        EpCombineAlign((tokens + 7U) / 8U + 32U, 256U);
}
TILEXR_EP_COMBINE_CONSTEXPR uint32_t EpCombinePayloadLaneCount(uint32_t tokens, uint32_t h)
{
    if (h == 0U || h > kEpServerDedupCombineUbBytes / 2U || tokens > kEpServerDedupCombineMaxTokenNum) {
        return 0U;
    }
    const uint32_t available = kEpServerDedupCombineUbBytes - kEpServerDedupCombineFixedUbBytes -
        EpCombineRowUbBytes(tokens);
    const uint32_t lanes = available / EpCombineAlign(h * 2U, 32U);
    return lanes > 8U ? 8U : lanes;
}
#undef TILEXR_EP_COMBINE_CONSTEXPR
} // namespace TileXREp
#endif
