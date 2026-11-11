/*
 * 功能：在一个 AIV 上读取 system cycle，等待本地 deadline；已过期的 deadline 直接返回。
 * 调用关系：Host launcher 将模型换算后的 deadline 按值传入；后续任务由同一 stream 排序。
 * 资源与生命周期：不使用 GM buffer 或 QP。
 */
#if defined(__DAV_C220_VEC__) || defined(__DAV_C310_VEC__)
#include "kernel_operator.h"

extern "C" __global__ __aicore__ void TileXRClockSleepAt(uint64_t localDeadline)
{
    if ASCEND_IS_AIV {
        while (static_cast<uint64_t>(AscendC::GetSystemCycle()) < localDeadline) {}
    }
}
#endif
