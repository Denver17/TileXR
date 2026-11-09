/*
 * 定义紧凑 Dispatch 的 Host/kernel 参数布局和 Memory state window 中的 gate 槽。
 * tilexr_ep_dispatch_compact.cpp 按 CompactKernelArgs 传递输入、池地址和 profiling 描述符；
 * tilexr_ep_dispatch_compact_kernel 读取同序参数，Memory Dispatch 通过 gate 交换各 rank
 * 的分配状态、输出地址、token 行数和本轮 magic。静态断言约束参数大小及窗口边界。
 */
#ifndef TILEXR_EP_COMPACT_LAYOUT_H
#define TILEXR_EP_COMPACT_LAYOUT_H
#include <cstdint>
#include "ep_memory_window.h"
namespace TileXREp {
constexpr uint64_t kCompactGateOffset = 960ULL * 1024;
constexpr uint64_t kCompactGateBytes = 64;
static_assert(kEpMemoryDispatchCumsumFlagOffset +
    uint64_t(kEpMemoryMaxVectorCoreNum) * kEpMemoryMaxCountCoreNum * 32 <= kCompactGateOffset,
    "compact gate overlaps cumulative count flags");
static_assert(kCompactGateOffset + kCompactGateBytes <= uint64_t(kEpMemoryStateWindowBytes),
    "compact gate exceeds state window");
struct CompactKernelArgs {
    uint64_t commArgs, x, expertIds, expertCounts, sendCounts, state, mailbox;
    int64_t bs, h, topK, experts, dtype, magic;
    uint64_t profilingGM; // Optional SDK descriptor; Host and kernel are deployed together.
};
static_assert(sizeof(CompactKernelArgs) == 14 * sizeof(uint64_t), "compact kernel launch ABI");
}
#endif
