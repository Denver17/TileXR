/*
 * 功能：执行逻辑 rank 0 与各 peer 的四时间戳交换，丢弃 warmup，并将采样或失败状态写入 GM。
 * 调用关系：clock_sync_kernel.cpp 的入口调用此实现；通过 MTE peer memory 或 RDMA queue 发送协议消息。
 * 资源与生命周期：Host session 提供采样和可选 RDMA buffer；kernel 使用固定 UB 区域及传入的 scratch。
 * Purpose: Exchange four timestamps between logical rank 0 and peers, discard warmups and write samples/failures to GM.
 * Calls: clock_sync_kernel.cpp invokes this implementation using MTE peer memory or RDMA queues.
 * Lifetime: The Host session supplies sample/optional RDMA buffers; the kernel uses fixed UB space and supplied scratch.
 */
#ifndef TILEXR_DIAGNOSTICS_KERNEL_CLOCK_SYNC_KERNEL_H
#define TILEXR_DIAGNOSTICS_KERNEL_CLOCK_SYNC_KERNEL_H

#include "comm_args.h"
#include "kernel_operator.h"
#include "tilexr_straggler_device.h"
#include "tilexr_straggler_layout.h"
#include "tilexr_rdma_device.h"
#include "clock_rdma_profile.h"
#include "clock_rdma_queue.h"

namespace TileXR {

__attribute__((always_inline)) inline __aicore__ void StragglerReadGm(
    __ubuf__ uint8_t *destination, GM_ADDR source, uint32_t bytes)
{
    AscendC::LocalTensor<uint8_t> destinationTensor;
    AscendC::GlobalTensor<uint8_t> sourceTensor;
    AscendC::DataCopyExtParams params(1, bytes, 0, 0, 0);
    AscendC::DataCopyPadExtParams<uint8_t> padParams;
    destinationTensor.address_.logicPos = static_cast<uint8_t>(AscendC::TPosition::VECIN);
    destinationTensor.address_.bufferAddr = reinterpret_cast<uint64_t>(destination);
    sourceTensor.SetGlobalBuffer(reinterpret_cast<__gm__ uint8_t *>(source));
    // The previous snapshot is consumed by Scalar before MTE2 reuses its UB.
    AscendC::SetFlag<AscendC::HardEvent::S_MTE2>(EVENT_ID0);
    AscendC::WaitFlag<AscendC::HardEvent::S_MTE2>(EVENT_ID0);
    AscendC::DataCopyPad(destinationTensor, sourceTensor, params, padParams);
    AscendC::SetFlag<AscendC::HardEvent::MTE2_S>(EVENT_ID0);
    AscendC::WaitFlag<AscendC::HardEvent::MTE2_S>(EVENT_ID0);
}

__attribute__((always_inline)) inline __aicore__ bool StragglerTimedOut(
    uint64_t startCycle, uint64_t timeoutCycles)
{
    return timeoutCycles != 0 &&
        static_cast<uint64_t>(AscendC::GetSystemCycle()) - startCycle >= timeoutCycles;
}

__attribute__((always_inline)) inline __aicore__ bool StragglerReadProtocolMatches(
    __ubuf__ TileXRClockSyncProtocolSlot *protocol, GM_ADDR slot, uint64_t magic,
    uint64_t sequence, uint64_t state, GM_ADDR rdmaMemory,
    __ubuf__ TileXRClockRdmaProfile *profile)
{
#if TILEXR_DIAGNOSTICS_CLOCK_PROFILE
    const uint64_t begin = static_cast<uint64_t>(AscendC::GetSystemCycle());
#endif
#if defined(__NPU_ARCH__) && (__NPU_ARCH__ == 3510)
    if (rdmaMemory != nullptr) {
        RdmaDeviceDetail::Invalidate(reinterpret_cast<uint64_t>(slot), sizeof(*protocol));
    }
#endif
#if TILEXR_DIAGNOSTICS_CLOCK_PROFILE
    const uint64_t invalidated = static_cast<uint64_t>(AscendC::GetSystemCycle());
#endif
    StragglerReadGm(reinterpret_cast<__ubuf__ uint8_t *>(protocol), slot, sizeof(*protocol));
    const bool matches = protocol->state == state && protocol->sequence == sequence && protocol->magic == magic;
#if TILEXR_DIAGNOSTICS_CLOCK_PROFILE
    const uint64_t end = static_cast<uint64_t>(AscendC::GetSystemCycle());
    auto &record = profile->receive[state - 1u];
    ++record.calls;
    record.matches += matches ? 1u : 0u;
    record.invalidateCycles += invalidated - begin;
    record.readCycles += end - invalidated;
#else
    (void)profile;
#endif
    return matches;
}

// Called after capturing t1/t3, so diagnostics do not shift their endpoints.
__attribute__((always_inline)) inline __aicore__ void StragglerRecordMatchedWindow(
    __ubuf__ TileXRClockRdmaProfile *profile, uint32_t state, uint64_t begin, uint64_t end)
{
#if TILEXR_DIAGNOSTICS_CLOCK_PROFILE
    auto &record = profile->receive[state - 1u];
    const uint64_t cycles = end - begin;
    ++record.windowMatches;
    record.matchWindowCycles += cycles;
    if (record.windowMatches == 1u || cycles < record.minMatchWindowCycles) {
        record.minMatchWindowCycles = cycles;
    }
    if (cycles > record.maxMatchWindowCycles) { record.maxMatchWindowCycles = cycles; }
#else
    (void)profile; (void)state; (void)begin; (void)end;
#endif
}

__attribute__((always_inline)) inline __aicore__ GM_ADDR StragglerPeerMemory(
    GM_ADDR commArgs, int rank, GM_ADDR rdmaMemory)
{
    // In RDMA mode all receive polling addresses are LOCAL. Remote addresses
    // and keys come exclusively from the corresponding registered device view.
    if (rdmaMemory != nullptr) {
        return rdmaMemory;
    }
    AscendC::GlobalTensor<GM_ADDR> peerMems;
    peerMems.SetGlobalBuffer(&(reinterpret_cast<__gm__ CommArgs *>(commArgs))->peerMems[0],
                             TILEXR_MAX_RANK_SIZE);
    return peerMems.GetValue(rank);
}

__attribute__((always_inline)) inline __aicore__ uint32_t StragglerSendClockSlot(
    GM_ADDR rdmaMemory, int peer, GM_ADDR destination,
    __ubuf__ TileXRClockSyncProtocolSlot *protocol,
    const AscendC::LocalTensor<uint8_t> &scratch, __ubuf__ TileXRClockRdmaProfile *profile,
    bool deferred, uint32_t totalSamples, uint64_t *publication = nullptr)
{
    if (rdmaMemory == nullptr) {
        TileXRStragglerWriteGm(destination, reinterpret_cast<__ubuf__ uint8_t *>(protocol),
                               sizeof(*protocol));
        return TILEXR_RDMA_DEVICE_SUCCESS;
    }
    const uint64_t offset = static_cast<uint64_t>(destination - rdmaMemory);
    uint64_t sourceOffset = TILEXR_CLOCK_RDMA_STAGING_OFFSET;
    if (deferred) {
        const uint64_t ordinal = peer == 0 ? 2u * protocol->sequence +
            (protocol->state == TILEXR_CLOCK_SYNC_ACK ? 1u : 0u) :
            3u * protocol->sequence + (protocol->state == TILEXR_CLOCK_SYNC_READY ? 0u :
                (protocol->state == TILEXR_CLOCK_SYNC_RESPONSE ? 1u : 2u));
        const uint64_t peerBase = peer == 0 ? 0u : static_cast<uint64_t>(peer - 1) * 3u * totalSamples;
        sourceOffset = TILEXR_CLOCK_RDMA_ASYNC_STAGING_OFFSET + (peerBase + ordinal) * 64u;
        if (sourceOffset > TILEXR_CLOCK_SYNC_REGION_OFFSET - 64u) {
            return TILEXR_RDMA_DEVICE_INVALID;
        }
    }
#if TILEXR_DIAGNOSTICS_CLOCK_PROFILE
    const uint64_t start = static_cast<uint64_t>(AscendC::GetSystemCycle());
    ClockRdmaSendTrace observer(&profile->send[protocol->state - 1u]);
#else
    (void)profile;
    RdmaWire::NoBatchTrace observer;
#endif
    ClockRdmaPublicationTrace<decltype(observer)> trace(observer);
    TileXRStragglerWriteGm(rdmaMemory + sourceOffset,
        reinterpret_cast<__ubuf__ uint8_t *>(protocol), sizeof(*protocol));
    const auto *view = reinterpret_cast<const __gm__ TileXRRDMADeviceView *>(
        rdmaMemory + TILEXR_CLOCK_RDMA_VIEWS_OFFSET) + peer;
    // Deferred sends use distinct, immutable slots until the final CQ drain.
#if TILEXR_DIAGNOSTICS_CLOCK_PROFILE
    const uint64_t staged = static_cast<uint64_t>(AscendC::GetSystemCycle());
#endif
    uint32_t status;
    if (deferred) {
#if defined(__NPU_ARCH__) && (__NPU_ARCH__ == 3510)
        trace.Stage(RdmaWire::BatchTraceStage::Enter);
        const auto loaded = RdmaDeviceDetail::LoadView(view);
        trace.Stage(RdmaWire::BatchTraceStage::ViewLoaded);
        RdmaDeviceDetail::QueueIO io(loaded.queue.state, scratch);
        status = ClockRdmaQueue::Post(loaded, sourceOffset, offset, io, trace);
#else
        status = TILEXR_RDMA_DEVICE_UNSUPPORTED;
#endif
    } else {
        status = RDMAPutStridedBatch(view, sourceOffset,
            offset, sizeof(*protocol), 1u, 0u, 0u, scratch, 1u, 1000000u, trace);
    }
    if (status == TILEXR_RDMA_DEVICE_SUCCESS && publication != nullptr) {
        *publication = trace.Midpoint();
    }
#if TILEXR_DIAGNOSTICS_CLOCK_PROFILE
    const uint64_t end = static_cast<uint64_t>(AscendC::GetSystemCycle());
    observer.Finish(start, staged, end, status, deferred);
#endif
    return status;
}

__attribute__((always_inline)) inline __aicore__ uint32_t StragglerDrainClockQueues(
    GM_ADDR memory, int rank, int rankSize, const AscendC::LocalTensor<uint8_t> &scratch,
    __ubuf__ TileXRClockRdmaProfile *profile)
{
#if defined(__NPU_ARCH__) && (__NPU_ARCH__ == 3510)
#if TILEXR_DIAGNOSTICS_CLOCK_PROFILE
    const uint64_t begin = static_cast<uint64_t>(AscendC::GetSystemCycle());
#else
    (void)profile;
#endif
    for (int peer = 0; peer < rankSize; ++peer) {
        if (peer == rank || (rank != 0 && peer != 0)) { continue; }
        const auto *view = reinterpret_cast<const __gm__ TileXRRDMADeviceView *>(
            memory + TILEXR_CLOCK_RDMA_VIEWS_OFFSET) + peer;
        const auto loaded = RdmaDeviceDetail::LoadView(view);
        const uint32_t valid = RdmaWire::ValidateView(loaded);
        if (valid != TILEXR_RDMA_DEVICE_SUCCESS) { return valid; }
        RdmaDeviceDetail::QueueIO io(loaded.queue.state, scratch);
        const uint32_t status = ClockRdmaQueue::Drain(loaded, io);
        if (status != TILEXR_RDMA_DEVICE_SUCCESS) { return status; }
    }
#if TILEXR_DIAGNOSTICS_CLOCK_PROFILE
    profile->reserved[1] = static_cast<uint64_t>(AscendC::GetSystemCycle()) - begin;
#endif
    return TILEXR_RDMA_DEVICE_SUCCESS;
#else
    (void)memory; (void)rank; (void)rankSize; (void)scratch; (void)profile;
    return TILEXR_RDMA_DEVICE_UNSUPPORTED;
#endif
}

__attribute__((always_inline)) inline __aicore__ void WriteClockFailure(
    GM_ADDR samples, uint32_t sequence, uint32_t detail = 0)
{
    __ubuf__ TileXRClockSyncDeviceSample *sample =
        reinterpret_cast<__ubuf__ TileXRClockSyncDeviceSample *>(TILEXR_STRAGGLER_UB_OFFSET +
            sizeof(TileXRClockSyncProtocolSlot));
    sample->t0 = 0;
    sample->t1 = 0;
    sample->t2 = 0;
    sample->t3 = 0;
    sample->roundTripCycles = 0;
    sample->offsetCycles = 0;
    sample->reserved = detail;
    sample->sequence = sequence;
    sample->status = TILEXR_CLOCK_SYNC_SAMPLE_TERMINAL_FAILURE;
    TileXRStragglerWriteGm(samples, reinterpret_cast<__ubuf__ uint8_t *>(sample), sizeof(*sample));
}

__attribute__((always_inline)) inline __aicore__ void TileXRClockSyncKernel(
    GM_ADDR commArgs, GM_ADDR samples, uint32_t sampleCount, uint32_t warmupSampleCount,
    uint64_t magic, uint64_t timeoutCycles, GM_ADDR rdmaMemory,
    const AscendC::LocalTensor<uint8_t> &scratch, __ubuf__ TileXRClockRdmaProfile *profile)
{
    if (AscendC::GetBlockIdx() != 0 || commArgs == nullptr || samples == nullptr || sampleCount == 0 ||
        sampleCount > TILEXR_CLOCK_SYNC_MAX_SAMPLES ||
        warmupSampleCount > TILEXR_CLOCK_SYNC_MAX_WARMUP_SAMPLES) {
        return;
    }
    const uint32_t totalSampleCount = warmupSampleCount + sampleCount;
    const uint64_t rdmaMode = rdmaMemory == nullptr ? 0u :
        reinterpret_cast<const __gm__ uint64_t *>(rdmaMemory)[0];
    const bool deferred = (rdmaMode & 1u) != 0u;
    const bool compact = (rdmaMode & 2u) != 0u;
#if TILEXR_DIAGNOSTICS_CLOCK_PROFILE
    profile->reserved[0] = rdmaMode;
#endif
    AscendC::GlobalTensor<int> commArgsGm;
    commArgsGm.SetGlobalBuffer(reinterpret_cast<__gm__ int *>(commArgs), 5);
    const int rank = commArgsGm.GetValue(0);
    const int rankSize = commArgsGm.GetValue(2);
    if (rank < 0 || rank >= rankSize || rankSize <= 0 || rankSize > TILEXR_MAX_RANK_SIZE) {
        WriteClockFailure(samples, 0);
        return;
    }

    __ubuf__ TileXRClockSyncProtocolSlot *protocol =
        reinterpret_cast<__ubuf__ TileXRClockSyncProtocolSlot *>(TILEXR_STRAGGLER_UB_OFFSET);
    __ubuf__ TileXRClockSyncDeviceSample *sample =
        reinterpret_cast<__ubuf__ TileXRClockSyncDeviceSample *>(TILEXR_STRAGGLER_UB_OFFSET +
            sizeof(TileXRClockSyncProtocolSlot));

    if (rank == 0) {
        GM_ADDR rootMemory = StragglerPeerMemory(commArgs, 0, rdmaMemory);
        if (rootMemory == nullptr) {
            WriteClockFailure(samples, 0);
            return;
        }
        for (uint32_t protocolSequence = 0; protocolSequence < totalSampleCount;
             ++protocolSequence) {
            for (int peer = 1; peer < rankSize; ++peer) {
                GM_ADDR peerMemory = StragglerPeerMemory(commArgs, peer, rdmaMemory);
                if (peerMemory == nullptr) {
                    const uint32_t sampleSequence = protocolSequence < warmupSampleCount ? 0u :
                        protocolSequence - warmupSampleCount;
                    WriteClockFailure(samples + static_cast<uint64_t>(sampleSequence) * sizeof(*sample),
                                      sampleSequence);
                    return;
                }
                const uint64_t slotOffset = ClockSyncProtocolSlotOffset(static_cast<uint32_t>(peer));
                GM_ADDR responseSlot = peerMemory + slotOffset;
                GM_ADDR requestSlot = rootMemory + slotOffset;
                GM_ADDR ackSlot = rootMemory + ClockSyncAckSlotOffset(static_cast<uint32_t>(peer));
                GM_ADDR timestampSlot =
                    peerMemory + ClockSyncTimestampSlotOffset(static_cast<uint32_t>(peer));
                protocol->magic = magic;
                protocol->sequence = protocolSequence;
                protocol->state = TILEXR_CLOCK_SYNC_READY;
                protocol->t0 = 0;
                protocol->t1 = 0;
                protocol->t2 = 0;
                protocol->reserved0 = 0;
                protocol->reserved1 = 0;
                uint32_t sendStatus = TILEXR_RDMA_DEVICE_SUCCESS;
                if (!compact) {
                    sendStatus = StragglerSendClockSlot(rdmaMemory, peer, responseSlot,
                        protocol, scratch, profile, deferred, totalSampleCount);
                }
                if (sendStatus != TILEXR_RDMA_DEVICE_SUCCESS) {
                    WriteClockFailure(samples, 0, sendStatus);
                    return;
                }
                const uint64_t waitStart = static_cast<uint64_t>(AscendC::GetSystemCycle());
                uint64_t t1 = 0;
                while (true) {
                    const uint64_t readStart = static_cast<uint64_t>(AscendC::GetSystemCycle());
                    if (StragglerReadProtocolMatches(protocol, requestSlot, magic, protocolSequence,
                                                     TILEXR_CLOCK_SYNC_REQUEST, rdmaMemory, profile)) {
                        const uint64_t readEnd = static_cast<uint64_t>(AscendC::GetSystemCycle());
                        t1 = readStart + (readEnd - readStart) / 2u;
                        StragglerRecordMatchedWindow(profile, TILEXR_CLOCK_SYNC_REQUEST, readStart, readEnd);
                        break;
                    }
                    if (StragglerTimedOut(waitStart, timeoutCycles)) {
                        const uint32_t sampleSequence = protocolSequence < warmupSampleCount ? 0u :
                            protocolSequence - warmupSampleCount;
                        WriteClockFailure(samples + static_cast<uint64_t>(sampleSequence) * sizeof(*sample),
                                          sampleSequence);
                        return;
                    }
                }
                protocol->magic = magic;
                protocol->sequence = protocolSequence;
                protocol->state = TILEXR_CLOCK_SYNC_RESPONSE;
                protocol->t0 = 0;
                protocol->t1 = t1;
                protocol->t2 = 0;
                protocol->reserved0 = 0;
                protocol->reserved1 = 0;
                const uint64_t responseWriteStart =
                    static_cast<uint64_t>(AscendC::GetSystemCycle());
                uint64_t responsePublication = 0;
                sendStatus = StragglerSendClockSlot(rdmaMemory, peer, responseSlot, protocol,
                    scratch, profile, deferred, totalSampleCount, &responsePublication);
                const uint64_t responseWriteEnd =
                    static_cast<uint64_t>(AscendC::GetSystemCycle());
                const uint64_t t2 = rdmaMemory == nullptr ?
                    responseWriteStart + (responseWriteEnd - responseWriteStart) / 2u :
                    responsePublication;
                if (sendStatus != TILEXR_RDMA_DEVICE_SUCCESS) {
                    WriteClockFailure(samples, 0, sendStatus);
                    return;
                }
                protocol->magic = magic;
                protocol->sequence = protocolSequence;
                protocol->state = TILEXR_CLOCK_SYNC_TIMESTAMP;
                protocol->t0 = 0;
                protocol->t1 = 0;
                protocol->t2 = t2;
                protocol->reserved0 = 0;
                protocol->reserved1 = 0;
                sendStatus = StragglerSendClockSlot(rdmaMemory, peer, timestampSlot, protocol, scratch, profile, deferred, totalSampleCount);
                if (sendStatus != TILEXR_RDMA_DEVICE_SUCCESS) {
                    WriteClockFailure(samples, 0, sendStatus);
                    return;
                }
                if (!compact) {
                    const uint64_t ackWaitStart = static_cast<uint64_t>(AscendC::GetSystemCycle());
                    while (true) {
                        if (StragglerReadProtocolMatches(protocol, ackSlot, magic, protocolSequence,
                                                         TILEXR_CLOCK_SYNC_ACK, rdmaMemory, profile)) {
                            break;
                        }
                        if (StragglerTimedOut(ackWaitStart, timeoutCycles)) {
                            const uint32_t sampleSequence = protocolSequence < warmupSampleCount ? 0u :
                                protocolSequence - warmupSampleCount;
                            WriteClockFailure(samples + static_cast<uint64_t>(sampleSequence) * sizeof(*sample),
                                              sampleSequence);
                            return;
                        }
                    }
                }
            }
            if (protocolSequence < warmupSampleCount) {
                continue;
            }
            const uint32_t sampleSequence = protocolSequence - warmupSampleCount;
            const uint64_t now = static_cast<uint64_t>(AscendC::GetSystemCycle());
            sample->t0 = now;
            sample->t1 = now;
            sample->t2 = now;
            sample->t3 = now;
            sample->roundTripCycles = 0;
            sample->offsetCycles = 0;
            sample->reserved = 0;
            sample->sequence = sampleSequence;
            sample->status = TILEXR_CLOCK_SYNC_SAMPLE_SUCCESS;
            TileXRStragglerWriteGm(samples + static_cast<uint64_t>(sampleSequence) * sizeof(*sample),
                             reinterpret_cast<__ubuf__ uint8_t *>(sample), sizeof(*sample));
        }
        if (deferred) {
            const uint32_t status = StragglerDrainClockQueues(rdmaMemory, rank, rankSize, scratch, profile);
            if (status != TILEXR_RDMA_DEVICE_SUCCESS) { WriteClockFailure(samples, 0, status); }
        }
        return;
    }

    GM_ADDR localMemory = StragglerPeerMemory(commArgs, rank, rdmaMemory);
    GM_ADDR rootMemory = StragglerPeerMemory(commArgs, 0, rdmaMemory);
    if (localMemory == nullptr || rootMemory == nullptr) {
        WriteClockFailure(samples, 0);
        return;
    }
    const uint64_t slotOffset = ClockSyncProtocolSlotOffset(static_cast<uint32_t>(rank));
    GM_ADDR responseSlot = localMemory + slotOffset;
    GM_ADDR requestSlot = rootMemory + slotOffset;
    GM_ADDR ackSlot = rootMemory + ClockSyncAckSlotOffset(static_cast<uint32_t>(rank));
    GM_ADDR timestampSlot = localMemory + ClockSyncTimestampSlotOffset(static_cast<uint32_t>(rank));
    for (uint32_t protocolSequence = 0; protocolSequence < totalSampleCount;
         ++protocolSequence) {
        if (!compact) {
            const uint64_t readyWaitStart = static_cast<uint64_t>(AscendC::GetSystemCycle());
            while (true) {
                if (StragglerReadProtocolMatches(protocol, responseSlot, magic, protocolSequence,
                                                 TILEXR_CLOCK_SYNC_READY, rdmaMemory, profile)) {
                    break;
                }
                if (StragglerTimedOut(readyWaitStart, timeoutCycles)) {
                    const uint32_t sampleSequence = protocolSequence < warmupSampleCount ? 0u :
                        protocolSequence - warmupSampleCount;
                    WriteClockFailure(samples + static_cast<uint64_t>(sampleSequence) * sizeof(*sample),
                                      sampleSequence);
                    return;
                }
            }
        }
        protocol->magic = magic;
        protocol->sequence = protocolSequence;
        protocol->state = TILEXR_CLOCK_SYNC_REQUEST;
        protocol->t0 = 0;
        protocol->t1 = 0;
        protocol->t2 = 0;
        protocol->reserved0 = 0;
        protocol->reserved1 = 0;
        const uint64_t requestWriteStart = static_cast<uint64_t>(AscendC::GetSystemCycle());
        uint64_t requestPublication = 0;
        uint32_t sendStatus = StragglerSendClockSlot(rdmaMemory, 0, requestSlot, protocol,
            scratch, profile, deferred, totalSampleCount, &requestPublication);
        const uint64_t requestWriteEnd = static_cast<uint64_t>(AscendC::GetSystemCycle());
        const uint64_t t0 = rdmaMemory == nullptr ?
            requestWriteStart + (requestWriteEnd - requestWriteStart) / 2u : requestPublication;
        if (sendStatus != TILEXR_RDMA_DEVICE_SUCCESS) {
            WriteClockFailure(samples, 0, sendStatus);
            return;
        }

        const uint64_t waitStart = static_cast<uint64_t>(AscendC::GetSystemCycle());
        uint64_t t3 = 0;
        while (true) {
            const uint64_t readStart = static_cast<uint64_t>(AscendC::GetSystemCycle());
            if (StragglerReadProtocolMatches(protocol, responseSlot, magic, protocolSequence,
                                             TILEXR_CLOCK_SYNC_RESPONSE, rdmaMemory, profile)) {
                const uint64_t readEnd = static_cast<uint64_t>(AscendC::GetSystemCycle());
                t3 = readStart + (readEnd - readStart) / 2u;
                StragglerRecordMatchedWindow(profile, TILEXR_CLOCK_SYNC_RESPONSE, readStart, readEnd);
                break;
            }
            if (StragglerTimedOut(waitStart, timeoutCycles)) {
                const uint32_t sampleSequence = protocolSequence < warmupSampleCount ? 0u :
                    protocolSequence - warmupSampleCount;
                WriteClockFailure(samples + static_cast<uint64_t>(sampleSequence) * sizeof(*sample),
                                  sampleSequence);
                return;
            }
        }
        const uint64_t t1 = protocol->t1;
        const uint64_t timestampWaitStart = static_cast<uint64_t>(AscendC::GetSystemCycle());
        while (true) {
            if (StragglerReadProtocolMatches(protocol, timestampSlot, magic, protocolSequence,
                                             TILEXR_CLOCK_SYNC_TIMESTAMP, rdmaMemory, profile)) {
                break;
            }
            if (StragglerTimedOut(timestampWaitStart, timeoutCycles)) {
                const uint32_t sampleSequence = protocolSequence < warmupSampleCount ? 0u :
                    protocolSequence - warmupSampleCount;
                WriteClockFailure(samples + static_cast<uint64_t>(sampleSequence) * sizeof(*sample),
                                  sampleSequence);
                return;
            }
        }
        const uint64_t t2 = protocol->t2;
        protocol->magic = magic;
        protocol->sequence = protocolSequence;
        protocol->state = TILEXR_CLOCK_SYNC_ACK;
        protocol->t0 = 0;
        protocol->t1 = 0;
        protocol->t2 = 0;
        protocol->reserved0 = 0;
        protocol->reserved1 = 0;
        if (!compact) {
            sendStatus = StragglerSendClockSlot(rdmaMemory, 0, ackSlot, protocol, scratch, profile, deferred, totalSampleCount);
        }
        if (sendStatus != TILEXR_RDMA_DEVICE_SUCCESS) {
            WriteClockFailure(samples, 0, sendStatus);
            return;
        }
        if (protocolSequence < warmupSampleCount) {
            continue;
        }
        const uint32_t sampleSequence = protocolSequence - warmupSampleCount;
        sample->t0 = 0;
        sample->t1 = 0;
        sample->t2 = 0;
        sample->t3 = 0;
        sample->roundTripCycles = 0;
        sample->offsetCycles = 0;
        sample->reserved = 0;
        sample->sequence = sampleSequence;
        sample->status = TILEXR_CLOCK_SYNC_SAMPLE_SUCCESS;
        sample->t0 = t0;
        sample->t1 = t1;
        sample->t2 = t2;
        sample->t3 = t3;
        if (t3 >= t0 && t2 >= t1 && t3 - t0 >= t2 - t1) {
            sample->roundTripCycles = (t3 - t0) - (t2 - t1);
        } else {
            sample->status = TILEXR_CLOCK_SYNC_SAMPLE_INVALID_TIMING;
        }
        TileXRStragglerWriteGm(samples + static_cast<uint64_t>(sampleSequence) * sizeof(*sample),
                         reinterpret_cast<__ubuf__ uint8_t *>(sample), sizeof(*sample));
    }
    if (deferred) {
        const uint32_t status = StragglerDrainClockQueues(rdmaMemory, rank, rankSize, scratch, profile);
        if (status != TILEXR_RDMA_DEVICE_SUCCESS) { WriteClockFailure(samples, 0, status); }
    }
}

} // namespace TileXR

#define TILEXR_DIAGNOSTICS_CLOCK_SYNC_KERNEL_DEF() \
extern "C" __global__ __aicore__ void TileXRClockSync( \
    GM_ADDR commArgs, GM_ADDR samples, uint64_t magic, uint64_t timeoutCycles, uint32_t sampleCount, \
    uint32_t warmupSampleCount, GM_ADDR rdmaMemory) \
{ \
    if ASCEND_IS_AIV { \
        AscendC::TPipe pipe; \
        AscendC::TBuf<AscendC::QuePosition::VECCALC> protocolStorage, scratchStorage; \
        pipe.InitBuffer(protocolStorage, 256); \
        pipe.InitBuffer(scratchStorage, TILEXR_RDMA_DEVICE_SCRATCH_BYTES); \
        const auto scratch = scratchStorage.Get<uint8_t>(); \
        TILEXR_CLOCK_PROFILE_INIT(pipe, magic); \
        const uint64_t start = static_cast<uint64_t>(AscendC::GetSystemCycle()); \
        TileXR::TileXRClockSyncKernel(commArgs, samples, sampleCount, warmupSampleCount, magic, \
                                      timeoutCycles, rdmaMemory, scratch, profile); \
        if (AscendC::GetBlockIdx() == 0 && samples != nullptr && sampleCount > 0 && \
            sampleCount <= TileXR::TILEXR_CLOCK_SYNC_MAX_SAMPLES) { \
            const uint64_t end = static_cast<uint64_t>(AscendC::GetSystemCycle()); \
            __ubuf__ TileXR::TileXRClockSyncDeviceSample *timing = \
                reinterpret_cast<__ubuf__ TileXR::TileXRClockSyncDeviceSample *>( \
                    TileXR::TILEXR_STRAGGLER_UB_OFFSET); \
            timing->t0 = start; timing->t3 = end; \
            timing->t1 = 0; timing->t2 = 0; timing->roundTripCycles = 0; \
            timing->offsetCycles = 0; timing->reserved = magic; \
            timing->sequence = sampleCount; \
            timing->status = TileXR::TILEXR_CLOCK_SYNC_SAMPLE_SUCCESS; \
            TileXR::TileXRStragglerWriteGm( \
                samples + static_cast<uint64_t>(sampleCount) * sizeof(*timing), \
                reinterpret_cast<__ubuf__ uint8_t *>(timing), sizeof(*timing)); \
        } \
        TILEXR_CLOCK_PROFILE_FLUSH(rdmaMemory); \
    } \
}

#endif // TILEXR_DIAGNOSTICS_KERNEL_CLOCK_SYNC_KERNEL_H
