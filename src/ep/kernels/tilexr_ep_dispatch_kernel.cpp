#include "comm_args.h"
#include "ep_window.h"
#include "kernel_operator.h"
#include "tilexr_sync.h"
#include "tilexr_udma.h"

namespace {

constexpr uint32_t kEpUbBytes = 64*1024;
constexpr uint32_t kEpSyncUbBytes = 4*1024;
constexpr uint32_t kEpCopyTileBytes = kEpUbBytes - kEpSyncUbBytes;
constexpr uint32_t kEpScalarUbBytes = 64;
constexpr uint32_t kEpScalarUbOffset = kEpSyncUbBytes - kEpScalarUbBytes;
constexpr int64_t kTileXrDataTypeFp16 = 3;
constexpr int64_t kTileXrDataTypeBfp16 = 11;
constexpr int64_t kEpQuantModeNone = 0;
constexpr int64_t kEpQuantModeStatic = 1;
constexpr int64_t kEpQuantModePerTokenDynamic = 2;

__aicore__ inline int64_t AlignUp(int64_t value, int64_t alignment)
{
    if (alignment <= 0) {
        return value;
    }
    const int64_t remainder = value % alignment;
    return remainder == 0 ? value : value + alignment - remainder;
}

__aicore__ inline int64_t SlotOffset(int64_t srcRank, int64_t slotBytes)
{
    return TileXREp::kEpWindowHeaderBytes + srcRank * slotBytes;
}

__aicore__ inline int64_t PayloadOffset(int64_t srcRank, int64_t slotBytes)
{
    return SlotOffset(srcRank, slotBytes) + TileXREp::kEpSrcSlotHeaderBytes;
}

__aicore__ inline int64_t AssistOffset(int64_t srcRank, int64_t slotBytes, int64_t payloadBytesPerSlot)
{
    return PayloadOffset(srcRank, slotBytes) + payloadBytesPerSlot;
}

__aicore__ inline int64_t UDMAReadyOffset(int64_t totalBytes, int32_t rank)
{
    return AlignUp(totalBytes, TileXREp::kEpWindowAlignmentBytes) * 2 +
        static_cast<int64_t>(rank) * static_cast<int64_t>(sizeof(uint64_t));
}

__aicore__ inline int64_t UDMARecvWindowOffset(int64_t totalBytes)
{
    return AlignUp(totalBytes, TileXREp::kEpWindowAlignmentBytes);
}

__aicore__ inline int64_t TileXREpTpWindowOffset(int64_t totalBytes)
{
    return AlignUp(totalBytes, TileXREp::kEpWindowAlignmentBytes) * 3;
}

__aicore__ inline void CopyBytesGmToGm(
    GM_ADDR dstGM, GM_ADDR srcGM, AscendC::TBuf<AscendC::QuePosition::VECCALC> &tBuf, int64_t bytes)
{
    if (dstGM == nullptr || srcGM == nullptr || bytes <= 0) {
        return;
    }

    AscendC::LocalTensor<uint8_t> local =
        tBuf.GetWithOffset<uint8_t>(kEpCopyTileBytes, kEpSyncUbBytes);
    AscendC::GlobalTensor<uint8_t> src;
    AscendC::GlobalTensor<uint8_t> dst;
    src.SetGlobalBuffer(reinterpret_cast<__gm__ uint8_t *>(srcGM), bytes);
    dst.SetGlobalBuffer(reinterpret_cast<__gm__ uint8_t *>(dstGM), bytes);

    for (int64_t copied = 0; copied < bytes; copied += kEpCopyTileBytes) {
        int64_t tileBytes = bytes - copied;
        if (tileBytes > static_cast<int64_t>(kEpCopyTileBytes)) {
            tileBytes = kEpCopyTileBytes;
        }

        AscendC::DataCopyPadParams padParams {false, 0, 0, 0};
        AscendC::DataCopyParams copyParams {1, static_cast<uint16_t>(tileBytes), 0, 0};
        AscendC::DataCopyPad(local, src[copied], copyParams, padParams);
        AscendC::SetFlag<AscendC::HardEvent::MTE2_MTE3>(EVENT_ID0);
        AscendC::WaitFlag<AscendC::HardEvent::MTE2_MTE3>(EVENT_ID0);
        AscendC::DataCopyPad(dst[copied], local, copyParams);
        AscendC::SetFlag<AscendC::HardEvent::MTE3_MTE2>(EVENT_ID0);
        AscendC::WaitFlag<AscendC::HardEvent::MTE3_MTE2>(EVENT_ID0);
    }
    AscendC::PipeBarrier<PIPE_ALL>();
}

__aicore__ inline int32_t LoadInt32FromGm(GM_ADDR srcGM, AscendC::TBuf<AscendC::QuePosition::VECCALC> &tBuf)
{
    AscendC::LocalTensor<int32_t> local =
        tBuf.GetWithOffset<int32_t>(kEpScalarUbBytes / sizeof(int32_t), kEpScalarUbOffset);
    AscendC::GlobalTensor<int32_t> src;
    src.SetGlobalBuffer(reinterpret_cast<__gm__ int32_t *>(srcGM), 1);

    AscendC::DataCopyExtParams copyParams {1, static_cast<uint32_t>(sizeof(int32_t)), 0, 0, 0};
    AscendC::DataCopyPadExtParams<int32_t> padParams {false, 0, 0, 0};
    AscendC::DataCopyPad(local, src, copyParams, padParams);
    AscendC::SetFlag<AscendC::HardEvent::MTE2_S>(EVENT_ID0);
    AscendC::WaitFlag<AscendC::HardEvent::MTE2_S>(EVENT_ID0);
    return local.GetValue(0);
}

__aicore__ inline uint64_t LoadUint64FromGm(GM_ADDR srcGM, AscendC::TBuf<AscendC::QuePosition::VECCALC> &tBuf)
{
    AscendC::LocalTensor<uint64_t> local =
        tBuf.GetWithOffset<uint64_t>(kEpScalarUbBytes / sizeof(uint64_t), kEpScalarUbOffset);
    AscendC::GlobalTensor<uint64_t> src;
    src.SetGlobalBuffer(reinterpret_cast<__gm__ uint64_t *>(srcGM), 1);

    AscendC::DataCopyExtParams copyParams {1, static_cast<uint32_t>(sizeof(uint64_t)), 0, 0, 0};
    AscendC::DataCopyPadExtParams<uint64_t> padParams {false, 0, 0, 0};
    AscendC::DataCopyPad(local, src, copyParams, padParams);
    AscendC::SetFlag<AscendC::HardEvent::MTE2_S>(EVENT_ID0);
    AscendC::WaitFlag<AscendC::HardEvent::MTE2_S>(EVENT_ID0);
    return local.GetValue(0);
}

__aicore__ inline uint16_t LoadUint16FromGm(GM_ADDR srcGM, AscendC::TBuf<AscendC::QuePosition::VECCALC> &tBuf)
{
    AscendC::LocalTensor<uint16_t> local =
        tBuf.GetWithOffset<uint16_t>(kEpScalarUbBytes / sizeof(uint16_t), kEpScalarUbOffset);
    AscendC::GlobalTensor<uint16_t> src;
    src.SetGlobalBuffer(reinterpret_cast<__gm__ uint16_t *>(srcGM), 1);

    AscendC::DataCopyExtParams copyParams {1, static_cast<uint32_t>(sizeof(uint16_t)), 0, 0, 0};
    AscendC::DataCopyPadExtParams<uint16_t> padParams {false, 0, 0, 0};
    AscendC::DataCopyPad(local, src, copyParams, padParams);
    AscendC::SetFlag<AscendC::HardEvent::MTE2_S>(EVENT_ID0);
    AscendC::WaitFlag<AscendC::HardEvent::MTE2_S>(EVENT_ID0);
    return local.GetValue(0);
}

__aicore__ inline float LoadFloatFromGm(GM_ADDR srcGM, AscendC::TBuf<AscendC::QuePosition::VECCALC> &tBuf)
{
    AscendC::LocalTensor<float> local =
        tBuf.GetWithOffset<float>(kEpScalarUbBytes / sizeof(float), kEpScalarUbOffset);
    AscendC::GlobalTensor<float> src;
    src.SetGlobalBuffer(reinterpret_cast<__gm__ float *>(srcGM), 1);

    AscendC::DataCopyExtParams copyParams {1, static_cast<uint32_t>(sizeof(float)), 0, 0, 0};
    AscendC::DataCopyPadExtParams<float> padParams {false, 0, 0, 0};
    AscendC::DataCopyPad(local, src, copyParams, padParams);
    AscendC::SetFlag<AscendC::HardEvent::MTE2_S>(EVENT_ID0);
    AscendC::WaitFlag<AscendC::HardEvent::MTE2_S>(EVENT_ID0);
    return local.GetValue(0);
}

__aicore__ inline float TileXREpHalfBitsToFloat(uint16_t bits)
{
    const int32_t sign = (bits & 0x8000U) != 0 ? -1 : 1;
    const int32_t exponent = static_cast<int32_t>((bits >> 10) & 0x1fU);
    const int32_t mantissa = static_cast<int32_t>(bits & 0x03ffU);
    if (exponent == 0 && mantissa == 0) {
        return sign < 0 ? -0.0f : 0.0f;
    }
    if (exponent == 0) {
        float value = static_cast<float>(mantissa) / 1024.0f;
        for (int32_t shift = 0; shift < 14; ++shift) {
            value *= 0.5f;
        }
        return static_cast<float>(sign) * value;
    }
    float value = 1.0f + static_cast<float>(mantissa) / 1024.0f;
    const int32_t power = exponent - 15;
    if (power >= 0) {
        for (int32_t shift = 0; shift < power; ++shift) {
            value *= 2.0f;
        }
    } else {
        for (int32_t shift = 0; shift < -power; ++shift) {
            value *= 0.5f;
        }
    }
    return static_cast<float>(sign) * value;
}

__aicore__ inline int8_t TileXREpClampInt8(float value)
{
    const float rounded = value >= 0.0f ? value + 0.5f : value - 0.5f;
    int32_t asInt = static_cast<int32_t>(rounded);
    if (asInt > 127) {
        asInt = 127;
    } else if (asInt < -128) {
        asInt = -128;
    }
    return static_cast<int8_t>(asInt);
}

__aicore__ inline float TileXREpAbsFloat(float value)
{
    return value < 0.0f ? -value : value;
}

__aicore__ inline void StoreFloatToGm(
    GM_ADDR dstGM, float value, AscendC::TBuf<AscendC::QuePosition::VECCALC> &tBuf)
{
    AscendC::LocalTensor<float> local =
        tBuf.GetWithOffset<float>(kEpScalarUbBytes / sizeof(float), kEpScalarUbOffset);
    AscendC::GlobalTensor<float> dst;
    dst.SetGlobalBuffer(reinterpret_cast<__gm__ float *>(dstGM), 1);
    local.SetValue(0, value);

    AscendC::DataCopyExtParams copyParams {1, static_cast<uint32_t>(sizeof(float)), 0, 0, 0};
    AscendC::SetFlag<AscendC::HardEvent::S_MTE3>(EVENT_ID0);
    AscendC::WaitFlag<AscendC::HardEvent::S_MTE3>(EVENT_ID0);
    AscendC::DataCopyPad(dst, local, copyParams);
    AscendC::SetFlag<AscendC::HardEvent::MTE3_S>(EVENT_ID0);
    AscendC::WaitFlag<AscendC::HardEvent::MTE3_S>(EVENT_ID0);
}

__aicore__ inline void StoreUint64ToGm(
    GM_ADDR dstGM, uint64_t value, AscendC::TBuf<AscendC::QuePosition::VECCALC> &tBuf)
{
    AscendC::LocalTensor<uint64_t> local =
        tBuf.GetWithOffset<uint64_t>(kEpScalarUbBytes / sizeof(uint64_t), kEpScalarUbOffset);
    AscendC::GlobalTensor<uint64_t> dst;
    dst.SetGlobalBuffer(reinterpret_cast<__gm__ uint64_t *>(dstGM), 1);
    local.SetValue(0, value);

    AscendC::DataCopyExtParams copyParams {1, static_cast<uint32_t>(sizeof(uint64_t)), 0, 0, 0};
    AscendC::SetFlag<AscendC::HardEvent::S_MTE3>(EVENT_ID0);
    AscendC::WaitFlag<AscendC::HardEvent::S_MTE3>(EVENT_ID0);
    AscendC::DataCopyPad(dst, local, copyParams);
    AscendC::SetFlag<AscendC::HardEvent::MTE3_S>(EVENT_ID0);
    AscendC::WaitFlag<AscendC::HardEvent::MTE3_S>(EVENT_ID0);
}

__aicore__ inline void StoreInt64WordsToGm(
    GM_ADDR dstGM, AscendC::LocalTensor<int64_t> &local, AscendC::TBuf<AscendC::QuePosition::VECCALC> &tBuf,
    uint32_t bytes)
{
    (void)tBuf;
    AscendC::GlobalTensor<int64_t> dst;
    dst.SetGlobalBuffer(reinterpret_cast<__gm__ int64_t *>(dstGM), bytes / sizeof(int64_t));

    AscendC::DataCopyExtParams copyParams {1, bytes, 0, 0, 0};
    AscendC::SetFlag<AscendC::HardEvent::S_MTE3>(EVENT_ID0);
    AscendC::WaitFlag<AscendC::HardEvent::S_MTE3>(EVENT_ID0);
    AscendC::DataCopyPad(dst, local, copyParams);
    AscendC::SetFlag<AscendC::HardEvent::MTE3_S>(EVENT_ID0);
    AscendC::WaitFlag<AscendC::HardEvent::MTE3_S>(EVENT_ID0);
}

__aicore__ inline int64_t PackInt32Pair(uint32_t low, int32_t high)
{
    const uint64_t packed = static_cast<uint64_t>(low) | (static_cast<uint64_t>(static_cast<uint32_t>(high)) << 32);
    return static_cast<int64_t>(packed);
}

__aicore__ inline void StoreWindowHeader(GM_ADDR headerGM, int32_t rankSize, int64_t maxRoutesPerSrc,
    int64_t rowBytes, int64_t slotBytes, int64_t totalBytes, AscendC::TBuf<AscendC::QuePosition::VECCALC> &tBuf)
{
    AscendC::LocalTensor<int64_t> local =
        tBuf.GetWithOffset<int64_t>(kEpScalarUbBytes / sizeof(int64_t), kEpScalarUbOffset);
    local.SetValue(0, PackInt32Pair(TileXREp::kEpWindowMagic, rankSize));
    local.SetValue(1, maxRoutesPerSrc);
    local.SetValue(2, rowBytes);
    local.SetValue(3, slotBytes);
    local.SetValue(4, totalBytes);
    local.SetValue(5, 0);
    local.SetValue(6, 0);
    local.SetValue(7, 0);
    StoreInt64WordsToGm(headerGM, local, tBuf, TileXREp::kEpWindowHeaderBytes);
}

__aicore__ inline void ClearLocalWindow(
    GM_ADDR localWindow, int32_t rankSize, int64_t maxRoutesPerSrc, int64_t rowBytes, int64_t slotBytes,
    int64_t totalBytes, AscendC::TBuf<AscendC::QuePosition::VECCALC> &tBuf)
{
    StoreWindowHeader(localWindow, rankSize, maxRoutesPerSrc, rowBytes, slotBytes, totalBytes, tBuf);

    AscendC::PipeBarrier<PIPE_ALL>();
}

__aicore__ inline bool TileXREpUsesUdmaWindow(const __gm__ TileXR::CommArgs *args, GM_ADDR workspaceGM)
{
    return workspaceGM != nullptr && args->localRankSize > 0 && args->localRankSize < args->rankSize &&
        TileXR::UDMARegistryEnabled(args);
}

__aicore__ inline GM_ADDR TileXREpWindowBase(GM_ADDR *shareAddrs, int32_t rank, bool useUdmaWindow, GM_ADDR workspaceGM)
{
    return useUdmaWindow ? workspaceGM : shareAddrs[rank] + TileXR::IPC_DATA_OFFSET;
}

__aicore__ inline bool TileXREpIsSameNodePeer(int32_t rank, int32_t peer, int32_t localRankSize)
{
    return localRankSize > 0 && rank / localRankSize == peer / localRankSize;
}

__aicore__ inline bool TileXREpIsRemotePeer(int32_t rank, int32_t peer, int32_t localRankSize)
{
    return !TileXREpIsSameNodePeer(rank, peer, localRankSize);
}

__aicore__ inline bool TileXREpUsesUdmaPeer(int32_t rank, int32_t peer, int32_t localRankSize)
{
    if (peer == rank) {
        return false;
    }
    if (localRankSize <= 1) {
        return true;
    }
    return TileXREpIsRemotePeer(rank, peer, localRankSize);
}

__aicore__ inline void TileXREpPublishLocalUdmaSlot(GM_ADDR localWindow, GM_ADDR sendWindow, int32_t dstRank,
    int64_t slotBytes, AscendC::TBuf<AscendC::QuePosition::VECCALC> &tBuf)
{
    CopyBytesGmToGm(localWindow + SlotOffset(dstRank, slotBytes), sendWindow + SlotOffset(dstRank, slotBytes), tBuf,
        slotBytes);
}

__aicore__ inline void TileXREpPublishLocalUdmaSlots(GM_ADDR localWindow, GM_ADDR sendWindow, int32_t rank,
    int32_t rankSize, int32_t localRankSize, int64_t slotBytes,
    AscendC::TBuf<AscendC::QuePosition::VECCALC> &tBuf)
{
    for (int32_t dstRank = 0; dstRank < rankSize; ++dstRank) {
        if (dstRank != rank && TileXREpIsSameNodePeer(rank, dstRank, localRankSize)) {
            TileXREpPublishLocalUdmaSlot(localWindow, sendWindow, dstRank, slotBytes, tBuf);
        }
    }
}

__aicore__ inline GM_ADDR TileXREpDispatchWriteWindow(GM_ADDR sendWindow, GM_ADDR localIpcWindow, int32_t rank,
    int32_t dstRank, int32_t localRankSize)
{
    if (localIpcWindow != nullptr && localRankSize > 1 && dstRank != rank &&
        TileXREpIsSameNodePeer(rank, dstRank, localRankSize)) {
        return localIpcWindow;
    }
    return sendWindow;
}

__aicore__ inline void TileXREpPublishUdmaSlots(const __gm__ TileXR::CommArgs *args,
    const AscendC::LocalTensor<uint8_t> &wqeScratch, GM_ADDR localWindow,
    int32_t rank, int32_t rankSize, int64_t slotBytes)
{
    for (int32_t dstRank = 0; dstRank < rankSize; ++dstRank) {
        if (dstRank == rank) {
            continue;
        }
        TileXR::UDMAPutNbi<uint8_t>(args, wqeScratch, dstRank,
            reinterpret_cast<__gm__ uint8_t *>(localWindow + SlotOffset(dstRank, slotBytes)),
            SlotOffset(rank, slotBytes), static_cast<uint32_t>(slotBytes));
        TileXR::UDMAQuiet(args, dstRank);
    }
}

__aicore__ inline void TileXREpPullUdmaSlots(
    const __gm__ TileXR::CommArgs *args, const AscendC::LocalTensor<uint8_t> &wqeScratch,
    GM_ADDR localWindow, int32_t rank, int32_t rankSize, int64_t slotBytes)
{
    for (int32_t srcRank = 0; srcRank < rankSize; ++srcRank) {
        if (srcRank == rank) {
            continue;
        }
        TileXR::UDMAGetNbi<uint8_t>(args, wqeScratch, srcRank,
            reinterpret_cast<__gm__ uint8_t *>(localWindow + SlotOffset(srcRank, slotBytes)),
            SlotOffset(rank, slotBytes), static_cast<uint32_t>(slotBytes));
        TileXR::UDMAQuiet(args, srcRank);
    }
}

__aicore__ inline uint64_t TileXREpReadyValue(int64_t magic)
{
    return (static_cast<uint64_t>(magic) << 32) | static_cast<uint64_t>(TileXREp::kEpStepDispatchReady);
}

__aicore__ inline int32_t TileXREpTpGroupStartRank(int32_t rank, int32_t tpWorldSize)
{
    if (tpWorldSize <= 0) {
        return -1;
    }
    return (rank / tpWorldSize) * tpWorldSize;
}

__aicore__ inline int32_t TileXREpGlobalDstRankForTp(
    int64_t expertDstRank, int32_t tpWorldSize, int32_t tpRankId)
{
    if (expertDstRank < 0 || tpWorldSize <= 0 || tpRankId < 0 || tpRankId >= tpWorldSize) {
        return -1;
    }
    if (tpWorldSize == 1) {
        return static_cast<int32_t>(expertDstRank);
    }
    return static_cast<int32_t>(expertDstRank * tpWorldSize + tpRankId);
}

__aicore__ inline void TileXREpNotifyUdmaReady(
    const __gm__ TileXR::CommArgs *args, const AscendC::LocalTensor<uint8_t> &wqeScratch,
    GM_ADDR localWindow, int32_t rank, int32_t rankSize,
    int32_t localRankSize, int64_t totalBytes, int64_t magic, int64_t slotBytes,
    AscendC::TBuf<AscendC::QuePosition::VECCALC> &tBuf)
{
    const uint64_t ready = TileXREpReadyValue(magic);
    const uint64_t signalOffset = static_cast<uint64_t>(UDMAReadyOffset(totalBytes, rank));
    StoreUint64ToGm(localWindow + UDMAReadyOffset(totalBytes, rank), ready, tBuf);
    for (int32_t peer = 0; peer < rankSize; ++peer) {
        if (!TileXREpUsesUdmaPeer(rank, peer, localRankSize)) {
            continue;
        }
        TileXR::UDMAPutNbi<uint8_t>(args, wqeScratch, peer,
            reinterpret_cast<__gm__ uint8_t *>(localWindow + SlotOffset(peer, slotBytes)),
            static_cast<uint64_t>(UDMARecvWindowOffset(totalBytes) + SlotOffset(rank, slotBytes)),
            static_cast<uint32_t>(slotBytes));
        TileXR::UDMAQuiet(args, peer);
        TileXR::UDMAPutNbi<uint8_t>(args, wqeScratch, peer,
            reinterpret_cast<__gm__ uint8_t *>(localWindow + UDMAReadyOffset(totalBytes, rank)),
            signalOffset, static_cast<uint32_t>(sizeof(uint64_t)));
        TileXR::UDMAQuiet(args, peer);
    }
}

__aicore__ inline void TileXREpNotifyAllUdmaReady(
    const __gm__ TileXR::CommArgs *args, const AscendC::LocalTensor<uint8_t> &wqeScratch,
    GM_ADDR localWindow, int32_t rank, int32_t rankSize,
    int64_t totalBytes, int64_t magic, int64_t slotBytes)
{
    const uint64_t ready = TileXREpReadyValue(magic);
    const uint64_t signalOffset = static_cast<uint64_t>(UDMAReadyOffset(totalBytes, rank));
    for (int32_t peer = 0; peer < rankSize; ++peer) {
        if (peer == rank) {
            continue;
        }
        TileXR::UDMAPutSignalNbi<uint8_t>(args, wqeScratch, peer,
            reinterpret_cast<__gm__ uint8_t *>(localWindow + SlotOffset(peer, slotBytes)),
            static_cast<uint64_t>(UDMARecvWindowOffset(totalBytes) + SlotOffset(rank, slotBytes)),
            static_cast<uint32_t>(slotBytes), signalOffset, ready);
        TileXR::UDMAQuiet(args, peer);
    }
}

__aicore__ inline void TileXREpWaitUdmaReady(GM_ADDR localWindow, int32_t rank, int32_t rankSize,
    int32_t localRankSize, int64_t totalBytes, int64_t magic, AscendC::TBuf<AscendC::QuePosition::VECCALC> &tBuf)
{
    const uint64_t ready = TileXREpReadyValue(magic);
    for (int32_t peer = 0; peer < rankSize; ++peer) {
        if (!TileXREpUsesUdmaPeer(rank, peer, localRankSize)) {
            continue;
        }
        while (LoadUint64FromGm(localWindow + UDMAReadyOffset(totalBytes, peer), tBuf) != ready) {
        }
    }
}

__aicore__ inline void TileXREpWaitAllUdmaReady(GM_ADDR localWindow, int32_t rank, int32_t rankSize,
    int64_t totalBytes, int64_t magic, AscendC::TBuf<AscendC::QuePosition::VECCALC> &tBuf)
{
    const uint64_t ready = TileXREpReadyValue(magic);
    for (int32_t peer = 0; peer < rankSize; ++peer) {
        if (peer == rank) {
            continue;
        }
        while (LoadUint64FromGm(localWindow + UDMAReadyOffset(totalBytes, peer), tBuf) != ready) {
        }
    }
}

__aicore__ inline void TileXREpFlushDispatchSlotHeaders(
    GM_ADDR localWindow, int32_t rankSize, int64_t slotBytes, AscendC::TBuf<AscendC::QuePosition::VECCALC> &tBuf)
{
    for (int32_t dstRank = 0; dstRank < rankSize; ++dstRank) {
        (void)LoadInt32FromGm(localWindow + SlotOffset(dstRank, slotBytes), tBuf);
    }
    AscendC::PipeBarrier<PIPE_ALL>();
}

#include "tilexr_ep_dispatch_helpers.h"
#include "tilexr_ep_combine_helpers.h"

__aicore__ inline TileXREp::EpAssistTuple LoadAssistTupleFromGm(
    GM_ADDR srcGM, AscendC::TBuf<AscendC::QuePosition::VECCALC> &tBuf)
{
    return TileXREpLoadAssistTuple(srcGM, tBuf);
}

__aicore__ inline void StoreSlotHeader(GM_ADDR slotGM, int32_t count, int32_t srcRank, int64_t payloadBytes,
    int64_t assistBytes, int64_t magic, AscendC::TBuf<AscendC::QuePosition::VECCALC> &tBuf)
{
    TileXREpStoreDispatchSlotHeader(slotGM, count, srcRank, payloadBytes, assistBytes, magic, tBuf);
}

__aicore__ inline void StoreAssistTuple(GM_ADDR assistGM, int64_t index, int32_t srcRank, int32_t tokenId,
    int32_t topKId, int32_t expertId, AscendC::TBuf<AscendC::QuePosition::VECCALC> &tBuf)
{
    TileXREpStoreAssistTuple(assistGM, index, srcRank, tokenId, topKId, expertId, tBuf);
}

__aicore__ inline void WriteAssist(
    GM_ADDR assistGM, int64_t index, int32_t srcRank, int32_t tokenId, int32_t topKId, int32_t expertId,
    AscendC::TBuf<AscendC::QuePosition::VECCALC> &tBuf)
{
    TileXREpStoreAssistTuple(assistGM, index, srcRank, tokenId, topKId, expertId, tBuf);
}

__aicore__ inline void TileXREpWriteRouteToWindow(GM_ADDR targetWindow, int64_t dstRank, int64_t dstIndex,
    int64_t slotBytes, int64_t payloadBytesPerSlot, GM_ADDR xGM, GM_ADDR scalesGM, int64_t token, int64_t h,
    int64_t inputRowBytes, int64_t rowBytes, int64_t payloadRowBytes, int64_t maxRoutesPerSrc, int64_t quantMode,
    int32_t rank, int32_t topKId, int32_t expertId, AscendC::TBuf<AscendC::QuePosition::VECCALC> &tBuf)
{
    if (quantMode == kEpQuantModeStatic) {
        TileXREpCopyStaticQuantRoutePayload(targetWindow, dstRank, dstIndex, slotBytes, xGM, scalesGM, token, h,
            inputRowBytes, payloadRowBytes, tBuf);
    } else if (quantMode == kEpQuantModePerTokenDynamic) {
        TileXREpCopyPerTokenDynamicQuantRoutePayload(targetWindow, dstRank, dstIndex, slotBytes, maxRoutesPerSrc,
            xGM, token, h, inputRowBytes, rowBytes, tBuf);
    } else {
        TileXREpCopyRoutePayload(targetWindow, dstRank, dstIndex, slotBytes, xGM, token, inputRowBytes, rowBytes,
            payloadRowBytes, tBuf);
    }
    WriteAssist(targetWindow + AssistOffset(dstRank, slotBytes, payloadBytesPerSlot), dstIndex, rank,
        static_cast<int32_t>(token), topKId, expertId, tBuf);
}

__aicore__ inline void TileXREpRouteLocalTokens(GM_ADDR sendWindow, GM_ADDR localIpcWindow, int32_t rank,
    int32_t rankSize, int32_t expertRankSize, int32_t tpWorldSize, int32_t tpRankId, int32_t localRankSize,
    GM_ADDR xGM, GM_ADDR expertIdsGM, GM_ADDR scalesGM, GM_ADDR xActiveMaskGM, int64_t bs, int64_t h,
    int64_t topK, int64_t moeExpertNum,
    int64_t sharedExpertNum, int64_t sharedExpertRankNum,
    int64_t localExpertNum, int64_t maxRoutesPerSrc, int64_t inputRowBytes, int64_t rowBytes,
    int64_t payloadRowBytes, int64_t slotBytes, int64_t payloadBytesPerSlot, int64_t quantMode, int64_t *dstCounts,
    AscendC::TBuf<AscendC::QuePosition::VECCALC> &tBuf)
{
    for (int64_t token = 0; token < bs; ++token) {
        if (!TileXREpIsTokenActive(xActiveMaskGM, token, tBuf)) {
            continue;
        }
        for (int64_t sharedExpertId = 0; sharedExpertId < sharedExpertNum; ++sharedExpertId) {
            const int64_t expertDstRank = TileXREpRouteToDstRank(static_cast<int32_t>(sharedExpertId), localExpertNum,
                expertRankSize, sharedExpertNum, sharedExpertRankNum);
            const int64_t dstRank = TileXREpGlobalDstRankForTp(expertDstRank, tpWorldSize, tpRankId);
            if (dstRank < 0 || dstCounts[dstRank] >= maxRoutesPerSrc) {
                continue;
            }
            const int64_t dstIndex = dstCounts[dstRank];
            GM_ADDR targetWindow = TileXREpDispatchWriteWindow(sendWindow, localIpcWindow, rank,
                static_cast<int32_t>(dstRank), localRankSize);
            TileXREpWriteRouteToWindow(targetWindow, dstRank, dstIndex, slotBytes, payloadBytesPerSlot, xGM, scalesGM,
                token, h, inputRowBytes, rowBytes, payloadRowBytes, maxRoutesPerSrc, quantMode, rank,
                static_cast<int32_t>(topK + sharedExpertId), static_cast<int32_t>(sharedExpertId), tBuf);
            ++dstCounts[dstRank];
        }
        for (int64_t topKId = 0; topKId < topK; ++topKId) {
            const int64_t route = token * topK + topKId;
            const int32_t expertId = LoadInt32FromGm(expertIdsGM + route * static_cast<int64_t>(sizeof(int32_t)),
                tBuf);
            if (expertId < 0 || static_cast<int64_t>(expertId) >= moeExpertNum) {
                continue;
            }
            const int32_t globalExpertId = static_cast<int32_t>(sharedExpertNum) + expertId;
            const int64_t expertDstRank = TileXREpRouteToDstRank(globalExpertId, localExpertNum, expertRankSize,
                sharedExpertNum, sharedExpertRankNum);
            const int64_t dstRank = TileXREpGlobalDstRankForTp(expertDstRank, tpWorldSize, tpRankId);
            if (dstRank < 0 || dstCounts[dstRank] >= maxRoutesPerSrc) {
                continue;
            }
            const int64_t dstIndex = dstCounts[dstRank];
            GM_ADDR targetWindow = TileXREpDispatchWriteWindow(sendWindow, localIpcWindow, rank,
                static_cast<int32_t>(dstRank), localRankSize);
            TileXREpWriteRouteToWindow(targetWindow, dstRank, dstIndex, slotBytes, payloadBytesPerSlot, xGM, scalesGM,
                token, h, inputRowBytes, rowBytes, payloadRowBytes, maxRoutesPerSrc, quantMode, rank,
                static_cast<int32_t>(topKId), globalExpertId, tBuf);
            ++dstCounts[dstRank];
        }
    }
}

__aicore__ inline bool IsValidShape(int64_t bs, int64_t h, int64_t topK, int64_t moeExpertNum, int64_t dtypeBytes,
    int64_t maxRoutesPerSrc, int64_t rowBytes, int64_t payloadRowBytes, int64_t payloadBytesPerSlot,
    int64_t assistBytesPerSlot, int64_t slotBytes, int64_t totalBytes, int32_t rankSize, int32_t expertRankSize,
    int64_t sharedExpertNum, int64_t sharedExpertRankNum, int64_t quantMode)
{
    const int64_t moeRankNum = static_cast<int64_t>(expertRankSize) - sharedExpertRankNum;
    if (bs <= 0 || h <= 0 || topK <= 0 || moeExpertNum <= 0 || dtypeBytes <= 0 || maxRoutesPerSrc <= 0 ||
        rowBytes <= 0 || payloadRowBytes != rowBytes || payloadBytesPerSlot <= 0 || assistBytesPerSlot <= 0 ||
        slotBytes <= 0 || totalBytes <= 0 ||
        sharedExpertNum < 0 || sharedExpertRankNum < 0 || sharedExpertNum != sharedExpertRankNum ||
        moeRankNum <= 0 || moeExpertNum % moeRankNum != 0 || expertRankSize <= 0 || expertRankSize > rankSize) {
        return false;
    }
    const int64_t expectedRoutes = bs * (topK + sharedExpertNum);
    const int64_t expectedRowBytes = h * dtypeBytes;
    const int64_t scaleBytes = quantMode == kEpQuantModePerTokenDynamic ?
        expectedRoutes * static_cast<int64_t>(sizeof(float)) : 0;
    const int64_t expectedPayload =
        AlignUp(expectedRoutes * payloadRowBytes + scaleBytes, TileXREp::kEpWindowAlignmentBytes);
    const int64_t expectedAssist = AlignUp(expectedRoutes * static_cast<int64_t>(sizeof(TileXREp::EpAssistTuple)),
        TileXREp::kEpWindowAlignmentBytes);
    const int64_t expectedSlot =
        AlignUp(TileXREp::kEpSrcSlotHeaderBytes + expectedPayload + expectedAssist, TileXREp::kEpWindowAlignmentBytes);
    const int64_t expectedTotal = TileXREp::kEpWindowHeaderBytes + static_cast<int64_t>(rankSize) * expectedSlot;
    return maxRoutesPerSrc == expectedRoutes && rowBytes == expectedRowBytes &&
        payloadBytesPerSlot == expectedPayload && assistBytesPerSlot == expectedAssist && slotBytes == expectedSlot &&
        totalBytes == expectedTotal && totalBytes <= TileXR::IPC_BUFF_MAX_SIZE;
}

__aicore__ inline void TileXREpPublishTpRows(GM_ADDR tpWindow, GM_ADDR expandXOutGM,
    __gm__ TileXREp::EpAssistTuple *localAssistBase, int32_t rank, int64_t outRecord, int64_t maxRoutesPerSrc,
    int64_t rowBytes, int64_t slotBytes, int64_t payloadBytesPerSlot, int64_t magic,
    AscendC::TBuf<AscendC::QuePosition::VECCALC> &tBuf)
{
    const int64_t publishCount = outRecord > maxRoutesPerSrc ? maxRoutesPerSrc : outRecord;
    for (int64_t item = 0; item < publishCount; ++item) {
        CopyBytesGmToGm(tpWindow + PayloadOffset(rank, slotBytes) + item * rowBytes,
            expandXOutGM + item * rowBytes, tBuf, rowBytes);
        const TileXREp::EpAssistTuple tuple = TileXREpLoadAssistTuple(
            reinterpret_cast<GM_ADDR>(localAssistBase + item), tBuf);
        TileXREpStoreAssistTuple(tpWindow + AssistOffset(rank, slotBytes, payloadBytesPerSlot), item, tuple.srcRank,
            tuple.tokenId, tuple.topKId, tuple.expertId, tBuf);
    }
    StoreSlotHeader(tpWindow + SlotOffset(rank, slotBytes), static_cast<int32_t>(publishCount), rank,
        publishCount * rowBytes, publishCount * static_cast<int64_t>(sizeof(TileXREp::EpAssistTuple)), magic, tBuf);
}

__aicore__ inline int64_t TileXREpAppendTpSourceSlotRows(GM_ADDR tpWindow, GM_ADDR expandXOutGM,
    __gm__ TileXREp::EpAssistTuple *localAssistBase, int32_t srcRank, int64_t outRecord,
    int64_t maxRoutesPerSrc, int64_t rowBytes, int64_t payloadRowBytes, int64_t slotBytes,
    int64_t payloadBytesPerSlot, int64_t quantMode, int64_t localExpertNum, int64_t sharedExpertNum,
    int64_t sharedExpertRankNum, GM_ADDR dynamicScalesOutGM, GM_ADDR expertTokenNumsOutGM,
    int64_t expertTokenNumsType, int64_t magic, int32_t *sourceCountOut,
    AscendC::TBuf<AscendC::QuePosition::VECCALC> &tBuf)
{
    const int32_t sourceCount = TileXREpWaitDispatchSlotReady(tpWindow + SlotOffset(srcRank, slotBytes), srcRank,
        magic, tBuf);
    if (sourceCountOut != nullptr) {
        *sourceCountOut = sourceCount;
    }
    if (sourceCount <= 0 || sourceCount > maxRoutesPerSrc) {
        return outRecord;
    }
    GM_ADDR peerPayloadBase = tpWindow + PayloadOffset(srcRank, slotBytes);
    GM_ADDR peerScaleBase = peerPayloadBase + maxRoutesPerSrc * rowBytes;
    GM_ADDR peerAssistBase = tpWindow + AssistOffset(srcRank, slotBytes, payloadBytesPerSlot);
    for (int64_t item = 0; item < sourceCount; ++item) {
        CopyBytesGmToGm(expandXOutGM + outRecord * rowBytes, peerPayloadBase + item * payloadRowBytes, tBuf,
            rowBytes);
        if (quantMode == kEpQuantModePerTokenDynamic && dynamicScalesOutGM != nullptr) {
            const float scale = LoadFloatFromGm(peerScaleBase + item * static_cast<int64_t>(sizeof(float)), tBuf);
            StoreFloatToGm(dynamicScalesOutGM + outRecord * static_cast<int64_t>(sizeof(float)), scale, tBuf);
        }
        const TileXREp::EpAssistTuple tuple = TileXREpLoadAssistTuple(
            peerAssistBase + item * static_cast<int64_t>(sizeof(TileXREp::EpAssistTuple)), tBuf);
        localAssistBase[outRecord].srcRank = tuple.srcRank;
        localAssistBase[outRecord].tokenId = tuple.tokenId;
        localAssistBase[outRecord].topKId = tuple.topKId;
        localAssistBase[outRecord].expertId = tuple.expertId;
        const int64_t localExpert =
            TileXREpRouteToLocalExpert(tuple.expertId, localExpertNum, sharedExpertNum, sharedExpertRankNum);
        if (localExpert >= 0 && localExpert < localExpertNum) {
            TileXREpIncrementExpertTokenNum(expertTokenNumsOutGM, localExpert, expertTokenNumsType);
        }
        ++outRecord;
    }
    return outRecord;
}

__aicore__ inline int64_t TileXREpAppendTpGroupRows(GM_ADDR *shareAddrs, GM_ADDR expandXOutGM,
    __gm__ TileXREp::EpAssistTuple *localAssistBase, int32_t rank, int32_t expertRankSize, int32_t tpWorldSize,
    int64_t outRecord, int64_t maxRoutesPerSrc, int64_t rowBytes, int64_t payloadRowBytes, int64_t slotBytes,
    int64_t payloadBytesPerSlot, int64_t totalBytes, int64_t quantMode, int64_t localExpertNum,
    int64_t sharedExpertNum, int64_t sharedExpertRankNum, GM_ADDR dynamicScalesOutGM, GM_ADDR expertTokenNumsOutGM,
    int64_t expertTokenNumsType, int64_t magic, __gm__ int32_t *tpRecvCountsOut,
    AscendC::TBuf<AscendC::QuePosition::VECCALC> &tBuf)
{
    const int32_t tpGroupStartRank = TileXREpTpGroupStartRank(rank, tpWorldSize);
    if (tpGroupStartRank < 0 || expertRankSize <= 0) {
        return outRecord;
    }
    for (int32_t tpLane = 0; tpLane < tpWorldSize; ++tpLane) {
        const int32_t tpPeerRank = tpGroupStartRank + tpLane;
        if (tpPeerRank == rank) {
            continue;
        }
        int32_t peerTpCount = 0;
        GM_ADDR peerTpWindow = shareAddrs[tpPeerRank] + TileXR::IPC_DATA_OFFSET +
            TileXREpTpWindowOffset(totalBytes);
        for (int32_t expertSrcRank = 0; expertSrcRank < expertRankSize; ++expertSrcRank) {
            const int32_t peerSourceRank = expertSrcRank * tpWorldSize + tpLane;
            int32_t sourceCount = 0;
            outRecord = TileXREpAppendTpSourceSlotRows(peerTpWindow, expandXOutGM, localAssistBase, peerSourceRank,
                outRecord, maxRoutesPerSrc, rowBytes, payloadRowBytes, slotBytes, payloadBytesPerSlot, quantMode,
                localExpertNum, sharedExpertNum, sharedExpertRankNum, dynamicScalesOutGM, expertTokenNumsOutGM,
                expertTokenNumsType, magic, &sourceCount, tBuf);
            peerTpCount += sourceCount > 0 ? sourceCount : 0;
        }
        if (tpRecvCountsOut != nullptr) {
            tpRecvCountsOut[tpLane] = peerTpCount;
        }
    }
    return outRecord;
}

} // namespace

extern "C" __global__ __aicore__ void tilexr_ep_dispatch_kernel(GM_ADDR commArgsGM, GM_ADDR xGM, GM_ADDR expertIdsGM,
    GM_ADDR scalesGM, GM_ADDR xActiveMaskGM, GM_ADDR expandXOutGM, GM_ADDR dynamicScalesOutGM,
    GM_ADDR expertTokenNumsOutGM, GM_ADDR epRecvCountsOutGM, GM_ADDR tpRecvCountsOutGM,
    GM_ADDR assistInfoForCombineOutGM, GM_ADDR workspaceGM, int64_t bs, int64_t h, int64_t topK,
    int64_t moeExpertNum, int64_t dtypeBytes, int64_t maxRoutesPerSrc, int64_t rowBytes, int64_t payloadRowBytes,
    int64_t payloadBytesPerSlot, int64_t assistBytesPerSlot, int64_t slotBytes, int64_t totalBytes,
    int64_t expertTokenNumsType, int64_t sharedExpertNum, int64_t sharedExpertRankNum, int64_t quantMode,
    int64_t tpWorldSize, int64_t tpRankId, int64_t magic)
{
    if constexpr (g_coreType == AscendC::AIV) {
        if (AscendC::GetBlockIdx() != 0) {
            return;
        }

        if (commArgsGM == nullptr || xGM == nullptr || expertIdsGM == nullptr || expandXOutGM == nullptr ||
            expertTokenNumsOutGM == nullptr || epRecvCountsOutGM == nullptr || assistInfoForCombineOutGM == nullptr) {
            return;
        }

        auto args = reinterpret_cast<__gm__ TileXR::CommArgs *>(commArgsGM);
        const int32_t rank = args->rank;
        const int32_t rankSize = args->rankSize;
        const int32_t effectiveTpWorldSize = tpWorldSize == 0 ? 1 : static_cast<int32_t>(tpWorldSize);
        const int32_t effectiveTpRankId = static_cast<int32_t>(tpRankId);
        const int32_t expertRankSize = effectiveTpWorldSize > 0 ? rankSize / effectiveTpWorldSize : 0;
        if (rankSize <= 0 || rankSize > TileXR::TILEXR_MAX_RANK_SIZE || rank < 0 || rank >= rankSize ||
            effectiveTpWorldSize <= 0 || effectiveTpRankId < 0 || effectiveTpRankId >= effectiveTpWorldSize ||
            expertRankSize <= 0 || expertRankSize * effectiveTpWorldSize != rankSize ||
            (quantMode != kEpQuantModeNone && quantMode != kEpQuantModeStatic &&
                quantMode != kEpQuantModePerTokenDynamic) ||
            (quantMode == kEpQuantModeStatic && scalesGM == nullptr) ||
            (quantMode == kEpQuantModePerTokenDynamic && (dynamicScalesOutGM == nullptr || scalesGM != nullptr)) ||
            !IsValidShape(bs, h, topK, moeExpertNum, dtypeBytes, maxRoutesPerSrc, rowBytes, payloadRowBytes,
                payloadBytesPerSlot, assistBytesPerSlot, slotBytes, totalBytes, rankSize, expertRankSize, sharedExpertNum,
                sharedExpertRankNum, quantMode)) {
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

        const bool useUdmaWindow = TileXREpUsesUdmaWindow(args, workspaceGM);
        GM_ADDR localWindow = TileXREpWindowBase(shareAddrs, rank, useUdmaWindow, workspaceGM);
        ClearLocalWindow(localWindow, rankSize, maxRoutesPerSrc, rowBytes, slotBytes, totalBytes, tBuf);
        sync.SetInnerFlag(static_cast<int32_t>(magic), TileXREp::kEpStepWindowCleared);
        for (int32_t peer = 0; peer < rankSize; ++peer) {
            sync.WaitRankInnerFlag(static_cast<int32_t>(magic), TileXREp::kEpStepWindowCleared, peer);
        }

        int64_t dstCounts[TileXR::TILEXR_MAX_RANK_SIZE];
        for (int32_t peer = 0; peer < rankSize; ++peer) {
            dstCounts[peer] = 0;
        }

        const int64_t localExpertNum = TileXREpLocalExpertCount(moeExpertNum, expertRankSize, sharedExpertRankNum);
        if (localExpertNum <= 0) {
            return;
        }
        const int64_t inputRowBytes = h * static_cast<int64_t>(sizeof(uint16_t));
        TileXREpRouteLocalTokens(localWindow, nullptr, rank, rankSize, expertRankSize, effectiveTpWorldSize,
            effectiveTpRankId, rankSize, xGM, expertIdsGM, scalesGM, xActiveMaskGM, bs, h, topK, moeExpertNum,
            sharedExpertNum, sharedExpertRankNum, localExpertNum, maxRoutesPerSrc, inputRowBytes, rowBytes,
            payloadRowBytes, slotBytes, payloadBytesPerSlot, quantMode, dstCounts, tBuf);

        for (int32_t dstRank = 0; dstRank < rankSize; ++dstRank) {
            const int64_t payloadBytes = dstCounts[dstRank] * payloadRowBytes +
                (quantMode == kEpQuantModePerTokenDynamic ?
                    dstCounts[dstRank] * static_cast<int64_t>(sizeof(float)) : 0);
            StoreSlotHeader(localWindow + SlotOffset(dstRank, slotBytes), static_cast<int32_t>(dstCounts[dstRank]),
                rank, payloadBytes, dstCounts[dstRank] * static_cast<int64_t>(sizeof(TileXREp::EpAssistTuple)),
                magic, tBuf);
        }
        TileXREpFlushDispatchSlotHeaders(localWindow, rankSize, slotBytes, tBuf);
        if (useUdmaWindow) {
            TileXREpPullUdmaSlots(
                args, udmaWqeBuf.Get<uint8_t>(), localWindow, rank, rankSize, slotBytes);
        } else {
            sync.SetInnerFlag(static_cast<int32_t>(magic), TileXREp::kEpStepDispatchReady);
            for (int32_t peer = 0; peer < rankSize; ++peer) {
                sync.WaitRankInnerFlag(static_cast<int32_t>(magic), TileXREp::kEpStepDispatchReady, peer);
            }
        }

        auto epRecvCountsOut = reinterpret_cast<__gm__ int32_t *>(epRecvCountsOutGM);
        auto tpRecvCountsOut = reinterpret_cast<__gm__ int32_t *>(tpRecvCountsOutGM);
        const int64_t localExpertNum64 = localExpertNum;
        TileXREpClearExpertTokenNums(expertTokenNumsOutGM, localExpertNum64, expertTokenNumsType);

        int64_t outRecord = 0;
        auto localAssistBase = reinterpret_cast<__gm__ TileXREp::EpAssistTuple *>(assistInfoForCombineOutGM);
        GM_ADDR tpPublishWindow = nullptr;
        if (effectiveTpWorldSize > 1 && tpRecvCountsOut != nullptr) {
            tpPublishWindow = localWindow + TileXREpTpWindowOffset(totalBytes);
        }
        for (int32_t expertSrcRank = 0; expertSrcRank < expertRankSize; ++expertSrcRank) {
            const int32_t srcRank = expertSrcRank * effectiveTpWorldSize + effectiveTpRankId;
            GM_ADDR sourceWindow = TileXREpWindowBase(shareAddrs, srcRank, useUdmaWindow, localWindow);
            const int32_t slotRank = useUdmaWindow ? srcRank : rank;
            outRecord = TileXREpDrainSourceWindow(sourceWindow, slotRank, srcRank, slotBytes, payloadBytesPerSlot,
                maxRoutesPerSrc, rowBytes, payloadRowBytes, quantMode, localExpertNum64, sharedExpertNum,
                sharedExpertRankNum, expandXOutGM, dynamicScalesOutGM, expertTokenNumsOutGM, expertTokenNumsType,
                magic, epRecvCountsOut, tpRecvCountsOut, localAssistBase, outRecord, tpPublishWindow, rank, tBuf);
        }
        if (tpPublishWindow != nullptr) {
            const int32_t localTpCount = static_cast<int32_t>(outRecord);
            tpRecvCountsOut[tpRankId] = localTpCount;
            AscendC::PipeBarrier<PIPE_ALL>();
            outRecord = TileXREpAppendTpGroupRows(shareAddrs, expandXOutGM, localAssistBase, rank,
                expertRankSize, effectiveTpWorldSize, outRecord, maxRoutesPerSrc, rowBytes, payloadRowBytes,
                slotBytes, payloadBytesPerSlot, totalBytes, quantMode, localExpertNum64, sharedExpertNum,
                sharedExpertRankNum, dynamicScalesOutGM, expertTokenNumsOutGM, expertTokenNumsType, magic,
                tpRecvCountsOut, tBuf);
        }
        TileXREpFinalizeExpertTokenNums(expertTokenNumsOutGM, localExpertNum64, expertTokenNumsType);
        if (!useUdmaWindow) {
            sync.SetInnerFlag(static_cast<int32_t>(magic), TileXREp::kEpStepDispatchDrained);
            for (int32_t peer = 0; peer < rankSize; ++peer) {
                sync.WaitRankInnerFlag(static_cast<int32_t>(magic), TileXREp::kEpStepDispatchDrained, peer);
            }
        }
    }
}

extern "C" __global__ __aicore__ void tilexr_ep_dispatch_cross_node_kernel(GM_ADDR commArgsGM, GM_ADDR xGM,
    GM_ADDR expertIdsGM, GM_ADDR scalesGM, GM_ADDR xActiveMaskGM, GM_ADDR expandXOutGM,
    GM_ADDR dynamicScalesOutGM, GM_ADDR expertTokenNumsOutGM, GM_ADDR epRecvCountsOutGM,
    GM_ADDR tpRecvCountsOutGM, GM_ADDR assistInfoForCombineOutGM, GM_ADDR workspaceGM,
    int64_t bs, int64_t h,
    int64_t topK, int64_t moeExpertNum, int64_t dtypeBytes, int64_t maxRoutesPerSrc, int64_t rowBytes,
    int64_t payloadRowBytes,
    int64_t payloadBytesPerSlot, int64_t assistBytesPerSlot, int64_t slotBytes, int64_t totalBytes,
    int64_t expertTokenNumsType, int64_t sharedExpertNum, int64_t sharedExpertRankNum, int64_t quantMode,
    int64_t tpWorldSize, int64_t tpRankId, int64_t magic)
{
    if constexpr (g_coreType == AscendC::AIV) {
        if (AscendC::GetBlockIdx() != 0) {
            return;
        }

        if (commArgsGM == nullptr || xGM == nullptr || expertIdsGM == nullptr || expandXOutGM == nullptr ||
            expertTokenNumsOutGM == nullptr || epRecvCountsOutGM == nullptr || assistInfoForCombineOutGM == nullptr ||
            workspaceGM == nullptr) {
            return;
        }

        auto args = reinterpret_cast<__gm__ TileXR::CommArgs *>(commArgsGM);
        const int32_t rank = args->rank;
        const int32_t rankSize = args->rankSize;
        const int32_t localRankSize = args->localRankSize;
        const int32_t effectiveTpWorldSize = tpWorldSize == 0 ? 1 : static_cast<int32_t>(tpWorldSize);
        const int32_t effectiveTpRankId = static_cast<int32_t>(tpRankId);
        const int32_t expertRankSize = effectiveTpWorldSize > 0 ? rankSize / effectiveTpWorldSize : 0;
        if (rankSize <= 0 || rankSize > TileXR::TILEXR_MAX_RANK_SIZE || rank < 0 || rank >= rankSize ||
            localRankSize <= 0 || localRankSize >= rankSize ||
            effectiveTpWorldSize <= 0 || effectiveTpRankId < 0 || effectiveTpRankId >= effectiveTpWorldSize ||
            expertRankSize <= 0 || expertRankSize * effectiveTpWorldSize != rankSize ||
            (quantMode != kEpQuantModeNone && quantMode != kEpQuantModeStatic &&
                quantMode != kEpQuantModePerTokenDynamic) ||
            (quantMode == kEpQuantModeStatic && scalesGM == nullptr) ||
            (quantMode == kEpQuantModePerTokenDynamic && (dynamicScalesOutGM == nullptr || scalesGM != nullptr)) ||
            !TileXR::UDMARegistryEnabled(args) ||
            !IsValidShape(bs, h, topK, moeExpertNum, dtypeBytes, maxRoutesPerSrc, rowBytes, payloadRowBytes,
                payloadBytesPerSlot, assistBytesPerSlot, slotBytes, totalBytes, rankSize, expertRankSize, sharedExpertNum,
                sharedExpertRankNum, quantMode)) {
            return;
        }

        GM_ADDR shareAddrs[TileXR::TILEXR_MAX_RANK_SIZE];
        if (localRankSize > 1) {
            AscendC::GlobalTensor<GM_ADDR> peerMems;
            peerMems.SetGlobalBuffer(&(args->peerMems[0]), TileXR::TILEXR_MAX_RANK_SIZE);
            for (int32_t peer = 0; peer < rankSize; ++peer) {
                shareAddrs[peer] = peerMems.GetValue(peer);
                if (shareAddrs[peer] == nullptr) {
                    return;
                }
            }
        }

        AscendC::TPipe pipe;
        AscendC::TBuf<AscendC::QuePosition::VECCALC> tBuf;
        AscendC::TBuf<AscendC::QuePosition::VECCALC> udmaWqeBuf;
        pipe.InitBuffer(tBuf, kEpUbBytes);
        pipe.InitBuffer(udmaWqeBuf, TileXR::TILEXR_UDMA_WQE_SCRATCH_BYTES);

        GM_ADDR sendWindow = workspaceGM;
        GM_ADDR recvWindow = workspaceGM + UDMARecvWindowOffset(totalBytes);
        GM_ADDR localIpcWindow = localRankSize > 1 ? shareAddrs[rank] + TileXR::IPC_DATA_OFFSET : nullptr;
        ClearLocalWindow(sendWindow, rankSize, maxRoutesPerSrc, rowBytes, slotBytes, totalBytes, tBuf);
        ClearLocalWindow(recvWindow, rankSize, maxRoutesPerSrc, rowBytes, slotBytes, totalBytes, tBuf);
        if (localRankSize > 1) {
            ClearLocalWindow(localIpcWindow, rankSize, maxRoutesPerSrc, rowBytes, slotBytes, totalBytes, tBuf);
        }

        int64_t dstCounts[TileXR::TILEXR_MAX_RANK_SIZE];
        for (int32_t peer = 0; peer < rankSize; ++peer) {
            dstCounts[peer] = 0;
        }

        const int64_t localExpertNum = TileXREpLocalExpertCount(moeExpertNum, expertRankSize, sharedExpertRankNum);
        if (localExpertNum <= 0) {
            return;
        }
        const int64_t inputRowBytes = h * static_cast<int64_t>(sizeof(uint16_t));
        TileXREpRouteLocalTokens(sendWindow, localIpcWindow, rank, rankSize, expertRankSize, effectiveTpWorldSize,
            effectiveTpRankId, localRankSize, xGM, expertIdsGM, scalesGM, xActiveMaskGM, bs, h, topK, moeExpertNum,
            sharedExpertNum, sharedExpertRankNum, localExpertNum, maxRoutesPerSrc, inputRowBytes, rowBytes,
            payloadRowBytes, slotBytes, payloadBytesPerSlot, quantMode, dstCounts, tBuf);

        for (int32_t dstRank = 0; dstRank < rankSize; ++dstRank) {
            GM_ADDR targetWindow = TileXREpDispatchWriteWindow(sendWindow, localIpcWindow, rank, dstRank,
                localRankSize);
            const int64_t payloadBytes = dstCounts[dstRank] * payloadRowBytes +
                (quantMode == kEpQuantModePerTokenDynamic ?
                    dstCounts[dstRank] * static_cast<int64_t>(sizeof(float)) : 0);
            StoreSlotHeader(targetWindow + SlotOffset(dstRank, slotBytes), static_cast<int32_t>(dstCounts[dstRank]),
                rank, payloadBytes, dstCounts[dstRank] * static_cast<int64_t>(sizeof(TileXREp::EpAssistTuple)),
                magic, tBuf);
        }
        CopyBytesGmToGm(recvWindow + SlotOffset(rank, slotBytes), sendWindow + SlotOffset(rank, slotBytes), tBuf,
            slotBytes);
        TileXREpFlushDispatchSlotHeaders(sendWindow, rankSize, slotBytes, tBuf);
        if (localRankSize > 1) {
            TileXREpFlushDispatchSlotHeaders(localIpcWindow, rankSize, slotBytes, tBuf);
        }

        if (localRankSize <= 1) {
            TileXREpNotifyAllUdmaReady(args, udmaWqeBuf.Get<uint8_t>(),
                sendWindow, rank, rankSize, totalBytes, magic, slotBytes);
            TileXREpWaitAllUdmaReady(workspaceGM, rank, rankSize, totalBytes, magic, tBuf);
        } else {
            TileXREpNotifyUdmaReady(args, udmaWqeBuf.Get<uint8_t>(), sendWindow,
                rank, rankSize, localRankSize, totalBytes, magic, slotBytes, tBuf);
        }

        auto epRecvCountsOut = reinterpret_cast<__gm__ int32_t *>(epRecvCountsOutGM);
        auto tpRecvCountsOut = reinterpret_cast<__gm__ int32_t *>(tpRecvCountsOutGM);
        TileXREpClearExpertTokenNums(expertTokenNumsOutGM, localExpertNum, expertTokenNumsType);

        int64_t outRecord = 0;
        auto localAssistBase = reinterpret_cast<__gm__ TileXREp::EpAssistTuple *>(assistInfoForCombineOutGM);
        GM_ADDR tpPublishWindow = nullptr;
        if (effectiveTpWorldSize > 1 && tpRecvCountsOut != nullptr && localRankSize > 1) {
            tpPublishWindow = localIpcWindow + TileXREpTpWindowOffset(totalBytes);
        }
        for (int32_t expertSrcRank = 0; expertSrcRank < expertRankSize; ++expertSrcRank) {
            const int32_t srcRank = expertSrcRank * effectiveTpWorldSize + effectiveTpRankId;
            const bool sameNodeSource = localRankSize > 1 && srcRank != rank &&
                TileXREpIsSameNodePeer(rank, srcRank, localRankSize);
            GM_ADDR sourceWindow = sameNodeSource ? shareAddrs[srcRank] + TileXR::IPC_DATA_OFFSET : recvWindow;
            const int32_t slotRank = sameNodeSource ? rank : srcRank;
            outRecord = TileXREpDrainSourceWindow(sourceWindow, slotRank, srcRank, slotBytes, payloadBytesPerSlot,
                maxRoutesPerSrc, rowBytes, payloadRowBytes, quantMode, localExpertNum, sharedExpertNum,
                sharedExpertRankNum, expandXOutGM, dynamicScalesOutGM, expertTokenNumsOutGM, expertTokenNumsType,
                magic, epRecvCountsOut, tpRecvCountsOut, localAssistBase, outRecord, tpPublishWindow, rank, tBuf);
        }
        if (tpPublishWindow != nullptr) {
            const int32_t localTpCount = static_cast<int32_t>(outRecord);
            tpRecvCountsOut[tpRankId] = localTpCount;
            AscendC::PipeBarrier<PIPE_ALL>();
            outRecord = TileXREpAppendTpGroupRows(shareAddrs, expandXOutGM, localAssistBase, rank,
                expertRankSize, effectiveTpWorldSize, outRecord, maxRoutesPerSrc, rowBytes, payloadRowBytes,
                slotBytes, payloadBytesPerSlot, totalBytes, quantMode, localExpertNum, sharedExpertNum,
                sharedExpertRankNum, dynamicScalesOutGM, expertTokenNumsOutGM, expertTokenNumsType, magic,
                tpRecvCountsOut, tBuf);
        }
        TileXREpFinalizeExpertTokenNums(expertTokenNumsOutGM, localExpertNum, expertTokenNumsType);
    }
}
