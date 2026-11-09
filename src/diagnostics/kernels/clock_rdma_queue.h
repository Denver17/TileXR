/*
 * 功能：为 clock 专用 queue 编码并发布 64-byte WRITE，推进 SQ，并轮询 CQ 更新完成状态。
 * 调用关系：clock_sync_kernel.h 提供 IO 和 trace；WQE/CQE 格式由 tilexr_rdma_wire.h 实现。
 * 资源与生命周期：queue 和 IO 由调用方持有；非 inline source 在发送完成前必须保持有效且内容不变。
 * Purpose: Encode/publish 64-byte WRITEs on clock queues, advance the SQ and poll CQ completions.
 * Calls: clock_sync_kernel.h supplies IO/traces; tilexr_rdma_wire.h implements WQE/CQE formats.
 * Lifetime: The caller owns queues/IO; non-inline sources must remain valid and unchanged until completion.
 */
#ifndef TILEXR_DIAGNOSTICS_CLOCK_RDMA_QUEUE_H
#define TILEXR_DIAGNOSTICS_CLOCK_RDMA_QUEUE_H

#include "tilexr_rdma_wire.h"
#if defined(__CCE_AICORE__) || defined(__CCE__) || defined(__NPU_ARCH__)
#define TILEXR_CLOCK_QUEUE_INLINE __aicore__ inline
#else
#define TILEXR_CLOCK_QUEUE_INLINE inline
#endif

namespace TileXR {
namespace ClockRdmaQueue {

// Clock owns these all-signaled, single-SGE queues exclusively. Every source
// slot stays unchanged until Drain succeeds; the next round may then reuse it.
template <typename IO, typename Trace>
TILEXR_CLOCK_QUEUE_INLINE uint32_t Post(const TileXRRDMADeviceView &view,
    uint64_t source, uint64_t destination, IO &io, Trace &trace)
{
    uint32_t status = RdmaWire::ValidateWrite(view, source, destination, 64u, 1000000u);
    if (status != TILEXR_RDMA_DEVICE_SUCCESS) { return status; }
    auto state = io.LoadState();
    const auto &q = view.queue;
    if (state.error != 0) { return TILEXR_RDMA_DEVICE_POISONED; }
    if (state.reserved[0] != 0 || state.producer < state.completed ||
        state.producer >= q.sqDepth - 1u || state.cqConsumer >= q.cqDepth - 1u ||
        state.producer - state.completed >= q.maxSendWr ||
        state.producer - state.completed >= q.cqDepth - state.cqConsumer - 1u) {
        return TILEXR_RDMA_DEVICE_INVALID;
    }
    trace.Stage(RdmaWire::BatchTraceStage::Validated);
    const uint64_t first = state.producer;
    const uint64_t wr = state.cqConsumer + first - state.completed;
    ++state.producer;
    io.StoreState(state); // Error cleanup must retain every potentially posted source.
    trace.Stage(RdmaWire::BatchTraceStage::Reserved);
    RdmaWire::EncodeStop(io, state.producer, q.sqDepth);
    io.Publish(q.sq + state.producer * RdmaWire::kEntryBytes,
        RdmaWire::kStopOffset, RdmaWire::kEntryBytes);
    trace.Stage(RdmaWire::BatchTraceStage::StopPublished);
    trace.WqeStart();
    RdmaWire::EncodeWrite(io, view, first, wr, source, destination, 64u);
    trace.WqeEncoded();
    io.Publish(q.sq + first * RdmaWire::kEntryBytes, 0u, RdmaWire::kEntryBytes);
    trace.WqePublished();
    trace.Stage(RdmaWire::BatchTraceStage::WqesPublished);
    if (q.dbMode == TILEXR_RDMA_DB_SOFT) {
        RdmaWire::StoreBe(io, RdmaWire::kRecordOffset, state.producer, 4u);
        io.Publish(q.sqDbr, RdmaWire::kRecordOffset, 4u);
    }
    io.Ring(RdmaWire::SqDbAddress(q, state.producer), RdmaWire::SqDbValue(q.sqDbValue, state.producer));
    trace.Stage(RdmaWire::BatchTraceStage::DoorbellIssued);
    return TILEXR_RDMA_DEVICE_SUCCESS;
}

template <typename IO, typename Trace>
TILEXR_CLOCK_QUEUE_INLINE uint32_t Drain(const TileXRRDMADeviceView &view, IO &io,
    uint32_t maxPolls, Trace &trace)
{
    auto state = io.LoadState();
    const auto &q = view.queue;
    if (state.error != 0) { return TILEXR_RDMA_DEVICE_POISONED; }
    if (state.producer < state.completed || state.producer >= q.sqDepth ||
        state.reserved[0] != 0 || state.cqConsumer >= q.cqDepth ||
        state.producer - state.completed > q.cqDepth - state.cqConsumer) {
        return TILEXR_RDMA_DEVICE_INVALID;
    }
    const uint64_t firstCq = state.cqConsumer;
    uint32_t status = TILEXR_RDMA_DEVICE_SUCCESS;
    while (state.completed != state.producer) {
        status = TILEXR_RDMA_DEVICE_TIMEOUT;
        for (uint32_t poll = 0; poll < maxPolls; ++poll) {
            trace.PollStart();
            io.ReadCqe(q.cq + state.cqConsumer * RdmaWire::kEntryBytes);
            trace.PollRead();
            const auto result = RdmaWire::DecodeCqe(io, q, state.cqConsumer, state.cqConsumer);
            trace.PollDecoded(result.status == RdmaWire::kPending);
            if (result.status == RdmaWire::kPending) { continue; }
            if (state.cqConsumer == firstCq) { trace.Stage(RdmaWire::BatchTraceStage::FirstCqe); }
            status = result.status;
            state.syndrome = result.syndrome;
            break;
        }
        if (status != TILEXR_RDMA_DEVICE_SUCCESS) { state.error = status; break; }
        ++state.completed;
        ++state.cqConsumer;
    }
    trace.Stage(RdmaWire::BatchTraceStage::CqDrained);
    if (state.cqConsumer != firstCq) {
        RdmaWire::StoreBe(io, RdmaWire::kRecordOffset, state.cqConsumer & 0xFFFFFFu, 4u);
        io.Publish(q.cqDbr, RdmaWire::kRecordOffset, 4u);
    }
    io.StoreState(state);
    trace.Stage(RdmaWire::BatchTraceStage::Retired);
    return status;
}

template <typename IO>
TILEXR_CLOCK_QUEUE_INLINE uint32_t Drain(const TileXRRDMADeviceView &view, IO &io,
    uint32_t maxPolls = 1000000u)
{
    RdmaWire::NoBatchTrace trace;
    return Drain(view, io, maxPolls, trace);
}
} // namespace ClockRdmaQueue
} // namespace TileXR
#undef TILEXR_CLOCK_QUEUE_INLINE
#endif
