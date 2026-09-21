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
// M1/M2 keep all resources distinct: three row arrays, compare mask, one
// 512-row table-2 cache, per-level detector/work/result/zero buffers and flags.
constexpr uint32_t kEpServerDedupCombineUbBytes = 190U * 1024U;
constexpr uint32_t kEpServerDedupCombineTableRows = 512U;
// Even before metadata is reserved, one admitted input/output pair has fewer
// than 256 blocks, so packing never narrows an oversized vector repeat count.
static_assert(kEpServerDedupCombineUbBytes / (480U + 512U) <= 255U, "M3 vector repeat budget");
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
    // M3 vector packing reads every 480B payload block, including its tail.
    // M1 uses the same input lanes and drains them before M3. Output lanes
    // remain distinct until their MTE3 consumers have completed.
    const uint32_t blocks = (h * 2U + 479U) / 480U;
    const uint32_t lanes = available / (blocks * (480U + 512U));
    return lanes > 8U ? 8U : lanes;
}
// M2 controls retain independent storage throughout Read-core stage reuse.
constexpr uint32_t kEpServerDedupCombineResidentUbBytes =
    kEpServerDedupCombineFixedUbBytes - kEpServerDedupCombineTableRows * 12U;
constexpr uint32_t kEpServerDedupCombineM4TableRows = 32U;
TILEXR_EP_COMBINE_CONSTEXPR uint32_t EpCombineM4TaskBytes(uint32_t tokens)
{
    return EpCombineAlign(((tokens + kEpServerDedupCombineRecvCoreNum - 1U) /
        kEpServerDedupCombineRecvCoreNum) * 4U, 32U);
}
TILEXR_EP_COMBINE_CONSTEXPR uint32_t EpCombineM4FixedUbBytes(uint32_t tokens, uint32_t h, uint32_t k)
{
    return kEpServerDedupCombineResidentUbBytes + EpCombineM4TaskBytes(tokens) +
        EpCombineAlign(kEpServerDedupCombineM4TableRows * 12U, 32U) +
        EpCombineAlign(kEpServerDedupCombineM4TableRows * k * 8U, 32U) +
        3U * EpCombineAlign(h * 4U, 256U) + ((h * 2U + 479U) / 480U) * 480U;
}
TILEXR_EP_COMBINE_CONSTEXPR uint32_t EpCombineM4InputLaneCount(uint32_t tokens, uint32_t h, uint32_t k)
{
    if (tokens > kEpServerDedupCombineMaxTokenNum || h == 0U ||
        h > kEpServerDedupCombineUbBytes / 2U || k == 0U || k > 32U) return 0U;
    const uint32_t fixed = EpCombineM4FixedUbBytes(tokens, h, k);
    const uint32_t output = ((h * 2U + 479U) / 480U) * 512U;
    if (fixed + output > kEpServerDedupCombineUbBytes) return 0U;
    // Reserve an independent output lane first, then use remaining UB for
    // prefetch lanes up to the eight event IDs in each pipeline direction.
    const uint32_t lanes = (kEpServerDedupCombineUbBytes - fixed - output) / EpCombineAlign(h * 2U, 32U);
    return lanes > 8U ? 8U : lanes;
}
TILEXR_EP_COMBINE_CONSTEXPR uint32_t EpCombineM4OutputLaneCount(uint32_t tokens, uint32_t h, uint32_t k)
{
    const uint32_t input = EpCombineM4InputLaneCount(tokens, h, k);
    if (input == 0U) return 0U;
    const uint32_t free = kEpServerDedupCombineUbBytes - EpCombineM4FixedUbBytes(tokens, h, k) -
        input * EpCombineAlign(h * 2U, 32U);
    const uint32_t lanes = free / (((h * 2U + 479U) / 480U) * 512U);
    return lanes > 8U ? 8U : lanes;
}
#undef TILEXR_EP_COMBINE_CONSTEXPR
} // namespace TileXREp
#endif
