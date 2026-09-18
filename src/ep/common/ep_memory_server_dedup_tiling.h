#ifndef TILEXR_EP_MEMORY_SERVER_DEDUP_TILING_H
#define TILEXR_EP_MEMORY_SERVER_DEDUP_TILING_H

#include <cstddef>
#include <cstdint>

namespace TileXREp {

// Shared Host/AICore wire layout. Host validates before narrowing; magic is a
// separate launch argument. Explicit padding keeps every submitted byte defined.
struct EpServerDedupTilingData {
    uint32_t bs;
    uint32_t h;
    uint32_t profilingLaunchSlot;
    uint16_t moeExpertNum;
    uint8_t topK;
    uint8_t reservedSendCoreNum;
    uint8_t expertTokenNumsType;
    uint8_t quantMode;
    uint8_t dtype;
    uint8_t expandXOutDtype;
    uint8_t reservedEnableServerDedup;
    uint8_t reserved[11];
};
static_assert(sizeof(EpServerDedupTilingData) == 32U, "tiling occupies one 32B block");
// Keep the wire structure naturally aligned. Kernel reads use scalar GM
// accesses, not an implicit vector/MTE load of the entire block.
static_assert(alignof(EpServerDedupTilingData) == 4U, "tiling uses natural scalar alignment");
static_assert(offsetof(EpServerDedupTilingData, profilingLaunchSlot) == 8U, "profiling slot offset");
static_assert(offsetof(EpServerDedupTilingData, moeExpertNum) == 12U, "expert count offset");
static_assert(offsetof(EpServerDedupTilingData, topK) == 14U, "topK offset");
static_assert(offsetof(EpServerDedupTilingData, reservedSendCoreNum) == 15U, "reserved offset");
static_assert(offsetof(EpServerDedupTilingData, reservedEnableServerDedup) == 20U, "reserved switch offset");

} // namespace TileXREp

#endif
