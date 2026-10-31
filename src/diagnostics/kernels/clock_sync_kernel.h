#ifndef TILEXR_DIAGNOSTICS_KERNEL_CLOCK_SYNC_KERNEL_H
#define TILEXR_DIAGNOSTICS_KERNEL_CLOCK_SYNC_KERNEL_H

#include "comm_args.h"
#include "kernel_operator.h"
#include "tilexr_straggler_device.h"
#include "tilexr_straggler_layout.h"

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
    uint64_t sequence, uint64_t state)
{
    StragglerReadGm(reinterpret_cast<__ubuf__ uint8_t *>(protocol), slot, sizeof(*protocol));
    return protocol->state == state && protocol->sequence == sequence && protocol->magic == magic;
}

__attribute__((always_inline)) inline __aicore__ GM_ADDR StragglerPeerMemory(
    GM_ADDR commArgs, int rank)
{
    AscendC::GlobalTensor<GM_ADDR> peerMems;
    peerMems.SetGlobalBuffer(&(reinterpret_cast<__gm__ CommArgs *>(commArgs))->peerMems[0],
                             TILEXR_MAX_RANK_SIZE);
    return peerMems.GetValue(rank);
}

__attribute__((always_inline)) inline __aicore__ void WriteClockFailure(
    GM_ADDR samples, uint32_t sequence)
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
    sample->reserved = 0;
    sample->sequence = sequence;
    sample->status = TILEXR_CLOCK_SYNC_SAMPLE_TERMINAL_FAILURE;
    TileXRStragglerWriteGm(samples, reinterpret_cast<__ubuf__ uint8_t *>(sample), sizeof(*sample));
}

__attribute__((always_inline)) inline __aicore__ void TileXRClockSyncKernel(
    GM_ADDR commArgs, GM_ADDR samples, uint32_t sampleCount, uint32_t warmupSampleCount,
    uint64_t magic, uint64_t timeoutCycles)
{
    if (AscendC::GetBlockIdx() != 0 || commArgs == nullptr || samples == nullptr || sampleCount == 0 ||
        sampleCount > TILEXR_CLOCK_SYNC_MAX_SAMPLES ||
        warmupSampleCount > TILEXR_CLOCK_SYNC_MAX_WARMUP_SAMPLES) {
        return;
    }
    const uint32_t totalSampleCount = warmupSampleCount + sampleCount;
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
        GM_ADDR rootMemory = StragglerPeerMemory(commArgs, 0);
        if (rootMemory == nullptr) {
            WriteClockFailure(samples, 0);
            return;
        }
        for (uint32_t protocolSequence = 0; protocolSequence < totalSampleCount;
             ++protocolSequence) {
            for (int peer = 1; peer < rankSize; ++peer) {
                GM_ADDR peerMemory = StragglerPeerMemory(commArgs, peer);
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
                TileXRStragglerWriteGm(responseSlot, reinterpret_cast<__ubuf__ uint8_t *>(protocol),
                                       sizeof(*protocol));
                const uint64_t waitStart = static_cast<uint64_t>(AscendC::GetSystemCycle());
                uint64_t t1 = 0;
                while (true) {
                    const uint64_t readStart = static_cast<uint64_t>(AscendC::GetSystemCycle());
                    if (StragglerReadProtocolMatches(protocol, requestSlot, magic, protocolSequence,
                                                     TILEXR_CLOCK_SYNC_REQUEST)) {
                        const uint64_t readEnd = static_cast<uint64_t>(AscendC::GetSystemCycle());
                        t1 = readStart + (readEnd - readStart) / 2u;
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
                TileXRStragglerWriteGm(responseSlot,
                                       reinterpret_cast<__ubuf__ uint8_t *>(protocol), sizeof(*protocol));
                const uint64_t responseWriteEnd =
                    static_cast<uint64_t>(AscendC::GetSystemCycle());
                const uint64_t t2 =
                    responseWriteStart + (responseWriteEnd - responseWriteStart) / 2u;
                protocol->magic = magic;
                protocol->sequence = protocolSequence;
                protocol->state = TILEXR_CLOCK_SYNC_TIMESTAMP;
                protocol->t0 = 0;
                protocol->t1 = 0;
                protocol->t2 = t2;
                protocol->reserved0 = 0;
                protocol->reserved1 = 0;
                TileXRStragglerWriteGm(timestampSlot,
                                       reinterpret_cast<__ubuf__ uint8_t *>(protocol), sizeof(*protocol));
                const uint64_t ackWaitStart = static_cast<uint64_t>(AscendC::GetSystemCycle());
                while (true) {
                    if (StragglerReadProtocolMatches(protocol, ackSlot, magic, protocolSequence,
                                                     TILEXR_CLOCK_SYNC_ACK)) {
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
        return;
    }

    GM_ADDR localMemory = StragglerPeerMemory(commArgs, rank);
    GM_ADDR rootMemory = StragglerPeerMemory(commArgs, 0);
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
        const uint64_t readyWaitStart = static_cast<uint64_t>(AscendC::GetSystemCycle());
        while (true) {
            if (StragglerReadProtocolMatches(protocol, responseSlot, magic, protocolSequence,
                                             TILEXR_CLOCK_SYNC_READY)) {
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
        protocol->magic = magic;
        protocol->sequence = protocolSequence;
        protocol->state = TILEXR_CLOCK_SYNC_REQUEST;
        protocol->t0 = 0;
        protocol->t1 = 0;
        protocol->t2 = 0;
        protocol->reserved0 = 0;
        protocol->reserved1 = 0;
        const uint64_t requestWriteStart = static_cast<uint64_t>(AscendC::GetSystemCycle());
        TileXRStragglerWriteGm(requestSlot, reinterpret_cast<__ubuf__ uint8_t *>(protocol),
                               sizeof(*protocol));
        const uint64_t requestWriteEnd = static_cast<uint64_t>(AscendC::GetSystemCycle());
        const uint64_t t0 = requestWriteStart + (requestWriteEnd - requestWriteStart) / 2u;

        const uint64_t waitStart = static_cast<uint64_t>(AscendC::GetSystemCycle());
        uint64_t t3 = 0;
        while (true) {
            const uint64_t readStart = static_cast<uint64_t>(AscendC::GetSystemCycle());
            if (StragglerReadProtocolMatches(protocol, responseSlot, magic, protocolSequence,
                                             TILEXR_CLOCK_SYNC_RESPONSE)) {
                const uint64_t readEnd = static_cast<uint64_t>(AscendC::GetSystemCycle());
                t3 = readStart + (readEnd - readStart) / 2u;
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
                                             TILEXR_CLOCK_SYNC_TIMESTAMP)) {
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
        TileXRStragglerWriteGm(ackSlot, reinterpret_cast<__ubuf__ uint8_t *>(protocol),
                               sizeof(*protocol));
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
}

} // namespace TileXR

#define TILEXR_DIAGNOSTICS_CLOCK_SYNC_KERNEL_DEF() \
extern "C" __global__ __aicore__ void TileXRClockSync( \
    GM_ADDR commArgs, GM_ADDR samples, uint64_t magic, uint64_t timeoutCycles, uint32_t sampleCount, \
    uint32_t warmupSampleCount) \
{ \
    if ASCEND_IS_AIV { \
        TileXR::TileXRClockSyncKernel(commArgs, samples, sampleCount, warmupSampleCount, magic, \
                                      timeoutCycles); \
    } \
}

#endif // TILEXR_DIAGNOSTICS_KERNEL_CLOCK_SYNC_KERNEL_H
