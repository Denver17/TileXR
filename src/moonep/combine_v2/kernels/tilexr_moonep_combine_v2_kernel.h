/*
 * Copyright (c) 2024-2026 TileXR Project
 * Licensed under the Apache License, Version 2.0
 */

/*
 * 执行 MoonEP Combine V2 的传输、归约及失败传播；Host combine_v2_launch.cpp 提供工作区与路由参数。
 */

#ifndef TILEXR_MOONEP_COMBINE_V2_KERNEL_H
#define TILEXR_MOONEP_COMBINE_V2_KERNEL_H

#include <cstdint>

#include "comm_args.h"
#include "kernel_operator.h"
#include "combine_v2_schedule.h"
#include "combine_v2_wqe_batch.h"
#include "combine_v2_weight.h"
#include "tilexr_udma.h"
#include "tilexr_udma_fullmesh.h"

namespace {
using namespace AscendC;

constexpr uint32_t kUbAlignBytes = 32U;
constexpr uint32_t kFullUbBytes = 216U * 1024U;
constexpr uint32_t kWqeBytes = 64U;
constexpr uint32_t kControlWqesPerLane = 2U;
constexpr uint32_t kSixPortPayloadCapacity = 96U;
constexpr uint32_t kTwoPortPayloadCapacity = 32U;
constexpr uint32_t kSixPortIssueCapacity = 98U;
constexpr uint32_t kTwoPortIssueCapacity = 34U;
constexpr uint32_t kTotalIssueCapacity =
    kSixPortIssueCapacity + kTwoPortIssueCapacity;
constexpr uint32_t kFullmeshIssueCapacity =
    TileXRMoonEp::kMoonEpCombineV2PayloadBatchRows + 1U;
constexpr uint32_t kSixPortIssueBytes = kSixPortIssueCapacity * kWqeBytes;
constexpr uint32_t kTwoPortIssueBytes = kTwoPortIssueCapacity * kWqeBytes;
constexpr uint32_t kTotalIssueBytes = kTotalIssueCapacity * kWqeBytes;
constexpr uint32_t kDstSlotBytes =
    TileXRMoonEp::kMoonEpCombineV2SelectionChunkRows * sizeof(int32_t);
constexpr uint32_t kSelectionIndexBytes =
    TileXRMoonEp::kMoonEpCombineV2SelectionChunkRows * sizeof(int16_t);
constexpr uint32_t kSelectionMaskBytes =
    ((TileXRMoonEp::kMoonEpCombineV2SelectionChunkRows + 7U) / 8U +
        kUbAlignBytes - 1U) / kUbAlignBytes * kUbAlignBytes;
constexpr uint32_t kSelectionMaskElements =
    kSelectionMaskBytes / sizeof(uint16_t);
constexpr uint32_t kSelectionMaskUint32Elements =
    kSelectionMaskBytes / sizeof(uint32_t);
constexpr uint32_t kWqeContextBytes = 256U;
constexpr uint32_t kWeightChunkBytes =
    TileXRMoonEp::kMoonEpCombineV2SelectionChunkRows * sizeof(float);
constexpr uint32_t kWeightPackedOffset = kWeightChunkBytes;
constexpr uint32_t kWeightPackedDstBytes =
    TileXRMoonEp::kMoonEpCombineV2SelectionChunkRows * sizeof(uint32_t);
constexpr uint32_t kWeightRecordLocalStrideBytes = 32U;
constexpr uint32_t kWeightRecordStageBytes =
    TileXRMoonEp::kMoonEpCombineV2PayloadBatchRows *
        kWeightRecordLocalStrideBytes;
constexpr uint32_t kWeightRecordStage0Offset = kWeightPackedDstBytes;
constexpr uint32_t kWeightRecordStage1Offset =
    kWeightRecordStage0Offset + kWeightRecordStageBytes;
constexpr uint32_t kWeightDoneScratchOffset =
    kWeightRecordStage1Offset + kWeightRecordStageBytes;
constexpr uint32_t kWeightOutputTileRows =
    kSelectionMaskBytes * 8U / 4U;
constexpr uint32_t kWeightOutputTensorBytes =
    kWeightOutputTileRows * sizeof(uint32_t);
constexpr uint32_t kSendBufferBytes = kDstSlotBytes + kTotalIssueBytes +
    2U * kSelectionIndexBytes + 2U * kSelectionMaskBytes + kWqeContextBytes +
    2U * TileXRMoonEp::kMoonEpCombineV2SelfRelayHalfBytes;
constexpr uint32_t kReduceMaxInputBuffers = 8U;
constexpr uint32_t kReduceOutputBuffers = 2U;
constexpr uint32_t kReduceMaxRowBytes = 14U * 1024U;
constexpr uint32_t kReduceMaxRowElements =
    kReduceMaxRowBytes / sizeof(bfloat16_t);
constexpr uint64_t kOperationTimeoutCycles = 10000000000ULL;
constexpr uint32_t kPollNoCompletion = UINT32_MAX;
constexpr uint32_t kPollInvalidState = UINT32_MAX - 1U;
// Trusted-input benchmark mode compiles defensive validation and diagnostics
// out of the transfer hot path.
constexpr bool kEnableSafetyChecks = false;
constexpr uint32_t kCollectiveInitStage = 15U;
constexpr uint32_t kCollectiveValidationStage = 14U;
constexpr uint32_t kCollectiveDoneStage = 13U;
constexpr uint32_t kCollectiveReduceStage = 12U;
static_assert(sizeof(TileXR::UDMASqeCtx) + sizeof(TileXR::UDMASgeCtx) ==
    kWqeBytes, "Combine V2 WRITE WQE must occupy one basic block");
static_assert(sizeof(TileXR::UDMACqeCtx) == 64U,
    "Combine V2 CQE must occupy one cache line");
static_assert(TileXRMoonEp::kMoonEpCombineV2MaxSelectedPayloadWqes % 4U == 0U,
    "Combine V2 maximum payload count must preserve the 3:1 QP split");
static_assert(TileXRMoonEp::kMoonEpCombineV2MaxSelectedPayloadWqes * 3U / 4U ==
        kSixPortPayloadCapacity,
    "Combine V2 six-port payload capacity must preserve the 3:1 QP split");
static_assert(TileXRMoonEp::kMoonEpCombineV2MaxSelectedPayloadWqes / 4U ==
        kTwoPortPayloadCapacity,
    "Combine V2 two-port payload capacity must preserve the 3:1 QP split");
static_assert(kSixPortPayloadCapacity + kControlWqesPerLane ==
        kSixPortIssueCapacity,
    "Combine V2 six-port issue buffer cannot hold payload plus controls");
static_assert(kTwoPortPayloadCapacity + kControlWqesPerLane ==
        kTwoPortIssueCapacity,
    "Combine V2 two-port issue buffer cannot hold payload plus controls");
static_assert(kTotalIssueCapacity == 132U,
    "Combine V2 issue buffer must contain 128 payload and four controls");
static_assert(kFullmeshIssueCapacity <= kTotalIssueCapacity,
    "Combine V2 issue buffer cannot hold Fullmesh payload plus done");
static_assert(kSelectionMaskBytes == 1024U,
    "Combine V2 selection mask size changed unexpectedly");
static_assert(kWeightPackedOffset + kWeightChunkBytes ==
        TileXRMoonEp::kMoonEpCombineV2SelfRelayHalfBytes,
    "Combine V2 raw and packed weight tensors must fill one relay buffer");
static_assert(kWeightDoneScratchOffset +
        TileXRMoonEp::kMoonEpCombineV2WeightDoneStrideBytes <=
        TileXRMoonEp::kMoonEpCombineV2SelfRelayHalfBytes,
    "Combine V2 weight stages exceed one relay buffer");
static_assert(4U * kWeightOutputTensorBytes <= kWeightPackedDstBytes,
    "Combine V2 weight output tensors exceed the packed destination region");
static_assert(kSendBufferBytes == 207360U,
    "Combine V2 send buffer layout must occupy 207360 bytes");
static_assert(kFullUbBytes - kSendBufferBytes == 13824U,
    "Combine V2 send buffer layout must leave 13824 bytes of UB");

template <HardEvent event> __aicore__ inline void SyncFunc()
{
    const TEventID eventId = GetTPipePtr()->FetchEventID(event);
    SetFlag<event>(eventId);
    WaitFlag<event>(eventId);
}

__aicore__ inline uint64_t CeilDiv(uint64_t value, uint64_t divisor)
{
    return value / divisor + (value % divisor == 0U ? 0U : 1U);
}

__aicore__ inline uint64_t AlignUp(uint64_t value, uint64_t alignment)
{
    return CeilDiv(value, alignment) * alignment;
}

__aicore__ inline bool TimedOut(uint64_t startCycles)
{
    return static_cast<uint64_t>(GetSystemCycle()) - startCycles >=
        kOperationTimeoutCycles;
}

struct MoonEpCombineV2RemoteFields {
    uint64_t remoteRowBase;
    uint64_t rmtEidL;
    uint64_t rmtEidH;
    uint32_t tokenEn;
    uint32_t rmtJettyType;
    uint32_t targetHint;
    uint32_t tpId;
    uint32_t rmtJettyOrSegId;
    uint32_t rmtTokenValue;
};

struct alignas(32) MoonEpCombineV2OperatorFields {
    uint64_t rowBytes;
    uint64_t reserved[3];
};

struct alignas(32) MoonEpCombineV2PeerFields {
    uint64_t rowBytes;
    uint64_t reserved[3];
    MoonEpCombineV2RemoteFields remote[
        TileXRMoonEp::kMoonEpCombineV2LaneCount];
};

struct alignas(32) MoonEpCombineV2BuildContext {
    uint64_t localRowBase;
    uint64_t rowBytes;
    uint64_t remoteRowBase[TileXRMoonEp::kMoonEpCombineV2LaneCount];
    uint64_t peerBase;
    uint32_t chunkStart;
    uint32_t batchOffset;
    uint32_t batchCount;
    uint32_t sequencePhase;
    uint32_t head[TileXRMoonEp::kMoonEpCombineV2LaneCount];
};

struct alignas(32) MoonEpCombineV2WeightStageRecord {
    float value;
    uint32_t reserved;
    uint64_t magic;
    uint64_t padding[2];
};

static_assert(sizeof(MoonEpCombineV2WeightStageRecord) ==
        kWeightRecordLocalStrideBytes,
    "Combine V2 staged weight record must use a 32-byte UB stride");

static_assert(sizeof(MoonEpCombineV2OperatorFields) <= kWqeContextBytes,
    "Combine V2 operator fields exceed their UB allocation");
static_assert(sizeof(MoonEpCombineV2PeerFields) <= kWqeContextBytes,
    "Combine V2 peer fields exceed their UB allocation");
static_assert(sizeof(MoonEpCombineV2BuildContext) <= kWqeContextBytes,
    "Combine V2 build context exceeds its UB allocation");

struct MoonEpCombineV2LaneState {
    __gm__ TileXR::UDMAWQCtx *sq;
    __gm__ TileXR::UDMACQCtx *cq;
    uint32_t qp;
    uint32_t head;
    uint32_t tail;
    uint32_t completionCount;
    uint32_t cqTail;
    uint32_t cqTarget;
    uint32_t submittedHead;
};

#if defined(CATLASS_ARCH) && CATLASS_ARCH == 3510
__simt_vf__ __aicore__ LAUNCH_BOUND(
    TileXRMoonEp::kMoonEpCombineV2BuilderThreads)
inline void MoonEpCombineV2PrefillOperatorWqesVf(
    __ubuf__ uint8_t *sixPortWqes, __ubuf__ uint8_t *twoPortWqes,
    __ubuf__ const MoonEpCombineV2OperatorFields *fields)
{
    constexpr uint32_t totalCapacity =
        kSixPortPayloadCapacity + kTwoPortPayloadCapacity;
    for (uint32_t task = static_cast<uint32_t>(threadIdx.x);
        task < totalCapacity;
        task += TileXRMoonEp::kMoonEpCombineV2BuilderThreads) {
        __ubuf__ uint8_t *wqe = task < kSixPortPayloadCapacity ?
            sixPortWqes + task * kWqeBytes :
            twoPortWqes + (task - kSixPortPayloadCapacity) * kWqeBytes;
        __ubuf__ uint32_t *words =
            reinterpret_cast<__ubuf__ uint32_t *>(wqe);
        for (uint32_t word = 0U; word < kWqeBytes / sizeof(uint32_t);
            ++word) {
            words[word] = 0U;
        }
        __ubuf__ TileXR::UDMASqeCtx *sqe =
            reinterpret_cast<__ubuf__ TileXR::UDMASqeCtx *>(wqe);
        sqe->opcode = static_cast<uint32_t>(TileXR::UDMAOpcode::WRITE);
        sqe->flag = 0U;
        sqe->nf = 0U;
        sqe->inlineMsgLen = 0U;
        sqe->sgeNum = 1U;
        __ubuf__ TileXR::UDMASgeCtx *sge =
            reinterpret_cast<__ubuf__ TileXR::UDMASgeCtx *>(
                wqe + sizeof(TileXR::UDMASqeCtx));
        sge->len = static_cast<uint32_t>(fields->rowBytes);
        sge->tokenId = 0U;
    }
}

__simt_vf__ __aicore__ LAUNCH_BOUND(
    TileXRMoonEp::kMoonEpCombineV2BuilderThreads)
inline void MoonEpCombineV2PrefillPeerWqesVf(
    __ubuf__ uint8_t *sixPortWqes, __ubuf__ uint8_t *twoPortWqes,
    __ubuf__ const MoonEpCombineV2PeerFields *fields)
{
    constexpr uint32_t totalCapacity =
        kSixPortPayloadCapacity + kTwoPortPayloadCapacity;
    for (uint32_t task = static_cast<uint32_t>(threadIdx.x);
        task < totalCapacity;
        task += TileXRMoonEp::kMoonEpCombineV2BuilderThreads) {
        const uint32_t lane = task < kSixPortPayloadCapacity ?
            TileXRMoonEp::MOONEP_COMBINE_V2_SIX_PORT :
            TileXRMoonEp::MOONEP_COMBINE_V2_TWO_PORT;
        const uint32_t laneIndex = task < kSixPortPayloadCapacity ? task :
            task - kSixPortPayloadCapacity;
        __ubuf__ uint8_t *wqe =
            (lane == TileXRMoonEp::MOONEP_COMBINE_V2_SIX_PORT ?
                sixPortWqes : twoPortWqes) + laneIndex * kWqeBytes;
        __ubuf__ uint32_t *words =
            reinterpret_cast<__ubuf__ uint32_t *>(wqe);
        for (uint32_t word = 0U; word < kWqeBytes / sizeof(uint32_t);
            ++word) {
            words[word] = 0U;
        }
        __ubuf__ TileXR::UDMASqeCtx *sqe =
            reinterpret_cast<__ubuf__ TileXR::UDMASqeCtx *>(wqe);
        sqe->opcode = static_cast<uint32_t>(TileXR::UDMAOpcode::WRITE);
        sqe->flag = 0U;
        sqe->nf = 0U;
        sqe->tokenEn = fields->remote[lane].tokenEn;
        sqe->rmtJettyType = fields->remote[lane].rmtJettyType;
        sqe->targetHint = fields->remote[lane].targetHint;
        sqe->inlineMsgLen = 0U;
        sqe->tpId = fields->remote[lane].tpId;
        sqe->sgeNum = 1U;
        sqe->rmtJettyOrSegId = fields->remote[lane].rmtJettyOrSegId;
        sqe->rmtTokenValue = fields->remote[lane].rmtTokenValue;
        sqe->rmtEidL = fields->remote[lane].rmtEidL;
        sqe->rmtEidH = fields->remote[lane].rmtEidH;
        __ubuf__ TileXR::UDMASgeCtx *sge =
            reinterpret_cast<__ubuf__ TileXR::UDMASgeCtx *>(
                wqe + sizeof(TileXR::UDMASqeCtx));
        sge->len = static_cast<uint32_t>(fields->rowBytes);
        sge->tokenId = 0U;
    }
}

__simt_vf__ __aicore__ LAUNCH_BOUND(
    TileXRMoonEp::kMoonEpCombineV2BuilderThreads)
inline void MoonEpCombineV2PrefillFullmeshWqesVf(
    __ubuf__ uint8_t *wqes,
    __ubuf__ const MoonEpCombineV2PeerFields *fields)
{
    for (uint32_t task = static_cast<uint32_t>(threadIdx.x);
        task < TileXRMoonEp::kMoonEpCombineV2PayloadBatchRows;
        task += TileXRMoonEp::kMoonEpCombineV2BuilderThreads) {
        __ubuf__ uint8_t *wqe = wqes + task * kWqeBytes;
        __ubuf__ uint32_t *words =
            reinterpret_cast<__ubuf__ uint32_t *>(wqe);
        for (uint32_t word = 0U; word < kWqeBytes / sizeof(uint32_t);
            ++word) {
            words[word] = 0U;
        }
        __ubuf__ const MoonEpCombineV2RemoteFields *remote =
            &fields->remote[0];
        __ubuf__ TileXR::UDMASqeCtx *sqe =
            reinterpret_cast<__ubuf__ TileXR::UDMASqeCtx *>(wqe);
        sqe->opcode = static_cast<uint32_t>(TileXR::UDMAOpcode::WRITE);
        sqe->flag = 0U;
        sqe->nf = 0U;
        sqe->tokenEn = remote->tokenEn;
        sqe->rmtJettyType = remote->rmtJettyType;
        sqe->targetHint = remote->targetHint;
        sqe->inlineMsgLen = 0U;
        sqe->tpId = remote->tpId;
        sqe->sgeNum = 1U;
        sqe->rmtJettyOrSegId = remote->rmtJettyOrSegId;
        sqe->rmtTokenValue = remote->rmtTokenValue;
        sqe->rmtEidL = remote->rmtEidL;
        sqe->rmtEidH = remote->rmtEidH;
        __ubuf__ TileXR::UDMASgeCtx *sge =
            reinterpret_cast<__ubuf__ TileXR::UDMASgeCtx *>(
                wqe + sizeof(TileXR::UDMASqeCtx));
        sge->len = static_cast<uint32_t>(fields->rowBytes);
        sge->tokenId = 0U;
    }
}

__simt_vf__ __aicore__ LAUNCH_BOUND(
    TileXRMoonEp::kMoonEpCombineV2BuilderThreads)
inline void MoonEpCombineV2BuildPayloadWqesVf(
    __ubuf__ uint8_t *sixPortWqes, __ubuf__ uint8_t *twoPortWqes,
    __ubuf__ const int32_t *dstSlots,
    __ubuf__ const int16_t *selectedIndices,
    __ubuf__ const MoonEpCombineV2BuildContext *context)
{
    const uint32_t task = static_cast<uint32_t>(threadIdx.x);
    if (task < context->batchCount) {
        const uint32_t densePosition = context->batchOffset + task;
        const uint32_t relativeIndex = static_cast<uint16_t>(
            selectedIndices[densePosition]);
        const uint32_t sourceSlotIndex = context->chunkStart + relativeIndex;
        const uint32_t targetSlot = static_cast<uint32_t>(
            static_cast<uint64_t>(dstSlots[relativeIndex]) -
                context->peerBase);
        const uint32_t position = context->sequencePhase + task;
        const uint32_t lane =
            TileXRMoonEp::MoonEpCombineV2LaneForPosition(position);
        const uint32_t firstTwoPort =
            (3U - (context->sequencePhase & 3U)) & 3U;
        const uint32_t twoBefore = task <= firstTwoPort ? 0U :
            1U + (task - 1U - firstTwoPort) / 4U;
        const uint32_t laneIndex =
            lane == TileXRMoonEp::MOONEP_COMBINE_V2_TWO_PORT ?
            twoBefore : task - twoBefore;
        __ubuf__ uint8_t *wqe =
            (lane == TileXRMoonEp::MOONEP_COMBINE_V2_SIX_PORT ?
                sixPortWqes : twoPortWqes) + laneIndex * kWqeBytes;
        const uint64_t localAddr = context->localRowBase +
            static_cast<uint64_t>(sourceSlotIndex) *
                context->rowBytes;
        const uint64_t remoteAddr = context->remoteRowBase[lane] +
            static_cast<uint64_t>(targetSlot) *
                context->rowBytes;
        const uint32_t absoluteHead = context->head[lane] + laneIndex;

        __ubuf__ TileXR::UDMASqeCtx *sqe =
            reinterpret_cast<__ubuf__ TileXR::UDMASqeCtx *>(wqe);
        sqe->sqeBbIdx = static_cast<uint16_t>(
            absoluteHead % TileXR::TILEXR_UDMA_SQ_BB_COUNT);
        sqe->owner =
            (absoluteHead & TileXR::TILEXR_UDMA_SQ_BB_COUNT) == 0U ? 1U : 0U;
        sqe->rmtAddrLOrTokenId = remoteAddr & 0xFFFFFFFFU;
        sqe->rmtAddrHOrTokenValue = (remoteAddr >> 32U) & 0xFFFFFFFFU;

        __ubuf__ TileXR::UDMASgeCtx *sge =
            reinterpret_cast<__ubuf__ TileXR::UDMASgeCtx *>(
                wqe + sizeof(TileXR::UDMASqeCtx));
        sge->va = localAddr;
    }
}

__simt_vf__ __aicore__ LAUNCH_BOUND(
    TileXRMoonEp::kMoonEpCombineV2BuilderThreads)
inline void MoonEpCombineV2BuildFullmeshPayloadWqesVf(
    __ubuf__ uint8_t *wqes, __ubuf__ const int32_t *dstSlots,
    __ubuf__ const int16_t *selectedIndices,
    __ubuf__ const MoonEpCombineV2BuildContext *context)
{
    const uint32_t task = static_cast<uint32_t>(threadIdx.x);
    if (task >= context->batchCount) {
        return;
    }
    const uint32_t densePosition = context->batchOffset + task;
    const uint32_t relativeIndex = static_cast<uint16_t>(
        selectedIndices[densePosition]);
    const uint32_t sourceSlotIndex = context->chunkStart + relativeIndex;
    const uint32_t targetSlot = static_cast<uint32_t>(
        static_cast<uint64_t>(dstSlots[relativeIndex]) - context->peerBase);
    __ubuf__ uint8_t *wqe = wqes + task * kWqeBytes;
    const uint64_t localAddr = context->localRowBase +
        static_cast<uint64_t>(sourceSlotIndex) * context->rowBytes;
    const uint64_t remoteAddr = context->remoteRowBase[0] +
        static_cast<uint64_t>(targetSlot) * context->rowBytes;
    const uint32_t absoluteHead = context->head[0] + task;

    __ubuf__ TileXR::UDMASqeCtx *sqe =
        reinterpret_cast<__ubuf__ TileXR::UDMASqeCtx *>(wqe);
    sqe->flag = 0U;
    sqe->sqeBbIdx = static_cast<uint16_t>(
        absoluteHead % TileXR::TILEXR_UDMA_SQ_BB_COUNT);
    sqe->owner =
        (absoluteHead & TileXR::TILEXR_UDMA_SQ_BB_COUNT) == 0U ? 1U : 0U;
    sqe->rmtAddrLOrTokenId = remoteAddr & 0xFFFFFFFFU;
    sqe->rmtAddrHOrTokenValue = (remoteAddr >> 32U) & 0xFFFFFFFFU;
    __ubuf__ TileXR::UDMASgeCtx *sge =
        reinterpret_cast<__ubuf__ TileXR::UDMASgeCtx *>(
            wqe + sizeof(TileXR::UDMASqeCtx));
    sge->va = localAddr;
}

__simt_vf__ __aicore__ LAUNCH_BOUND(
    TileXRMoonEp::kMoonEpCombineV2BuilderThreads)
inline void MoonEpCombineV2BuildWeightRecordsVf(
    __ubuf__ MoonEpCombineV2WeightStageRecord *records,
    __ubuf__ const float *weights,
    uint64_t magic, uint32_t batchStart, uint32_t batchCount)
{
    const uint32_t task = static_cast<uint32_t>(threadIdx.x);
    if (task < batchCount) {
        __ubuf__ MoonEpCombineV2WeightStageRecord *record =
            records + task;
        record->value = weights[batchStart + task];
        record->reserved = 0U;
        record->magic = magic;
        record->padding[0] = 0U;
        record->padding[1] = 0U;
    }
}

#endif

class MoonEpCombineV2 {
public:
    __aicore__ inline void Init(GM_ADDR commArgsGM,
        GM_ADDR registeredWorkspaceGM, GM_ADDR dstLocalGM,
        uint64_t scratchEpoch0Offset,
        uint64_t scratchEpoch1Offset, uint64_t doneOffset,
        uint64_t reservedOffset0, uint64_t controlSourceOffset,
        uint64_t failureOffset, uint64_t reservedSyncReceiveOffset,
        uint64_t reservedSyncSourceOffset, uint64_t collectiveStatusOffset,
        uint64_t outputOffset, int64_t bs,
        int64_t h, int64_t topK, int64_t nvS, uint64_t rowBytes,
        bool reduceHidden, int64_t magic, GM_ADDR weightMemoryCommArgs,
        GM_ADDR routeWeightsNvs, GM_ADDR routeWeightsSk,
        uint64_t weightRecordOffset, uint64_t weightDoneOffset,
        uint64_t weightWindowBytes, uint64_t weightOutputElements,
        bool hasRouteWeight, TPipe *pipe);
    __aicore__ inline void Process();

private:
    __aicore__ inline void InitBuffers();
    __aicore__ inline void PrefillOperatorWqes();
    __aicore__ inline bool PrefillPeerWqes(uint32_t peer);
    __aicore__ inline bool PrefillFullmeshWqes(uint32_t peer);
    __aicore__ inline void SetFailure(uint32_t status, uint32_t step,
        uint32_t peer, uint32_t lane, uint32_t cqStatus = 0U,
        uint64_t expected = 0U, uint64_t observed = 0U,
        uint32_t qp = UINT32_MAX);
    __aicore__ inline void PublishFailureRecord(uint32_t stageId);
    __aicore__ inline bool CheckPoison();
    __aicore__ inline bool ValidateDestinations();
    __aicore__ inline void LoadSelectionChunk(uint64_t chunkStart,
        uint32_t chunkElements);
    __aicore__ inline uint32_t SelectPeerIndices(uint32_t peer,
        uint32_t chunkElements);
    __aicore__ inline uint32_t SelectPeerDestinations(uint32_t peer,
        uint32_t chunkElements, LocalTensor<int32_t> packedDestinations);
    __aicore__ inline bool InitLaneStates();
    __aicore__ inline bool InitFullmeshState(uint32_t peer);
    __aicore__ inline bool ResolveRemoteFields(uint32_t targetRank,
        uint32_t lane, uint64_t remoteBaseOffset,
        __ubuf__ MoonEpCombineV2RemoteFields *fields);
    __aicore__ inline bool ResolveFullmeshRemoteFields(uint32_t targetRank,
        uint64_t remoteBaseOffset,
        __ubuf__ MoonEpCombineV2RemoteFields *fields);
    __aicore__ inline uint32_t PollCqOnce(
        MoonEpCombineV2LaneState &state);
    __aicore__ inline bool WaitStepCqs(uint32_t step);
    __aicore__ inline bool WaitFullmeshCq(uint32_t step, uint32_t peer);
    __aicore__ inline bool WaitStepCredit(uint32_t step,
        const TileXRMoonEp::MoonEpCombineV2StepRoute &route);
    __aicore__ inline bool PublishNextCredit(uint32_t step,
        const TileXRMoonEp::MoonEpCombineV2StepRoute &route);
    __aicore__ inline bool BuildPayloadWqes(uint32_t chunkStart,
        uint32_t batchOffset, uint32_t batchCount, uint32_t sequenceBase,
        uint64_t peerBase);
    __aicore__ inline bool BuildFullmeshPayloadWqes(uint32_t chunkStart,
        uint32_t batchOffset, uint32_t batchCount, uint64_t peerBase);
    __aicore__ inline bool AppendControlWqe(LocalTensor<uint8_t> issue,
        uint32_t outputIndex, MoonEpCombineV2LaneState &state,
        __gm__ TileXR::UDMAInfo *info,
        uint32_t targetRank, uint64_t remoteOffset,
        __gm__ uint64_t *localSource, uint32_t flag,
        uint32_t transferBytes = sizeof(uint64_t));
    __aicore__ inline void PublishControlToken(
        __gm__ uint64_t *destination, uint64_t value);
    __aicore__ inline void CopyIssueToSq(LocalTensor<uint8_t> issue,
        MoonEpCombineV2LaneState &state, uint32_t count);
    __aicore__ inline bool SubmitPair(uint32_t peer, uint32_t step,
        uint32_t chunkStart, uint32_t batchOffset, uint32_t batchCount,
        uint32_t sequenceBase, uint64_t peerBase, bool finalBatch);
    __aicore__ inline bool SendRemoteStep(uint32_t peer, uint32_t step);
    __aicore__ inline bool SubmitFullmeshBatch(uint32_t peer, uint32_t step,
        uint32_t chunkStart, uint32_t batchOffset, uint32_t batchCount,
        uint64_t peerBase, bool finalBatch);
    __aicore__ inline bool SendFullmeshStep(uint32_t peer, uint32_t step);
    __aicore__ inline void CopySelfRowsIn(uint32_t selectedStart,
        uint32_t selectedCount, uint32_t chunkStart,
        uint32_t localRowStride,
        LocalTensor<uint8_t> relay);
    __aicore__ inline void CopySelfRowsOut(uint32_t selectedStart,
        uint32_t selectedCount, uint64_t peerBase,
        uint32_t localRowStride,
        LocalTensor<uint8_t> relay);
    __aicore__ inline void CopySelfTileIn(uint32_t selectedIndex,
        uint32_t chunkStart, uint64_t rowOffset, uint32_t tileBytes,
        LocalTensor<uint8_t> relay);
    __aicore__ inline void CopySelfTileOut(uint32_t selectedIndex,
        uint64_t peerBase, uint64_t rowOffset, uint32_t tileBytes,
        LocalTensor<uint8_t> relay);
    __aicore__ inline bool CopySelfSelectedIndices(uint32_t selectedCount,
        uint32_t chunkStart, uint64_t peerBase);
    __aicore__ inline bool SendSelfStep(uint32_t peer, uint32_t step);
    __aicore__ inline bool SendWeightMemoryStep(
        uint32_t peer, uint32_t step);
    __aicore__ inline bool WaitInboundDone(
        uint32_t step, uint32_t source);
    __aicore__ inline bool WaitInboundWeightDone(
        uint32_t step, uint32_t source);
    __aicore__ inline bool CopyReceivedWeights();
    __aicore__ inline bool BeginCollectiveStage(uint32_t stageId);
    __aicore__ inline bool EndCollectiveStage(
        uint32_t stageId, bool localSucceeded);
    __aicore__ inline void InitReduceBuffers(uint32_t inputBufferNum,
        uint32_t inputSlotBytes, uint32_t floatRowBytes,
        uint32_t outputSlotBytes);
    __aicore__ inline void CopyReduceInput(uint64_t workOrdinal,
        int64_t tokenBegin, uint64_t rowCount);
    __aicore__ inline bool ReduceHidden();
    __aicore__ inline uint64_t LoadToken(__gm__ uint64_t *token);

    bool valid_{false};
    bool collectiveReady_{false};
    bool activeWorker_{false};
    TPipe *pipe_{nullptr};
    __gm__ TileXR::CommArgs *args_{nullptr};
    __gm__ TileXR::CommArgs *weightMemoryArgs_{nullptr};
    __gm__ TileXR::TileXRUDMARegistry *registry_{nullptr};
    __gm__ TileXR::TileXRUDMAFullmeshDeviceView *fullmeshView_{nullptr};
    __gm__ TileXR::UDMAInfo *fullmeshInfo_{nullptr};
    __gm__ uint8_t *workspace_{nullptr};
    __gm__ int32_t *dstGlobalAddr_{nullptr};
    __gm__ uint8_t *scratch_{nullptr};
    __gm__ uint8_t *doneBase_{nullptr};
    __gm__ uint8_t *controlSourceBase_{nullptr};
    __gm__ uint8_t *failureBase_{nullptr};
    __gm__ uint8_t *collectiveStatusBase_{nullptr};
    __gm__ uint8_t *routeWeightsNvsAddr_{nullptr};
    __gm__ uint8_t *routeWeightsSkAddr_{nullptr};
    __gm__ uint8_t *localWeightWindow_{nullptr};
    uint64_t scratchOffset_{0U};
    uint64_t doneOffset_{0U};
    uint64_t failureOffset_{0U};
    uint64_t outputOffset_{0U};
    uint64_t weightRecordOffset_{0U};
    uint64_t weightRecordEpochStride_{0U};
    uint64_t weightDoneOffset_{0U};
    uint64_t weightDoneEpochStride_{0U};
    uint64_t weightWindowBytes_{0U};
    uint64_t weightOutputElements_{0U};
    uint64_t slots_{0U};
    uint64_t rowBytes_{0U};
    int64_t bs_{0};
    int64_t h_{0};
    int64_t topK_{0};
    uint64_t magic_{0U};
    uint64_t operationStartCycles_{0U};
    uint32_t epoch_{0U};
    uint32_t rank_{0U};
    uint32_t rankSize_{0U};
    uint32_t core_{0U};
    uint32_t activeCoreCount_{0U};
    uint32_t launchCoreCount_{0U};
    uint32_t stepCount_{0U};
    uint32_t issuedRows_{0U};
    uint32_t selectedPeerRows_{0U};
    uint64_t remoteRowBase_[TileXRMoonEp::kMoonEpCombineV2LaneCount] {};
    uint32_t failureStatus_{TileXRMoonEp::MOONEP_COMBINE_V2_SUCCESS};
    uint32_t failureStep_{UINT32_MAX};
    uint32_t failurePeer_{UINT32_MAX};
    uint32_t failureLane_{UINT32_MAX};
    uint32_t failureCqStatus_{0U};
    uint32_t failureQp_{UINT32_MAX};
    uint64_t failureExpected_{0U};
    uint64_t failureObserved_{0U};
    bool selectionBuffersUsed_{false};
    bool reduceHidden_{false};
    bool hasRouteWeight_{false};
    TileXRMoonEp::MoonEpCombineV2ScheduleMode scheduleMode_{
        TileXRMoonEp::MOONEP_COMBINE_V2_SINGLE_RING};
    MoonEpCombineV2LaneState lane_[
        TileXRMoonEp::kMoonEpCombineV2LaneCount] {};
    MoonEpCombineV2LaneState fullmeshLane_ {};
    uint32_t fullmeshPeer_{TileXRMoonEp::kMoonEpCombineV2InvalidPeer};

    TBuf<QuePosition::VECCALC> dstSlotBuf_;
    TBuf<QuePosition::VECCALC> slotIndexBuf_;
    TBuf<QuePosition::VECCALC> selectedIndexBuf_;
    TBuf<QuePosition::VECCALC> lowerMaskBuf_;
    TBuf<QuePosition::VECCALC> upperMaskBuf_;
    TBuf<QuePosition::VECCALC> wqeIssueBuf_;
    TBuf<QuePosition::VECCALC> wqeContextBuf_;
    TBuf<QuePosition::VECCALC> selfRelayPingBuf_;
    TBuf<QuePosition::VECCALC> selfRelayPongBuf_;
    TQue<QuePosition::VECIN, kReduceMaxInputBuffers> reduceInputQueue_;
    TQue<QuePosition::VECOUT, 1> reduceOutputQueue_;
    TBuf<QuePosition::VECCALC> reduceRowBuf_;
    TBuf<QuePosition::VECCALC> reduceAccumulatorBuf_;
};

__aicore__ inline void MoonEpCombineV2::Init(
    GM_ADDR commArgsGM, GM_ADDR registeredWorkspaceGM, GM_ADDR dstLocalGM,
    uint64_t scratchEpoch0Offset,
    uint64_t scratchEpoch1Offset, uint64_t doneOffset,
    uint64_t reservedOffset0, uint64_t controlSourceOffset,
    uint64_t failureOffset, uint64_t reservedSyncReceiveOffset,
    uint64_t reservedSyncSourceOffset, uint64_t collectiveStatusOffset,
    uint64_t outputOffset, int64_t bs,
    int64_t h, int64_t topK, int64_t nvS, uint64_t rowBytes,
    bool reduceHidden, int64_t magic, GM_ADDR weightMemoryCommArgs,
    GM_ADDR routeWeightsNvs, GM_ADDR routeWeightsSk,
    uint64_t weightRecordOffset, uint64_t weightDoneOffset,
    uint64_t weightWindowBytes, uint64_t weightOutputElements,
    bool hasRouteWeight, TPipe *pipe)
{
    pipe_ = pipe;
    (void)reservedOffset0;
    (void)reservedSyncReceiveOffset;
    (void)reservedSyncSourceOffset;
    args_ = reinterpret_cast<__gm__ TileXR::CommArgs *>(commArgsGM);
    weightMemoryArgs_ = reinterpret_cast<__gm__ TileXR::CommArgs *>(
        weightMemoryCommArgs);
    workspace_ = reinterpret_cast<__gm__ uint8_t *>(registeredWorkspaceGM);
    dstGlobalAddr_ = reinterpret_cast<__gm__ int32_t *>(dstLocalGM);
    routeWeightsNvsAddr_ = reinterpret_cast<__gm__ uint8_t *>(
        routeWeightsNvs);
    routeWeightsSkAddr_ = reinterpret_cast<__gm__ uint8_t *>(
        routeWeightsSk);
    hasRouteWeight_ = hasRouteWeight;
    core_ = static_cast<uint32_t>(GetBlockIdx());
    launchCoreCount_ = static_cast<uint32_t>(GetBlockNum());
    operationStartCycles_ = static_cast<uint64_t>(GetSystemCycle());
    collectiveReady_ = pipe_ != nullptr && args_ != nullptr &&
        workspace_ != nullptr && dstGlobalAddr_ != nullptr &&
        launchCoreCount_ == TileXRMoonEp::kMoonEpCombineV2CoreCount &&
        core_ < launchCoreCount_;
    if (!collectiveReady_) {
        return;
    }
    failureOffset_ = failureOffset;
    failureBase_ = workspace_ + failureOffset;
    collectiveStatusBase_ = workspace_ + collectiveStatusOffset;
    magic_ = magic > 0 ? static_cast<uint64_t>(magic) : 0U;
    epoch_ = TileXRMoonEp::MoonEpCombineV2Epoch(magic_);
    rank_ = args_->rank >= 0 ? static_cast<uint32_t>(args_->rank) : 0U;
    rankSize_ = args_->rankSize > 0 ?
        static_cast<uint32_t>(args_->rankSize) : 0U;
    scheduleMode_ = TileXRMoonEp::MoonEpCombineV2ScheduleModeForRankSize(
        rankSize_);
    activeCoreCount_ = TileXRMoonEp::MoonEpCombineV2RankSizeSupported(
            rankSize_) ?
        TileXRMoonEp::MoonEpCombineV2ActiveCoreCount(rankSize_) : 0U;
    activeWorker_ = core_ < activeCoreCount_;
    __gm__ TileXR::UDMAInfo *closInfo = TileXR::GetUDMAInfo(args_);
    fullmeshView_ = reinterpret_cast<__gm__
        TileXR::TileXRUDMAFullmeshDeviceView *>(args_->udmaFullmeshPtr);
    if (!TileXR::UDMARegistryEnabled(args_) ||
        (args_->extraFlag &
            TileXR::ExtraFlag::UDMA_SHARED_QP) == 0U ||
        (args_->extraFlag & TileXR::ExtraFlag::UDMA_FULLMESH) == 0U ||
        closInfo == nullptr || fullmeshView_ == nullptr ||
        !TileXRMoonEp::MoonEpCombineV2RankSizeSupported(
            static_cast<uint32_t>(args_->rankSize)) ||
        args_->rank < 0 || args_->rank >= args_->rankSize ||
        args_->localRank < 0 ||
        args_->localRankSize != static_cast<int>(
            TileXRMoonEp::MoonEpCombineV2LocalRankSize(
                static_cast<uint32_t>(args_->rankSize))) ||
        args_->localRank >= args_->localRankSize ||
        args_->rank % args_->localRankSize != args_->localRank ||
        !TileXRMoonEp::MoonEpCombineV2ShapeValid(
            bs, h, topK, nvS) || magic <= 0 ||
        rowBytes == 0U || rowBytes > UINT32_MAX ||
        (reduceHidden && rowBytes !=
            static_cast<uint64_t>(h) * sizeof(bfloat16_t)) ||
        closInfo->qpNum < TileXRMoonEp::kMoonEpCombineV2QpCount ||
        fullmeshView_->magic != TileXR::TILEXR_UDMA_FULLMESH_MAGIC ||
        fullmeshView_->version != TileXR::TILEXR_UDMA_FULLMESH_VERSION ||
        fullmeshView_->slotCount !=
            TileXRMoonEp::kMoonEpCombineV2FullmeshSlotCount ||
        fullmeshView_->localRank != static_cast<uint32_t>(args_->localRank) ||
        fullmeshView_->connectedCount + 1U !=
            static_cast<uint32_t>(args_->localRankSize) ||
        fullmeshView_->validPeerMask != TileXR::UDMAFullmeshExpectedPeerMask(
            static_cast<uint32_t>(args_->localRank),
            static_cast<uint32_t>(args_->localRankSize)) ||
        fullmeshView_->registrationReady == 0U ||
        fullmeshView_->registrationGeneration == 0U ||
        fullmeshView_->registrationGeneration !=
            args_->udmaRegistrationGeneration ||
        fullmeshView_->infoPtr == 0U) {
        return;
    }
    fullmeshInfo_ = reinterpret_cast<__gm__ TileXR::UDMAInfo *>(
        fullmeshView_->infoPtr);
    if (fullmeshInfo_ == nullptr || fullmeshInfo_->qpNum !=
        TileXRMoonEp::kMoonEpCombineV2FullmeshSlotCount) {
        return;
    }
    for (uint32_t peer = 0U; peer < rankSize_; ++peer) {
        if (args_->creditMems[peer] == nullptr) {
            return;
        }
    }
    if (!TileXRMoonEp::MoonEpCombineV2MagicValid(magic_)) {
        return;
    }
    stepCount_ = TileXRMoonEp::MoonEpCombineV2StepCount(rankSize_);
    slots_ = static_cast<uint64_t>(nvS);
    rowBytes_ = rowBytes;
    bs_ = bs;
    h_ = h;
    topK_ = topK;
    reduceHidden_ = reduceHidden;
    weightRecordOffset_ = weightRecordOffset;
    weightDoneOffset_ = weightDoneOffset;
    weightWindowBytes_ = weightWindowBytes;
    weightOutputElements_ = weightOutputElements;
    if (hasRouteWeight_) {
        const uint64_t recordRegionBytes =
            weightRecordOffset_ <= weightDoneOffset_ ?
                weightDoneOffset_ - weightRecordOffset_ : 0U;
        const uint64_t doneRegionBytes =
            weightDoneOffset_ <= weightWindowBytes_ ?
                weightWindowBytes_ - weightDoneOffset_ : 0U;
        weightRecordEpochStride_ = recordRegionBytes /
            TileXRMoonEp::kMoonEpCombineV2EpochCount;
        weightDoneEpochStride_ = doneRegionBytes /
            TileXRMoonEp::kMoonEpCombineV2EpochCount;
        if (weightMemoryArgs_ == nullptr ||
            routeWeightsNvsAddr_ == nullptr || routeWeightsSkAddr_ == nullptr ||
            weightMemoryArgs_->rank != args_->rank ||
            weightMemoryArgs_->rankSize != args_->rankSize ||
            weightMemoryArgs_->localRank != args_->localRank ||
            weightMemoryArgs_->localRankSize != args_->localRankSize ||
            weightMemoryArgs_->commDomain == args_->commDomain ||
            (weightMemoryArgs_->extraFlag &
                TileXR::ExtraFlag::MEMORY_ONLY) == 0U ||
            weightOutputElements_ > slots_ ||
            weightRecordOffset_ > weightDoneOffset_ ||
            recordRegionBytes % TileXRMoonEp::kMoonEpCombineV2EpochCount !=
                0U ||
            weightRecordEpochStride_ %
                TileXRMoonEp::kMoonEpCombineV2WeightDoneStrideBytes != 0U ||
            slots_ > weightRecordEpochStride_ /
                TileXRMoonEp::kMoonEpCombineV2WeightRecordBytes ||
            weightDoneOffset_ > weightWindowBytes_ ||
            doneRegionBytes % TileXRMoonEp::kMoonEpCombineV2EpochCount != 0U ||
            weightDoneEpochStride_ %
                TileXRMoonEp::kMoonEpCombineV2WeightDoneStrideBytes != 0U ||
            rankSize_ > weightDoneEpochStride_ /
                TileXRMoonEp::kMoonEpCombineV2WeightDoneStrideBytes ||
            weightMemoryArgs_->peerMemBytes <=
                static_cast<uint64_t>(TileXR::IPC_DATA_OFFSET) ||
            weightWindowBytes_ > weightMemoryArgs_->peerMemBytes -
                static_cast<uint64_t>(TileXR::IPC_DATA_OFFSET)) {
            return;
        }
        for (uint32_t peer = 0U; peer < rankSize_; ++peer) {
            if (weightMemoryArgs_->peerMems[peer] == nullptr) {
                return;
            }
        }
        localWeightWindow_ = reinterpret_cast<__gm__ uint8_t *>(
            weightMemoryArgs_->peerMems[rank_]) + TileXR::IPC_DATA_OFFSET;
    }
    scratchOffset_ = epoch_ == 0U ? scratchEpoch0Offset :
        scratchEpoch1Offset;
    doneOffset_ = doneOffset;
    failureOffset_ = failureOffset;
    outputOffset_ = outputOffset;
    scratch_ = workspace_ + scratchOffset_;
    doneBase_ = workspace_ + doneOffset;
    controlSourceBase_ = workspace_ + controlSourceOffset +
        static_cast<uint64_t>(core_) *
            TileXRMoonEp::kMoonEpCombineV2LaneCount *
            TileXRMoonEp::kMoonEpCombineV2TokenStrideBytes;
    failureBase_ = workspace_ + failureOffset;
    valid_ = true;
    const bool laneStatesReady = !activeWorker_ || InitLaneStates();
    if (!laneStatesReady) {
        SetFailure(TileXRMoonEp::MOONEP_COMBINE_V2_INVALID_CONFIG,
            UINT32_MAX, UINT32_MAX, UINT32_MAX);
        valid_ = false;
    }
    (void)laneStatesReady;

}

__aicore__ inline void MoonEpCombineV2::InitBuffers()
{
    static_assert(kSendBufferBytes <= kFullUbBytes,
        "Combine V2 send buffers exceed 216 KiB UB");
    pipe_->Reset();
    pipe_->InitBuffer(dstSlotBuf_, kDstSlotBytes);
    pipe_->InitBuffer(slotIndexBuf_, kSelectionIndexBytes);
    pipe_->InitBuffer(selectedIndexBuf_, kSelectionIndexBytes);
    pipe_->InitBuffer(lowerMaskBuf_, kSelectionMaskBytes);
    pipe_->InitBuffer(upperMaskBuf_, kSelectionMaskBytes);
    pipe_->InitBuffer(wqeIssueBuf_, kTotalIssueBytes);
    pipe_->InitBuffer(wqeContextBuf_, kWqeContextBytes);
    pipe_->InitBuffer(selfRelayPingBuf_,
        TileXRMoonEp::kMoonEpCombineV2SelfRelayHalfBytes);
    pipe_->InitBuffer(selfRelayPongBuf_,
        TileXRMoonEp::kMoonEpCombineV2SelfRelayHalfBytes);
    CreateVecIndex(slotIndexBuf_.Get<int16_t>(), static_cast<int16_t>(0),
        TileXRMoonEp::kMoonEpCombineV2SelectionChunkRows);
    PipeBarrier<PIPE_V>();
}

__aicore__ inline void MoonEpCombineV2::PrefillOperatorWqes()
{
    __ubuf__ MoonEpCombineV2OperatorFields *fields = reinterpret_cast<__ubuf__
        MoonEpCombineV2OperatorFields *>(
            wqeContextBuf_.Get<uint8_t>().GetPhyAddr());
    fields->rowBytes = rowBytes_;
    LocalTensor<uint8_t> issue = wqeIssueBuf_.Get<uint8_t>();
#if defined(CATLASS_ARCH) && CATLASS_ARCH == 3510
    PipeBarrier<PIPE_ALL>();
    Simt::VF_CALL<MoonEpCombineV2PrefillOperatorWqesVf>(
        Simt::Dim3{TileXRMoonEp::kMoonEpCombineV2BuilderThreads, 1U, 1U},
        reinterpret_cast<__ubuf__ uint8_t *>(
            issue.GetPhyAddr()),
        reinterpret_cast<__ubuf__ uint8_t *>(
            issue[kSixPortIssueBytes].GetPhyAddr()), fields);
    PipeBarrier<PIPE_ALL>();
#endif
}

__aicore__ inline bool MoonEpCombineV2::PrefillPeerWqes(uint32_t peer)
{
    __ubuf__ MoonEpCombineV2PeerFields *fields = reinterpret_cast<__ubuf__
        MoonEpCombineV2PeerFields *>(
            wqeContextBuf_.Get<uint8_t>().GetPhyAddr());
    fields->rowBytes = rowBytes_;
    for (uint32_t lane = 0U;
        lane < TileXRMoonEp::kMoonEpCombineV2LaneCount; ++lane) {
        const bool ready = ResolveRemoteFields(
            peer, lane, scratchOffset_, &fields->remote[lane]);
        if (kEnableSafetyChecks && !ready) {
            return false;
        }
        (void)ready;
        remoteRowBase_[lane] = fields->remote[lane].remoteRowBase;
    }
    LocalTensor<uint8_t> issue = wqeIssueBuf_.Get<uint8_t>();
#if defined(CATLASS_ARCH) && CATLASS_ARCH == 3510
    PipeBarrier<PIPE_ALL>();
    Simt::VF_CALL<MoonEpCombineV2PrefillPeerWqesVf>(
        Simt::Dim3{TileXRMoonEp::kMoonEpCombineV2BuilderThreads, 1U, 1U},
        reinterpret_cast<__ubuf__ uint8_t *>(
            issue.GetPhyAddr()),
        reinterpret_cast<__ubuf__ uint8_t *>(
            issue[kSixPortIssueBytes].GetPhyAddr()), fields);
    PipeBarrier<PIPE_ALL>();
#endif
    return true;
}

__aicore__ inline bool MoonEpCombineV2::PrefillFullmeshWqes(uint32_t peer)
{
    __ubuf__ MoonEpCombineV2PeerFields *fields = reinterpret_cast<__ubuf__
        MoonEpCombineV2PeerFields *>(
            wqeContextBuf_.Get<uint8_t>().GetPhyAddr());
    fields->rowBytes = rowBytes_;
    if (!ResolveFullmeshRemoteFields(peer, scratchOffset_,
            &fields->remote[0])) {
        return false;
    }
    remoteRowBase_[0] = fields->remote[0].remoteRowBase;
#if defined(CATLASS_ARCH) && CATLASS_ARCH == 3510
    PipeBarrier<PIPE_ALL>();
    Simt::VF_CALL<MoonEpCombineV2PrefillFullmeshWqesVf>(
        Simt::Dim3{TileXRMoonEp::kMoonEpCombineV2BuilderThreads, 1U, 1U},
        reinterpret_cast<__ubuf__ uint8_t *>(
            wqeIssueBuf_.Get<uint8_t>().GetPhyAddr()), fields);
    PipeBarrier<PIPE_ALL>();
#endif
    return true;
}

__aicore__ inline void MoonEpCombineV2::SetFailure(
    uint32_t status, uint32_t step, uint32_t peer, uint32_t lane,
    uint32_t cqStatus, uint64_t expected, uint64_t observed, uint32_t qp)
{
    if (failureStatus_ != TileXRMoonEp::MOONEP_COMBINE_V2_SUCCESS) {
        return;
    }
    failureStatus_ = status;
    failureStep_ = step;
    failurePeer_ = peer;
    failureLane_ = lane;
    failureCqStatus_ = cqStatus;
    failureQp_ = qp;
    failureExpected_ = expected;
    failureObserved_ = observed;
}

__aicore__ inline void MoonEpCombineV2::PublishFailureRecord(uint32_t stageId)
{
    const uint64_t index = TileXRMoonEp::MoonEpCombineV2FailureIndex(
        epoch_, core_);
    __gm__ TileXRMoonEp::MoonEpCombineV2FailureRecord *record =
        reinterpret_cast<__gm__
            TileXRMoonEp::MoonEpCombineV2FailureRecord *>(failureBase_ +
                index * TileXRMoonEp::kMoonEpCombineV2TokenStrideBytes);
    record->magic = magic_;
    record->status = failureStatus_;
    record->rank = rank_;
    record->core = core_;
    record->step = failureStep_ == UINT32_MAX ? stageId : failureStep_;
    record->peer = failurePeer_;
    record->lane = failureLane_;
    record->qp = failureQp_ != UINT32_MAX ? failureQp_ :
        (failureLane_ < TileXRMoonEp::kMoonEpCombineV2LaneCount ?
            TileXRMoonEp::MoonEpCombineV2Qp(core_, failureLane_) :
            UINT32_MAX);
    record->cqStatus = failureCqStatus_;
    record->expected = failureExpected_;
    record->observed = failureObserved_;
    record->poison = 1U;
    record->marker = TileXRMoonEp::kMoonEpCombineV2FailureMarker |
        (epoch_ & 1U);
    TileXR::UDMACleanCacheLines(reinterpret_cast<__gm__ uint8_t *>(record),
        sizeof(*record));
}

__aicore__ inline bool MoonEpCombineV2::CheckPoison()
{
    for (uint32_t epoch = 0U;
        epoch < TileXRMoonEp::kMoonEpCombineV2EpochCount; ++epoch) {
        for (uint32_t core = 0U;
            core < activeCoreCount_; ++core) {
            const uint64_t index =
                TileXRMoonEp::MoonEpCombineV2FailureIndex(epoch, core);
            __gm__ TileXRMoonEp::MoonEpCombineV2FailureRecord *record = reinterpret_cast<__gm__
                TileXRMoonEp::MoonEpCombineV2FailureRecord *>(failureBase_ +
                    index * TileXRMoonEp::kMoonEpCombineV2TokenStrideBytes);
            TileXR::UDMACleanCacheLines(
                reinterpret_cast<__gm__ uint8_t *>(record), sizeof(*record));
            if ((record->marker & ~1U) ==
                    TileXRMoonEp::kMoonEpCombineV2FailureMarker &&
                record->poison != 0U) {
                SetFailure(TileXRMoonEp::MOONEP_COMBINE_V2_POISONED,
                    UINT32_MAX, UINT32_MAX, UINT32_MAX, 0U,
                    record->magic, record->status);
                return false;
            }
        }
    }
    return true;
}

__aicore__ inline bool MoonEpCombineV2::InitLaneStates()
{
    __gm__ TileXR::UDMAInfo *info = TileXR::GetUDMAInfo(args_);
    if (info == nullptr || info->sqPtr == 0U || info->scqPtr == 0U ||
        info->memPtr == 0U) {
        return false;
    }
    const uint32_t firstPeer = TileXRMoonEp::MoonEpCombineV2MapSendDst(
        scheduleMode_, rank_, core_, 0U, rankSize_);
    for (uint32_t lane = 0U;
        lane < TileXRMoonEp::kMoonEpCombineV2LaneCount; ++lane) {
        MoonEpCombineV2LaneState &state = lane_[lane];
        state.qp = TileXRMoonEp::MoonEpCombineV2Qp(core_, lane);
        state.sq = TileXR::UDMAGetWQCtx(info, firstPeer, state.qp);
        state.cq = TileXR::UDMAGetSCQCtx(info, firstPeer, state.qp);
        if (state.sq == nullptr || state.cq == nullptr ||
            state.sq->baseBkShift >= 32U ||
            (1U << state.sq->baseBkShift) != kWqeBytes ||
            state.sq->depth != TileXR::TILEXR_UDMA_SQ_BB_COUNT ||
            state.sq->bufAddr == 0U || state.sq->headAddr == 0U ||
            state.sq->tailAddr == 0U || state.sq->wqeCntAddr == 0U ||
            state.sq->dbAddr == 0U ||
            state.cq->baseBkShift >= 32U ||
            (1U << state.cq->baseBkShift) !=
                sizeof(TileXR::UDMACqeCtx) ||
            state.cq->depth != TileXR::TILEXR_UDMA_CQ_DEPTH ||
            state.cq->bufAddr == 0U || state.cq->tailAddr == 0U ||
            state.cq->dbAddr == 0U) {
            return false;
        }
        state.head = ld_dev(reinterpret_cast<__gm__ uint32_t *>(
            state.sq->headAddr), 0);
        state.tail = ld_dev(reinterpret_cast<__gm__ uint32_t *>(
            state.sq->tailAddr), 0);
        state.completionCount = ld_dev(reinterpret_cast<__gm__ uint32_t *>(
            state.sq->wqeCntAddr), 0);
        state.cqTail = ld_dev(reinterpret_cast<__gm__ uint32_t *>(
            state.cq->tailAddr), 0);
        state.cqTarget = state.cqTail;
        state.submittedHead = state.head;
        if (state.head != state.tail) {
            return false;
        }
    }
    return true;
}

__aicore__ inline bool MoonEpCombineV2::InitFullmeshState(uint32_t peer)
{
    if (peer == rank_ ||
        !TileXRMoonEp::MoonEpCombineV2SameServer(
            rank_, peer, static_cast<uint32_t>(args_->localRankSize)) ||
        fullmeshInfo_ == nullptr || fullmeshInfo_->sqPtr == 0U ||
        fullmeshInfo_->scqPtr == 0U || fullmeshInfo_->memPtr == 0U) {
        return false;
    }
    if (fullmeshPeer_ == peer) {
        return fullmeshLane_.sq != nullptr && fullmeshLane_.cq != nullptr;
    }
    if (fullmeshPeer_ != TileXRMoonEp::kMoonEpCombineV2InvalidPeer &&
        (fullmeshLane_.head != fullmeshLane_.tail ||
        fullmeshLane_.cqTail != fullmeshLane_.cqTarget)) {
        return false;
    }
    const uint32_t slot = TileXRMoonEp::MoonEpCombineV2LocalSlot(
        peer, static_cast<uint32_t>(args_->localRankSize));
    if (slot >= TileXRMoonEp::kMoonEpCombineV2FullmeshSlotCount ||
        (fullmeshView_->validPeerMask & (1U << slot)) == 0U) {
        return false;
    }
    MoonEpCombineV2LaneState state {};
    state.qp = slot;
    state.sq = TileXR::UDMAGetWQCtx(fullmeshInfo_, peer, slot);
    state.cq = TileXR::UDMAGetSCQCtx(fullmeshInfo_, peer, slot);
    if (state.sq == nullptr || state.cq == nullptr ||
        state.sq->baseBkShift >= 32U ||
        (1U << state.sq->baseBkShift) != kWqeBytes ||
        state.sq->depth != TileXR::TILEXR_UDMA_SQ_BB_COUNT ||
        state.sq->bufAddr == 0U || state.sq->headAddr == 0U ||
        state.sq->tailAddr == 0U || state.sq->wqeCntAddr == 0U ||
        state.sq->dbAddr == 0U || state.cq->baseBkShift >= 32U ||
        (1U << state.cq->baseBkShift) != sizeof(TileXR::UDMACqeCtx) ||
        state.cq->depth != TileXR::TILEXR_UDMA_CQ_DEPTH ||
        state.cq->bufAddr == 0U || state.cq->tailAddr == 0U ||
        state.cq->dbAddr == 0U) {
        return false;
    }
    state.head = ld_dev(reinterpret_cast<__gm__ uint32_t *>(
        state.sq->headAddr), 0);
    state.tail = ld_dev(reinterpret_cast<__gm__ uint32_t *>(
        state.sq->tailAddr), 0);
    state.completionCount = ld_dev(reinterpret_cast<__gm__ uint32_t *>(
        state.sq->wqeCntAddr), 0);
    state.cqTail = ld_dev(reinterpret_cast<__gm__ uint32_t *>(
        state.cq->tailAddr), 0);
    state.cqTarget = state.cqTail;
    state.submittedHead = state.head;
    if (state.head != state.tail) {
        return false;
    }
    fullmeshLane_ = state;
    fullmeshPeer_ = peer;
    return true;
}

__aicore__ inline bool MoonEpCombineV2::ResolveRemoteFields(
    uint32_t targetRank, uint32_t lane, uint64_t remoteBaseOffset,
    __ubuf__ MoonEpCombineV2RemoteFields *fields)
{
    if (kEnableSafetyChecks &&
        (targetRank >= rankSize_ ||
        lane >= TileXRMoonEp::kMoonEpCombineV2LaneCount ||
        fields == nullptr ||
        !TileXR::UDMARegisteredRangeValid(registry_, targetRank,
            remoteBaseOffset, rowBytes_ * slots_))) {
        return false;
    }
    const uint32_t qp = TileXRMoonEp::MoonEpCombineV2Qp(core_, lane);
    __gm__ TileXR::UDMAMemInfo *mem = TileXR::UDMAGetRemoteMemInfo(
        TileXR::GetUDMAInfo(args_), targetRank, qp);
    const uint64_t rangeBytes = rowBytes_ * slots_;
    const uint64_t registeredBase = reinterpret_cast<uint64_t>(
        registry_->regions[targetRank].base);
    if (kEnableSafetyChecks &&
        (mem->eidAddr == 0U || !mem->tokenValueValid || mem->addr == 0U ||
        mem->addr != registeredBase || remoteBaseOffset > mem->len ||
        rangeBytes > static_cast<uint64_t>(mem->len) - remoteBaseOffset)) {
        return false;
    }
    __gm__ uint64_t *eid = reinterpret_cast<__gm__ uint64_t *>(mem->eidAddr);
    fields->remoteRowBase = reinterpret_cast<uint64_t>(
        TileXR::UDMARegisteredRemoteAddr(registry_, targetRank,
            remoteBaseOffset));
    fields->rmtEidL = eid[0];
    fields->rmtEidH = eid[1];
    fields->tokenEn = mem->tokenValueValid;
    fields->rmtJettyType = mem->rmtJettyType;
    fields->targetHint = mem->targetHint;
    fields->tpId = mem->tpn;
    fields->rmtJettyOrSegId = mem->tid;
    fields->rmtTokenValue = mem->rmtTokenValue;
    return true;
}

__aicore__ inline bool MoonEpCombineV2::ResolveFullmeshRemoteFields(
    uint32_t targetRank, uint64_t remoteBaseOffset,
    __ubuf__ MoonEpCombineV2RemoteFields *fields)
{
    const uint32_t localRankSize = static_cast<uint32_t>(
        args_->localRankSize);
    const uint32_t slot = TileXRMoonEp::MoonEpCombineV2LocalSlot(
        targetRank, localRankSize);
    const uint64_t rangeBytes = rowBytes_ * slots_;
    if (targetRank >= rankSize_ || targetRank == rank_ ||
        !TileXRMoonEp::MoonEpCombineV2SameServer(
            rank_, targetRank, localRankSize) ||
        slot >= TileXRMoonEp::kMoonEpCombineV2FullmeshSlotCount ||
        fields == nullptr ||
        !TileXR::UDMARegisteredRangeValid(registry_, targetRank,
            remoteBaseOffset, rangeBytes)) {
        return false;
    }
    __gm__ TileXR::UDMAMemInfo *mem = TileXR::UDMAGetRemoteMemInfo(
        fullmeshInfo_, targetRank, slot);
    const uint64_t registeredBase = reinterpret_cast<uint64_t>(
        registry_->regions[targetRank].base);
    if (mem == nullptr || mem->eidAddr == 0U || !mem->tokenValueValid ||
        mem->addr == 0U || mem->addr != registeredBase ||
        remoteBaseOffset > mem->len ||
        rangeBytes > static_cast<uint64_t>(mem->len) - remoteBaseOffset) {
        return false;
    }
    __gm__ uint64_t *eid = reinterpret_cast<__gm__ uint64_t *>(mem->eidAddr);
    fields->remoteRowBase = reinterpret_cast<uint64_t>(
        TileXR::UDMARegisteredRemoteAddr(
            registry_, targetRank, remoteBaseOffset));
    fields->rmtEidL = eid[0];
    fields->rmtEidH = eid[1];
    fields->tokenEn = mem->tokenValueValid;
    fields->rmtJettyType = mem->rmtJettyType;
    fields->targetHint = mem->targetHint;
    fields->tpId = mem->tpn;
    fields->rmtJettyOrSegId = mem->tid;
    fields->rmtTokenValue = mem->rmtTokenValue;
    return true;
}

__aicore__ inline bool MoonEpCombineV2::ValidateDestinations()
{
    const uint32_t rowsPerCore = static_cast<uint32_t>(CeilDiv(
        slots_, activeCoreCount_));
    const uint32_t firstSlot = core_ * rowsPerCore;
    const uint32_t rowCount = firstSlot >= slots_ ? 0U :
        static_cast<uint32_t>(slots_ - firstSlot < rowsPerCore ?
            slots_ - firstSlot : rowsPerCore);
    if (rowCount == 0U) {
        return true;
    }
    for (uint32_t validated = 0U; validated < rowCount;
        validated += TileXRMoonEp::kMoonEpCombineV2SelectionChunkRows) {
        const uint32_t chunkElements = rowCount - validated <
                TileXRMoonEp::kMoonEpCombineV2SelectionChunkRows ?
            rowCount - validated :
            TileXRMoonEp::kMoonEpCombineV2SelectionChunkRows;
        GlobalTensor<int32_t> dstGlobal;
        dstGlobal.SetGlobalBuffer(dstGlobalAddr_, slots_);
        LocalTensor<int32_t> dst = dstSlotBuf_.Get<int32_t>();
        const DataCopyExtParams params {
            1U, chunkElements * static_cast<uint32_t>(sizeof(int32_t)),
            0U, 0U, 0U};
        const DataCopyPadExtParams<int32_t> pad {false, 0U, 0U, 0U};
        if (selectionBuffersUsed_) {
            SyncFunc<HardEvent::S_MTE2>();
        }
        DataCopyPad(dst, dstGlobal[firstSlot + validated], params, pad);
        SyncFunc<HardEvent::MTE2_S>();
        selectionBuffersUsed_ = true;
        for (uint32_t localSlot = 0U; localSlot < chunkElements;
            ++localSlot) {
            const int32_t encoded = dst.GetValue(localSlot);
            if (!TileXRMoonEp::MoonEpCombineV2DestinationValid(
                    encoded, slots_, rankSize_)) {
                const uint32_t peer = encoded < 0 ? UINT32_MAX :
                    static_cast<uint32_t>(
                        static_cast<uint64_t>(encoded) / slots_);
                SetFailure(TileXRMoonEp::MOONEP_COMBINE_V2_BAD_DESTINATION,
                    UINT32_MAX, peer, UINT32_MAX,
                    firstSlot + validated + localSlot,
                    static_cast<uint64_t>(rankSize_) * slots_,
                    static_cast<uint32_t>(encoded));
                return false;
            }
        }
    }
    return true;
}

__aicore__ inline void MoonEpCombineV2::LoadSelectionChunk(
    uint64_t chunkStart, uint32_t chunkElements)
{
    if (selectionBuffersUsed_) {
        SyncFunc<HardEvent::S_MTE2>();
    }
    GlobalTensor<int32_t> dstGlobal;
    dstGlobal.SetGlobalBuffer(dstGlobalAddr_, slots_);
    LocalTensor<int32_t> dst = dstSlotBuf_.Get<int32_t>();
    const DataCopyExtParams params {
        1U, chunkElements * static_cast<uint32_t>(sizeof(int32_t)),
        0U, 0U, 0U};
    const DataCopyPadExtParams<int32_t> pad {false, 0U, 0U, 0U};
    DataCopyPad(dst, dstGlobal[chunkStart], params, pad);
    SyncFunc<HardEvent::MTE2_V>();
    selectionBuffersUsed_ = true;
}

__aicore__ inline uint32_t MoonEpCombineV2::SelectPeerIndices(
    uint32_t peer, uint32_t chunkElements)
{
    const uint64_t peerBase = static_cast<uint64_t>(peer) * slots_;
    const uint64_t peerEnd = peerBase + slots_;
    if (peerBase > static_cast<uint64_t>(INT32_MAX)) {
        return 0U;
    }
    LocalTensor<int32_t> dstSlots = dstSlotBuf_.Get<int32_t>();
    LocalTensor<uint8_t> lowerMask = lowerMaskBuf_.Get<uint8_t>();
    Compares(lowerMask, dstSlots, static_cast<int32_t>(peerBase),
        CMPMODE::GE, chunkElements);
    if (peerEnd <= static_cast<uint64_t>(INT32_MAX)) {
        LocalTensor<uint8_t> upperMask = upperMaskBuf_.Get<uint8_t>();
        Compares(upperMask, dstSlots, static_cast<int32_t>(peerEnd),
            CMPMODE::LT, chunkElements);
        PipeBarrier<PIPE_V>();
        And(lowerMask.ReinterpretCast<uint16_t>(),
            lowerMask.ReinterpretCast<uint16_t>(),
            upperMask.ReinterpretCast<uint16_t>(),
            static_cast<int32_t>(kSelectionMaskElements));
    }
    PipeBarrier<PIPE_V>();
    uint64_t selectedCount = 0U;
    GatherMask(selectedIndexBuf_.Get<int16_t>(),
        slotIndexBuf_.Get<int16_t>(), lowerMask.ReinterpretCast<uint16_t>(),
        true, chunkElements, {1U, 1U, 0U, 0U}, selectedCount);
    SyncFunc<HardEvent::V_S>();
    return static_cast<uint32_t>(selectedCount);
}

__aicore__ inline uint32_t MoonEpCombineV2::SelectPeerDestinations(
    uint32_t peer, uint32_t chunkElements,
    LocalTensor<int32_t> packedDestinations)
{
    const uint64_t peerBase = static_cast<uint64_t>(peer) * slots_;
    const uint64_t peerEnd = peerBase + slots_;
    if (peerBase > static_cast<uint64_t>(INT32_MAX)) {
        return 0U;
    }
    LocalTensor<int32_t> dstSlots = dstSlotBuf_.Get<int32_t>();
    LocalTensor<uint8_t> lowerMask = lowerMaskBuf_.Get<uint8_t>();
    Compares(lowerMask, dstSlots, static_cast<int32_t>(peerBase),
        CMPMODE::GE, chunkElements);
    if (peerEnd <= static_cast<uint64_t>(INT32_MAX)) {
        LocalTensor<uint8_t> upperMask = upperMaskBuf_.Get<uint8_t>();
        Compares(upperMask, dstSlots, static_cast<int32_t>(peerEnd),
            CMPMODE::LT, chunkElements);
        PipeBarrier<PIPE_V>();
        And(lowerMask.ReinterpretCast<uint16_t>(),
            lowerMask.ReinterpretCast<uint16_t>(),
            upperMask.ReinterpretCast<uint16_t>(),
            static_cast<int32_t>(kSelectionMaskElements));
    }
    PipeBarrier<PIPE_V>();
    uint64_t selectedCount = 0U;
    GatherMask(packedDestinations, dstSlots,
        lowerMask.ReinterpretCast<uint32_t>(), true, chunkElements,
        {1U, 1U, 0U, 0U}, selectedCount);
    SyncFunc<HardEvent::V_S>();
    return static_cast<uint32_t>(selectedCount);
}

__aicore__ inline uint64_t MoonEpCombineV2::LoadToken(
    __gm__ uint64_t *token)
{
    TileXR::UDMACleanCacheLines(
        reinterpret_cast<__gm__ uint8_t *>(token), sizeof(uint64_t));
    return *token;
}

__aicore__ inline uint32_t MoonEpCombineV2::PollCqOnce(
    MoonEpCombineV2LaneState &state)
{
    if (TileXRMoonEp::MoonEpCombineV2CqTargetReached(
            state.cqTail, state.cqTarget)) {
        return 0U;
    }
    __gm__ TileXR::UDMACqeCtx *cqe = reinterpret_cast<__gm__ TileXR::UDMACqeCtx *>(
        state.cq->bufAddr + sizeof(TileXR::UDMACqeCtx) *
            (state.cqTail % TileXR::TILEXR_UDMA_CQ_DEPTH));
    TileXR::UDMACleanCacheLines(reinterpret_cast<__gm__ uint8_t *>(cqe),
        sizeof(TileXR::UDMACqeCtx));
    uint32_t completedTail = state.tail;
    uint32_t detail = 0U;
    const uint32_t result = TileXRMoonEp::MoonEpCombineV2AdvanceSingleCqe(
        state.cqTail, TileXR::TILEXR_UDMA_CQ_DEPTH,
        static_cast<uint32_t>(cqe->owner),
        static_cast<uint32_t>(cqe->status),
        static_cast<uint32_t>(cqe->substatus), state.tail,
        state.submittedHead, static_cast<uint32_t>(cqe->entryIdx),
        TileXR::TILEXR_UDMA_SQ_BB_COUNT, completedTail, detail);
    if (result ==
        TileXRMoonEp::MOONEP_COMBINE_V2_SINGLE_CQE_NO_COMPLETION) {
        return kPollNoCompletion;
    }
    if (result == TileXRMoonEp::MOONEP_COMBINE_V2_SINGLE_CQE_ERROR) {
        return detail == 0U ? kPollInvalidState : detail;
    }
    if (result ==
        TileXRMoonEp::MOONEP_COMBINE_V2_SINGLE_CQE_INVALID_STATE) {
        return kPollInvalidState;
    }
    ++state.cqTail;
    state.tail = completedTail;
    st_dev(state.cqTail, reinterpret_cast<__gm__ uint32_t *>(
        state.cq->tailAddr), 0);
    st_dev(state.cqTail & 0xFFFFFFU,
        reinterpret_cast<__gm__ uint32_t *>(state.cq->dbAddr), 0);
    st_dev(state.tail, reinterpret_cast<__gm__ uint32_t *>(
        state.sq->tailAddr), 0);
    return TileXRMoonEp::MoonEpCombineV2CqTargetReached(
        state.cqTail, state.cqTarget) ? 0U : kPollNoCompletion;
}

__aicore__ inline bool MoonEpCombineV2::WaitStepCqs(uint32_t step)
{
    bool ready[2] = {
        TileXRMoonEp::MoonEpCombineV2CqTargetReached(
            lane_[0].cqTail, lane_[0].cqTarget),
        TileXRMoonEp::MoonEpCombineV2CqTargetReached(
            lane_[1].cqTail, lane_[1].cqTarget)};
    uint32_t cursor = 0U;
    while (!(ready[0] && ready[1])) {
        for (uint32_t offset = 0U; offset < 2U; ++offset) {
            const uint32_t lane = (cursor + offset) & 1U;
            if (ready[lane]) {
                continue;
            }
            const uint32_t status = PollCqOnce(lane_[lane]);
            if (status == 0U) {
                ready[lane] = true;
            } else if (status != kPollNoCompletion) {
                SetFailure(TileXRMoonEp::MOONEP_COMBINE_V2_CQ_ERROR,
                    step, UINT32_MAX, lane, status);
                return false;
            }
        }
        cursor ^= 1U;
        if (TimedOut(operationStartCycles_)) {
            for (uint32_t lane = 0U; lane < 2U; ++lane) {
                if (!ready[lane]) {
                    SetFailure(TileXRMoonEp::MOONEP_COMBINE_V2_CQ_TIMEOUT,
                        step, UINT32_MAX, lane);
                    return false;
                }
            }
        }
    }
    for (uint32_t lane = 0U;
        lane < TileXRMoonEp::kMoonEpCombineV2LaneCount; ++lane) {
        MoonEpCombineV2LaneState &state = lane_[lane];
        if (state.cqTail != state.cqTarget ||
            state.tail != state.submittedHead || state.head != state.tail) {
            SetFailure(TileXRMoonEp::MOONEP_COMBINE_V2_CQ_ERROR,
                step, UINT32_MAX, lane, kPollInvalidState,
                state.submittedHead, state.tail, state.qp);
            return false;
        }
    }
    return true;
}

__aicore__ inline bool MoonEpCombineV2::WaitFullmeshCq(
    uint32_t step, uint32_t peer)
{
    const uint32_t logicalQp =
        TileXRMoonEp::MoonEpCombineV2FullmeshLogicalQp(
            peer, static_cast<uint32_t>(args_->localRankSize));
    while (!TileXRMoonEp::MoonEpCombineV2CqTargetReached(
        fullmeshLane_.cqTail, fullmeshLane_.cqTarget)) {
        const uint32_t status = PollCqOnce(fullmeshLane_);
        if (status != 0U && status != kPollNoCompletion) {
            SetFailure(TileXRMoonEp::MOONEP_COMBINE_V2_CQ_ERROR,
                step, peer, TileXRMoonEp::MOONEP_COMBINE_V2_FULLMESH,
                status, fullmeshLane_.cqTarget, fullmeshLane_.cqTail,
                logicalQp);
            return false;
        }
        if (TimedOut(operationStartCycles_)) {
            SetFailure(TileXRMoonEp::MOONEP_COMBINE_V2_CQ_TIMEOUT,
                step, peer, TileXRMoonEp::MOONEP_COMBINE_V2_FULLMESH,
                0U, fullmeshLane_.cqTarget, fullmeshLane_.cqTail,
                logicalQp);
            return false;
        }
    }
    if (fullmeshLane_.tail != fullmeshLane_.submittedHead ||
        fullmeshLane_.head != fullmeshLane_.tail) {
        SetFailure(TileXRMoonEp::MOONEP_COMBINE_V2_CQ_ERROR,
            step, peer, TileXRMoonEp::MOONEP_COMBINE_V2_FULLMESH,
            kPollInvalidState, fullmeshLane_.submittedHead,
            fullmeshLane_.tail, logicalQp);
        return false;
    }
    return true;
}

__aicore__ inline bool MoonEpCombineV2::WaitStepCredit(uint32_t step,
    const TileXRMoonEp::MoonEpCombineV2StepRoute &route)
{
    if (!route.waitCredit) {
        return true;
    }
    const uint32_t sourceRank = route.creditWaitSrcRank;
    const uint32_t sourceCore = route.creditWaitSrcCore;
    const uint64_t receiveOffset =
        TileXRMoonEp::MoonEpCombineV2CreditReceiveOffset(
            epoch_, step, core_);
    if (sourceRank >= rankSize_ || sourceCore >= activeCoreCount_ ||
        receiveOffset == UINT64_MAX || args_->creditMems[rank_] == nullptr) {
        SetFailure(TileXRMoonEp::MOONEP_COMBINE_V2_INVALID_CONFIG,
            step, sourceRank, UINT32_MAX);

        return false;
    }

    __gm__ uint64_t *credit = reinterpret_cast<__gm__ uint64_t *>(
        args_->creditMems[rank_] + receiveOffset);
    GlobalTensor<uint64_t> creditGlobal;
    creditGlobal.SetGlobalBuffer(credit,
        TileXRMoonEp::kMoonEpCombineV2CreditSignalBytes / sizeof(uint64_t));
    LocalTensor<uint64_t> creditLocal =
        wqeIssueBuf_.Get<uint8_t>().ReinterpretCast<uint64_t>();
    const uint64_t expectedGuard = TileXRMoonEp::MoonEpCombineV2CreditGuard(
        magic_, step, sourceRank, sourceCore, rank_, core_);
    uint64_t observedMagic = 0U;
    while (true) {
        TileXR::UDMACleanCacheLines(
            reinterpret_cast<__gm__ uint8_t *>(credit),
            TileXRMoonEp::kMoonEpCombineV2CreditSignalBytes);
        PipeBarrier<PIPE_ALL>();
        SyncFunc<HardEvent::S_MTE2>();
        DataCopy(creditLocal, creditGlobal,
            TileXRMoonEp::kMoonEpCombineV2CreditSignalBytes /
                sizeof(uint64_t));
        SyncFunc<HardEvent::MTE2_S>();
        observedMagic = creditLocal.GetValue(0U);
        if (observedMagic == magic_ &&
            creditLocal.GetValue(1U) == expectedGuard &&
            creditLocal.GetValue(2U) ==
                (static_cast<uint64_t>(step) << 32U |
                    TileXRMoonEp::kMoonEpCombineV2CreditMarker) &&
            creditLocal.GetValue(3U) ==
                (static_cast<uint64_t>(sourceCore) << 32U | sourceRank) &&
            creditLocal.GetValue(4U) ==
                (static_cast<uint64_t>(core_) << 32U | rank_)) {
            return true;
        }
        if (TimedOut(operationStartCycles_)) {
            SetFailure(TileXRMoonEp::MOONEP_COMBINE_V2_CREDIT_TIMEOUT,
                step, sourceRank, UINT32_MAX, 0U, magic_, observedMagic);

            return false;
        }
    }
}

__aicore__ inline bool MoonEpCombineV2::PublishNextCredit(uint32_t step,
    const TileXRMoonEp::MoonEpCombineV2StepRoute &route)
{
    if (!route.publishCredit) {
        return true;
    }
    const uint32_t transitionStep = step + 1U;
    const uint32_t targetRank = route.creditPublishDstRank;
    const uint32_t targetCore = route.creditPublishDstCore;
    const uint64_t receiveOffset =
        TileXRMoonEp::MoonEpCombineV2CreditReceiveOffset(
            epoch_, transitionStep, targetCore);
    if (targetRank >= rankSize_ || targetCore >= activeCoreCount_ ||
        receiveOffset == UINT64_MAX || args_->creditMems[targetRank] == nullptr) {
        SetFailure(TileXRMoonEp::MOONEP_COMBINE_V2_INVALID_CONFIG,
            step, targetRank, UINT32_MAX);

        return false;
    }

    LocalTensor<uint64_t> creditLocal =
        wqeIssueBuf_.Get<uint8_t>().ReinterpretCast<uint64_t>();
    creditLocal.SetValue(0U, magic_);
    creditLocal.SetValue(1U, TileXRMoonEp::MoonEpCombineV2CreditGuard(
        magic_, transitionStep, rank_, core_, targetRank, targetCore));
    creditLocal.SetValue(2U,
        static_cast<uint64_t>(transitionStep) << 32U |
            TileXRMoonEp::kMoonEpCombineV2CreditMarker);
    creditLocal.SetValue(3U,
        static_cast<uint64_t>(core_) << 32U | rank_);
    creditLocal.SetValue(4U,
        static_cast<uint64_t>(targetCore) << 32U | targetRank);
    for (uint32_t word = 5U; word < 8U; ++word) {
        creditLocal.SetValue(word, 0U);
    }
    GlobalTensor<uint64_t> remoteCreditGlobal;
    remoteCreditGlobal.SetGlobalBuffer(
        reinterpret_cast<__gm__ uint64_t *>(
            args_->creditMems[targetRank] + receiveOffset),
        TileXRMoonEp::kMoonEpCombineV2CreditSignalBytes / sizeof(uint64_t));
    SyncFunc<HardEvent::S_MTE3>();
    DataCopy(remoteCreditGlobal, creditLocal,
        TileXRMoonEp::kMoonEpCombineV2CreditSignalBytes / sizeof(uint64_t));
    SyncFunc<HardEvent::MTE3_S>();

    return true;
}

__aicore__ inline bool MoonEpCombineV2::BuildPayloadWqes(
    uint32_t chunkStart, uint32_t batchOffset, uint32_t batchCount,
    uint32_t sequenceBase, uint64_t peerBase)
{
    LocalTensor<uint8_t> contextTensor = wqeContextBuf_.Get<uint8_t>();
    __ubuf__ MoonEpCombineV2BuildContext *context = reinterpret_cast<__ubuf__
        MoonEpCombineV2BuildContext *>(contextTensor.GetPhyAddr());
    context->localRowBase = reinterpret_cast<uint64_t>(workspace_);
    context->rowBytes = rowBytes_;
    context->peerBase = peerBase;
    context->chunkStart = chunkStart;
    context->batchOffset = batchOffset;
    context->batchCount = batchCount;
    context->sequencePhase = sequenceBase & 3U;
    for (uint32_t lane = 0U; lane < 2U; ++lane) {
        context->head[lane] = lane_[lane].head;
        context->remoteRowBase[lane] = remoteRowBase_[lane];
    }
    LocalTensor<uint8_t> issue = wqeIssueBuf_.Get<uint8_t>();
#if defined(CATLASS_ARCH) && CATLASS_ARCH == 3510
    PipeBarrier<PIPE_ALL>();
    Simt::VF_CALL<MoonEpCombineV2BuildPayloadWqesVf>(
        Simt::Dim3{TileXRMoonEp::kMoonEpCombineV2BuilderThreads, 1U, 1U},
        reinterpret_cast<__ubuf__ uint8_t *>(
            issue.GetPhyAddr()),
        reinterpret_cast<__ubuf__ uint8_t *>(
            issue[kSixPortIssueBytes].GetPhyAddr()),
        reinterpret_cast<__ubuf__ const int32_t *>(
            dstSlotBuf_.Get<int32_t>().GetPhyAddr()),
        reinterpret_cast<__ubuf__ const int16_t *>(
            selectedIndexBuf_.Get<int16_t>().GetPhyAddr()), context);
    PipeBarrier<PIPE_ALL>();
#endif
    return true;
}

__aicore__ inline bool MoonEpCombineV2::BuildFullmeshPayloadWqes(
    uint32_t chunkStart, uint32_t batchOffset, uint32_t batchCount,
    uint64_t peerBase)
{
    __ubuf__ MoonEpCombineV2BuildContext *context = reinterpret_cast<__ubuf__
        MoonEpCombineV2BuildContext *>(
            wqeContextBuf_.Get<uint8_t>().GetPhyAddr());
    context->localRowBase = reinterpret_cast<uint64_t>(workspace_);
    context->rowBytes = rowBytes_;
    context->remoteRowBase[0] = remoteRowBase_[0];
    context->peerBase = peerBase;
    context->chunkStart = chunkStart;
    context->batchOffset = batchOffset;
    context->batchCount = batchCount;
    context->head[0] = fullmeshLane_.head;
#if defined(CATLASS_ARCH) && CATLASS_ARCH == 3510
    PipeBarrier<PIPE_ALL>();
    Simt::VF_CALL<MoonEpCombineV2BuildFullmeshPayloadWqesVf>(
        Simt::Dim3{TileXRMoonEp::kMoonEpCombineV2BuilderThreads, 1U, 1U},
        reinterpret_cast<__ubuf__ uint8_t *>(
            wqeIssueBuf_.Get<uint8_t>().GetPhyAddr()),
        reinterpret_cast<__ubuf__ const int32_t *>(
            dstSlotBuf_.Get<int32_t>().GetPhyAddr()),
        reinterpret_cast<__ubuf__ const int16_t *>(
            selectedIndexBuf_.Get<int16_t>().GetPhyAddr()), context);
    PipeBarrier<PIPE_ALL>();
#endif
    return true;
}

__aicore__ inline bool MoonEpCombineV2::AppendControlWqe(
    LocalTensor<uint8_t> issue, uint32_t outputIndex,
    MoonEpCombineV2LaneState &state, __gm__ TileXR::UDMAInfo *info,
    uint32_t targetRank,
    uint64_t remoteOffset, __gm__ uint64_t *localSource, uint32_t flag,
    uint32_t transferBytes)
{
    if (info == nullptr || transferBytes == 0U ||
        outputIndex >= kTotalIssueCapacity ||
        !TileXR::UDMARegisteredRangeValid(registry_, targetRank,
            remoteOffset, transferBytes)) {
        return false;
    }
    __gm__ TileXR::UDMAMemInfo *mem = TileXR::UDMAGetRemoteMemInfo(
        info, targetRank, state.qp);
    const uint64_t registeredBase = reinterpret_cast<uint64_t>(
        registry_->regions[targetRank].base);
    if (localSource == nullptr || mem == nullptr || mem->eidAddr == 0U ||
        !mem->tokenValueValid || mem->addr == 0U ||
        mem->addr != registeredBase || remoteOffset > mem->len ||
        transferBytes > static_cast<uint64_t>(mem->len) - remoteOffset) {
        return false;
    }
    __gm__ uint64_t *eid = reinterpret_cast<__gm__ uint64_t *>(mem->eidAddr);
    __ubuf__ uint8_t *wqe = reinterpret_cast<__ubuf__ uint8_t *>(
        issue.GetPhyAddr()) + outputIndex * kWqeBytes;
    __ubuf__ uint32_t *words = reinterpret_cast<__ubuf__ uint32_t *>(wqe);
    for (uint32_t word = 0U; word < kWqeBytes / sizeof(uint32_t); ++word) {
        words[word] = 0U;
    }
    const uint32_t absoluteHead = state.head + outputIndex;
    __ubuf__ TileXR::UDMASqeCtx *sqe =
        reinterpret_cast<__ubuf__ TileXR::UDMASqeCtx *>(wqe);
    sqe->opcode = static_cast<uint32_t>(TileXR::UDMAOpcode::WRITE);
    sqe->flag = flag;
    sqe->nf = 0U;
    sqe->tokenEn = mem->tokenValueValid;
    sqe->rmtJettyType = mem->rmtJettyType;
    sqe->sqeBbIdx = static_cast<uint16_t>(
        absoluteHead % TileXR::TILEXR_UDMA_SQ_BB_COUNT);
    sqe->owner =
        (absoluteHead & TileXR::TILEXR_UDMA_SQ_BB_COUNT) == 0U ? 1U : 0U;
    sqe->targetHint = mem->targetHint;
    sqe->inlineMsgLen = 0U;
    sqe->tpId = mem->tpn;
    sqe->sgeNum = 1U;
    sqe->rmtJettyOrSegId = mem->tid;
    sqe->rmtTokenValue = mem->rmtTokenValue;
    sqe->rmtEidL = eid[0];
    sqe->rmtEidH = eid[1];
    const uint64_t remoteAddr = reinterpret_cast<uint64_t>(
        TileXR::UDMARegisteredRemoteAddr(registry_, targetRank, remoteOffset));
    sqe->rmtAddrLOrTokenId = remoteAddr & 0xFFFFFFFFU;
    sqe->rmtAddrHOrTokenValue = (remoteAddr >> 32U) & 0xFFFFFFFFU;
    __ubuf__ TileXR::UDMASgeCtx *sge =
        reinterpret_cast<__ubuf__ TileXR::UDMASgeCtx *>(
            wqe + sizeof(TileXR::UDMASqeCtx));
    sge->len = transferBytes;
    sge->tokenId = 0U;
    sge->va = reinterpret_cast<uint64_t>(localSource);
    return true;
}
__aicore__ inline void MoonEpCombineV2::CopyIssueToSq(
    LocalTensor<uint8_t> issue, MoonEpCombineV2LaneState &state,
    uint32_t count)
{
    const TileXRMoonEp::MoonEpCombineV2RingSegments segments =
        TileXRMoonEp::MoonEpCombineV2SplitRingCopy(
            state.head, count, TileXR::TILEXR_UDMA_SQ_BB_COUNT);
    const uint32_t ringIndex =
        state.head % TileXR::TILEXR_UDMA_SQ_BB_COUNT;
    const uint32_t firstCount = segments.first;
    GlobalTensor<uint8_t> firstDst;
    firstDst.SetGlobalBuffer(reinterpret_cast<__gm__ uint8_t *>(
        state.sq->bufAddr + static_cast<uint64_t>(ringIndex) * kWqeBytes));
    const DataCopyExtParams firstParams {
        1U, firstCount * kWqeBytes, 0U, 0U, 0U};
    DataCopyPad(firstDst, issue, firstParams);
    const uint32_t secondCount = segments.second;
    if (secondCount != 0U) {
        GlobalTensor<uint8_t> secondDst;
        secondDst.SetGlobalBuffer(
            reinterpret_cast<__gm__ uint8_t *>(state.sq->bufAddr));
        const DataCopyExtParams secondParams {
            1U, secondCount * kWqeBytes, 0U, 0U, 0U};
        DataCopyPad(secondDst, issue[firstCount * kWqeBytes], secondParams);
    }
}

__aicore__ inline void MoonEpCombineV2::PublishControlToken(
    __gm__ uint64_t *destination, uint64_t value)
{
    LocalTensor<uint64_t> token = wqeContextBuf_.Get<uint64_t>();
    token.SetValue(0U, value);
    for (uint32_t word = 1U;
        word < TileXRMoonEp::kMoonEpCombineV2TokenStrideBytes /
                sizeof(uint64_t);
        ++word) {
        token.SetValue(word, 0U);
    }
    GlobalTensor<uint8_t> destinationGlobal;
    destinationGlobal.SetGlobalBuffer(
        reinterpret_cast<__gm__ uint8_t *>(destination));
    const DataCopyExtParams copyOut {1U,
        static_cast<uint32_t>(
            TileXRMoonEp::kMoonEpCombineV2TokenStrideBytes),
        0U, 0U, 0U};
    SyncFunc<HardEvent::S_MTE3>();
    DataCopyPad(destinationGlobal, token.ReinterpretCast<uint8_t>(), copyOut);
    SyncFunc<HardEvent::MTE3_S>();
}

__aicore__ inline bool MoonEpCombineV2::SubmitPair(
    uint32_t peer, uint32_t step, uint32_t chunkStart,
    uint32_t batchOffset, uint32_t batchCount, uint32_t sequenceBase,
    uint64_t peerBase, bool finalBatch)
{
    const bool payloadReady = BuildPayloadWqes(chunkStart, batchOffset,
        batchCount, sequenceBase, peerBase);

    if (kEnableSafetyChecks && !payloadReady) {
        SetFailure(TileXRMoonEp::MOONEP_COMBINE_V2_INVALID_CONFIG,
            step, peer, UINT32_MAX);
        return false;
    }
    (void)payloadReady;
    const uint32_t phase = sequenceBase & 3U;
    const TileXRMoonEp::MoonEpCombineV2LaneCounts batchCounts =
        TileXRMoonEp::MoonEpCombineV2BatchLaneCounts(
            batchCount, phase, step, stepCount_, false);
    uint32_t count[2] = {batchCounts.sixPort, batchCounts.twoPort};
    LocalTensor<uint8_t> allIssue = wqeIssueBuf_.Get<uint8_t>();
    LocalTensor<uint8_t> laneIssue[2] = {
        allIssue,
        allIssue[kSixPortIssueBytes]
    };
    if (finalBatch) {
        for (uint32_t lane = 0U; lane < 2U; ++lane) {
            LocalTensor<uint8_t> issue = laneIssue[lane];
            __gm__ uint64_t *doneSource = reinterpret_cast<__gm__ uint64_t *>(
                controlSourceBase_ + static_cast<uint64_t>(lane) *
                    TileXRMoonEp::kMoonEpCombineV2TokenStrideBytes);
            PublishControlToken(doneSource,
                TileXRMoonEp::MoonEpCombineV2Token(magic_, step));
            const uint64_t doneIndex =
                TileXRMoonEp::MoonEpCombineV2DoneIndex(
                    epoch_, rank_, lane);
            const bool doneAppended = AppendControlWqe(
                issue, count[lane], lane_[lane],
                TileXR::GetUDMAInfo(args_), peer,
                doneOffset_ + doneIndex *
                        TileXRMoonEp::kMoonEpCombineV2TokenStrideBytes,
                doneSource,
                TileXR::TILEXR_UDMA_SQE_FLAG_ORDERED_COMPLETION);
            if (!doneAppended) {
                SetFailure(
                    TileXRMoonEp::MOONEP_COMBINE_V2_INVALID_CONFIG,
                    step, peer, lane);

                return false;
            }
            (void)doneAppended;
            ++count[lane];
        }
    }

    for (uint32_t lane = 0U; lane < 2U; ++lane) {
        const uint32_t outstanding = lane_[lane].head - lane_[lane].tail;
        if (kEnableSafetyChecks &&
            (static_cast<uint64_t>(outstanding) + count[lane] >=
                TileXRMoonEp::kMoonEpCombineV2MaxOutstanding)) {
            SetFailure(
                TileXRMoonEp::MOONEP_COMBINE_V2_OUTSTANDING_LIMIT,
                step, peer, lane, outstanding + count[lane]);

            return false;
        }
    }

    SyncFunc<HardEvent::S_MTE3>();
    if (count[0] != 0U) {
        CopyIssueToSq(laneIssue[0], lane_[0], count[0]);
    }
    if (count[1] != 0U) {
        CopyIssueToSq(laneIssue[1], lane_[1], count[1]);
    }
    SyncFunc<HardEvent::MTE3_S>();
    for (uint32_t lane = 0U; lane < 2U; ++lane) {
        lane_[lane].head += count[lane];
        lane_[lane].completionCount +=
            TileXRMoonEp::MoonEpCombineV2CompletionCount(finalBatch);
        st_dev(lane_[lane].head, reinterpret_cast<__gm__ uint32_t *>(
            lane_[lane].sq->headAddr), 0);
        st_dev(lane_[lane].completionCount,
            reinterpret_cast<__gm__ uint32_t *>(
            lane_[lane].sq->wqeCntAddr), 0);
    }
    if (count[0] != 0U) {
        st_dev(lane_[0].head, reinterpret_cast<__gm__ uint32_t *>(
            lane_[0].sq->dbAddr), 0);
    }
    if (count[1] != 0U) {
        st_dev(lane_[1].head, reinterpret_cast<__gm__ uint32_t *>(
            lane_[1].sq->dbAddr), 0);
    }
    if (finalBatch) {
        for (uint32_t lane = 0U; lane < 2U; ++lane) {
            lane_[lane].submittedHead = lane_[lane].head;
            lane_[lane].cqTarget =
                TileXRMoonEp::MoonEpCombineV2NextCqTarget(
                    lane_[lane].cqTail, true);
        }
    }
    issuedRows_ += batchCount;

    return true;
}

__aicore__ inline bool MoonEpCombineV2::SendRemoteStep(
    uint32_t peer, uint32_t step)
{
    issuedRows_ = 0U;
    selectedPeerRows_ = 0U;
    const uint64_t peerBase = static_cast<uint64_t>(peer) * slots_;
    const bool prefilled = PrefillPeerWqes(peer);
    if (kEnableSafetyChecks && !prefilled) {
        SetFailure(TileXRMoonEp::MOONEP_COMBINE_V2_INVALID_CONFIG,
            step, peer, UINT32_MAX);
        return false;
    }
    (void)prefilled;
    for (uint64_t chunkStart = 0U; chunkStart < slots_;
        chunkStart += TileXRMoonEp::kMoonEpCombineV2SelectionChunkRows) {
        const uint32_t chunkElements = static_cast<uint32_t>(
            slots_ - chunkStart <
                TileXRMoonEp::kMoonEpCombineV2SelectionChunkRows ?
            slots_ - chunkStart :
            TileXRMoonEp::kMoonEpCombineV2SelectionChunkRows);
        LoadSelectionChunk(chunkStart, chunkElements);

        const uint32_t selectedCount = SelectPeerIndices(peer, chunkElements);

        const bool lastChunk = chunkStart + chunkElements == slots_;
        if (selectedCount == 0U && lastChunk) {
            const bool submitted = SubmitPair(peer, step,
                static_cast<uint32_t>(chunkStart), 0U, 0U, issuedRows_,
                peerBase, true);
            if (kEnableSafetyChecks && !submitted) {
                return false;
            }
            (void)submitted;
        }
        for (uint32_t batchOffset = 0U; batchOffset < selectedCount;
            batchOffset += TileXRMoonEp::kMoonEpCombineV2PayloadBatchRows) {
            const uint32_t remaining = selectedCount - batchOffset;
            const uint32_t batchCount = remaining <
                    TileXRMoonEp::kMoonEpCombineV2PayloadBatchRows ?
                remaining : TileXRMoonEp::kMoonEpCombineV2PayloadBatchRows;
            const bool finalBatch = lastChunk &&
                batchOffset + batchCount == selectedCount;
            const bool submitted = SubmitPair(peer, step,
                static_cast<uint32_t>(chunkStart), batchOffset, batchCount,
                issuedRows_, peerBase, finalBatch);
            if (kEnableSafetyChecks && !submitted) {
                return false;
            }
            (void)submitted;
            selectedPeerRows_ += batchCount;
            if (kEnableSafetyChecks && selectedPeerRows_ >
                TileXRMoonEp::kMoonEpCombineV2MaxOutstanding) {
                SetFailure(TileXRMoonEp::MOONEP_COMBINE_V2_OUTSTANDING_LIMIT,
                    step, peer, UINT32_MAX, selectedPeerRows_);
                return false;
            }
        }
    }
    return true;
}

__aicore__ inline bool MoonEpCombineV2::SubmitFullmeshBatch(
    uint32_t peer, uint32_t step, uint32_t chunkStart,
    uint32_t batchOffset, uint32_t batchCount, uint64_t peerBase,
    bool finalBatch)
{
    if (batchCount > TileXRMoonEp::kMoonEpCombineV2PayloadBatchRows ||
        !BuildFullmeshPayloadWqes(
            chunkStart, batchOffset, batchCount, peerBase)) {
        SetFailure(TileXRMoonEp::MOONEP_COMBINE_V2_INVALID_CONFIG,
            step, peer, TileXRMoonEp::MOONEP_COMBINE_V2_FULLMESH, 0U,
            0U, 0U, TileXRMoonEp::MoonEpCombineV2FullmeshLogicalQp(
                peer, static_cast<uint32_t>(args_->localRankSize)));
        return false;
    }
    uint32_t count = batchCount;
    LocalTensor<uint8_t> issue = wqeIssueBuf_.Get<uint8_t>();
    if (finalBatch) {
        __gm__ uint64_t *doneSource = reinterpret_cast<__gm__ uint64_t *>(
            controlSourceBase_);
        PublishControlToken(doneSource,
            TileXRMoonEp::MoonEpCombineV2Token(magic_, step));
        const uint64_t doneIndex = TileXRMoonEp::MoonEpCombineV2DoneIndex(
            epoch_, rank_, TileXRMoonEp::MOONEP_COMBINE_V2_SIX_PORT);
        if (!AppendControlWqe(issue, count, fullmeshLane_, fullmeshInfo_,
                peer, doneOffset_ + doneIndex *
                    TileXRMoonEp::kMoonEpCombineV2TokenStrideBytes,
                doneSource,
                TileXR::TILEXR_UDMA_SQE_FLAG_ORDERED_COMPLETION)) {
            SetFailure(TileXRMoonEp::MOONEP_COMBINE_V2_INVALID_CONFIG,
                step, peer, TileXRMoonEp::MOONEP_COMBINE_V2_FULLMESH,
                0U, 0U, 0U,
                TileXRMoonEp::MoonEpCombineV2FullmeshLogicalQp(
                    peer, static_cast<uint32_t>(args_->localRankSize)));
            return false;
        }
        ++count;
    }
    const uint32_t outstanding =
        fullmeshLane_.head - fullmeshLane_.tail;
    const bool completionBatch = finalBatch ||
        (count != 0U && static_cast<uint64_t>(outstanding) + count >=
            TileXRMoonEp::kMoonEpCombineV2MaxOutstanding -
                TileXRMoonEp::kMoonEpCombineV2PayloadBatchRows);
    if (completionBatch && !finalBatch) {
        __ubuf__ TileXR::UDMASqeCtx *last = reinterpret_cast<__ubuf__
            TileXR::UDMASqeCtx *>(issue[(count - 1U) * kWqeBytes].GetPhyAddr());
        last->flag = TileXR::TILEXR_UDMA_SQE_FLAG_ORDERED_COMPLETION;
    }
    if (count == 0U || static_cast<uint64_t>(outstanding) + count >=
        TileXRMoonEp::kMoonEpCombineV2MaxOutstanding) {
        SetFailure(TileXRMoonEp::MOONEP_COMBINE_V2_OUTSTANDING_LIMIT,
            step, peer, TileXRMoonEp::MOONEP_COMBINE_V2_FULLMESH,
            outstanding + count, 0U, 0U,
            TileXRMoonEp::MoonEpCombineV2FullmeshLogicalQp(
                peer, static_cast<uint32_t>(args_->localRankSize)));
        return false;
    }

    SyncFunc<HardEvent::S_MTE3>();
    CopyIssueToSq(issue, fullmeshLane_, count);
    SyncFunc<HardEvent::MTE3_S>();
    fullmeshLane_.head += count;
    if (completionBatch) {
        ++fullmeshLane_.completionCount;
        fullmeshLane_.submittedHead = fullmeshLane_.head;
        fullmeshLane_.cqTarget =
            TileXRMoonEp::MoonEpCombineV2NextCqTarget(
                fullmeshLane_.cqTail, true);
    }
    st_dev(fullmeshLane_.head, reinterpret_cast<__gm__ uint32_t *>(
        fullmeshLane_.sq->headAddr), 0);
    st_dev(fullmeshLane_.completionCount,
        reinterpret_cast<__gm__ uint32_t *>(
            fullmeshLane_.sq->wqeCntAddr), 0);
    st_dev(fullmeshLane_.head, reinterpret_cast<__gm__ uint32_t *>(
        fullmeshLane_.sq->dbAddr), 0);
    issuedRows_ += batchCount;
    if (completionBatch && !finalBatch &&
        !WaitFullmeshCq(step, peer)) {
        return false;
    }
    return true;
}

__aicore__ inline bool MoonEpCombineV2::SendFullmeshStep(
    uint32_t peer, uint32_t step)
{
    issuedRows_ = 0U;
    selectedPeerRows_ = 0U;
    if (!InitFullmeshState(peer) || !PrefillFullmeshWqes(peer)) {
        SetFailure(TileXRMoonEp::MOONEP_COMBINE_V2_INVALID_CONFIG,
            step, peer, TileXRMoonEp::MOONEP_COMBINE_V2_FULLMESH,
            0U, 0U, 0U,
            TileXRMoonEp::MoonEpCombineV2FullmeshLogicalQp(
                peer, static_cast<uint32_t>(args_->localRankSize)));
        return false;
    }
    const uint64_t peerBase = static_cast<uint64_t>(peer) * slots_;
    for (uint64_t chunkStart = 0U; chunkStart < slots_;
        chunkStart += TileXRMoonEp::kMoonEpCombineV2SelectionChunkRows) {
        const uint32_t chunkElements = static_cast<uint32_t>(
            slots_ - chunkStart <
                    TileXRMoonEp::kMoonEpCombineV2SelectionChunkRows ?
                slots_ - chunkStart :
                TileXRMoonEp::kMoonEpCombineV2SelectionChunkRows);
        LoadSelectionChunk(chunkStart, chunkElements);
        const uint32_t selectedCount = SelectPeerIndices(peer, chunkElements);
        const bool lastChunk = chunkStart + chunkElements == slots_;
        if (selectedCount == 0U && lastChunk &&
            !SubmitFullmeshBatch(peer, step,
                static_cast<uint32_t>(chunkStart), 0U, 0U,
                peerBase, true)) {
            return false;
        }
        for (uint32_t batchOffset = 0U; batchOffset < selectedCount;
            batchOffset += TileXRMoonEp::kMoonEpCombineV2PayloadBatchRows) {
            const uint32_t remaining = selectedCount - batchOffset;
            const uint32_t batchCount = remaining <
                    TileXRMoonEp::kMoonEpCombineV2PayloadBatchRows ?
                remaining : TileXRMoonEp::kMoonEpCombineV2PayloadBatchRows;
            const bool finalBatch = lastChunk &&
                batchOffset + batchCount == selectedCount;
            if (!SubmitFullmeshBatch(peer, step,
                    static_cast<uint32_t>(chunkStart), batchOffset,
                    batchCount, peerBase, finalBatch)) {
                return false;
            }
            selectedPeerRows_ += batchCount;
        }
    }
    return true;
}

__aicore__ inline void MoonEpCombineV2::CopySelfRowsIn(
    uint32_t selectedStart, uint32_t selectedCount, uint32_t chunkStart,
    uint32_t localRowStride, LocalTensor<uint8_t> relay)
{
    LocalTensor<int16_t> selectedIndices =
        selectedIndexBuf_.Get<int16_t>();
    const uint32_t copyBytes = static_cast<uint32_t>(rowBytes_);
    const DataCopyExtParams copyIn {1U, copyBytes, 0U, 0U, 0U};
    const DataCopyPadExtParams<uint8_t> padIn {false, 0U, 0U, 0U};
    for (uint32_t row = 0U; row < selectedCount; ++row) {
        const uint32_t relativeIndex = static_cast<uint16_t>(
            selectedIndices.GetValue(selectedStart + row));
        const uint32_t sourceSlotIndex = chunkStart + relativeIndex;
        GlobalTensor<uint8_t> source;
        source.SetGlobalBuffer(workspace_ +
            static_cast<uint64_t>(sourceSlotIndex) * rowBytes_);
        DataCopyPad(relay[row * localRowStride], source, copyIn, padIn);
    }
}

__aicore__ inline void MoonEpCombineV2::CopySelfRowsOut(
    uint32_t selectedStart, uint32_t selectedCount, uint64_t peerBase,
    uint32_t localRowStride, LocalTensor<uint8_t> relay)
{
    LocalTensor<int16_t> selectedIndices =
        selectedIndexBuf_.Get<int16_t>();
    LocalTensor<int32_t> dstSlots = dstSlotBuf_.Get<int32_t>();
    const uint32_t copyBytes = static_cast<uint32_t>(rowBytes_);
    const DataCopyExtParams copyOut {1U, copyBytes, 0U, 0U, 0U};
    for (uint32_t row = 0U; row < selectedCount; ++row) {
        const uint32_t relativeIndex = static_cast<uint16_t>(
            selectedIndices.GetValue(selectedStart + row));
        const uint64_t encoded = static_cast<uint64_t>(
            dstSlots.GetValue(relativeIndex));
        const uint32_t targetSlot = static_cast<uint32_t>(
            encoded - peerBase);
        GlobalTensor<uint8_t> target;
        target.SetGlobalBuffer(scratch_ +
            static_cast<uint64_t>(targetSlot) * rowBytes_);
        DataCopyPad(target, relay[row * localRowStride], copyOut);
    }
}

__aicore__ inline void MoonEpCombineV2::CopySelfTileIn(
    uint32_t selectedIndex, uint32_t chunkStart, uint64_t rowOffset,
    uint32_t tileBytes, LocalTensor<uint8_t> relay)
{
    const uint32_t relativeIndex = static_cast<uint16_t>(
        selectedIndexBuf_.Get<int16_t>().GetValue(selectedIndex));
    const uint32_t sourceSlotIndex = chunkStart + relativeIndex;
    GlobalTensor<uint8_t> source;
    source.SetGlobalBuffer(workspace_ +
        static_cast<uint64_t>(sourceSlotIndex) * rowBytes_ + rowOffset);
    const DataCopyExtParams copyIn {1U, tileBytes, 0U, 0U, 0U};
    const DataCopyPadExtParams<uint8_t> padIn {false, 0U, 0U, 0U};
    DataCopyPad(relay, source, copyIn, padIn);
}

__aicore__ inline void MoonEpCombineV2::CopySelfTileOut(
    uint32_t selectedIndex, uint64_t peerBase, uint64_t rowOffset,
    uint32_t tileBytes, LocalTensor<uint8_t> relay)
{
    const uint32_t relativeIndex = static_cast<uint16_t>(
        selectedIndexBuf_.Get<int16_t>().GetValue(selectedIndex));
    const uint64_t encoded = static_cast<uint64_t>(
        dstSlotBuf_.Get<int32_t>().GetValue(relativeIndex));
    const uint32_t targetSlot = static_cast<uint32_t>(encoded - peerBase);
    GlobalTensor<uint8_t> target;
    target.SetGlobalBuffer(scratch_ +
        static_cast<uint64_t>(targetSlot) * rowBytes_ + rowOffset);
    const DataCopyExtParams copyOut {1U, tileBytes, 0U, 0U, 0U};
    DataCopyPad(target, relay, copyOut);
}

__aicore__ inline bool MoonEpCombineV2::CopySelfSelectedIndices(
    uint32_t selectedCount, uint32_t chunkStart, uint64_t peerBase)
{
    if (selectedCount == 0U) {
        return true;
    }
    const uint64_t localRowStride =
        TileXRMoonEp::MoonEpCombineV2SelfAlignedRowBytes(rowBytes_);
    if (kEnableSafetyChecks && (localRowStride == 0U ||
        selectedCount >
            TileXRMoonEp::kMoonEpCombineV2SelectionChunkRows)) {
        return false;
    }

    const uint32_t rowsPerBatch =
        TileXRMoonEp::MoonEpCombineV2SelfRowsPerBatch(rowBytes_);
    if (rowsPerBatch != 0U) {
        const uint32_t rowStride = static_cast<uint32_t>(localRowStride);
        uint32_t pendingStart = 0U;
        uint32_t pendingRows = selectedCount < rowsPerBatch ?
            selectedCount : rowsPerBatch;
        LocalTensor<uint8_t> first = selfRelayPingBuf_.Get<uint8_t>();
        CopySelfRowsIn(pendingStart, pendingRows, chunkStart,
            rowStride, first);

        uint32_t nextStart = pendingRows;
        bool pendingPing = true;
        while (nextStart < selectedCount) {
            LocalTensor<uint8_t> pending = pendingPing ?
                selfRelayPingBuf_.Get<uint8_t>() :
                selfRelayPongBuf_.Get<uint8_t>();
            SyncFunc<HardEvent::MTE2_MTE3>();

            const uint32_t remaining = selectedCount - nextStart;
            const uint32_t currentRows = remaining < rowsPerBatch ?
                remaining : rowsPerBatch;
            LocalTensor<uint8_t> current = pendingPing ?
                selfRelayPongBuf_.Get<uint8_t>() :
                selfRelayPingBuf_.Get<uint8_t>();
            CopySelfRowsIn(nextStart, currentRows, chunkStart,
                rowStride, current);
            CopySelfRowsOut(pendingStart, pendingRows, peerBase,
                rowStride, pending);
            SyncFunc<HardEvent::MTE3_S>();

            pendingStart = nextStart;
            pendingRows = currentRows;
            nextStart += currentRows;
            pendingPing = !pendingPing;
        }

        LocalTensor<uint8_t> pending = pendingPing ?
            selfRelayPingBuf_.Get<uint8_t>() :
            selfRelayPongBuf_.Get<uint8_t>();
        SyncFunc<HardEvent::MTE2_MTE3>();
        CopySelfRowsOut(pendingStart, pendingRows, peerBase,
            rowStride, pending);
        SyncFunc<HardEvent::MTE3_S>();
        return true;
    }

    const uint64_t tileCapacity =
        TileXRMoonEp::kMoonEpCombineV2SelfRelayHalfBytes;
    const uint32_t tilesPerRow = static_cast<uint32_t>(
        TileXRMoonEp::MoonEpCombineV2SelfTileCount(rowBytes_));
    const uint64_t totalTiles =
        static_cast<uint64_t>(selectedCount) * tilesPerRow;
    uint32_t pendingRoute = 0U;
    uint64_t pendingOffset = 0U;
    uint32_t pendingBytes =
        TileXRMoonEp::MoonEpCombineV2SelfTileBytes(rowBytes_, 0U);
    LocalTensor<uint8_t> first = selfRelayPingBuf_.Get<uint8_t>();
    CopySelfTileIn(pendingRoute, chunkStart, pendingOffset,
        pendingBytes, first);

    bool pendingPing = true;
    for (uint64_t task = 1U; task < totalTiles; ++task) {
        LocalTensor<uint8_t> pending = pendingPing ?
            selfRelayPingBuf_.Get<uint8_t>() :
            selfRelayPongBuf_.Get<uint8_t>();
        SyncFunc<HardEvent::MTE2_MTE3>();

        const uint32_t currentRoute = static_cast<uint32_t>(task / tilesPerRow);
        const uint32_t tileInRow = static_cast<uint32_t>(task % tilesPerRow);
        const uint64_t currentOffset =
            static_cast<uint64_t>(tileInRow) * tileCapacity;
        const uint32_t currentBytes =
            TileXRMoonEp::MoonEpCombineV2SelfTileBytes(
                rowBytes_, tileInRow);
        LocalTensor<uint8_t> current = pendingPing ?
            selfRelayPongBuf_.Get<uint8_t>() :
            selfRelayPingBuf_.Get<uint8_t>();
        CopySelfTileIn(currentRoute, chunkStart, currentOffset,
            currentBytes, current);
        CopySelfTileOut(pendingRoute, peerBase, pendingOffset,
            pendingBytes, pending);
        SyncFunc<HardEvent::MTE3_S>();

        pendingRoute = currentRoute;
        pendingOffset = currentOffset;
        pendingBytes = currentBytes;
        pendingPing = !pendingPing;
    }

    LocalTensor<uint8_t> pending = pendingPing ?
        selfRelayPingBuf_.Get<uint8_t>() :
        selfRelayPongBuf_.Get<uint8_t>();
    SyncFunc<HardEvent::MTE2_MTE3>();
    CopySelfTileOut(pendingRoute, peerBase, pendingOffset,
        pendingBytes, pending);
    SyncFunc<HardEvent::MTE3_S>();
    return true;
}
__aicore__ inline bool MoonEpCombineV2::SendSelfStep(
    uint32_t peer, uint32_t step)
{
    if (kEnableSafetyChecks && peer != rank_) {
        return false;
    }
    const uint64_t peerBase = static_cast<uint64_t>(peer) * slots_;
    for (uint64_t chunkStart = 0U; chunkStart < slots_;
        chunkStart += TileXRMoonEp::kMoonEpCombineV2SelectionChunkRows) {
        const uint32_t chunkElements = static_cast<uint32_t>(
            slots_ - chunkStart <
                TileXRMoonEp::kMoonEpCombineV2SelectionChunkRows ?
            slots_ - chunkStart :
            TileXRMoonEp::kMoonEpCombineV2SelectionChunkRows);
        LoadSelectionChunk(chunkStart, chunkElements);

        const uint32_t selectedCount =
            SelectPeerIndices(peer, chunkElements);

        const bool copied = CopySelfSelectedIndices(selectedCount,
            static_cast<uint32_t>(chunkStart), peerBase);

        if (kEnableSafetyChecks && !copied) {
            return false;
        }
        (void)copied;
    }
    return true;
}

__aicore__ inline bool MoonEpCombineV2::SendWeightMemoryStep(
    uint32_t peer, uint32_t step)
{
    if (!hasRouteWeight_) {
        return true;
    }
    if (peer >= rankSize_ || weightMemoryArgs_ == nullptr ||
        weightMemoryArgs_->peerMems[peer] == nullptr) {
        SetFailure(TileXRMoonEp::
            MOONEP_COMBINE_V2_WEIGHT_MEMORY_INVALID_CONFIG,
            step, peer, UINT32_MAX);
        return false;
    }

    __gm__ uint8_t *remoteWindow = reinterpret_cast<__gm__ uint8_t *>(
        weightMemoryArgs_->peerMems[peer]) + TileXR::IPC_DATA_OFFSET;
    const uint64_t peerBase = static_cast<uint64_t>(peer) * slots_;
    LocalTensor<uint8_t> ping = selfRelayPingBuf_.Get<uint8_t>();
    LocalTensor<uint8_t> pong = selfRelayPongBuf_.Get<uint8_t>();
    LocalTensor<float> rawWeights = ping.ReinterpretCast<float>();
    LocalTensor<float> packedWeights =
        ping[kWeightPackedOffset].ReinterpretCast<float>();
    LocalTensor<int32_t> packedDestinations =
        pong.ReinterpretCast<int32_t>();
    LocalTensor<uint8_t> doneScratch = pong[kWeightDoneScratchOffset];
    const TEventID stage0Event =
        pipe_->AllocEventID<HardEvent::MTE3_S>();
    const TEventID stage1Event =
        pipe_->AllocEventID<HardEvent::MTE3_S>();
    bool stage0InFlight = false;
    bool stage1InFlight = false;
    uint32_t batchSequence = 0U;
    GlobalTensor<float> sourceWeights;
    sourceWeights.SetGlobalBuffer(
        reinterpret_cast<__gm__ float *>(routeWeightsNvsAddr_), slots_);
    const DataCopyPadExtParams<float> valuePad {false, 0U, 0U, 0U};
    for (uint64_t chunkStart = 0U; chunkStart < slots_;
        chunkStart += TileXRMoonEp::kMoonEpCombineV2SelectionChunkRows) {
        const uint32_t chunkElements = static_cast<uint32_t>(
            slots_ - chunkStart <
                    TileXRMoonEp::kMoonEpCombineV2SelectionChunkRows ?
                slots_ - chunkStart :
                TileXRMoonEp::kMoonEpCombineV2SelectionChunkRows);
        LoadSelectionChunk(chunkStart, chunkElements);
        const uint32_t selectedCount = SelectPeerDestinations(
            peer, chunkElements, packedDestinations);
        if (selectedCount == 0U) {
            continue;
        }

        Adds(packedDestinations, packedDestinations,
            -static_cast<int32_t>(peerBase), selectedCount);

        const DataCopyExtParams valueIn {1U,
            chunkElements * static_cast<uint32_t>(sizeof(float)),
            0U, 0U, 0U};
        SyncFunc<HardEvent::S_MTE2>();
        DataCopyPad(rawWeights, sourceWeights[chunkStart], valueIn, valuePad);
        SyncFunc<HardEvent::MTE2_V>();
        uint64_t weightCount = 0U;
        GatherMask(packedWeights, rawWeights,
            lowerMaskBuf_.Get<uint32_t>(), true, chunkElements,
            {1U, 1U, 0U, 0U}, weightCount);
        SyncFunc<HardEvent::V_S>();

        if (kEnableSafetyChecks && weightCount != selectedCount) {
            if (stage0InFlight) {
                WaitFlag<HardEvent::MTE3_S>(stage0Event);
            }
            if (stage1InFlight) {
                WaitFlag<HardEvent::MTE3_S>(stage1Event);
            }
            pipe_->ReleaseEventID<HardEvent::MTE3_S>(stage0Event);
            pipe_->ReleaseEventID<HardEvent::MTE3_S>(stage1Event);
            SetFailure(TileXRMoonEp::
                MOONEP_COMBINE_V2_WEIGHT_MEMORY_INVALID_CONFIG,
                step, peer, UINT32_MAX, selectedCount,
                selectedCount, weightCount);
            return false;
        }
        if (kEnableSafetyChecks) {
            for (uint32_t index = 0U; index < selectedCount; ++index) {
                const int32_t target = packedDestinations.GetValue(index);
                if (target < 0 || static_cast<uint64_t>(target) >= slots_) {
                    if (stage0InFlight) {
                        WaitFlag<HardEvent::MTE3_S>(stage0Event);
                    }
                    if (stage1InFlight) {
                        WaitFlag<HardEvent::MTE3_S>(stage1Event);
                    }
                    pipe_->ReleaseEventID<HardEvent::MTE3_S>(stage0Event);
                    pipe_->ReleaseEventID<HardEvent::MTE3_S>(stage1Event);
                    SetFailure(TileXRMoonEp::
                        MOONEP_COMBINE_V2_BAD_DESTINATION,
                        step, peer, UINT32_MAX, index, slots_,
                        static_cast<uint32_t>(target));
                    return false;
                }
            }
        }

        for (uint32_t batchStart = 0U; batchStart < selectedCount;
            batchStart += TileXRMoonEp::kMoonEpCombineV2PayloadBatchRows) {
            const uint32_t batchCount =
                TileXRMoonEp::MoonEpCombineV2WeightBatchRows(
                    selectedCount, batchStart);
            const bool useStage0 =
                TileXRMoonEp::MoonEpCombineV2WeightStageIndex(
                    batchSequence) == 0U;
            const TEventID stageEvent = useStage0 ?
                stage0Event : stage1Event;
            if ((useStage0 && stage0InFlight) ||
                (!useStage0 && stage1InFlight)) {
                WaitFlag<HardEvent::MTE3_S>(stageEvent);

            }
            LocalTensor<uint8_t> recordStage = pong[useStage0 ?
                kWeightRecordStage0Offset : kWeightRecordStage1Offset];
#if defined(CATLASS_ARCH) && CATLASS_ARCH == 3510
            Simt::VF_CALL<MoonEpCombineV2BuildWeightRecordsVf>(
                Simt::Dim3{
                    TileXRMoonEp::kMoonEpCombineV2BuilderThreads, 1U, 1U},
                reinterpret_cast<__ubuf__ MoonEpCombineV2WeightStageRecord *>(
                    recordStage.GetPhyAddr()),
                reinterpret_cast<__ubuf__ const float *>(
                    packedWeights.GetPhyAddr()),
                magic_, batchStart, batchCount);
#else
            pipe_->ReleaseEventID<HardEvent::MTE3_S>(stage0Event);
            pipe_->ReleaseEventID<HardEvent::MTE3_S>(stage1Event);
            return false;
#endif
            SyncFunc<HardEvent::V_MTE3>();

            uint32_t commandStart = 0U;
            while (commandStart < batchCount) {
                const uint32_t target = static_cast<uint32_t>(
                    packedDestinations.GetValue(batchStart + commandStart));
                uint32_t runLength = 1U;
                while (commandStart + runLength < batchCount &&
                    TileXRMoonEp::MoonEpCombineV2WeightRunContinues(
                        target, runLength, static_cast<uint32_t>(
                            packedDestinations.GetValue(
                                batchStart + commandStart + runLength)))) {
                    ++runLength;
                }
                GlobalTensor<uint8_t> destination;
                destination.SetGlobalBuffer(
                    remoteWindow + weightRecordOffset_ +
                    static_cast<uint64_t>(epoch_) *
                        weightRecordEpochStride_ +
                    static_cast<uint64_t>(target) *
                        TileXRMoonEp::kMoonEpCombineV2WeightRecordBytes);
                const DataCopyExtParams recordOut {
                    static_cast<uint16_t>(runLength),
                    static_cast<uint32_t>(
                        TileXRMoonEp::kMoonEpCombineV2WeightRecordBytes),
                    0U, 0U, 0U};
                DataCopyPad(destination,
                    recordStage[commandStart *
                        kWeightRecordLocalStrideBytes], recordOut);
                commandStart += runLength;
            }
            SetFlag<HardEvent::MTE3_S>(stageEvent);

            if (useStage0) {
                stage0InFlight = true;
            } else {
                stage1InFlight = true;
            }
            ++batchSequence;
        }
    }

    if (stage0InFlight) {
        WaitFlag<HardEvent::MTE3_S>(stage0Event);
    }
    if (stage1InFlight) {
        WaitFlag<HardEvent::MTE3_S>(stage1Event);
    }
    pipe_->ReleaseEventID<HardEvent::MTE3_S>(stage0Event);
    pipe_->ReleaseEventID<HardEvent::MTE3_S>(stage1Event);
    LocalTensor<uint64_t> token = doneScratch.ReinterpretCast<uint64_t>();
    token.SetValue(0U, TileXRMoonEp::MoonEpCombineV2Token(magic_, step));
    for (uint32_t word = 1U;
        word < TileXRMoonEp::kMoonEpCombineV2WeightDoneStrideBytes /
                sizeof(uint64_t);
        ++word) {
        token.SetValue(word, 0U);
    }
    GlobalTensor<uint8_t> done;
    done.SetGlobalBuffer(remoteWindow + weightDoneOffset_ +
        static_cast<uint64_t>(epoch_) * weightDoneEpochStride_ +
        static_cast<uint64_t>(rank_) *
            TileXRMoonEp::kMoonEpCombineV2WeightDoneStrideBytes);
    const DataCopyExtParams doneOut {1U,
        static_cast<uint32_t>(
            TileXRMoonEp::kMoonEpCombineV2WeightDoneStrideBytes),
        0U, 0U, 0U};
    SyncFunc<HardEvent::S_MTE3>();
    DataCopyPad(done, doneScratch, doneOut);
    SyncFunc<HardEvent::MTE3_S>();

    return true;
}

__aicore__ inline bool MoonEpCombineV2::WaitInboundWeightDone(
    uint32_t step, uint32_t source)
{
    if (!hasRouteWeight_) {
        return true;
    }
    const uint64_t expected =
        TileXRMoonEp::MoonEpCombineV2Token(magic_, step);
    __gm__ uint64_t *done = reinterpret_cast<__gm__ uint64_t *>(
        localWeightWindow_ + weightDoneOffset_ +
        static_cast<uint64_t>(epoch_) * weightDoneEpochStride_ +
        static_cast<uint64_t>(source) *
            TileXRMoonEp::kMoonEpCombineV2WeightDoneStrideBytes);
    uint64_t observed = LoadToken(done);
    while (observed != expected) {
        if (TimedOut(operationStartCycles_)) {
            SetFailure(TileXRMoonEp::MOONEP_COMBINE_V2_WEIGHT_DONE_TIMEOUT,
                step, source, UINT32_MAX, 0U, expected, observed);

            return false;
        }
        observed = LoadToken(done);
    }

    return true;
}

__aicore__ inline bool MoonEpCombineV2::CopyReceivedWeights()
{
    if (!hasRouteWeight_) {
        return true;
    }
    const uint64_t begin = weightOutputElements_ * core_ / activeCoreCount_;
    const uint64_t end = weightOutputElements_ * (core_ + 1U) /
        activeCoreCount_;
    LocalTensor<uint8_t> recordBytes =
        selfRelayPingBuf_.Get<uint8_t>();
    LocalTensor<uint8_t> outputStorage =
        selfRelayPongBuf_.Get<uint8_t>();
    LocalTensor<uint32_t> valueWords =
        outputStorage.ReinterpretCast<uint32_t>();
    LocalTensor<uint32_t> magicLowWords =
        outputStorage[kWeightOutputTensorBytes].ReinterpretCast<uint32_t>();
    LocalTensor<uint32_t> magicHighWords =
        outputStorage[2U * kWeightOutputTensorBytes].
            ReinterpretCast<uint32_t>();
    LocalTensor<float> filteredValues =
        outputStorage[3U * kWeightOutputTensorBytes].
            ReinterpretCast<float>();
    LocalTensor<uint32_t> gatherPattern =
        lowerMaskBuf_.Get<uint32_t>();
    LocalTensor<uint8_t> validMask = lowerMaskBuf_.Get<uint8_t>();
    LocalTensor<uint8_t> highValidMask = upperMaskBuf_.Get<uint8_t>();
    const DataCopyPadExtParams<uint8_t> recordPad {false, 0U, 0U, 0U};
    GlobalTensor<float> outputWeights;
    outputWeights.SetGlobalBuffer(
        reinterpret_cast<__gm__ float *>(routeWeightsSkAddr_),
        weightOutputElements_);
    for (uint64_t tileStart = begin; tileStart < end;
        tileStart += kWeightOutputTileRows) {
        const uint32_t tileElements = static_cast<uint32_t>(
            end - tileStart < kWeightOutputTileRows ?
                end - tileStart : kWeightOutputTileRows);
        const uint32_t tileWords = tileElements * 4U;
        GlobalTensor<uint8_t> sourceRecords;
        sourceRecords.SetGlobalBuffer(
            localWeightWindow_ + weightRecordOffset_ +
            static_cast<uint64_t>(epoch_) * weightRecordEpochStride_ +
            tileStart * TileXRMoonEp::kMoonEpCombineV2WeightRecordBytes);
        const DataCopyExtParams recordIn {1U,
            tileElements * static_cast<uint32_t>(
                TileXRMoonEp::kMoonEpCombineV2WeightRecordBytes),
            0U, 0U, 0U};
        SyncFunc<HardEvent::S_MTE2>();
        DataCopyPad(recordBytes, sourceRecords, recordIn, recordPad);
        SyncFunc<HardEvent::MTE2_V>();

        uint64_t valueCount = 0U;
        uint64_t magicLowCount = 0U;
        uint64_t magicHighCount = 0U;
        Duplicate(gatherPattern, 0x11111111U,
            kSelectionMaskUint32Elements);
        PipeBarrier<PIPE_V>();
        GatherMask(valueWords, recordBytes.ReinterpretCast<uint32_t>(),
            gatherPattern, true, tileWords, {1U, 1U, 0U, 0U}, valueCount);
        PipeBarrier<PIPE_V>();
        Duplicate(gatherPattern, 0x44444444U,
            kSelectionMaskUint32Elements);
        PipeBarrier<PIPE_V>();
        GatherMask(magicLowWords, recordBytes.ReinterpretCast<uint32_t>(),
            gatherPattern, true, tileWords, {1U, 1U, 0U, 0U},
            magicLowCount);
        PipeBarrier<PIPE_V>();
        Duplicate(gatherPattern, 0x88888888U,
            kSelectionMaskUint32Elements);
        PipeBarrier<PIPE_V>();
        GatherMask(magicHighWords, recordBytes.ReinterpretCast<uint32_t>(),
            gatherPattern, true, tileWords, {1U, 1U, 0U, 0U},
            magicHighCount);
        PipeBarrier<PIPE_V>();
        Compares(validMask, magicLowWords,
            static_cast<uint32_t>(magic_), CMPMODE::EQ, tileElements);
        Compares(highValidMask, magicHighWords,
            static_cast<uint32_t>(magic_ >> 32U),
            CMPMODE::EQ, tileElements);
        PipeBarrier<PIPE_V>();
        And(validMask.ReinterpretCast<uint32_t>(),
            validMask.ReinterpretCast<uint32_t>(),
            highValidMask.ReinterpretCast<uint32_t>(),
            static_cast<int32_t>((tileElements + 31U) / 32U));
        PipeBarrier<PIPE_V>();
        Select(filteredValues, validMask,
            valueWords.ReinterpretCast<float>(), 0.0F,
            SELMODE::VSEL_TENSOR_SCALAR_MODE, tileElements);
        SyncFunc<HardEvent::V_S>();
        if (kEnableSafetyChecks &&
            (valueCount != tileElements || magicLowCount != tileElements ||
                magicHighCount != tileElements)) {
            SetFailure(TileXRMoonEp::
                MOONEP_COMBINE_V2_WEIGHT_MEMORY_INVALID_CONFIG,
                UINT32_MAX, rank_, UINT32_MAX, tileElements,
                tileElements, valueCount);

            return false;
        }
        const DataCopyExtParams valueOut {1U,
            tileElements * static_cast<uint32_t>(sizeof(float)),
            0U, 0U, 0U};
        SyncFunc<HardEvent::V_MTE3>();
        DataCopyPad(outputWeights[tileStart], filteredValues, valueOut);
        SyncFunc<HardEvent::MTE3_S>();
    }

    return true;
}

__aicore__ inline bool MoonEpCombineV2::WaitInboundDone(
    uint32_t step, uint32_t source)
{
    if (source >= rankSize_) {
        SetFailure(TileXRMoonEp::MOONEP_COMBINE_V2_INVALID_CONFIG,
            step, source, UINT32_MAX);
        return false;
    }
    bool ready[TileXRMoonEp::kMoonEpCombineV2LaneCount] = {};
    uint64_t observed[TileXRMoonEp::kMoonEpCombineV2LaneCount] = {};
    uint32_t remaining = 0U;
    for (uint32_t lane = 0U;
        lane < TileXRMoonEp::kMoonEpCombineV2LaneCount; ++lane) {
        ready[lane] = !TileXRMoonEp::MoonEpCombineV2DoneLaneRequired(
            source, rank_, lane,
            static_cast<uint32_t>(args_->localRankSize));
        if (!ready[lane]) {
            ++remaining;
        }
    }
    const uint64_t expected =
        TileXRMoonEp::MoonEpCombineV2Token(magic_, step);
    while (remaining != 0U) {
        for (uint32_t lane = 0U;
            lane < TileXRMoonEp::kMoonEpCombineV2LaneCount; ++lane) {
            if (ready[lane]) {
                continue;
            }
            const uint64_t index = TileXRMoonEp::MoonEpCombineV2DoneIndex(
                epoch_, source, lane);
            __gm__ uint64_t *token = reinterpret_cast<__gm__ uint64_t *>(
                doneBase_ + index *
                    TileXRMoonEp::kMoonEpCombineV2TokenStrideBytes);
            observed[lane] = LoadToken(token);
            if (observed[lane] == expected) {
                ready[lane] = true;
                --remaining;
            }
        }
        if (TimedOut(operationStartCycles_)) {
            for (uint32_t lane = 0U;
                lane < TileXRMoonEp::kMoonEpCombineV2LaneCount; ++lane) {
                if (ready[lane]) {
                    continue;
                }
                SetFailure(TileXRMoonEp::MOONEP_COMBINE_V2_DONE_TIMEOUT,
                    step, source, lane, 0U, expected, observed[lane]);
                return false;
            }
        }
    }
    return WaitInboundWeightDone(step, source);
}
__aicore__ inline bool MoonEpCombineV2::BeginCollectiveStage(
    uint32_t stageId)
{
    const uint64_t index = TileXRMoonEp::MoonEpCombineV2CollectiveStatusIndex(
        epoch_, stageId);
    __gm__ TileXRMoonEp::MoonEpCombineV2CollectiveStatus *status =
        reinterpret_cast<__gm__
            TileXRMoonEp::MoonEpCombineV2CollectiveStatus *>(
                collectiveStatusBase_ + index *
                    TileXRMoonEp::kMoonEpCombineV2CollectiveStatusSlotBytes);
    if (core_ == 0U) {
        status->magic = magic_;
        status->guard = TileXRMoonEp::MoonEpCombineV2CollectiveStatusGuard(
            magic_, stageId);
        status->marker = TileXRMoonEp::kMoonEpCombineV2CollectiveStatusMarker;
        status->stageId = stageId;
        status->status = TileXRMoonEp::MOONEP_COMBINE_V2_SUCCESS;
        status->firstFailureCore = UINT32_MAX;
        for (uint32_t reserved = 0U; reserved < 4U; ++reserved) {
            status->reserved[reserved] = 0U;
        }
        TileXR::UDMACleanCacheLines(
            reinterpret_cast<__gm__ uint8_t *>(status),
            TileXRMoonEp::kMoonEpCombineV2CollectiveStatusSlotBytes);
    }
    AscendC::SyncAll<true>();
    TileXR::UDMACleanCacheLines(reinterpret_cast<__gm__ uint8_t *>(status),
        TileXRMoonEp::kMoonEpCombineV2CollectiveStatusSlotBytes);
    const bool ready = status->magic == magic_ &&
        status->marker == TileXRMoonEp::kMoonEpCombineV2CollectiveStatusMarker &&
        status->stageId == stageId &&
        status->guard == TileXRMoonEp::MoonEpCombineV2CollectiveStatusGuard(
            magic_, stageId);
    if (!ready) {
        SetFailure(TileXRMoonEp::MOONEP_COMBINE_V2_COLLECTIVE_STATUS_ERROR,
            stageId, rank_, UINT32_MAX, 0U,
            TileXRMoonEp::MoonEpCombineV2CollectiveStatusGuard(
                magic_, stageId), status->guard);
    }
    return ready;
}

__aicore__ inline bool MoonEpCombineV2::EndCollectiveStage(
    uint32_t stageId, bool localSucceeded)
{
    const uint64_t index = TileXRMoonEp::MoonEpCombineV2CollectiveStatusIndex(
        epoch_, stageId);
    __gm__ TileXRMoonEp::MoonEpCombineV2CollectiveStatus *status =
        reinterpret_cast<__gm__
            TileXRMoonEp::MoonEpCombineV2CollectiveStatus *>(
                collectiveStatusBase_ + index *
                    TileXRMoonEp::kMoonEpCombineV2CollectiveStatusSlotBytes);
    if (!localSucceeded) {
        if (failureStatus_ == TileXRMoonEp::MOONEP_COMBINE_V2_SUCCESS) {
            SetFailure(
                TileXRMoonEp::MOONEP_COMBINE_V2_COLLECTIVE_STATUS_ERROR,
                stageId, rank_, UINT32_MAX);
        }
        PublishFailureRecord(stageId);
        (void)AscendC::AtomicCas(&status->firstFailureCore, UINT32_MAX, core_);
        (void)AscendC::AtomicCas(&status->status,
            static_cast<uint32_t>(TileXRMoonEp::MOONEP_COMBINE_V2_SUCCESS),
            failureStatus_);
    }
    AscendC::SyncAll<true>();
    TileXR::UDMACleanCacheLines(reinterpret_cast<__gm__ uint8_t *>(status),
        TileXRMoonEp::kMoonEpCombineV2CollectiveStatusSlotBytes);
    const bool succeeded = status->magic == magic_ &&
        status->marker == TileXRMoonEp::kMoonEpCombineV2CollectiveStatusMarker &&
        status->stageId == stageId &&
        status->guard == TileXRMoonEp::MoonEpCombineV2CollectiveStatusGuard(
            magic_, stageId) &&
        status->status == TileXRMoonEp::MOONEP_COMBINE_V2_SUCCESS;
    AscendC::SyncAll<true>();
    return succeeded;
}
__aicore__ inline void MoonEpCombineV2::InitReduceBuffers(
    uint32_t inputBufferNum, uint32_t inputSlotBytes,
    uint32_t floatRowBytes, uint32_t outputSlotBytes)
{
    constexpr uint32_t maxFloatRowBytes =
        kReduceMaxRowElements * sizeof(float);
    static_assert(kReduceMaxInputBuffers * kReduceMaxRowBytes +
            2U * maxFloatRowBytes +
            kReduceOutputBuffers * kReduceMaxRowBytes <= kFullUbBytes,
        "Combine V2 multi-buffer reduction exceeds UB");
    pipe_->Reset();
    pipe_->InitBuffer(reduceInputQueue_, inputBufferNum, inputSlotBytes);
    pipe_->InitBuffer(reduceOutputQueue_, kReduceOutputBuffers,
        outputSlotBytes);
    pipe_->InitBuffer(reduceRowBuf_, floatRowBytes);
    pipe_->InitBuffer(reduceAccumulatorBuf_, floatRowBytes);
}

__aicore__ inline void MoonEpCombineV2::CopyReduceInput(
    uint64_t workOrdinal, int64_t tokenBegin, uint64_t rowCount)
{
    const uint64_t topK = static_cast<uint64_t>(topK_);
    const uint64_t groupOrdinal = workOrdinal / topK;
    const uint64_t route = workOrdinal % topK;
    const uint64_t tokenLocal = groupOrdinal / rowCount;
    const uint64_t hiddenRow = groupOrdinal % rowCount;
    const uint64_t token = static_cast<uint64_t>(tokenBegin) + tokenLocal;
    const uint64_t hiddenOffset = hiddenRow * kReduceMaxRowElements;
    const uint32_t rowElements = static_cast<uint32_t>(
        static_cast<uint64_t>(h_) - hiddenOffset <
                kReduceMaxRowElements ?
            static_cast<uint64_t>(h_) - hiddenOffset :
            kReduceMaxRowElements);

    LocalTensor<bfloat16_t> inputRow =
        reduceInputQueue_.AllocTensor<bfloat16_t>();
    GlobalTensor<bfloat16_t> input;
    input.SetGlobalBuffer(reinterpret_cast<__gm__ bfloat16_t *>(
        scratch_ + (token * topK + route) * rowBytes_) + hiddenOffset,
        rowElements);
    const DataCopyExtParams copyIn {
        1U, static_cast<uint32_t>(rowElements * sizeof(bfloat16_t)),
        0U, 0U, 0U};
    const DataCopyPadExtParams<bfloat16_t> pad {false, 0U, 0U, 0U};
    DataCopyPad(inputRow, input, copyIn, pad);
    reduceInputQueue_.EnQue(inputRow);
}

__aicore__ inline bool MoonEpCombineV2::ReduceHidden()
{
    if (!reduceHidden_) {
        return true;
    }
    const int64_t tokenBegin = bs_ * core_ / activeCoreCount_;
    const int64_t tokenEnd = bs_ * (core_ + 1U) / activeCoreCount_;
    if (tokenBegin >= tokenEnd) {
        return true;
    }
    const uint64_t rowCapacityElements = static_cast<uint64_t>(h_) <
            kReduceMaxRowElements ?
        static_cast<uint64_t>(h_) : kReduceMaxRowElements;
    const uint64_t rowCount = CeilDiv(
        static_cast<uint64_t>(h_), kReduceMaxRowElements);
    const uint32_t inputSlotBytes = static_cast<uint32_t>(AlignUp(
        rowCapacityElements * sizeof(bfloat16_t), kUbAlignBytes));
    const uint32_t floatRowBytes = static_cast<uint32_t>(AlignUp(
        rowCapacityElements * sizeof(float), kUbAlignBytes));
    const uint32_t fixedBytes = 2U * floatRowBytes +
        kReduceOutputBuffers * inputSlotBytes;
    if (fixedBytes >= kFullUbBytes) {
        return false;
    }
    const uint64_t totalRouteWorkItems =
        static_cast<uint64_t>(tokenEnd - tokenBegin) * rowCount *
        static_cast<uint64_t>(topK_);
    uint32_t inputBufferNum = static_cast<uint32_t>(
        (kFullUbBytes - fixedBytes) / inputSlotBytes);
    inputBufferNum = inputBufferNum < kReduceMaxInputBuffers ?
        inputBufferNum : kReduceMaxInputBuffers;
    inputBufferNum = totalRouteWorkItems < inputBufferNum ?
        static_cast<uint32_t>(totalRouteWorkItems) : inputBufferNum;
    if (inputBufferNum == 0U) {
        return false;
    }

    PipeBarrier<PIPE_ALL>();
    InitReduceBuffers(inputBufferNum, inputSlotBytes, floatRowBytes,
        inputSlotBytes);
    LocalTensor<float> row = reduceRowBuf_.Get<float>();
    LocalTensor<float> accumulator = reduceAccumulatorBuf_.Get<float>();

    uint64_t issueOrdinal = 0U;
    for (; issueOrdinal < inputBufferNum; ++issueOrdinal) {
        CopyReduceInput(issueOrdinal, tokenBegin, rowCount);
    }

    const uint64_t topK = static_cast<uint64_t>(topK_);
    for (uint64_t consumeOrdinal = 0U;
        consumeOrdinal < totalRouteWorkItems; ++consumeOrdinal) {
        const uint64_t groupOrdinal = consumeOrdinal / topK;
        const uint64_t route = consumeOrdinal % topK;
        const uint64_t tokenLocal = groupOrdinal / rowCount;
        const uint64_t hiddenRow = groupOrdinal % rowCount;
        const uint64_t hiddenOffset = hiddenRow * kReduceMaxRowElements;
        const uint32_t rowElements = static_cast<uint32_t>(
            static_cast<uint64_t>(h_) - hiddenOffset <
                    kReduceMaxRowElements ?
                static_cast<uint64_t>(h_) - hiddenOffset :
                kReduceMaxRowElements);

        LocalTensor<bfloat16_t> inputRow =
            reduceInputQueue_.DeQue<bfloat16_t>();
        if (route == 0U) {
            Duplicate(accumulator, 0.0f,
                static_cast<int32_t>(rowElements));
            PipeBarrier<PIPE_V>();
        }
        Cast(row, inputRow, RoundMode::CAST_NONE,
            static_cast<int32_t>(rowElements));
        PipeBarrier<PIPE_V>();
        Add(accumulator, accumulator, row,
            static_cast<int32_t>(rowElements));
        PipeBarrier<PIPE_V>();
        reduceInputQueue_.FreeTensor(inputRow);

        if (issueOrdinal < totalRouteWorkItems) {
            CopyReduceInput(issueOrdinal, tokenBegin, rowCount);
            ++issueOrdinal;
        }

        if (route + 1U != topK) {
            continue;
        }
        LocalTensor<bfloat16_t> outputRow =
            reduceOutputQueue_.AllocTensor<bfloat16_t>();
        Cast(outputRow, accumulator, RoundMode::CAST_RINT,
            static_cast<int32_t>(rowElements));
        reduceOutputQueue_.EnQue(outputRow);
        outputRow = reduceOutputQueue_.DeQue<bfloat16_t>();
        GlobalTensor<bfloat16_t> output;
        const uint64_t token =
            static_cast<uint64_t>(tokenBegin) + tokenLocal;
        output.SetGlobalBuffer(reinterpret_cast<__gm__ bfloat16_t *>(
            workspace_ + outputOffset_) + token *
                static_cast<uint64_t>(h_) + hiddenOffset,
            rowElements);
        const DataCopyExtParams copyOut {
            1U, static_cast<uint32_t>(rowElements * sizeof(bfloat16_t)),
            0U, 0U, 0U};
        DataCopyPad(output, outputRow, copyOut);
        reduceOutputQueue_.FreeTensor(outputRow);
    }
    PipeBarrier<PIPE_ALL>();
    return true;
}

__aicore__ inline void MoonEpCombineV2::Process()
{
    AscendC::SyncAll<true>();
    if (!collectiveReady_) {
        return;
    }

    bool stageReady = BeginCollectiveStage(kCollectiveInitStage);
    bool localSucceeded = stageReady && valid_;
    if (activeWorker_ && localSucceeded) {
        registry_ = TileXR::GetUDMARegistry(args_);
        localSucceeded = registry_ != nullptr;
        if (localSucceeded) {
            InitBuffers();
        } else {
            SetFailure(TileXRMoonEp::MOONEP_COMBINE_V2_INVALID_CONFIG,
                UINT32_MAX, UINT32_MAX, UINT32_MAX);
        }
    }
    bool succeeded = EndCollectiveStage(kCollectiveInitStage,
        !activeWorker_ || localSucceeded);
    if (!succeeded) {
        return;
    }

    stageReady = BeginCollectiveStage(kCollectiveValidationStage);
    localSucceeded = stageReady;
    if (activeWorker_ && localSucceeded && kEnableSafetyChecks) {
        localSucceeded = failureStatus_ ==
            TileXRMoonEp::MOONEP_COMBINE_V2_SUCCESS && CheckPoison();
        if (localSucceeded) {
            localSucceeded = ValidateDestinations();
        }
    }
    succeeded = EndCollectiveStage(kCollectiveValidationStage,
        !activeWorker_ || localSucceeded);

    if (!succeeded) {
        return;
    }

    if (activeWorker_) {
        PrefillOperatorWqes();
    }
    for (uint32_t step = 0U; step < stepCount_; ++step) {
        localSucceeded = succeeded;
        bool fullmeshStep = false;
        const TileXRMoonEp::MoonEpCombineV2StepRoute route =
            TileXRMoonEp::MoonEpCombineV2BuildStepRoute(
                scheduleMode_, rank_, core_, step, rankSize_);
        if (activeWorker_ && localSucceeded && !route.valid) {
            SetFailure(TileXRMoonEp::MOONEP_COMBINE_V2_INVALID_CONFIG,
                step, route.sendDstRank, UINT32_MAX);
            localSucceeded = false;
        }
        if (activeWorker_ && localSucceeded) {
            localSucceeded = WaitStepCredit(step, route);
        }
        if (activeWorker_ && localSucceeded) {
            const uint32_t peer = route.sendDstRank;
            const bool selfStep = peer == rank_;
            fullmeshStep = !selfStep &&
                TileXRMoonEp::MoonEpCombineV2SameServer(rank_, peer,
                    static_cast<uint32_t>(args_->localRankSize));
            if (selfStep) {
                localSucceeded = SendSelfStep(rank_, step);
                if (localSucceeded) {
                    localSucceeded = SendWeightMemoryStep(rank_, step);
                }
            } else if (fullmeshStep) {
                localSucceeded = SendFullmeshStep(peer, step);
                if (localSucceeded) {
                    localSucceeded = SendWeightMemoryStep(peer, step);
                }
                if (localSucceeded) {
                    localSucceeded = WaitFullmeshCq(step, peer);
                }
            } else {
                localSucceeded = SendRemoteStep(peer, step);
                if (localSucceeded) {
                    localSucceeded = SendWeightMemoryStep(peer, step);
                }
            }
        }

        if (activeWorker_ && localSucceeded) {
            localSucceeded = WaitStepCqs(step);
        }
        if (activeWorker_ && localSucceeded) {
            localSucceeded = WaitInboundDone(step, route.recvSrcRank);
        }
        if (activeWorker_ && localSucceeded) {
            localSucceeded = PublishNextCredit(step, route);
        }
        succeeded = localSucceeded;

        if (!succeeded) {
            break;
        }
    }
    stageReady = BeginCollectiveStage(kCollectiveDoneStage);
    localSucceeded = succeeded && stageReady;
    succeeded = EndCollectiveStage(kCollectiveDoneStage,
        !activeWorker_ || localSucceeded);

    stageReady = BeginCollectiveStage(kCollectiveReduceStage);
    localSucceeded = succeeded && stageReady;
    if (activeWorker_ && localSucceeded && !CopyReceivedWeights()) {
        SetFailure(TileXRMoonEp::
            MOONEP_COMBINE_V2_WEIGHT_MEMORY_INVALID_CONFIG,
            UINT32_MAX, UINT32_MAX, UINT32_MAX);
        localSucceeded = false;
    }
    if (activeWorker_ && localSucceeded && !ReduceHidden()) {
        SetFailure(TileXRMoonEp::MOONEP_COMBINE_V2_INVALID_CONFIG,
            UINT32_MAX, UINT32_MAX, UINT32_MAX);
        localSucceeded = false;
    }
    succeeded = EndCollectiveStage(kCollectiveReduceStage,
        !activeWorker_ || localSucceeded);
    (void)succeeded;

}

} // namespace

#endif // TILEXR_MOONEP_COMBINE_V2_KERNEL_H
