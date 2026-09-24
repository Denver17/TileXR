#ifndef TILEXR_EP_MEMORY_SERVER_DEDUP_MODE_H
#define TILEXR_EP_MEMORY_SERVER_DEDUP_MODE_H
// One fixed build setting shared by Host, kernels and layout consumers.
#ifndef TILEXR_EP_SERVER_DEDUP_ENABLE
#define TILEXR_EP_SERVER_DEDUP_ENABLE 1
#endif
static_assert(TILEXR_EP_SERVER_DEDUP_ENABLE == 0 || TILEXR_EP_SERVER_DEDUP_ENABLE == 1,
    "TILEXR_EP_SERVER_DEDUP_ENABLE must be 0 or 1");
namespace TileXREp {
constexpr bool kEpServerDedupEnable = TILEXR_EP_SERVER_DEDUP_ENABLE != 0;
}
#endif
