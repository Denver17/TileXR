/* Copyright (c) 2026 TileXR Project. Licensed under the Apache License, Version 2.0. */

/*
 * 功能：提供 AIV RDMA 入口与 QueueIO，缓存 view、恢复和保存 UB context，经 MTE 发布 WQE，并用 st_dev 写 doorbell。
 * 调用关系：tilexr_rdma_write.h 调用这些入口；发送与 CQE 处理使用 wire/pipeline 模板，RDMAWait 等待 pending batch。
 * 资源与生命周期：每个 QP 仅由一个 AIV 使用；调用方持有跨调用的 context UB，并显式 Restore/Save。Save 不等待 NIC 完成。
 *
 * Purpose: Provide AIV RDMA entries and QueueIO, cache views, restore/save UB contexts, publish WQEs through MTE and ring doorbells with st_dev.
 * Calls: tilexr_rdma_write.h uses these entries; wire/pipeline templates process sends/CQEs and RDMAWait drains pending batches.
 * Lifetime: One AIV exclusively uses each QP. Callers retain context UB across calls and explicitly Restore/Save; Save does not wait for the NIC.
 */
#ifndef TILEXR_RDMA_DEVICE_H
#define TILEXR_RDMA_DEVICE_H

#include "kernel_operator.h"
#include "tilexr_rdma_wire.h"
#include "tilexr_rdma_pipeline.h"
#include "tilexr_rdma_write_options.h"

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

    // AIV-owned metadata only; NIC descriptors still use UB -> MTE3 Publish.
    __aicore__ inline uint64_t LoadPending(uint64_t address)
    {
        Invalidate(address, sizeof(uint64_t));
        return *reinterpret_cast<const __gm__ uint64_t*>(address);
    }

    __aicore__ inline void StorePending(uint64_t address, uint64_t value)
    {
        *reinterpret_cast<__gm__ uint64_t*>(address) = value;
        Invalidate(address, sizeof(uint64_t));
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
    // Reject older/smaller views before touching fields in the V3 layout.
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
    view.queue.pendingBatches = deviceView->queue.pendingBatches;
    view.queue.pendingCapacity = deviceView->queue.pendingCapacity;
    view.queue.pendingReserved = deviceView->queue.pendingReserved;
    return view;
}

} // namespace RdmaDeviceDetail
#endif

/* One caller-owned UB context per exclusively owned QP. The UB header/FIFO
 * outlive every Write/Wait using it. Save explicitly before ending a kernel;
 * saving does not wait for NIC completion. Never resume an unsaved aborted kernel.
 * Restore snapshots the entire view in caller-local storage. Keep its GM identity,
 * MR keys/extents and QP metadata immutable until all calls finish; Save does not
 * invalidate the snapshot. Save/Wait and Restore again before rebinding a view. */
class RDMAWriteContext {
public:
    __aicore__ inline explicit RDMAWriteContext(const AscendC::LocalTensor<uint8_t>& storage)
        : storage_(storage) {}
    __aicore__ inline const TileXRRDMADeviceQueue& Queue() const { return view_.queue; }
    __aicore__ inline const TileXRRDMADeviceView& View() const { return view_; }
    __aicore__ inline bool Matches(const __gm__ TileXRRDMADeviceView* view) const
    { return boundView_ == nullptr || boundView_ == view; }
    __aicore__ inline RDMAWriteTrace& Trace() { return trace_; }
    __aicore__ inline bool Loaded() const { return loaded_; }
    __aicore__ inline TileXRRDMADeviceState State() const
    {
        const auto words = storage_.ReinterpretCast<uint64_t>();
        TileXRRDMADeviceState s{};
        s.producer = words.GetValue(0); s.completed = words.GetValue(1); s.cqConsumer = words.GetValue(2);
        const uint64_t errors = words.GetValue(3);
        s.error = uint32_t(errors); s.syndrome = uint32_t(errors >> 32);
        for (uint32_t i = 0; i < 4U; ++i) { s.reserved[i] = words.GetValue(4U + i); }
        return s;
    }
    __aicore__ inline void SetState(const TileXRRDMADeviceState& s)
    {
        const auto words = storage_.ReinterpretCast<uint64_t>();
        words.SetValue(0, s.producer); words.SetValue(1, s.completed); words.SetValue(2, s.cqConsumer);
        words.SetValue(3, uint64_t(s.error) | (uint64_t(s.syndrome) << 32));
        for (uint32_t i = 0; i < 4U; ++i) { words.SetValue(4U + i, s.reserved[i]); }
    }
    __aicore__ inline uint64_t Pending(uint64_t address) const
    { return storage_.ReinterpretCast<uint64_t>().GetValue(8U + uint32_t((address - Queue().pendingBatches) / 8U)); }
    __aicore__ inline void SetPending(uint64_t address, uint64_t value)
    { storage_.ReinterpretCast<uint64_t>().SetValue(8U + uint32_t((address - Queue().pendingBatches) / 8U), value); }

    __aicore__ inline uint32_t Restore(const __gm__ TileXRRDMADeviceView* deviceView)
    {
#if defined(__NPU_ARCH__) && (__NPU_ARCH__ == 3510)
#if TILEXR_RDMA_DEBUG
        if (deviceView == nullptr || (reinterpret_cast<uint64_t>(deviceView) & 7U) != 0U) {
            return TILEXR_RDMA_DEVICE_INVALID;
        }
#endif
        return RestoreView(RdmaDeviceDetail::LoadView(deviceView), deviceView);
#else
        (void)deviceView; return TILEXR_RDMA_DEVICE_UNSUPPORTED;
#endif
    }
    // A caller-supplied snapshot has no GM identity unless explicitly supplied.
    __aicore__ inline uint32_t RestoreView(const TileXRRDMADeviceView& view,
        const __gm__ TileXRRDMADeviceView* deviceView = nullptr)
    {
#if defined(__NPU_ARCH__) && (__NPU_ARCH__ == 3510)
        RDMAWriteTraceScope interval(&trace_, RDMAWriteEvent::RestoreBegin, RDMAWriteEvent::RestoreEnd);
        const uint32_t status = RdmaWire::ValidatePipelineView(view);
        if (status != 0U) { return status; }
        const uint32_t needed = RDMAWriteContextBytes(view.queue.pendingCapacity);
        if (needed == 0U || storage_.GetSize() < needed ||
            (static_cast<uint64_t>(storage_.GetPhyAddr()) & 31U) != 0U) {
            return TILEXR_RDMA_DEVICE_INVALID;
        }
        loaded_ = false;
        view_ = view;
        boundView_ = deviceView;
        CopyIn(Queue().state, storage_, sizeof(TileXRRDMADeviceState));
        const auto state = State();
        if (state.reserved[0] != 0U && state.reserved[0] != RdmaWire::kPipelineStateTag) {
            return TILEXR_RDMA_DEVICE_UNSUPPORTED;
        }
        const uint32_t count = RdmaWire::PendingBatchCount(state);
        if (count > Queue().pendingCapacity) { return TILEXR_RDMA_DEVICE_INVALID; }
        TransferPending(state.cqConsumer, count, true);
        loaded_ = true;
        return TILEXR_RDMA_DEVICE_SUCCESS;
#else
        (void)view; (void)deviceView; return TILEXR_RDMA_DEVICE_UNSUPPORTED;
#endif
    }
    __aicore__ inline uint32_t Save()
    {
#if defined(__NPU_ARCH__) && (__NPU_ARCH__ == 3510)
        RDMAWriteTraceScope interval(&trace_, RDMAWriteEvent::SaveBegin, RDMAWriteEvent::SaveEnd);
        if (!loaded_) { return TILEXR_RDMA_DEVICE_INVALID; }
        const auto state = State();
        const uint32_t count = RdmaWire::PendingBatchCount(state);
        if (count > Queue().pendingCapacity) { return TILEXR_RDMA_DEVICE_INVALID; }
        TransferPending(state.cqConsumer, count, false);
        CopyOut(Queue().state, storage_, sizeof(TileXRRDMADeviceState));
        return TILEXR_RDMA_DEVICE_SUCCESS;
#else
        return TILEXR_RDMA_DEVICE_UNSUPPORTED;
#endif
    }
private:
#if defined(__NPU_ARCH__) && (__NPU_ARCH__ == 3510)
    __aicore__ inline static void CopyIn(uint64_t address, const AscendC::LocalTensor<uint8_t>& ub, uint32_t bytes)
    {
        RdmaDeviceDetail::Invalidate(address, bytes);
        AscendC::GlobalTensor<uint8_t> gm;
        gm.SetGlobalBuffer(reinterpret_cast<__gm__ uint8_t*>(address), bytes);
        const AscendC::DataCopyExtParams params{1U, bytes, 0U, 0U, 0U};
        const AscendC::DataCopyPadExtParams<uint8_t> pad{false, 0U, 0U, 0U};
        RdmaDeviceDetail::Sync<AscendC::HardEvent::S_MTE2>();
        AscendC::DataCopyPad(ub, gm, params, pad);
        RdmaDeviceDetail::Sync<AscendC::HardEvent::MTE2_S>();
    }
    __aicore__ inline static void CopyOut(uint64_t address, const AscendC::LocalTensor<uint8_t>& ub, uint32_t bytes)
    {
        AscendC::GlobalTensor<uint8_t> gm;
        gm.SetGlobalBuffer(reinterpret_cast<__gm__ uint8_t*>(address), bytes);
        const AscendC::DataCopyExtParams params{1U, bytes, 0U, 0U, 0U};
        RdmaDeviceDetail::Sync<AscendC::HardEvent::S_MTE3>();
        AscendC::DataCopyPad(gm, ub, params);
        RdmaDeviceDetail::Sync<AscendC::HardEvent::MTE3_S>();
        RdmaDeviceDetail::Invalidate(address, bytes);
    }
    __aicore__ inline void TransferPending(uint64_t consumer, uint32_t count, bool restore)
    {
        const uint32_t first = uint32_t(consumer % Queue().pendingCapacity);
        const uint32_t tail = count < Queue().pendingCapacity - first ? count : Queue().pendingCapacity - first;
        for (uint32_t part = 0U; part < 2U; ++part) {
            const uint32_t n = part == 0U ? tail : count - tail;
            if (n == 0U) { continue; }
            const uint32_t begin = part == 0U ? first : 0U;
            const uint32_t alignedBegin = begin & ~3U;
            const uint32_t roundedEnd = (begin + n + 3U) & ~3U;
            const uint32_t end = roundedEnd < Queue().pendingCapacity ? roundedEnd : Queue().pendingCapacity;
            const uint32_t offset = alignedBegin * 8U;
            // DataCopyPad requires aligned UB endpoints. Include neighboring
            // records, never bytes beyond the actual GM allocation. On save,
            // zero only inactive padding; wrapped active records must survive.
            if (!restore) {
                for (uint32_t i = alignedBegin; i < begin; ++i) {
                    if ((uint64_t(i) + Queue().pendingCapacity - first) % Queue().pendingCapacity >= count) {
                        storage_.ReinterpretCast<uint64_t>().SetValue(8U + i, 0U);
                    }
                }
                for (uint32_t i = begin + n; i < end; ++i) {
                    if ((uint64_t(i) + Queue().pendingCapacity - first) % Queue().pendingCapacity >= count) {
                        storage_.ReinterpretCast<uint64_t>().SetValue(8U + i, 0U);
                    }
                }
            }
            if (restore) { CopyIn(Queue().pendingBatches + offset, storage_[64U + offset], (end - alignedBegin) * 8U); }
            else { CopyOut(Queue().pendingBatches + offset, storage_[64U + offset], (end - alignedBegin) * 8U); }
        }
    }
#endif
    AscendC::LocalTensor<uint8_t> storage_;
    TileXRRDMADeviceView view_{};
    const __gm__ TileXRRDMADeviceView* boundView_ = nullptr;
    RDMAWriteTrace trace_;
    bool loaded_ = false;
};

#if defined(__NPU_ARCH__) && (__NPU_ARCH__ == 3510)
namespace RdmaDeviceDetail {
class ContextIO : public QueueIO {
public:
    __aicore__ inline ContextIO(RDMAWriteContext& context, const AscendC::LocalTensor<uint8_t>& scratch)
        : QueueIO(context.Queue().state, scratch), context_(context) {}
    __aicore__ inline TileXRRDMADeviceState LoadState() { return context_.State(); }
    __aicore__ inline void StoreState(const TileXRRDMADeviceState& state) { context_.SetState(state); }
    __aicore__ inline uint64_t LoadPending(uint64_t address) { return context_.Pending(address); }
    __aicore__ inline void StorePending(uint64_t address, uint64_t value) { context_.SetPending(address, value); }
private:
    RDMAWriteContext& context_;
};
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

/* Shared adapter: explicit contexts never touch GM metadata in this path;
 * compatibility calls restore/save a temporary context in the scratch suffix. */
__aicore__ inline uint32_t RDMARunWritePipeline(const __gm__ TileXRRDMADeviceView* deviceView,
    const TileXRRDMADeviceSge* sges, uint32_t numSge, uint64_t remoteOffset, uint32_t count,
    uint64_t localStride, uint64_t remoteStride, const RdmaWire::PipelineOptions& options,
    const AscendC::LocalTensor<uint8_t>& scratch, uint32_t maxPolls, RDMAWriteContext* context, bool wait)
{
#if defined(__NPU_ARCH__) && (__NPU_ARCH__ == 3510)
    RDMAWriteTrace* trace = context != nullptr ? &context->Trace() : nullptr;
    const uint32_t workBytes = wait ? TILEXR_RDMA_DEVICE_SCRATCH_BYTES : RDMAWriteWorkScratchBytes(numSge);
#if TILEXR_RDMA_DEBUG
    if (deviceView == nullptr || (reinterpret_cast<uint64_t>(deviceView) & 7U) != 0U ||
        !RdmaWire::Span(reinterpret_cast<uint64_t>(deviceView), sizeof(TileXRRDMADeviceView)) ||
        workBytes == 0U || scratch.GetSize() < workBytes ||
        (static_cast<uint64_t>(scratch.GetPhyAddr()) & 31U) != 0U) { return TILEXR_RDMA_DEVICE_INVALID; }
#endif
    TileXRRDMADeviceView loadedView;
    const TileXRRDMADeviceView* selectedView;
    {
        RDMAWriteTraceScope interval(trace, RDMAWriteEvent::ViewBegin, RDMAWriteEvent::ViewEnd);
        if (context != nullptr) { selectedView = &context->View(); }
        else { loadedView = RdmaDeviceDetail::LoadView(deviceView); selectedView = &loadedView; }
    }
    const auto& view = *selectedView;
    if (view.version != TILEXR_RDMA_DEVICE_VERSION || view.structBytes != sizeof(view)) {
        return TILEXR_RDMA_DEVICE_UNSUPPORTED;
    }
    if (context != nullptr) {
#if TILEXR_RDMA_DEBUG
        if (!context->Loaded() || !context->Matches(deviceView)) { return TILEXR_RDMA_DEVICE_INVALID; }
#endif
        RdmaDeviceDetail::ContextIO io(*context, scratch);
        return wait ? RdmaWire::WaitPipeline(view, maxPolls, io, trace) :
            RdmaWire::PutSgeListPipeline(view, sges, numSge, remoteOffset, count,
                localStride, remoteStride, options, maxPolls, io, trace);
    }
    const uint32_t contextBytes = RDMAWriteContextBytes(view.queue.pendingCapacity);
#if TILEXR_RDMA_DEBUG
    if (contextBytes == 0U || contextBytes > scratch.GetSize() - workBytes) {
        return TILEXR_RDMA_DEVICE_INVALID;
    }
#else
    (void)contextBytes;
#endif
    RDMAWriteContext temporary(scratch[workBytes]);
    const uint32_t restored = temporary.RestoreView(view);
    if (restored != 0U) { return restored; }
    RdmaDeviceDetail::ContextIO io(temporary, scratch);
    const uint32_t status = wait ? RdmaWire::WaitPipeline(view, maxPolls, io) :
        RdmaWire::PutSgeListPipeline(view, sges, numSge, remoteOffset, count,
            localStride, remoteStride, options, maxPolls, io);
    const uint32_t saved = temporary.Save();
    return status != 0U ? status : saved;
#else
    (void)deviceView; (void)sges; (void)numSge; (void)remoteOffset; (void)count;
    (void)localStride; (void)remoteStride; (void)options; (void)scratch; (void)maxPolls; (void)context; (void)wait;
    return TILEXR_RDMA_DEVICE_UNSUPPORTED;
#endif
}

/* Submit-only capacity-driven pipeline. Compatibility scratch includes a UB context suffix.
 * Each batch signals only its last WR; this path rejects physical ring reuse.
 * Backpressure may poll CQEs. RDMAWait supplies the final completion boundary.
 * After entering this mode the QP cannot return to the all-signaled APIs. */
__aicore__ inline uint32_t RDMAPutPipeline(const __gm__ TileXRRDMADeviceView* deviceView,
    uint64_t localOffset, uint64_t remoteOffset, uint32_t bytes, uint32_t count,
    uint64_t localStride, uint64_t remoteStride, const RdmaWire::PipelineOptions& options,
    const AscendC::LocalTensor<uint8_t>& scratch, uint32_t maxPolls = 1000000U,
    RDMAWriteContext* context = nullptr)
{
    const TileXRRDMADeviceSge sge{localOffset, bytes, 0U};
    return RDMARunWritePipeline(deviceView, &sge, 1U, remoteOffset, count, localStride,
        remoteStride, options, scratch, maxPolls, context, false);
}

/* Submit each WQE from numSge local segments into one contiguous remote
 * span. The list is stable for the call; localStride advances every segment.
 * SQ accounting uses WQEBBs, in-flight limits and CQ identities use WRs.
 * Explicit contexts need work scratch; compatibility calls also need the
 * temporary context suffix. No SQ/CQ physical wrap is supported. */
__aicore__ inline uint32_t RDMAPutSgeListPipeline(const __gm__ TileXRRDMADeviceView* deviceView,
    const TileXRRDMADeviceSge* sgeList, uint32_t numSge, uint64_t remoteOffset, uint32_t count,
    uint64_t localStride, uint64_t remoteStride, const RdmaWire::PipelineOptions& options,
    const AscendC::LocalTensor<uint8_t>& scratch, uint32_t maxPolls = 1000000U,
    RDMAWriteContext* context = nullptr)
{
    return RDMARunWritePipeline(deviceView, sgeList, numSge, remoteOffset, count, localStride,
        remoteStride, options, scratch, maxPolls, context, false);
}

/* Drain all pending pipeline batches on this QP, including other MR views.
 * Compatibility scratch includes a temporary context; explicit context needs only 512B of work scratch.
 * maxPolls bounds polling for each pending batch, not wall-clock elapsed time.
 * Empty/repeated waits do not poll. Success is a send-completion boundary,
 * not remote-consumer synchronization. Errors retain/poison the posted QP. */
__aicore__ inline uint32_t RDMAWait(const __gm__ TileXRRDMADeviceView* deviceView,
    const AscendC::LocalTensor<uint8_t>& scratch, uint32_t maxPolls = 1000000U)
{
    const RdmaWire::PipelineOptions options{};
    return RDMARunWritePipeline(deviceView, nullptr, 1U, 0U, 0U, 0U, 0U, options,
        scratch, maxPolls, nullptr, true);
}

__aicore__ inline uint32_t RDMAWait(const __gm__ TileXRRDMADeviceView* deviceView,
    const AscendC::LocalTensor<uint8_t>& scratch, RDMAWriteContext& context, uint32_t maxPolls = 1000000U)
{
    const RdmaWire::PipelineOptions options{};
    return RDMARunWritePipeline(deviceView, nullptr, 1U, 0U, 0U, 0U, 0U, options,
        scratch, maxPolls, &context, true);
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
