/* Copyright (c) 2026 TileXR Project. Licensed under the Apache License, Version 2.0. */

/*
 * 功能：从 GM 读取 view/state，用 MTE 发布 UB 中的 WQE，以 st_dev 写 doorbell，并读取 CQE。
 * 调用关系：tilexr_rdma_write.h 使用本文件的 AIV 入口；QueueIO 为 wire/pipeline 算法提供 Ascend C 操作。
 * 资源与生命周期：借用 view、queue state 和调用方的 UB scratch；一个 QP 由一个 AIV 独占，远端 consumer 由应用同步。
 *
 * Purpose: Load GM views/state, publish UB WQEs through MTE, ring doorbells with st_dev and read CQEs.
 * Calls: tilexr_rdma_write.h uses these AIV entries; QueueIO supplies Ascend C operations to wire/pipeline algorithms.
 * Lifetime: Borrows views, queue state and caller UB scratch. One AIV exclusively uses each QP; applications synchronize remote consumers.
 */
#ifndef TILEXR_RDMA_DEVICE_H
#define TILEXR_RDMA_DEVICE_H

#include "kernel_operator.h"
#include "tilexr_rdma_wire.h"
#include "tilexr_rdma_pipeline.h"

namespace TileXR {

#if defined(__NPU_ARCH__) && (__NPU_ARCH__ == 3510)
namespace RdmaDeviceDetail {

template <AscendC::HardEvent event>
__aicore__ inline void Sync()
{
    const int32_t id = static_cast<int32_t>(GetTPipePtr()->FetchEventID(event));
    AscendC::SetFlag<event>(id);
    AscendC::WaitFlag<event>(id);
}

__aicore__ inline void Invalidate(uint64_t address, uint32_t bytes)
{
    const uint64_t first = address & ~uint64_t(63);
    const uint32_t lines = static_cast<uint32_t>(((address & 63U) + bytes + 63U) / 64U);
    for (uint32_t i = 0; i < lines; ++i) {
        AscendC::GlobalTensor<uint32_t> memory;
        memory.SetGlobalBuffer(reinterpret_cast<__gm__ uint32_t*>(first + uint64_t(i) * 64U), 16U);
        AscendC::DataCacheCleanAndInvalid<uint32_t, AscendC::CacheLine::SINGLE_CACHE_LINE,
            AscendC::DcciDst::CACHELINE_OUT>(memory);
    }
}

class QueueIO {
public:
    __aicore__ inline QueueIO(uint64_t stateAddress, const AscendC::LocalTensor<uint8_t>& scratch)
        : stateAddress_(stateAddress), scratch_(scratch)
    {
    }

    __aicore__ inline void SetByte(uint32_t offset, uint8_t value)
    {
        scratch_.SetValue(offset, value);
    }

    __aicore__ inline uint8_t GetByte(uint32_t offset) const
    {
        return scratch_.GetValue(offset);
    }

    __aicore__ inline void SetWord(uint32_t offset, uint32_t value)
    {
        scratch_.ReinterpretCast<uint32_t>().SetValue(offset / 4U, value);
    }

    __aicore__ inline TileXRRDMADeviceState LoadState()
    {
        Invalidate(stateAddress_, sizeof(TileXRRDMADeviceState));
        const __gm__ TileXRRDMADeviceState* state =
            reinterpret_cast<const __gm__ TileXRRDMADeviceState*>(stateAddress_);
        TileXRRDMADeviceState value = {};
        value.producer = state->producer;
        value.completed = state->completed;
        value.cqConsumer = state->cqConsumer;
        value.error = state->error;
        value.syndrome = state->syndrome;
        for (uint32_t i = 0; i < 4U; ++i) { value.reserved[i] = state->reserved[i]; }
        return value;
    }

    __aicore__ inline void StoreState(const TileXRRDMADeviceState& value)
    {
        __gm__ TileXRRDMADeviceState* state = reinterpret_cast<__gm__ TileXRRDMADeviceState*>(stateAddress_);
        state->producer = value.producer;
        state->completed = value.completed;
        state->cqConsumer = value.cqConsumer;
        state->error = value.error;
        state->syndrome = value.syndrome;
        for (uint32_t i = 0; i < 4U; ++i) { state->reserved[i] = value.reserved[i]; }
        Invalidate(stateAddress_, sizeof(TileXRRDMADeviceState));
    }

    __aicore__ inline void Publish(uint64_t address, uint32_t offset, uint32_t bytes)
    {
        AscendC::GlobalTensor<uint8_t> target;
        target.SetGlobalBuffer(reinterpret_cast<__gm__ uint8_t*>(address), bytes);
        const AscendC::DataCopyExtParams params{1U, bytes, 0U, 0U, 0U};
        Sync<AscendC::HardEvent::S_MTE3>(); // Scalar UB construction precedes MTE3.
        AscendC::DataCopyPad(target, scratch_[offset], params);
        Sync<AscendC::HardEvent::MTE3_S>(); // Publication completes before DB or UB reuse.
    }

    __aicore__ inline void Ring(uint64_t address, uint64_t value)
    {
        st_dev(value, reinterpret_cast<__gm__ uint64_t*>(address), 0);
        AscendC::PipeBarrier<PIPE_MTE3>();
    }

    __aicore__ inline void ReadCqe(uint64_t address)
    {
        Invalidate(address, RdmaWire::kEntryBytes);
        AscendC::GlobalTensor<uint8_t> source;
        source.SetGlobalBuffer(reinterpret_cast<__gm__ uint8_t*>(address), RdmaWire::kEntryBytes);
        const AscendC::DataCopyExtParams params{1U, RdmaWire::kEntryBytes, 0U, 0U, 0U};
        const AscendC::DataCopyPadExtParams<uint8_t> padding{false, 0U, 0U, 0U};
        // Scalar decoding has finished before a subsequent poll overwrites UB.
        Sync<AscendC::HardEvent::S_MTE2>();
        AscendC::DataCopyPad(scratch_[RdmaWire::kCqeOffset], source, params, padding);
        Sync<AscendC::HardEvent::MTE2_S>();
    }

private:
    uint64_t stateAddress_;
    AscendC::LocalTensor<uint8_t> scratch_;
};

__aicore__ inline TileXRRDMADeviceView LoadView(const __gm__ TileXRRDMADeviceView* deviceView)
{
    Invalidate(reinterpret_cast<uint64_t>(deviceView), 8U);
    // Ascend C address spaces forbid aggregate assignment from __gm__ to a
    // local POD. Load each scalar explicitly, including the nested queue.
    TileXRRDMADeviceView view = {};
    view.version = deviceView->version;
    view.structBytes = deviceView->structBytes;
    // Reject a smaller V1 view before touching V2-only fields.
    if (view.version != TILEXR_RDMA_DEVICE_VERSION || view.structBytes != sizeof(view)) { return view; }
    Invalidate(reinterpret_cast<uint64_t>(deviceView), sizeof(TileXRRDMADeviceView));
    view.localAddress = deviceView->localAddress;
    view.localBytes = deviceView->localBytes;
    view.remoteAddress = deviceView->remoteAddress;
    view.remoteBytes = deviceView->remoteBytes;
    view.localKey = deviceView->localKey;
    view.remoteKey = deviceView->remoteKey;
    view.queue.sq = deviceView->queue.sq;
    view.queue.cq = deviceView->queue.cq;
    view.queue.sqDbr = deviceView->queue.sqDbr;
    view.queue.cqDbr = deviceView->queue.cqDbr;
    view.queue.doorbell = deviceView->queue.doorbell;
    view.queue.state = deviceView->queue.state;
    view.queue.sqDbValue = deviceView->queue.sqDbValue;
    view.queue.doorbellBytes = deviceView->queue.doorbellBytes;
    view.queue.qpn = deviceView->queue.qpn;
    view.queue.cqn = deviceView->queue.cqn;
    view.queue.sqDepth = deviceView->queue.sqDepth;
    view.queue.sqEntryBytes = deviceView->queue.sqEntryBytes;
    view.queue.cqDepth = deviceView->queue.cqDepth;
    view.queue.cqEntryBytes = deviceView->queue.cqEntryBytes;
    view.queue.dbMode = deviceView->queue.dbMode;
    view.queue.queueAbi = deviceView->queue.queueAbi;
    view.queue.maxSendSge = deviceView->queue.maxSendSge;
    view.queue.maxSendWr = deviceView->queue.maxSendWr;
    return view;
}

} // namespace RdmaDeviceDetail
#endif

/* Synchronous strided batch: all WRs are signaled, with up to maxInFlight
 * outstanding per QP. The effective window also reserves one SQ/CQ entry.
 * localStride=0 reuses a source; strides and offsets are bytes. The whole
 * span is validated before posting. Success completes the whole batch.
 * scratch is caller-owned UB >=TILEXR_RDMA_DEVICE_SCRATCH_BYTES bytes,
 * aligned32 and alive for the call.
 * QP/state ownership is exclusive across AIVs, streams AND registrations.
 * A timeout/error retains the in-flight producer and poisons the shared QP;
 * synchronize and destroy it before freeing any potentially referenced memory.
 * maxPolls bounds snapshots per CQE, not a wall-clock duration. */
/* A trace observer records scalar issue/completion boundaries. In particular,
 * DoorbellIssued is not an acknowledgement that the NIC consumed the DB. */
template <typename Trace>
__aicore__ inline uint32_t RDMAPutStridedBatch(const __gm__ TileXRRDMADeviceView* deviceView,
    uint64_t localOffset, uint64_t remoteOffset, uint32_t bytes, uint32_t count,
    uint64_t localStride, uint64_t remoteStride, const AscendC::LocalTensor<uint8_t>& scratch,
    uint32_t maxInFlight, uint32_t maxPolls, Trace& trace)
{
    trace.Stage(RdmaWire::BatchTraceStage::Enter);
#if defined(__NPU_ARCH__) && (__NPU_ARCH__ == 3510)
    const uint64_t address = reinterpret_cast<uint64_t>(deviceView);
    if (deviceView == nullptr || (address & 7U) != 0U ||
        !RdmaWire::Span(address, sizeof(TileXRRDMADeviceView)) ||
        scratch.GetSize() < TILEXR_RDMA_DEVICE_SCRATCH_BYTES ||
        (static_cast<uint64_t>(scratch.GetPhyAddr()) & (TILEXR_RDMA_DEVICE_SCRATCH_ALIGNMENT - 1U)) != 0U) {
        return TILEXR_RDMA_DEVICE_INVALID;
    }
    const auto view = RdmaDeviceDetail::LoadView(deviceView);
    trace.Stage(RdmaWire::BatchTraceStage::ViewLoaded);
    RdmaDeviceDetail::QueueIO io(view.queue.state, scratch);
    return RdmaWire::PutStridedBatch(view, localOffset, remoteOffset, bytes, count,
        localStride, remoteStride, maxInFlight, maxPolls, io, trace);
#else
    (void)deviceView;
    (void)localOffset;
    (void)remoteOffset;
    (void)bytes;
    (void)count;
    (void)localStride;
    (void)remoteStride;
    (void)maxInFlight;
    (void)scratch;
    (void)maxPolls;
    return TILEXR_RDMA_DEVICE_UNSUPPORTED;
#endif
}

__aicore__ inline uint32_t RDMAPutStridedBatch(const __gm__ TileXRRDMADeviceView* deviceView,
    uint64_t localOffset, uint64_t remoteOffset, uint32_t bytes, uint32_t count,
    uint64_t localStride, uint64_t remoteStride, const AscendC::LocalTensor<uint8_t>& scratch,
    uint32_t maxInFlight = TILEXR_RDMA_DEVICE_DEFAULT_MAX_IN_FLIGHT, uint32_t maxPolls = 1000000U)
{
    RdmaWire::NoBatchTrace trace;
    return RDMAPutStridedBatch(deviceView, localOffset, remoteOffset, bytes, count,
        localStride, remoteStride, scratch, maxInFlight, maxPolls, trace);
}

/* ONE WQE gathers 1..maxSendSge local segments (within this view's MR) and
 * concatenates them into ONE remote span, in list order. sgeList is a kernel
 * local C++ array, stable for this synchronous call. No payload staging copy or
 * Host post_send is used. The SQ must have room for the entire WQE and stop
 * before its physical tail; this first implementation rejects crossing it.
 * Scratch, exclusivity, stream ordering and posted-error lifetime match Put. */
__aicore__ inline uint32_t RDMAPutSgeList(const __gm__ TileXRRDMADeviceView* deviceView,
    const TileXRRDMADeviceSge* sgeList, uint32_t numSge, uint64_t remoteOffset,
    const AscendC::LocalTensor<uint8_t>& scratch, uint32_t maxPolls = 1000000U)
{
#if defined(__NPU_ARCH__) && (__NPU_ARCH__ == 3510)
    const uint64_t address = reinterpret_cast<uint64_t>(deviceView);
    if (deviceView == nullptr || (address & 7U) != 0U ||
        !RdmaWire::Span(address, sizeof(TileXRRDMADeviceView)) ||
        scratch.GetSize() < TILEXR_RDMA_DEVICE_SCRATCH_BYTES ||
        (static_cast<uint64_t>(scratch.GetPhyAddr()) & (TILEXR_RDMA_DEVICE_SCRATCH_ALIGNMENT - 1U)) != 0U) {
        return TILEXR_RDMA_DEVICE_INVALID;
    }
    const auto view = RdmaDeviceDetail::LoadView(deviceView);
    RdmaDeviceDetail::QueueIO io(view.queue.state, scratch);
    return RdmaWire::PutSgeList(view, sgeList, numSge, remoteOffset, maxPolls, io);
#else
    (void)deviceView; (void)sgeList; (void)numSge; (void)remoteOffset; (void)scratch; (void)maxPolls;
    return TILEXR_RDMA_DEVICE_UNSUPPORTED;
#endif
}

/* Opt-in two-batch pipeline. Scratch must contain kPipelineScratchBytes bytes.
 * Each batch signals only its last WR; this path rejects physical ring reuse.
 * After entering this mode the QP cannot return to the all-signaled APIs. */
__aicore__ inline uint32_t RDMAPutPipeline(const __gm__ TileXRRDMADeviceView* deviceView,
    uint64_t localOffset, uint64_t remoteOffset, uint32_t bytes, uint32_t count,
    uint64_t localStride, uint64_t remoteStride, const RdmaWire::PipelineOptions& options,
    const AscendC::LocalTensor<uint8_t>& scratch, uint32_t maxPolls = 1000000U)
{
#if defined(__NPU_ARCH__) && (__NPU_ARCH__ == 3510)
    if (deviceView == nullptr || (reinterpret_cast<uint64_t>(deviceView) & 7U) != 0U ||
        !RdmaWire::Span(reinterpret_cast<uint64_t>(deviceView), sizeof(TileXRRDMADeviceView)) ||
        scratch.GetSize() < RdmaWire::kPipelineScratchBytes ||
        (static_cast<uint64_t>(scratch.GetPhyAddr()) & (TILEXR_RDMA_DEVICE_SCRATCH_ALIGNMENT - 1U)) != 0U) {
        return TILEXR_RDMA_DEVICE_INVALID;
    }
    const auto view = RdmaDeviceDetail::LoadView(deviceView);
    RdmaDeviceDetail::QueueIO io(view.queue.state, scratch);
    return RdmaWire::PutPipeline(view, localOffset, remoteOffset, bytes, count,
        localStride, remoteStride, options, maxPolls, io);
#else
    (void)deviceView; (void)localOffset; (void)remoteOffset; (void)bytes; (void)count;
    (void)localStride; (void)remoteStride; (void)options; (void)scratch; (void)maxPolls;
    return TILEXR_RDMA_DEVICE_UNSUPPORTED;
#endif
}

/* Gather each WQE from numSge local segments and concatenate into one remote
 * span. The list is stable for the call; localStride advances every segment.
 * SQ accounting uses WQEBBs, in-flight limits and CQ identities use WRs.
 * Requires kPipelineSgeScratchBytes; no SQ/CQ physical wrap is supported. */
__aicore__ inline uint32_t RDMAPutSgeListPipeline(const __gm__ TileXRRDMADeviceView* deviceView,
    const TileXRRDMADeviceSge* sgeList, uint32_t numSge, uint64_t remoteOffset, uint32_t count,
    uint64_t localStride, uint64_t remoteStride, const RdmaWire::PipelineOptions& options,
    const AscendC::LocalTensor<uint8_t>& scratch, uint32_t maxPolls = 1000000U)
{
#if defined(__NPU_ARCH__) && (__NPU_ARCH__ == 3510)
    if (deviceView == nullptr || (reinterpret_cast<uint64_t>(deviceView) & 7U) != 0U ||
        !RdmaWire::Span(reinterpret_cast<uint64_t>(deviceView), sizeof(TileXRRDMADeviceView)) ||
        scratch.GetSize() < RdmaWire::kPipelineSgeScratchBytes ||
        (static_cast<uint64_t>(scratch.GetPhyAddr()) & (TILEXR_RDMA_DEVICE_SCRATCH_ALIGNMENT - 1U)) != 0U) {
        return TILEXR_RDMA_DEVICE_INVALID;
    }
    const auto view = RdmaDeviceDetail::LoadView(deviceView);
    RdmaDeviceDetail::QueueIO io(view.queue.state, scratch);
    return RdmaWire::PutSgeListPipeline(view, sgeList, numSge, remoteOffset, count,
        localStride, remoteStride, options, maxPolls, io);
#else
    (void)deviceView; (void)sgeList; (void)numSge; (void)remoteOffset; (void)count;
    (void)localStride; (void)remoteStride; (void)options; (void)scratch; (void)maxPolls;
    return TILEXR_RDMA_DEVICE_UNSUPPORTED;
#endif
}

/* Single-WR compatibility entry: success still means that WRITE completed. */
__aicore__ inline uint32_t RDMAPut(const __gm__ TileXRRDMADeviceView* deviceView,
    uint64_t localOffset, uint64_t remoteOffset, uint32_t bytes,
    const AscendC::LocalTensor<uint8_t>& scratch, uint32_t maxPolls = 1000000U)
{
    return RDMAPutStridedBatch(deviceView, localOffset, remoteOffset, bytes, 1U,
        0U, 0U, scratch, 1U, maxPolls);
}

} // namespace TileXR
#endif
