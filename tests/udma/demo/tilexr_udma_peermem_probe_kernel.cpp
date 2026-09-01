/*
 * Copyright (c) 2024-2026 TileXR Project
 * Licensed under the Apache License, Version 2.0
 */

#include "kernel_operator.h"
#include "tilexr_udma.h"
#include "tilexr_udma_peermem_probe_layout.h"

namespace {

using namespace TileXRUDMAPeerMemProbe;

__aicore__ inline uint64_t DeviceGenerationToken(uint32_t generation)
{
    return kTokenPrefix | static_cast<uint64_t>(generation + 1U);
}

__aicore__ inline uint64_t LoadTokenAcquire(__gm__ uint8_t* address)
{
    AscendC::GlobalTensor<uint64_t> tokenGlobal;
    tokenGlobal.SetGlobalBuffer(reinterpret_cast<__gm__ uint64_t*>(address), 1U);
    AscendC::DataCacheCleanAndInvalid<uint64_t,
        AscendC::CacheLine::SINGLE_CACHE_LINE,
        AscendC::DcciDst::CACHELINE_OUT>(tokenGlobal);
    return tokenGlobal.GetValue(0);
}

__aicore__ inline bool WaitTokenAcquire(
    __gm__ uint8_t* address, uint64_t expected)
{
    for (uint64_t poll = 0U; poll < kMaxPollIterations; ++poll) {
        if (LoadTokenAcquire(address) == expected) {
            return true;
        }
    }
    return false;
}

__aicore__ inline uint32_t PutAndWait(
    const __gm__ TileXR::CommArgs* args,
    const AscendC::LocalTensor<uint8_t>& wqeScratch,
    int32_t targetRank, __gm__ uint8_t* localSource,
    uint64_t remoteOffset, uint32_t bytes)
{
    TileXR::UDMACleanCacheLines(localSource, bytes);
    AscendC::PipeBarrier<PIPE_ALL>();
    uint32_t status = TileXR::UDMAPutNbiOnQp<uint8_t>(
        args, wqeScratch, targetRank, 0U, localSource, remoteOffset, bytes);
    if (status == TileXR::TILEXR_UDMA_STATUS_SUCCESS) {
        status = TileXR::UDMAQuietStatusOnQp(args, targetRank, 0U);
    }
    return status;
}

__aicore__ inline void CopyPayloadMte(
    __gm__ uint8_t* source, __gm__ uint8_t* destination,
    const AscendC::LocalTensor<uint8_t>& payloadLocal)
{
    AscendC::GlobalTensor<uint8_t> sourceGlobal;
    AscendC::GlobalTensor<uint8_t> destinationGlobal;
    sourceGlobal.SetGlobalBuffer(source, kPayloadBytes);
    destinationGlobal.SetGlobalBuffer(destination, kPayloadBytes);
    AscendC::DataCopy(payloadLocal, sourceGlobal, kPayloadBytes);
    TileXR::UDMASyncEvent<AscendC::HardEvent::MTE2_MTE3>();
    AscendC::DataCopy(destinationGlobal, payloadLocal, kPayloadBytes);
    TileXR::UDMASyncEvent<AscendC::HardEvent::MTE3_S>();
}

__aicore__ inline void StoreTokenMte(
    __gm__ uint8_t* destination,
    const AscendC::LocalTensor<uint64_t>& tokenLocal, uint64_t value)
{
    tokenLocal.SetValue(0U, value);
    AscendC::GlobalTensor<uint64_t> destinationGlobal;
    destinationGlobal.SetGlobalBuffer(
        reinterpret_cast<__gm__ uint64_t*>(destination), 1U);
    const AscendC::DataCopyExtParams copyParams {
        1U, static_cast<uint32_t>(sizeof(uint64_t)), 0U, 0U, 0U};
    TileXR::UDMASyncEvent<AscendC::HardEvent::S_MTE3>();
    AscendC::DataCopyPad(destinationGlobal, tokenLocal, copyParams);
    TileXR::UDMASyncEvent<AscendC::HardEvent::MTE3_S>();
}

__aicore__ inline void StoreStatus(
    __gm__ Status* output, uint64_t result, uint64_t completed,
    const __gm__ TileXR::CommArgs* args,
    const __gm__ TileXR::TileXRUDMARegistry* registry, uint64_t failureStep)
{
    if (output == nullptr) {
        return;
    }
    output->magic = kStatusMagic;
    output->result = result;
    output->completedGenerations = completed;
    output->role = static_cast<uint64_t>(args->rank);
    output->localPeerMem = reinterpret_cast<uint64_t>(args->peerMems[args->rank]);
    output->observedOwnerPeerMem =
        reinterpret_cast<uint64_t>(args->peerMems[kOwnerRank]);
    output->registryBase = reinterpret_cast<uint64_t>(
        registry == nullptr ? nullptr : registry->regions[args->rank].base);
    output->registryOwnerBase = reinterpret_cast<uint64_t>(
        registry == nullptr ? nullptr : registry->regions[kOwnerRank].base);
    output->failureStep = result == TileXR::TILEXR_UDMA_STATUS_SUCCESS ? 0U : failureStep;
    TileXR::UDMACleanCacheLines(
        reinterpret_cast<__gm__ uint8_t*>(output), sizeof(Status));
    AscendC::PipeBarrier<PIPE_ALL>();
}

} // namespace

using namespace TileXRUDMAPeerMemProbe;

extern "C" __global__ __aicore__ void tilexr_udma_peermem_probe_kernel(
    GM_ADDR commArgsGM, GM_ADDR outputGM, GM_ADDR statusGM,
    uint32_t generations)
{
    auto args = reinterpret_cast<__gm__ TileXR::CommArgs*>(commArgsGM);
    auto output = reinterpret_cast<__gm__ uint8_t*>(outputGM);
    auto status = reinterpret_cast<__gm__ Status*>(statusGM);

    AscendC::TPipe pipe;
    AscendC::TBuf<AscendC::QuePosition::VECCALC> wqeBuffer;
    pipe.InitBuffer(wqeBuffer, TileXR::TILEXR_UDMA_WQE_SCRATCH_BYTES);
    auto wqeScratch = wqeBuffer.Get<uint8_t>();
    auto tokenLocal = wqeBuffer.Get<uint64_t>();

    if (args == nullptr || args->rankSize != kRankSize ||
        generations == 0U || generations > kMaxGenerations ||
        args->peerMemBytes < kRequiredPeerMemBytes ||
        !TileXR::UDMARegistryEnabled(args)) {
        return;
    }
    auto registry = TileXR::GetUDMARegistry(args);
    if (registry == nullptr || registry->regions[args->rank].base !=
            args->peerMems[args->rank] ||
        registry->regions[args->rank].bytes != args->peerMemBytes) {
        StoreStatus(status, TileXR::TILEXR_UDMA_STATUS_INVALID, 0U, args, registry, 1U);
        return;
    }

    __gm__ uint8_t* localPeer = args->peerMems[args->rank];
    uint64_t completed = 0U;
    uint32_t result = TileXR::TILEXR_UDMA_STATUS_SUCCESS;
    uint64_t failureStep = 0U;

    if (args->rank == kOwnerRank) {
        failureStep = 10U;
        if (!WaitTokenAcquire(localPeer + kPreparedOffset, kInitToken)) {
            result = TileXR::TILEXR_UDMA_STATUS_CQ_TIMEOUT;
        } else {
            failureStep = 11U;
            result = PutAndWait(args, wqeScratch, kProducerRank,
                localPeer + kInitSourceOffset, kInitAckOffset, kTokenBytes);
        }
    } else if (args->rank == kProducerRank) {
        failureStep = 20U;
        result = PutAndWait(args, wqeScratch, kOwnerRank,
            localPeer + kInitSourceOffset, kPreparedOffset, kTokenBytes);
        if (result == TileXR::TILEXR_UDMA_STATUS_SUCCESS) {
            failureStep = 21U;
            if (!WaitTokenAcquire(localPeer + kInitAckOffset, kInitToken)) {
                result = TileXR::TILEXR_UDMA_STATUS_CQ_TIMEOUT;
            }
        }
        for (uint32_t generation = 0U;
            result == TileXR::TILEXR_UDMA_STATUS_SUCCESS && generation < generations;
            ++generation) {
            failureStep = 100U + static_cast<uint64_t>(generation) * 3U;
            result = PutAndWait(args, wqeScratch, kOwnerRank,
                localPeer + kSourcePayloadOffset +
                    static_cast<uint64_t>(generation) * kPayloadBytes,
                kPayloadOffset, kPayloadBytes);
            if (result == TileXR::TILEXR_UDMA_STATUS_SUCCESS) {
                failureStep += 1U;
                result = PutAndWait(args, wqeScratch, kOwnerRank,
                    localPeer + kReadySourceOffset +
                        static_cast<uint64_t>(generation) * kTokenBytes,
                    kReadyOffset, kTokenBytes);
            }
            if (result == TileXR::TILEXR_UDMA_STATUS_SUCCESS) {
                failureStep += 1U;
                if (!WaitTokenAcquire(localPeer + kAckOffset,
                        DeviceGenerationToken(generation))) {
                    result = TileXR::TILEXR_UDMA_STATUS_CQ_TIMEOUT;
                }
            }
            if (result == TileXR::TILEXR_UDMA_STATUS_SUCCESS) {
                completed = generation + 1U;
            }
        }
    } else if (args->rank == kConsumerRank) {
        AscendC::TBuf<AscendC::QuePosition::VECCALC> payloadBuffer;
        pipe.InitBuffer(payloadBuffer, kPayloadBytes);
        auto payloadLocal = payloadBuffer.Get<uint8_t>();
        __gm__ uint8_t* ownerPeer = args->peerMems[kOwnerRank];
        if (ownerPeer == nullptr || output == nullptr) {
            result = TileXR::TILEXR_UDMA_STATUS_INVALID;
        }
        for (uint32_t generation = 0U;
            result == TileXR::TILEXR_UDMA_STATUS_SUCCESS && generation < generations;
            ++generation) {
            failureStep = 200U + static_cast<uint64_t>(generation) * 2U;
            if (!WaitTokenAcquire(ownerPeer + kReadyOffset,
                    DeviceGenerationToken(generation))) {
                result = TileXR::TILEXR_UDMA_STATUS_CQ_TIMEOUT;
                break;
            }
            TileXR::UDMACleanCacheLines(
                ownerPeer + kPayloadOffset, kPayloadBytes);
            AscendC::PipeBarrier<PIPE_ALL>();
            CopyPayloadMte(ownerPeer + kPayloadOffset,
                output + static_cast<uint64_t>(generation) * kPayloadBytes,
                payloadLocal);
            failureStep += 1U;
            StoreTokenMte(args->peerMems[kProducerRank] + kAckOffset,
                tokenLocal, DeviceGenerationToken(generation));
            completed = generation + 1U;
        }
        if (result == TileXR::TILEXR_UDMA_STATUS_SUCCESS) {
            failureStep = 299U;
            StoreTokenMte(ownerPeer + kDoneOffset, tokenLocal,
                DeviceGenerationToken(generations - 1U));
        }
    } else {
        result = TileXR::TILEXR_UDMA_STATUS_INVALID;
    }

    if (args->rank == kOwnerRank &&
        result == TileXR::TILEXR_UDMA_STATUS_SUCCESS) {
        failureStep = 12U;
        if (WaitTokenAcquire(localPeer + kDoneOffset,
                DeviceGenerationToken(generations - 1U))) {
            completed = generations;
        } else {
            result = TileXR::TILEXR_UDMA_STATUS_CQ_TIMEOUT;
        }
    }
    StoreStatus(status, result, completed, args, registry, failureStep);
}
