/* Copyright (c) 2026 TileXR Project. Licensed under the Apache License, Version 2.0. */

/*
 * 功能：校验 device view 和传输范围，编码 UBNIC WRITE WQE/doorbell，并解析 CQE、推进发送状态。
 * 调用关系：device 和 pipeline 层使用这些模板；IO 提供 buffer 访问、queue 发布和 CQE 读取。
 * 资源与生命周期：不分配 queue 内存；已提交操作出错后记录 sticky error，保留未完成的发送计数。
 *
 * Purpose: Validate device views and transfer ranges, encode UBNIC WRITE WQEs/doorbells, decode CQEs and advance send state.
 * Calls: Used by device and pipeline code; IO supplies buffer access, queue publication and CQE reads.
 * Lifetime: Allocates no queue memory; posted failures record a sticky error and retain outstanding send counters.
 */
#ifndef TILEXR_RDMA_WIRE_H
#define TILEXR_RDMA_WIRE_H

#include "tilexr_rdma_device_types.h"

#if defined(__CCE_AICORE__) || defined(__CCE__) || defined(__NPU_ARCH__)
#define TILEXR_RDMA_WIRE_INLINE __aicore__ inline
#else
#define TILEXR_RDMA_WIRE_INLINE inline
#endif

namespace TileXR {
namespace RdmaWire {

/* Independently encoded UBNIC RC WRITE contract, reference provenance:
 * Ascend/perf-reference-ascend, GitCode commit
 * 66a7475f610a12d05827c23ad4750f9181e3a5d3, nda_for_ubnic.h:
 * ctrl/task/SGE 457/513/686; builder 2270; DB 2516; CQ 1366/2554.
 * WQE and PI/CI records are BE. CQ fields and the 64-bit SQ DB are LE/native
 * on Ascend950 (the reference's convert_ntohl/htonll are identity macros).
 * This is a narrow versioned ABI, not a portable verbs or URMA queue layout.
 * Reference NDA sources have no established repository-wide license grant;
 * no vendor implementation is included or linked here. */
constexpr uint32_t kEntryBytes = 64U;
constexpr uint32_t kMaxWqeBytes = ((48U + 16U * TILEXR_RDMA_DEVICE_MAX_SGE + 63U) / 64U) * 64U;
constexpr uint32_t kStopOffset = kMaxWqeBytes;
constexpr uint32_t kCqeOffset = kStopOffset + kEntryBytes;
constexpr uint32_t kRecordOffset = kCqeOffset + kEntryBytes;
constexpr uint32_t kPending = 0xFFFFFFFFU;
static_assert(kRecordOffset + 32U <= TILEXR_RDMA_DEVICE_SCRATCH_BYTES, "RDMA scratch layout");

/* Optional compile-time observer. The default has no clock reads or storage;
 * tracing must not change queue ownership, signaling or publication ordering. */
enum class BatchTraceStage : uint32_t {
    Enter, ViewLoaded, Validated, Reserved, StopPublished, WqesPublished,
    DoorbellIssued, FirstCqe, CqDrained, Retired, Count
};

struct NoBatchTrace {
    TILEXR_RDMA_WIRE_INLINE void Stage(BatchTraceStage) {}
    TILEXR_RDMA_WIRE_INLINE void WqeStart() {}
    TILEXR_RDMA_WIRE_INLINE void WqeEncoded() {}
    TILEXR_RDMA_WIRE_INLINE void WqePublished() {}
    TILEXR_RDMA_WIRE_INLINE void PollStart() {}
    TILEXR_RDMA_WIRE_INLINE void PollRead() {}
    TILEXR_RDMA_WIRE_INLINE void PollDecoded(bool) {}
};

TILEXR_RDMA_WIRE_INLINE bool PowerOfTwo(uint32_t n)
{
    return n >= 2U && (n & (n - 1U)) == 0U;
}

/* Validated inputs: QPN 20 bits, GID 7 bits, verbs MTU enum 1..5, COS 3 bits.
 * The template has zero PI and RC-only reserved/control/XRC/subtype fields. */
TILEXR_RDMA_WIRE_INLINE uint64_t MakeSqDbValue(uint32_t qpn, uint32_t gidIndex,
    uint32_t mtuEnum, uint32_t cos)
{
    return uint64_t(qpn) | (uint64_t(1) << 20) | (uint64_t(cos) << 24) |
        (uint64_t(21) << 27) | (uint64_t(mtuEnum - 1U) << 50) | (uint64_t(gidIndex) << 53);
}

TILEXR_RDMA_WIRE_INLINE uint64_t SqDbValue(uint64_t base, uint64_t postPi)
{
    return base | (((postPi >> 8) & 255U) << 32);
}

TILEXR_RDMA_WIRE_INLINE uint64_t SqDbAddress(const TileXRRDMADeviceQueue& q, uint64_t postPi)
{
    return q.doorbell + (q.dbMode == TILEXR_RDMA_DB_HARD ? 8U * (postPi & 255U) : 0U);
}

TILEXR_RDMA_WIRE_INLINE bool Span(uint64_t address, uint64_t bytes)
{
    return address != 0U && bytes != 0U && bytes <= UINT64_MAX - address;
}

TILEXR_RDMA_WIRE_INLINE uint32_t ValidateView(const TileXRRDMADeviceView& v)
{
    if (v.version != TILEXR_RDMA_DEVICE_VERSION || v.structBytes != sizeof(v) ||
        v.queue.queueAbi != TILEXR_RDMA_QUEUE_ABI_UBNIC_V2) {
        return TILEXR_RDMA_DEVICE_UNSUPPORTED;
    }
    const TileXRRDMADeviceQueue& q = v.queue;
    if ((q.dbMode != TILEXR_RDMA_DB_SOFT && q.dbMode != TILEXR_RDMA_DB_HARD) ||
        q.sqEntryBytes != kEntryBytes || q.cqEntryBytes != kEntryBytes) {
        return TILEXR_RDMA_DEVICE_UNSUPPORTED;
    }
    const uint64_t allowed = uint64_t(0xFFFFFU) | (uint64_t(3) << 20) |
        (uint64_t(7) << 24) | (uint64_t(31) << 27) |
        (uint64_t(7) << 50) | (uint64_t(127) << 53);
    if (!PowerOfTwo(q.sqDepth) || !PowerOfTwo(q.cqDepth) ||
        q.maxSendSge == 0U || q.maxSendSge > TILEXR_RDMA_DEVICE_MAX_SGE || q.maxSendWr == 0U ||
        q.qpn > 0xFFFFFU ||
        (q.sqDbValue & ~allowed) != 0U || (q.sqDbValue & 0xFFFFFU) != q.qpn ||
        ((q.sqDbValue >> 20) & 3U) != 1U || ((q.sqDbValue >> 27) & 31U) != 21U ||
        ((q.sqDbValue >> 50) & 7U) > 4U) {
        return TILEXR_RDMA_DEVICE_INVALID;
    }
    const uint64_t dbBytes = q.dbMode == TILEXR_RDMA_DB_HARD ? 2048U : 8U;
    if ((q.sq & 63U) != 0U || (q.cq & 63U) != 0U || (q.state & 63U) != 0U ||
        (q.doorbell & 7U) != 0U || (q.cqDbr & 3U) != 0U ||
        !Span(q.sq, uint64_t(q.sqDepth) * kEntryBytes) ||
        !Span(q.cq, uint64_t(q.cqDepth) * kEntryBytes) || !Span(q.state, sizeof(TileXRRDMADeviceState)) ||
        !Span(q.cqDbr, 4U) || q.doorbellBytes < dbBytes || !Span(q.doorbell, q.doorbellBytes) ||
        (q.dbMode == TILEXR_RDMA_DB_SOFT && ((q.sqDbr & 3U) != 0U || !Span(q.sqDbr, 4U))) ||
        !Span(v.localAddress, v.localBytes) || !Span(v.remoteAddress, v.remoteBytes)) {
        return TILEXR_RDMA_DEVICE_INVALID;
    }
    return TILEXR_RDMA_DEVICE_SUCCESS;
}

TILEXR_RDMA_WIRE_INLINE uint32_t ValidateWrite(const TileXRRDMADeviceView& v,
    uint64_t localOffset, uint64_t remoteOffset, uint32_t bytes, uint32_t maxPolls)
{
    const uint32_t status = ValidateView(v);
    if (status != TILEXR_RDMA_DEVICE_SUCCESS) {
        return status;
    }
    /* Reference SGE limit is 2^31; keep the unproven inclusive boundary out of
     * this ABI. Subtraction avoids overflow in offset + bytes. */
    if (bytes == 0U || bytes >= 0x80000000U || maxPolls == 0U ||
        localOffset > v.localBytes || bytes > v.localBytes - localOffset ||
        remoteOffset > v.remoteBytes || bytes > v.remoteBytes - remoteOffset) {
        return TILEXR_RDMA_DEVICE_INVALID;
    }
    return TILEXR_RDMA_DEVICE_SUCCESS;
}

/* Buffer supplies SetByte/GetByte. The device implementation writes actual UB;
 * the portable test supplies bytes and a modeled NIC, not a second encoder. */
template <typename Buffer>
TILEXR_RDMA_WIRE_INLINE void StoreBe(Buffer& b, uint32_t offset, uint64_t value, uint32_t bytes)
{
    for (uint32_t i = 0; i < bytes; ++i) {
        b.SetByte(offset + i, static_cast<uint8_t>(value >> (8U * (bytes - i - 1U))));
    }
}

template <typename Buffer>
TILEXR_RDMA_WIRE_INLINE uint32_t LoadLe(const Buffer& b, uint32_t offset, uint32_t bytes)
{
    uint32_t value = 0U;
    for (uint32_t i = 0; i < bytes; ++i) {
        value |= uint32_t(b.GetByte(offset + i)) << (8U * i);
    }
    return value;
}

template <typename Buffer>
TILEXR_RDMA_WIRE_INLINE void ClearEntry(Buffer& b, uint32_t offset)
{
    for (uint32_t i = 0; i < kEntryBytes; ++i) {
        b.SetByte(offset + i, 0U);
    }
}

template <typename Buffer>
TILEXR_RDMA_WIRE_INLINE void EncodeWrite(Buffer& b, const TileXRRDMADeviceView& v,
    uint64_t sequence, uint64_t wrSequence, uint64_t localOffset, uint64_t remoteOffset, uint32_t bytes)
{
    ClearEntry(b, 0U);
    b.SetByte(0U, static_cast<uint8_t>(0x40U | ((sequence & v.queue.sqDepth) != 0U ? 0x80U : 0U)));
    b.SetByte(1U, 0xA4U); // CR=1, VA=1, TSL=32/8.
    StoreBe(b, 2U, 2U | ((wrSequence & 3U) << 12), 2U); // BDSL=16/8, per-WR SSN.
    StoreBe(b, 4U, 0x10000000U, 4U); // CL=1; no signature/mask PI/direct WQE.
    StoreBe(b, 16U, 0x24000000U, 4U); // WRITE=4 at bit24, signal=1 at bit29.
    StoreBe(b, 20U, bytes, 4U);
    StoreBe(b, 32U, v.remoteAddress + remoteOffset, 8U);
    StoreBe(b, 40U, v.remoteKey, 4U);
    StoreBe(b, 44U, v.localKey & 0xFFFFFF00U, 4U); // ULP/MPT index, not zero.
    StoreBe(b, 48U, v.localAddress + localOffset, 8U);
    StoreBe(b, 56U, bytes, 4U);
    StoreBe(b, 60U, v.localKey | 0x80000000U, 4U); // Last SGE, next invalid.
}

template <typename Buffer>
TILEXR_RDMA_WIRE_INLINE void EncodeStop(Buffer& b, uint64_t next, uint32_t depth)
{
    ClearEntry(b, kStopOffset);
    b.SetByte(kStopOffset, (next & depth) == 0U ? 0xFFU : 0x7FU);
}

TILEXR_RDMA_WIRE_INLINE uint32_t WqeBlocks(uint32_t numSge)
{
    return (48U + 16U * numSge + kEntryBytes - 1U) / kEntryBytes;
}

template <typename Buffer>
TILEXR_RDMA_WIRE_INLINE void EncodeWriteSgeList(Buffer& b, const TileXRRDMADeviceView& v,
    uint64_t sequence, uint64_t wrSequence, const TileXRRDMADeviceSge* sgeList,
    uint32_t numSge, uint64_t remoteOffset, uint32_t totalBytes)
{
    EncodeWrite(b, v, sequence, wrSequence, sgeList[0].localOffset, remoteOffset, totalBytes);
    const uint32_t wqeBytes = WqeBlocks(numSge) * kEntryBytes;
    for (uint32_t i = kEntryBytes; i < wqeBytes; ++i) { b.SetByte(i, 0U); }
    StoreBe(b, 2U, (numSge * 2U) | ((wrSequence & 3U) << 12), 2U);
    for (uint32_t i = 0; i < numSge; ++i) {
        const uint32_t offset = 48U + 16U * i;
        StoreBe(b, offset, v.localAddress + sgeList[i].localOffset, 8U);
        StoreBe(b, offset + 8U, sgeList[i].bytes, 4U);
        StoreBe(b, offset + 12U, (v.localKey & 0x7FFFFFFFU) |
            (i + 1U == numSge ? 0x80000000U : 0U), 4U);
    }
}

struct CqeResult {
    uint32_t status;
    uint32_t syndrome;
};

template <typename Buffer>
TILEXR_RDMA_WIRE_INLINE CqeResult DecodeCqe(const Buffer& b, const TileXRRDMADeviceQueue& q,
    uint64_t cqConsumer, uint64_t expectedSequence)
{
    const uint32_t ownerQpn = LoadLe(b, kCqeOffset, 4U);
    const uint32_t op = LoadLe(b, kCqeOffset + 4U, 4U);
    const uint32_t opcode = op >> 27;
    if ((ownerQpn >> 31) != uint32_t((cqConsumer & q.cqDepth) != 0U) || opcode == 31U) {
        return {kPending, 0U};
    }
    const uint32_t syndrome = b.GetByte(kCqeOffset + 28U);
    if (opcode == 30U) {
        return {TILEXR_RDMA_DEVICE_CQE_ERROR, syndrome}; // Even syndrome=0 is an error.
    }
    /* The SQ identity is LE16 wqe_counter@30, not the CQ slot or the low
     * WQEBB field (used for RQ by the reference's generic CQ path). */
    if ((ownerQpn & 0xFFFFFU) != q.qpn || opcode != 4U || ((op >> 26) & 1U) != 1U ||
        (op & (7U << 23)) != 0U ||
        LoadLe(b, kCqeOffset + 30U, 2U) != (expectedSequence & 0xFFFFU)) {
        return {TILEXR_RDMA_DEVICE_CQE_MISMATCH, syndrome};
    }
    return {TILEXR_RDMA_DEVICE_SUCCESS, 0U};
}

/* IO implements UB byte access, LoadState/StoreState, Publish (completed MTE3),
 * Ring (st_dev) and ReadCqe (fresh snapshot at kCqeOffset). This shared algorithm
 * is exercised by the portable NIC model and the real Ascend C adapter alike. */
/* All addresses/strides are bytes. Validate the whole batch before publishing
 * anything; division avoids overflowing (count - 1) * stride. A zero stride
 * deliberately reuses that source/destination. */
TILEXR_RDMA_WIRE_INLINE bool BatchFits(uint64_t regionBytes, uint64_t offset,
    uint32_t bytes, uint64_t stride, uint32_t count)
{
    return count != 0U && offset <= regionBytes && bytes <= regionBytes - offset &&
        (stride == 0U || uint64_t(count - 1U) <= (regionBytes - offset - bytes) / stride);
}

template <typename IO, typename Trace>
TILEXR_RDMA_WIRE_INLINE uint32_t PutStridedBatch(const TileXRRDMADeviceView& v,
    uint64_t localOffset, uint64_t remoteOffset, uint32_t bytes, uint32_t count,
    uint64_t localStride, uint64_t remoteStride, uint32_t maxInFlight, uint32_t maxPolls,
    IO& io, Trace& trace)
{
    uint32_t status = ValidateWrite(v, localOffset, remoteOffset, bytes, maxPolls);
    if (status != TILEXR_RDMA_DEVICE_SUCCESS) {
        return status;
    }
    if (maxInFlight == 0U || !BatchFits(v.localBytes, localOffset, bytes, localStride, count) ||
        !BatchFits(v.remoteBytes, remoteOffset, bytes, remoteStride, count)) {
        return TILEXR_RDMA_DEVICE_INVALID;
    }
    TileXRRDMADeviceState state = io.LoadState();
    if (state.error != 0U) {
        return TILEXR_RDMA_DEVICE_POISONED;
    }
    if (state.reserved[0] != 0U) { return TILEXR_RDMA_DEVICE_UNSUPPORTED; }
    if (state.producer < state.completed || uint64_t(count) > UINT64_MAX - state.producer ||
        uint64_t(count) > UINT64_MAX - state.cqConsumer) {
        return TILEXR_RDMA_DEVICE_INVALID;
    }
    if (state.producer != state.completed) {
        return TILEXR_RDMA_DEVICE_BUSY;
    }
    const TileXRRDMADeviceQueue& q = v.queue;
    uint32_t window = maxInFlight < q.sqDepth - 1U ? maxInFlight : q.sqDepth - 1U;
    window = window < q.cqDepth - 1U ? window : q.cqDepth - 1U;
    window = window < q.maxSendWr ? window : q.maxSendWr;
    trace.Stage(BatchTraceStage::Validated);
    uint32_t sent = 0U;
    while (sent < count) {
        const uint32_t batch = count - sent < window ? count - sent : window;
        const uint64_t first = state.producer;
        const uint64_t next = first + batch;
        const uint64_t firstCq = state.cqConsumer;
        state.producer = next;
        io.StoreState(state); // Preserve every possibly posted WR on failure.
        trace.Stage(BatchTraceStage::Reserved);
        // Keep one invalid SQ entry beyond the batch. Publish it before making
        // the first WQE valid: the NIC may prefetch before the final doorbell.
        EncodeStop(io, next, q.sqDepth);
        io.Publish(q.sq + (next & (q.sqDepth - 1U)) * kEntryBytes, kStopOffset, kEntryBytes);
        trace.Stage(BatchTraceStage::StopPublished);
        for (uint32_t i = 0; i < batch; ++i) {
            const uint64_t index = uint64_t(sent) + i;
            trace.WqeStart();
            EncodeWrite(io, v, first + i, firstCq + i, localOffset + index * localStride,
                remoteOffset + index * remoteStride, bytes);
            trace.WqeEncoded();
            io.Publish(q.sq + ((first + i) & (q.sqDepth - 1U)) * kEntryBytes, 0U, kEntryBytes);
            trace.WqePublished();
        }
        trace.Stage(BatchTraceStage::WqesPublished);
        if (q.dbMode == TILEXR_RDMA_DB_SOFT) {
            StoreBe(io, kRecordOffset, uint32_t(next), 4U);
            io.Publish(q.sqDbr, kRecordOffset, 4U);
        }
        io.Ring(SqDbAddress(q, next), SqDbValue(q.sqDbValue, next));
        trace.Stage(BatchTraceStage::DoorbellIssued);
        for (uint32_t i = 0; i < batch; ++i) {
            const uint64_t cqeAddress = q.cq + (state.cqConsumer & (q.cqDepth - 1U)) * kEntryBytes;
            status = TILEXR_RDMA_DEVICE_TIMEOUT;
            for (uint32_t poll = 0; poll < maxPolls; ++poll) {
                trace.PollStart();
                io.ReadCqe(cqeAddress);
                trace.PollRead();
                const CqeResult result = DecodeCqe(io, q, state.cqConsumer, state.cqConsumer);
                trace.PollDecoded(result.status == kPending);
                if (result.status == kPending) { continue; }
                status = result.status;
                state.syndrome = result.syndrome;
                break;
            }
            if (status != TILEXR_RDMA_DEVICE_SUCCESS) {
                state.error = status;
                break;
            }
            ++state.completed;
            ++state.cqConsumer;
            if (i == 0U) { trace.Stage(BatchTraceStage::FirstCqe); }
        }
        trace.Stage(BatchTraceStage::CqDrained);
        // Retire the successful prefix even on an error. Outstanding WRs stay
        // pinned/poisoned until teardown; a failed batch is never resubmitted.
        if (state.cqConsumer != firstCq) {
            StoreBe(io, kRecordOffset, state.cqConsumer & 0xFFFFFFU, 4U);
            io.Publish(q.cqDbr, kRecordOffset, 4U);
        }
        io.StoreState(state);
        trace.Stage(BatchTraceStage::Retired);
        if (status != TILEXR_RDMA_DEVICE_SUCCESS) { return status; }
        sent += batch;
    }
    return TILEXR_RDMA_DEVICE_SUCCESS;
}

template <typename IO>
TILEXR_RDMA_WIRE_INLINE uint32_t PutStridedBatch(const TileXRRDMADeviceView& v,
    uint64_t localOffset, uint64_t remoteOffset, uint32_t bytes, uint32_t count,
    uint64_t localStride, uint64_t remoteStride, uint32_t maxInFlight, uint32_t maxPolls, IO& io)
{
    NoBatchTrace trace;
    return PutStridedBatch(v, localOffset, remoteOffset, bytes, count,
        localStride, remoteStride, maxInFlight, maxPolls, io, trace);
}

template <typename IO>
TILEXR_RDMA_WIRE_INLINE uint32_t PutSgeList(const TileXRRDMADeviceView& v,
    const TileXRRDMADeviceSge* sgeList, uint32_t numSge, uint64_t remoteOffset,
    uint32_t maxPolls, IO& io)
{
    uint32_t status = ValidateView(v);
    if (status != TILEXR_RDMA_DEVICE_SUCCESS) { return status; }
    if (sgeList == nullptr || numSge == 0U || numSge > TILEXR_RDMA_DEVICE_MAX_SGE || maxPolls == 0U) {
        return TILEXR_RDMA_DEVICE_INVALID;
    }
    const auto& q = v.queue;
    if (numSge > q.maxSendSge) { return TILEXR_RDMA_DEVICE_UNSUPPORTED; }
    uint64_t total = 0U;
    for (uint32_t i = 0; i < numSge; ++i) {
        const auto& sge = sgeList[i];
        if (sge.reserved != 0U || sge.bytes == 0U || sge.bytes >= 0x80000000U ||
            sge.localOffset > v.localBytes || sge.bytes > v.localBytes - sge.localOffset) {
            return TILEXR_RDMA_DEVICE_INVALID;
        }
        total += sge.bytes;
    }
    if (total >= 0x80000000ULL || remoteOffset > v.remoteBytes || total > v.remoteBytes - remoteOffset) {
        return TILEXR_RDMA_DEVICE_INVALID;
    }
    TileXRRDMADeviceState state = io.LoadState();
    if (state.error != 0U) { return TILEXR_RDMA_DEVICE_POISONED; }
    if (state.reserved[0] != 0U) { return TILEXR_RDMA_DEVICE_UNSUPPORTED; }
    const uint32_t blocks = WqeBlocks(numSge);
    if (state.producer < state.completed || state.producer > UINT64_MAX - blocks ||
        state.cqConsumer == UINT64_MAX) { return TILEXR_RDMA_DEVICE_INVALID; }
    if (state.producer != state.completed) { return TILEXR_RDMA_DEVICE_BUSY; }
    // This initial path deliberately excludes a WQE/stop crossing the SQ tail.
    const uint64_t slot = state.producer & (q.sqDepth - 1U);
    if (slot + blocks >= q.sqDepth) { return TILEXR_RDMA_DEVICE_INVALID; }
    const uint64_t first = state.producer;
    state.producer += blocks;
    io.StoreState(state); // A posted failure retains every possibly used BB.
    EncodeStop(io, state.producer, q.sqDepth);
    io.Publish(q.sq + (slot + blocks) * kEntryBytes, kStopOffset, kEntryBytes);
    EncodeWriteSgeList(io, v, first, state.cqConsumer, sgeList, numSge, remoteOffset,
        static_cast<uint32_t>(total));
    // A prefetched valid control must never reference uninitialized tail SGEs.
    if (blocks > 1U) {
        io.Publish(q.sq + (slot + 1U) * kEntryBytes, kEntryBytes, (blocks - 1U) * kEntryBytes);
    }
    io.Publish(q.sq + slot * kEntryBytes, 0U, kEntryBytes);
    if (q.dbMode == TILEXR_RDMA_DB_SOFT) {
        StoreBe(io, kRecordOffset, uint32_t(state.producer), 4U);
        io.Publish(q.sqDbr, kRecordOffset, 4U);
    }
    io.Ring(SqDbAddress(q, state.producer), SqDbValue(q.sqDbValue, state.producer));
    status = TILEXR_RDMA_DEVICE_TIMEOUT;
    for (uint32_t poll = 0; poll < maxPolls; ++poll) {
        io.ReadCqe(q.cq + (state.cqConsumer & (q.cqDepth - 1U)) * kEntryBytes);
        const auto result = DecodeCqe(io, q, state.cqConsumer, state.cqConsumer);
        if (result.status == kPending) { continue; }
        status = result.status;
        state.syndrome = result.syndrome;
        break;
    }
    if (status == TILEXR_RDMA_DEVICE_SUCCESS) {
        state.completed += blocks;
        ++state.cqConsumer; // ONE CQE for the whole SGE list.
        StoreBe(io, kRecordOffset, state.cqConsumer & 0xFFFFFFU, 4U);
        io.Publish(q.cqDbr, kRecordOffset, 4U);
    } else {
        state.error = status;
    }
    io.StoreState(state);
    return status;
}

template <typename IO>
TILEXR_RDMA_WIRE_INLINE uint32_t Put(const TileXRRDMADeviceView& v,
    uint64_t localOffset, uint64_t remoteOffset, uint32_t bytes, uint32_t maxPolls, IO& io)
{
    return PutStridedBatch(v, localOffset, remoteOffset, bytes, 1U, 0U, 0U, 1U, maxPolls, io);
}

} // namespace RdmaWire
} // namespace TileXR

#undef TILEXR_RDMA_WIRE_INLINE
#endif
