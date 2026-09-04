#include "tilexr_ep_kernel_common.h"

namespace {

template <typename T>
__aicore__ inline void ZeroRows(GM_ADDR yOutGM, int64_t bs, int64_t h)
{
    auto yOut = reinterpret_cast<__gm__ T *>(yOutGM);
    const int64_t elements = bs * h;
    for (int64_t index = 0; index < elements; ++index) {
        yOut[index] = static_cast<T>(0.0f);
    }
}

template <typename T>
__aicore__ inline void AccumulateRow(GM_ADDR yOutGM, GM_ADDR srcGM, int64_t token, int64_t h)
{
    auto yOut = reinterpret_cast<__gm__ T *>(yOutGM);
    auto src = reinterpret_cast<__gm__ T *>(srcGM);
    const int64_t yOffset = token * h;
    for (int64_t elem = 0; elem < h; ++elem) {
        const T yValue = yOut[yOffset + elem];
        const T srcValue = src[elem];
        yOut[yOffset + elem] = static_cast<T>(static_cast<float>(yValue) + static_cast<float>(srcValue));
    }
}

__aicore__ inline void ScatterCombineRows(GM_ADDR localWindow, GM_ADDR expertOutGM, GM_ADDR assistInfoForCombineGM,
    GM_ADDR epRecvCountsGM, int32_t rankSize, int64_t maxRoutesPerSrc, int64_t rowBytes, int64_t slotBytes,
    int64_t payloadBytesPerSlot, AscendC::TBuf<AscendC::QuePosition::VECCALC> &tBuf)
{
    int64_t inRecord = 0;
    for (int32_t srcRank = 0; srcRank < rankSize; ++srcRank) {
        int64_t count = LoadInt32FromGm(epRecvCountsGM + srcRank * static_cast<int64_t>(sizeof(int32_t)), tBuf);
        if (count < 0 || count > maxRoutesPerSrc) {
            count = 0;
        }

        GM_ADDR payloadBase = localWindow + PayloadOffset(srcRank, slotBytes);
        GM_ADDR assistBase = localWindow + AssistOffset(srcRank, slotBytes, payloadBytesPerSlot);
        for (int64_t item = 0; item < count; ++item) {
            CopyBytesGmToGm(payloadBase + item * rowBytes, expertOutGM + inRecord * rowBytes, tBuf, rowBytes);
            const TileXREp::EpAssistTuple tuple = LoadAssistTupleFromGm(
                assistInfoForCombineGM + inRecord * static_cast<int64_t>(sizeof(TileXREp::EpAssistTuple)), tBuf);
            WriteAssist(assistBase, item, srcRank, tuple.tokenId, tuple.topKId, tuple.expertId, tBuf);
            ++inRecord;
        }
        StoreSlotHeader(localWindow + SlotOffset(srcRank, slotBytes), static_cast<int32_t>(count), srcRank,
            count * rowBytes, count * static_cast<int64_t>(sizeof(TileXREp::EpAssistTuple)), 0, tBuf);
    }
}

template <typename T>
__aicore__ inline void DrainCombineWindowRows(GM_ADDR sourceWindow, GM_ADDR yOutGM, int32_t rank,
    int32_t slotRank, int64_t bs, int64_t h, int64_t topK, int64_t maxRoutesPerSrc, int64_t rowBytes,
    int64_t slotBytes, int64_t payloadBytesPerSlot, AscendC::TBuf<AscendC::QuePosition::VECCALC> &tBuf)
{
    const int64_t count = LoadInt32FromGm(sourceWindow + SlotOffset(slotRank, slotBytes), tBuf);
    if (count <= 0 || count > maxRoutesPerSrc) {
        return;
    }

    GM_ADDR payloadBase = sourceWindow + PayloadOffset(slotRank, slotBytes);
    GM_ADDR assistBase = sourceWindow + AssistOffset(slotRank, slotBytes, payloadBytesPerSlot);
    for (int64_t item = 0; item < count; ++item) {
        const TileXREp::EpAssistTuple tuple = LoadAssistTupleFromGm(
            assistBase + item * static_cast<int64_t>(sizeof(TileXREp::EpAssistTuple)), tBuf);
        if (tuple.srcRank != rank || tuple.tokenId < 0 || static_cast<int64_t>(tuple.tokenId) >= bs ||
            tuple.topKId < 0 || static_cast<int64_t>(tuple.topKId) >= topK) {
            continue;
        }
        AccumulateRow<T>(yOutGM, payloadBase + item * rowBytes, tuple.tokenId, h);
    }
}

template <typename T>
__aicore__ inline void DrainCombineRows(GM_ADDR *shareAddrs, GM_ADDR yOutGM, int32_t rank, int32_t rankSize,
    int64_t bs, int64_t h, int64_t topK, int64_t maxRoutesPerSrc, int64_t rowBytes, int64_t slotBytes,
    int64_t payloadBytesPerSlot, AscendC::TBuf<AscendC::QuePosition::VECCALC> &tBuf)
{
    ZeroRows<T>(yOutGM, bs, h);
    for (int32_t expertRank = 0; expertRank < rankSize; ++expertRank) {
        GM_ADDR sourceWindow = shareAddrs[expertRank] + TileXR::IPC_DATA_OFFSET;
        const int64_t count = LoadInt32FromGm(sourceWindow + SlotOffset(rank, slotBytes), tBuf);
        if (count <= 0 || count > maxRoutesPerSrc) {
            continue;
        }

        GM_ADDR payloadBase = sourceWindow + PayloadOffset(rank, slotBytes);
        GM_ADDR assistBase = sourceWindow + AssistOffset(rank, slotBytes, payloadBytesPerSlot);
        for (int64_t item = 0; item < count; ++item) {
            const TileXREp::EpAssistTuple tuple = LoadAssistTupleFromGm(
                assistBase + item * static_cast<int64_t>(sizeof(TileXREp::EpAssistTuple)), tBuf);
            if (tuple.srcRank != rank || tuple.tokenId < 0 || static_cast<int64_t>(tuple.tokenId) >= bs ||
                tuple.topKId < 0 || static_cast<int64_t>(tuple.topKId) >= topK) {
                continue;
            }
            AccumulateRow<T>(yOutGM, payloadBase + item * rowBytes, tuple.tokenId, h);
        }
    }
}

} // namespace

extern "C" __global__ __aicore__ void tilexr_ep_combine_kernel(GM_ADDR commArgsGM, GM_ADDR expertOutGM,
    GM_ADDR assistInfoForCombineGM, GM_ADDR epRecvCountsGM, GM_ADDR yOutGM, int64_t bs, int64_t h, int64_t topK,
    int64_t moeExpertNum, int64_t dtype, int64_t dtypeBytes, int64_t maxRoutesPerSrc, int64_t rowBytes,
    int64_t payloadBytesPerSlot, int64_t assistBytesPerSlot, int64_t slotBytes, int64_t totalBytes, int64_t magic)
{
    if constexpr (g_coreType == AscendC::AIV) {
        if (AscendC::GetBlockIdx() != 0) {
            return;
        }

        if (commArgsGM == nullptr || expertOutGM == nullptr || assistInfoForCombineGM == nullptr ||
            epRecvCountsGM == nullptr || yOutGM == nullptr) {
            return;
        }

        auto args = reinterpret_cast<__gm__ TileXR::CommArgs *>(commArgsGM);
        const int32_t rank = args->rank;
        const int32_t rankSize = args->rankSize;
        if (rankSize <= 0 || rankSize > TileXR::TILEXR_MAX_RANK_SIZE || rank < 0 || rank >= rankSize ||
            !IsValidShape(bs, h, topK, moeExpertNum, dtypeBytes, maxRoutesPerSrc, rowBytes, payloadBytesPerSlot,
                assistBytesPerSlot, slotBytes, totalBytes, rankSize) || dtype != kTileXrDataTypeFp16) {
            return;
        }

        GM_ADDR shareAddrs[TileXR::TILEXR_MAX_RANK_SIZE];
        AscendC::GlobalTensor<GM_ADDR> peerMems;
        peerMems.SetGlobalBuffer(&(args->peerMems[0]), TileXR::TILEXR_MAX_RANK_SIZE);
        for (int32_t peer = 0; peer < rankSize; ++peer) {
            shareAddrs[peer] = peerMems.GetValue(peer);
            if (shareAddrs[peer] == nullptr) {
                return;
            }
        }

        AscendC::TPipe pipe;
        AscendC::TBuf<AscendC::QuePosition::VECCALC> tBuf;
        pipe.InitBuffer(tBuf, kEpUbBytes);
        SyncCollectives sync;
        sync.Init(rank, rankSize, shareAddrs, tBuf);

        GM_ADDR localWindow = shareAddrs[rank] + TileXR::IPC_DATA_OFFSET;
        ClearLocalWindow(localWindow, rankSize, maxRoutesPerSrc, rowBytes, slotBytes, totalBytes, tBuf);
        sync.SetInnerFlag(static_cast<int32_t>(magic), TileXREp::kEpStepCombineWindowCleared);
        for (int32_t peer = 0; peer < rankSize; ++peer) {
            sync.WaitRankInnerFlag(static_cast<int32_t>(magic), TileXREp::kEpStepCombineWindowCleared, peer);
        }

        ScatterCombineRows(localWindow, expertOutGM, assistInfoForCombineGM, epRecvCountsGM, rankSize,
            maxRoutesPerSrc, rowBytes, slotBytes, payloadBytesPerSlot, tBuf);
        TileXREpFlushDispatchSlotHeaders(localWindow, rankSize, slotBytes, tBuf);
        AscendC::PipeBarrier<PIPE_ALL>();

        sync.SetInnerFlag(static_cast<int32_t>(magic), TileXREp::kEpStepCombineReady);
        for (int32_t peer = 0; peer < rankSize; ++peer) {
            sync.WaitRankInnerFlag(static_cast<int32_t>(magic), TileXREp::kEpStepCombineReady, peer);
        }

        DrainCombineRows<float16_t>(shareAddrs, yOutGM, rank, rankSize, bs, h, topK, maxRoutesPerSrc, rowBytes,
            slotBytes, payloadBytesPerSlot, tBuf);
    }
}

extern "C" __global__ __aicore__ void tilexr_ep_combine_cross_node_kernel(GM_ADDR commArgsGM, GM_ADDR expertOutGM,
    GM_ADDR assistInfoForCombineGM, GM_ADDR epRecvCountsGM, GM_ADDR yOutGM, GM_ADDR workspaceGM, int64_t bs,
    int64_t h, int64_t topK, int64_t moeExpertNum, int64_t dtype, int64_t dtypeBytes, int64_t maxRoutesPerSrc,
    int64_t rowBytes, int64_t payloadBytesPerSlot, int64_t assistBytesPerSlot, int64_t slotBytes,
    int64_t totalBytes, int64_t magic)
{
    if constexpr (g_coreType == AscendC::AIV) {
        if (AscendC::GetBlockIdx() != 0) {
            return;
        }

        if (commArgsGM == nullptr || expertOutGM == nullptr || assistInfoForCombineGM == nullptr ||
            epRecvCountsGM == nullptr || yOutGM == nullptr || workspaceGM == nullptr) {
            return;
        }

        auto args = reinterpret_cast<__gm__ TileXR::CommArgs *>(commArgsGM);
        const int32_t rank = args->rank;
        const int32_t rankSize = args->rankSize;
        const int32_t localRankSize = args->localRankSize;
        if (rankSize <= 0 || rankSize > TileXR::TILEXR_MAX_RANK_SIZE || rank < 0 || rank >= rankSize ||
            localRankSize <= 0 || localRankSize > rankSize || !TileXR::UDMARegistryEnabled(args) ||
            !IsValidShape(bs, h, topK, moeExpertNum, dtypeBytes, maxRoutesPerSrc, rowBytes, payloadBytesPerSlot,
                assistBytesPerSlot, slotBytes, totalBytes, rankSize) || dtype != kTileXrDataTypeFp16) {
            return;
        }

        GM_ADDR shareAddrs[TileXR::TILEXR_MAX_RANK_SIZE];
        AscendC::GlobalTensor<GM_ADDR> peerMems;
        peerMems.SetGlobalBuffer(&(args->peerMems[0]), TileXR::TILEXR_MAX_RANK_SIZE);
        for (int32_t peer = 0; peer < rankSize; ++peer) {
            shareAddrs[peer] = peerMems.GetValue(peer);
            if (shareAddrs[peer] == nullptr) {
                return;
            }
        }

        AscendC::TPipe pipe;
        AscendC::TBuf<AscendC::QuePosition::VECCALC> tBuf;
        AscendC::TBuf<AscendC::QuePosition::VECCALC> udmaWqeBuf;
        pipe.InitBuffer(tBuf, kEpUbBytes);
        pipe.InitBuffer(udmaWqeBuf, TileXR::TILEXR_UDMA_WQE_SCRATCH_BYTES);
        SyncCollectives sync;
        sync.Init(rank, rankSize, shareAddrs, tBuf);

        GM_ADDR localIpcWindow = shareAddrs[rank] + TileXR::IPC_DATA_OFFSET;
        const int64_t combineWindowOffset = UDMASecondOperationOffset(totalBytes, rankSize, slotBytes);
        GM_ADDR sendWindow = workspaceGM + combineWindowOffset;
        GM_ADDR recvWindow = sendWindow + UDMARecvWindowOffset(totalBytes);
        TileXREpStoreStatusValue(workspaceGM, totalBytes, rankSize, slotBytes, TileXREp::kEpStatusOk);
        TileXREpInvalidateLocalCacheLines(localIpcWindow, totalBytes);
        ClearLocalWindow(localIpcWindow, rankSize, maxRoutesPerSrc, rowBytes, slotBytes, totalBytes, tBuf);
        ClearLocalWindow(sendWindow, rankSize, maxRoutesPerSrc, rowBytes, slotBytes, totalBytes, tBuf);
        ClearLocalWindow(recvWindow, rankSize, maxRoutesPerSrc, rowBytes, slotBytes, totalBytes, tBuf);

        const int32_t localNodeStart = TileXREpNodeStart(rank, localRankSize);
        const int32_t localNodeEnd = TileXREpNodeEnd(rank, localRankSize, rankSize);
        sync.SetInnerFlag(static_cast<int32_t>(magic), TileXREp::kEpStepCombineWindowCleared);
        for (int32_t peer = localNodeStart; peer < localNodeEnd; ++peer) {
            sync.WaitRankInnerFlag(static_cast<int32_t>(magic), TileXREp::kEpStepCombineWindowCleared, peer);
        }

        ScatterCombineRows(sendWindow, expertOutGM, assistInfoForCombineGM, epRecvCountsGM, rankSize,
            maxRoutesPerSrc, rowBytes, slotBytes, payloadBytesPerSlot, tBuf);
        for (int32_t dstRank = 0; dstRank < rankSize; ++dstRank) {
            if (TileXREpSameNode(dstRank, rank, localRankSize)) {
                CopyBytesGmToGm(shareAddrs[dstRank] + TileXR::IPC_DATA_OFFSET + SlotOffset(rank, slotBytes),
                    sendWindow + SlotOffset(dstRank, slotBytes), tBuf, slotBytes);
            }
        }
        TileXREpFlushDispatchSlotHeaders(sendWindow, rankSize, slotBytes, tBuf);
        TileXREpFlushUdmaSourceWindow(sendWindow, totalBytes);
        AscendC::PipeBarrier<PIPE_ALL>();

        sync.SetInnerFlag(static_cast<int32_t>(magic), TileXREp::kEpStepCombineReady);
        for (int32_t peer = localNodeStart; peer < localNodeEnd; ++peer) {
            sync.WaitRankInnerFlag(static_cast<int32_t>(magic), TileXREp::kEpStepCombineReady, peer);
        }

        TileXREpNotifyRemoteUdmaReadySeparate(args, udmaWqeBuf.Get<uint8_t>(), sendWindow,
            rank, rankSize, localRankSize, totalBytes, magic, slotBytes,
            TileXREp::kEpStepCombineReady, combineWindowOffset + UDMARecvWindowOffset(totalBytes));
        sync.SetInnerFlag(static_cast<int32_t>(magic), TileXREp::kEpStepCombineGatewayReady);
        for (int32_t peer = localNodeStart; peer < localNodeEnd; ++peer) {
            sync.WaitRankInnerFlag(static_cast<int32_t>(magic), TileXREp::kEpStepCombineGatewayReady, peer);
        }
        const bool remoteReady = TileXREpWaitRemoteUdmaReady(sendWindow, rank, rankSize, localRankSize, totalBytes,
            magic, TileXREp::kEpStepCombineReady, tBuf);
        if (!remoteReady) {
            TileXREpStoreStatusValue(workspaceGM, totalBytes, rankSize, slotBytes,
                TileXREp::kEpStatusRemoteReadyTimeout);
        }
        sync.SetInnerFlag(static_cast<int32_t>(magic), TileXREp::kEpStepCombineRelayReady);
        for (int32_t peer = localNodeStart; peer < localNodeEnd; ++peer) {
            sync.WaitRankInnerFlag(static_cast<int32_t>(magic), TileXREp::kEpStepCombineRelayReady, peer);
        }
    }
}

extern "C" __global__ __aicore__ void tilexr_ep_combine_cross_node_drain_kernel(GM_ADDR commArgsGM, GM_ADDR yOutGM,
    GM_ADDR workspaceGM, int64_t bs, int64_t h, int64_t topK, int64_t moeExpertNum, int64_t dtype,
    int64_t dtypeBytes, int64_t maxRoutesPerSrc, int64_t rowBytes, int64_t payloadBytesPerSlot,
    int64_t assistBytesPerSlot, int64_t slotBytes, int64_t totalBytes)
{
    if constexpr (g_coreType == AscendC::AIV) {
        if (AscendC::GetBlockIdx() != 0) {
            return;
        }
        if (commArgsGM == nullptr || yOutGM == nullptr || workspaceGM == nullptr) {
            return;
        }

        auto args = reinterpret_cast<__gm__ TileXR::CommArgs *>(commArgsGM);
        const int32_t rank = args->rank;
        const int32_t rankSize = args->rankSize;
        const int32_t localRankSize = args->localRankSize;
        if (rankSize <= 0 || rankSize > TileXR::TILEXR_MAX_RANK_SIZE || rank < 0 || rank >= rankSize ||
            localRankSize <= 0 || localRankSize > rankSize ||
            !IsValidShape(bs, h, topK, moeExpertNum, dtypeBytes, maxRoutesPerSrc, rowBytes, payloadBytesPerSlot,
                assistBytesPerSlot, slotBytes, totalBytes, rankSize) || dtype != kTileXrDataTypeFp16) {
            return;
        }

        AscendC::TPipe pipe;
        AscendC::TBuf<AscendC::QuePosition::VECCALC> tBuf;
        pipe.InitBuffer(tBuf, kEpUbBytes);
        GM_ADDR localIpcWindow = args->peerMems[rank] + TileXR::IPC_DATA_OFFSET;
        const int64_t combineWindowOffset = UDMASecondOperationOffset(totalBytes, rankSize, slotBytes);
        GM_ADDR sendWindow = workspaceGM + combineWindowOffset;
        GM_ADDR recvWindow = sendWindow + UDMARecvWindowOffset(totalBytes);
        GM_ADDR statusAddr = workspaceGM + UDMAStatusOffset(totalBytes, rankSize, slotBytes);
        TileXREpInvalidateLocalCacheLines(statusAddr, static_cast<int64_t>(sizeof(uint64_t)));
        if (LoadUint64FromGm(statusAddr, tBuf) != TileXREp::kEpStatusOk) {
            AscendC::printf("tilexr_ep_combine drain skipped because status is non-zero\n");
            return;
        }
        ZeroRows<float16_t>(yOutGM, bs, h);
        for (int32_t expertRank = 0; expertRank < rankSize; ++expertRank) {
            GM_ADDR sourceWindow = recvWindow;
            if (TileXREpSameNode(expertRank, rank, localRankSize)) {
                sourceWindow = localIpcWindow;
            } else {
                GM_ADDR slotBase = recvWindow + SlotOffset(expertRank, slotBytes);
                TileXREpInvalidateLocalCacheLines(slotBase, slotBytes);
                (void)LoadInt32FromGm(slotBase, tBuf);
            }
            DrainCombineWindowRows<float16_t>(sourceWindow, yOutGM, rank, expertRank, bs, h, topK,
                maxRoutesPerSrc, rowBytes, slotBytes, payloadBytesPerSlot, tBuf);
        }
    }
}
