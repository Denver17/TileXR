/*
 * 功能：为单 QP benchmark 提交 WRITE 并显式 Wait，记录耗时、状态和队列进度；校准路径只读取 cycle counter。
 * 调用关系：bench.cpp 启动本 kernel，bench_abi.h 定义参数与结果；发送使用 RDMAWrite adapter，Debug profiling 可写入 Host 提供的 lane。
 * 资源与生命周期：kernel 分配本次调用的 UB context/scratch，并在 context 已加载时调用 Save；view、MR 和 payload 由 Host 持有。
 *
 * Purpose: Submit/wait for single-QP benchmark WRITEs and return timing/status/queue progress; calibration only reads the cycle counter.
 * Calls: bench.cpp launches this kernel using bench_abi.h; sends use RDMAWrite adapters and optional Debug profiling writes Host-provided lanes.
 * Lifetime: The kernel allocates launch-local UB context/scratch and calls Save if loaded; the Host owns views, MRs and payloads.
 */
#include "kernel_operator.h"
#include "tilexr_rdma_write.h"
#include "bench_abi.h"
#include "case_layout.h"

extern "C" __global__ __aicore__ void tilexr_matched_write(
    GM_ADDR viewGM, GM_ADDR resultGM, uint64_t sourceOffset, uint64_t targetOffset,
    uint64_t stride, uint32_t bytes, uint32_t count, uint32_t rank,
    uint32_t direction, uint32_t cyclicSlots, uint32_t calibration,
    uint64_t targetStride, uint32_t numSge, uint32_t reserved, GM_ADDR profilingGM)
{
    (void)reserved;
    const auto view = reinterpret_cast<const __gm__ TileXRRDMADeviceView*>(viewGM);
    AscendC::TPipe pipe;
    AscendC::TBuf<AscendC::QuePosition::VECCALC> scratchBuffer, outputBuffer, contextBuffer;
    pipe.InitBuffer(scratchBuffer, TileXR::RDMAWriteWorkScratchBytes(numSge));
    pipe.InitBuffer(contextBuffer, TileXR::RDMAWriteContextBytes(view->queue.pendingCapacity));
    pipe.InitBuffer(outputBuffer, sizeof(BenchResult));
    auto scratch = scratchBuffer.Get<uint8_t>();
    auto output = outputBuffer.Get<uint64_t>();
    TileXR::RDMAWriteContext context(contextBuffer.Get<uint8_t>());
#if TILEXR_RDMA_PROFILE_ACTIVE
    TileXR::Profiling::DirectWriter writer;
    if (profilingGM != nullptr && !calibration) {
        writer.Initialize(profilingGM, 0);
        context.Trace().Attach(writer);
    }
#else
    (void)profilingGM;
#endif
    uint32_t status = 0;
    uint64_t start = 0, end = 0;
    if (!calibration) { context.Trace().Mark(TileXR::RDMAWriteEvent::TotalBegin); status = context.Restore(view); }
    if (calibration) {
        start = AscendC::GetSystemCycle();
        do { end = AscendC::GetSystemCycle(); } while (end - start < 100000000ULL);
    } else if (status == 0U && (direction == 2U || direction == rank)) {
        TileXR::RDMAWriteOptions options;
        options.count = count; options.localStride = stride; options.remoteStride = targetStride;
        options.maxPolls = 10000000U;
        options.maxInFlight = 128U; // Matched comparison policy, not the public default.
        options.cyclicSlots = cyclicSlots;
        if (numSge == 1U) {
            start = AscendC::GetSystemCycle();
            status = TileXR::RDMAWrite(view, sourceOffset, targetOffset, bytes, options, scratch, context);
            if (status == TILEXR_RDMA_DEVICE_SUCCESS) {
                status = TileXR::RDMAWait(view, scratch, context, options.maxPolls);
            }
            end = AscendC::GetSystemCycle();
        } else if (numSge > 1U && numSge <= TILEXR_RDMA_DEVICE_MAX_SGE) {
            TileXRRDMADeviceSge sgeList[TILEXR_RDMA_DEVICE_MAX_SGE];
            for (uint32_t i = 0; i < numSge; ++i) {
                sgeList[i].localOffset = SgeSourceOffset(sourceOffset, bytes, numSge, i);
                sgeList[i].bytes = bytes;
                sgeList[i].reserved = 0U;
            }
            start = AscendC::GetSystemCycle();
            status = TileXR::RDMAWriteSge(view, sgeList, numSge, targetOffset, options, scratch, context);
            if (status == TILEXR_RDMA_DEVICE_SUCCESS) {
                status = TileXR::RDMAWait(view, scratch, context, options.maxPolls);
            }
            end = AscendC::GetSystemCycle();
        } else {
            status = TILEXR_RDMA_DEVICE_INVALID;
        }
    }
    if (context.Loaded()) { const uint32_t saved = context.Save(); if (status == 0U) { status = saved; } }
    context.Trace().Mark(TileXR::RDMAWriteEvent::TotalEnd);
#if TILEXR_RDMA_PROFILE_ACTIVE
    if (profilingGM != nullptr && !calibration) { writer.Publish(); }
#endif
    const auto state = context.Loaded() ? context.State() : TileXRRDMADeviceState{};
    output.SetValue(0, 0x5458524D41544348ULL);
    output.SetValue(1, status); output.SetValue(2, start); output.SetValue(3, end);
    output.SetValue(4, state.producer); output.SetValue(5, state.completed);
    output.SetValue(6, state.cqConsumer);
    output.SetValue(7, uint64_t(state.error) | (uint64_t(state.syndrome) << 32));
    AscendC::GlobalTensor<uint64_t> result;
    result.SetGlobalBuffer(reinterpret_cast<__gm__ uint64_t*>(resultGM), 8);
    TileXR::RdmaDeviceDetail::Sync<AscendC::HardEvent::S_MTE3>();
    AscendC::DataCopy(result, output, 8);
    TileXR::RdmaDeviceDetail::Sync<AscendC::HardEvent::MTE3_S>();
}
