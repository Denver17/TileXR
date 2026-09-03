/*
 * Copyright (c) 2024-2026 TileXR Project
 * Licensed under the Apache License, Version 2.0
 */

#include <cstdint>

#include "alltoall_a5_common.h"
#include "kernel_operator.h"
#include "tilexr_profiling.h"
#include "tilexr_sdma.h"
#include "tilexr_udma.h"

namespace {

constexpr uint32_t kIpcTransferBytes = 64U;
constexpr uint32_t kIpcBufferBytes = 512U;
using TileXRGrouped::kStatusCreditTimeout;
using TileXRGrouped::kStatusInvalid;
using TileXRGrouped::kStatusPostFailed;
using TileXRGrouped::kStatusQuietFailed;
using TileXRGrouped::kStatusReadyTimeout;
using TileXRGrouped::kStatusSdmaFailed;
using TileXRGrouped::kStatusSdmaPrewarmFailed;
using TileXRGrouped::kStatusSuccess;

constexpr uint32_t kEventKernelBegin = 1U;
constexpr uint32_t kEventKernelEnd = 2U;
constexpr uint32_t kEventSendBegin = 10U;
constexpr uint32_t kEventSendEnd = 11U;
constexpr uint32_t kEventSelfCopyBegin = 20U;
constexpr uint32_t kEventSelfCopyEnd = 21U;
constexpr uint32_t kEventSdmaPrewarmBegin = 22U;
constexpr uint32_t kEventSdmaPrewarmEnd = 23U;
constexpr uint32_t kEventReceiveBegin = 30U;
constexpr uint32_t kEventReceiveEnd = 31U;
constexpr uint32_t kEventPeerStride = 2U;
constexpr uint32_t kEventCreditWaitBase = 1000U;
constexpr uint32_t kEventSendPutSignalBase = 2000U;
constexpr uint32_t kEventSendQuietBase = 3000U;
constexpr uint32_t kEventReceiveWaitBase = 4000U;
constexpr uint32_t kEventReceiveCopyBase = 5000U;
constexpr uint32_t kEventSdmaSubmitBase = 6000U;
constexpr uint32_t kEventSdmaPrepareBase = 7000U;
constexpr uint32_t kEventSdmaCacheCleanBase = 8000U;
constexpr uint32_t kEventSdmaDsbBase = 9000U;
constexpr uint32_t kEventSdmaDoorbellBase = 10000U;
constexpr uint32_t kEventSdmaWaitBase = 11000U;

__aicore__ inline uint64_t ProfilingCycle()
{
#if defined(TILEXR_ENABLE_PROFILING)
    return static_cast<uint64_t>(AscendC::GetSystemCycle());
#else
    return 0U;
#endif
}

__aicore__ inline void RecordInterval(TileXR::DirectProfilingWriter *writer,
    uint32_t beginEvent, uint32_t endEvent, uint64_t begin, uint64_t end)
{
#if defined(TILEXR_ENABLE_PROFILING)
    if (writer != nullptr) {
        writer->RecordAt(beginEvent, begin);
        writer->RecordAt(endEvent, end);
    }
#else
    (void)writer;
    (void)beginEvent;
    (void)endEvent;
    (void)begin;
    (void)end;
#endif
}

__aicore__ inline void RecordPeerInterval(TileXR::DirectProfilingWriter *writer,
    uint32_t eventBase, uint32_t peer, uint64_t begin, uint64_t end)
{
#if defined(TILEXR_ENABLE_PROFILING)
    RecordInterval(writer,
        eventBase + peer * kEventPeerStride,
        eventBase + peer * kEventPeerStride + 1U,
        begin, end);
#else
    (void)writer;
    (void)eventBase;
    (void)peer;
    (void)begin;
    (void)end;
#endif
}

__aicore__ inline void FinishSendProfiling(TileXR::DirectProfilingWriter *writer,
    uint64_t kernelBegin, uint64_t sendBegin)
{
#if defined(TILEXR_ENABLE_PROFILING)
    const uint64_t end = ProfilingCycle();
    RecordInterval(writer, kEventSendBegin, kEventSendEnd, sendBegin, end);
    RecordInterval(writer, kEventKernelBegin, kEventKernelEnd, kernelBegin, end);
    writer->Publish();
#else
    (void)writer;
    (void)kernelBegin;
    (void)sendBegin;
#endif
}

__aicore__ inline void FinishReceiveProfiling(
    TileXR::DirectProfilingWriter *writers, uint64_t kernelBegin,
    uint64_t receiveBegin)
{
#if defined(TILEXR_ENABLE_PROFILING)
    const uint64_t end = ProfilingCycle();
    for (uint32_t lane = 0U; lane < TileXRGrouped::kGroupWidth; ++lane) {
        RecordInterval(&writers[lane], kEventReceiveBegin, kEventReceiveEnd,
            receiveBegin, end);
        RecordInterval(&writers[lane], kEventKernelBegin, kEventKernelEnd,
            kernelBegin, end);
        writers[lane].Publish();
    }
#else
    (void)writers;
    (void)kernelBegin;
    (void)receiveBegin;
#endif
}

struct KernelContext {
    const __gm__ TileXR::CommArgs *comm;
    __gm__ TileXR::UDMAInfo *profileInfo;
    const __gm__ TileXR::TileXRUDMAProfileRegistry *registry;
    __gm__ uint8_t *input;
    __gm__ uint8_t *output;
    __gm__ uint8_t *workspace;
    __gm__ uint64_t *status;
    uint64_t bytesPerPeer;
    uint64_t waitIterations;
    int64_t magic;
    int64_t previousMagic;
    uint32_t rank;
    uint32_t rankSize;
    uint32_t groupCount;
    uint32_t primaryQp;
    uint32_t secondaryQp;
    uint32_t primaryWeight;
    uint32_t secondaryWeight;
    uint64_t payloadOffset[TileXRGrouped::kPlaneCount];
    uint64_t signalOffset[TileXRGrouped::kPlaneCount];
    uint64_t signalSourceOffset;
    bool firstInvocation;
};

__aicore__ inline void RecordStatus(__gm__ uint64_t *status,
    uint32_t core, uint32_t code, uint32_t group, int32_t peer, uint64_t detail)
{
    if (status == nullptr || code == kStatusSuccess) {
        return;
    }
    status[0] = code;
    status[1] = core;
    status[2] = group;
    status[3] = static_cast<uint32_t>(peer);
    status[4] = detail;
}

__aicore__ inline uint64_t LoadIpcToken(__gm__ uint64_t *address,
    AscendC::LocalTensor<uint8_t> local)
{
    AscendC::GlobalTensor<uint8_t> global;
    global.SetGlobalBuffer(reinterpret_cast<__gm__ uint8_t *>(address), kIpcTransferBytes);
    const AscendC::DataCopyExtParams copy {1U, kIpcTransferBytes, 0U, 0U, 0U};
    const AscendC::DataCopyPadExtParams<uint8_t> pad {false, 0U, 0U, 0U};
    AscendC::DataCopyPad(local, global, copy, pad);
    AscendC::SetFlag<AscendC::HardEvent::MTE2_S>(EVENT_ID0);
    AscendC::WaitFlag<AscendC::HardEvent::MTE2_S>(EVENT_ID0);
    return local.ReinterpretCast<uint64_t>().GetValue(0U);
}

__aicore__ inline uint64_t LoadReadyToken(__gm__ uint64_t *address,
    AscendC::LocalTensor<uint8_t> local)
{
    AscendC::GlobalTensor<uint8_t> global;
    global.SetGlobalBuffer(reinterpret_cast<__gm__ uint8_t *>(address), sizeof(uint64_t));
    const AscendC::DataCopyExtParams copy {
        1U, static_cast<uint32_t>(sizeof(uint64_t)), 0U, 0U, 0U};
    const AscendC::DataCopyPadExtParams<uint8_t> pad {false, 0U, 0U, 0U};
    AscendC::DataCopyPad(local, global, copy, pad);
    AscendC::SetFlag<AscendC::HardEvent::MTE2_S>(EVENT_ID0);
    AscendC::WaitFlag<AscendC::HardEvent::MTE2_S>(EVENT_ID0);
    return local.ReinterpretCast<uint64_t>().GetValue(0U);
}

__aicore__ inline bool WaitIpcToken(__gm__ uint64_t *address,
    uint64_t expected, uint64_t waitIterations,
    AscendC::LocalTensor<uint8_t> local, uint64_t &observed)
{
    observed = 0U;
    for (uint64_t iteration = 0U; iteration < waitIterations; ++iteration) {
        observed = LoadIpcToken(address, local);
        if (observed >= expected) {
            return true;
        }
    }
    return false;
}

__aicore__ inline void PublishIpcToken(__gm__ uint64_t *address,
    uint64_t token, AscendC::LocalTensor<uint8_t> local)
{
    auto words = local.ReinterpretCast<uint64_t>();
    words.SetValue(0U, token);
    AscendC::GlobalTensor<uint8_t> global;
    global.SetGlobalBuffer(reinterpret_cast<__gm__ uint8_t *>(address), kIpcTransferBytes);
    const AscendC::DataCopyExtParams copy {1U, kIpcTransferBytes, 0U, 0U, 0U};
    AscendC::SetFlag<AscendC::HardEvent::S_MTE3>(EVENT_ID0);
    AscendC::WaitFlag<AscendC::HardEvent::S_MTE3>(EVENT_ID0);
    AscendC::DataCopyPad(global, local, copy);
    AscendC::SetFlag<AscendC::HardEvent::MTE3_S>(EVENT_ID0);
    AscendC::WaitFlag<AscendC::HardEvent::MTE3_S>(EVENT_ID0);
}

__aicore__ inline __gm__ uint64_t *LocalIpc(const KernelContext &ctx,
    uint32_t peer, uint32_t fieldOffset)
{
    return reinterpret_cast<__gm__ uint64_t *>(ctx.comm->creditMems[ctx.rank] +
        TileXRGrouped::IpcEntryOffset(peer, fieldOffset));
}

__aicore__ inline __gm__ uint64_t *RemoteIpc(const KernelContext &ctx,
    uint32_t peer, uint32_t fieldOffset)
{
    return reinterpret_cast<__gm__ uint64_t *>(ctx.comm->creditMems[peer] +
        TileXRGrouped::IpcEntryOffset(ctx.rank, fieldOffset));
}

__aicore__ inline uint32_t ActivePlane(const KernelContext &ctx)
{
    return static_cast<uint32_t>(static_cast<uint64_t>(ctx.magic) & 1U);
}

__aicore__ inline __gm__ uint64_t *LocalReadySignal(
    const KernelContext &ctx, uint32_t peer, uint32_t route)
{
    return reinterpret_cast<__gm__ uint64_t *>(ctx.workspace +
        ctx.signalOffset[ActivePlane(ctx)] +
        TileXRGrouped::SignalByteOffset(peer, route));
}

__aicore__ inline bool WaitSendCredit(const KernelContext &ctx,
    uint32_t group, uint32_t peer, AscendC::LocalTensor<uint8_t> ipcLocal,
    uint64_t &observed)
{
    if (group == 0U && ctx.firstInvocation) {
        observed = 0U;
        return true;
    }
    const uint64_t expected = group == 0U ?
        TileXRGrouped::CreditToken(ctx.previousMagic, ctx.groupCount) :
        TileXRGrouped::CreditToken(ctx.magic, group);
    return WaitIpcToken(LocalIpc(ctx, peer, TileXRGrouped::kCreditOffsetBytes),
        expected, ctx.waitIterations, ipcLocal, observed);
}

__aicore__ inline bool SendRoute(const KernelContext &ctx, uint32_t core,
    uint32_t lane, uint32_t route, uint32_t group, uint32_t peer,
    AscendC::LocalTensor<uint8_t> wqeLocal,
    AscendC::LocalTensor<uint8_t> ipcLocal,
    TileXR::DirectProfilingWriter *profiler)
{
    const uint64_t creditBegin = ProfilingCycle();
    uint64_t observed = 0U;
    const bool creditReady = WaitSendCredit(ctx, group, peer, ipcLocal, observed);
    RecordPeerInterval(profiler, kEventCreditWaitBase, peer,
        creditBegin, ProfilingCycle());
    if (!creditReady) {
        RecordStatus(ctx.status, core, kStatusCreditTimeout, group,
            static_cast<int32_t>(peer), observed);
        return false;
    }

    const bool crossNode = ctx.rank / TileXRGrouped::kRanksPerNode !=
        peer / TileXRGrouped::kRanksPerNode;
    const uint64_t routeWeight = static_cast<uint64_t>(ctx.primaryWeight) +
        ctx.secondaryWeight;
    const uint64_t primaryBytes = crossNode ?
        ctx.bytesPerPeer * ctx.primaryWeight / routeWeight : ctx.bytesPerPeer;
    const uint64_t routeOffset = route == 0U ? 0U : primaryBytes;
    const uint64_t routeBytes = route == 0U ?
        primaryBytes : ctx.bytesPerPeer - primaryBytes;
    const uint64_t putBegin = ProfilingCycle();
    if (routeBytes == 0U) {
        RecordPeerInterval(profiler, kEventSendPutSignalBase, peer,
            putBegin, ProfilingCycle());
        return true;
    }
    const uint32_t directQp = TileXRGrouped::DirectQp(
        ctx.rank, peer, ctx.registry->qpCount);
    const uint32_t qp = !crossNode && route == 0U &&
        directQp < ctx.registry->qpCount ? directQp :
        (route == 0U ? ctx.primaryQp : ctx.secondaryQp) + lane;
    const uint32_t plane = ActivePlane(ctx);
    const __gm__ uint8_t *localPayload = ctx.input +
        static_cast<uint64_t>(peer) * ctx.bytesPerPeer + routeOffset;
    const uint64_t remotePayloadOffset = ctx.payloadOffset[plane] +
        static_cast<uint64_t>(ctx.rank) * ctx.bytesPerPeer + routeOffset;
    const uint32_t post = TileXR::UDMAProfilePutRawLocalNbiOnQp(
        ctx.comm, ctx.profileInfo, ctx.registry, wqeLocal,
        static_cast<int32_t>(peer), qp, localPayload,
        TileXRGrouped::kWorkspaceRegion, TileXRGrouped::kWorkspaceRegion,
        remotePayloadOffset, routeBytes, TileXR::TILEXR_UDMA_SQE_FLAG_COMPLETION);
    if (post != TileXR::TILEXR_UDMA_STATUS_SUCCESS) {
        RecordPeerInterval(profiler, kEventSendPutSignalBase, peer,
            putBegin, ProfilingCycle());
        RecordStatus(ctx.status, core, kStatusPostFailed, group,
            static_cast<int32_t>(peer), post);
        return false;
    }
    const uint64_t token = TileXRGrouped::ReadyToken(ctx.magic, group, route);
    const uint64_t localSignalOffset = ctx.signalSourceOffset +
        TileXRGrouped::SignalSourceByteOffset(core);
    __gm__ uint64_t *signalSource = reinterpret_cast<__gm__ uint64_t *>(
        ctx.workspace + localSignalOffset);
    *signalSource = token;
    TileXR::UDMACleanCacheLines(
        reinterpret_cast<__gm__ uint8_t *>(signalSource), sizeof(uint64_t));
    const uint64_t remoteSignalOffset = ctx.signalOffset[plane] +
        TileXRGrouped::SignalByteOffset(ctx.rank, route);
    const uint32_t signalPost = TileXR::UDMAProfilePutNbiOnQpWithFlag(
        ctx.comm, ctx.profileInfo, ctx.registry, wqeLocal,
        static_cast<int32_t>(peer), qp,
        TileXRGrouped::kWorkspaceRegion, localSignalOffset,
        TileXRGrouped::kWorkspaceRegion, remoteSignalOffset, sizeof(uint64_t),
        TileXR::TILEXR_UDMA_SQE_FLAG_ORDERED_COMPLETION);
    if (signalPost != TileXR::TILEXR_UDMA_STATUS_SUCCESS) {
        RecordPeerInterval(profiler, kEventSendPutSignalBase, peer,
            putBegin, ProfilingCycle());
        RecordStatus(ctx.status, core, kStatusPostFailed, group,
            static_cast<int32_t>(peer), signalPost);
        return false;
    }
    RecordPeerInterval(profiler, kEventSendPutSignalBase, peer,
        putBegin, ProfilingCycle());
    const uint64_t quietBegin = ProfilingCycle();
    const uint32_t quiet = TileXR::UDMAProfileQuietStatusOnQp(
        ctx.comm, ctx.profileInfo, ctx.registry,
        static_cast<int32_t>(peer), qp);
    if (quiet != TileXR::TILEXR_UDMA_STATUS_SUCCESS) {
        RecordPeerInterval(profiler, kEventSendQuietBase, peer,
            quietBegin, ProfilingCycle());
        RecordStatus(ctx.status, core, kStatusQuietFailed, group,
            static_cast<int32_t>(peer), quiet);
        return false;
    }
    RecordPeerInterval(profiler, kEventSendQuietBase, peer,
        quietBegin, ProfilingCycle());
    return true;
}

__aicore__ inline bool Ready(const KernelContext &ctx, uint32_t group,
    uint32_t peer, AscendC::LocalTensor<uint8_t> ipcLocal)
{
    const uint64_t primary = LoadReadyToken(LocalReadySignal(ctx, peer, 0U), ipcLocal);
    if (primary < TileXRGrouped::ReadyToken(ctx.magic, group, 0U)) {
        return false;
    }
    const bool crossNode = ctx.rank / TileXRGrouped::kRanksPerNode !=
        peer / TileXRGrouped::kRanksPerNode;
    if (!crossNode) {
        return true;
    }
    const uint64_t secondary = LoadReadyToken(LocalReadySignal(ctx, peer, 1U), ipcLocal);
    return secondary >= TileXRGrouped::ReadyToken(ctx.magic, group, 1U);
}

__aicore__ inline bool CopyPeer(const KernelContext &ctx, uint32_t peer,
    TileXR::DirectProfilingWriter *profiler)
{
    __gm__ uint8_t *src = ctx.workspace + ctx.payloadOffset[ActivePlane(ctx)] +
        static_cast<uint64_t>(peer) * ctx.bytesPerPeer;
    __gm__ uint8_t *dst = ctx.output + static_cast<uint64_t>(peer) * ctx.bytesPerPeer;
    const uint32_t channel = peer % 16U;
    const uint64_t copyBegin = ProfilingCycle();
    const uint64_t submitBegin = ProfilingCycle();
#if defined(TILEXR_ENABLE_PROFILING)
    TileXR::SDMASubmitTrace submitTrace {};
    const uint64_t event = TileXR::SDMACopyNbi(ctx.comm, dst, src,
        ctx.bytesPerPeer, channel, &submitTrace);
#else
    const uint64_t event = TileXR::SDMACopyNbi(ctx.comm, dst, src,
        ctx.bytesPerPeer, channel);
#endif
    const uint64_t submitEnd = ProfilingCycle();
    const uint64_t waitBegin = ProfilingCycle();
    const bool copied = event != 0U && TileXR::SDMAWait(ctx.comm, event, channel);
    const uint64_t waitEnd = ProfilingCycle();
    const uint64_t copyEnd = ProfilingCycle();
    RecordPeerInterval(profiler, kEventReceiveCopyBase, peer, copyBegin, copyEnd);
    RecordPeerInterval(profiler, kEventSdmaSubmitBase, peer, submitBegin, submitEnd);
#if defined(TILEXR_ENABLE_PROFILING)
    RecordPeerInterval(profiler, kEventSdmaPrepareBase, peer,
        submitTrace.prepareBegin, submitTrace.prepareEnd);
    RecordPeerInterval(profiler, kEventSdmaCacheCleanBase, peer,
        submitTrace.cacheCleanBegin, submitTrace.cacheCleanEnd);
    RecordPeerInterval(profiler, kEventSdmaDsbBase, peer,
        submitTrace.dsbBegin, submitTrace.dsbEnd);
    RecordPeerInterval(profiler, kEventSdmaDoorbellBase, peer,
        submitTrace.doorbellBegin, submitTrace.doorbellEnd);
#endif
    RecordPeerInterval(profiler, kEventSdmaWaitBase, peer, waitBegin, waitEnd);
    return copied;
}

__aicore__ inline bool CopySelf(const KernelContext &ctx)
{
    const uint64_t selfEvent = TileXR::SDMACopyNbi(ctx.comm,
        ctx.output + static_cast<uint64_t>(ctx.rank) * ctx.bytesPerPeer,
        ctx.input + static_cast<uint64_t>(ctx.rank) * ctx.bytesPerPeer,
        ctx.bytesPerPeer, ctx.rank % 16U);
    if (selfEvent == 0U || !TileXR::SDMAWait(ctx.comm, selfEvent, ctx.rank % 16U)) {
        RecordStatus(ctx.status, TileXRGrouped::kSendCoreCount,
            kStatusSdmaFailed, 0U, static_cast<int32_t>(ctx.rank), selfEvent);
        return false;
    }
    return true;
}

__aicore__ inline void RunReceive(const KernelContext &ctx,
    AscendC::LocalTensor<uint8_t> ipcLocal,
    TileXR::DirectProfilingWriter *profilers)
{
    bool completed[TileXRGrouped::kMaxRankSize] = {};
#if defined(TILEXR_ENABLE_PROFILING)
    uint64_t waitBegins[TileXRGrouped::kMaxRankSize] = {};
#endif
    uint32_t remaining = ctx.rankSize - 1U;
    uint64_t idleScans = 0U;
    while (remaining != 0U && idleScans < ctx.waitIterations) {
        bool progressed = false;
        for (uint32_t group = 0U; group < ctx.groupCount; ++group) {
            for (uint32_t lane = 0U; lane < TileXRGrouped::kGroupWidth; ++lane) {
                const int32_t peerValue = TileXRGrouped::Peer(
                    ctx.rank, ctx.rankSize, group, lane);
                if (peerValue < 0) {
                    continue;
                }
                const uint32_t peer = static_cast<uint32_t>(peerValue);
                if (completed[peer]) {
                    continue;
                }
#if defined(TILEXR_ENABLE_PROFILING)
                if (waitBegins[peer] == 0U) {
                    waitBegins[peer] = ProfilingCycle();
                }
#endif
                if (!Ready(ctx, group, peer, ipcLocal)) {
                    continue;
                }
                TileXR::DirectProfilingWriter *profiler =
                    profilers == nullptr ? nullptr : &profilers[peer % 16U];
#if defined(TILEXR_ENABLE_PROFILING)
                RecordPeerInterval(profiler, kEventReceiveWaitBase, peer,
                    waitBegins[peer], ProfilingCycle());
#endif
                progressed = true;
                const int32_t nextPeer = TileXRGrouped::NextCreditPeer(
                    ctx.rank, ctx.rankSize, group, lane);
                if (nextPeer >= 0) {
                    const uint32_t tokenGroup = TileXRGrouped::NextCreditTokenGroup(
                        ctx.rank, ctx.rankSize, group, lane);
                    PublishIpcToken(RemoteIpc(ctx,
                        static_cast<uint32_t>(nextPeer), TileXRGrouped::kCreditOffsetBytes),
                        TileXRGrouped::CreditToken(ctx.magic, tokenGroup), ipcLocal);
                }
                if (!CopyPeer(ctx, peer, profiler)) {
                    RecordStatus(ctx.status, TileXRGrouped::kSendCoreCount,
                        kStatusSdmaFailed, group, peerValue, 0U);
                    return;
                }
                completed[peer] = true;
                --remaining;
            }
        }
        idleScans = progressed ? 0U : idleScans + 1U;
    }
    if (remaining != 0U) {
        RecordStatus(ctx.status, TileXRGrouped::kSendCoreCount,
            kStatusReadyTimeout, 0U, -1, remaining);
    }
}

} // namespace

#if defined(TILEXR_GROUPED_KERNEL_VARIANT_128)
#define TILEXR_GROUPED_KERNEL_ENTRY tilexr_alltoall_a5_kernel_128
#else
#define TILEXR_GROUPED_KERNEL_ENTRY tilexr_alltoall_a5_kernel
#endif

extern "C" __global__ __aicore__ void TILEXR_GROUPED_KERNEL_ENTRY(
    GM_ADDR commArgsGM, GM_ADDR profileInfoGM, GM_ADDR profileRegistryGM,
    GM_ADDR inputGM, GM_ADDR outputGM, GM_ADDR workspaceGM, GM_ADDR statusGM,
    uint64_t elementsPerPeer, uint64_t bytesPerPeer, uint64_t bytesPerRank,
    uint64_t waitIterations, int64_t magic, int64_t previousMagic,
    uint32_t rank, uint32_t rankSize, uint32_t groupCount,
    uint32_t primaryQp, uint32_t secondaryQp,
    uint32_t primaryWeight, uint32_t secondaryWeight,
    uint32_t firstInvocation, uint32_t profilingLaunchSlot,
    uint64_t payloadOffset0, uint64_t payloadOffset1,
    uint64_t signalOffset0, uint64_t signalOffset1,
    uint64_t signalSourceOffset)
{
    const uint32_t core = static_cast<uint32_t>(AscendC::GetBlockIdx());
    AscendC::TPipe pipe;
    AscendC::TBuf<AscendC::TPosition::VECCALC> wqeBuf;
    AscendC::TBuf<AscendC::TPosition::VECCALC> ipcBuf;
    pipe.InitBuffer(wqeBuf, TileXR::TILEXR_UDMA_WQE_SCRATCH_BYTES);
    pipe.InitBuffer(ipcBuf, kIpcBufferBytes);
    auto wqeLocal = wqeBuf.Get<uint8_t>();
    auto ipcLocal = ipcBuf.Get<uint8_t>();

    KernelContext ctx {
        reinterpret_cast<const __gm__ TileXR::CommArgs *>(commArgsGM),
        reinterpret_cast<__gm__ TileXR::UDMAInfo *>(profileInfoGM),
        reinterpret_cast<const __gm__ TileXR::TileXRUDMAProfileRegistry *>(profileRegistryGM),
        reinterpret_cast<__gm__ uint8_t *>(inputGM),
        reinterpret_cast<__gm__ uint8_t *>(outputGM),
        reinterpret_cast<__gm__ uint8_t *>(workspaceGM),
        reinterpret_cast<__gm__ uint64_t *>(statusGM),
        bytesPerPeer, waitIterations, magic, previousMagic,
        rank, rankSize, groupCount,
        primaryQp, secondaryQp, primaryWeight, secondaryWeight,
        {payloadOffset0, payloadOffset1},
        {signalOffset0, signalOffset1},
        signalSourceOffset, firstInvocation != 0U,
    };
    (void)elementsPerPeer;
    (void)bytesPerRank;
    (void)profilingLaunchSlot;
    if (ctx.comm == nullptr || ctx.profileInfo == nullptr || ctx.registry == nullptr ||
        ctx.input == nullptr || ctx.output == nullptr || ctx.workspace == nullptr ||
        ctx.status == nullptr || !TileXRGrouped::ValidRankSize(ctx.rankSize) ||
        ctx.rank >= ctx.rankSize || ctx.groupCount != TileXRGrouped::GroupCount(ctx.rankSize) ||
        ctx.bytesPerPeer == 0U || ctx.bytesPerPeer > UINT32_MAX ||
        ctx.primaryWeight == 0U || ctx.secondaryWeight == 0U ||
        static_cast<uint64_t>(ctx.primaryWeight) + ctx.secondaryWeight == 0U ||
        ctx.primaryQp + TileXRGrouped::kGroupWidth > ctx.registry->qpCount ||
        ctx.secondaryQp + TileXRGrouped::kGroupWidth > ctx.registry->qpCount) {
        RecordStatus(ctx.status, core, kStatusInvalid, 0U, -1, 0U);
        return;
    }
    if (core < TileXRGrouped::kSendCoreCount) {
#if defined(TILEXR_ENABLE_PROFILING)
        TileXR::DirectProfilingWriter profiler;
        profiler.Initialize(commArgsGM, core, profilingLaunchSlot,
            TileXRGrouped::kProfilingRecordsPerLaunch);
        const uint64_t kernelBegin = ProfilingCycle();
        const uint64_t sendBegin = kernelBegin;
#endif
        const uint32_t lane = core % TileXRGrouped::kGroupWidth;
        const uint32_t route = core / TileXRGrouped::kGroupWidth;
        for (uint32_t group = 0U; group < ctx.groupCount; ++group) {
            const int32_t peer = TileXRGrouped::Peer(
                ctx.rank, ctx.rankSize, group, lane);
            if (peer >= 0 && !SendRoute(ctx, core, lane, route, group,
                    static_cast<uint32_t>(peer), wqeLocal, ipcLocal,
#if defined(TILEXR_ENABLE_PROFILING)
                    &profiler
#else
                    nullptr
#endif
                    )) {
#if defined(TILEXR_ENABLE_PROFILING)
                FinishSendProfiling(&profiler, kernelBegin, sendBegin);
#endif
                return;
            }
        }
#if defined(TILEXR_ENABLE_PROFILING)
        FinishSendProfiling(&profiler, kernelBegin, sendBegin);
#endif
        return;
    }
    if (core == TileXRGrouped::kSendCoreCount) {
#if defined(TILEXR_ENABLE_PROFILING)
        TileXR::DirectProfilingWriter profilers[TileXRGrouped::kGroupWidth];
        for (uint32_t lane = 0U; lane < TileXRGrouped::kGroupWidth; ++lane) {
            profilers[lane].Initialize(
                commArgsGM, TileXRGrouped::kSendCoreCount + lane,
                profilingLaunchSlot,
                TileXRGrouped::kProfilingRecordsPerLaunch);
        }
        const uint64_t kernelBegin = ProfilingCycle();
        const uint64_t prewarmBegin = kernelBegin;
#endif
        const bool prewarmOk = !ctx.firstInvocation || TileXR::SDMAPrewarmSqPages(
            ctx.comm, 16U, ipcLocal);
#if defined(TILEXR_ENABLE_PROFILING)
        RecordInterval(&profilers[0], kEventSdmaPrewarmBegin,
            kEventSdmaPrewarmEnd, prewarmBegin, ProfilingCycle());
#endif
        if (!prewarmOk) {
            RecordStatus(ctx.status, core, kStatusSdmaPrewarmFailed,
                0U, static_cast<int32_t>(ctx.rank), 0U);
#if defined(TILEXR_ENABLE_PROFILING)
            FinishReceiveProfiling(profilers, kernelBegin, ProfilingCycle());
#endif
            return;
        }
#if defined(TILEXR_ENABLE_PROFILING)
        const uint64_t selfCopyBegin = ProfilingCycle();
#endif
        if (!CopySelf(ctx)) {
#if defined(TILEXR_ENABLE_PROFILING)
            RecordInterval(&profilers[0], kEventSelfCopyBegin,
                kEventSelfCopyEnd, selfCopyBegin, ProfilingCycle());
            FinishReceiveProfiling(profilers, kernelBegin, ProfilingCycle());
#endif
            return;
        }
#if defined(TILEXR_ENABLE_PROFILING)
        RecordInterval(&profilers[0], kEventSelfCopyBegin,
            kEventSelfCopyEnd, selfCopyBegin, ProfilingCycle());
        const uint64_t receiveBegin = ProfilingCycle();
#endif
        RunReceive(ctx, ipcLocal,
#if defined(TILEXR_ENABLE_PROFILING)
            profilers
#else
            nullptr
#endif
            );
#if defined(TILEXR_ENABLE_PROFILING)
        FinishReceiveProfiling(profilers, kernelBegin, receiveBegin);
#endif
    }
}

#undef TILEXR_GROUPED_KERNEL_ENTRY
