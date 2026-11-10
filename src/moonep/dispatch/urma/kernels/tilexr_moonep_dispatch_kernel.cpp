/*
 * Copyright (c) 2024-2026 TileXR Project
 * Licensed under the Apache License, Version 2.0
 */

/*
 * 执行 URMA Dispatch 路由和信用控制，向工作区写入 DFX/状态；dispatch_launch.cpp 提供外置 kernel 参数。
 */

#include <cstdint>

#include "comm_args.h"
#include "../common/dispatch_common.h"
#include "../common/dispatch_credit.h"
#include "../common/dispatch_diagnostics.h"
#include "../common/dispatch_schedule.h"
#include "../common/dispatch_wqe_batch.h"
#include "kernel_operator.h"
#include "tilexr_udma.h"
#include "tilexr_udma_fullmesh.h"

namespace {
constexpr uint32_t kUbAlignBytes = 32U;
constexpr uint32_t kFullUbBytes = 216U * 1024U;
constexpr uint32_t kRelayUbBytes = 4U * 1024U;
constexpr uint32_t kLocalCopyTileBytes = 8U * 1024U;
constexpr uint32_t kDiagnosticUbBytes = 4U * 1024U;
constexpr uint32_t kRouteTileElements = 1024U;
constexpr uint32_t kRouteTileBytes = kRouteTileElements * sizeof(int32_t);
constexpr uint32_t kRouteSelectChunkElements = 8192U;
// Grouped launches reuse plans across stages, so keep their SQ/CQ state in the
// registry-owned QP domain that is initialized for each operator generation.
constexpr bool kDispatchGroupedAllowFullmesh = false;
constexpr uint32_t kVectorCompareMinElements = 256U / sizeof(int32_t);
constexpr uint32_t kOutputCopyBufferNum = 2U;
constexpr uint64_t kDeferredReclaimWqes =
    TileXR::TILEXR_UDMA_SQ_BB_COUNT / 4U;
constexpr uint32_t kDispatchUdmaWqeBytes = 64U;
constexpr uint32_t kDispatchWqeBuildThreads = 64U;
constexpr uint32_t kDispatchQpSelectionSingleLane = 1U << 4U;
constexpr uint32_t kDispatchSqPollReserve = 10U;
constexpr uint32_t kDispatchPreparedPeerCapacity = 16U;
constexpr uint32_t kDispatchCqePollBatchCapacity =
    kRelayUbBytes / static_cast<uint32_t>(sizeof(TileXR::UDMACqeCtx));

constexpr uint32_t kDispatchWqeBatchBytes =
    TileXRMoonEp::kDispatchWqeBatchCapacity * kDispatchUdmaWqeBytes;

struct alignas(32) DispatchWqeOperatorContext {
    uint64_t reserved[4];
};

struct DispatchWqePeerQpFields {
    uint64_t rmtEidL;
    uint64_t rmtEidH;
    uint32_t tokenEn;
    uint32_t rmtJettyType;
    uint32_t targetHint;
    uint32_t tpId;
    uint32_t rmtJettyOrSegId;
    uint32_t rmtTokenValue;
};

struct alignas(32) DispatchWqePeerContext {
    DispatchWqePeerQpFields qp[TileXRMoonEp::kDispatchQpCount];
};

struct alignas(32) DispatchWqePatchContext {
    uint64_t hiddenLocalSourceBase;
    uint64_t hiddenRemoteScratchBase;
    uint64_t hiddenRowBytes;
    uint64_t weightLocalSourceBase;
    uint64_t weightRemoteScratchBase;
    uint64_t weightRowBytes;
    uint64_t destinationBase;
    uint64_t signalLocalAddr;
    uint64_t signalRemoteAddr;
    uint32_t batchHead;
    uint32_t batchOutputOffset;
    uint32_t dataTaskCount;
    uint32_t appendSignal;
    uint32_t topKMagic;
    uint32_t topKShift;
    uint32_t hasWeight;
    uint32_t dataTaskStart;
    uint32_t qpSelection;
    uint32_t routePlanStart;
};

constexpr uint32_t kDispatchWqeBatchContextBytes =
    sizeof(DispatchWqePeerContext) > sizeof(DispatchWqePatchContext) ?
    static_cast<uint32_t>(sizeof(DispatchWqePeerContext)) :
    static_cast<uint32_t>(sizeof(DispatchWqePatchContext));
constexpr uint32_t kDispatchWqeBatchContextOffset = kDispatchWqeBatchBytes;
constexpr uint32_t kDispatchUdmaIssueUbBytes =
    kDispatchWqeBatchBytes + kDispatchWqeBatchContextBytes;
constexpr uint32_t kMaxRoutePlanUbBytes =
    kRouteSelectChunkElements * static_cast<uint32_t>(sizeof(int32_t));
constexpr uint32_t kMaxRouteIndexUbBytes =
    kRouteSelectChunkElements * static_cast<uint32_t>(sizeof(int16_t));
constexpr uint32_t kMaxRouteCompareMaskUbBytes =
    kRouteSelectChunkElements / 8U;
constexpr uint32_t kMaxRouteSelectSendUbBytes =
    kMaxRoutePlanUbBytes + 2U * kMaxRouteIndexUbBytes +
    2U * kMaxRouteCompareMaskUbBytes + kRelayUbBytes +
    2U * kDispatchUdmaIssueUbBytes;

static_assert(sizeof(TileXR::UDMASqeCtx) + sizeof(TileXR::UDMASgeCtx) ==
    kDispatchUdmaWqeBytes, "MoonEP Dispatch WRITE WQE must occupy one basic block");
static_assert(sizeof(TileXR::UDMACqeCtx) == 64U,
    "MoonEP Dispatch UDMA CQE must occupy one cache line");
static_assert(kDispatchWqeBatchBytes == 8192U,
    "MoonEP Dispatch WQE batch must occupy 8 KiB of UB");
static_assert(sizeof(DispatchWqeOperatorContext) == 32U,
    "MoonEP Dispatch WQE operator context ABI changed");
static_assert(sizeof(DispatchWqePeerContext) == 96U,
    "MoonEP Dispatch WQE peer context ABI changed");
static_assert(sizeof(DispatchWqePatchContext) == 128U,
    "MoonEP Dispatch WQE patch context ABI changed");
static_assert(kDispatchUdmaIssueUbBytes % kUbAlignBytes == 0U,
    "MoonEP Dispatch issue UB must be 32-byte aligned");
static_assert(kDispatchCqePollBatchCapacity > 0U,
    "MoonEP Dispatch CQ poll relay must hold at least one CQE");
static_assert(kDispatchUdmaIssueUbBytes >= kLocalCopyTileBytes,
    "MoonEP Dispatch issue UB must hold one local-copy tile");
static_assert(kMaxRouteSelectSendUbBytes < kFullUbBytes,
    "MoonEP Dispatch route chunk exceeds the send UB budget");

#if defined(CATLASS_ARCH) && CATLASS_ARCH == 3510
__simt_vf__ __aicore__ LAUNCH_BOUND(kDispatchWqeBuildThreads)
inline void DispatchPrefillOperatorWqesVf(
    __ubuf__ uint8_t *qp0WqeBytes, __ubuf__ uint8_t *qp1WqeBytes,
    __ubuf__ const DispatchWqeOperatorContext *context)
{
    constexpr uint32_t templateCount =
        TileXRMoonEp::kDispatchQpCount *
        TileXRMoonEp::kDispatchWqeBatchCapacity;
    for (uint32_t task = static_cast<uint32_t>(threadIdx.x);
        task < templateCount; task += kDispatchWqeBuildThreads) {
        const uint32_t qpIdx = task / TileXRMoonEp::kDispatchWqeBatchCapacity;
        const uint32_t outputIndex =
            task % TileXRMoonEp::kDispatchWqeBatchCapacity;
        __ubuf__ uint8_t *wqeBytes = qpIdx == 0U ? qp0WqeBytes : qp1WqeBytes;
        __ubuf__ uint8_t *wqe = wqeBytes +
            outputIndex * kDispatchUdmaWqeBytes;
        __ubuf__ uint32_t *words =
            reinterpret_cast<__ubuf__ uint32_t *>(wqe);
        for (uint32_t word = 0U;
            word < kDispatchUdmaWqeBytes / sizeof(uint32_t); ++word) {
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
        sge->tokenId = 0U;
    }
}

__simt_vf__ __aicore__ LAUNCH_BOUND(kDispatchWqeBuildThreads)
inline void DispatchPatchWriteWqePeerFieldsVf(
    __ubuf__ uint8_t *qp0WqeBytes, __ubuf__ uint8_t *qp1WqeBytes,
    __ubuf__ const DispatchWqePeerContext *context)
{
    constexpr uint32_t templateCount =
        TileXRMoonEp::kDispatchQpCount *
        TileXRMoonEp::kDispatchWqeBatchCapacity;
    for (uint32_t task = static_cast<uint32_t>(threadIdx.x);
        task < templateCount; task += kDispatchWqeBuildThreads) {
        const uint32_t qpIdx = task / TileXRMoonEp::kDispatchWqeBatchCapacity;
        const uint32_t outputIndex =
            task % TileXRMoonEp::kDispatchWqeBatchCapacity;
        __ubuf__ uint8_t *wqeBytes = qpIdx == 0U ? qp0WqeBytes : qp1WqeBytes;
        __ubuf__ uint8_t *wqe = wqeBytes +
            outputIndex * kDispatchUdmaWqeBytes;
        __ubuf__ const DispatchWqePeerQpFields *fields =
            &context->qp[qpIdx];
        __ubuf__ TileXR::UDMASqeCtx *sqe =
            reinterpret_cast<__ubuf__ TileXR::UDMASqeCtx *>(wqe);
        sqe->tokenEn = fields->tokenEn;
        sqe->rmtJettyType = fields->rmtJettyType;
        sqe->targetHint = fields->targetHint;
        sqe->tpId = fields->tpId;
        sqe->rmtJettyOrSegId = fields->rmtJettyOrSegId;
        sqe->rmtTokenValue = fields->rmtTokenValue;
        sqe->rmtEidL = fields->rmtEidL;
        sqe->rmtEidH = fields->rmtEidH;
    }
}

__simt_vf__ __aicore__ LAUNCH_BOUND(kDispatchWqeBuildThreads)
inline void DispatchPatchWriteWqeBatchVf(__ubuf__ uint8_t *wqeBytes,
    __ubuf__ const int16_t *selectedRouteIndices,
    __ubuf__ const int32_t *dstValues,
    __ubuf__ const DispatchWqePatchContext *context)
{
    const uint32_t taskCount = context->dataTaskCount + context->appendSignal;
    for (uint32_t task = static_cast<uint32_t>(threadIdx.x);
        task < taskCount; task += kDispatchWqeBuildThreads) {
        const uint32_t outputIndex = context->batchOutputOffset + task;
        __ubuf__ uint8_t *wqe = wqeBytes +
            outputIndex * kDispatchUdmaWqeBytes;

        uint64_t localAddr = context->signalLocalAddr;
        uint64_t remoteAddr = context->signalRemoteAddr;
        uint32_t sqeFlag =
            TileXR::TILEXR_UDMA_SQE_FLAG_ORDERED_COMPLETION;
        uint64_t rowBytes = sizeof(uint64_t);
        if (task < context->dataTaskCount) {
            const uint32_t dataTask = context->dataTaskStart + task;
            const bool weightTask =
                TileXRMoonEp::DispatchDataTaskIsWeight(
                    dataTask, context->hasWeight != 0U);
            const uint32_t qpRouteIndex =
                TileXRMoonEp::DispatchDataTaskRouteIndex(
                    dataTask, context->hasWeight != 0U);
            const uint32_t qpIdx = (context->qpSelection >> 2U) & 1U;
            const uint32_t sequencePhase = context->qpSelection & 3U;
            const bool singleLane =
                (context->qpSelection & kDispatchQpSelectionSingleLane) != 0U;
            const uint32_t selectedIndex = singleLane ? qpRouteIndex :
                TileXRMoonEp::DispatchQpSelectedIndex(
                    qpRouteIndex, sequencePhase, qpIdx);
            const uint32_t routeInChunk = static_cast<uint16_t>(
                selectedRouteIndices[selectedIndex]);
            const uint32_t routeId =
                context->routePlanStart + routeInChunk;
            const uint64_t targetSlot = static_cast<uint64_t>(
                static_cast<uint32_t>(dstValues[routeInChunk])) -
                context->destinationBase;
            const uint32_t sourceRow = weightTask ? routeId :
                AscendC::Simt::UintDiv(routeId, context->topKMagic,
                    context->topKShift);
            rowBytes = weightTask ? context->weightRowBytes :
                context->hiddenRowBytes;
            localAddr = (weightTask ? context->weightLocalSourceBase :
                context->hiddenLocalSourceBase) +
                static_cast<uint64_t>(sourceRow) * rowBytes;
            remoteAddr = (weightTask ? context->weightRemoteScratchBase :
                context->hiddenRemoteScratchBase) + targetSlot * rowBytes;
            sqeFlag = 0U;
        }

        __ubuf__ TileXR::UDMASqeCtx *sqe =
            reinterpret_cast<__ubuf__ TileXR::UDMASqeCtx *>(wqe);
        sqe->flag = sqeFlag;
        const uint32_t wqeHead = context->batchHead + outputIndex;
        sqe->sqeBbIdx = static_cast<uint16_t>(
            wqeHead % TileXR::TILEXR_UDMA_SQ_BB_COUNT);
        sqe->owner =
            (wqeHead & TileXR::TILEXR_UDMA_SQ_BB_COUNT) == 0U ? 1U : 0U;
        sqe->rmtAddrLOrTokenId = remoteAddr & 0xFFFFFFFFU;
        sqe->rmtAddrHOrTokenValue =
            (remoteAddr >> 32U) & 0xFFFFFFFFU;

        __ubuf__ TileXR::UDMASgeCtx *sge =
            reinterpret_cast<__ubuf__ TileXR::UDMASgeCtx *>(
                wqe + sizeof(TileXR::UDMASqeCtx));
        sge->len = static_cast<uint32_t>(rowBytes);
        sge->tokenId = 0U;
        sge->va = localAddr;
    }
}
#endif

template <AscendC::HardEvent event>
__aicore__ inline void SyncFunc()
{
    const AscendC::TEventID eventId = GetTPipePtr()->FetchEventID(event);
    AscendC::SetFlag<event>(eventId);
    AscendC::WaitFlag<event>(eventId);
}

__aicore__ inline uint64_t CeilDiv(uint64_t value, uint64_t divisor)
{
    return value / divisor + (value % divisor == 0U ? 0U : 1U);
}

__aicore__ inline uint64_t AlignUp(uint64_t value, uint64_t alignment)
{
    return CeilDiv(value, alignment) * alignment;
}

__aicore__ inline uint64_t MultiplyU32ToU64(uint32_t lhs, uint32_t rhs)
{
    constexpr uint32_t kHalfMask = UINT16_MAX;
    const uint32_t lhsLow = lhs & kHalfMask;
    const uint32_t lhsHigh = lhs >> 16U;
    const uint32_t rhsLow = rhs & kHalfMask;
    const uint32_t rhsHigh = rhs >> 16U;
    return static_cast<uint64_t>(lhsLow * rhsLow) +
        (static_cast<uint64_t>(lhsLow * rhsHigh) << 16U) +
        (static_cast<uint64_t>(lhsHigh * rhsLow) << 16U) +
        (static_cast<uint64_t>(lhsHigh * rhsHigh) << 32U);
}

__aicore__ inline uint32_t DispatchRouteSelectChunkElements(
    uint64_t routeCount, uint32_t topK)
{
    if (routeCount < kVectorCompareMinElements || topK == 0U) {
        return 0U;
    }
    uint64_t chunkElements = routeCount < kRouteSelectChunkElements ?
        routeCount : static_cast<uint64_t>(kRouteSelectChunkElements);
    chunkElements -= chunkElements % topK;
    if (chunkElements < kVectorCompareMinElements ||
        chunkElements > static_cast<uint64_t>(INT16_MAX) + 1U) {
        return 0U;
    }
    return static_cast<uint32_t>(chunkElements);
}

__aicore__ inline bool ShouldFlushPartialDoorbell(
    uint64_t issuedWqes, uint64_t totalWqes)
{
    if (issuedWqes == 0U || issuedWqes >= totalWqes) {
        return false;
    }
    return issuedWqes == CeilDiv(totalWqes, 4U) ||
        issuedWqes == CeilDiv(totalWqes, 2U) ||
        issuedWqes == totalWqes - totalWqes / 4U;
}

__aicore__ inline void ReclaimDeferredSegment(const __gm__ TileXR::CommArgs *args,
    int32_t peer, uint32_t qpIdx, uint64_t issuedPeerWqes, uint32_t phase,
    uint32_t &dfxFlags, uint32_t &firstQuietStatus,
    uint32_t &firstQuietPhase)
{
    if (issuedPeerWqes == 0U || issuedPeerWqes % kDeferredReclaimWqes != 0U) {
        return;
    }
    TileXR::UDMAFlushQpDoorbell(args, peer, qpIdx);
    const uint32_t quietStatus = TileXR::UDMAQuietStatusOnQp(args, peer, qpIdx);
    if (quietStatus != 0U && firstQuietStatus == 0U) {
        firstQuietStatus = quietStatus;
        firstQuietPhase = phase;
        dfxFlags |= TileXRMoonEp::kDispatchDfxQuietError;
    }
}

struct DispatchWqeBatchInitContext {
    const __gm__ TileXR::CommArgs *args;
    __gm__ TileXR::UDMAInfo *udmaInfo;
    __gm__ TileXR::TileXRUDMAFullmeshDeviceView *fullmeshView;
    __gm__ TileXR::UDMAInfo *fullmeshInfo;
    __gm__ TileXR::TileXRUDMARegistry *registry;
    uint64_t hiddenRemoteScratchOffset;
    uint64_t hiddenScratchBytes;
    uint64_t weightRemoteScratchOffset;
    uint64_t weightScratchBytes;
    uint64_t remoteFlagBase;
    uint32_t coreIdx;
    int32_t rank;
    int32_t localRankSize;
    bool sharedQp;
    bool hasWeight;
};

struct DispatchWqeBatchState {
    const __gm__ TileXR::CommArgs *args;
    __gm__ TileXR::UDMAInfo *udmaInfo;
    __gm__ TileXR::UDMAWQCtx *qpCtxEntry;
    __gm__ TileXR::UDMACQCtx *cqCtxEntry;
    __gm__ uint8_t *hiddenRemoteScratchBase;
    __gm__ uint8_t *weightRemoteScratchBase;
    __gm__ uint8_t *remoteSignalAddr;
    uint64_t rmtEidL;
    uint64_t rmtEidH;
    int32_t targetRank;
    uint32_t qpIdx;
    uint32_t physicalQpIdx;
    uint32_t head;
    uint32_t completionCount;
    uint32_t tail;
    uint32_t cqTail;
    uint32_t outstanding;
    uint32_t batchCount;
    uint32_t batchLimit;
    uint32_t tokenEn;
    uint32_t rmtJettyType;
    uint32_t targetHint;
    uint32_t tpId;
    uint32_t rmtJettyOrSegId;
    uint32_t rmtTokenValue;
    uint32_t stagedDoorbellHead;
    uint32_t finalHead;
    uint32_t doorbellPending;
    uint32_t finalStaged;
    uint32_t doorbellRung;
    uint32_t singleLane;
};

struct DispatchPreparedPeer {
    int32_t targetRank;
    uint32_t issuePhase;
    bool initialized;
    uint32_t qpCount;
    bool fullmesh;
    DispatchWqeBatchState qpState[TileXRMoonEp::kDispatchQpCount];
};

__aicore__ inline bool DispatchFullmeshDeviceViewValid(
    const __gm__ TileXR::CommArgs *args,
    __gm__ TileXR::TileXRUDMAFullmeshDeviceView *view)
{
    if (args == nullptr || view == nullptr || args->rankSize <= 1 ||
        args->localRankSize <= 1 ||
        args->localRankSize >
            static_cast<int32_t>(TileXR::TILEXR_UDMA_FULLMESH_SLOT_COUNT) ||
        args->localRank < 0 || args->localRank >= args->localRankSize ||
        args->rank % args->localRankSize != args->localRank ||
        view->magic != TileXR::TILEXR_UDMA_FULLMESH_MAGIC ||
        view->version != TileXR::TILEXR_UDMA_FULLMESH_VERSION ||
        view->slotCount != TileXR::TILEXR_UDMA_FULLMESH_SLOT_COUNT ||
        view->localRank != static_cast<uint32_t>(args->localRank) ||
        view->connectedCount + 1U !=
            static_cast<uint32_t>(args->localRankSize) ||
        view->validPeerMask != TileXR::UDMAFullmeshExpectedPeerMask(
            static_cast<uint32_t>(args->localRank),
            static_cast<uint32_t>(args->localRankSize)) ||
        view->registrationReady == 0U ||
        view->registrationGeneration == 0U ||
        view->registrationGeneration != args->udmaRegistrationGeneration ||
        view->infoPtr == 0U) {
        return false;
    }
    auto info = reinterpret_cast<__gm__ TileXR::UDMAInfo *>(view->infoPtr);
    return info != nullptr &&
        info->qpNum == TileXR::TILEXR_UDMA_FULLMESH_SLOT_COUNT &&
        info->sqPtr != 0U && info->scqPtr != 0U && info->memPtr != 0U;
}

__aicore__ inline bool InitDispatchWqeBatchInitContext(
    const __gm__ TileXR::CommArgs *args, uint64_t hiddenRemoteScratchOffset,
    uint64_t hiddenScratchBytes, uint64_t weightRemoteScratchOffset,
    uint64_t weightScratchBytes, uint64_t remoteFlagBase, uint32_t coreIdx,
    bool hasWeight, bool allowFullmesh,
    DispatchWqeBatchInitContext &context)
{
    if (!TileXR::UDMARegistryEnabled(args)) {
        return false;
    }
    const bool sharedQp =
        (args->extraFlag & TileXR::ExtraFlag::UDMA_SHARED_QP) != 0U;
    __gm__ TileXR::UDMAInfo *udmaInfo = TileXR::GetUDMAInfo(args);
    if (udmaInfo == nullptr ||
        !TileXRMoonEp::DispatchQpCountSupported(
            udmaInfo->qpNum, sharedQp)) {
        return false;
    }
    __gm__ TileXR::TileXRUDMARegistry *registry = TileXR::GetUDMARegistry(args);
    if (registry == nullptr) {
        return false;
    }
    context.args = args;
    context.udmaInfo = udmaInfo;
    context.fullmeshView = nullptr;
    context.fullmeshInfo = nullptr;
    if (allowFullmesh &&
        (args->extraFlag & TileXR::ExtraFlag::UDMA_FULLMESH) != 0U &&
        args->udmaFullmeshPtr != nullptr) {
        auto fullmeshView = reinterpret_cast<__gm__
            TileXR::TileXRUDMAFullmeshDeviceView *>(args->udmaFullmeshPtr);
        if (DispatchFullmeshDeviceViewValid(args, fullmeshView)) {
            context.fullmeshView = fullmeshView;
            context.fullmeshInfo = reinterpret_cast<__gm__ TileXR::UDMAInfo *>(
                fullmeshView->infoPtr);
        }
    }
    context.registry = registry;
    context.hiddenRemoteScratchOffset = hiddenRemoteScratchOffset;
    context.hiddenScratchBytes = hiddenScratchBytes;
    context.weightRemoteScratchOffset = weightRemoteScratchOffset;
    context.weightScratchBytes = weightScratchBytes;
    context.remoteFlagBase = remoteFlagBase;
    context.coreIdx = coreIdx;
    context.rank = args->rank;
    context.localRankSize = args->localRankSize;
    context.sharedQp = sharedQp;
    context.hasWeight = hasWeight;
    return true;
}

__aicore__ inline bool InitDispatchWqeBatchState(
    const DispatchWqeBatchInitContext &context, int32_t targetRank,
    uint32_t qpIdx, __gm__ uint8_t *hiddenRemoteScratchBase,
    __gm__ uint8_t *weightRemoteScratchBase,
    __gm__ uint8_t *remoteSignalAddr, DispatchWqeBatchState &state)
{
    if (qpIdx >= TileXRMoonEp::kDispatchQpCount) {
        return false;
    }
    const uint32_t physicalQpIdx =
        TileXRMoonEp::DispatchPhysicalQpIndex(
            qpIdx, context.coreIdx, context.sharedQp);
    if (physicalQpIdx >= context.udmaInfo->qpNum) {
        return false;
    }

    __gm__ TileXR::UDMAWQCtx *qpCtxEntry = TileXR::UDMAGetWQCtx(
        context.udmaInfo, static_cast<uint32_t>(targetRank), physicalQpIdx);
    __gm__ TileXR::UDMACQCtx *cqCtxEntry = TileXR::UDMAGetSCQCtx(
        context.udmaInfo, static_cast<uint32_t>(targetRank), physicalQpIdx);
    if (qpCtxEntry == nullptr || qpCtxEntry->baseBkShift >= 32U ||
        (1U << qpCtxEntry->baseBkShift) != kDispatchUdmaWqeBytes ||
        qpCtxEntry->depth != TileXR::TILEXR_UDMA_SQ_BB_COUNT ||
        cqCtxEntry == nullptr || cqCtxEntry->baseBkShift >= 32U ||
        (1U << cqCtxEntry->baseBkShift) != sizeof(TileXR::UDMACqeCtx) ||
        cqCtxEntry->depth != TileXR::TILEXR_UDMA_CQ_DEPTH) {
        return false;
    }
    __gm__ TileXR::UDMAMemInfo *remoteMemInfo = TileXR::UDMAGetRemoteMemInfo(
        context.udmaInfo, static_cast<uint32_t>(targetRank), physicalQpIdx);
    if (remoteMemInfo == nullptr || remoteMemInfo->eidAddr == 0U) {
        return false;
    }
    __gm__ uint64_t *remoteEid = reinterpret_cast<__gm__ uint64_t *>(
        remoteMemInfo->eidAddr);

    state.args = context.args;
    state.udmaInfo = context.udmaInfo;
    state.qpCtxEntry = qpCtxEntry;
    state.cqCtxEntry = cqCtxEntry;
    state.hiddenRemoteScratchBase = hiddenRemoteScratchBase;
    state.weightRemoteScratchBase = weightRemoteScratchBase;
    state.remoteSignalAddr = remoteSignalAddr;
    state.rmtEidL = remoteEid[0];
    state.rmtEidH = remoteEid[1];
    state.targetRank = targetRank;
    state.qpIdx = qpIdx;
    state.physicalQpIdx = physicalQpIdx;
    state.head = ld_dev(reinterpret_cast<__gm__ uint32_t *>(
        qpCtxEntry->headAddr), 0);
    state.completionCount = ld_dev(reinterpret_cast<__gm__ uint32_t *>(
        qpCtxEntry->wqeCntAddr), 0);
    state.tail = ld_dev(reinterpret_cast<__gm__ uint32_t *>(
        qpCtxEntry->tailAddr), 0);
    state.cqTail = ld_dev(reinterpret_cast<__gm__ uint32_t *>(
        cqCtxEntry->tailAddr), 0);
    state.outstanding = state.head - state.tail;
    state.batchCount = 0U;
    state.batchLimit = TileXRMoonEp::DispatchWqeBatchCount(
        UINT64_MAX, state.head, TileXR::TILEXR_UDMA_SQ_BB_COUNT);
    state.tokenEn = remoteMemInfo->tokenValueValid;
    state.rmtJettyType = remoteMemInfo->rmtJettyType;
    state.targetHint = remoteMemInfo->targetHint;
    state.tpId = remoteMemInfo->tpn;
    state.rmtJettyOrSegId = remoteMemInfo->tid;
    state.rmtTokenValue = remoteMemInfo->rmtTokenValue;
    state.stagedDoorbellHead = state.head;
    state.finalHead = state.head;
    state.doorbellPending = 0U;
    state.finalStaged = 0U;
    state.doorbellRung = 0U;
    state.singleLane = 0U;
    return state.batchLimit != 0U &&
        state.outstanding < TileXR::TILEXR_UDMA_SQ_BB_COUNT;
}

__aicore__ inline bool InitDispatchFullmeshWqeBatchState(
    const DispatchWqeBatchInitContext &context, int32_t targetRank,
    __gm__ uint8_t *hiddenRemoteScratchBase,
    __gm__ uint8_t *weightRemoteScratchBase,
    __gm__ uint8_t *remoteSignalAddr, DispatchWqeBatchState &state)
{
    const uint32_t slot = TileXRMoonEp::DispatchFullmeshSlot(
        targetRank, context.localRankSize);
    if (context.fullmeshView == nullptr || context.fullmeshInfo == nullptr ||
        !TileXRMoonEp::DispatchSameServer(
            context.rank, targetRank, context.localRankSize) ||
        slot >= TileXR::TILEXR_UDMA_FULLMESH_SLOT_COUNT ||
        (context.fullmeshView->validPeerMask & (1U << slot)) == 0U) {
        return false;
    }
    __gm__ TileXR::UDMAWQCtx *qpCtxEntry = TileXR::UDMAGetWQCtx(
        context.fullmeshInfo, static_cast<uint32_t>(targetRank), slot);
    __gm__ TileXR::UDMACQCtx *cqCtxEntry = TileXR::UDMAGetSCQCtx(
        context.fullmeshInfo, static_cast<uint32_t>(targetRank), slot);
    __gm__ TileXR::UDMAMemInfo *remoteMemInfo =
        TileXR::UDMAGetRemoteMemInfo(
            context.fullmeshInfo, static_cast<uint32_t>(targetRank), slot);
    if (qpCtxEntry == nullptr || qpCtxEntry->baseBkShift >= 32U ||
        (1U << qpCtxEntry->baseBkShift) != kDispatchUdmaWqeBytes ||
        qpCtxEntry->depth != TileXR::TILEXR_UDMA_SQ_BB_COUNT ||
        cqCtxEntry == nullptr || cqCtxEntry->baseBkShift >= 32U ||
        (1U << cqCtxEntry->baseBkShift) != sizeof(TileXR::UDMACqeCtx) ||
        cqCtxEntry->depth != TileXR::TILEXR_UDMA_CQ_DEPTH ||
        remoteMemInfo == nullptr || remoteMemInfo->eidAddr == 0U) {
        return false;
    }
    __gm__ uint64_t *remoteEid = reinterpret_cast<__gm__ uint64_t *>(
        remoteMemInfo->eidAddr);
    state.args = context.args;
    state.udmaInfo = context.fullmeshInfo;
    state.qpCtxEntry = qpCtxEntry;
    state.cqCtxEntry = cqCtxEntry;
    state.hiddenRemoteScratchBase = hiddenRemoteScratchBase;
    state.weightRemoteScratchBase = weightRemoteScratchBase;
    state.remoteSignalAddr = remoteSignalAddr;
    state.rmtEidL = remoteEid[0];
    state.rmtEidH = remoteEid[1];
    state.targetRank = targetRank;
    state.qpIdx = 0U;
    state.physicalQpIdx = slot;
    state.head = ld_dev(reinterpret_cast<__gm__ uint32_t *>(
        qpCtxEntry->headAddr), 0);
    state.completionCount = ld_dev(reinterpret_cast<__gm__ uint32_t *>(
        qpCtxEntry->wqeCntAddr), 0);
    state.tail = ld_dev(reinterpret_cast<__gm__ uint32_t *>(
        qpCtxEntry->tailAddr), 0);
    state.cqTail = ld_dev(reinterpret_cast<__gm__ uint32_t *>(
        cqCtxEntry->tailAddr), 0);
    state.outstanding = state.head - state.tail;
    state.batchCount = 0U;
    state.batchLimit = TileXRMoonEp::DispatchWqeBatchCount(
        UINT64_MAX, state.head, TileXR::TILEXR_UDMA_SQ_BB_COUNT);
    state.tokenEn = remoteMemInfo->tokenValueValid;
    state.rmtJettyType = remoteMemInfo->rmtJettyType;
    state.targetHint = remoteMemInfo->targetHint;
    state.tpId = remoteMemInfo->tpn;
    state.rmtJettyOrSegId = remoteMemInfo->tid;
    state.rmtTokenValue = remoteMemInfo->rmtTokenValue;
    state.stagedDoorbellHead = state.head;
    state.finalHead = state.head;
    state.doorbellPending = 0U;
    state.finalStaged = 0U;
    state.doorbellRung = 0U;
    state.singleLane = 1U;
    return state.batchLimit != 0U &&
        state.outstanding < TileXR::TILEXR_UDMA_SQ_BB_COUNT;
}

__aicore__ inline bool InitDispatchCreditQpState(
    const __gm__ TileXR::CommArgs *args, __gm__ TileXR::UDMAInfo *udmaInfo,
    int32_t targetRank, uint32_t physicalQpIdx,
    DispatchWqeBatchState &state)
{
    if (args == nullptr || udmaInfo == nullptr || targetRank < 0 ||
        physicalQpIdx >= udmaInfo->qpNum) {
        return false;
    }
    __gm__ TileXR::UDMAWQCtx *qpCtxEntry = TileXR::UDMAGetWQCtx(
        udmaInfo, static_cast<uint32_t>(targetRank), physicalQpIdx);
    __gm__ TileXR::UDMACQCtx *cqCtxEntry = TileXR::UDMAGetSCQCtx(
        udmaInfo, static_cast<uint32_t>(targetRank), physicalQpIdx);
    if (qpCtxEntry == nullptr || qpCtxEntry->baseBkShift >= 32U ||
        (1U << qpCtxEntry->baseBkShift) != kDispatchUdmaWqeBytes ||
        qpCtxEntry->depth != TileXR::TILEXR_UDMA_SQ_BB_COUNT ||
        cqCtxEntry == nullptr || cqCtxEntry->baseBkShift >= 32U ||
        (1U << cqCtxEntry->baseBkShift) != sizeof(TileXR::UDMACqeCtx) ||
        cqCtxEntry->depth != TileXR::TILEXR_UDMA_CQ_DEPTH) {
        return false;
    }

    state.args = args;
    state.udmaInfo = udmaInfo;
    state.qpCtxEntry = qpCtxEntry;
    state.cqCtxEntry = cqCtxEntry;
    state.targetRank = targetRank;
    state.qpIdx = physicalQpIdx;
    state.physicalQpIdx = physicalQpIdx;
    state.head = ld_dev(reinterpret_cast<__gm__ uint32_t *>(
        qpCtxEntry->headAddr), 0);
    state.completionCount = ld_dev(reinterpret_cast<__gm__ uint32_t *>(
        qpCtxEntry->wqeCntAddr), 0);
    state.tail = ld_dev(reinterpret_cast<__gm__ uint32_t *>(
        qpCtxEntry->tailAddr), 0);
    state.cqTail = ld_dev(reinterpret_cast<__gm__ uint32_t *>(
        cqCtxEntry->tailAddr), 0);
    state.outstanding = state.head - state.tail;
    state.batchCount = 0U;
    state.batchLimit = TileXRMoonEp::DispatchWqeBatchCount(
        UINT64_MAX, state.head, TileXR::TILEXR_UDMA_SQ_BB_COUNT);
    state.stagedDoorbellHead = state.head;
    state.finalHead = state.head;
    state.doorbellPending = 0U;
    state.finalStaged = 0U;
    state.doorbellRung = 0U;
    state.singleLane = 0U;
    return state.batchLimit != 0U &&
        state.outstanding < TileXR::TILEXR_UDMA_SQ_BB_COUNT;
}

__aicore__ inline void RecordDispatchCreditQpPost(
    DispatchWqeBatchState &state, int32_t targetRank)
{
    state.targetRank = targetRank;
    ++state.head;
    ++state.completionCount;
    ++state.outstanding;
    state.finalHead = state.head;
    state.batchLimit = TileXRMoonEp::DispatchWqeBatchCount(
        UINT64_MAX, state.head, TileXR::TILEXR_UDMA_SQ_BB_COUNT);
}

__aicore__ inline bool InitDispatchPreparedPeer(
    const DispatchWqeBatchInitContext &context, int32_t targetRank,
    uint32_t issuePhase, DispatchPreparedPeer &preparedPeer)
{
    const uint64_t remoteFlagBytes =
        TileXRMoonEp::kDispatchQpCount * sizeof(uint64_t);
    preparedPeer.targetRank = targetRank;
    preparedPeer.issuePhase = issuePhase;
    preparedPeer.initialized = false;
    preparedPeer.fullmesh = context.fullmeshInfo != nullptr &&
        TileXRMoonEp::DispatchSameServer(
            context.rank, targetRank, context.localRankSize);
    preparedPeer.qpCount = preparedPeer.fullmesh ? 1U :
        TileXRMoonEp::kDispatchQpCount;
    if (!TileXR::UDMARegisteredRangeValid(context.registry, targetRank,
            context.hiddenRemoteScratchOffset, context.hiddenScratchBytes) ||
        (context.hasWeight && !TileXR::UDMARegisteredRangeValid(
            context.registry, targetRank, context.weightRemoteScratchOffset,
            context.weightScratchBytes)) ||
        !TileXR::UDMARegisteredRangeValid(context.registry, targetRank,
            context.remoteFlagBase, remoteFlagBytes)) {
        return false;
    }
    __gm__ uint8_t *hiddenRemoteScratchBase = TileXR::UDMARegisteredRemoteAddr(
        context.registry, targetRank, context.hiddenRemoteScratchOffset);
    __gm__ uint8_t *weightRemoteScratchBase = context.hasWeight ?
        TileXR::UDMARegisteredRemoteAddr(context.registry, targetRank,
            context.weightRemoteScratchOffset) : nullptr;
    __gm__ uint8_t *remoteFlagBase = TileXR::UDMARegisteredRemoteAddr(
        context.registry, targetRank, context.remoteFlagBase);
    if (preparedPeer.fullmesh) {
        if (!InitDispatchFullmeshWqeBatchState(context, targetRank,
                hiddenRemoteScratchBase, weightRemoteScratchBase,
                remoteFlagBase, preparedPeer.qpState[0])) {
            return false;
        }
        preparedPeer.qpState[1] = preparedPeer.qpState[0];
    } else {
        for (uint32_t qpIdx = 0U;
            qpIdx < TileXRMoonEp::kDispatchQpCount; ++qpIdx) {
            if (!InitDispatchWqeBatchState(context, targetRank, qpIdx,
                    hiddenRemoteScratchBase, weightRemoteScratchBase,
                    remoteFlagBase + qpIdx * sizeof(uint64_t),
                    preparedPeer.qpState[qpIdx])) {
                return false;
            }
        }
    }
    preparedPeer.initialized = true;
    return true;
}

__aicore__ inline bool PrefillDispatchOperatorWqes(
    AscendC::LocalTensor<uint8_t> qp0IssueLocal,
    AscendC::LocalTensor<uint8_t> qp1IssueLocal)
{
    __ubuf__ uint8_t *qp0IssueAddr = reinterpret_cast<__ubuf__ uint8_t *>(
        qp0IssueLocal.GetPhyAddr());
    __ubuf__ uint8_t *qp1IssueAddr = reinterpret_cast<__ubuf__ uint8_t *>(
        qp1IssueLocal.GetPhyAddr());
    __ubuf__ DispatchWqeOperatorContext *context =
        reinterpret_cast<__ubuf__ DispatchWqeOperatorContext *>(
            qp0IssueAddr + kDispatchWqeBatchContextOffset);
    context->reserved[0] = 0U;

#if defined(CATLASS_ARCH) && CATLASS_ARCH == 3510
    AscendC::PipeBarrier<PIPE_ALL>();
    AscendC::Simt::VF_CALL<DispatchPrefillOperatorWqesVf>(
        AscendC::Simt::Dim3{kDispatchWqeBuildThreads, 1U, 1U},
        qp0IssueAddr, qp1IssueAddr,
        reinterpret_cast<__ubuf__ const DispatchWqeOperatorContext *>(context));
    AscendC::PipeBarrier<PIPE_ALL>();
    return true;
#else
    return false;
#endif
}

__aicore__ inline bool PrefillDispatchPeerWqes(
    AscendC::LocalTensor<uint8_t> qp0IssueLocal,
    AscendC::LocalTensor<uint8_t> qp1IssueLocal,
    const DispatchPreparedPeer &preparedPeer)
{
    if (!preparedPeer.initialized) {
        return false;
    }
    __ubuf__ uint8_t *qp0IssueAddr = reinterpret_cast<__ubuf__ uint8_t *>(
        qp0IssueLocal.GetPhyAddr());
    __ubuf__ uint8_t *qp1IssueAddr = reinterpret_cast<__ubuf__ uint8_t *>(
        qp1IssueLocal.GetPhyAddr());
    __ubuf__ DispatchWqePeerContext *context =
        reinterpret_cast<__ubuf__ DispatchWqePeerContext *>(
            qp0IssueAddr + kDispatchWqeBatchContextOffset);
    for (uint32_t qpIdx = 0U;
        qpIdx < TileXRMoonEp::kDispatchQpCount; ++qpIdx) {
        const DispatchWqeBatchState &state = preparedPeer.qpState[qpIdx];
        __ubuf__ DispatchWqePeerQpFields *fields = &context->qp[qpIdx];
        fields->rmtEidL = state.rmtEidL;
        fields->rmtEidH = state.rmtEidH;
        fields->tokenEn = state.tokenEn;
        fields->rmtJettyType = state.rmtJettyType;
        fields->targetHint = state.targetHint;
        fields->tpId = state.tpId;
        fields->rmtJettyOrSegId = state.rmtJettyOrSegId;
        fields->rmtTokenValue = state.rmtTokenValue;
    }

#if defined(CATLASS_ARCH) && CATLASS_ARCH == 3510
    AscendC::PipeBarrier<PIPE_ALL>();
    AscendC::Simt::VF_CALL<DispatchPatchWriteWqePeerFieldsVf>(
        AscendC::Simt::Dim3{kDispatchWqeBuildThreads, 1U, 1U},
        qp0IssueAddr, qp1IssueAddr,
        reinterpret_cast<__ubuf__ const DispatchWqePeerContext *>(context));
    AscendC::PipeBarrier<PIPE_ALL>();
    return true;
#else
    return false;
#endif
}

__aicore__ inline void PrepareDispatchRemotePeerBatch(
    const DispatchWqeBatchInitContext *context, int32_t rank,
    int32_t rankSize, int64_t groupCount, uint32_t peerWorkCount,
    uint32_t blockIdx, uint32_t blockNum, uint64_t &peerCursor,
    DispatchPreparedPeer *peerBatch, uint32_t &peerCount)
{
    peerCount = 0U;
    const uint64_t totalPeerAssignments =
        static_cast<uint64_t>(groupCount) * peerWorkCount;
    while (peerCursor < totalPeerAssignments &&
        peerCount < kDispatchPreparedPeerCapacity) {
        const uint64_t issuePhase = peerCursor / peerWorkCount;
        const uint32_t peerWork = static_cast<uint32_t>(
            peerCursor % peerWorkCount);
        ++peerCursor;
        const int64_t peer = TileXRMoonEp::DispatchPeerForCore(
            rank, rankSize, static_cast<int64_t>(issuePhase), blockIdx,
            blockNum, peerWork);
        if (peer < 0) {
            continue;
        }
        if (peer == rank) {
            continue;
        }
        DispatchPreparedPeer &preparedPeer = peerBatch[peerCount];
        preparedPeer.targetRank = static_cast<int32_t>(peer);
        preparedPeer.issuePhase = static_cast<uint32_t>(issuePhase);
        preparedPeer.initialized = context != nullptr &&
            InitDispatchPreparedPeer(*context, static_cast<int32_t>(peer),
                static_cast<uint32_t>(issuePhase), preparedPeer);
        ++peerCount;
    }
}

__aicore__ inline uint32_t SelectDispatchPeerRoutes(
    AscendC::LocalTensor<uint8_t> lowerMaskLocal,
    AscendC::LocalTensor<uint8_t> upperMaskLocal,
    AscendC::LocalTensor<int32_t> routePlanLocal,
    AscendC::LocalTensor<int16_t> routeIndexLocal,
    AscendC::LocalTensor<int16_t> selectedRouteIndexLocal,
    int32_t peer, uint64_t destinationCapacity, uint32_t routeCount,
    uint32_t compareMaskElements)
{
    const uint64_t peerBase = static_cast<uint64_t>(
        static_cast<uint32_t>(peer)) * destinationCapacity;
    const uint64_t peerEnd = peerBase + destinationCapacity;
    if (peerBase > static_cast<uint64_t>(INT32_MAX)) {
        return 0U;
    }
    AscendC::Compares(lowerMaskLocal, routePlanLocal,
        static_cast<int32_t>(peerBase), AscendC::CMPMODE::GE, routeCount);
    if (peerEnd <= static_cast<uint64_t>(INT32_MAX)) {
        AscendC::Compares(upperMaskLocal, routePlanLocal,
            static_cast<int32_t>(peerEnd), AscendC::CMPMODE::LT, routeCount);
        AscendC::PipeBarrier<PIPE_V>();
        AscendC::And(lowerMaskLocal.ReinterpretCast<uint16_t>(),
            lowerMaskLocal.ReinterpretCast<uint16_t>(),
            upperMaskLocal.ReinterpretCast<uint16_t>(),
            static_cast<int32_t>(compareMaskElements));
    }
    AscendC::PipeBarrier<PIPE_V>();
    uint64_t selectedCount = 0U;
    AscendC::GatherMask(selectedRouteIndexLocal, routeIndexLocal,
        lowerMaskLocal.ReinterpretCast<uint16_t>(), true, routeCount,
        {1U, 1U, 0U, 0U}, selectedCount);
    SyncFunc<AscendC::HardEvent::V_S>();
    return static_cast<uint32_t>(selectedCount);
}

__aicore__ inline void LoadDispatchRouteChunk(
    AscendC::GlobalTensor<int32_t> dstGlobal, uint64_t chunkStart,
    uint32_t chunkElements, AscendC::LocalTensor<int32_t> routePlanLocal,
    bool waitForReuse)
{
    if (waitForReuse) {
        SyncFunc<AscendC::HardEvent::S_MTE2>();
    }
    const AscendC::DataCopyExtParams copyIn {
        1U, chunkElements * static_cast<uint32_t>(sizeof(int32_t)),
        0U, 0U, 0U};
    const AscendC::DataCopyPadExtParams<int32_t> padIn {
        false, 0U, 0U, 0U};
    AscendC::DataCopyPad(routePlanLocal, dstGlobal[chunkStart], copyIn, padIn);
    SyncFunc<AscendC::HardEvent::MTE2_V>();
    if (waitForReuse) {
        SyncFunc<AscendC::HardEvent::S_V>();
    }
}

__aicore__ inline bool BuildDispatchWriteWqeBatch(
    AscendC::LocalTensor<uint8_t> issueLocal,
    AscendC::LocalTensor<int16_t> selectedRouteIndices,
    AscendC::LocalTensor<int32_t> dstValues,
    DispatchWqeBatchState &state, uint64_t hiddenLocalSourceBase,
    uint64_t hiddenRowBytes, uint64_t weightLocalSourceBase,
    uint64_t weightRowBytes, bool hasWeight, uint64_t destinationCapacity,
    uint32_t topKMagic, uint32_t topKShift, uint32_t dataTaskStart,
    uint32_t dataTaskCount, bool appendSignal, uint64_t signalLocalAddr,
    uint32_t sequencePhase, uint32_t routePlanStart)
{
    __ubuf__ uint8_t *issueAddr = reinterpret_cast<__ubuf__ uint8_t *>(
        issueLocal.GetPhyAddr());
    __ubuf__ DispatchWqePatchContext *context =
        reinterpret_cast<__ubuf__ DispatchWqePatchContext *>(
            issueAddr + kDispatchWqeBatchContextOffset);
    context->hiddenLocalSourceBase = hiddenLocalSourceBase;
    context->hiddenRemoteScratchBase =
        reinterpret_cast<uint64_t>(state.hiddenRemoteScratchBase);
    context->hiddenRowBytes = hiddenRowBytes;
    context->weightLocalSourceBase = weightLocalSourceBase;
    context->weightRemoteScratchBase =
        reinterpret_cast<uint64_t>(state.weightRemoteScratchBase);
    context->weightRowBytes = weightRowBytes;
    context->destinationBase = static_cast<uint64_t>(
        static_cast<uint32_t>(state.targetRank)) * destinationCapacity;
    context->signalLocalAddr = signalLocalAddr;
    context->signalRemoteAddr =
        reinterpret_cast<uint64_t>(state.remoteSignalAddr);
    context->batchHead = state.head;
    context->batchOutputOffset = state.batchCount;
    context->dataTaskCount = dataTaskCount;
    context->appendSignal = appendSignal ? 1U : 0U;
    context->topKMagic = topKMagic;
    context->topKShift = topKShift;
    context->hasWeight = hasWeight ? 1U : 0U;
    context->dataTaskStart = dataTaskStart;
    context->qpSelection =
        (state.qpIdx << 2U) | (sequencePhase & 3U) |
        (state.singleLane != 0U ? kDispatchQpSelectionSingleLane : 0U);
    context->routePlanStart = routePlanStart;

#if defined(CATLASS_ARCH) && CATLASS_ARCH == 3510
    AscendC::PipeBarrier<PIPE_ALL>();
    AscendC::Simt::VF_CALL<DispatchPatchWriteWqeBatchVf>(
        AscendC::Simt::Dim3{kDispatchWqeBuildThreads, 1U, 1U},
        issueAddr,
        reinterpret_cast<__ubuf__ const int16_t *>(
            selectedRouteIndices.GetPhyAddr()),
        reinterpret_cast<__ubuf__ const int32_t *>(
            dstValues.GetPhyAddr()),
        reinterpret_cast<__ubuf__ const DispatchWqePatchContext *>(context));
    AscendC::PipeBarrier<PIPE_ALL>();
    return true;
#else
    return false;
#endif
}

__aicore__ inline uint32_t DispatchPollCqBatch(
    DispatchWqeBatchState &state, AscendC::LocalTensor<uint8_t> cqeLocal)
{
    const uint32_t pollCount = TileXRMoonEp::DispatchCqePollBatchCount(
        state.cqTail, TileXR::TILEXR_UDMA_CQ_DEPTH,
        kDispatchCqePollBatchCapacity);
    if (pollCount == 0U) {
        return 0xFFU;
    }

    const uint32_t cqeBytes =
        pollCount * static_cast<uint32_t>(sizeof(TileXR::UDMACqeCtx));
    __gm__ uint8_t *cqeAddr = reinterpret_cast<__gm__ uint8_t *>(
        state.cqCtxEntry->bufAddr + sizeof(TileXR::UDMACqeCtx) *
            (state.cqTail % TileXR::TILEXR_UDMA_CQ_DEPTH));
    AscendC::GlobalTensor<uint8_t> cqeGlobal;
    cqeGlobal.SetGlobalBuffer(cqeAddr, cqeBytes);
    const AscendC::DataCopyExtParams copyParams {
        1U, cqeBytes, 0U, 0U, 0U};
    const AscendC::DataCopyPadExtParams<uint8_t> padParams {
        false, 0U, 0U, 0U};

    for (uint32_t retry = 0U;
        retry < TileXR::TILEXR_UDMA_MAX_RETRY_TIMES; ++retry) {
        TileXR::UDMACleanCacheLines(cqeAddr, cqeBytes);
        SyncFunc<AscendC::HardEvent::S_MTE2>();
        AscendC::DataCopyPad(cqeLocal, cqeGlobal, copyParams, padParams);
        SyncFunc<AscendC::HardEvent::MTE2_S>();

        __ubuf__ TileXR::UDMACqeCtx *cqeRecords =
            reinterpret_cast<__ubuf__ TileXR::UDMACqeCtx *>(
                cqeLocal.GetPhyAddr());
        uint32_t validCount = 0U;
        uint32_t completedSqTail = state.tail;
        for (; validCount < pollCount; ++validCount) {
            const uint32_t absoluteCqTail = state.cqTail + validCount;
            __ubuf__ const TileXR::UDMACqeCtx *cqe =
                cqeRecords + validCount;
            if (!TileXRMoonEp::DispatchCqeOwnerReady(
                    absoluteCqTail, TileXR::TILEXR_UDMA_CQ_DEPTH,
                    cqe->owner)) {
                break;
            }
            const uint32_t status = static_cast<uint32_t>(cqe->status);
            const uint32_t subStatus = static_cast<uint32_t>(cqe->substatus);
            if (status != 0U || subStatus != 0U) {
                return (status << 8U) | subStatus;
            }
            uint32_t candidateSqTail = state.tail;
            if (!TileXRMoonEp::DispatchCompletedSqTail(
                    state.tail, state.outstanding,
                    static_cast<uint32_t>(cqe->entryIdx),
                    TileXR::TILEXR_UDMA_SQ_BB_COUNT, candidateSqTail)) {
                return 0xFEU;
            }
            if (TileXRMoonEp::DispatchSqTailIsFurther(
                    state.tail, candidateSqTail, completedSqTail)) {
                completedSqTail = candidateSqTail;
            }
        }
        if (validCount == 0U) {
            continue;
        }

        const uint32_t newCqTail = state.cqTail + validCount;
        st_dev(newCqTail, reinterpret_cast<__gm__ uint32_t *>(
            state.cqCtxEntry->tailAddr), 0);
        st_dev(newCqTail & 0xFFFFFFU,
            reinterpret_cast<__gm__ uint32_t *>(
                state.cqCtxEntry->dbAddr), 0);
        st_dev(completedSqTail, reinterpret_cast<__gm__ uint32_t *>(
            state.qpCtxEntry->tailAddr), 0);
        state.cqTail = newCqTail;
        state.tail = completedSqTail;
        state.outstanding = state.head - state.tail;
        return 0U;
    }
    return 0xFFU;
}

__aicore__ inline bool DispatchEnsureSqBatchCapacity(
    DispatchWqeBatchState &state, uint32_t batchCount,
    AscendC::LocalTensor<uint8_t> cqeLocal, uint32_t phase,
    uint32_t &dfxFlags, uint32_t &firstQuietStatus,
    uint32_t &firstQuietPhase)
{
    while (static_cast<uint64_t>(state.outstanding) + batchCount >
        TileXR::TILEXR_UDMA_SQ_BB_COUNT - kDispatchSqPollReserve) {
        const uint32_t pollStatus = DispatchPollCqBatch(state, cqeLocal);
        if (pollStatus != 0U) {
            if (firstQuietStatus == 0U) {
                firstQuietStatus = pollStatus;
                firstQuietPhase = phase;
            }
            dfxFlags |= TileXRMoonEp::kDispatchDfxQuietError;
            return false;
        }
    }
    return true;
}

__aicore__ inline bool ReserveDispatchPeerChunkSq(
    DispatchPreparedPeer &peer,
    const uint32_t qpChunkWqeCount[TileXRMoonEp::kDispatchQpCount],
    AscendC::LocalTensor<uint8_t> cqeLocal, uint32_t phase,
    uint32_t &dfxFlags, uint32_t &firstQuietStatus,
    uint32_t &firstQuietPhase)
{
    constexpr uint32_t usableSqEntries =
        TileXR::TILEXR_UDMA_SQ_BB_COUNT - kDispatchSqPollReserve;
    for (uint32_t qpIdx = 0U; qpIdx < peer.qpCount;
        ++qpIdx) {
        const uint32_t chunkWqeCount = qpChunkWqeCount[qpIdx];
        if (chunkWqeCount == 0U) {
            continue;
        }
        if (chunkWqeCount > usableSqEntries ||
            !DispatchEnsureSqBatchCapacity(peer.qpState[qpIdx],
                chunkWqeCount, cqeLocal, phase, dfxFlags,
                firstQuietStatus, firstQuietPhase)) {
            return false;
        }
    }
    return true;
}

__aicore__ inline bool DispatchPeerChunkFitsWithoutCq(
    const DispatchPreparedPeer &peer,
    const uint32_t qpChunkWqeCount[TileXRMoonEp::kDispatchQpCount])
{
    constexpr uint32_t usableSqEntries =
        TileXR::TILEXR_UDMA_SQ_BB_COUNT - kDispatchSqPollReserve;
    for (uint32_t qpIdx = 0U; qpIdx < peer.qpCount; ++qpIdx) {
        if (static_cast<uint64_t>(peer.qpState[qpIdx].outstanding) +
                qpChunkWqeCount[qpIdx] > usableSqEntries) {
            return false;
        }
    }
    return true;
}

__aicore__ inline bool SubmitDispatchWqeBatch(DispatchWqeBatchState &state,
    AscendC::LocalTensor<uint8_t> issueLocal,
    AscendC::LocalTensor<uint8_t> cqeLocal, uint32_t phase,
    uint32_t &dfxFlags, uint32_t &firstQuietStatus,
    uint32_t &firstQuietPhase)
{
    const uint32_t batchCount = state.batchCount;
    if (batchCount == 0U) {
        return true;
    }
    if (batchCount > state.batchLimit) {
        return false;
    }

    const bool hasCapacity = DispatchEnsureSqBatchCapacity(state, batchCount,
        cqeLocal, phase, dfxFlags, firstQuietStatus, firstQuietPhase);
    if (!hasCapacity) {
        return false;
    }

    __ubuf__ uint8_t *issueAddr = reinterpret_cast<__ubuf__ uint8_t *>(
        issueLocal.GetPhyAddr());
    __ubuf__ TileXR::UDMASqeCtx *lastSqe =
        reinterpret_cast<__ubuf__ TileXR::UDMASqeCtx *>(
            issueAddr + (batchCount - 1U) * kDispatchUdmaWqeBytes);
    lastSqe->flag = static_cast<uint32_t>(lastSqe->flag) |
        TileXR::TILEXR_UDMA_SQE_FLAG_COMPLETION;

    __gm__ uint8_t *wqeAddr = reinterpret_cast<__gm__ uint8_t *>(
        state.qpCtxEntry->bufAddr + kDispatchUdmaWqeBytes *
            (state.head % TileXR::TILEXR_UDMA_SQ_BB_COUNT));
    const uint32_t batchBytes = batchCount * kDispatchUdmaWqeBytes;
    AscendC::GlobalTensor<uint8_t> wqeGlobal;
    wqeGlobal.SetGlobalBuffer(wqeAddr, batchBytes);
    const AscendC::DataCopyExtParams copyParams {
        1U, batchBytes, 0U, 0U, 0U};

    SyncFunc<AscendC::HardEvent::S_MTE3>();
    AscendC::DataCopyPad(wqeGlobal, issueLocal, copyParams);
    SyncFunc<AscendC::HardEvent::MTE3_S>();

    const uint32_t batchEndHead = state.head + batchCount;
    const uint32_t batchEndCompletionCount = state.completionCount + 1U;
    st_dev(batchEndHead, reinterpret_cast<__gm__ uint32_t *>(
        state.qpCtxEntry->headAddr), 0);
    st_dev(batchEndCompletionCount, reinterpret_cast<__gm__ uint32_t *>(
        state.qpCtxEntry->wqeCntAddr), 0);

    state.head = batchEndHead;
    state.completionCount = batchEndCompletionCount;
    state.outstanding += batchCount;
    state.stagedDoorbellHead = batchEndHead;
    state.doorbellPending = 1U;
    state.batchCount = 0U;
    state.batchLimit = TileXRMoonEp::DispatchWqeBatchCount(
        UINT64_MAX, state.head, TileXR::TILEXR_UDMA_SQ_BB_COUNT);
    return state.batchLimit != 0U;
}

__aicore__ inline bool AppendDispatchWqes(DispatchWqeBatchState &state,
    AscendC::LocalTensor<uint8_t> issueLocal,
    AscendC::LocalTensor<uint8_t> cqeLocal,
    AscendC::LocalTensor<int16_t> selectedRouteIndices,
    AscendC::LocalTensor<int32_t> dstValues, uint64_t hiddenLocalSourceBase,
    uint64_t hiddenRowBytes, uint64_t weightLocalSourceBase,
    uint64_t weightRowBytes, bool hasWeight, uint64_t destinationCapacity,
    uint32_t topKMagic, uint32_t topKShift, uint32_t selectedRouteCount,
    bool appendSignal,
    uint64_t signalLocalAddr, uint32_t phase, uint32_t &dfxFlags,
    uint32_t &firstQuietStatus, uint32_t &firstQuietPhase,
    uint32_t sequencePhase, uint32_t routePlanStart)
{
    uint64_t dataTaskCount64 = 0U;
    if (!TileXRMoonEp::DispatchDataWqeCount(selectedRouteCount, hasWeight,
            dataTaskCount64) || dataTaskCount64 > UINT32_MAX) {
        return false;
    }
    const uint32_t dataTaskCount = static_cast<uint32_t>(dataTaskCount64);
    uint32_t dataTaskStart = 0U;
    bool signalPending = appendSignal;
    while (dataTaskStart < dataTaskCount || signalPending) {
        if (state.batchCount == state.batchLimit &&
            !SubmitDispatchWqeBatch(state, issueLocal, cqeLocal,
                phase, dfxFlags,
                firstQuietStatus, firstQuietPhase)) {
            return false;
        }
        const uint32_t available = state.batchLimit - state.batchCount;
        const uint32_t dataTaskRemaining = dataTaskCount - dataTaskStart;
        const bool appendSignalNow = signalPending &&
            static_cast<uint64_t>(dataTaskRemaining) + 1U <= available;
        const uint32_t dataTaskCapacity = available -
            (appendSignalNow ? 1U : 0U);

        const uint32_t batchDataTaskCount =
            dataTaskRemaining < dataTaskCapacity ?
            dataTaskRemaining : dataTaskCapacity;
        if (!BuildDispatchWriteWqeBatch(issueLocal,
                selectedRouteIndices, dstValues, state, hiddenLocalSourceBase,
                hiddenRowBytes, weightLocalSourceBase, weightRowBytes,
                hasWeight, destinationCapacity, topKMagic, topKShift,
                dataTaskStart, batchDataTaskCount,
                appendSignalNow, signalLocalAddr, sequencePhase,
                routePlanStart)) {
            return false;
        }

        state.batchCount += batchDataTaskCount + (appendSignalNow ? 1U : 0U);
        dataTaskStart += batchDataTaskCount;
        if (appendSignalNow) {
            signalPending = false;
        }
        if (state.batchCount == state.batchLimit &&
            !SubmitDispatchWqeBatch(state, issueLocal, cqeLocal,
                phase, dfxFlags,
                firstQuietStatus, firstQuietPhase)) {
            return false;
        }
    }
    return !appendSignal || SubmitDispatchWqeBatch(state, issueLocal,
        cqeLocal, phase, dfxFlags, firstQuietStatus, firstQuietPhase);
}

__aicore__ inline bool DispatchDrainSqToExpected(
    DispatchWqeBatchState &state, uint32_t expectedSqTail,
    AscendC::LocalTensor<uint8_t> cqeLocal, uint64_t timeoutTicks,
    uint32_t phase, uint32_t &dfxFlags, uint32_t &firstQuietStatus,
    uint32_t &firstQuietPhase, uint32_t &timeoutPeer,
    uint32_t &timeoutPhase, uint64_t &timeoutObserved)
{
    const uint32_t pending = expectedSqTail - state.tail;
    if (pending == 0U) {
        return true;
    }
    if (pending >= TileXR::TILEXR_UDMA_SQ_BB_COUNT) {
        dfxFlags |= TileXRMoonEp::kDispatchDfxCqError;
        if (firstQuietStatus == 0U) {
            firstQuietStatus = 0xFFFFFFFCU;
            firstQuietPhase = phase;
        }
        return false;
    }

    const uint64_t waitStart = static_cast<uint64_t>(AscendC::GetSystemCycle());
    while (state.tail != expectedSqTail) {
        const uint32_t pollStatus = DispatchPollCqBatch(state, cqeLocal);
        if (pollStatus != 0U && pollStatus != 0xFFU) {
            dfxFlags |= TileXRMoonEp::kDispatchDfxCqError;
            if (firstQuietStatus == 0U) {
                firstQuietStatus = pollStatus;
                firstQuietPhase = phase;
            }
            return false;
        }
        if (static_cast<uint64_t>(AscendC::GetSystemCycle()) - waitStart >=
            timeoutTicks) {
            dfxFlags |= TileXRMoonEp::kDispatchDfxCqError;
            if (timeoutPeer == UINT32_MAX) {
                timeoutPeer = static_cast<uint32_t>(state.targetRank);
                timeoutPhase = phase;
                timeoutObserved = state.tail;
            }
            return false;
        }
    }
    return true;
}

__aicore__ inline bool DrainDispatchCreditQp(
    const __gm__ TileXR::CommArgs *args, __gm__ TileXR::UDMAInfo *udmaInfo,
    int32_t targetRank, uint32_t physicalQpIdx,
    AscendC::LocalTensor<uint8_t> cqeLocal, uint64_t timeoutTicks,
    uint32_t phase, uint32_t &dfxFlags, uint32_t &firstQuietStatus,
    uint32_t &firstQuietPhase, uint32_t &timeoutPeer,
    uint32_t &timeoutPhase, uint64_t &timeoutObserved,
    DispatchWqeBatchState &state)
{
    const bool ready = InitDispatchCreditQpState(
            args, udmaInfo, targetRank, physicalQpIdx, state) &&
        DispatchDrainSqToExpected(state, state.head, cqeLocal, timeoutTicks,
            phase, dfxFlags, firstQuietStatus, firstQuietPhase, timeoutPeer,
            timeoutPhase, timeoutObserved);

    return ready;
}

__aicore__ inline bool DispatchDrainHistoricalCq(
    DispatchPreparedPeer &peer, AscendC::LocalTensor<uint8_t> cqeLocal,
    uint64_t timeoutTicks, uint32_t &dfxFlags,
    uint32_t &firstQuietStatus, uint32_t &firstQuietPhase,
    uint32_t &timeoutPeer, uint32_t &timeoutPhase,
    uint64_t &timeoutObserved)
{
    for (uint32_t qpIdx = 0U; qpIdx < peer.qpCount; ++qpIdx) {
        DispatchWqeBatchState &state = peer.qpState[qpIdx];
        if (!DispatchDrainSqToExpected(state, state.head, cqeLocal,
                timeoutTicks, peer.issuePhase, dfxFlags, firstQuietStatus,
                firstQuietPhase, timeoutPeer, timeoutPhase, timeoutObserved)) {
            return false;
        }
    }
    return true;
}

__aicore__ inline bool DispatchBuildGroupedQpBatch(
    DispatchWqeBatchState &state, AscendC::LocalTensor<uint8_t> issueLocal,
    AscendC::LocalTensor<int16_t> selectedRouteIndices,
    AscendC::LocalTensor<int32_t> dstValues, uint64_t hiddenLocalSourceBase,
    uint64_t hiddenRowBytes, uint64_t weightLocalSourceBase,
    uint64_t weightRowBytes, bool hasWeight, uint64_t destinationCapacity,
    uint32_t topKMagic, uint32_t topKShift, uint32_t selectedRouteCount,
    uint32_t &dataTaskStart, bool &signalPending, uint64_t signalLocalAddr,
    uint32_t sequencePhase, uint32_t routePlanStart, bool &finalBatch)
{
    finalBatch = false;
    state.batchCount = 0U;
    uint64_t dataTaskCount64 = 0U;
    if (!TileXRMoonEp::DispatchDataWqeCount(selectedRouteCount, hasWeight,
            dataTaskCount64) || dataTaskCount64 > UINT32_MAX) {
        return false;
    }
    const uint32_t dataTaskCount = static_cast<uint32_t>(dataTaskCount64);
    if (dataTaskStart >= dataTaskCount && !signalPending) {
        return true;
    }
    const uint32_t available = state.batchLimit;
    if (available == 0U) {
        return false;
    }
    const uint32_t dataTaskRemaining = dataTaskCount - dataTaskStart;
    const bool appendSignalNow = signalPending &&
        static_cast<uint64_t>(dataTaskRemaining) + 1U <= available;
    const uint32_t dataTaskCapacity = available -
        (appendSignalNow ? 1U : 0U);
    const uint32_t batchDataTaskCount = dataTaskRemaining < dataTaskCapacity ?
        dataTaskRemaining : dataTaskCapacity;
    if (batchDataTaskCount == 0U && !appendSignalNow) {
        return false;
    }
    if (!BuildDispatchWriteWqeBatch(issueLocal, selectedRouteIndices,
            dstValues, state, hiddenLocalSourceBase, hiddenRowBytes,
            weightLocalSourceBase, weightRowBytes, hasWeight, destinationCapacity,
            topKMagic, topKShift, dataTaskStart, batchDataTaskCount,
            appendSignalNow, signalLocalAddr, sequencePhase,
            routePlanStart)) {
        return false;
    }
    state.batchCount = batchDataTaskCount + (appendSignalNow ? 1U : 0U);
    dataTaskStart += batchDataTaskCount;
    if (appendSignalNow) {
        signalPending = false;
        finalBatch = true;
    }
    return true;
}

__aicore__ inline bool StageDispatchQpBatch(
    DispatchWqeBatchState &state, AscendC::LocalTensor<uint8_t> issueLocal,
    AscendC::LocalTensor<uint8_t> cqeLocal, bool finalBatch,
    uint32_t phase, uint32_t &dfxFlags, uint32_t &firstQuietStatus,
    uint32_t &firstQuietPhase)
{
    const uint32_t batchCount = state.batchCount;
    if (batchCount == 0U || batchCount > state.batchLimit) {
        return false;
    }

    const bool hasCapacity = DispatchEnsureSqBatchCapacity(state, batchCount,
        cqeLocal, phase, dfxFlags, firstQuietStatus, firstQuietPhase);
    if (!hasCapacity) {
        return false;
    }

    __ubuf__ uint8_t *issueAddr = reinterpret_cast<__ubuf__ uint8_t *>(
        issueLocal.GetPhyAddr());
    __ubuf__ TileXR::UDMASqeCtx *lastSqe =
        reinterpret_cast<__ubuf__ TileXR::UDMASqeCtx *>(
            issueAddr + (batchCount - 1U) * kDispatchUdmaWqeBytes);
    lastSqe->flag = static_cast<uint32_t>(lastSqe->flag) |
        TileXR::TILEXR_UDMA_SQE_FLAG_COMPLETION;

    __gm__ uint8_t *wqeAddr = reinterpret_cast<__gm__ uint8_t *>(
        state.qpCtxEntry->bufAddr + kDispatchUdmaWqeBytes *
            (state.head % TileXR::TILEXR_UDMA_SQ_BB_COUNT));
    const uint32_t batchBytes = batchCount * kDispatchUdmaWqeBytes;
    AscendC::GlobalTensor<uint8_t> wqeGlobal;
    wqeGlobal.SetGlobalBuffer(wqeAddr, batchBytes);
    const AscendC::DataCopyExtParams copyParams {
        1U, batchBytes, 0U, 0U, 0U};

    SyncFunc<AscendC::HardEvent::S_MTE3>();
    AscendC::DataCopyPad(wqeGlobal, issueLocal, copyParams);
    SyncFunc<AscendC::HardEvent::MTE3_S>();

    state.head += batchCount;
    state.completionCount += 1U;
    state.outstanding += batchCount;
    state.stagedDoorbellHead = state.head;
    state.doorbellPending = 1U;
    if (finalBatch) {
        state.finalHead = state.head;
        state.finalStaged = 1U;
    }
    st_dev(state.head, reinterpret_cast<__gm__ uint32_t *>(
        state.qpCtxEntry->headAddr), 0);
    st_dev(state.completionCount, reinterpret_cast<__gm__ uint32_t *>(
        state.qpCtxEntry->wqeCntAddr), 0);

    state.batchCount = 0U;
    state.batchLimit = TileXRMoonEp::DispatchWqeBatchCount(
        UINT64_MAX, state.head, TileXR::TILEXR_UDMA_SQ_BB_COUNT);
    return state.batchLimit != 0U;
}

__aicore__ inline void RingDispatchPeerDoorbells(DispatchPreparedPeer &peer)
{
    for (uint32_t qpIdx = 0U; qpIdx < peer.qpCount; ++qpIdx) {
        DispatchWqeBatchState &state = peer.qpState[qpIdx];
        if (state.doorbellPending == 0U) {
            continue;
        }
        st_dev(state.stagedDoorbellHead, reinterpret_cast<__gm__ uint32_t *>(
            state.qpCtxEntry->dbAddr), 0);
        state.doorbellPending = 0U;
        state.doorbellRung = 1U;
    }
}

__aicore__ inline bool DispatchDrainPeerFinalCq(
    DispatchPreparedPeer &peer, AscendC::LocalTensor<uint8_t> cqeLocal,
    uint64_t timeoutTicks, uint32_t &dfxFlags,
    uint32_t &firstQuietStatus, uint32_t &firstQuietPhase,
    uint32_t &timeoutPeer, uint32_t &timeoutPhase,
    uint64_t &timeoutObserved)
{
    for (uint32_t qpIdx = 0U; qpIdx < peer.qpCount; ++qpIdx) {
        DispatchWqeBatchState &state = peer.qpState[qpIdx];
        if (state.finalStaged == 0U || state.doorbellRung == 0U ||
            !DispatchDrainSqToExpected(state, state.finalHead, cqeLocal,
                timeoutTicks, peer.issuePhase, dfxFlags, firstQuietStatus,
                firstQuietPhase, timeoutPeer, timeoutPhase, timeoutObserved)) {
            return false;
        }
    }
    return true;
}

__aicore__ inline bool DecodeSendDst(int32_t encoded, uint64_t destinationCapacity,
    int32_t rankSize, int32_t &targetRank, uint64_t &targetSlot)
{
    int64_t rank = -1;
    int64_t slot = -1;
    const bool valid = TileXRMoonEp::DispatchDecodeDestination(encoded,
        static_cast<int64_t>(destinationCapacity), rankSize, &rank, &slot);
    targetRank = static_cast<int32_t>(rank);
    targetSlot = static_cast<uint64_t>(slot);
    return valid;
}

__aicore__ inline int32_t DecodeRawDst(int32_t encoded)
{
    return encoded;
}

__aicore__ inline void RecordInvalidRoute(uint64_t routeId, int32_t rawDst,
    uint32_t &dfxFlags, uint32_t &firstInvalidRouteId,
    int32_t &firstInvalidRawDst)
{
    dfxFlags |= TileXRMoonEp::kDispatchDfxInvalidRoute;
    if (firstInvalidRouteId == UINT32_MAX) {
        firstInvalidRouteId = static_cast<uint32_t>(routeId);
        firstInvalidRawDst = rawDst;
    }
}

__aicore__ inline void CopyBytesGmToGm(__gm__ uint8_t *dst,
    const __gm__ uint8_t *src, uint32_t bytes,
    AscendC::LocalTensor<uint8_t> relayLocal)
{
    for (uint32_t offset = 0U; offset < bytes; offset += kRelayUbBytes) {
        const uint32_t remaining = bytes - offset;
        const uint32_t tileBytes = remaining < kRelayUbBytes ? remaining : kRelayUbBytes;
        AscendC::GlobalTensor<uint8_t> srcGlobal;
        srcGlobal.SetGlobalBuffer(const_cast<__gm__ uint8_t *>(src) + offset);
        const AscendC::DataCopyExtParams copyIn {1U, tileBytes, 0U, 0U, 0U};
        const AscendC::DataCopyPadExtParams<uint8_t> padIn {false, 0U, 0U, 0U};
        AscendC::DataCopyPad(relayLocal, srcGlobal, copyIn, padIn);
        AscendC::SetFlag<AscendC::HardEvent::MTE2_MTE3>(EVENT_ID0);
        AscendC::WaitFlag<AscendC::HardEvent::MTE2_MTE3>(EVENT_ID0);

        AscendC::GlobalTensor<uint8_t> dstGlobal;
        dstGlobal.SetGlobalBuffer(dst + offset);
        const AscendC::DataCopyExtParams copyOut {1U, tileBytes, 0U, 0U, 0U};
        AscendC::DataCopyPad(dstGlobal, relayLocal, copyOut);
        AscendC::SetFlag<AscendC::HardEvent::MTE3_MTE2>(EVENT_ID0);
        AscendC::WaitFlag<AscendC::HardEvent::MTE3_MTE2>(EVENT_ID0);
    }
    AscendC::PipeBarrier<PIPE_ALL>();
}

struct DispatchLocalCopyPipeline {
    AscendC::LocalTensor<uint8_t> ping;
    AscendC::LocalTensor<uint8_t> pong;
    uint64_t tileCount;
};

__aicore__ inline void InitDispatchLocalCopyPipeline(
    DispatchLocalCopyPipeline &pipeline,
    AscendC::LocalTensor<uint8_t> ping,
    AscendC::LocalTensor<uint8_t> pong)
{
    pipeline.ping = ping;
    pipeline.pong = pong;
    pipeline.tileCount = 0U;
    AscendC::SetFlag<AscendC::HardEvent::MTE3_MTE2>(EVENT_ID0);
    AscendC::SetFlag<AscendC::HardEvent::MTE3_MTE2>(EVENT_ID1);
}

__aicore__ inline void SubmitDispatchLocalCopy(
    DispatchLocalCopyPipeline &pipeline, __gm__ uint8_t *dst,
    const __gm__ uint8_t *src, uint32_t bytes)
{
    const AscendC::DataCopyPadExtParams<uint8_t> padIn {
        false, 0U, 0U, 0U};
    for (uint32_t offset = 0U; offset < bytes;
        offset += kLocalCopyTileBytes) {
        const uint32_t remaining = bytes - offset;
        const uint32_t tileBytes = remaining < kLocalCopyTileBytes ?
            remaining : kLocalCopyTileBytes;
        const uint32_t bufferIndex =
            static_cast<uint32_t>(pipeline.tileCount & 1U);
        const AscendC::TEventID eventId = bufferIndex == 0U ?
            EVENT_ID0 : EVENT_ID1;
        AscendC::LocalTensor<uint8_t> local = bufferIndex == 0U ?
            pipeline.ping : pipeline.pong;

        AscendC::WaitFlag<AscendC::HardEvent::MTE3_MTE2>(eventId);
        AscendC::GlobalTensor<uint8_t> srcGlobal;
        srcGlobal.SetGlobalBuffer(
            const_cast<__gm__ uint8_t *>(src) + offset, tileBytes);
        const AscendC::DataCopyExtParams copyIn {
            1U, tileBytes, 0U, 0U, 0U};
        AscendC::DataCopyPad(local, srcGlobal, copyIn, padIn);
        AscendC::SetFlag<AscendC::HardEvent::MTE2_MTE3>(eventId);
        AscendC::WaitFlag<AscendC::HardEvent::MTE2_MTE3>(eventId);

        AscendC::GlobalTensor<uint8_t> dstGlobal;
        dstGlobal.SetGlobalBuffer(dst + offset, tileBytes);
        const AscendC::DataCopyExtParams copyOut {
            1U, tileBytes, 0U, 0U, 0U};
        AscendC::DataCopyPad(dstGlobal, local, copyOut);
        AscendC::SetFlag<AscendC::HardEvent::MTE3_MTE2>(eventId);
        ++pipeline.tileCount;
    }
}

__aicore__ inline void DrainDispatchLocalCopyPipeline()
{
    AscendC::WaitFlag<AscendC::HardEvent::MTE3_MTE2>(EVENT_ID0);
    AscendC::WaitFlag<AscendC::HardEvent::MTE3_MTE2>(EVENT_ID1);
    AscendC::SetFlag<AscendC::HardEvent::MTE3_S>(EVENT_ID3);
    AscendC::WaitFlag<AscendC::HardEvent::MTE3_S>(EVENT_ID3);
}

__aicore__ inline void CopyContiguousBytesGmToGmPipelined(
    __gm__ uint8_t *dst, const __gm__ uint8_t *src, uint64_t bytes,
    uint32_t tileBytes,
    AscendC::TQue<AscendC::QuePosition::VECIN, 1> &outputCopyQueue)
{
    if (bytes == 0U || tileBytes == 0U) {
        return;
    }

    AscendC::GlobalTensor<uint8_t> srcGlobal;
    AscendC::GlobalTensor<uint8_t> dstGlobal;
    srcGlobal.SetGlobalBuffer(const_cast<__gm__ uint8_t *>(src), bytes);
    dstGlobal.SetGlobalBuffer(dst, bytes);

    const AscendC::DataCopyPadExtParams<uint8_t> padIn {false, 0U, 0U, 0U};
    uint64_t pendingOffset = 0U;
    uint32_t pendingBytes = static_cast<uint32_t>(
        bytes < tileBytes ? bytes : tileBytes);
    AscendC::LocalTensor<uint8_t> firstLocal =
        outputCopyQueue.AllocTensor<uint8_t>();
    const AscendC::DataCopyExtParams firstCopyIn {
        1U, pendingBytes, 0U, 0U, 0U};
    AscendC::DataCopyPad(firstLocal, srcGlobal, firstCopyIn, padIn);
    outputCopyQueue.EnQue(firstLocal);

    uint64_t offset = pendingBytes;
    while (offset < bytes) {
        AscendC::LocalTensor<uint8_t> pendingLocal =
            outputCopyQueue.DeQue<uint8_t>();
        SyncFunc<AscendC::HardEvent::MTE2_MTE3>();

        const uint64_t remaining = bytes - offset;
        const uint32_t currentBytes = static_cast<uint32_t>(
            remaining < tileBytes ? remaining : tileBytes);
        AscendC::LocalTensor<uint8_t> currentLocal =
            outputCopyQueue.AllocTensor<uint8_t>();
        const AscendC::DataCopyExtParams currentCopyIn {
            1U, currentBytes, 0U, 0U, 0U};
        AscendC::DataCopyPad(currentLocal, srcGlobal[offset], currentCopyIn, padIn);

        const AscendC::DataCopyExtParams copyOut {
            1U, pendingBytes, 0U, 0U, 0U};
        AscendC::DataCopyPad(dstGlobal[pendingOffset], pendingLocal, copyOut);
        SyncFunc<AscendC::HardEvent::MTE3_S>();
        outputCopyQueue.FreeTensor(pendingLocal);

        outputCopyQueue.EnQue(currentLocal);
        pendingOffset = offset;
        pendingBytes = currentBytes;
        offset += currentBytes;
    }

    AscendC::LocalTensor<uint8_t> pendingLocal =
        outputCopyQueue.DeQue<uint8_t>();
    SyncFunc<AscendC::HardEvent::MTE2_MTE3>();
    const AscendC::DataCopyExtParams copyOut {
        1U, pendingBytes, 0U, 0U, 0U};
    AscendC::DataCopyPad(dstGlobal[pendingOffset], pendingLocal, copyOut);
    SyncFunc<AscendC::HardEvent::MTE3_S>();
    outputCopyQueue.FreeTensor(pendingLocal);
}

__aicore__ inline void PublishDispatchSignalSource(
    __gm__ uint64_t *signalSource, uint64_t expectedFlag,
    AscendC::LocalTensor<uint8_t> relayLocal)
{
    AscendC::LocalTensor<uint64_t> signalLocal =
        relayLocal.ReinterpretCast<uint64_t>();
    for (uint32_t qpIdx = 0U;
        qpIdx < TileXRMoonEp::kDispatchQpCount; ++qpIdx) {
        signalLocal.SetValue(qpIdx, expectedFlag);
    }
    AscendC::GlobalTensor<uint64_t> signalGlobal;
    signalGlobal.SetGlobalBuffer(signalSource,
        TileXRMoonEp::kDispatchQpCount);
    const AscendC::DataCopyExtParams copyOut {
        1U, TileXRMoonEp::kDispatchQpCount *
            static_cast<uint32_t>(sizeof(uint64_t)), 0U, 0U, 0U};
    SyncFunc<AscendC::HardEvent::S_MTE3>();
    AscendC::DataCopyPad(signalGlobal, signalLocal, copyOut);
    SyncFunc<AscendC::HardEvent::MTE3_S>();
    TileXR::UDMACleanCacheLines(
        reinterpret_cast<__gm__ uint8_t *>(signalSource),
        TileXRMoonEp::kDispatchQpCount * sizeof(uint64_t));
    AscendC::PipeBarrier<PIPE_ALL>();
}

__aicore__ inline uint64_t LoadCompletionFlag(__gm__ uint64_t *flag)
{
    TileXR::UDMACleanCacheLines(
        reinterpret_cast<__gm__ uint8_t *>(flag), sizeof(uint64_t));
    AscendC::PipeBarrier<PIPE_ALL>();
    return flag[0];
}

__aicore__ inline bool WaitCompletionFlag(__gm__ uint64_t *flag,
    uint64_t expected, uint64_t waitStart, uint64_t timeoutTicks,
    uint64_t &observed)
{
    observed = LoadCompletionFlag(flag);
    while (observed < expected) {
        if (static_cast<uint64_t>(AscendC::GetSystemCycle()) - waitStart >=
            timeoutTicks) {
            return false;
        }
        observed = LoadCompletionFlag(flag);
    }
    return true;
}

__aicore__ inline uint64_t LoadDispatchCredit(
    __gm__ uint64_t *credit, AscendC::LocalTensor<uint8_t> relayLocal)
{
    AscendC::GlobalTensor<uint64_t> creditGlobal;
    creditGlobal.SetGlobalBuffer(credit,
        TileXRMoonEp::kDispatchCreditStrideBytes / sizeof(uint64_t));
    auto creditLocal = relayLocal.ReinterpretCast<uint64_t>();
    TileXR::UDMACleanCacheLines(
        reinterpret_cast<__gm__ uint8_t *>(credit),
        TileXRMoonEp::kDispatchCreditStrideBytes);
    SyncFunc<AscendC::HardEvent::S_MTE2>();
    AscendC::DataCopy(creditLocal, creditGlobal,
        TileXRMoonEp::kDispatchCreditStrideBytes / sizeof(uint64_t));
    SyncFunc<AscendC::HardEvent::MTE2_S>();
    return creditLocal.GetValue(0);
}

__aicore__ inline bool WaitDispatchPeerCredit(
    __gm__ uint8_t *workspace, uint64_t creditOffset, int32_t peer,
    uint32_t group, int64_t magic, uint64_t timeoutTicks,
    AscendC::LocalTensor<uint8_t> relayLocal, uint64_t &observed)
{
    if (!TileXRMoonEp::DispatchCreditRequired(group)) {
        observed = 0U;
        return true;
    }
    uint64_t expected = 0U;
    const uint64_t planeOffset = TileXRMoonEp::DispatchCreditPlaneOffset(magic);
    const uint64_t entryOffset = TileXRMoonEp::DispatchCreditEntryOffset(
        static_cast<uint32_t>(peer));
    if (workspace == nullptr || peer < 0 || planeOffset == UINT64_MAX ||
        entryOffset == UINT64_MAX ||
        !TileXRMoonEp::DispatchCreditToken(magic, group, expected)) {
        observed = 0U;
        return false;
    }
    auto credit = reinterpret_cast<__gm__ uint64_t *>(
        workspace + creditOffset + planeOffset + entryOffset);
    const uint64_t waitStart = static_cast<uint64_t>(AscendC::GetSystemCycle());
    observed = LoadDispatchCredit(credit, relayLocal);
    while (!TileXRMoonEp::DispatchCreditReady(observed, expected)) {
        if (static_cast<uint64_t>(AscendC::GetSystemCycle()) - waitStart >=
            timeoutTicks) {
            return false;
        }
        observed = LoadDispatchCredit(credit, relayLocal);
    }
    return true;
}

__aicore__ inline bool PublishDispatchPeerCredit(
    const __gm__ TileXR::CommArgs *args, __gm__ uint8_t *workspace,
    uint64_t creditOffset, uint64_t creditSourceOffset, uint32_t core,
    uint32_t physicalQpIdx, int32_t rank, int32_t peer, uint32_t group,
    int64_t magic, AscendC::LocalTensor<uint8_t> wqeLocal,
    DispatchWqeBatchState *fullmeshState)
{
    uint64_t token = 0U;
    const uint64_t planeOffset = TileXRMoonEp::DispatchCreditPlaneOffset(magic);
    const uint64_t entryOffset = TileXRMoonEp::DispatchCreditEntryOffset(
        static_cast<uint32_t>(rank));
    const uint64_t sourceOffset = TileXRMoonEp::DispatchCreditSourceOffset(
        core, group);
    if (args == nullptr || workspace == nullptr ||
        planeOffset == UINT64_MAX || entryOffset == UINT64_MAX ||
        sourceOffset == UINT64_MAX ||
        !TileXRMoonEp::DispatchCreditToken(magic, group, token)) {
        return false;
    }
    const uint64_t remoteCreditOffset =
        creditOffset + planeOffset + entryOffset;
    auto creditSource = reinterpret_cast<__gm__ uint64_t *>(
        workspace + creditSourceOffset + sourceOffset);
    creditSource[0] = token;
    TileXR::UDMACleanCacheLines(
        reinterpret_cast<__gm__ uint8_t *>(creditSource), sizeof(uint64_t));
    if (fullmeshState != nullptr) {
        auto registry = TileXR::GetUDMARegistry(args);
        auto remoteAddr = TileXR::UDMARegisteredRemoteAddr(
            registry, peer, remoteCreditOffset);
        const uint32_t status = TileXR::UDMAPostSend<
            TileXR::UDMAOpcode::WRITE, true>(
                fullmeshState->udmaInfo, wqeLocal, remoteAddr,
                reinterpret_cast<__gm__ uint8_t *>(creditSource),
                static_cast<uint32_t>(peer),
                fullmeshState->physicalQpIdx, sizeof(uint64_t), nullptr,
                TileXR::TILEXR_UDMA_SQE_FLAG_ORDERED_COMPLETION);
        if (status != TileXR::TILEXR_UDMA_STATUS_SUCCESS) {
            return false;
        }
        RecordDispatchCreditQpPost(*fullmeshState, peer);
        fullmeshState->doorbellRung = 1U;
        return true;
    }
    return TileXR::UDMAPutNbiOnQp<uint64_t>(
        args, wqeLocal, peer, physicalQpIdx,
        creditSource, remoteCreditOffset, sizeof(uint64_t)) ==
        TileXR::TILEXR_UDMA_STATUS_SUCCESS;
}

__aicore__ inline bool WaitDispatchIncomingPeer(
    __gm__ uint64_t *receiveFlags, int32_t incomingPeer, uint32_t group,
    uint32_t completionLaneCount, uint64_t magic, uint64_t timeoutTicks,
    AscendC::LocalTensor<uint8_t> relayLocal, uint32_t &dfxFlags,
    uint32_t &timeoutPeer, uint32_t &timeoutPhase,
    uint64_t &timeoutObserved)
{
    const uint64_t waitStart = static_cast<uint64_t>(AscendC::GetSystemCycle());

    const uint64_t peerFlagBase = static_cast<uint64_t>(incomingPeer) *
        TileXRMoonEp::kDispatchQpCount;
    if (completionLaneCount == 0U ||
        completionLaneCount > TileXRMoonEp::kDispatchQpCount) {
        dfxFlags |= TileXRMoonEp::kDispatchDfxInvalidConfig;
        return false;
    }
    for (uint32_t qpIdx = 0U; qpIdx < completionLaneCount; ++qpIdx) {
        uint64_t observed = 0U;
        if (!WaitCompletionFlag(receiveFlags + peerFlagBase + qpIdx,
                static_cast<uint64_t>(magic), waitStart, timeoutTicks,
                observed)) {
            dfxFlags |= TileXRMoonEp::kDispatchDfxCompletionTimeout;
            if (timeoutPeer == UINT32_MAX) {
                timeoutPeer = static_cast<uint32_t>(incomingPeer);
                timeoutPhase = group;
                timeoutObserved = observed;
            }

            return false;
        }
    }

    return true;
}

__aicore__ inline uint32_t LoadRouteTile(
    AscendC::GlobalTensor<int32_t> dstGlobal, uint64_t tileStart,
    uint64_t routeCount, AscendC::LocalTensor<int32_t> routeTileLocal)
{
    SyncFunc<AscendC::HardEvent::S_MTE2>();
    const uint64_t remaining = routeCount - tileStart;
    const uint32_t tileElements = static_cast<uint32_t>(
        remaining < kRouteTileElements ? remaining : kRouteTileElements);
    const AscendC::DataCopyExtParams copyIn {
        1U, tileElements * static_cast<uint32_t>(sizeof(int32_t)), 0U, 0U, 0U};
    const AscendC::DataCopyPadExtParams<int32_t> padIn {false, 0U, 0U, 0U};
    AscendC::DataCopyPad(routeTileLocal, dstGlobal[tileStart], copyIn, padIn);
    SyncFunc<AscendC::HardEvent::MTE2_S>();
    return tileElements;
}

#if defined(TILEXR_MOONEP_DISPATCH_ENABLE_DFX)
__aicore__ inline void WriteDfxRecord(__gm__ uint8_t *dfx,
    uint32_t payloadMode, uint32_t rank, uint32_t core, uint32_t flags,
    uint32_t firstInvalidRouteId, int32_t firstInvalidRawDst,
    uint32_t firstQuietStatus, uint32_t firstQuietPhase,
    uint32_t timeoutPeer, uint32_t timeoutPhase, uint64_t expectedRouteCount,
    uint64_t processedRouteCount, uint64_t magic, uint64_t timeoutExpectedMagic,
    uint64_t timeoutObservedFlag, uint64_t localSignalObserved,
    uint64_t completionFlagCount, uint64_t outgoingCqStatuses,
    uint64_t outgoingRemainingSqEntries,
    AscendC::LocalTensor<uint8_t> diagnosticLocal)
{
    __ubuf__ TileXRMoonEp::DispatchDfxRecord *record =
        reinterpret_cast<__ubuf__ TileXRMoonEp::DispatchDfxRecord *>(
            diagnosticLocal.GetPhyAddr());
    record->marker = TileXRMoonEp::kDispatchDfxMarker;
    record->version = TileXRMoonEp::kDispatchDiagnosticVersion;
    record->recordBytes = sizeof(TileXRMoonEp::DispatchDfxRecord);
    record->payloadMode = payloadMode;
    record->rank = rank;
    record->core = core;
    record->flags = flags;
    record->firstInvalidRouteId = firstInvalidRouteId;
    record->firstInvalidRawDst = firstInvalidRawDst;
    record->firstQuietStatus = firstQuietStatus;
    record->firstQuietPhase = firstQuietPhase;
    record->timeoutPeer = timeoutPeer;
    record->timeoutPhase = timeoutPhase;
    record->reserved0 = 0U;
    record->expectedRouteCount = expectedRouteCount;
    record->processedRouteCount = processedRouteCount;
    record->magic = magic;
    record->timeoutExpectedMagic = timeoutExpectedMagic;
    record->timeoutObservedFlag = timeoutObservedFlag;
    record->reserved[0] = localSignalObserved;
    record->reserved[1] = completionFlagCount;
    record->reserved[2] = outgoingCqStatuses;
    record->reserved[3] = outgoingRemainingSqEntries;
    SyncFunc<AscendC::HardEvent::S_MTE3>();

    AscendC::GlobalTensor<uint8_t> dfxGlobal;
    dfxGlobal.SetGlobalBuffer(dfx + static_cast<uint64_t>(core) *
        sizeof(TileXRMoonEp::DispatchDfxRecord));
    const AscendC::DataCopyExtParams copyOut {
        1U, static_cast<uint32_t>(sizeof(TileXRMoonEp::DispatchDfxRecord)),
        0U, 0U, 0U};
    AscendC::DataCopyPad(dfxGlobal, diagnosticLocal, copyOut);
    SyncFunc<AscendC::HardEvent::MTE3_S>();
}
#endif

__aicore__ inline int32_t StatusFromDfxFlags(uint32_t flags)
{
    if ((flags & TileXRMoonEp::kDispatchDfxCreditTimeout) != 0U) {
        return TileXRMoonEp::kDispatchStatusCreditTimeout;
    }
    if ((flags & TileXRMoonEp::kDispatchDfxCqError) != 0U) {
        return TileXRMoonEp::kDispatchStatusCqError;
    }
    if ((flags & TileXRMoonEp::kDispatchDfxCompletionTimeout) != 0U) {
        return TileXRMoonEp::kDispatchStatusCompletionTimeout;
    }
    if ((flags & TileXRMoonEp::kDispatchDfxInvalidConfig) != 0U) {
        return TileXRMoonEp::kDispatchStatusInvalidConfig;
    }
    if ((flags & TileXRMoonEp::kDispatchDfxInvalidRoute) != 0U) {
        return TileXRMoonEp::kDispatchStatusInvalidRoute;
    }
    if ((flags & TileXRMoonEp::kDispatchDfxRouteCountMismatch) != 0U) {
        return TileXRMoonEp::kDispatchStatusRouteCountMismatch;
    }
    if ((flags & TileXRMoonEp::kDispatchDfxUpstreamPlannerError) != 0U) {
        return TileXRMoonEp::kDispatchStatusUpstreamPlanner;
    }
    if ((flags & TileXRMoonEp::kDispatchDfxQuietError) != 0U) {
        return TileXRMoonEp::kDispatchStatusQuietError;
    }
    return TileXRMoonEp::kDispatchStatusSuccess;
}

__aicore__ inline int32_t LoadDispatchPlanStatus(__gm__ int32_t *planStatus,
    AscendC::LocalTensor<uint8_t> relayLocal)
{
    AscendC::GlobalTensor<uint8_t> statusGlobal;
    statusGlobal.SetGlobalBuffer(reinterpret_cast<__gm__ uint8_t *>(planStatus),
        sizeof(int32_t));
    const AscendC::DataCopyExtParams copyIn {
        1U, static_cast<uint32_t>(sizeof(int32_t)), 0U, 0U, 0U};
    const AscendC::DataCopyPadExtParams<uint8_t> padIn {
        false, 0U, 0U, 0U};
    AscendC::DataCopyPad(relayLocal, statusGlobal, copyIn, padIn);
    SyncFunc<AscendC::HardEvent::MTE2_S>();
    return relayLocal.ReinterpretCast<int32_t>().GetValue(0U);
}

__aicore__ inline void DispatchPublishFirstStatus(
    __gm__ int32_t *planStatus, int32_t status)
{
    if (planStatus == nullptr || status == TileXRMoonEp::kDispatchStatusSuccess) {
        return;
    }
    AscendC::AtomicCas(
        reinterpret_cast<__gm__ uint32_t *>(planStatus),
        static_cast<uint32_t>(TileXRMoonEp::kDispatchStatusSuccess),
        static_cast<uint32_t>(status));
}

__aicore__ inline void ClearDispatchBytes(__gm__ uint8_t *dst,
    uint64_t bytes, AscendC::LocalTensor<uint8_t> relayLocal)
{
    AscendC::LocalTensor<uint16_t> zeros = relayLocal.ReinterpretCast<uint16_t>();
    AscendC::Duplicate(zeros, static_cast<uint16_t>(0),
        static_cast<int32_t>(kRelayUbBytes / sizeof(uint16_t)));
    SyncFunc<AscendC::HardEvent::V_MTE3>();

    AscendC::GlobalTensor<uint8_t> dstGlobal;
    dstGlobal.SetGlobalBuffer(dst, bytes);
    for (uint64_t offset = 0U; offset < bytes; offset += kRelayUbBytes) {
        const uint64_t remaining = bytes - offset;
        const uint32_t tileBytes = static_cast<uint32_t>(
            remaining < kRelayUbBytes ? remaining : kRelayUbBytes);
        const AscendC::DataCopyExtParams copyOut {
            1U, tileBytes, 0U, 0U, 0U};
        AscendC::DataCopyPad(dstGlobal[offset], relayLocal, copyOut);
    }
    SyncFunc<AscendC::HardEvent::MTE3_V>();
}

__aicore__ inline bool ClearDispatchZeroFillRanges(
    __gm__ uint8_t *scratch, const __gm__ int32_t *zeroFillRanges,
    uint64_t zeroFillRangeCount, uint64_t destinationCapacity,
    uint64_t rowBytes, uint64_t blockIdx, uint64_t blockNum,
    AscendC::LocalTensor<uint8_t> relayLocal)
{
    for (uint64_t range = blockIdx; range < zeroFillRangeCount;
         range += blockNum) {
        const int32_t startValue = zeroFillRanges[range * 2U];
        const int32_t countValue = zeroFillRanges[range * 2U + 1U];
        if (startValue < 0 || countValue < 0) {
            return false;
        }
        const uint64_t start = static_cast<uint64_t>(startValue);
        const uint64_t count = static_cast<uint64_t>(countValue);
        if (start > destinationCapacity ||
            count > destinationCapacity - start) {
            return false;
        }
        if (count != 0U) {
            ClearDispatchBytes(scratch + start * rowBytes,
                count * rowBytes, relayLocal);
        }
    }
    return true;
}

} // namespace

extern "C" __global__ __aicore__ void tilexr_moonep_dispatch_urma_kernel(
    GM_ADDR commArgsGM, GM_ADDR hiddenInputGM, GM_ADDR weightInputGM,
    GM_ADDR dstGM, GM_ADDR zeroFillRangesGM, GM_ADDR workspaceGM,
    GM_ADDR hiddenOutputGM, GM_ADDR weightOutputGM, GM_ADDR planStatusGM,
    uint64_t hiddenSourceOffset, uint64_t hiddenScratchOffset,
    uint64_t hiddenRowBytes, uint64_t weightSourceOffset,
    uint64_t weightScratchOffset, uint64_t weightRowBytes,
    uint64_t completionFlagsOffset, uint64_t creditOffset,
    uint64_t creditSourceOffset, uint64_t signalOffset,
    uint64_t hiddenDfxOffset, uint64_t weightDfxOffset,
    uint64_t kernelStatusOffset,
    int64_t s, int64_t k, int64_t h, int64_t routeCountArg,
    int64_t destinationCapacityArg, int64_t zeroFillRangeCountArg,
    uint64_t hasWeightArg, int64_t magic,
    uint64_t completionTimeoutTicks, uint64_t peerMode,
    uint64_t groupWidthArg)
{
    if constexpr (g_coreType == AscendC::AIV) {
        auto args = reinterpret_cast<__gm__ TileXR::CommArgs *>(commArgsGM);
        auto hiddenInput = reinterpret_cast<__gm__ uint8_t *>(hiddenInputGM);
        auto weightInput = reinterpret_cast<__gm__ uint8_t *>(weightInputGM);
        auto dst = reinterpret_cast<__gm__ int32_t *>(dstGM);
        auto zeroFillRanges = reinterpret_cast<__gm__ int32_t *>(zeroFillRangesGM);
        auto workspace = reinterpret_cast<__gm__ uint8_t *>(workspaceGM);
        auto hiddenOutput = reinterpret_cast<__gm__ uint8_t *>(hiddenOutputGM);
        auto weightOutput = reinterpret_cast<__gm__ uint8_t *>(weightOutputGM);
        auto planStatus = reinterpret_cast<__gm__ int32_t *>(planStatusGM);
        const bool hasWeight = hasWeightArg != 0U;
        if (args == nullptr || hiddenInput == nullptr || dst == nullptr ||
            zeroFillRanges == nullptr || workspace == nullptr ||
            hiddenOutput == nullptr ||
            hasWeightArg > 1U || hasWeight != (weightInput != nullptr) ||
            hasWeight != (weightOutput != nullptr) ||
            planStatus == nullptr || s <= 0 || k <= 0 || h <= 0 ||
            routeCountArg <= 0 || destinationCapacityArg < routeCountArg ||
            zeroFillRangeCountArg <= 0 || zeroFillRangeCountArg > UINT32_MAX ||
            hiddenRowBytes == 0U || hiddenRowBytes > UINT32_MAX ||
            weightRowBytes != sizeof(float) || magic <= 0 ||
            completionTimeoutTicks == 0U ||
            peerMode > UINT32_MAX || groupWidthArg > UINT32_MAX ||
            !TileXRMoonEp::DispatchPeerModeValid(static_cast<uint32_t>(peerMode)) ||
            !TileXRMoonEp::DispatchGroupWidthValid(
                static_cast<uint32_t>(groupWidthArg))) {
            return;
        }

        const int32_t rank = args->rank;
        const int32_t rankSize = args->rankSize;
        const bool localOnly = rankSize == 1;
        const bool groupedPeerMode = TileXRMoonEp::DispatchPeerModeUsesGroups(
            static_cast<uint32_t>(peerMode));
        const bool creditPeerMode = TileXRMoonEp::DispatchPeerModeUsesCredit(
            static_cast<uint32_t>(peerMode));
        const uint32_t groupWidth = static_cast<uint32_t>(groupWidthArg);
        const bool sharedQp =
            (args->extraFlag & TileXR::ExtraFlag::UDMA_SHARED_QP) != 0U;
        auto udmaInfo = localOnly ? nullptr : TileXR::GetUDMAInfo(args);
        auto fullmeshView = localOnly ? nullptr : reinterpret_cast<__gm__
            TileXR::TileXRUDMAFullmeshDeviceView *>(args->udmaFullmeshPtr);
        if (rankSize <= 0 || rankSize > TileXR::TILEXR_MAX_RANK_SIZE || rank < 0 ||
            rank >= rankSize || (!localOnly && (!TileXR::UDMARegistryEnabled(args) ||
                udmaInfo == nullptr ||
                !TileXRMoonEp::DispatchQpCountSupported(
                    TileXR::UDMAQpCount(args), sharedQp))) ||
            (groupedPeerMode && !localOnly &&
                !DispatchFullmeshDeviceViewValid(args, fullmeshView))) {
            return;
        }

        const uint64_t blockIdx = static_cast<uint64_t>(AscendC::GetBlockIdx());
        const uint64_t blockNum = static_cast<uint64_t>(AscendC::GetBlockNum());
        const uint64_t routeCount = static_cast<uint64_t>(routeCountArg);
        const uint64_t destinationCapacity = static_cast<uint64_t>(
            destinationCapacityArg);
        const uint64_t zeroFillRangeCount = static_cast<uint64_t>(
            zeroFillRangeCountArg);
        if (blockNum == 0U || blockNum > TileXRMoonEp::kDispatchAivCoreCount ||
            routeCount > UINT32_MAX || destinationCapacity > UINT32_MAX ||
            routeCount / static_cast<uint64_t>(k) != static_cast<uint64_t>(s) ||
            routeCount % static_cast<uint64_t>(k) != 0U) {
            return;
        }
        const uint32_t peerCoreCount =
            TileXRMoonEp::DispatchPeerCoreCount(
                static_cast<uint32_t>(blockNum), sharedQp);
        if (peerCoreCount == 0U) {
            return;
        }
        const uint32_t physicalQp[TileXRMoonEp::kDispatchQpCount] = {
            TileXRMoonEp::DispatchPhysicalQpIndex(
                0U, static_cast<uint32_t>(blockIdx), sharedQp),
            TileXRMoonEp::DispatchPhysicalQpIndex(
                1U, static_cast<uint32_t>(blockIdx), sharedQp)};
        const bool dedicatedCreditQp = sharedQp && creditPeerMode;
        const uint32_t creditPhysicalQp =
            TileXRMoonEp::DispatchCreditPhysicalQpIndex(
                static_cast<uint32_t>(blockIdx), sharedQp);
        if (!localOnly && blockIdx < peerCoreCount &&
            (physicalQp[0] >= TileXR::UDMAQpCount(args) ||
                physicalQp[1] >= TileXR::UDMAQpCount(args) ||
                (dedicatedCreditQp &&
                    creditPhysicalQp >= TileXR::UDMAQpCount(args)))) {
            return;
        }

        uint32_t topKMagic = 0U;
        uint32_t topKShift = 0U;
        AscendC::GetUintDivMagicAndShift(topKMagic, topKShift,
            static_cast<uint32_t>(k));
        const uint64_t hiddenScratchSlotBytes = MultiplyU32ToU64(
            static_cast<uint32_t>(destinationCapacity),
            static_cast<uint32_t>(hiddenRowBytes));
        const uint64_t weightScratchSlotBytes = MultiplyU32ToU64(
            static_cast<uint32_t>(destinationCapacity),
            static_cast<uint32_t>(weightRowBytes));
        const uint64_t expectedFlag = static_cast<uint64_t>(magic);
        const uint64_t scratchIndex = expectedFlag %
            TileXRMoonEp::kDispatchScratchBufferCount;
        const int64_t groupCount = groupedPeerMode ?
            static_cast<int64_t>(TileXRMoonEp::DispatchGroupedGroupCount(
                rankSize, groupWidth)) :
            TileXRMoonEp::DispatchGroupCount(rankSize);
        const uint32_t peerWorkCount = groupedPeerMode ?
            TileXRMoonEp::DispatchGroupedPeerWorkCount(rankSize, groupWidth,
                peerCoreCount) :
            TileXRMoonEp::DispatchPeerWorkCount(peerCoreCount);
        if ((!localOnly && (groupCount <= 0 || peerWorkCount == 0U)) ||
            (!groupedPeerMode && groupCount > 8)) {
            return;
        }

        uint32_t routeSelectChunkElements = 0U;
        bool useVectorSlotSelect = true;
        if (routeCount < kVectorCompareMinElements) {
            useVectorSlotSelect = false;
        } else if (routeCount > UINT32_MAX) {
            useVectorSlotSelect = false;
        } else {
            routeSelectChunkElements = DispatchRouteSelectChunkElements(
                routeCount, static_cast<uint32_t>(k));
            if (routeSelectChunkElements == 0U) {
                useVectorSlotSelect = false;
            }
        }

        uint32_t routePlanUbBytes = kRouteTileBytes;
        uint32_t routeIndexUbBytes = 0U;
        uint32_t compareMaskUbBytes = 0U;
        if (useVectorSlotSelect) {
            routePlanUbBytes = static_cast<uint32_t>(AlignUp(
                static_cast<uint64_t>(routeSelectChunkElements) * sizeof(int32_t),
                kUbAlignBytes));
            routeIndexUbBytes = static_cast<uint32_t>(AlignUp(
                static_cast<uint64_t>(routeSelectChunkElements) * sizeof(int16_t),
                kUbAlignBytes));
            compareMaskUbBytes = static_cast<uint32_t>(AlignUp(
                CeilDiv(routeSelectChunkElements, 8U), kUbAlignBytes));
            const uint64_t sendFixedUbBytes =
                routePlanUbBytes + 2ULL * routeIndexUbBytes +
                2ULL * compareMaskUbBytes + kRelayUbBytes +
                2ULL * kDispatchUdmaIssueUbBytes;
            if (sendFixedUbBytes >= kFullUbBytes) {
                useVectorSlotSelect = false;
                routeSelectChunkElements = 0U;
                routePlanUbBytes = kRouteTileBytes;
                routeIndexUbBytes = 0U;
                compareMaskUbBytes = 0U;
            }
        }
        if (static_cast<uint64_t>(routePlanUbBytes) + kRelayUbBytes >=
            kFullUbBytes) {
            return;
        }
        if (groupedPeerMode && !localOnly && !useVectorSlotSelect) {
            DispatchPublishFirstStatus(planStatus,
                TileXRMoonEp::kDispatchStatusInvalidConfig);
            return;
        }

        const uint64_t outputCopyFixedUbBytes =
            kRelayUbBytes + kDiagnosticUbBytes;
        if (outputCopyFixedUbBytes >= kFullUbBytes) {
            return;
        }
        const uint64_t outputCopyBytesPerBuffer =
            (kFullUbBytes - outputCopyFixedUbBytes) / kOutputCopyBufferNum;
        const uint32_t outputCopyTileBytes = static_cast<uint32_t>(
            outputCopyBytesPerBuffer / kUbAlignBytes * kUbAlignBytes);

        AscendC::TPipe pipe;
        AscendC::TBuf<AscendC::QuePosition::VECCALC> routePlanBuf;
        AscendC::TBuf<AscendC::QuePosition::VECCALC> relayBuf;
        AscendC::TBuf<AscendC::QuePosition::VECCALC> udmaIssueQp0Buf;
        AscendC::TBuf<AscendC::QuePosition::VECCALC> udmaIssueQp1Buf;
        AscendC::TBuf<AscendC::QuePosition::VECCALC> routeIndexBuf;
        AscendC::TBuf<AscendC::QuePosition::VECCALC> selectedRouteIndexBuf;
        AscendC::TBuf<AscendC::QuePosition::VECCALC> lowerMaskBuf;
        AscendC::TBuf<AscendC::QuePosition::VECCALC> upperMaskBuf;
        AscendC::TBuf<AscendC::QuePosition::VECCALC> outputRelayBuf;
        AscendC::TBuf<AscendC::QuePosition::VECCALC> outputDiagnosticBuf;
        AscendC::TQue<AscendC::QuePosition::VECIN, 1> outputCopyQueue;

        const uint32_t routeIndexAllocBytes = useVectorSlotSelect ?
            routeIndexUbBytes : kUbAlignBytes;
        const uint32_t compareMaskAllocBytes = useVectorSlotSelect ?
            compareMaskUbBytes : kUbAlignBytes;
        pipe.Reset();
        pipe.InitBuffer(routePlanBuf, routePlanUbBytes);
        pipe.InitBuffer(relayBuf, kRelayUbBytes);
        pipe.InitBuffer(udmaIssueQp0Buf, kDispatchUdmaIssueUbBytes);
        pipe.InitBuffer(udmaIssueQp1Buf, kDispatchUdmaIssueUbBytes);
        pipe.InitBuffer(routeIndexBuf, routeIndexAllocBytes);
        pipe.InitBuffer(selectedRouteIndexBuf, routeIndexAllocBytes);
        pipe.InitBuffer(lowerMaskBuf, compareMaskAllocBytes);
        pipe.InitBuffer(upperMaskBuf, compareMaskAllocBytes);

        AscendC::LocalTensor<int32_t> routePlanLocal = routePlanBuf.Get<int32_t>();
        AscendC::LocalTensor<uint8_t> relayLocal = relayBuf.Get<uint8_t>();
        AscendC::LocalTensor<uint8_t> udmaIssueQp0Local =
            udmaIssueQp0Buf.Get<uint8_t>();
        AscendC::LocalTensor<uint8_t> udmaIssueQp1Local =
            udmaIssueQp1Buf.Get<uint8_t>();
        AscendC::LocalTensor<int16_t> routeIndexLocal = routeIndexBuf.Get<int16_t>();
        AscendC::LocalTensor<int16_t> selectedRouteIndexLocal =
            selectedRouteIndexBuf.Get<int16_t>();
        AscendC::LocalTensor<uint8_t> lowerMaskLocal = lowerMaskBuf.Get<uint8_t>();
        AscendC::LocalTensor<uint8_t> upperMaskLocal = upperMaskBuf.Get<uint8_t>();
        AscendC::LocalTensor<uint8_t> diagnosticLocal;

        for (uint64_t sourceRow = blockIdx; sourceRow < static_cast<uint64_t>(s);
            sourceRow += blockNum) {
            CopyBytesGmToGm(workspace + hiddenSourceOffset +
                    sourceRow * hiddenRowBytes,
                hiddenInput + sourceRow * hiddenRowBytes,
                static_cast<uint32_t>(hiddenRowBytes), relayLocal);
        }
        if (hasWeight) {
            for (uint64_t sourceRow = blockIdx; sourceRow < routeCount;
                sourceRow += blockNum) {
                CopyBytesGmToGm(workspace + weightSourceOffset +
                        sourceRow * weightRowBytes,
                    weightInput + sourceRow * weightRowBytes,
                static_cast<uint32_t>(weightRowBytes), relayLocal);
            }
        }

        AscendC::SyncAll<true>();

        const int32_t upstreamStatus = LoadDispatchPlanStatus(planStatus, relayLocal);
        auto currentHiddenScratch = workspace + hiddenScratchOffset +
            scratchIndex * hiddenScratchSlotBytes;
        auto currentWeightScratch = workspace + weightScratchOffset +
            scratchIndex * weightScratchSlotBytes;
        auto receiveFlags = reinterpret_cast<__gm__ uint64_t *>(
            workspace + completionFlagsOffset);
        auto signalSource = reinterpret_cast<__gm__ uint64_t *>(
            workspace + signalOffset + blockIdx * TileXRMoonEp::kDispatchSignalStrideBytes);
        if (!localOnly) {
            PublishDispatchSignalSource(signalSource, expectedFlag, relayLocal);
        }

        AscendC::GlobalTensor<int32_t> dstGlobal;
        dstGlobal.SetGlobalBuffer(dst, routeCount);
        uint32_t dfxFlags = upstreamStatus == TileXRMoonEp::kDispatchStatusSuccess ? 0U :
            TileXRMoonEp::kDispatchDfxUpstreamPlannerError;
        uint32_t firstInvalidRouteId = UINT32_MAX;
        int32_t firstInvalidRawDst = 0;
        uint32_t firstQuietStatus = 0U;
        uint32_t firstQuietPhase = 0U;
        uint32_t timeoutPeer = UINT32_MAX;
        uint32_t timeoutPhase = UINT32_MAX;
        uint64_t timeoutObservedFlag = 0U;
        uint64_t outgoingCqStatuses = UINT64_MAX;
        uint64_t outgoingRemainingSqEntries = UINT64_MAX;
        uint64_t selectedRouteCount = 0U;
        uint64_t processedRouteCount = 0U;
        uint64_t completionFlagCount = 0U;
        bool requiresFinalQuiet = !useVectorSlotSelect;
        bool operatorWqePrefillReady = true;
        if (useVectorSlotSelect && !localOnly &&
            upstreamStatus == TileXRMoonEp::kDispatchStatusSuccess) {
            operatorWqePrefillReady = PrefillDispatchOperatorWqes(
                udmaIssueQp0Local, udmaIssueQp1Local);
            if (!operatorWqePrefillReady) {
                dfxFlags |= TileXRMoonEp::kDispatchDfxInvalidConfig;
            }
        }

        const bool singleRouteChunkCached = useVectorSlotSelect &&
            routeCount <= static_cast<uint64_t>(routeSelectChunkElements);
        if (useVectorSlotSelect) {
            AscendC::CreateVecIndex(routeIndexLocal, static_cast<int16_t>(0),
                routeSelectChunkElements);
            AscendC::PipeBarrier<PIPE_V>();
            if (singleRouteChunkCached && upstreamStatus ==
                    TileXRMoonEp::kDispatchStatusSuccess) {
                LoadDispatchRouteChunk(dstGlobal, 0U,
                    static_cast<uint32_t>(routeCount), routePlanLocal, false);

            }
        }

        if (useVectorSlotSelect && groupedPeerMode) {
            const uint64_t remoteHiddenScratchOffset = hiddenScratchOffset +
                scratchIndex * hiddenScratchSlotBytes;
            const uint64_t remoteWeightScratchOffset = weightScratchOffset +
                scratchIndex * weightScratchSlotBytes;
            const uint64_t remoteFlagBase = completionFlagsOffset +
                static_cast<uint64_t>(rank) *
                    TileXRMoonEp::kDispatchQpCount * sizeof(uint64_t);
            DispatchWqeBatchInitContext initContext {};
            const bool initContextValid = localOnly ||
                (operatorWqePrefillReady &&
                    InitDispatchWqeBatchInitContext(args,
                    remoteHiddenScratchOffset, hiddenScratchSlotBytes,
                    remoteWeightScratchOffset, weightScratchSlotBytes,
                    remoteFlagBase, static_cast<uint32_t>(blockIdx),
                    hasWeight, kDispatchGroupedAllowFullmesh, initContext));
            DispatchPreparedPeer previousPeer {};
            bool previousPeerValid = false;

            for (uint32_t peerWork = 0U; peerWork < peerWorkCount; ++peerWork) {
                uint32_t group = UINT32_MAX;
                uint32_t lane = UINT32_MAX;
                const int64_t peerValue = TileXRMoonEp::DispatchGroupedPeerForCore(
                    rank, rankSize, groupWidth, static_cast<uint32_t>(blockIdx),
                    peerCoreCount, peerWork, group, lane);
                if (peerValue < 0) {
                    continue;
                }
                if (peerValue == rank) {
                    continue;
                }
                const int32_t peer = static_cast<int32_t>(peerValue);
                const bool payloadReady = upstreamStatus ==
                    TileXRMoonEp::kDispatchStatusSuccess && dfxFlags == 0U;
                DispatchPreparedPeer preparedPeer {};

                const bool peerInitialized = initContextValid &&
                    InitDispatchPreparedPeer(initContext, peer, group, preparedPeer);

                if (!peerInitialized) {
                    dfxFlags |= TileXRMoonEp::kDispatchDfxInvalidConfig;
                    break;
                }
                if (preparedPeer.fullmesh && !DispatchDrainHistoricalCq(
                        preparedPeer, relayLocal, completionTimeoutTicks,
                        dfxFlags, firstQuietStatus, firstQuietPhase,
                        timeoutPeer, timeoutPhase, timeoutObservedFlag)) {
                    dfxFlags |= TileXRMoonEp::kDispatchDfxCqError;
                    break;
                }
                if (creditPeerMode &&
                    TileXRMoonEp::DispatchCreditRequired(group)) {
                    const int64_t previousIncomingPeer =
                        TileXRMoonEp::DispatchGroupedPeer(
                            rank, rankSize, group - 1U, lane, groupWidth);
                    const bool incomingAlreadyWaited =
                        previousPeerValid &&
                        previousPeer.targetRank == previousIncomingPeer &&
                        previousPeer.issuePhase == group - 1U;
                    if (previousIncomingPeer < 0 ||
                        (!incomingAlreadyWaited &&
                         !WaitDispatchIncomingPeer(receiveFlags,
                            static_cast<int32_t>(previousIncomingPeer),
                            group - 1U,
                            TileXRMoonEp::DispatchCompletionLaneCount(
                                rank, previousIncomingPeer, args->localRankSize),
                            magic, completionTimeoutTicks,
                            relayLocal, dfxFlags, timeoutPeer, timeoutPhase,
                            timeoutObservedFlag))) {
                        if (previousIncomingPeer < 0) {
                            dfxFlags |= TileXRMoonEp::kDispatchDfxInvalidConfig;
                        }
                        break;
                    }
                    if (previousPeerValid && !dedicatedCreditQp) {
                        const bool previousCqOk =
                            DispatchDrainPeerFinalCq(previousPeer,
                                relayLocal, completionTimeoutTicks, dfxFlags,
                                firstQuietStatus, firstQuietPhase, timeoutPeer,
                                timeoutPhase, timeoutObservedFlag);
                        previousPeerValid = false;
                        if (!previousCqOk) {
                            break;
                        }
                    }
                    DispatchWqeBatchState creditQpState {};
                    if (dedicatedCreditQp && !preparedPeer.fullmesh) {
                        if (!DrainDispatchCreditQp(args, udmaInfo, peer,
                                creditPhysicalQp, relayLocal,
                                completionTimeoutTicks, group, dfxFlags,
                                firstQuietStatus, firstQuietPhase,
                                timeoutPeer, timeoutPhase,
                                timeoutObservedFlag, creditQpState)) {
                            dfxFlags |= TileXRMoonEp::kDispatchDfxCqError;
                            break;
                        }
                    }
                    DispatchWqeBatchState *creditState = preparedPeer.fullmesh ?
                        &preparedPeer.qpState[0] : &creditQpState;
                    if ((dedicatedCreditQp || preparedPeer.fullmesh) &&
                        !DispatchEnsureSqBatchCapacity(*creditState, 1U,
                            relayLocal, group, dfxFlags, firstQuietStatus,
                            firstQuietPhase)) {
                        break;
                    }

                    const bool published = PublishDispatchPeerCredit(
                        args, workspace, creditOffset, creditSourceOffset,
                        static_cast<uint32_t>(blockIdx),
                        preparedPeer.fullmesh ?
                            preparedPeer.qpState[0].physicalQpIdx :
                            (dedicatedCreditQp ? creditPhysicalQp : physicalQp[0]),
                        rank, peer, group, magic, relayLocal,
                        preparedPeer.fullmesh ? &preparedPeer.qpState[0] : nullptr);
                    if (published && dedicatedCreditQp &&
                        !preparedPeer.fullmesh) {
                        RecordDispatchCreditQpPost(creditQpState, peer);
                    }

                    if (!published) {
                        dfxFlags |= TileXRMoonEp::kDispatchDfxInvalidConfig;
                        break;
                    }
                }
                const bool previousCqTracked =
                    dedicatedCreditQp && previousPeerValid &&
                    !preparedPeer.fullmesh && !previousPeer.fullmesh;

                const bool historicalCqOk = preparedPeer.fullmesh ||
                    previousCqTracked ||
                    DispatchDrainHistoricalCq(preparedPeer, relayLocal,
                        completionTimeoutTicks, dfxFlags, firstQuietStatus,
                        firstQuietPhase, timeoutPeer, timeoutPhase,
                        timeoutObservedFlag);

                if (!historicalCqOk) {
                    dfxFlags |= TileXRMoonEp::kDispatchDfxCqError;
                    break;
                }
                if (!PrefillDispatchPeerWqes(
                        udmaIssueQp0Local, udmaIssueQp1Local,
                        preparedPeer)) {
                    dfxFlags |= TileXRMoonEp::kDispatchDfxInvalidConfig;
                    break;
                }

                bool firstLogicalBatch = true;
                bool sendOk = true;
                uint32_t sequencePhase = 0U;
                uint64_t peerSelectedCount = 0U;
                for (uint64_t chunkStart = 0U;
                    sendOk && chunkStart < routeCount;
                    chunkStart += routeSelectChunkElements) {
                    const uint64_t chunkRemaining = routeCount - chunkStart;
                    const uint32_t chunkElements = static_cast<uint32_t>(
                        chunkRemaining < routeSelectChunkElements ?
                        chunkRemaining : routeSelectChunkElements);
                    if (!singleRouteChunkCached) {
                        LoadDispatchRouteChunk(dstGlobal, chunkStart,
                            chunkElements, routePlanLocal, chunkStart != 0U);

                    }

                    const uint32_t selectedCount = SelectDispatchPeerRoutes(
                        lowerMaskLocal, upperMaskLocal, routePlanLocal,
                        routeIndexLocal, selectedRouteIndexLocal, peer,
                        destinationCapacity, chunkElements,
                        compareMaskUbBytes / sizeof(uint16_t));

                    selectedRouteCount += selectedCount;
                    peerSelectedCount += selectedCount;

                    uint32_t qpSelectedCount[TileXRMoonEp::kDispatchQpCount] = {};
                    uint32_t qpDataTaskCount[TileXRMoonEp::kDispatchQpCount] = {};
                    uint32_t qpDataTaskStart[TileXRMoonEp::kDispatchQpCount] = {};
                    const bool appendSignal =
                        chunkStart + chunkElements == routeCount;
                    bool qpSignalPending[TileXRMoonEp::kDispatchQpCount] = {
                        appendSignal,
                        appendSignal && preparedPeer.qpCount > 1U};
                    uint32_t qpChunkWqeCount[
                        TileXRMoonEp::kDispatchQpCount] = {};
                    for (uint32_t qpIdx = 0U;
                        qpIdx < preparedPeer.qpCount; ++qpIdx) {
                        qpSelectedCount[qpIdx] = preparedPeer.fullmesh ?
                            selectedCount : TileXRMoonEp::DispatchQpRouteCount(
                                selectedCount, sequencePhase, qpIdx);
                        qpDataTaskCount[qpIdx] = qpSelectedCount[qpIdx] *
                            TileXRMoonEp::DispatchPayloadWqesPerRoute(hasWeight);
                        qpChunkWqeCount[qpIdx] = qpDataTaskCount[qpIdx] +
                            (qpSignalPending[qpIdx] ? 1U : 0U);
                    }
                    if (previousPeerValid && dedicatedCreditQp &&
                        !preparedPeer.fullmesh && !previousPeer.fullmesh &&
                        !DispatchPeerChunkFitsWithoutCq(
                            preparedPeer, qpChunkWqeCount)) {
                        sendOk = DispatchDrainPeerFinalCq(previousPeer,
                            relayLocal, completionTimeoutTicks, dfxFlags,
                            firstQuietStatus, firstQuietPhase, timeoutPeer,
                            timeoutPhase, timeoutObservedFlag);
                        previousPeerValid = false;
                    }
                    if (!sendOk) {
                        break;
                    }
                    if (!ReserveDispatchPeerChunkSq(preparedPeer,
                            qpChunkWqeCount, relayLocal, group, dfxFlags,
                            firstQuietStatus, firstQuietPhase)) {
                        sendOk = false;
                        break;
                    }
                    while (sendOk &&
                        (qpDataTaskStart[0] < qpDataTaskCount[0] ||
                         qpSignalPending[0] ||
                         (preparedPeer.qpCount > 1U &&
                            (qpDataTaskStart[1] < qpDataTaskCount[1] ||
                             qpSignalPending[1])))) {
                        bool finalBatch[TileXRMoonEp::kDispatchQpCount] = {};
                        for (uint32_t qpIdx = 0U;
                            sendOk && qpIdx < preparedPeer.qpCount;
                            ++qpIdx) {
                            DispatchWqeBatchState &qpState =
                                preparedPeer.qpState[qpIdx];
                            AscendC::LocalTensor<uint8_t> issueLocal =
                                qpIdx == 0U ? udmaIssueQp0Local : udmaIssueQp1Local;

                            sendOk = DispatchBuildGroupedQpBatch(qpState,
                                issueLocal, selectedRouteIndexLocal, routePlanLocal,
                                reinterpret_cast<uint64_t>(workspace +
                                    hiddenSourceOffset), hiddenRowBytes,
                                reinterpret_cast<uint64_t>(workspace +
                                    weightSourceOffset), weightRowBytes,
                                hasWeight,
                                destinationCapacity, topKMagic, topKShift,
                                qpSelectedCount[qpIdx],
                                qpDataTaskStart[qpIdx], qpSignalPending[qpIdx],
                                reinterpret_cast<uint64_t>(signalSource + qpIdx),
                                sequencePhase, static_cast<uint32_t>(chunkStart),
                                finalBatch[qpIdx]);

                        }
                        for (uint32_t qpIdx = 0U;
                            sendOk && qpIdx < preparedPeer.qpCount;
                            ++qpIdx) {
                            if (preparedPeer.qpState[qpIdx].batchCount == 0U) {
                                continue;
                            }
                            sendOk = StageDispatchQpBatch(
                                preparedPeer.qpState[qpIdx],
                                qpIdx == 0U ? udmaIssueQp0Local : udmaIssueQp1Local,
                                relayLocal, finalBatch[qpIdx], group, dfxFlags,
                                firstQuietStatus, firstQuietPhase);
                        }
                        if (sendOk && firstLogicalBatch && previousPeerValid) {
                            sendOk = DispatchDrainPeerFinalCq(previousPeer,
                                relayLocal, completionTimeoutTicks, dfxFlags,
                                firstQuietStatus, firstQuietPhase, timeoutPeer,
                                timeoutPhase, timeoutObservedFlag);
                            previousPeerValid = false;
                        }
                        if (sendOk && firstLogicalBatch && creditPeerMode &&
                            TileXRMoonEp::DispatchCreditRequired(group)) {
                            uint64_t observedCredit = 0U;

                            const bool creditReady = WaitDispatchPeerCredit(
                                    workspace, creditOffset, peer, group,
                                    magic, completionTimeoutTicks, relayLocal,
                                    observedCredit);

                            if (!creditReady) {
                                dfxFlags |= TileXRMoonEp::kDispatchDfxCreditTimeout;
                                if (timeoutPeer == UINT32_MAX) {
                                    timeoutPeer = static_cast<uint32_t>(peer);
                                    timeoutPhase = group;
                                    timeoutObservedFlag = observedCredit;
                                }
                                sendOk = false;
                            }
                        }
                        firstLogicalBatch = false;
                    }
                    if (sendOk) {
                        RingDispatchPeerDoorbells(
                            preparedPeer);
                    }
                    sequencePhase = (sequencePhase + selectedCount) & 3U;
                }

                const bool optimizedSignalSubmitted = sendOk &&
                    preparedPeer.qpState[0].finalStaged != 0U &&
                    (preparedPeer.qpCount == 1U ||
                        preparedPeer.qpState[1].finalStaged != 0U);
                bool completionSubmitted = optimizedSignalSubmitted;
                if (!optimizedSignalSubmitted) {
                    if ((dfxFlags & (TileXRMoonEp::kDispatchDfxCreditTimeout |
                            TileXRMoonEp::kDispatchDfxCqError)) == 0U) {
                        dfxFlags |= TileXRMoonEp::kDispatchDfxInvalidConfig;
                    }
                    requiresFinalQuiet = true;
                    completionSubmitted = true;
                    for (uint32_t qpIdx = 0U;
                        qpIdx < preparedPeer.qpCount; ++qpIdx) {
                        if (preparedPeer.fullmesh) {
                            completionSubmitted = false;
                            break;
                        }
                        const uint32_t signalStatus =
                            TileXR::UDMAPutNbiOnQpWithFlagDeferred<uint64_t>(
                                args, qpIdx == 0U ? udmaIssueQp0Local :
                                    udmaIssueQp1Local,
                                peer, physicalQp[qpIdx], signalSource + qpIdx,
                                remoteFlagBase + qpIdx * sizeof(uint64_t),
                                static_cast<uint32_t>(sizeof(uint64_t)),
                                TileXR::TILEXR_UDMA_SQE_FLAG_ORDERED_COMPLETION);
                        const uint32_t flushStatus =
                            TileXR::UDMAFlushQpDoorbell(
                                args, peer, physicalQp[qpIdx]);
                        if (signalStatus != TileXR::TILEXR_UDMA_STATUS_SUCCESS ||
                            flushStatus != TileXR::TILEXR_UDMA_STATUS_SUCCESS) {
                            completionSubmitted = false;
                            dfxFlags |= TileXRMoonEp::kDispatchDfxQuietError;
                            if (firstQuietStatus == 0U) {
                                firstQuietStatus = signalStatus !=
                                    TileXR::TILEXR_UDMA_STATUS_SUCCESS ?
                                    signalStatus : flushStatus;
                                firstQuietPhase = group;
                            }
                        }
                    }
                }
                processedRouteCount += peerSelectedCount;
                completionFlagCount += preparedPeer.qpCount;
                previousPeer = preparedPeer;
                previousPeerValid = true;

                if (!WaitDispatchIncomingPeer(receiveFlags, peer, group,
                        TileXRMoonEp::DispatchCompletionLaneCount(
                            rank, peer, args->localRankSize),
                        magic, completionTimeoutTicks, relayLocal, dfxFlags,
                        timeoutPeer, timeoutPhase,
                        timeoutObservedFlag)) {
                    break;
                }
            }
            if (previousPeerValid && !DispatchDrainPeerFinalCq(previousPeer,
                    relayLocal, completionTimeoutTicks, dfxFlags,
                    firstQuietStatus, firstQuietPhase, timeoutPeer,
                    timeoutPhase, timeoutObservedFlag)) {
                dfxFlags |= TileXRMoonEp::kDispatchDfxCqError;
            }
            if (dedicatedCreditQp) {
                for (uint32_t peerWork = 0U;
                    peerWork < peerWorkCount; ++peerWork) {
                    uint32_t creditGroup = UINT32_MAX;
                    uint32_t creditLane = UINT32_MAX;
                    const int64_t creditPeer =
                        TileXRMoonEp::DispatchGroupedPeerForCore(
                            rank, rankSize, groupWidth,
                            static_cast<uint32_t>(blockIdx), peerCoreCount,
                            peerWork, creditGroup, creditLane);
                    if (creditPeer < 0 || creditPeer == rank ||
                        !TileXRMoonEp::DispatchCreditRequired(creditGroup) ||
                        TileXRMoonEp::DispatchSameServer(
                            rank, creditPeer, args->localRankSize)) {
                        continue;
                    }
                    DispatchWqeBatchState creditQpState {};
                    if (!DrainDispatchCreditQp(args, udmaInfo,
                            static_cast<int32_t>(creditPeer), creditPhysicalQp,
                            relayLocal, completionTimeoutTicks, creditGroup,
                            dfxFlags, firstQuietStatus, firstQuietPhase,
                            timeoutPeer, timeoutPhase, timeoutObservedFlag,
                            creditQpState)) {
                        dfxFlags |= TileXRMoonEp::kDispatchDfxCqError;
                        break;
                    }
                }
            }
        } else if (useVectorSlotSelect) {
            const uint64_t remoteHiddenScratchOffset = hiddenScratchOffset +
                scratchIndex * hiddenScratchSlotBytes;
            const uint64_t remoteWeightScratchOffset = weightScratchOffset +
                scratchIndex * weightScratchSlotBytes;
            const uint64_t remoteFlagBase = completionFlagsOffset +
                static_cast<uint64_t>(rank) *
                    TileXRMoonEp::kDispatchQpCount * sizeof(uint64_t);
            DispatchWqeBatchInitContext initContext {};
            const bool initContextValid = operatorWqePrefillReady &&
                InitDispatchWqeBatchInitContext(args,
                    remoteHiddenScratchOffset, hiddenScratchSlotBytes,
                    remoteWeightScratchOffset, weightScratchSlotBytes,
                    remoteFlagBase, static_cast<uint32_t>(blockIdx), hasWeight,
                    false, initContext);
            DispatchPreparedPeer peerBatch[kDispatchPreparedPeerCapacity] {};
            uint64_t peerCursor = 0U;
            const uint64_t totalPeerAssignments =
                static_cast<uint64_t>(groupCount) * peerWorkCount;
            bool preparedPeerPathActive = true;
            while (preparedPeerPathActive && peerCursor < totalPeerAssignments) {
                uint32_t peerCount = 0U;
                PrepareDispatchRemotePeerBatch(
                    initContextValid ? &initContext : nullptr, rank, rankSize,
                    groupCount, peerWorkCount, static_cast<uint32_t>(blockIdx),
                    peerCoreCount, peerCursor, peerBatch,
                    peerCount);
                for (uint32_t peerIndex = 0U;
                    peerIndex < peerCount; ++peerIndex) {
                    DispatchPreparedPeer &preparedPeer = peerBatch[peerIndex];
                    const int32_t peer = preparedPeer.targetRank;
                    const uint32_t issuePhase = preparedPeer.issuePhase;
                    if (upstreamStatus != TileXRMoonEp::kDispatchStatusSuccess) {
                        requiresFinalQuiet = true;
                        for (uint32_t qpIdx = 0U;
                            qpIdx < TileXRMoonEp::kDispatchQpCount; ++qpIdx) {
                            TileXR::UDMAPutNbiOnQpWithFlagDeferred<uint64_t>(
                                args, qpIdx == 0U ? udmaIssueQp0Local :
                                    udmaIssueQp1Local,
                                peer, physicalQp[qpIdx], signalSource + qpIdx,
                                remoteFlagBase + qpIdx * sizeof(uint64_t),
                                static_cast<uint32_t>(sizeof(uint64_t)),
                                TileXR::TILEXR_UDMA_SQE_FLAG_ORDERED_COMPLETION);
                            TileXR::UDMAFlushQpDoorbell(
                                args, peer, physicalQp[qpIdx]);
                        }
                        completionFlagCount += TileXRMoonEp::kDispatchQpCount;
                        continue;
                    }

                    if (!preparedPeer.initialized) {
                        dfxFlags |= TileXRMoonEp::kDispatchDfxInvalidConfig;
                        preparedPeerPathActive = false;
                        break;
                    }
                    if (!PrefillDispatchPeerWqes(
                            udmaIssueQp0Local, udmaIssueQp1Local,
                            preparedPeer)) {
                        dfxFlags |= TileXRMoonEp::kDispatchDfxInvalidConfig;
                        preparedPeerPathActive = false;
                        break;
                    }

                    bool batchOk = true;
                    uint32_t sequencePhase = 0U;
                    uint64_t peerSelectedCount = 0U;
                    for (uint64_t chunkStart = 0U;
                        batchOk && chunkStart < routeCount;
                        chunkStart += routeSelectChunkElements) {
                        const uint64_t chunkRemaining = routeCount - chunkStart;
                        const uint32_t chunkElements = static_cast<uint32_t>(
                            chunkRemaining < routeSelectChunkElements ?
                            chunkRemaining : routeSelectChunkElements);
                        if (!singleRouteChunkCached) {
                            LoadDispatchRouteChunk(dstGlobal, chunkStart,
                                chunkElements, routePlanLocal, chunkStart != 0U);

                        }

                        const uint32_t selectedCount = SelectDispatchPeerRoutes(
                            lowerMaskLocal, upperMaskLocal, routePlanLocal,
                            routeIndexLocal, selectedRouteIndexLocal, peer,
                            destinationCapacity, chunkElements,
                            compareMaskUbBytes / sizeof(uint16_t));

                        selectedRouteCount += selectedCount;
                        peerSelectedCount += selectedCount;

                        const bool appendSignal =
                            chunkStart + chunkElements == routeCount;
                        uint32_t qpRouteCount[
                            TileXRMoonEp::kDispatchQpCount] = {};
                        uint32_t qpChunkWqeCount[
                            TileXRMoonEp::kDispatchQpCount] = {};
                        for (uint32_t qpIdx = 0U;
                            qpIdx < TileXRMoonEp::kDispatchQpCount; ++qpIdx) {
                            qpRouteCount[qpIdx] =
                                TileXRMoonEp::DispatchQpRouteCount(
                                    selectedCount, sequencePhase, qpIdx);
                            qpChunkWqeCount[qpIdx] = qpRouteCount[qpIdx] *
                                TileXRMoonEp::DispatchPayloadWqesPerRoute(hasWeight) +
                                (appendSignal ? 1U : 0U);
                        }
                        if (!ReserveDispatchPeerChunkSq(preparedPeer,
                                qpChunkWqeCount, relayLocal, issuePhase,
                                dfxFlags, firstQuietStatus, firstQuietPhase)) {
                            batchOk = false;
                            break;
                        }
                        for (uint32_t qpIdx = 0U;
                            batchOk && qpIdx < TileXRMoonEp::kDispatchQpCount;
                            ++qpIdx) {
                            batchOk = AppendDispatchWqes(
                                preparedPeer.qpState[qpIdx],
                                qpIdx == 0U ? udmaIssueQp0Local :
                                    udmaIssueQp1Local,
                                relayLocal, selectedRouteIndexLocal,
                                routePlanLocal,
                                reinterpret_cast<uint64_t>(workspace +
                                    hiddenSourceOffset), hiddenRowBytes,
                                reinterpret_cast<uint64_t>(workspace +
                                    weightSourceOffset), weightRowBytes,
                                hasWeight,
                                destinationCapacity, topKMagic, topKShift,
                                qpRouteCount[qpIdx], appendSignal,
                                reinterpret_cast<uint64_t>(signalSource + qpIdx),
                                issuePhase, dfxFlags, firstQuietStatus,
                                firstQuietPhase, sequencePhase,
                                static_cast<uint32_t>(chunkStart));
                        }
                        if (batchOk) {
                            RingDispatchPeerDoorbells(preparedPeer);
                        }
                        sequencePhase = (sequencePhase + selectedCount) & 3U;
                    }
                    if (!batchOk) {
                        dfxFlags |= TileXRMoonEp::kDispatchDfxQuietError;
                        if (firstQuietStatus == 0U) {
                            firstQuietStatus = 0xFFFFFFFDU;
                            firstQuietPhase = issuePhase;
                        }
                        preparedPeerPathActive = false;
                        break;
                    }
                    processedRouteCount += peerSelectedCount;
                    completionFlagCount += TileXRMoonEp::kDispatchQpCount;
                }
            }

        } else {
            for (int64_t issuePhase = 0; issuePhase < groupCount; ++issuePhase) {
                for (uint32_t peerWork = 0U;
                    peerWork < peerWorkCount; ++peerWork) {
                    const int64_t peer = TileXRMoonEp::DispatchPeerForCore(
                        rank, rankSize, issuePhase,
                        static_cast<uint32_t>(blockIdx),
                        peerCoreCount, peerWork);
                    if (peer < 0) {
                        continue;
                    }
                    if (peer == rank) {
                        continue;
                    }
                    uint64_t issuedPeerWqeCount = 0U;
                    if (upstreamStatus ==
                        TileXRMoonEp::kDispatchStatusSuccess) {
                        for (uint64_t tileStart = 0U;
                            tileStart < routeCount;
                            tileStart += kRouteTileElements) {
                            const uint32_t tileElements = LoadRouteTile(
                                dstGlobal, tileStart, routeCount,
                                routePlanLocal);
                            for (uint32_t tileRoute = 0U;
                                tileRoute < tileElements; ++tileRoute) {
                                const uint64_t routeId = tileStart + tileRoute;
                                const int32_t encoded =
                                    routePlanLocal.GetValue(tileRoute);
                                int32_t targetRank = -1;
                                uint64_t targetSlot = 0U;
                                if (!DecodeSendDst(encoded, destinationCapacity,
                                        rankSize, targetRank, targetSlot)) {
                                    RecordInvalidRoute(routeId,
                                        DecodeRawDst(encoded), dfxFlags,
                                        firstInvalidRouteId,
                                        firstInvalidRawDst);
                                    continue;
                                }
                                if (targetRank != peer) {
                                    continue;
                                }
                                ++selectedRouteCount;
                                TileXR::UDMAPutNbiOnQpWithFlagDeferred<uint8_t>(
                                    args, udmaIssueQp0Local, targetRank,
                                    physicalQp[0],
                                    workspace + hiddenSourceOffset +
                                        routeId / static_cast<uint64_t>(k) *
                                            hiddenRowBytes,
                                    hiddenScratchOffset +
                                        scratchIndex * hiddenScratchSlotBytes +
                                        targetSlot * hiddenRowBytes,
                                    static_cast<uint32_t>(hiddenRowBytes),
                                    TileXR::TILEXR_UDMA_SQE_FLAG_COMPLETION);
                                ++issuedPeerWqeCount;
                                ReclaimDeferredSegment(args, targetRank,
                                    physicalQp[0],
                                    issuedPeerWqeCount,
                                    static_cast<uint32_t>(issuePhase),
                                    dfxFlags, firstQuietStatus,
                                    firstQuietPhase);
                                if (hasWeight) {
                                    TileXR::UDMAPutNbiOnQpWithFlagDeferred<uint8_t>(
                                        args, udmaIssueQp0Local, targetRank,
                                        physicalQp[0],
                                        workspace + weightSourceOffset +
                                            routeId * weightRowBytes,
                                        weightScratchOffset +
                                            scratchIndex * weightScratchSlotBytes +
                                            targetSlot * weightRowBytes,
                                        static_cast<uint32_t>(weightRowBytes),
                                        TileXR::TILEXR_UDMA_SQE_FLAG_COMPLETION);
                                    ++issuedPeerWqeCount;
                                    ReclaimDeferredSegment(args, targetRank,
                                        physicalQp[0], issuedPeerWqeCount,
                                        static_cast<uint32_t>(issuePhase),
                                        dfxFlags, firstQuietStatus,
                                        firstQuietPhase);
                                }
                                ++processedRouteCount;
                                if (issuedPeerWqeCount %
                                        TileXRMoonEp::kDispatchWqeBatchCapacity == 0U) {
                                    TileXR::UDMAFlushQpDoorbell(
                                        args, targetRank, physicalQp[0]);
                                }
                            }
                        }
                    }

                    if (peer != rank) {
                        const uint64_t remoteFlagBase = completionFlagsOffset +
                            static_cast<uint64_t>(rank) *
                                TileXRMoonEp::kDispatchQpCount *
                                sizeof(uint64_t);
                        for (uint32_t qpIdx = 0U;
                            qpIdx < TileXRMoonEp::kDispatchQpCount; ++qpIdx) {
                            TileXR::UDMAPutNbiOnQpWithFlagDeferred<uint64_t>(
                                args, qpIdx == 0U ? udmaIssueQp0Local :
                                    udmaIssueQp1Local,
                                static_cast<int32_t>(peer), physicalQp[qpIdx],
                                signalSource + qpIdx,
                                remoteFlagBase + qpIdx * sizeof(uint64_t),
                                static_cast<uint32_t>(sizeof(uint64_t)),
                                TileXR::TILEXR_UDMA_SQE_FLAG_ORDERED_COMPLETION);
                            TileXR::UDMAFlushQpDoorbell(
                                args, static_cast<int32_t>(peer),
                                physicalQp[qpIdx]);
                        }
                        completionFlagCount +=
                            TileXRMoonEp::kDispatchQpCount;
                    }
                }
            }
        }

        DispatchLocalCopyPipeline localCopyPipeline {};
        InitDispatchLocalCopyPipeline(localCopyPipeline,
            udmaIssueQp0Local, udmaIssueQp1Local);
        if (upstreamStatus == TileXRMoonEp::kDispatchStatusSuccess) {
            if (useVectorSlotSelect) {
                for (uint64_t chunkStart = 0U; chunkStart < routeCount;
                    chunkStart += routeSelectChunkElements) {
                    const uint64_t chunkRemaining = routeCount - chunkStart;
                    const uint32_t chunkElements = static_cast<uint32_t>(
                        chunkRemaining < routeSelectChunkElements ?
                        chunkRemaining : routeSelectChunkElements);
                    if (!singleRouteChunkCached) {
                        LoadDispatchRouteChunk(dstGlobal, chunkStart,
                            chunkElements, routePlanLocal, chunkStart != 0U);
                    }
                    const uint32_t localRouteCount = SelectDispatchPeerRoutes(
                        lowerMaskLocal, upperMaskLocal, routePlanLocal,
                        routeIndexLocal, selectedRouteIndexLocal, rank,
                        destinationCapacity, chunkElements,
                        compareMaskUbBytes / sizeof(uint16_t));
                    uint64_t localStart = 0U;
                    uint64_t localEnd = 0U;
                    TileXRMoonEp::DispatchContiguousRange(localRouteCount,
                        static_cast<uint32_t>(blockNum),
                        static_cast<uint32_t>(blockIdx), localStart, localEnd);
                    const uint64_t localCount = localEnd - localStart;
                    selectedRouteCount += localCount;
                    for (uint64_t selected = localStart;
                        selected < localEnd; ++selected) {
                        const uint64_t routeInChunk = static_cast<uint16_t>(
                            selectedRouteIndexLocal.GetValue(selected));
                        const uint64_t routeId = chunkStart + routeInChunk;
                        if (routeInChunk >= chunkElements) {
                            RecordInvalidRoute(routeId, 0, dfxFlags,
                                firstInvalidRouteId, firstInvalidRawDst);
                            continue;
                        }
                        const int32_t rawDst = routePlanLocal.GetValue(routeInChunk);
                        int32_t targetRank = -1;
                        uint64_t targetSlot = 0U;
                        if (!DecodeSendDst(rawDst, destinationCapacity, rankSize,
                                targetRank, targetSlot) || targetRank != rank) {
                            RecordInvalidRoute(routeId, rawDst, dfxFlags,
                                firstInvalidRouteId, firstInvalidRawDst);
                            continue;
                        }
                        SubmitDispatchLocalCopy(localCopyPipeline,
                            currentHiddenScratch + targetSlot * hiddenRowBytes,
                            workspace + hiddenSourceOffset +
                                routeId / static_cast<uint64_t>(k) * hiddenRowBytes,
                            static_cast<uint32_t>(hiddenRowBytes));
                        if (hasWeight) {
                            SubmitDispatchLocalCopy(localCopyPipeline,
                                currentWeightScratch + targetSlot * weightRowBytes,
                                workspace + weightSourceOffset +
                                    routeId * weightRowBytes,
                                static_cast<uint32_t>(weightRowBytes));
                        }
                        ++processedRouteCount;
                    }
                }
            } else {
                uint64_t localStart = 0U;
                uint64_t localEnd = 0U;
                TileXRMoonEp::DispatchContiguousRange(routeCount,
                    static_cast<uint32_t>(blockNum),
                    static_cast<uint32_t>(blockIdx), localStart, localEnd);
                for (uint64_t tileStart = localStart;
                    tileStart < localEnd; tileStart += kRouteTileElements) {
                    const uint32_t tileElements = LoadRouteTile(
                        dstGlobal, tileStart, localEnd, routePlanLocal);
                    for (uint32_t tileRoute = 0U;
                        tileRoute < tileElements; ++tileRoute) {
                        const uint64_t routeId = tileStart + tileRoute;
                        const int32_t rawDst =
                            routePlanLocal.GetValue(tileRoute);
                        int32_t targetRank = -1;
                        uint64_t targetSlot = 0U;
                        if (!DecodeSendDst(rawDst, destinationCapacity, rankSize,
                                targetRank, targetSlot)) {
                            RecordInvalidRoute(routeId, rawDst, dfxFlags,
                                firstInvalidRouteId, firstInvalidRawDst);
                            continue;
                        }
                        if (targetRank != rank) {
                            continue;
                        }
                        ++selectedRouteCount;
                        SubmitDispatchLocalCopy(localCopyPipeline,
                            currentHiddenScratch + targetSlot * hiddenRowBytes,
                            workspace + hiddenSourceOffset +
                                routeId / static_cast<uint64_t>(k) * hiddenRowBytes,
                            static_cast<uint32_t>(hiddenRowBytes));
                        if (hasWeight) {
                            SubmitDispatchLocalCopy(localCopyPipeline,
                                currentWeightScratch + targetSlot * weightRowBytes,
                                workspace + weightSourceOffset +
                                    routeId * weightRowBytes,
                                static_cast<uint32_t>(weightRowBytes));
                        }
                        ++processedRouteCount;
                    }
                }
            }
        }
        DrainDispatchLocalCopyPipeline();

        // Finish every send-side consumer before reusing the send UB layout.
        AscendC::PipeBarrier<PIPE_ALL>();
        pipe.Reset();
        pipe.InitBuffer(outputRelayBuf, kRelayUbBytes);
        pipe.InitBuffer(outputDiagnosticBuf, kDiagnosticUbBytes);
        if (outputCopyTileBytes != 0U) {
            pipe.InitBuffer(outputCopyQueue,
                static_cast<uint8_t>(kOutputCopyBufferNum),
                outputCopyTileBytes);
        }
        relayLocal = outputRelayBuf.Get<uint8_t>();
        diagnosticLocal = outputDiagnosticBuf.Get<uint8_t>();

        const uint64_t flagWaitStartCycle =
            static_cast<uint64_t>(AscendC::GetSystemCycle());
        if (!groupedPeerMode) {
            for (int64_t completionPhase = 0; completionPhase < groupCount;
                ++completionPhase) {
                for (uint32_t peerWork = 0U;
                    peerWork < peerWorkCount; ++peerWork) {
                    const int64_t peer = TileXRMoonEp::DispatchPeerForCore(
                        rank, rankSize, completionPhase,
                        static_cast<uint32_t>(blockIdx),
                        peerCoreCount, peerWork);
                    if (peer >= 0 && peer != rank) {
                        const uint64_t peerFlagBase = static_cast<uint64_t>(peer) *
                            TileXRMoonEp::kDispatchQpCount;
                        for (uint32_t qpIdx = 0U;
                            qpIdx < TileXRMoonEp::kDispatchQpCount; ++qpIdx) {
                            uint64_t observed = 0U;

                            const bool completionReady = WaitCompletionFlag(
                                    receiveFlags + peerFlagBase + qpIdx,
                                    expectedFlag, flagWaitStartCycle,
                                    completionTimeoutTicks, observed);

                            if (!completionReady) {
                                dfxFlags |= TileXRMoonEp::
                                    kDispatchDfxCompletionTimeout;
                                if (timeoutPeer == UINT32_MAX) {
                                    timeoutPeer = static_cast<uint32_t>(peer);
                                    timeoutPhase = static_cast<uint32_t>(
                                        completionPhase);
                                    timeoutObservedFlag = observed;
                                }
                            }
                        }
                    }
                }
            }
        }
        if (selectedRouteCount != processedRouteCount) {
            dfxFlags |= TileXRMoonEp::kDispatchDfxRouteCountMismatch;
        }
        if (upstreamStatus == TileXRMoonEp::kDispatchStatusSuccess &&
            (!ClearDispatchZeroFillRanges(currentHiddenScratch, zeroFillRanges,
                 zeroFillRangeCount, destinationCapacity, hiddenRowBytes,
                 blockIdx, blockNum, relayLocal) ||
             (hasWeight && !ClearDispatchZeroFillRanges(currentWeightScratch,
                 zeroFillRanges, zeroFillRangeCount, destinationCapacity,
                 weightRowBytes, blockIdx, blockNum, relayLocal)))) {
            dfxFlags |= TileXRMoonEp::kDispatchDfxInvalidConfig;
        }
        const int32_t localExecutionStatus = StatusFromDfxFlags(dfxFlags);
        DispatchPublishFirstStatus(planStatus, localExecutionStatus);
        const uint32_t ownerPayloadMode = static_cast<uint32_t>(hasWeight ?
            TileXRMoonEp::DispatchPayloadMode::RouteWeight :
            TileXRMoonEp::DispatchPayloadMode::Hidden);
#if defined(TILEXR_MOONEP_DISPATCH_ENABLE_DFX)
        uint64_t localSignalObserved = expectedFlag;
        if (!localOnly && dfxFlags != 0U) {
            localSignalObserved = LoadCompletionFlag(signalSource);
        }
        WriteDfxRecord(workspace + hiddenDfxOffset,
            static_cast<uint32_t>(TileXRMoonEp::DispatchPayloadMode::Hidden),
            static_cast<uint32_t>(rank), static_cast<uint32_t>(blockIdx), dfxFlags,
            firstInvalidRouteId, firstInvalidRawDst, firstQuietStatus, firstQuietPhase,
            timeoutPeer, timeoutPhase, routeCount, processedRouteCount, expectedFlag,
            expectedFlag, timeoutObservedFlag, localSignalObserved,
            completionFlagCount, outgoingCqStatuses,
            outgoingRemainingSqEntries, diagnosticLocal);
        if (hasWeight) {
            WriteDfxRecord(workspace + weightDfxOffset, ownerPayloadMode,
                static_cast<uint32_t>(rank), static_cast<uint32_t>(blockIdx),
                dfxFlags, firstInvalidRouteId, firstInvalidRawDst,
                firstQuietStatus, firstQuietPhase, timeoutPeer, timeoutPhase,
                routeCount, processedRouteCount, expectedFlag, expectedFlag,
                timeoutObservedFlag, localSignalObserved, completionFlagCount,
                outgoingCqStatuses, outgoingRemainingSqEntries,
                diagnosticLocal);
        }
#endif

        AscendC::SyncAll<true>();

#if defined(TILEXR_MOONEP_DISPATCH_ENABLE_DFX)
        uint32_t globalDfxFlags = 0U;
        auto allDfx = reinterpret_cast<__gm__ TileXRMoonEp::DispatchDfxRecord *>(
            workspace + (hasWeight ? weightDfxOffset : hiddenDfxOffset));
        for (uint32_t core = 0U; core < static_cast<uint32_t>(blockNum); ++core) {
            globalDfxFlags |= allDfx[core].flags;
        }
        const int32_t executionStatus = StatusFromDfxFlags(globalDfxFlags);
#else
        const int32_t executionStatus = LoadDispatchPlanStatus(planStatus, relayLocal);
#endif
        auto kernelStatus = reinterpret_cast<__gm__ TileXRMoonEp::DispatchKernelStatus *>(
            workspace + kernelStatusOffset);
        if (blockIdx == 0U) {
            kernelStatus->marker = TileXRMoonEp::kDispatchKernelStatusMarker;
            kernelStatus->version = TileXRMoonEp::kDispatchDiagnosticVersion;
            kernelStatus->recordBytes = sizeof(TileXRMoonEp::DispatchKernelStatus);
            kernelStatus->status = executionStatus;
            kernelStatus->payloadMode = ownerPayloadMode;
            kernelStatus->magic = expectedFlag;
            for (uint32_t index = 0U; index < 5U; ++index) {
                kernelStatus->reserved[index] = 0U;
            }
#if defined(TILEXR_MOONEP_DISPATCH_ENABLE_DFX)
            kernelStatus->reserved[0] |=
                TileXRMoonEp::kDispatchKernelStatusFeatureDfxEnabled;
#endif
            if (hasWeight) {
                kernelStatus->reserved[0] |=
                    TileXRMoonEp::kDispatchKernelStatusFeatureFusedEpoch;
            }

        }

        if (executionStatus == TileXRMoonEp::kDispatchStatusSuccess) {
            uint64_t outputStartSlot = 0U;
            uint64_t outputEndSlot = 0U;
            TileXRMoonEp::DispatchContiguousRange(destinationCapacity,
                static_cast<uint32_t>(blockNum),
                static_cast<uint32_t>(blockIdx),
                outputStartSlot, outputEndSlot);
            const uint64_t outputSlotCount = outputEndSlot - outputStartSlot;
            if (outputCopyTileBytes != 0U) {
                CopyContiguousBytesGmToGmPipelined(
                    hiddenOutput + outputStartSlot * hiddenRowBytes,
                    currentHiddenScratch + outputStartSlot * hiddenRowBytes,
                    outputSlotCount * hiddenRowBytes, outputCopyTileBytes,
                    outputCopyQueue);
            } else {
                for (uint64_t targetSlot = outputStartSlot;
                    targetSlot < outputEndSlot; ++targetSlot) {
                    CopyBytesGmToGm(
                        hiddenOutput + targetSlot * hiddenRowBytes,
                        currentHiddenScratch + targetSlot * hiddenRowBytes,
                        static_cast<uint32_t>(hiddenRowBytes), relayLocal);
                }
            }
            if (hasWeight) {
                for (uint64_t targetSlot = outputStartSlot;
                    targetSlot < outputEndSlot; ++targetSlot) {
                    CopyBytesGmToGm(
                        weightOutput + targetSlot * weightRowBytes,
                        currentWeightScratch + targetSlot * weightRowBytes,
                        static_cast<uint32_t>(weightRowBytes), relayLocal);
                }
            }
        }

        if (requiresFinalQuiet) {
            for (int64_t completionPhase = 0;
                completionPhase < groupCount; ++completionPhase) {
                for (uint32_t peerWork = 0U;
                    peerWork < peerWorkCount; ++peerWork) {
                    const int64_t peer = TileXRMoonEp::DispatchPeerForCore(
                        rank, rankSize, completionPhase,
                        static_cast<uint32_t>(blockIdx),
                        peerCoreCount, peerWork);
                    if (peer < 0 || peer == rank) {
                        continue;
                    }
                    for (uint32_t qpIdx = 0U;
                        qpIdx < TileXRMoonEp::kDispatchQpCount; ++qpIdx) {
                        const uint32_t quietStatus = TileXR::UDMAQuietStatusOnQp(
                            args, static_cast<int32_t>(peer),
                            physicalQp[qpIdx]);
                        if (quietStatus != 0U && firstQuietStatus == 0U) {
                            firstQuietStatus = quietStatus;
                            firstQuietPhase =
                                static_cast<uint32_t>(completionPhase);
                            dfxFlags |= TileXRMoonEp::kDispatchDfxQuietError;
                            DispatchPublishFirstStatus(planStatus,
                                TileXRMoonEp::kDispatchStatusQuietError);
                        }
                    }
                }
            }
        }
        AscendC::SyncAll<true>();
        if (blockIdx == 0U) {
            kernelStatus->status = LoadDispatchPlanStatus(planStatus, relayLocal);
        }
#if defined(TILEXR_MOONEP_DISPATCH_ENABLE_DFX)
        WriteDfxRecord(workspace + hiddenDfxOffset,
            static_cast<uint32_t>(TileXRMoonEp::DispatchPayloadMode::Hidden),
            static_cast<uint32_t>(rank), static_cast<uint32_t>(blockIdx), dfxFlags,
            firstInvalidRouteId, firstInvalidRawDst, firstQuietStatus, firstQuietPhase,
            timeoutPeer, timeoutPhase, routeCount, processedRouteCount, expectedFlag,
            expectedFlag, timeoutObservedFlag, localSignalObserved,
            completionFlagCount, outgoingCqStatuses,
            outgoingRemainingSqEntries, diagnosticLocal);
        if (hasWeight) {
            WriteDfxRecord(workspace + weightDfxOffset, ownerPayloadMode,
                static_cast<uint32_t>(rank), static_cast<uint32_t>(blockIdx),
                dfxFlags, firstInvalidRouteId, firstInvalidRawDst,
                firstQuietStatus, firstQuietPhase, timeoutPeer, timeoutPhase,
                routeCount, processedRouteCount, expectedFlag, expectedFlag,
                timeoutObservedFlag, localSignalObserved, completionFlagCount,
                outgoingCqStatuses, outgoingRemainingSqEntries,
                diagnosticLocal);
        }
#endif
    }
}
