/*
 * 定义 ServerDedup Dispatch 的 Host/AICore tiling 布局；Host 校验并缓存快照，kernel 按字段读取。
 */
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
    uint16_t moeExpertNum;
    uint8_t topK;
    uint8_t reservedSendCoreNum;
    uint8_t expertTokenNumsType;
    uint8_t quantMode;
    uint8_t dtype;
    uint8_t expandXOutDtype;
    uint8_t reservedEnableServerDedup;
    uint8_t reserved[15];
};
static_assert(sizeof(EpServerDedupTilingData) == 32U, "tiling occupies one 32B block");
// Keep the wire structure naturally aligned. Kernel reads use scalar GM
// accesses, not an implicit vector/MTE load of the entire block.
static_assert(alignof(EpServerDedupTilingData) == 4U, "tiling uses natural scalar alignment");
static_assert(offsetof(EpServerDedupTilingData, moeExpertNum) == 8U, "expert count offset");
static_assert(offsetof(EpServerDedupTilingData, topK) == 10U, "topK offset");
static_assert(offsetof(EpServerDedupTilingData, reservedSendCoreNum) == 11U, "reserved offset");
static_assert(offsetof(EpServerDedupTilingData, reservedEnableServerDedup) == 16U, "reserved switch offset");

} // namespace TileXREp

#endif
