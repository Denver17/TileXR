/*
 * 功能：提供 compact Memory Dispatch 的 AICore 入口，选择 FP16/BF16 实现并传入 Pool view。
 * 调用关系：Host compact launcher 注册并启动此 binary；发送、计数、分配和输出复制由
 * MoeDispatchMemory 执行。每个 lane 创建 TPipe；启用 profiling 时创建 recorder，
 * 在 Init/Run 返回后 Flush 到 Host 提供的 profiling buffer。
 */
#define TILEXR_EP_COMPACT_ENABLED 1
#include <cstddef>
#include "tilexr_ep_dispatch_memory_kernel.h"

extern "C" __global__ __aicore__ void tilexr_ep_dispatch_compact_kernel(
    GM_ADDR commArgs, GM_ADDR x, GM_ADDR expertIds, GM_ADDR expertCounts, GM_ADDR sendCounts,
    uint64_t state, uint64_t mailbox, int64_t bs, int64_t h, int64_t topK, int64_t experts,
    int64_t dtype, int64_t magic, uint64_t profilingGM)
{
    AscendC::TPipe pipe;
    const auto *args = reinterpret_cast<__gm__ TileXR::CommArgs *>(commArgs);
#if defined(TILEXR_ENABLE_PROFILING) && TILEXR_ENABLE_PROFILING == 1
    TileXR::Elastic::Profile::Recorder profiler;
    profiler.RecordMarker<TileXR::Elastic::Profile::KernelBegin, 1>();
#else
    (void)profilingGM;
#endif
    if (dtype == TileXR::TILEXR_DATA_TYPE_FP16) {
        Mc2Kernel::MoeDispatchMemory<half, false, false> op;
        op.Init(commArgs, x, expertIds, nullptr, nullptr, nullptr, expertCounts, sendCounts,
            nullptr, bs, h, topK, experts, 0, 0, bs * args->rankSize, 1, 0, 0, dtype, magic, &pipe);
        op.SetCompactView({state, mailbox});
#if defined(TILEXR_ENABLE_PROFILING) && TILEXR_ENABLE_PROFILING == 1
        op.SetCompactProfiler(&profiler);
#endif
        op.Run();
    } else {
        Mc2Kernel::MoeDispatchMemory<bfloat16_t, false, false> op;
        op.Init(commArgs, x, expertIds, nullptr, nullptr, nullptr, expertCounts, sendCounts,
            nullptr, bs, h, topK, experts, 0, 0, bs * args->rankSize, 1, 0, 0, dtype, magic, &pipe);
        op.SetCompactView({state, mailbox});
#if defined(TILEXR_ENABLE_PROFILING) && TILEXR_ENABLE_PROFILING == 1
        op.SetCompactProfiler(&profiler);
#endif
        op.Run();
    }
#if defined(TILEXR_ENABLE_PROFILING) && TILEXR_ENABLE_PROFILING == 1
    // This interval ends at Init/Run return, not physical completion of every
    // final flag-clear transfer. msprof Task Duration includes the tail and Flush.
    profiler.RecordMarker<TileXR::Elastic::Profile::KernelEnd,
        TileXR::Elastic::Profile::KernelEnd + 1>();
    profiler.Flush(reinterpret_cast<GM_ADDR>(profilingGM), AscendC::GetBlockIdx());
#endif
}
