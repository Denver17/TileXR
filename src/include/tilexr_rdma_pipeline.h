/* Copyright (c) 2026 TileXR Project. Licensed under the Apache License, Version 2.0. */

/*
 * 功能：校验单段/多 SGE WRITE 批次范围，编码 WQE，并以最多两个在途 batch 提交和轮询完成。
 * 调用关系：tilexr_rdma_device.h 调用模板算法；复用 wire 校验与编码辅助函数，实际读写交给 IO。
 * 资源与生命周期：每批最后一个 WR 请求 CQE；更新借用的 QP state，拒绝物理 SQ/CQ 环复用。
 *
 * Purpose: Check single/SGE WRITE batch ranges, encode WQEs, post up to two in-flight batches and poll completions.
 * Calls: tilexr_rdma_device.h calls these templates; wire supplies validation/encoding helpers and IO performs queue access.
 * Lifetime: Requests a CQE for each batch's last WR, updates borrowed QP state and rejects physical SQ/CQ ring reuse.
 */
#ifndef TILEXR_RDMA_PIPELINE_H
#define TILEXR_RDMA_PIPELINE_H
#include "tilexr_rdma_wire.h"
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
constexpr uint64_t kPipelineStateTag = 0x5458525049504531ULL;

/* This opt-in path is deliberately non-wrapping. reserved[0]
 * tags its state; reserved[1] is the next WR ordinal, independent of CQ CI.
 * A fully drained all-signaled QP can enter it; the reverse transition is
 * rejected by the old APIs because their CQ ordinal assumption no longer holds. */
struct PipelineOptions {
    uint32_t postBatch = 64U;
    uint32_t maxInFlight = 128U;
    uint32_t cyclicSlots = 0U; // zero: use strides; otherwise both spans cycle.
};

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
    uint64_t endSq, uint64_t lastWr, uint32_t maxPolls, TileXRRDMADeviceState& state, IO& io)
{
    uint32_t status = TILEXR_RDMA_DEVICE_TIMEOUT;
    for (uint32_t poll = 0; poll < maxPolls; ++poll) {
        io.ReadCqe(q.cq + state.cqConsumer * kEntryBytes);
        const auto cqe = DecodeCqe(io, q, state.cqConsumer, lastWr);
        if (cqe.status == kPending) { continue; }
        status = cqe.status;
        state.syndrome = cqe.syndrome;
        break;
    }
    if (status == TILEXR_RDMA_DEVICE_SUCCESS) {
        state.completed = endSq;
        ++state.cqConsumer;
        StoreBe(io, kRecordOffset, state.cqConsumer & 0xFFFFFFU, 4U);
        io.Publish(q.cqDbr, kRecordOffset, 4U);
    } else {
        state.error = status;
    }
    io.StoreState(state);
    return status;
}

template <typename IO>
TILEXR_RDMA_WIRE_INLINE uint32_t PutSgeListPipeline(const TileXRRDMADeviceView& v,
    const TileXRRDMADeviceSge* sges, uint32_t numSge, uint64_t remoteOffset, uint32_t count,
    uint64_t localStride, uint64_t remoteStride, const PipelineOptions& options,
    uint32_t maxPolls, IO& io)
{
    uint32_t status = ValidateView(v);
    if (status != 0U) { return status; }
    if (sges == nullptr || numSge == 0U || numSge > TILEXR_RDMA_DEVICE_MAX_SGE || maxPolls == 0U) {
        return TILEXR_RDMA_DEVICE_INVALID;
    }
    if (numSge > v.queue.maxSendSge) { return TILEXR_RDMA_DEVICE_UNSUPPORTED; }
    const uint32_t batch = options.postBatch;
    const uint32_t spans = options.cyclicSlots == 0U ? count : options.cyclicSlots;
    const uint32_t peak = count < batch * 2U ? count : batch * 2U;
    if (count == 0U || batch == 0U || batch > kPipelineBatchMax ||
        peak > options.maxInFlight || peak > v.queue.maxSendWr) {
        return TILEXR_RDMA_DEVICE_INVALID;
    }
    uint64_t totalBytes = 0;
    for (uint32_t i = 0; i < numSge; ++i) {
        if (sges[i].reserved != 0U || sges[i].bytes == 0U || sges[i].bytes >= 0x80000000U ||
            !BatchFits(v.localBytes, sges[i].localOffset, sges[i].bytes, localStride, spans)) {
            return TILEXR_RDMA_DEVICE_INVALID;
        }
        totalBytes += sges[i].bytes;
    }
    if (totalBytes >= 0x80000000ULL ||
        !BatchFits(v.remoteBytes, remoteOffset, uint32_t(totalBytes), remoteStride, spans)) {
        return TILEXR_RDMA_DEVICE_INVALID;
    }
    auto state = io.LoadState();
    if (state.error != 0U) { return TILEXR_RDMA_DEVICE_POISONED; }
    if (state.producer != state.completed) { return TILEXR_RDMA_DEVICE_BUSY; }
    if (state.reserved[0] != 0U && state.reserved[0] != kPipelineStateTag) {
        return TILEXR_RDMA_DEVICE_UNSUPPORTED;
    }
    const uint64_t completions = (uint64_t(count) + batch - 1U) / batch;
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
    state.reserved[0] = kPipelineStateTag;
    uint64_t ends[2] = {}, lastWrs[2] = {};
    uint32_t posted = 0U, retired = 0U, sent = 0U;
    while (sent < count) {
        if (posted - retired == 2U) {
            status = DrainPipelineBatch(q, ends[retired % 2U], lastWrs[retired % 2U], maxPolls, state, io);
            if (status != 0U) { return status; }
            ++retired;
        }
        const uint32_t n = count - sent < batch ? count - sent : batch;
        const uint64_t first = state.producer;
        state.producer += uint64_t(n) * blocks;
        state.reserved[1] = wr + n;
        io.StoreState(state);
        EncodeStop(io, state.producer, q.sqDepth);
        io.Publish(q.sq + state.producer * kEntryBytes, kStopOffset, kEntryBytes);
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
        // The first entry may be a prefetched invalid stop from the last DB.
        // Complete the tail before making that entry valid; do not rely on a
        // multi-cache-line MTE3 write becoming atomically visible to the NIC.
        if (uint64_t(n) * blocks > 1U) {
            io.Publish(q.sq + (first + 1U) * kEntryBytes,
                kPipelineWqeOffset + kEntryBytes, (n * blocks - 1U) * kEntryBytes);
        }
        io.Publish(q.sq + first * kEntryBytes, kPipelineWqeOffset, kEntryBytes);
        if (q.dbMode == TILEXR_RDMA_DB_SOFT) {
            StoreBe(io, kRecordOffset, uint32_t(state.producer), 4U);
            io.Publish(q.sqDbr, kRecordOffset, 4U);
        }
        io.Ring(SqDbAddress(q, state.producer), SqDbValue(q.sqDbValue, state.producer));
        ends[posted % 2U] = state.producer;
        lastWrs[posted % 2U] = wr + n - 1U;
        ++posted;
        sent += n;
        wr += n;
    }
    while (retired < posted) {
        status = DrainPipelineBatch(q, ends[retired % 2U], lastWrs[retired % 2U], maxPolls, state, io);
        if (status != 0U) { return status; }
        ++retired;
    }
    return TILEXR_RDMA_DEVICE_SUCCESS;
}

template <typename IO>
TILEXR_RDMA_WIRE_INLINE uint32_t PutPipeline(const TileXRRDMADeviceView& v,
    uint64_t localOffset, uint64_t remoteOffset, uint32_t bytes, uint32_t count,
    uint64_t localStride, uint64_t remoteStride, const PipelineOptions& options,
    uint32_t maxPolls, IO& io)
{
    const TileXRRDMADeviceSge sge{localOffset, bytes, 0U};
    return PutSgeListPipeline(v, &sge, 1U, remoteOffset, count,
        localStride, remoteStride, options, maxPolls, io);
}
} // namespace RdmaWire
} // namespace TileXR
#undef TILEXR_RDMA_WIRE_INLINE
#endif
