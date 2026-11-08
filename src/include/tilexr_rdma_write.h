/* Copyright (c) 2026 TileXR Project. Licensed under the Apache License, Version 2.0. */

/*
 * 功能：为 AIV kernel 提供单段和多 SGE WRITE adapter；multi-QP 入口只选择指定 QP，发送完成由 RDMAWait 显式等待。
 * 调用关系：入口将 RDMAWriteOptions 转为 pipeline 参数，再调用 tilexr_rdma_device.h；SGE 列表按顺序写入一个远端连续区域。
 * 资源与生命周期：显式 context 在调用间保留 UB 状态；无 context 的入口借用 scratch 后缀并恢复、保存状态。payload/MR 须保留到完成。
 *
 * Purpose: Provide single-segment and multi-SGE AIV WRITE adapters; multi-QP entries select one QP and RDMAWait explicitly waits for completion.
 * Calls: Adapters translate RDMAWriteOptions for tilexr_rdma_device.h; SGEs gather in list order into one contiguous remote span.
 * Lifetime: Explicit contexts retain UB state across calls; context-free entries borrow a scratch suffix and restore/save state.
 *           Payloads/MRs must remain live until completion.
 */
#ifndef TILEXR_RDMA_WRITE_H
#define TILEXR_RDMA_WRITE_H

#include "tilexr_rdma_device.h"
#include "tilexr_rdma_write_options.h"

namespace TileXR {
namespace RdmaWriteDetail {

__aicore__ inline bool ValidQpArray(const __gm__ TileXRRDMADeviceView* views,
    uint32_t qpCount, uint32_t qpIndex)
{
    const uint64_t address = reinterpret_cast<uint64_t>(views);
    return views != nullptr && (address & 7U) == 0U &&
        qpCount >= 1U && qpCount <= TILEXR_RDMA_MAX_QPS && qpIndex < qpCount &&
        RdmaWire::Span(address, uint64_t(qpCount) * sizeof(TileXRRDMADeviceView));
}

/* Submit a WRITE batch; success does not imply completion. Call RDMAWait before
 * reusing payload/MRs or tearing down the QP. Backpressure may still poll CQEs.
 * All offsets/strides are bytes. Zero strides reuse
 * a span; cyclicSlots follows the existing pipeline's persistent WR ordinal.
 * postBatch is 1..64; the default WR window follows SQ BB capacity/maxSendWr.
 * A positive maxInFlight adds an application cap; batches shrink to fit it.
 * Scratch is UB >= RDMAWriteScratchBytes(), aligned32 and live for the call.
 * The pipelines retain full-span/no-wrap admission, fresh CQ polling, ordered
 * MTE3 publication and st_dev doorbells. Entering pipeline mode is permanent
 * for this QP; do not switch back to the legacy all-signaled APIs.
 * One AIV exclusively owns each QP across regions/streams. Remote consumers
 * need application synchronization. Posted errors retain/poison the QP;
 * quiesce and destroy it before freeing potentially referenced memory. */
__aicore__ inline uint32_t Write(const __gm__ TileXRRDMADeviceView* view,
    uint64_t localOffset, uint64_t remoteOffset, uint32_t bytes,
    const RDMAWriteOptions& options, const AscendC::LocalTensor<uint8_t>& scratch, RDMAWriteContext* context)
{
    RDMAWriteTraceScope interval(context != nullptr ? &context->Trace() : nullptr,
        RDMAWriteEvent::WriteBegin, RDMAWriteEvent::WriteEnd);
    const RdmaWire::PipelineOptions pipeline{options.postBatch, options.maxInFlight, options.cyclicSlots};
    return RDMAPutPipeline(view, localOffset, remoteOffset, bytes, options.count,
        options.localStride, options.remoteStride, pipeline, scratch, options.maxPolls, context);
}

/* A kernel-local SGE array gathers into one contiguous remote span per WR.
 * localStride advances every SGE. Explicit contexts use RDMAWriteWorkScratchBytes(numSge);
 * compatibility calls use RDMAWriteScratchBytes(numSge, actualPendingCapacity).
 * The list is stable for the call and each entry's reserved field is zero. */
__aicore__ inline uint32_t Sge(const __gm__ TileXRRDMADeviceView* view,
    const TileXRRDMADeviceSge* sges, uint32_t numSge, uint64_t remoteOffset,
    const RDMAWriteOptions& options, const AscendC::LocalTensor<uint8_t>& scratch, RDMAWriteContext* context)
{
    RDMAWriteTraceScope interval(context != nullptr ? &context->Trace() : nullptr,
        RDMAWriteEvent::SgeBegin, RDMAWriteEvent::SgeEnd);
#if TILEXR_RDMA_DEBUG
    if (sges == nullptr || numSge == 0U || numSge > TILEXR_RDMA_DEVICE_MAX_SGE) {
        return TILEXR_RDMA_DEVICE_INVALID;
    }
#endif
    if (numSge == 1U) {
#if TILEXR_RDMA_DEBUG
        if (sges[0].reserved != 0U) { return TILEXR_RDMA_DEVICE_INVALID; }
#endif
        return Write(view, sges[0].localOffset, remoteOffset, sges[0].bytes, options, scratch, context);
    }
    const RdmaWire::PipelineOptions pipeline{options.postBatch, options.maxInFlight, options.cyclicSlots};
    return RDMAPutSgeListPipeline(view, sges, numSge, remoteOffset, options.count,
        options.localStride, options.remoteStride, pipeline, scratch, options.maxPolls, context);
}

/* views names qpCount contiguous V3 views in GM, qpCount=1..16. Each call
 * selects only qpIndex, exclusively owned by the calling AIV. Launch/partition
 * AIVs explicitly; there is no QP loop, implicit barrier or payload partition.
 * Debug validates the complete array span before selected-pointer arithmetic;
 * the caller supplies the actual allocation and keeps every view alive. */
__aicore__ inline uint32_t Multi(const __gm__ TileXRRDMADeviceView* views,
    uint32_t qpCount, uint32_t qpIndex, uint64_t localOffset, uint64_t remoteOffset, uint32_t bytes,
    const RDMAWriteOptions& options, const AscendC::LocalTensor<uint8_t>& scratch, RDMAWriteContext* context)
{
    RDMAWriteTraceScope interval(context != nullptr ? &context->Trace() : nullptr,
        RDMAWriteEvent::MultiBegin, RDMAWriteEvent::MultiEnd);
#if TILEXR_RDMA_DEBUG
    if (!ValidQpArray(views, qpCount, qpIndex)) { return TILEXR_RDMA_DEVICE_INVALID; }
#else
    (void)qpCount;
#endif
    return Write(views + qpIndex, localOffset, remoteOffset, bytes, options, scratch, context);
}

__aicore__ inline uint32_t SgeMulti(const __gm__ TileXRRDMADeviceView* views,
    uint32_t qpCount, uint32_t qpIndex, const TileXRRDMADeviceSge* sges, uint32_t numSge,
    uint64_t remoteOffset, const RDMAWriteOptions& options, const AscendC::LocalTensor<uint8_t>& scratch,
    RDMAWriteContext* context)
{
    RDMAWriteTraceScope interval(context != nullptr ? &context->Trace() : nullptr,
        RDMAWriteEvent::SgeMultiBegin, RDMAWriteEvent::SgeMultiEnd);
#if TILEXR_RDMA_DEBUG
    if (!ValidQpArray(views, qpCount, qpIndex)) { return TILEXR_RDMA_DEVICE_INVALID; }
#else
    (void)qpCount;
#endif
    return Sge(views + qpIndex, sges, numSge, remoteOffset, options, scratch, context);
}
} // namespace RdmaWriteDetail

// The overloads preserve the existing source signatures. Context overloads
// need only work scratch and require explicit Restore/Save at kernel boundaries.
__aicore__ inline uint32_t RDMAWrite(const __gm__ TileXRRDMADeviceView* view,
    uint64_t local, uint64_t remote, uint32_t bytes, const RDMAWriteOptions& options,
    const AscendC::LocalTensor<uint8_t>& scratch)
{ return RdmaWriteDetail::Write(view, local, remote, bytes, options, scratch, nullptr); }
__aicore__ inline uint32_t RDMAWrite(const __gm__ TileXRRDMADeviceView* view,
    uint64_t local, uint64_t remote, uint32_t bytes, const RDMAWriteOptions& options,
    const AscendC::LocalTensor<uint8_t>& scratch, RDMAWriteContext& context)
{
    RDMAWriteTraceScope api(&context.Trace(), RDMAWriteEvent::ApiBegin, RDMAWriteEvent::ApiEnd);
    return RdmaWriteDetail::Write(view, local, remote, bytes, options, scratch, &context);
}
__aicore__ inline uint32_t RDMAWriteSge(const __gm__ TileXRRDMADeviceView* view,
    const TileXRRDMADeviceSge* sges, uint32_t numSge, uint64_t remote, const RDMAWriteOptions& options,
    const AscendC::LocalTensor<uint8_t>& scratch)
{ return RdmaWriteDetail::Sge(view, sges, numSge, remote, options, scratch, nullptr); }
__aicore__ inline uint32_t RDMAWriteSge(const __gm__ TileXRRDMADeviceView* view,
    const TileXRRDMADeviceSge* sges, uint32_t numSge, uint64_t remote, const RDMAWriteOptions& options,
    const AscendC::LocalTensor<uint8_t>& scratch, RDMAWriteContext& context)
{
    RDMAWriteTraceScope api(&context.Trace(), RDMAWriteEvent::ApiBegin, RDMAWriteEvent::ApiEnd);
    return RdmaWriteDetail::Sge(view, sges, numSge, remote, options, scratch, &context);
}
__aicore__ inline uint32_t RDMAWriteMultiQp(const __gm__ TileXRRDMADeviceView* views,
    uint32_t count, uint32_t index, uint64_t local, uint64_t remote, uint32_t bytes,
    const RDMAWriteOptions& options, const AscendC::LocalTensor<uint8_t>& scratch)
{ return RdmaWriteDetail::Multi(views, count, index, local, remote, bytes, options, scratch, nullptr); }
__aicore__ inline uint32_t RDMAWriteMultiQp(const __gm__ TileXRRDMADeviceView* views,
    uint32_t count, uint32_t index, uint64_t local, uint64_t remote, uint32_t bytes,
    const RDMAWriteOptions& options, const AscendC::LocalTensor<uint8_t>& scratch, RDMAWriteContext& context)
{
    RDMAWriteTraceScope api(&context.Trace(), RDMAWriteEvent::ApiBegin, RDMAWriteEvent::ApiEnd);
    return RdmaWriteDetail::Multi(views, count, index, local, remote, bytes, options, scratch, &context);
}
__aicore__ inline uint32_t RDMAWriteSgeMultiQp(const __gm__ TileXRRDMADeviceView* views,
    uint32_t count, uint32_t index, const TileXRRDMADeviceSge* sges, uint32_t numSge, uint64_t remote,
    const RDMAWriteOptions& options, const AscendC::LocalTensor<uint8_t>& scratch)
{ return RdmaWriteDetail::SgeMulti(views, count, index, sges, numSge, remote, options, scratch, nullptr); }
__aicore__ inline uint32_t RDMAWriteSgeMultiQp(const __gm__ TileXRRDMADeviceView* views,
    uint32_t count, uint32_t index, const TileXRRDMADeviceSge* sges, uint32_t numSge, uint64_t remote,
    const RDMAWriteOptions& options, const AscendC::LocalTensor<uint8_t>& scratch, RDMAWriteContext& context)
{
    RDMAWriteTraceScope api(&context.Trace(), RDMAWriteEvent::ApiBegin, RDMAWriteEvent::ApiEnd);
    return RdmaWriteDetail::SgeMulti(views, count, index, sges, numSge, remote, options, scratch, &context);
}

} // namespace TileXR
#endif
