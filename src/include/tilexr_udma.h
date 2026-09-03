/*
 * Copyright (c) 2024-2026 TileXR Project
 * Licensed under the Apache License, Version 2.0
 */

#ifndef TILEXR_UDMA_H
#define TILEXR_UDMA_H

#include "kernel_operator.h"
#include "comm_args.h"
#include "tilexr_udma_reg.h"
#include "tilexr_udma_types.h"

namespace TileXR {

constexpr uint32_t TILEXR_UDMA_SQE_FLAG_COMPLETION = 0x20U;
constexpr uint32_t TILEXR_UDMA_SQE_FLAG_STRONG_ORDER = 0x02U;
constexpr uint32_t TILEXR_UDMA_SQE_FLAG_ORDERED_COMPLETION =
    TILEXR_UDMA_SQE_FLAG_COMPLETION | TILEXR_UDMA_SQE_FLAG_STRONG_ORDER;
constexpr uint32_t TILEXR_UDMA_WQE_BB_BYTES = 64U;
constexpr uint32_t TILEXR_UDMA_WQE_SCRATCH_BYTES = 2U * TILEXR_UDMA_WQE_BB_BYTES;
constexpr uint32_t TILEXR_UDMA_WQE_SCRATCH_ALIGNMENT = 32U;

constexpr uint32_t TILEXR_UDMA_STATUS_SUCCESS = 0U;
constexpr uint32_t TILEXR_UDMA_STATUS_CQ_TIMEOUT = 0xFFU;
constexpr uint32_t TILEXR_UDMA_STATUS_SQ_FULL = 0xFFFFFFFEU;
constexpr uint32_t TILEXR_UDMA_STATUS_INVALID = 0xFFFFFFFFU;
constexpr uint32_t TILEXR_UDMA_DEVICE_MAX_QP_COUNT = 48U;

/**
 * @file tilexr_udma.h
 * @brief Device-side UDMA wrapper for TileXR-registered memory.
 *
 * Host side registers ordinary device memory with TileXRUDMARegister. Device
 * kernels then use byte offsets into that registered region for PUT/GET/SIGNAL.
 * This header is self-contained for the device path and intentionally avoids
 * symmetric-memory APIs.
 */

#if (defined(__NPU_ARCH__) && (__NPU_ARCH__ == 3510)) || \
    (defined(CATLASS_ARCH) && (CATLASS_ARCH == 3510)) || defined(TILEXR_UDMA_FORCE_ENABLE)
constexpr bool TILEXR_UDMA_ARCH_SUPPORTED = true;
#else
constexpr bool TILEXR_UDMA_ARCH_SUPPORTED = false;
#endif

struct UDMASignalParams {
    __gm__ uint64_t* sigAddr;
    uint64_t signal;
};

static_assert(sizeof(UDMASqeCtx) + sizeof(UDMASgeCtx) == TILEXR_UDMA_WQE_BB_BYTES,
    "UDMA WRITE/READ WQE must occupy one basic block");
static_assert(sizeof(UDMASqeCtx) + sizeof(UDMANotifyCtx) + sizeof(UDMASgeCtx) <=
        TILEXR_UDMA_WQE_SCRATCH_BYTES,
    "UDMA WRITE_WITH_NOTIFY WQE must fit in two basic blocks");

__aicore__ inline bool UDMAEnabled(const __gm__ CommArgs* args)
{
    return args != nullptr && ((args->extraFlag & ExtraFlag::UDMA) != 0) && args->udmaInfoPtr != nullptr;
}

__aicore__ inline bool UDMARegistryEnabled(const __gm__ CommArgs* args)
{
    return UDMAEnabled(args) && args->udmaRegistryPtr != nullptr;
}

__aicore__ inline uint32_t UDMAQpCount(const __gm__ CommArgs* args)
{
    if (!UDMAEnabled(args)) {
        return 0U;
    }
    const uint32_t qpNum = reinterpret_cast<__gm__ UDMAInfo*>(args->udmaInfoPtr)->qpNum;
    return qpNum > 0U && qpNum <= TILEXR_UDMA_DEVICE_MAX_QP_COUNT ? qpNum : 0U;
}

__aicore__ inline bool UDMAQpValid(const __gm__ CommArgs* args, uint32_t qpIdx)
{
    return qpIdx < UDMAQpCount(args);
}

__aicore__ inline bool UDMARankValid(const __gm__ CommArgs* args, int peer)
{
    return args != nullptr && args->rankSize > 0 && args->rankSize <= TILEXR_MAX_RANK_SIZE &&
        peer >= 0 && peer < args->rankSize && peer != args->rank;
}

__aicore__ inline bool UDMAQueueOperationValid(
    const __gm__ CommArgs* args, int peer, uint32_t qpIdx)
{
    if (!UDMARankValid(args, peer)) {
        return false;
    }
    return UDMAQpValid(args, qpIdx);
}

__aicore__ inline __gm__ UDMAInfo* GetUDMAInfo(const __gm__ CommArgs* args)
{
    return reinterpret_cast<__gm__ UDMAInfo*>(args->udmaInfoPtr);
}

__aicore__ inline __gm__ TileXRUDMARegistry* GetUDMARegistry(const __gm__ CommArgs* args)
{
    return reinterpret_cast<__gm__ TileXRUDMARegistry*>(args->udmaRegistryPtr);
}

__aicore__ inline bool UDMAProfileRegistryValid(
    const __gm__ CommArgs* args, const __gm__ UDMAInfo* udmaInfo,
    const __gm__ TileXRUDMAProfileRegistry* registry)
{
    if (args == nullptr || (args->extraFlag & ExtraFlag::UDMA) == 0U ||
        udmaInfo == nullptr || registry == nullptr ||
        registry->magic != TILEXR_UDMA_PROFILE_MAGIC ||
        registry->version != TILEXR_UDMA_PROFILE_VERSION ||
        args->rankSize <= 0 || args->rankSize > TILEXR_MAX_RANK_SIZE ||
        args->rank < 0 || args->rank >= args->rankSize ||
        registry->rankSize != static_cast<uint32_t>(args->rankSize) ||
        registry->regionCount == 0 ||
        registry->regionCount > TILEXR_UDMA_PROFILE_MAX_REGIONS ||
        registry->qpCount == 0 || registry->qpCount > TILEXR_UDMA_DEVICE_MAX_QP_COUNT ||
        udmaInfo->qpNum != registry->qpCount || udmaInfo->sqPtr == 0U ||
        udmaInfo->memPtr == 0U) {
        return false;
    }
    return true;
}

__aicore__ inline const __gm__ TileXRUDMAProfileRegionDesc* UDMAProfileGetRegion(
    const __gm__ TileXRUDMAProfileRegistry* registry, int rank, uint32_t region)
{
    if (registry == nullptr || rank < 0 || static_cast<uint32_t>(rank) >= registry->rankSize ||
        region >= registry->regionCount || registry->rankSize > TILEXR_MAX_RANK_SIZE ||
        registry->regionCount > TILEXR_UDMA_PROFILE_MAX_REGIONS) {
        return nullptr;
    }
    const uint64_t index = static_cast<uint64_t>(rank) *
        TILEXR_UDMA_PROFILE_MAX_REGIONS + region;
    return &registry->regions[index];
}

__aicore__ inline bool UDMAProfileRegisteredRangeValid(
    const __gm__ TileXRUDMAProfileRegistry* registry, int rank, uint32_t region,
    uint64_t byteOffset, uint64_t byteCount)
{
    const __gm__ TileXRUDMAProfileRegionDesc* desc =
        UDMAProfileGetRegion(registry, rank, region);
    if (desc == nullptr || desc->base == nullptr || desc->bytes == 0 ||
        byteOffset > desc->bytes) {
        return false;
    }
    return byteCount <= desc->bytes - byteOffset;
}

__aicore__ inline __gm__ uint8_t* UDMAProfileRegisteredAddr(
    const __gm__ TileXRUDMAProfileRegistry* registry, int rank, uint32_t region,
    uint64_t byteOffset)
{
    const __gm__ TileXRUDMAProfileRegionDesc* desc =
        UDMAProfileGetRegion(registry, rank, region);
    if (desc == nullptr || desc->base == nullptr || byteOffset > desc->bytes) {
        return nullptr;
    }
    return reinterpret_cast<__gm__ uint8_t*>(desc->base + byteOffset);
}

__aicore__ inline bool UDMARegisteredRangeValid(
    const __gm__ TileXRUDMARegistry* registry, int targetRank, uint64_t byteOffset, uint64_t byteCount)
{
    if (registry == nullptr || registry->magic != TILEXR_UDMA_REGISTRY_MAGIC ||
        registry->version != TILEXR_UDMA_REGISTRY_VERSION || registry->regionCount == 0 ||
        registry->regionCount > TILEXR_UDMA_MAX_REGIONS || registry->rankSize == 0 ||
        registry->rankSize > TILEXR_MAX_RANK_SIZE || targetRank < 0 ||
        static_cast<uint32_t>(targetRank) >= registry->rankSize) {
        return false;
    }
    const auto& region = registry->regions[targetRank];
    if (region.base == nullptr || byteOffset > region.bytes) {
        return false;
    }
    return byteCount <= region.bytes - byteOffset;
}

__aicore__ inline __gm__ uint8_t* UDMARegisteredRemoteAddr(
    const __gm__ TileXRUDMARegistry* registry, int targetRank, uint64_t byteOffset)
{
    return reinterpret_cast<__gm__ uint8_t*>(registry->regions[targetRank].base + byteOffset);
}

__aicore__ inline void UDMACleanCacheLines(__gm__ uint8_t* addr, uint64_t length)
{
    if (addr == nullptr || length == 0) {
        return;
    }
    __gm__ uint8_t* start = reinterpret_cast<__gm__ uint8_t*>(
        reinterpret_cast<uint64_t>(addr) / TILEXR_UDMA_CACHE_LINE_SIZE * TILEXR_UDMA_CACHE_LINE_SIZE);
    __gm__ uint8_t* end = reinterpret_cast<__gm__ uint8_t*>(
        (reinterpret_cast<uint64_t>(addr) + length - 1) / TILEXR_UDMA_CACHE_LINE_SIZE *
        TILEXR_UDMA_CACHE_LINE_SIZE);
    AscendC::GlobalTensor<uint8_t> global;
    global.SetGlobalBuffer(start);
    for (uint64_t i = 0; i <= static_cast<uint64_t>(end - start); i += TILEXR_UDMA_CACHE_LINE_SIZE) {
        __asm__ __volatile__("");
        AscendC::DataCacheCleanAndInvalid<uint8_t,
            AscendC::CacheLine::SINGLE_CACHE_LINE, AscendC::DcciDst::CACHELINE_OUT>(global[i]);
        __asm__ __volatile__("");
    }
}

__aicore__ inline __gm__ UDMAWQCtx* UDMAGetWQCtx(__gm__ UDMAInfo* udmaInfo, uint32_t pe, uint32_t qpIdx)
{
    const uint32_t qpNum = udmaInfo == nullptr ? 0U : udmaInfo->qpNum;
    if (qpNum == 0U || qpNum > TILEXR_UDMA_DEVICE_MAX_QP_COUNT || qpIdx >= qpNum) {
        return nullptr;
    }
    const uint64_t entry = static_cast<uint64_t>(pe) * qpNum + qpIdx;
    return reinterpret_cast<__gm__ UDMAWQCtx*>(udmaInfo->sqPtr + entry * sizeof(UDMAWQCtx));
}

__aicore__ inline __gm__ UDMACQCtx* UDMAGetSCQCtx(__gm__ UDMAInfo* udmaInfo, uint32_t pe, uint32_t qpIdx)
{
    const uint32_t qpNum = udmaInfo == nullptr ? 0U : udmaInfo->qpNum;
    if (qpNum == 0U || qpNum > TILEXR_UDMA_DEVICE_MAX_QP_COUNT || qpIdx >= qpNum) {
        return nullptr;
    }
    const uint64_t entry = static_cast<uint64_t>(pe) * qpNum + qpIdx;
    return reinterpret_cast<__gm__ UDMACQCtx*>(udmaInfo->scqPtr + entry * sizeof(UDMACQCtx));
}

__aicore__ inline __gm__ UDMAMemInfo* UDMAGetRemoteMemInfo(
    __gm__ UDMAInfo* udmaInfo, uint32_t pe, uint32_t qpIdx)
{
    const uint32_t qpNum = udmaInfo == nullptr ? 0U : udmaInfo->qpNum;
    if (qpNum == 0U || qpNum > TILEXR_UDMA_DEVICE_MAX_QP_COUNT || qpIdx >= qpNum) {
        return nullptr;
    }
    const uint64_t entry = static_cast<uint64_t>(pe) * qpNum + qpIdx;
    return reinterpret_cast<__gm__ UDMAMemInfo*>(
        udmaInfo->memPtr + sizeof(UDMAMemInfo) * entry);
}

__aicore__ inline __gm__ UDMAMemInfo* UDMAGetRemoteMemInfo(__gm__ UDMAInfo* udmaInfo, uint32_t pe)
{
    return UDMAGetRemoteMemInfo(udmaInfo, pe, 0U);
}

__aicore__ inline uint32_t UDMAWqeBBCnt(UDMAOpcode opcode)
{
    return opcode == UDMAOpcode::WRITE_WITH_NOTIFY ? 2U : 1U;
}

__aicore__ inline void UDMAPollCQUpdateInfo(
    uint32_t cqTail, uint32_t sqTail, __gm__ UDMACQCtx* cqCtxEntry, __gm__ UDMAWQCtx* wqCtxEntry)
{
    st_dev(cqTail, reinterpret_cast<__gm__ uint32_t*>(cqCtxEntry->tailAddr), 0);
    st_dev(sqTail, reinterpret_cast<__gm__ uint32_t*>(wqCtxEntry->tailAddr), 0);
    st_dev(static_cast<uint32_t>(cqTail & 0xFFFFFFU),
        reinterpret_cast<__gm__ uint32_t*>(cqCtxEntry->dbAddr), 0);
}

__aicore__ inline uint32_t UDMAPollCQ(__gm__ UDMAInfo* udmaInfo, uint32_t pe, uint32_t qpIdx, uint32_t idx)
{
    if (udmaInfo == nullptr || udmaInfo->qpNum == 0U ||
        udmaInfo->qpNum > TILEXR_UDMA_DEVICE_MAX_QP_COUNT || qpIdx >= udmaInfo->qpNum || udmaInfo->sqPtr == 0U ||
        udmaInfo->scqPtr == 0U) {
        return TILEXR_UDMA_STATUS_INVALID;
    }
    __gm__ UDMACQCtx* cqCtxEntry = UDMAGetSCQCtx(udmaInfo, pe, qpIdx);
    __gm__ UDMAWQCtx* wqCtxEntry = UDMAGetWQCtx(udmaInfo, pe, qpIdx);
    if (wqCtxEntry->bufAddr == 0U || wqCtxEntry->headAddr == 0U ||
        wqCtxEntry->tailAddr == 0U || wqCtxEntry->depth != TILEXR_UDMA_SQ_BB_COUNT ||
        wqCtxEntry->baseBkShift >= 32U ||
        (1U << wqCtxEntry->baseBkShift) < sizeof(UDMASqeCtx) + sizeof(UDMASgeCtx) ||
        cqCtxEntry->bufAddr == 0U || cqCtxEntry->tailAddr == 0U || cqCtxEntry->dbAddr == 0U ||
        cqCtxEntry->depth != TILEXR_UDMA_CQ_DEPTH || cqCtxEntry->baseBkShift >= 32U ||
        (1U << cqCtxEntry->baseBkShift) < sizeof(UDMACqeCtx)) {
        return TILEXR_UDMA_STATUS_INVALID;
    }
    uint64_t cqBaseAddr = cqCtxEntry->bufAddr;
    uint32_t cqeSize = 1U << cqCtxEntry->baseBkShift;
    uint32_t cqTail = ld_dev(reinterpret_cast<__gm__ uint32_t*>(cqCtxEntry->tailAddr), 0);
    uint32_t sqTail = ld_dev(reinterpret_cast<__gm__ uint32_t*>(wqCtxEntry->tailAddr), 0);
    const uint32_t completionCount = idx - cqTail;
    if (completionCount > cqCtxEntry->depth) {
        return TILEXR_UDMA_STATUS_INVALID;
    }
    for (uint32_t completed = 0U; completed < completionCount; ++completed) {
        __gm__ UDMACqeCtx* cqeAddr = reinterpret_cast<__gm__ UDMACqeCtx*>(
            cqBaseAddr + cqeSize * (cqTail & (TILEXR_UDMA_CQ_DEPTH - 1U)));
        bool validOwner = ((cqTail / TILEXR_UDMA_CQ_DEPTH) & 1U) != 0U;
        uint32_t times = 0;
        while ((validOwner ^ (cqeAddr->owner != 0)) == 0 && times < TILEXR_UDMA_MAX_RETRY_TIMES) {
            UDMACleanCacheLines(reinterpret_cast<__gm__ uint8_t*>(cqeAddr), sizeof(UDMACqeCtx));
            ++times;
        }
        if (times >= TILEXR_UDMA_MAX_RETRY_TIMES) {
            return TILEXR_UDMA_STATUS_CQ_TIMEOUT;
        }
        uint8_t status = cqeAddr->status & 0xFF;
        uint8_t subStatus = cqeAddr->substatus & 0xFF;
        if (status != 0 || subStatus != 0) {
            return (static_cast<uint32_t>(status) << 8) | subStatus;
        }

        const uint32_t sqHead = ld_dev(reinterpret_cast<__gm__ uint32_t*>(wqCtxEntry->headAddr), 0);
        const uint32_t sqOutstanding = sqHead - sqTail;
        if (sqOutstanding == 0U || sqOutstanding > wqCtxEntry->depth) {
            return TILEXR_UDMA_STATUS_INVALID;
        }
        const uint32_t tailIndex = sqTail % wqCtxEntry->depth;
        const uint32_t completedEntryIndex = cqeAddr->entryIdx % wqCtxEntry->depth;
        const uint32_t completedBb =
            (completedEntryIndex + wqCtxEntry->depth - tailIndex) %
                wqCtxEntry->depth + 1U;
        if (completedBb > TILEXR_UDMA_MAX_SQE_BB_NUM ||
            completedBb > sqOutstanding) {
            return TILEXR_UDMA_STATUS_INVALID;
        }
        sqTail += completedBb;
        ++cqTail;
        UDMAPollCQUpdateInfo(cqTail, sqTail, cqCtxEntry, wqCtxEntry);
    }
    return TILEXR_UDMA_STATUS_SUCCESS;
}

__aicore__ inline uint32_t UDMAPollCQUntilSqTail(
    __gm__ UDMAInfo* udmaInfo, uint32_t pe, uint32_t qpIdx, uint32_t sqTarget)
{
    if (udmaInfo == nullptr || udmaInfo->qpNum == 0U ||
        udmaInfo->qpNum > TILEXR_UDMA_DEVICE_MAX_QP_COUNT || qpIdx >= udmaInfo->qpNum ||
        udmaInfo->sqPtr == 0U || udmaInfo->scqPtr == 0U) {
        return TILEXR_UDMA_STATUS_INVALID;
    }
    __gm__ UDMACQCtx* cqCtxEntry = UDMAGetSCQCtx(udmaInfo, pe, qpIdx);
    __gm__ UDMAWQCtx* wqCtxEntry = UDMAGetWQCtx(udmaInfo, pe, qpIdx);
    if (wqCtxEntry->bufAddr == 0U || wqCtxEntry->headAddr == 0U ||
        wqCtxEntry->tailAddr == 0U || wqCtxEntry->depth != TILEXR_UDMA_SQ_BB_COUNT ||
        wqCtxEntry->baseBkShift >= 32U ||
        (1U << wqCtxEntry->baseBkShift) < sizeof(UDMASqeCtx) + sizeof(UDMASgeCtx) ||
        cqCtxEntry->bufAddr == 0U || cqCtxEntry->tailAddr == 0U || cqCtxEntry->dbAddr == 0U ||
        cqCtxEntry->depth != TILEXR_UDMA_CQ_DEPTH || cqCtxEntry->baseBkShift >= 32U ||
        (1U << cqCtxEntry->baseBkShift) < sizeof(UDMACqeCtx)) {
        return TILEXR_UDMA_STATUS_INVALID;
    }
    const uint32_t cqeSize = 1U << cqCtxEntry->baseBkShift;
    uint32_t cqTail = ld_dev(reinterpret_cast<__gm__ uint32_t*>(cqCtxEntry->tailAddr), 0);
    uint32_t sqTail = ld_dev(reinterpret_cast<__gm__ uint32_t*>(wqCtxEntry->tailAddr), 0);
    if (sqTarget - sqTail > wqCtxEntry->depth) {
        return TILEXR_UDMA_STATUS_INVALID;
    }
    while (sqTail != sqTarget) {
        __gm__ UDMACqeCtx* cqeAddr = reinterpret_cast<__gm__ UDMACqeCtx*>(
            cqCtxEntry->bufAddr + cqeSize * (cqTail & (TILEXR_UDMA_CQ_DEPTH - 1U)));
        bool validOwner = ((cqTail / TILEXR_UDMA_CQ_DEPTH) & 1U) != 0U;
        uint32_t times = 0U;
        while ((validOwner ^ (cqeAddr->owner != 0)) == 0 && times < TILEXR_UDMA_MAX_RETRY_TIMES) {
            UDMACleanCacheLines(reinterpret_cast<__gm__ uint8_t*>(cqeAddr), sizeof(UDMACqeCtx));
            ++times;
        }
        if (times >= TILEXR_UDMA_MAX_RETRY_TIMES) {
            return TILEXR_UDMA_STATUS_CQ_TIMEOUT;
        }
        uint8_t status = cqeAddr->status & 0xFF;
        uint8_t subStatus = cqeAddr->substatus & 0xFF;
        if (status != 0 || subStatus != 0) {
            return (static_cast<uint32_t>(status) << 8) | subStatus;
        }

        const uint32_t sqHead = ld_dev(reinterpret_cast<__gm__ uint32_t*>(wqCtxEntry->headAddr), 0);
        const uint32_t sqOutstanding = sqHead - sqTail;
        const uint32_t remainingToTarget = sqTarget - sqTail;
        if (sqOutstanding == 0U || sqOutstanding > wqCtxEntry->depth ||
            remainingToTarget == 0U || remainingToTarget > sqOutstanding) {
            return TILEXR_UDMA_STATUS_INVALID;
        }
        const uint32_t tailIndex = sqTail % wqCtxEntry->depth;
        const uint32_t completedEntryIndex = cqeAddr->entryIdx % wqCtxEntry->depth;
        const uint32_t completedBb =
            (completedEntryIndex + wqCtxEntry->depth - tailIndex) %
                wqCtxEntry->depth + 1U;
        if (completedBb == 0U || completedBb > remainingToTarget ||
            completedBb > sqOutstanding) {
            return TILEXR_UDMA_STATUS_INVALID;
        }
        sqTail += completedBb;
        ++cqTail;
        UDMAPollCQUpdateInfo(cqTail, sqTail, cqCtxEntry, wqCtxEntry);
    }
    return TILEXR_UDMA_STATUS_SUCCESS;
}

__attribute__((always_inline)) inline __aicore__ bool UDMAWqeScratchValid(
    const AscendC::LocalTensor<uint8_t>& wqeScratch)
{
    const uint64_t scratchAddr = wqeScratch.GetPhyAddr();
    return (scratchAddr % TILEXR_UDMA_WQE_SCRATCH_ALIGNMENT) == 0U &&
        wqeScratch.GetSize() >= TILEXR_UDMA_WQE_SCRATCH_BYTES;
}

template <AscendC::HardEvent event>
__attribute__((always_inline)) inline __aicore__ void UDMASyncEvent()
{
    const auto eventId = GetTPipePtr()->FetchEventID(event);
    AscendC::SetFlag<event>(eventId);
    AscendC::WaitFlag<event>(eventId);
}

__attribute__((always_inline)) inline __aicore__ void UDMAClearWqeScratch(
    __ubuf__ uint8_t* wqeBytes, uint32_t byteCount)
{
    __ubuf__ uint32_t* words = reinterpret_cast<__ubuf__ uint32_t*>(wqeBytes);
    for (uint32_t word = 0U; word < byteCount / sizeof(uint32_t); ++word) {
        words[word] = 0U;
    }
}

__attribute__((always_inline)) inline __aicore__ void UDMAFillNotifyData(
    __ubuf__ uint8_t* wqeBytes, uint32_t tid, uint32_t tokenValue,
    const UDMASignalParams* params)
{
    __ubuf__ UDMANotifyCtx* notifyCtx = reinterpret_cast<__ubuf__ UDMANotifyCtx*>(
        wqeBytes + sizeof(UDMASqeCtx));
    const uint64_t signalAddr = reinterpret_cast<uint64_t>(params->sigAddr);
    notifyCtx->notifyTokenId = tid & 0xFFFFFU;
    notifyCtx->notifyTokenValue = tokenValue;
    notifyCtx->notifyAddrL = static_cast<uint32_t>(signalAddr);
    notifyCtx->notifyAddrH = static_cast<uint32_t>(signalAddr >> 32U);
    notifyCtx->notifyDataL = static_cast<uint32_t>(params->signal);
    notifyCtx->notifyDataH = static_cast<uint32_t>(params->signal >> 32U);
}

// BiSheng must not outline the posting chain: late device-call arguments can be corrupted.
template <UDMAOpcode opcode>
__attribute__((always_inline)) inline __aicore__ void UDMAFillSqeCtx(
    __ubuf__ UDMASqeCtx* sqeCtx, __gm__ uint8_t* remoteAddr,
    __gm__ UDMAMemInfo* remoteMemInfo, uint32_t curHead, uint32_t sqeFlag)
{
    sqeCtx->sqeBbIdx = curHead % TILEXR_UDMA_SQ_BB_COUNT;
    sqeCtx->opcode = static_cast<uint32_t>(opcode);
    sqeCtx->flag = sqeFlag;
    sqeCtx->rsv0 = 0;
    sqeCtx->nf = 0;
    sqeCtx->tokenEn = remoteMemInfo->tokenValueValid;
    sqeCtx->rmtJettyType = remoteMemInfo->rmtJettyType;
    sqeCtx->owner = ((curHead / TILEXR_UDMA_SQ_BB_COUNT) & 1U) == 0U ? 1U : 0U;
    sqeCtx->targetHint = remoteMemInfo->targetHint;
    sqeCtx->inlineMsgLen = 0;
    sqeCtx->rsv1 = 0;
    sqeCtx->tpId = remoteMemInfo->tpn;
    sqeCtx->sgeNum = 1;
    sqeCtx->rmtJettyOrSegId = remoteMemInfo->tid;
    sqeCtx->rsv2 = 0;
    sqeCtx->rmtTokenValue = remoteMemInfo->rmtTokenValue;
    sqeCtx->udfType = 0;
    sqeCtx->reduceDataType = 0;
    sqeCtx->reduceOpcode = 0;
    sqeCtx->rsv3 = 0;
    uint64_t remoteAddrValue = reinterpret_cast<uint64_t>(remoteAddr);
    sqeCtx->rmtAddrLOrTokenId = remoteAddrValue & 0xFFFFFFFF;
    sqeCtx->rmtAddrHOrTokenValue = (remoteAddrValue >> 32) & 0xFFFFFFFF;
    __gm__ uint64_t* rmtEid = reinterpret_cast<__gm__ uint64_t*>(remoteMemInfo->eidAddr);
    sqeCtx->rmtEidL = rmtEid[0];
    sqeCtx->rmtEidH = rmtEid[1];
}

__attribute__((always_inline)) inline __aicore__ void UDMAFillSgeCtx(
    __ubuf__ UDMASgeCtx* sgeCtx, uint64_t messageLen, __gm__ uint8_t* localAddr,
    uint32_t localTokenId)
{
    sgeCtx->len = messageLen;
    sgeCtx->tokenId = localTokenId;
    sgeCtx->va = reinterpret_cast<uint64_t>(localAddr);
}

__aicore__ inline void UDMARingDoorbell(uint32_t curHead, __gm__ UDMAWQCtx* qpCtxEntry)
{
    st_dev(curHead, reinterpret_cast<__gm__ uint32_t*>(qpCtxEntry->dbAddr), 0);
}

__attribute__((always_inline)) inline __aicore__ void UDMAMte3CopyToSq(
    const AscendC::LocalTensor<uint8_t>& wqeScratch, uint32_t scratchOffset,
    __gm__ uint8_t* sqDst, uint32_t byteCount)
{
    AscendC::GlobalTensor<uint8_t> sqGlobal;
    sqGlobal.SetGlobalBuffer(sqDst, byteCount);
    const AscendC::DataCopyExtParams copyParams {1U, byteCount, 0U, 0U, 0U};
    AscendC::DataCopyPad(sqGlobal, wqeScratch[scratchOffset], copyParams);
}

__attribute__((always_inline)) inline __aicore__ void UDMAPublishWqe(
    const AscendC::LocalTensor<uint8_t>& wqeScratch, __gm__ UDMAWQCtx* qpCtxEntry,
    uint32_t curHead, uint32_t wqeBbCnt)
{
    const uint32_t headIndex = curHead % qpCtxEntry->depth;
    const uint32_t firstBb = wqeBbCnt < qpCtxEntry->depth - headIndex ?
        wqeBbCnt : qpCtxEntry->depth - headIndex;
    const uint32_t firstBytes = firstBb * TILEXR_UDMA_WQE_BB_BYTES;
    __gm__ uint8_t* firstDst = reinterpret_cast<__gm__ uint8_t*>(
        qpCtxEntry->bufAddr + static_cast<uint64_t>(headIndex) * TILEXR_UDMA_WQE_BB_BYTES);

    UDMASyncEvent<AscendC::HardEvent::S_MTE3>();
    UDMAMte3CopyToSq(wqeScratch, 0U, firstDst, firstBytes);
    if (firstBb != wqeBbCnt) {
        const uint32_t wrappedBytes =
            (wqeBbCnt - firstBb) * TILEXR_UDMA_WQE_BB_BYTES;
        UDMAMte3CopyToSq(wqeScratch, firstBytes,
            reinterpret_cast<__gm__ uint8_t*>(qpCtxEntry->bufAddr), wrappedBytes);
    }
    UDMASyncEvent<AscendC::HardEvent::MTE3_S>();
}

template <UDMAOpcode opcode>
__attribute__((always_inline)) inline __aicore__ uint32_t UDMAValidatePostSend(
    __gm__ UDMAInfo* udmaInfo, __gm__ uint8_t* remoteAddr,
    __gm__ uint8_t* localAddr, uint32_t qpIdx, uint64_t messageLen,
    uint32_t sqeFlag, __gm__ UDMAWQCtx* qpCtxEntry)
{
    if (udmaInfo == nullptr || qpIdx >= udmaInfo->qpNum ||
        udmaInfo->sqPtr == 0U || udmaInfo->memPtr == 0U) {
        return TILEXR_UDMA_STATUS_INVALID;
    }
    if (remoteAddr == nullptr) {
        return TILEXR_UDMA_STATUS_INVALID;
    }
    if (localAddr == nullptr && messageLen != 0U) {
        return TILEXR_UDMA_STATUS_INVALID;
    }
    if (messageLen > 0xFFFFFFFFULL) {
        return TILEXR_UDMA_STATUS_INVALID;
    }
    if ((sqeFlag & TILEXR_UDMA_SQE_FLAG_COMPLETION) == 0U) {
        return TILEXR_UDMA_STATUS_INVALID;
    }
    if (opcode != UDMAOpcode::WRITE && opcode != UDMAOpcode::READ &&
        opcode != UDMAOpcode::WRITE_WITH_NOTIFY) {
        return TILEXR_UDMA_STATUS_INVALID;
    }
    if (qpCtxEntry == nullptr || qpCtxEntry->bufAddr == 0U ||
        qpCtxEntry->headAddr == 0U || qpCtxEntry->tailAddr == 0U ||
        qpCtxEntry->wqeCntAddr == 0U || qpCtxEntry->dbAddr == 0U ||
        qpCtxEntry->depth != TILEXR_UDMA_SQ_BB_COUNT ||
        qpCtxEntry->baseBkShift >= 32U) {
        return TILEXR_UDMA_STATUS_INVALID;
    }

    const uint32_t wqeSize = 1U << qpCtxEntry->baseBkShift;
    const uint32_t requiredBb = UDMAWqeBBCnt(opcode);
    const uint32_t requiredBytes = sizeof(UDMASqeCtx) + sizeof(UDMASgeCtx) +
        (opcode == UDMAOpcode::WRITE_WITH_NOTIFY ? sizeof(UDMANotifyCtx) : 0U);
    if (wqeSize != TILEXR_UDMA_WQE_BB_BYTES ||
        requiredBb > qpCtxEntry->depth ||
        static_cast<uint64_t>(wqeSize) * requiredBb < requiredBytes) {
        return TILEXR_UDMA_STATUS_INVALID;
    }

    const uint32_t head = ld_dev(reinterpret_cast<__gm__ uint32_t*>(qpCtxEntry->headAddr), 0);
    const uint32_t tail = ld_dev(reinterpret_cast<__gm__ uint32_t*>(qpCtxEntry->tailAddr), 0);
    const uint32_t outstanding = head - tail;
    if (outstanding > qpCtxEntry->depth) {
        return TILEXR_UDMA_STATUS_INVALID;
    }
    if (requiredBb > qpCtxEntry->depth - outstanding) {
        return TILEXR_UDMA_STATUS_SQ_FULL;
    }
    return TILEXR_UDMA_STATUS_SUCCESS;
}

template <UDMAOpcode opcode, bool ringDoorbell>
__attribute__((always_inline)) inline __aicore__ uint32_t UDMAPostSend(
    __gm__ UDMAInfo* udmaInfo, const AscendC::LocalTensor<uint8_t>& wqeScratch,
    __gm__ uint8_t* remoteAddr,
    __gm__ uint8_t* localAddr, uint32_t pe, uint32_t qpIdx, uint64_t messageLen,
    const UDMASignalParams* signalParams, uint32_t sqeFlag)
{
    if (udmaInfo == nullptr || udmaInfo->qpNum == 0U ||
        udmaInfo->qpNum > TILEXR_UDMA_DEVICE_MAX_QP_COUNT || qpIdx >= udmaInfo->qpNum ||
        udmaInfo->sqPtr == 0U || !UDMAWqeScratchValid(wqeScratch)) {
        return TILEXR_UDMA_STATUS_INVALID;
    }
    if (opcode == UDMAOpcode::WRITE_WITH_NOTIFY &&
        (signalParams == nullptr || signalParams->sigAddr == nullptr)) {
        return TILEXR_UDMA_STATUS_INVALID;
    }
    __gm__ UDMAWQCtx* qpCtxEntry = UDMAGetWQCtx(udmaInfo, pe, qpIdx);
    uint32_t validation = UDMAValidatePostSend<opcode>(
        udmaInfo, remoteAddr, localAddr, qpIdx, messageLen, sqeFlag, qpCtxEntry);
    if (validation == TILEXR_UDMA_STATUS_SQ_FULL && ringDoorbell) {
        const uint32_t submittedHead =
            ld_dev(reinterpret_cast<__gm__ uint32_t*>(qpCtxEntry->headAddr), 0);
        UDMARingDoorbell(submittedHead, qpCtxEntry);
        const uint32_t submittedWqeCount =
            ld_dev(reinterpret_cast<__gm__ uint32_t*>(qpCtxEntry->wqeCntAddr), 0);
        const uint32_t reclaimStatus = UDMAPollCQ(udmaInfo, pe, qpIdx, submittedWqeCount);
        if (reclaimStatus != TILEXR_UDMA_STATUS_SUCCESS) {
            return reclaimStatus;
        }
        validation = UDMAValidatePostSend<opcode>(
            udmaInfo, remoteAddr, localAddr, qpIdx, messageLen, sqeFlag, qpCtxEntry);
    }
    if (validation != TILEXR_UDMA_STATUS_SUCCESS) {
        return validation;
    }

    __gm__ UDMAMemInfo* remoteMemInfo = UDMAGetRemoteMemInfo(udmaInfo, pe, qpIdx);
    if (remoteMemInfo->eidAddr == 0U) {
        return TILEXR_UDMA_STATUS_INVALID;
    }
    uint32_t curHead = ld_dev(reinterpret_cast<__gm__ uint32_t*>(qpCtxEntry->headAddr), 0);
    uint32_t wqeCnt = ld_dev(reinterpret_cast<__gm__ uint32_t*>(qpCtxEntry->wqeCntAddr), 0);
    __ubuf__ uint8_t* wqeBytes = reinterpret_cast<__ubuf__ uint8_t*>(wqeScratch.GetPhyAddr());
    const uint32_t wqeBbCnt = UDMAWqeBBCnt(opcode);
    UDMAClearWqeScratch(wqeBytes, wqeBbCnt * TILEXR_UDMA_WQE_BB_BYTES);
    __ubuf__ UDMASqeCtx* sqeCtx = reinterpret_cast<__ubuf__ UDMASqeCtx*>(wqeBytes);
    UDMAFillSqeCtx<opcode>(sqeCtx, remoteAddr, remoteMemInfo, curHead, sqeFlag);

    if (opcode == UDMAOpcode::WRITE_WITH_NOTIFY) {
        UDMAFillNotifyData(wqeBytes, remoteMemInfo->tid,
            remoteMemInfo->rmtTokenValue, signalParams);
    }

    const uint32_t sgeOffset = sizeof(UDMASqeCtx) +
        (opcode == UDMAOpcode::WRITE_WITH_NOTIFY ? sizeof(UDMANotifyCtx) : 0U);
    __ubuf__ UDMASgeCtx* sgeCtx = reinterpret_cast<__ubuf__ UDMASgeCtx*>(
        wqeBytes + sgeOffset);
    UDMAFillSgeCtx(sgeCtx, messageLen, localAddr, qpCtxEntry->localTokenId);
    UDMAPublishWqe(wqeScratch, qpCtxEntry, curHead, wqeBbCnt);
    curHead += wqeBbCnt;
    ++wqeCnt;
    st_dev(curHead, reinterpret_cast<__gm__ uint32_t*>(qpCtxEntry->headAddr), 0);
    st_dev(wqeCnt, reinterpret_cast<__gm__ uint32_t*>(qpCtxEntry->wqeCntAddr), 0);
    if (ringDoorbell) {
        UDMARingDoorbell(curHead, qpCtxEntry);
    }
    return TILEXR_UDMA_STATUS_SUCCESS;
}

// Preserve the historical grouped-AllToAll posting path: construct the WRITE
// directly in SQ GM, clean it, then publish doorbell before the software head.
__attribute__((always_inline)) inline __aicore__ uint32_t UDMAPostWriteDirect(
    __gm__ UDMAInfo* udmaInfo, __gm__ uint8_t* remoteAddr,
    __gm__ uint8_t* localAddr, uint32_t pe, uint32_t qpIdx,
    uint64_t messageLen, uint32_t sqeFlag)
{
    if (udmaInfo == nullptr || udmaInfo->qpNum == 0U ||
        udmaInfo->qpNum > TILEXR_UDMA_DEVICE_MAX_QP_COUNT || qpIdx >= udmaInfo->qpNum ||
        udmaInfo->sqPtr == 0U) {
        return TILEXR_UDMA_STATUS_INVALID;
    }
    __gm__ UDMAWQCtx* qpCtxEntry = UDMAGetWQCtx(udmaInfo, pe, qpIdx);
    uint32_t validation = UDMAValidatePostSend<UDMAOpcode::WRITE>(
        udmaInfo, remoteAddr, localAddr, qpIdx, messageLen, sqeFlag, qpCtxEntry);
    if (validation == TILEXR_UDMA_STATUS_SQ_FULL) {
        const uint32_t submittedWqeCount =
            ld_dev(reinterpret_cast<__gm__ uint32_t*>(qpCtxEntry->wqeCntAddr), 0);
        const uint32_t reclaimStatus = UDMAPollCQ(
            udmaInfo, pe, qpIdx, submittedWqeCount);
        if (reclaimStatus != TILEXR_UDMA_STATUS_SUCCESS) {
            return reclaimStatus;
        }
        validation = UDMAValidatePostSend<UDMAOpcode::WRITE>(
            udmaInfo, remoteAddr, localAddr, qpIdx, messageLen, sqeFlag, qpCtxEntry);
    }
    if (validation != TILEXR_UDMA_STATUS_SUCCESS) {
        return validation;
    }

    __gm__ UDMAMemInfo* remoteMemInfo = UDMAGetRemoteMemInfo(udmaInfo, pe, qpIdx);
    if (remoteMemInfo->eidAddr == 0U) {
        return TILEXR_UDMA_STATUS_INVALID;
    }
    uint32_t curHead = ld_dev(
        reinterpret_cast<__gm__ uint32_t*>(qpCtxEntry->headAddr), 0);
    uint32_t wqeCnt = ld_dev(
        reinterpret_cast<__gm__ uint32_t*>(qpCtxEntry->wqeCntAddr), 0);
    __gm__ uint8_t* wqeAddr = reinterpret_cast<__gm__ uint8_t*>(
        qpCtxEntry->bufAddr + static_cast<uint64_t>(curHead % qpCtxEntry->depth) *
            TILEXR_UDMA_WQE_BB_BYTES);
    __gm__ UDMASqeCtx* sqeCtx = reinterpret_cast<__gm__ UDMASqeCtx*>(wqeAddr);
    sqeCtx->sqeBbIdx = curHead % TILEXR_UDMA_SQ_BB_COUNT;
    sqeCtx->opcode = static_cast<uint32_t>(UDMAOpcode::WRITE);
    sqeCtx->flag = sqeFlag;
    sqeCtx->rsv0 = 0U;
    sqeCtx->nf = 0U;
    sqeCtx->tokenEn = remoteMemInfo->tokenValueValid;
    sqeCtx->rmtJettyType = remoteMemInfo->rmtJettyType;
    sqeCtx->owner = ((curHead / TILEXR_UDMA_SQ_BB_COUNT) & 1U) == 0U ? 1U : 0U;
    sqeCtx->targetHint = remoteMemInfo->targetHint;
    sqeCtx->inlineMsgLen = 0U;
    sqeCtx->rsv1 = 0U;
    sqeCtx->tpId = remoteMemInfo->tpn;
    sqeCtx->sgeNum = 1U;
    sqeCtx->rmtJettyOrSegId = remoteMemInfo->tid;
    sqeCtx->rsv2 = 0U;
    sqeCtx->rmtTokenValue = remoteMemInfo->rmtTokenValue;
    sqeCtx->udfType = 0U;
    sqeCtx->reduceDataType = 0U;
    sqeCtx->reduceOpcode = 0U;
    sqeCtx->rsv3 = 0U;
    const uint64_t remoteAddrValue = reinterpret_cast<uint64_t>(remoteAddr);
    sqeCtx->rmtAddrLOrTokenId = static_cast<uint32_t>(remoteAddrValue);
    sqeCtx->rmtAddrHOrTokenValue = static_cast<uint32_t>(remoteAddrValue >> 32U);
    __gm__ uint64_t* remoteEid =
        reinterpret_cast<__gm__ uint64_t*>(remoteMemInfo->eidAddr);
    sqeCtx->rmtEidL = remoteEid[0];
    sqeCtx->rmtEidH = remoteEid[1];

    __gm__ UDMASgeCtx* sgeCtx = reinterpret_cast<__gm__ UDMASgeCtx*>(
        wqeAddr + sizeof(UDMASqeCtx));
    sgeCtx->len = messageLen;
    sgeCtx->tokenId = qpCtxEntry->localTokenId;
    sgeCtx->va = reinterpret_cast<uint64_t>(localAddr);
    UDMACleanCacheLines(wqeAddr, TILEXR_UDMA_WQE_BB_BYTES);

    ++curHead;
    ++wqeCnt;
    UDMARingDoorbell(curHead, qpCtxEntry);
    st_dev(curHead, reinterpret_cast<__gm__ uint32_t*>(qpCtxEntry->headAddr), 0);
    st_dev(wqeCnt, reinterpret_cast<__gm__ uint32_t*>(qpCtxEntry->wqeCntAddr), 0);
    return TILEXR_UDMA_STATUS_SUCCESS;
}

__attribute__((always_inline)) inline __aicore__ uint32_t UDMAWrite(
    const __gm__ CommArgs* args, const AscendC::LocalTensor<uint8_t>& wqeScratch,
    __gm__ uint8_t* remoteAddr, __gm__ uint8_t* localAddr,
    uint32_t pe, uint32_t qpIdx, uint64_t messageLen,
    uint32_t sqeFlag = TILEXR_UDMA_SQE_FLAG_ORDERED_COMPLETION, bool ringDoorbell = true)
{
    if (!TILEXR_UDMA_ARCH_SUPPORTED) {
        return TILEXR_UDMA_STATUS_INVALID;
    }
    if (!UDMAQueueOperationValid(args, static_cast<int>(pe), qpIdx)) {
        return TILEXR_UDMA_STATUS_INVALID;
    }
    if (ringDoorbell) {
        return UDMAPostSend<UDMAOpcode::WRITE, true>(GetUDMAInfo(args), wqeScratch, remoteAddr,
            localAddr, pe, qpIdx, messageLen, nullptr, sqeFlag);
    }
    return UDMAPostSend<UDMAOpcode::WRITE, false>(GetUDMAInfo(args), wqeScratch, remoteAddr,
        localAddr, pe, qpIdx, messageLen, nullptr, sqeFlag);
}

__attribute__((always_inline)) inline __aicore__ uint32_t UDMARead(
    const __gm__ CommArgs* args, const AscendC::LocalTensor<uint8_t>& wqeScratch,
    __gm__ uint8_t* localAddr, __gm__ uint8_t* remoteAddr,
    uint32_t pe, uint32_t qpIdx, uint64_t messageLen, bool ringDoorbell = true)
{
    if (TILEXR_UDMA_ARCH_SUPPORTED && UDMAQueueOperationValid(
            args, static_cast<int>(pe), qpIdx)) {
        if (ringDoorbell) {
            return UDMAPostSend<UDMAOpcode::READ, true>(GetUDMAInfo(args), wqeScratch,
                remoteAddr, localAddr, pe, qpIdx, messageLen, nullptr,
                TILEXR_UDMA_SQE_FLAG_ORDERED_COMPLETION);
        }
        return UDMAPostSend<UDMAOpcode::READ, false>(GetUDMAInfo(args), wqeScratch,
            remoteAddr, localAddr, pe, qpIdx, messageLen, nullptr,
            TILEXR_UDMA_SQE_FLAG_ORDERED_COMPLETION);
    }
    return TILEXR_UDMA_STATUS_INVALID;
}

__aicore__ inline uint32_t UDMAWriteNotify(
    const __gm__ CommArgs* args, const AscendC::LocalTensor<uint8_t>& wqeScratch,
    __gm__ uint8_t* remoteAddr, __gm__ uint8_t* localAddr,
    uint32_t pe, uint32_t qpIdx, uint64_t messageLen, const UDMASignalParams* signalParams)
{
    if (TILEXR_UDMA_ARCH_SUPPORTED && UDMAQueueOperationValid(
            args, static_cast<int>(pe), qpIdx)) {
        if (signalParams == nullptr || signalParams->sigAddr == nullptr) {
            return TILEXR_UDMA_STATUS_INVALID;
        }
        return UDMAPostSend<UDMAOpcode::WRITE_WITH_NOTIFY, true>(GetUDMAInfo(args),
            wqeScratch, remoteAddr, localAddr, pe, qpIdx, messageLen, signalParams,
            TILEXR_UDMA_SQE_FLAG_ORDERED_COMPLETION);
    }
    return TILEXR_UDMA_STATUS_INVALID;
}

__attribute__((always_inline)) inline __aicore__ bool UDMARegisteredOperationValid(
    const __gm__ CommArgs* args, int peer, uint32_t qpIdx, uint64_t byteOffset, uint64_t byteCount)
{
    if (!UDMARegistryEnabled(args)) {
        return false;
    }
    if (!UDMARankValid(args, peer)) {
        return false;
    }
    if (!UDMAQpValid(args, qpIdx)) {
        return false;
    }
    const __gm__ TileXRUDMARegistry* registry = GetUDMARegistry(args);
    if (registry->rankSize != static_cast<uint32_t>(args->rankSize)) {
        return false;
    }
    return UDMARegisteredRangeValid(registry, peer, byteOffset, byteCount);
}

template <typename T>
__attribute__((always_inline)) inline __aicore__ uint32_t UDMAPutNbiOnQpWithFlagDeferred(
    const __gm__ CommArgs* args, const AscendC::LocalTensor<uint8_t>& wqeScratch,
    int targetRank, uint32_t qpIdx,
    const __gm__ T* localSrc, uint64_t byteOffset, uint32_t byteCount, uint32_t sqeFlag)
{
    if (!UDMARegisteredOperationValid(args, targetRank, qpIdx, byteOffset, byteCount) ||
        (sqeFlag & TILEXR_UDMA_SQE_FLAG_COMPLETION) == 0U) {
        return TILEXR_UDMA_STATUS_INVALID;
    }

    auto registry = GetUDMARegistry(args);
    auto localAddr = reinterpret_cast<__gm__ uint8_t*>(const_cast<__gm__ T*>(localSrc));
    auto remoteAddr = UDMARegisteredRemoteAddr(registry, targetRank, byteOffset);
    return UDMAWrite(args, wqeScratch, remoteAddr, localAddr,
        static_cast<uint32_t>(targetRank), qpIdx, byteCount, sqeFlag, false);
}

template <typename T>
__attribute__((always_inline)) inline __aicore__ uint32_t UDMAPutNbiOnQpWithFlag(
    const __gm__ CommArgs* args, const AscendC::LocalTensor<uint8_t>& wqeScratch,
    int targetRank, uint32_t qpIdx,
    const __gm__ T* localSrc, uint64_t byteOffset, uint32_t byteCount, uint32_t sqeFlag)
{
    if (!UDMARegisteredOperationValid(args, targetRank, qpIdx, byteOffset, byteCount) ||
        (sqeFlag & TILEXR_UDMA_SQE_FLAG_COMPLETION) == 0U) {
        return TILEXR_UDMA_STATUS_INVALID;
    }

    auto registry = GetUDMARegistry(args);
    auto localAddr = reinterpret_cast<__gm__ uint8_t*>(const_cast<__gm__ T*>(localSrc));
    auto remoteAddr = UDMARegisteredRemoteAddr(registry, targetRank, byteOffset);
    return UDMAWrite(args, wqeScratch, remoteAddr, localAddr,
        static_cast<uint32_t>(targetRank), qpIdx, byteCount, sqeFlag, true);
}

template <typename T>
__aicore__ inline uint32_t UDMAPutNbiOnQp(
    const __gm__ CommArgs* args, const AscendC::LocalTensor<uint8_t>& wqeScratch,
    int targetRank, uint32_t qpIdx,
    const __gm__ T* localSrc, uint64_t byteOffset, uint32_t byteCount)
{
    return UDMAPutNbiOnQpWithFlag<T>(args, wqeScratch, targetRank, qpIdx, localSrc,
        byteOffset, byteCount, TILEXR_UDMA_SQE_FLAG_ORDERED_COMPLETION);
}

template <typename T>
__aicore__ inline void UDMAPutNbi(
    const __gm__ CommArgs* args, const AscendC::LocalTensor<uint8_t>& wqeScratch,
    int targetRank, const __gm__ T* localSrc,
    uint64_t byteOffset, uint32_t byteCount)
{
    (void)UDMAPutNbiOnQp<T>(args, wqeScratch, targetRank, 0U, localSrc, byteOffset, byteCount);
}

template <typename T>
__aicore__ inline void UDMAPutRegisteredNbi(
    const __gm__ CommArgs* args, const AscendC::LocalTensor<uint8_t>& wqeScratch,
    int targetRank, const __gm__ T* localSrc, uint64_t byteOffset, uint32_t byteCount)
{
    UDMAPutNbi<T>(args, wqeScratch, targetRank, localSrc, byteOffset, byteCount);
}

template <typename T>
__aicore__ inline uint32_t UDMAGetNbiOnQp(
    const __gm__ CommArgs* args, const AscendC::LocalTensor<uint8_t>& wqeScratch,
    int sourceRank, uint32_t qpIdx,
    __gm__ T* localDst, uint64_t byteOffset, uint32_t byteCount)
{
    if (!UDMARegisteredOperationValid(args, sourceRank, qpIdx, byteOffset, byteCount) ||
        (localDst == nullptr && byteCount != 0U)) {
        return TILEXR_UDMA_STATUS_INVALID;
    }

    auto registry = GetUDMARegistry(args);
    auto localAddr = reinterpret_cast<__gm__ uint8_t*>(localDst);
    auto remoteAddr = UDMARegisteredRemoteAddr(registry, sourceRank, byteOffset);
    return UDMARead(args, wqeScratch, localAddr, remoteAddr,
        static_cast<uint32_t>(sourceRank), qpIdx, byteCount);
}

template <typename T>
__aicore__ inline void UDMAGetNbi(
    const __gm__ CommArgs* args, const AscendC::LocalTensor<uint8_t>& wqeScratch,
    int sourceRank, __gm__ T* localDst,
    uint64_t byteOffset, uint32_t byteCount)
{
    (void)UDMAGetNbiOnQp<T>(args, wqeScratch, sourceRank, 0U, localDst, byteOffset, byteCount);
}

template <typename T>
__aicore__ inline void UDMAGetRegisteredNbi(
    const __gm__ CommArgs* args, const AscendC::LocalTensor<uint8_t>& wqeScratch,
    int sourceRank, __gm__ T* localDst, uint64_t byteOffset, uint32_t byteCount)
{
    UDMAGetNbi<T>(args, wqeScratch, sourceRank, localDst, byteOffset, byteCount);
}

__attribute__((always_inline)) inline __aicore__ bool UDMAProfileGetOperationValid(
    const __gm__ CommArgs* args, const __gm__ UDMAInfo* udmaInfo,
    const __gm__ TileXRUDMAProfileRegistry* registry,
    int sourceRank, uint32_t qpIdx, uint32_t localRegion, uint64_t localByteOffset,
    uint32_t remoteRegion, uint64_t remoteByteOffset, uint64_t byteCount)
{
    if (!TILEXR_UDMA_ARCH_SUPPORTED ||
        !UDMAProfileRegistryValid(args, udmaInfo, registry) ||
        sourceRank < 0 || sourceRank >= args->rankSize || sourceRank == args->rank ||
        qpIdx >= registry->qpCount || byteCount == 0U || byteCount > 0xFFFFFFFFULL) {
        return false;
    }
    const __gm__ TileXRUDMAProfileQpBinding* binding = &registry->qpBindings[qpIdx];
    if (binding->localRegion != localRegion || binding->remoteRegion != remoteRegion) {
        return false;
    }
    return UDMAProfileRegisteredRangeValid(registry, args->rank, localRegion,
               localByteOffset, byteCount) &&
        UDMAProfileRegisteredRangeValid(registry, sourceRank, remoteRegion,
            remoteByteOffset, byteCount);
}

__attribute__((always_inline)) inline __aicore__ uint32_t UDMAProfileGetNbiOnQpDeferred(
    const __gm__ CommArgs* args, __gm__ UDMAInfo* udmaInfo,
    const __gm__ TileXRUDMAProfileRegistry* registry,
    const AscendC::LocalTensor<uint8_t>& wqeScratch,
    int sourceRank, uint32_t qpIdx, uint32_t localRegion, uint64_t localByteOffset,
    uint32_t remoteRegion, uint64_t remoteByteOffset, uint64_t byteCount)
{
    if (!UDMAProfileGetOperationValid(args, udmaInfo, registry, sourceRank, qpIdx,
            localRegion, localByteOffset, remoteRegion, remoteByteOffset, byteCount)) {
        return TILEXR_UDMA_STATUS_INVALID;
    }
    __gm__ uint8_t* localAddr = UDMAProfileRegisteredAddr(
        registry, args->rank, localRegion, localByteOffset);
    __gm__ uint8_t* remoteAddr = UDMAProfileRegisteredAddr(
        registry, sourceRank, remoteRegion, remoteByteOffset);
    return UDMAPostSend<UDMAOpcode::READ, false>(udmaInfo, wqeScratch,
        remoteAddr, localAddr, static_cast<uint32_t>(sourceRank), qpIdx, byteCount,
        nullptr, TILEXR_UDMA_SQE_FLAG_ORDERED_COMPLETION);
}

__attribute__((always_inline)) inline __aicore__ bool UDMAProfilePutOperationValid(
    const __gm__ CommArgs* args, const __gm__ UDMAInfo* udmaInfo,
    const __gm__ TileXRUDMAProfileRegistry* registry,
    int targetRank, uint32_t qpIdx, uint32_t localRegion, uint64_t localByteOffset,
    uint32_t remoteRegion, uint64_t remoteByteOffset, uint64_t byteCount)
{
    if (!TILEXR_UDMA_ARCH_SUPPORTED ||
        !UDMAProfileRegistryValid(args, udmaInfo, registry) ||
        targetRank < 0 || targetRank >= args->rankSize || targetRank == args->rank ||
        qpIdx >= registry->qpCount || byteCount == 0U || byteCount > 0xFFFFFFFFULL) {
        return false;
    }
    const __gm__ TileXRUDMAProfileQpBinding* binding = &registry->qpBindings[qpIdx];
    if (binding->localRegion != localRegion || binding->remoteRegion != remoteRegion) {
        return false;
    }
    return UDMAProfileRegisteredRangeValid(registry, args->rank, localRegion,
               localByteOffset, byteCount) &&
        UDMAProfileRegisteredRangeValid(registry, targetRank, remoteRegion,
            remoteByteOffset, byteCount);
}

__attribute__((always_inline)) inline __aicore__ uint32_t
UDMAProfilePutNbiOnQpWithFlagDeferred(
    const __gm__ CommArgs* args, __gm__ UDMAInfo* udmaInfo,
    const __gm__ TileXRUDMAProfileRegistry* registry,
    const AscendC::LocalTensor<uint8_t>& wqeScratch,
    int targetRank, uint32_t qpIdx, uint32_t localRegion, uint64_t localByteOffset,
    uint32_t remoteRegion, uint64_t remoteByteOffset, uint64_t byteCount,
    uint32_t sqeFlag)
{
    if (!UDMAProfilePutOperationValid(args, udmaInfo, registry, targetRank, qpIdx,
            localRegion, localByteOffset, remoteRegion, remoteByteOffset, byteCount) ||
        (sqeFlag & TILEXR_UDMA_SQE_FLAG_COMPLETION) == 0U) {
        return TILEXR_UDMA_STATUS_INVALID;
    }
    __gm__ uint8_t* localAddr = UDMAProfileRegisteredAddr(
        registry, args->rank, localRegion, localByteOffset);
    __gm__ uint8_t* remoteAddr = UDMAProfileRegisteredAddr(
        registry, targetRank, remoteRegion, remoteByteOffset);
    return UDMAPostSend<UDMAOpcode::WRITE, false>(udmaInfo, wqeScratch,
        remoteAddr, localAddr, static_cast<uint32_t>(targetRank), qpIdx, byteCount,
        nullptr, sqeFlag);
}

__attribute__((always_inline)) inline __aicore__ uint32_t
UDMAProfilePutNbiOnQpWithFlag(
    const __gm__ CommArgs* args, __gm__ UDMAInfo* udmaInfo,
    const __gm__ TileXRUDMAProfileRegistry* registry,
    const AscendC::LocalTensor<uint8_t>& wqeScratch,
    int targetRank, uint32_t qpIdx, uint32_t localRegion, uint64_t localByteOffset,
    uint32_t remoteRegion, uint64_t remoteByteOffset, uint64_t byteCount,
    uint32_t sqeFlag)
{
    (void)wqeScratch;
    if (!UDMAProfilePutOperationValid(args, udmaInfo, registry, targetRank, qpIdx,
            localRegion, localByteOffset, remoteRegion, remoteByteOffset, byteCount) ||
        (sqeFlag & TILEXR_UDMA_SQE_FLAG_COMPLETION) == 0U) {
        return TILEXR_UDMA_STATUS_INVALID;
    }
    __gm__ uint8_t* localAddr = UDMAProfileRegisteredAddr(
        registry, args->rank, localRegion, localByteOffset);
    __gm__ uint8_t* remoteAddr = UDMAProfileRegisteredAddr(
        registry, targetRank, remoteRegion, remoteByteOffset);
    return UDMAPostWriteDirect(udmaInfo,
        remoteAddr, localAddr, static_cast<uint32_t>(targetRank), qpIdx, byteCount,
        sqeFlag);
}

__attribute__((always_inline)) inline __aicore__ uint32_t UDMAProfilePutNbiOnQpDeferred(
    const __gm__ CommArgs* args, __gm__ UDMAInfo* udmaInfo,
    const __gm__ TileXRUDMAProfileRegistry* registry,
    const AscendC::LocalTensor<uint8_t>& wqeScratch,
    int targetRank, uint32_t qpIdx, uint32_t localRegion, uint64_t localByteOffset,
    uint32_t remoteRegion, uint64_t remoteByteOffset, uint64_t byteCount)
{
    return UDMAProfilePutNbiOnQpWithFlagDeferred(
        args, udmaInfo, registry, wqeScratch, targetRank, qpIdx,
        localRegion, localByteOffset, remoteRegion, remoteByteOffset, byteCount,
        TILEXR_UDMA_SQE_FLAG_ORDERED_COMPLETION);
}

// The selected QP contributes the local MR token while the payload address is
// caller-owned GM. This is the legacy grouped-AllToAll registration contract.
__attribute__((always_inline)) inline __aicore__ uint32_t
UDMAProfilePutRawLocalNbiOnQpDeferred(
    const __gm__ CommArgs* args, __gm__ UDMAInfo* udmaInfo,
    const __gm__ TileXRUDMAProfileRegistry* registry,
    const AscendC::LocalTensor<uint8_t>& wqeScratch,
    int targetRank, uint32_t qpIdx, const __gm__ uint8_t* localAddr,
    uint32_t localTokenRegion, uint32_t remoteRegion,
    uint64_t remoteByteOffset, uint64_t byteCount, uint32_t sqeFlag)
{
    if (!TILEXR_UDMA_ARCH_SUPPORTED ||
        !UDMAProfileRegistryValid(args, udmaInfo, registry) ||
        targetRank < 0 || targetRank >= args->rankSize || targetRank == args->rank ||
        qpIdx >= registry->qpCount || localAddr == nullptr || byteCount == 0U ||
        byteCount > 0xFFFFFFFFULL ||
        (sqeFlag & TILEXR_UDMA_SQE_FLAG_COMPLETION) == 0U) {
        return TILEXR_UDMA_STATUS_INVALID;
    }
    const __gm__ TileXRUDMAProfileQpBinding* binding = &registry->qpBindings[qpIdx];
    if (binding->localRegion != localTokenRegion ||
        binding->remoteRegion != remoteRegion ||
        !UDMAProfileRegisteredRangeValid(registry, targetRank, remoteRegion,
            remoteByteOffset, byteCount)) {
        return TILEXR_UDMA_STATUS_INVALID;
    }
    __gm__ uint8_t* remoteAddr = UDMAProfileRegisteredAddr(
        registry, targetRank, remoteRegion, remoteByteOffset);
    return UDMAPostSend<UDMAOpcode::WRITE, false>(udmaInfo, wqeScratch,
        remoteAddr, const_cast<__gm__ uint8_t*>(localAddr),
        static_cast<uint32_t>(targetRank), qpIdx, byteCount, nullptr, sqeFlag);
}

__attribute__((always_inline)) inline __aicore__ uint32_t
UDMAProfilePutRawLocalNbiOnQp(
    const __gm__ CommArgs* args, __gm__ UDMAInfo* udmaInfo,
    const __gm__ TileXRUDMAProfileRegistry* registry,
    const AscendC::LocalTensor<uint8_t>& wqeScratch,
    int targetRank, uint32_t qpIdx, const __gm__ uint8_t* localAddr,
    uint32_t localTokenRegion, uint32_t remoteRegion,
    uint64_t remoteByteOffset, uint64_t byteCount, uint32_t sqeFlag)
{
    (void)wqeScratch;
    if (!TILEXR_UDMA_ARCH_SUPPORTED ||
        !UDMAProfileRegistryValid(args, udmaInfo, registry) ||
        targetRank < 0 || targetRank >= args->rankSize || targetRank == args->rank ||
        qpIdx >= registry->qpCount || localAddr == nullptr || byteCount == 0U ||
        byteCount > 0xFFFFFFFFULL ||
        (sqeFlag & TILEXR_UDMA_SQE_FLAG_COMPLETION) == 0U) {
        return TILEXR_UDMA_STATUS_INVALID;
    }
    const __gm__ TileXRUDMAProfileQpBinding* binding = &registry->qpBindings[qpIdx];
    if (binding->localRegion != localTokenRegion ||
        binding->remoteRegion != remoteRegion ||
        !UDMAProfileRegisteredRangeValid(registry, targetRank, remoteRegion,
            remoteByteOffset, byteCount)) {
        return TILEXR_UDMA_STATUS_INVALID;
    }
    __gm__ uint8_t* remoteAddr = UDMAProfileRegisteredAddr(
        registry, targetRank, remoteRegion, remoteByteOffset);
    return UDMAPostWriteDirect(udmaInfo,
        remoteAddr, const_cast<__gm__ uint8_t*>(localAddr),
        static_cast<uint32_t>(targetRank), qpIdx, byteCount, sqeFlag);
}

__aicore__ inline uint32_t UDMAProfileCompletionFrontier(
    const __gm__ CommArgs* args, __gm__ UDMAInfo* udmaInfo,
    const __gm__ TileXRUDMAProfileRegistry* registry, int sourceRank, uint32_t qpIdx)
{
    if (!UDMAProfileRegistryValid(args, udmaInfo, registry) || sourceRank < 0 ||
        sourceRank >= args->rankSize || sourceRank == args->rank || qpIdx >= registry->qpCount) {
        return 0U;
    }
    __gm__ UDMAWQCtx* qpCtxEntry = UDMAGetWQCtx(
        udmaInfo, static_cast<uint32_t>(sourceRank), qpIdx);
    if (qpCtxEntry == nullptr || qpCtxEntry->headAddr == 0U) {
        return 0U;
    }
    return ld_dev(reinterpret_cast<__gm__ uint32_t*>(qpCtxEntry->headAddr), 0);
}

__aicore__ inline uint32_t UDMAProfileFlushQpDoorbell(
    const __gm__ CommArgs* args, __gm__ UDMAInfo* udmaInfo,
    const __gm__ TileXRUDMAProfileRegistry* registry, int sourceRank, uint32_t qpIdx)
{
    if (!UDMAProfileRegistryValid(args, udmaInfo, registry) || sourceRank < 0 ||
        sourceRank >= args->rankSize || sourceRank == args->rank || qpIdx >= registry->qpCount) {
        return TILEXR_UDMA_STATUS_INVALID;
    }
    __gm__ UDMAWQCtx* qpCtxEntry = UDMAGetWQCtx(
        udmaInfo, static_cast<uint32_t>(sourceRank), qpIdx);
    if (qpCtxEntry == nullptr || qpCtxEntry->headAddr == 0U ||
        qpCtxEntry->tailAddr == 0U || qpCtxEntry->dbAddr == 0U ||
        qpCtxEntry->depth != TILEXR_UDMA_SQ_BB_COUNT) {
        return TILEXR_UDMA_STATUS_INVALID;
    }
    const uint32_t head = ld_dev(reinterpret_cast<__gm__ uint32_t*>(qpCtxEntry->headAddr), 0);
    const uint32_t tail = ld_dev(reinterpret_cast<__gm__ uint32_t*>(qpCtxEntry->tailAddr), 0);
    if (head - tail > qpCtxEntry->depth) {
        return TILEXR_UDMA_STATUS_INVALID;
    }
    UDMARingDoorbell(head, qpCtxEntry);
    return TILEXR_UDMA_STATUS_SUCCESS;
}

__aicore__ inline uint32_t UDMAProfileQuietStatusOnQpUntil(
    const __gm__ CommArgs* args, __gm__ UDMAInfo* udmaInfo,
    const __gm__ TileXRUDMAProfileRegistry* registry,
    int sourceRank, uint32_t qpIdx, uint32_t completionFrontier)
{
    if (!UDMAProfileRegistryValid(args, udmaInfo, registry) || sourceRank < 0 ||
        sourceRank >= args->rankSize || sourceRank == args->rank || qpIdx >= registry->qpCount ||
        udmaInfo->scqPtr == 0U) {
        return TILEXR_UDMA_STATUS_INVALID;
    }
    __gm__ UDMAWQCtx* qpCtxEntry = UDMAGetWQCtx(
        udmaInfo, static_cast<uint32_t>(sourceRank), qpIdx);
    if (qpCtxEntry == nullptr || qpCtxEntry->headAddr == 0U ||
        qpCtxEntry->tailAddr == 0U || qpCtxEntry->depth != TILEXR_UDMA_SQ_BB_COUNT) {
        return TILEXR_UDMA_STATUS_INVALID;
    }
    const uint32_t submittedHead = ld_dev(
        reinterpret_cast<__gm__ uint32_t*>(qpCtxEntry->headAddr), 0);
    const uint32_t submittedTail = ld_dev(
        reinterpret_cast<__gm__ uint32_t*>(qpCtxEntry->tailAddr), 0);
    if (submittedHead - submittedTail > qpCtxEntry->depth ||
        completionFrontier - submittedTail > qpCtxEntry->depth ||
        submittedHead - completionFrontier > qpCtxEntry->depth) {
        return TILEXR_UDMA_STATUS_INVALID;
    }
    return UDMAPollCQUntilSqTail(udmaInfo, static_cast<uint32_t>(sourceRank), qpIdx,
        completionFrontier);
}

__aicore__ inline uint32_t UDMAProfileQuietStatusOnQp(
    const __gm__ CommArgs* args, __gm__ UDMAInfo* udmaInfo,
    const __gm__ TileXRUDMAProfileRegistry* registry,
    int sourceRank, uint32_t qpIdx)
{
    if (!UDMAProfileRegistryValid(args, udmaInfo, registry) || sourceRank < 0 ||
        sourceRank >= args->rankSize || sourceRank == args->rank || qpIdx >= registry->qpCount ||
        udmaInfo->scqPtr == 0U) {
        return TILEXR_UDMA_STATUS_INVALID;
    }
    __gm__ UDMAWQCtx* qpCtxEntry = UDMAGetWQCtx(
        udmaInfo, static_cast<uint32_t>(sourceRank), qpIdx);
    if (qpCtxEntry == nullptr || qpCtxEntry->wqeCntAddr == 0U) {
        return TILEXR_UDMA_STATUS_INVALID;
    }
    const uint32_t completionTarget = ld_dev(
        reinterpret_cast<__gm__ uint32_t*>(qpCtxEntry->wqeCntAddr), 0);
    return UDMAPollCQ(
        udmaInfo, static_cast<uint32_t>(sourceRank), qpIdx, completionTarget);
}

template <typename T>
__aicore__ inline uint32_t UDMAPutSignalNbiOnQp(
    const __gm__ CommArgs* args, const AscendC::LocalTensor<uint8_t>& wqeScratch,
    int targetRank, uint32_t qpIdx, const __gm__ T* localSrc, uint64_t byteOffset,
    uint32_t byteCount, uint64_t signalByteOffset, uint64_t signal)
{
    if (!UDMARegisteredOperationValid(args, targetRank, qpIdx, byteOffset, byteCount) ||
        !UDMARegisteredOperationValid(args, targetRank, qpIdx, signalByteOffset, sizeof(uint64_t)) ||
        (localSrc == nullptr && byteCount != 0U)) {
        return TILEXR_UDMA_STATUS_INVALID;
    }

    auto registry = GetUDMARegistry(args);
    auto localAddr = reinterpret_cast<__gm__ uint8_t*>(const_cast<__gm__ T*>(localSrc));
    UDMASignalParams signalParams = {};
    signalParams.sigAddr = reinterpret_cast<__gm__ uint64_t*>(
        UDMARegisteredRemoteAddr(registry, targetRank, signalByteOffset));
    signalParams.signal = signal;
    auto remoteAddr = UDMARegisteredRemoteAddr(registry, targetRank, byteOffset);
    return UDMAWriteNotify(args, wqeScratch, remoteAddr, localAddr,
        static_cast<uint32_t>(targetRank), qpIdx, byteCount, &signalParams);
}

template <typename T>
__aicore__ inline void UDMAPutSignalNbi(
    const __gm__ CommArgs* args, const AscendC::LocalTensor<uint8_t>& wqeScratch,
    int targetRank, const __gm__ T* localSrc, uint64_t byteOffset,
    uint32_t byteCount, uint64_t signalByteOffset, uint64_t signal)
{
    (void)UDMAPutSignalNbiOnQp<T>(args, wqeScratch, targetRank, 0U, localSrc,
        byteOffset, byteCount, signalByteOffset, signal);
}

template <typename T>
__aicore__ inline uint32_t UDMAPutRegisteredSignalNbiOnQp(
    const __gm__ CommArgs* args, const AscendC::LocalTensor<uint8_t>& wqeScratch,
    int targetRank, uint32_t qpIdx, const __gm__ T* localSrc, uint64_t byteOffset,
    uint32_t byteCount, uint64_t signalByteOffset, uint64_t signal)
{
    return UDMAPutSignalNbiOnQp<T>(args, wqeScratch, targetRank, qpIdx, localSrc,
        byteOffset, byteCount, signalByteOffset, signal);
}

template <typename T>
__aicore__ inline void UDMAPutRegisteredSignalNbi(
    const __gm__ CommArgs* args, const AscendC::LocalTensor<uint8_t>& wqeScratch,
    int targetRank, const __gm__ T* localSrc, uint64_t byteOffset,
    uint32_t byteCount, uint64_t signalByteOffset, uint64_t signal)
{
    (void)UDMAPutRegisteredSignalNbiOnQp<T>(args, wqeScratch, targetRank, 0U,
        localSrc, byteOffset, byteCount, signalByteOffset, signal);
}

__aicore__ inline uint32_t UDMAFlushQpDoorbell(
    const __gm__ CommArgs* args, int targetRank, uint32_t qpIdx)
{
    if (!TILEXR_UDMA_ARCH_SUPPORTED) {
        return TILEXR_UDMA_STATUS_INVALID;
    }
    if (!UDMAQueueOperationValid(args, targetRank, qpIdx)) {
        return TILEXR_UDMA_STATUS_INVALID;
    }
    __gm__ UDMAInfo* udmaInfo = GetUDMAInfo(args);
    if (udmaInfo->sqPtr == 0U) {
        return TILEXR_UDMA_STATUS_INVALID;
    }
    __gm__ UDMAWQCtx* qpCtxEntry =
        UDMAGetWQCtx(udmaInfo, static_cast<uint32_t>(targetRank), qpIdx);
    if (qpCtxEntry->headAddr == 0U || qpCtxEntry->tailAddr == 0U || qpCtxEntry->dbAddr == 0U ||
        qpCtxEntry->depth != TILEXR_UDMA_SQ_BB_COUNT) {
        return TILEXR_UDMA_STATUS_INVALID;
    }
    const uint32_t head = ld_dev(reinterpret_cast<__gm__ uint32_t*>(qpCtxEntry->headAddr), 0);
    const uint32_t tail = ld_dev(reinterpret_cast<__gm__ uint32_t*>(qpCtxEntry->tailAddr), 0);
    if (head - tail > qpCtxEntry->depth) {
        return TILEXR_UDMA_STATUS_INVALID;
    }
    UDMARingDoorbell(head, qpCtxEntry);
    return TILEXR_UDMA_STATUS_SUCCESS;
}

__aicore__ inline uint32_t UDMAQuietStatusOnQpUntil(
    const __gm__ CommArgs* args, int targetRank, uint32_t qpIdx,
    uint32_t completionTarget)
{
    if (!TILEXR_UDMA_ARCH_SUPPORTED) {
        return TILEXR_UDMA_STATUS_INVALID;
    }
    if (!UDMAQueueOperationValid(args, targetRank, qpIdx)) {
        return TILEXR_UDMA_STATUS_INVALID;
    }
    __gm__ UDMAInfo* udmaInfo = GetUDMAInfo(args);
    if (udmaInfo->sqPtr == 0U || udmaInfo->scqPtr == 0U) {
        return TILEXR_UDMA_STATUS_INVALID;
    }
    return UDMAPollCQ(
        udmaInfo, static_cast<uint32_t>(targetRank), qpIdx, completionTarget);
}

__aicore__ inline uint32_t UDMAQuietStatusOnQp(
    const __gm__ CommArgs* args, int targetRank, uint32_t qpIdx)
{
    if (!TILEXR_UDMA_ARCH_SUPPORTED ||
        !UDMAQueueOperationValid(args, targetRank, qpIdx)) {
        return TILEXR_UDMA_STATUS_INVALID;
    }
    __gm__ UDMAInfo* udmaInfo = GetUDMAInfo(args);
    __gm__ UDMAWQCtx* qpCtxEntry =
        UDMAGetWQCtx(udmaInfo, static_cast<uint32_t>(targetRank), qpIdx);
    if (qpCtxEntry == nullptr || qpCtxEntry->wqeCntAddr == 0U) {
        return TILEXR_UDMA_STATUS_INVALID;
    }
    const uint32_t completionTarget = ld_dev(
        reinterpret_cast<__gm__ uint32_t*>(qpCtxEntry->wqeCntAddr), 0);
    return UDMAQuietStatusOnQpUntil(
        args, targetRank, qpIdx, completionTarget);
}

__aicore__ inline uint32_t UDMAQuietStatus(const __gm__ CommArgs* args, int targetRank)
{
    return UDMAQuietStatusOnQp(args, targetRank, 0U);
}

__aicore__ inline void UDMAQuiet(const __gm__ CommArgs* args, int targetRank)
{
    (void)UDMAQuietStatus(args, targetRank);
}

} // namespace TileXR

#endif // TILEXR_UDMA_H
