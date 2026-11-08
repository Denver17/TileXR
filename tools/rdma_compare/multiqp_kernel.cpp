/*
 * 功能：每个 AIV 使用一个 QP，先 warmup 并 Wait，再分别计时 WRITE 提交与最终 Wait；校准路径不提交 WR。
 * 调用关系：multiqp.cpp 按 QP 数启动 AIV；本 kernel 使用 WRITE adapter，通过 bench_abi.h 返回每个 QP 的状态、进度和计时。
 * 资源与生命周期：各 AIV 持有本次 launch 的 UB context/scratch，并在已加载时 Save；Host 持有 view、MR、payload 和可选 profiling buffer。
 *
 * Purpose: Use one QP per AIV, warm up and wait, then time WRITE issue and final Wait separately; calibration posts no WRs.
 * Calls: multiqp.cpp launches one AIV per QP; WRITE adapters submit work and bench_abi.h carries per-QP status, progress and timing.
 * Lifetime: Each AIV holds launch-local UB context/scratch and saves loaded contexts; Host owns views, MRs, payloads and optional profiling buffers.
 */
#include "kernel_operator.h"
#include "tilexr_rdma_write.h"
#include "bench_abi.h"
#include "case_layout.h"

extern "C" __global__ __aicore__ void tilexr_multiqp_write(
    GM_ADDR viewsGM, GM_ADDR resultsGM, uint64_t sourceOffset, uint64_t targetOffset,
    uint64_t stride, uint32_t bytes, uint32_t count, uint32_t rank,
    uint32_t direction, uint32_t cyclicSlots, uint32_t calibration,
    uint64_t targetStride, uint32_t numSge, uint32_t qps, GM_ADDR profilingGM)
{
    const uint32_t q = AscendC::GetBlockIdx();
    if (q >= qps || numSge == 0U || numSge > TILEXR_RDMA_DEVICE_MAX_SGE) { return; }
    const auto views = reinterpret_cast<const __gm__ TileXRRDMADeviceView*>(viewsGM);
    const auto view = views + q;
    AscendC::TPipe pipe;
    AscendC::TBuf<AscendC::QuePosition::VECCALC> scratchBuffer, outputBuffer, contextBuffer;
    pipe.InitBuffer(scratchBuffer, TileXR::RDMAWriteWorkScratchBytes(numSge));
    pipe.InitBuffer(contextBuffer, TileXR::RDMAWriteContextBytes(view->queue.pendingCapacity));
    pipe.InitBuffer(outputBuffer, sizeof(BenchResult) + sizeof(MultiQpTiming));
    const auto scratch = scratchBuffer.Get<uint8_t>();
    const auto output = outputBuffer.Get<uint64_t>();
    TileXR::RDMAWriteContext context(contextBuffer.Get<uint8_t>());
#if TILEXR_RDMA_PROFILE_ACTIVE
    TileXR::Profiling::DirectWriter writer;
    if (profilingGM != nullptr && !calibration) {
        writer.Initialize(profilingGM, q);
        context.Trace().Attach(writer);
    }
#else
    (void)profilingGM;
#endif
    uint32_t status = 0;
    uint64_t start = 0, end = 0, issueEnd = 0;
    if (!calibration) {
        context.Trace().Mark(TileXR::RDMAWriteEvent::TotalBegin);
        status = context.Restore(view);
    }
    if (calibration) {
        start = AscendC::GetSystemCycle();
        do { end = AscendC::GetSystemCycle(); } while (end - start < 100000000ULL);
    } else if (status == 0U && direction == rank) {
        TileXR::RDMAWriteOptions options;
        options.localStride = stride;
        options.remoteStride = targetStride;
        options.maxPolls = 10000000U;
        // Both passes use the same QP/context/marker paths. Warmup touches only
        // slots that the measured pass will overwrite before Host verification.
        for (uint32_t pass = kMultiQpWarmupCalls ? 0U : 1U; pass < 2U && status == 0U; ++pass) {
            const bool warmup = pass == 0U;
            const uint32_t calls = warmup ? kMultiQpWarmupCalls : (kMultiQpSingleWqeCalls ? count : 1U);
            options.count = warmup || kMultiQpSingleWqeCalls ? 1U : count;
            options.postBatch = warmup ? 1U : kMultiQpPostBatch;
            options.cyclicSlots = warmup ? 0U : cyclicSlots;
            TileXRRDMADeviceSge sgeList[TILEXR_RDMA_DEVICE_MAX_SGE];
            for (uint32_t i = 0; i < numSge; ++i) {
                sgeList[i].bytes = bytes;
                sgeList[i].reserved = 0U;
            }
            context.Trace().Mark(warmup ? TileXR::RDMAWriteEvent::WarmupBegin : TileXR::RDMAWriteEvent::IssueBegin);
            if (!warmup) { start = AscendC::GetSystemCycle(); }
            for (uint32_t call = 0; call < calls && status == 0U; ++call) {
                // Warmup repeats one measured slot without advancing cyclic addresses.
                const uint32_t slot = warmup ? MultiQpWarmupSlot(call, count, cyclicSlots) :
                    (kMultiQpSingleWqeCalls ? call : 0U);
                const uint64_t local = sourceOffset + uint64_t(slot) * stride;
                const uint64_t remote = targetOffset + uint64_t(slot) * targetStride;
                if (numSge == 1U) {
                    status = qps == 1U ? TileXR::RDMAWrite(view, local, remote, bytes, options, scratch, context) :
                        TileXR::RDMAWriteMultiQp(views, qps, q, local, remote, bytes, options, scratch, context);
                } else {
                    for (uint32_t i = 0; i < numSge; ++i) {
                        sgeList[i].localOffset = SgeSourceOffset(local, bytes, numSge, i);
                    }
                    status = qps == 1U ? TileXR::RDMAWriteSge(view, sgeList, numSge, remote, options, scratch, context) :
                        TileXR::RDMAWriteSgeMultiQp(views, qps, q, sgeList, numSge, remote, options, scratch, context);
                }
            }
            if (!warmup) {
                issueEnd = AscendC::GetSystemCycle();
                context.Trace().Mark(TileXR::RDMAWriteEvent::IssueEnd);
            }
            if (status == 0U) { status = TileXR::RDMAWait(view, scratch, context, options.maxPolls); }
            if (warmup) { context.Trace().Mark(TileXR::RDMAWriteEvent::WarmupEnd); }
            else { end = AscendC::GetSystemCycle(); }
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
    output.SetValue(8, issueEnd);
    output.SetValue(9, !calibration && rank == direction ? kMultiQpWarmupCalls : 0U);
    output.SetValue(10, 0U); output.SetValue(11, 0U);
    AscendC::GlobalTensor<uint64_t> result, timing;
    result.SetGlobalBuffer(reinterpret_cast<__gm__ uint64_t*>(resultsGM) + uint64_t(q) * 8U, 8);
    timing.SetGlobalBuffer(reinterpret_cast<__gm__ uint64_t*>(resultsGM) + uint64_t(qps) * 8U + uint64_t(q) * 4U, 4);
    TileXR::RdmaDeviceDetail::Sync<AscendC::HardEvent::S_MTE3>();
    AscendC::DataCopy(result, output, 8);
    AscendC::DataCopy(timing, output[8], 4);
    TileXR::RdmaDeviceDetail::Sync<AscendC::HardEvent::MTE3_S>();
}
