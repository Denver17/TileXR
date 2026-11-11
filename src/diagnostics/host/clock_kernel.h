/*
 * 功能：定义 clock sync 和 sleep_at 的 Host launch 接口及 kernel 参数布局。
 * 调用关系：session 提供参数，clock_kernel.cpp 按此 ABI 调用 CANN runtime。
 * 资源与生命周期：sync 参数借用 session 的内存；sleep_at 按值传递 deadline。
 */
#ifndef TILEXR_DIAGNOSTICS_HOST_CLOCK_KERNEL_H
#define TILEXR_DIAGNOSTICS_HOST_CLOCK_KERNEL_H

#include <cstddef>
#include <cstdint>

#include "acl/acl_base.h"

namespace TileXRDiagnostics {
namespace Host {

struct StragglerSession;

struct ClockSyncKernelArgs {
    const void *commArgsPtr = nullptr;
    void *samples = nullptr;
    uint64_t magic = 0;
    uint64_t timeoutCycles = 0;
    uint32_t sampleCount = 0;
    uint32_t warmupSampleCount = 0;
    void *rdmaMemory = nullptr;
};

static_assert(offsetof(ClockSyncKernelArgs, commArgsPtr) == 0, "clock-sync commArgs ABI changed");
static_assert(offsetof(ClockSyncKernelArgs, samples) == 8, "clock-sync samples ABI changed");
static_assert(offsetof(ClockSyncKernelArgs, magic) == 16, "clock-sync magic ABI changed");
static_assert(offsetof(ClockSyncKernelArgs, timeoutCycles) == 24, "clock-sync timeout ABI changed");
static_assert(offsetof(ClockSyncKernelArgs, sampleCount) == 32, "clock-sync sample-count ABI changed");
static_assert(offsetof(ClockSyncKernelArgs, warmupSampleCount) == 36,
              "clock-sync warmup-count ABI changed");
static_assert(offsetof(ClockSyncKernelArgs, rdmaMemory) == 40, "clock-sync RDMA ABI changed");
static_assert(sizeof(ClockSyncKernelArgs) == 48, "clock-sync argument ABI changed");

struct ClockSleepAtKernelArgs {
    uint64_t localDeadline;
};
static_assert(sizeof(ClockSleepAtKernelArgs) == 8, "sleep-at argument ABI changed");
int LaunchClockSleepAtKernel(uint64_t localDeadline, aclrtStream stream);

int LaunchClockSyncKernel(const StragglerSession &session, uint32_t warmupSamples,
                          uint32_t samples, int64_t magic, uint64_t timeoutCycles,
                          aclrtStream stream);

} // namespace Host
} // namespace TileXRDiagnostics

#endif // TILEXR_DIAGNOSTICS_HOST_CLOCK_KERNEL_H
