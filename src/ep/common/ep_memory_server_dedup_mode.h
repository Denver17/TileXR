#ifndef TILEXR_EP_MEMORY_SERVER_DEDUP_MODE_H
#define TILEXR_EP_MEMORY_SERVER_DEDUP_MODE_H
#include <cstdint>
// Layout stays fixed for the lifetime of a binary; auto changes execution only.
#ifndef TILEXR_EP_SERVER_DEDUP_ENABLE
#define TILEXR_EP_SERVER_DEDUP_ENABLE 1
#endif
#ifndef TILEXR_EP_SERVER_DEDUP_AUTO_ENABLE
#define TILEXR_EP_SERVER_DEDUP_AUTO_ENABLE 0
#endif
static_assert(TILEXR_EP_SERVER_DEDUP_ENABLE == 0 || TILEXR_EP_SERVER_DEDUP_ENABLE == 1,
    "TILEXR_EP_SERVER_DEDUP_ENABLE must be 0 or 1");
static_assert(TILEXR_EP_SERVER_DEDUP_AUTO_ENABLE == 0 || TILEXR_EP_SERVER_DEDUP_AUTO_ENABLE == 1,
    "TILEXR_EP_SERVER_DEDUP_AUTO_ENABLE must be 0 or 1");
static_assert(!TILEXR_EP_SERVER_DEDUP_AUTO_ENABLE || TILEXR_EP_SERVER_DEDUP_ENABLE,
    "Automatic dedup requires the dedup resource layout");
namespace TileXREp {
constexpr bool kEpServerDedupEnable = TILEXR_EP_SERVER_DEDUP_ENABLE != 0;
constexpr bool kEpServerDedupAutoEnable = TILEXR_EP_SERVER_DEDUP_AUTO_ENABLE != 0;
constexpr uint32_t kEpServerDedupAutoThreshold = 32U;
#if defined(__CCE__) && defined(__CCE_IS_AICORE__)
__aicore__ inline
#endif
    constexpr bool
    EpServerDedupEnabled(uint32_t bs)
{
    return kEpServerDedupAutoEnable ? bs > kEpServerDedupAutoThreshold : kEpServerDedupEnable;
}
}
#endif
