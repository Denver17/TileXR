/* Copyright (c) 2026 TileXR Project. Licensed under the Apache License, Version 2.0. */

/*
 * 功能：编码并分批提交 WRITE，按 QP 容量和应用上限执行 CQ 背压，通过显式 Wait 处理剩余 completion；拒绝 SQ/CQ 物理回绕。
 * 调用关系：device adapter 调用本文件的模板；wire 提供布局、编码和 CQE 校验，IO 提供队列及 state/pending 读写。
 * 资源与生命周期：模板不分配资源；提交成功后仍可能有 pending WR，payload 和 MR 必须保留到 completion 或成功 teardown。
 *
 * Purpose: Encode and batch WRITE submissions, poll CQ under capacity/application backpressure and explicitly wait for remaining completions;
 *          reject physical SQ/CQ wraparound.
 * Calls: Device adapters invoke these templates; wire supplies layout/encoding/CQE checks and IO provides queue/state/pending access.
 * Lifetime: Templates allocate no resources. Successful submission may leave pending WRs; retain payloads/MRs until completion or successful teardown.
 */
#ifndef TILEXR_RDMA_PIPELINE_H
#define TILEXR_RDMA_PIPELINE_H
#include "tilexr_rdma_wire.h"
#include "tilexr_rdma_write_trace.h"
#if defined(__CCE_AICORE__) || defined(__CCE__) || defined(__NPU_ARCH__)
#define TILEXR_RDMA_WIRE_INLINE __aicore__ inline
#else
#define TILEXR_RDMA_WIRE_INLINE inline
#endif

namespace TileXR {
namespace RdmaWire {
constexpr uint32_t kPipelineBatchMax = 64U;
constexpr uint32_t kPipelineWqeOffset = TILEXR_RDMA_DEVICE_SCRATCH_BYTES;
constexpr uint32_t kPipelineScratchBytes = kPipelineWqeOffset + kPipelineBatchMax * kEntryBytes;
constexpr uint32_t kPipelineSgeScratchBytes = kPipelineWqeOffset + kPipelineBatchMax * kMaxWqeBytes;
constexpr uint64_t kPipelineStateTag = 0x5458525049504532ULL;

/* This opt-in path is deliberately non-wrapping. reserved[0]
 * tags its state; reserved[1] is the next WR ordinal, independent of CQ CI;
 * reserved[2] is the next batch ordinal; reserved[3] counts pending WRs.
 * Production IO keeps batch records in UB and checkpoints the Host-allocated FIFO.
 * A fully drained all-signaled QP can enter it; the reverse transition is
 * rejected by the old APIs because their CQ ordinal assumption no longer holds. */
struct PipelineOptions {
    uint32_t postBatch = 64U;
    uint32_t maxInFlight = UINT32_MAX; // default: derive the limit from the actual QP.
    uint32_t cyclicSlots = 0U; // zero: use strides; otherwise both spans cycle.
};

/* Non-wrapping SQ positions fit in 32 bits. The CQE matches only the low
 * 16 WR bits; retain the full next-WR ordinal separately in reserved[1].
 * Store WR count (1..64) too: SQ WQEBBs are not WRs for multi-SGE batches. */
TILEXR_RDMA_WIRE_INLINE uint64_t PendingBatch(uint64_t endSq, uint64_t lastWr, uint32_t count)
{
    return uint32_t(endSq) | ((lastWr & 0xFFFFU) << 32) | (uint64_t(count) << 48);
}

TILEXR_RDMA_WIRE_INLINE uint32_t PendingBatchCount(const TileXRRDMADeviceState& state)
{
    return state.reserved[0] == 0U ? 0U : uint32_t(state.reserved[2] - state.cqConsumer);
}

TILEXR_RDMA_WIRE_INLINE uint32_t PendingWrCount(const TileXRRDMADeviceState& state)
{
    return uint32_t(state.reserved[3]);
}

TILEXR_RDMA_WIRE_INLINE uint32_t ValidatePipelineView(const TileXRRDMADeviceView& v)
{
    const uint32_t status = ValidateView(v);
    if (status != 0U) { return status; }
    const auto& q = v.queue;
    if (q.pendingReserved != 0U || q.pendingCapacity == 0U || (q.pendingBatches & 63U) != 0U ||
        !Span(q.pendingBatches, uint64_t(q.pendingCapacity) * sizeof(uint64_t))) {
        return TILEXR_RDMA_DEVICE_INVALID;
    }
    return TILEXR_RDMA_DEVICE_SUCCESS;
}

TILEXR_RDMA_WIRE_INLINE uint32_t BigEndianWord(uint32_t x)
{
    return (x >> 24) | ((x >> 8) & 0xFF00U) | ((x << 8) & 0xFF0000U) | (x << 24);
}

template <typename IO>
TILEXR_RDMA_WIRE_INLINE void EncodePipelineSgeWrite(IO& io, uint32_t slot,
    const TileXRRDMADeviceView& v, uint64_t sqSequence, uint64_t wrSequence,
    const TileXRRDMADeviceSge* sges, uint32_t numSge, uint64_t localDelta,
    uint64_t remoteOffset, uint32_t totalBytes, bool signal)
{
    const uint32_t wqeBytes = WqeBlocks(numSge) * kEntryBytes;
    const uint32_t base = kPipelineWqeOffset + slot * wqeBytes;
    for (uint32_t i = 0; i < wqeBytes; i += 4U) { io.SetWord(base + i, 0U); }
    const uint32_t owner = 0x40U | ((sqSequence & v.queue.sqDepth) ? 0x80U : 0U);
    const uint32_t ssn = (numSge * 2U) | (uint32_t(wrSequence & 3U) << 12);
    const uint32_t control = signal ? 0xA4U : 0x24U;
    io.SetWord(base, owner | (control << 8) | ((ssn >> 8) << 16) | ((ssn & 255U) << 24));
    io.SetWord(base + 4U, BigEndianWord(0x10000000U));
    io.SetWord(base + 16U, BigEndianWord(signal ? 0x24000000U : 0x04000000U));
    io.SetWord(base + 20U, BigEndianWord(totalBytes));
    const uint64_t remote = v.remoteAddress + remoteOffset;
    io.SetWord(base + 32U, BigEndianWord(uint32_t(remote >> 32)));
    io.SetWord(base + 36U, BigEndianWord(uint32_t(remote)));
    io.SetWord(base + 40U, BigEndianWord(v.remoteKey));
    io.SetWord(base + 44U, BigEndianWord(v.localKey & 0xFFFFFF00U));
    for (uint32_t i = 0; i < numSge; ++i) {
        const uint32_t offset = base + 48U + 16U * i;
        const uint64_t address = v.localAddress + sges[i].localOffset + localDelta;
        io.SetWord(offset, BigEndianWord(uint32_t(address >> 32)));
        io.SetWord(offset + 4U, BigEndianWord(uint32_t(address)));
        io.SetWord(offset + 8U, BigEndianWord(sges[i].bytes));
        const uint32_t key = (v.localKey & 0x7FFFFFFFU) | (i + 1U == numSge ? 0x80000000U : 0U);
        io.SetWord(offset + 12U, BigEndianWord(key));
    }
}

template <typename IO>
TILEXR_RDMA_WIRE_INLINE void EncodePipelineWrite(IO& io, uint32_t slot,
    const TileXRRDMADeviceView& v, uint64_t sqSequence, uint64_t wrSequence,
    uint64_t localOffset, uint64_t remoteOffset, uint32_t bytes, bool signal)
{
    const uint32_t base = kPipelineWqeOffset + slot * kEntryBytes;
    for (uint32_t i = 0; i < 16U; ++i) { io.SetWord(base + i * 4U, 0U); }
    const uint32_t owner = 0x40U | ((sqSequence & v.queue.sqDepth) ? 0x80U : 0U);
    const uint32_t ssn = 2U | (uint32_t(wrSequence & 3U) << 12);
    const uint32_t control = signal ? 0xA4U : 0x24U;
    io.SetWord(base, owner | (control << 8) | ((ssn >> 8) << 16) | ((ssn & 255U) << 24));
    io.SetWord(base + 4U, BigEndianWord(0x10000000U));
    io.SetWord(base + 16U, BigEndianWord(signal ? 0x24000000U : 0x04000000U));
    io.SetWord(base + 20U, BigEndianWord(bytes));
    const uint64_t remote = v.remoteAddress + remoteOffset;
    const uint64_t local = v.localAddress + localOffset;
    io.SetWord(base + 32U, BigEndianWord(uint32_t(remote >> 32)));
    io.SetWord(base + 36U, BigEndianWord(uint32_t(remote)));
    io.SetWord(base + 40U, BigEndianWord(v.remoteKey));
    io.SetWord(base + 44U, BigEndianWord(v.localKey & 0xFFFFFF00U));
    io.SetWord(base + 48U, BigEndianWord(uint32_t(local >> 32)));
    io.SetWord(base + 52U, BigEndianWord(uint32_t(local)));
    io.SetWord(base + 56U, BigEndianWord(bytes));
    io.SetWord(base + 60U, BigEndianWord(v.localKey | 0x80000000U));
}

template <typename IO>
TILEXR_RDMA_WIRE_INLINE uint32_t DrainPipelineBatch(const TileXRRDMADeviceQueue& q,
    uint32_t maxPolls, TileXRRDMADeviceState& state, IO& io)
{
    const uint64_t pending = io.LoadPending(q.pendingBatches +
        (state.cqConsumer % q.pendingCapacity) * sizeof(uint64_t));
    uint32_t status = TILEXR_RDMA_DEVICE_TIMEOUT;
    for (uint32_t poll = 0; poll < maxPolls; ++poll) {
        io.ReadCqe(q.cq + state.cqConsumer * kEntryBytes);
        const auto cqe = DecodeCqe(io, q, state.cqConsumer, (pending >> 32) & 0xFFFFU);
        if (cqe.status == kPending) { continue; }
        status = cqe.status;
        state.syndrome = cqe.syndrome;
        break;
    }
    if (status == TILEXR_RDMA_DEVICE_SUCCESS) {
        state.completed = uint32_t(pending);
        ++state.cqConsumer;
        state.reserved[3] -= pending >> 48;
        StoreBe(io, kRecordOffset, state.cqConsumer & 0xFFFFFFU, 4U);
        io.Publish(q.cqDbr, kRecordOffset, 4U);
    } else {
        state.error = status;
    }
    io.StoreState(state);
    return status;
}

template <typename IO>
TILEXR_RDMA_WIRE_INLINE uint32_t WaitPipeline(const TileXRRDMADeviceView& v,
    uint32_t maxPolls, IO& io, RDMAWriteTrace* trace = nullptr)
{
    RDMAWriteTraceScope interval(trace, RDMAWriteEvent::WaitBegin, RDMAWriteEvent::WaitEnd);
#if TILEXR_RDMA_DEBUG
    const uint32_t status = ValidatePipelineView(v);
    if (status != 0U) { return status; }
    if (maxPolls == 0U) { return TILEXR_RDMA_DEVICE_INVALID; }
#else
    if (v.version != TILEXR_RDMA_DEVICE_VERSION || v.structBytes != sizeof(v)) {
        return TILEXR_RDMA_DEVICE_UNSUPPORTED;
    }
#endif
    auto state = io.LoadState();
    if (state.error != 0U) { return TILEXR_RDMA_DEVICE_POISONED; }
    if (state.reserved[0] == 0U) {
        return state.producer == state.completed ? TILEXR_RDMA_DEVICE_SUCCESS : TILEXR_RDMA_DEVICE_BUSY;
    }
    if (state.reserved[0] != kPipelineStateTag) { return TILEXR_RDMA_DEVICE_UNSUPPORTED; }
    while (PendingBatchCount(state) != 0U) {
        const uint32_t result = DrainPipelineBatch(v.queue, maxPolls, state, io);
        if (result != 0U) { return result; }
    }
    return TILEXR_RDMA_DEVICE_SUCCESS;
}

template <typename IO>
TILEXR_RDMA_WIRE_INLINE uint32_t PutSgeListPipeline(const TileXRRDMADeviceView& v,
    const TileXRRDMADeviceSge* sges, uint32_t numSge, uint64_t remoteOffset, uint32_t count,
    uint64_t localStride, uint64_t remoteStride, const PipelineOptions& options,
    uint32_t maxPolls, IO& io, RDMAWriteTrace* trace = nullptr)
{
    uint32_t status = 0U;
    uint32_t window = 0U, batch = 0U;
    uint64_t totalBytes = 0U;
    {
        RDMAWriteTraceScope interval(trace, RDMAWriteEvent::ValidateBegin, RDMAWriteEvent::ValidateEnd);
#if TILEXR_RDMA_DEBUG
        status = ValidatePipelineView(v);
        if (status != 0U) { return status; }
        if (sges == nullptr || numSge == 0U || numSge > TILEXR_RDMA_DEVICE_MAX_SGE || maxPolls == 0U) {
            return TILEXR_RDMA_DEVICE_INVALID;
        }
        if (numSge > v.queue.maxSendSge) { return TILEXR_RDMA_DEVICE_UNSUPPORTED; }
#else
        if (v.version != TILEXR_RDMA_DEVICE_VERSION || v.structBytes != sizeof(v)) {
            return TILEXR_RDMA_DEVICE_UNSUPPORTED;
        }
#endif
        window = PipelineWindow(v.queue, numSge, options.maxInFlight);
        batch = options.postBatch < window ? options.postBatch : window;
#if TILEXR_RDMA_DEBUG
        const uint32_t spans = options.cyclicSlots == 0U ? count : options.cyclicSlots;
        if (count == 0U || batch == 0U || options.postBatch == 0U || options.postBatch > kPipelineBatchMax) {
            return TILEXR_RDMA_DEVICE_INVALID;
        }
#endif
        for (uint32_t i = 0; i < numSge; ++i) {
#if TILEXR_RDMA_DEBUG
            if (sges[i].reserved != 0U || sges[i].bytes == 0U || sges[i].bytes >= 0x80000000U ||
                !BatchFits(v.localBytes, sges[i].localOffset, sges[i].bytes, localStride, spans)) {
                return TILEXR_RDMA_DEVICE_INVALID;
            }
#endif
            totalBytes += sges[i].bytes;
        }
#if TILEXR_RDMA_DEBUG
        if (totalBytes >= 0x80000000ULL ||
            !BatchFits(v.remoteBytes, remoteOffset, uint32_t(totalBytes), remoteStride, spans)) {
            return TILEXR_RDMA_DEVICE_INVALID;
        }
#endif
    }
    TileXRRDMADeviceState state;
    {
        RDMAWriteTraceScope interval(trace, RDMAWriteEvent::StateBegin, RDMAWriteEvent::StateEnd);
        state = io.LoadState();
    }
    if (state.error != 0U) { return TILEXR_RDMA_DEVICE_POISONED; }
    if (state.reserved[0] == 0U && state.producer != state.completed) {
        return TILEXR_RDMA_DEVICE_BUSY;
    }
    if (state.reserved[0] != 0U && state.reserved[0] != kPipelineStateTag) {
        return TILEXR_RDMA_DEVICE_UNSUPPORTED;
    }
    const uint64_t completions = PendingBatchCount(state) + (uint64_t(count) + batch - 1U) / batch;
    const uint32_t blocks = WqeBlocks(numSge);
    const uint64_t sqBlocks = uint64_t(count) * blocks;
    const auto& q = v.queue;
    // No modulo reuse: validate the whole call, including the invalid stop,
    // before reserving, publishing or ringing anything.
    if (state.producer >= q.sqDepth || sqBlocks >= q.sqDepth - state.producer ||
        state.cqConsumer >= q.cqDepth || completions >= q.cqDepth - state.cqConsumer) {
        return TILEXR_RDMA_DEVICE_INVALID;
    }
    uint64_t wr = state.reserved[0] == 0U ? state.cqConsumer : state.reserved[1];
    if (uint64_t(count) > UINT64_MAX - wr) { return TILEXR_RDMA_DEVICE_INVALID; }
    if (state.reserved[0] == 0U) { state.reserved[2] = state.cqConsumer; }
    state.reserved[0] = kPipelineStateTag;
    uint32_t sent = 0U;
    while (sent < count) {
        const uint32_t n = count - sent < batch ? count - sent : batch;
        while (PendingBatchCount(state) == q.pendingCapacity ||
            uint64_t(PendingWrCount(state)) + n > window ||
            state.producer - state.completed + uint64_t(n) * blocks >= q.sqDepth) {
            RDMAWriteTraceScope interval(trace, RDMAWriteEvent::BackpressureBegin, RDMAWriteEvent::BackpressureEnd);
            status = DrainPipelineBatch(q, maxPolls, state, io);
            if (status != 0U) { return status; }
        }
        const uint64_t first = state.producer;
        {
            RDMAWriteTraceScope interval(trace, RDMAWriteEvent::StateBegin, RDMAWriteEvent::StateEnd);
            state.producer += uint64_t(n) * blocks;
            state.reserved[1] = wr + n;
            io.StorePending(q.pendingBatches + (state.reserved[2] % q.pendingCapacity) * sizeof(uint64_t),
                PendingBatch(state.producer, wr + n - 1U, n));
            ++state.reserved[2];
            state.reserved[3] += n;
            io.StoreState(state);
        }
        {
            RDMAWriteTraceScope interval(trace, RDMAWriteEvent::StopBegin, RDMAWriteEvent::StopEnd);
            EncodeStop(io, state.producer, q.sqDepth);
            io.Publish(q.sq + state.producer * kEntryBytes, kStopOffset, kEntryBytes);
        }
        {
            RDMAWriteTraceScope interval(trace, RDMAWriteEvent::EncodeBegin, RDMAWriteEvent::EncodeEnd);
            for (uint32_t i = 0; i < n; ++i) {
                const uint64_t index = options.cyclicSlots == 0U ? uint64_t(sent) + i :
                    (wr + i + 1U) % options.cyclicSlots;
                if (numSge == 1U) {
                    EncodePipelineWrite(io, i, v, first + i, wr + i,
                        sges[0].localOffset + index * localStride, remoteOffset + index * remoteStride,
                        sges[0].bytes, i + 1U == n);
                } else {
                    EncodePipelineSgeWrite(io, i, v, first + uint64_t(i) * blocks, wr + i,
                        sges, numSge, index * localStride, remoteOffset + index * remoteStride,
                        uint32_t(totalBytes), i + 1U == n);
                }
            }
        }
        // The first entry may be a prefetched invalid stop from the last DB.
        // Complete the tail before making that entry valid; do not rely on a
        // multi-cache-line MTE3 write becoming atomically visible to the NIC.
        if (uint64_t(n) * blocks > 1U) {
            RDMAWriteTraceScope interval(trace, RDMAWriteEvent::TailBegin, RDMAWriteEvent::TailEnd);
            io.Publish(q.sq + (first + 1U) * kEntryBytes,
                kPipelineWqeOffset + kEntryBytes, (n * blocks - 1U) * kEntryBytes);
        }
        {
            RDMAWriteTraceScope interval(trace, RDMAWriteEvent::HeadBegin, RDMAWriteEvent::HeadEnd);
            io.Publish(q.sq + first * kEntryBytes, kPipelineWqeOffset, kEntryBytes);
        }
        {
            RDMAWriteTraceScope interval(trace, RDMAWriteEvent::DoorbellBegin, RDMAWriteEvent::DoorbellEnd);
            if (q.dbMode == TILEXR_RDMA_DB_SOFT) {
                StoreBe(io, kRecordOffset, uint32_t(state.producer), 4U);
                io.Publish(q.sqDbr, kRecordOffset, 4U);
            }
            io.Ring(SqDbAddress(q, state.producer), SqDbValue(q.sqDbValue, state.producer));
        }
        sent += n;
        wr += n;
    }
    return TILEXR_RDMA_DEVICE_SUCCESS;
}

template <typename IO>
TILEXR_RDMA_WIRE_INLINE uint32_t PutPipeline(const TileXRRDMADeviceView& v,
    uint64_t localOffset, uint64_t remoteOffset, uint32_t bytes, uint32_t count,
    uint64_t localStride, uint64_t remoteStride, const PipelineOptions& options,
    uint32_t maxPolls, IO& io, RDMAWriteTrace* trace = nullptr)
{
    const TileXRRDMADeviceSge sge{localOffset, bytes, 0U};
    return PutSgeListPipeline(v, &sge, 1U, remoteOffset, count,
        localStride, remoteStride, options, maxPolls, io, trace);
}
} // namespace RdmaWire
} // namespace TileXR
#undef TILEXR_RDMA_WIRE_INLINE
#endif
