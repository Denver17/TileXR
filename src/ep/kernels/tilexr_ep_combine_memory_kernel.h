/*
 * Memory Combine 按 assist metadata 将 expert 输出送回源 rank，并按 token 累加结果。
 * 外置 kernel 入口调用本实现；TPipe 持有计算与搬运所用的 UB buffer。
 */
#ifndef TILEXR_EP_KERNELS_TILEXR_EP_COMBINE_MEMORY_KERNEL_H
#define TILEXR_EP_KERNELS_TILEXR_EP_COMBINE_MEMORY_KERNEL_H

#include <cstdint>

#include "adv_api/reduce/sum.h"
#include "comm_args.h"
#include "ep_memory_window.h"
#include "kernel_operator.h"
#include "tilexr_data_as_flag.h"
#include "tilexr_ep_mxfp8_quant.h"
#include "tilexr_types.h"

#define FLOAT_OVERFLOW_MODE_CTRL 60

namespace Mc2Kernel {

using namespace AscendC;

constexpr uint32_t UB_ALIGN = 32U;
constexpr uint32_t VECTOR_ALIGN = 256U;
constexpr uint32_t A5_QUANT_VECTOR_ALIGN = 512U;
constexpr uint32_t ASSIST_INFO_WIDTH = TileXREp::kEpMemoryAssistInfoWidth;
constexpr uint32_t RECEIVE_BUFFER_NUM = 2U;
constexpr uint32_t RECEIVE_BUFFER_SINGLE = 1U;
constexpr uint32_t MAX_DISPATCH_BUFFER_NUM = 8U;
constexpr int64_t ACTIVE_MASK_NONE = 0;
constexpr int64_t ACTIVE_MASK_TOKEN = 1;
constexpr int64_t UNQUANT = 0;
constexpr int64_t MXFP8_E5M2_COMM_QUANT = 3;
constexpr int64_t MXFP8_E4M3_COMM_QUANT = 4;

template <AscendC::HardEvent event>
__aicore__ inline void SyncFunc()
{
    AscendC::TEventID eventId = GetTPipePtr()->FetchEventID(event);
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

class TileXRCombineMemoryContext {
public:
    __aicore__ inline void Init(GM_ADDR commArgsGM, int64_t magic)
    {
        args_ = reinterpret_cast<__gm__ TileXR::CommArgs *>(commArgsGM);
        magic_ = magic;
    }

    __aicore__ inline uint32_t Rank() const
    {
        return static_cast<uint32_t>(args_->rank);
    }

    __aicore__ inline uint32_t RankSize() const
    {
        return static_cast<uint32_t>(args_->rankSize);
    }

    __aicore__ inline GM_ADDR CombineWindowBase(uint32_t rank) const
    {
        return TileXREp::TileXREpMemoryRoundDataBase(args_->peerMems[rank], magic_) +
            TileXREp::kEpMemoryStateWindowBytes;
    }

private:
    __gm__ TileXR::CommArgs *args_{nullptr};
    int64_t magic_{0};
};

template <typename ExpandXType, int64_t QuantMode>
class MoeCombineMemory {
public:
    __aicore__ inline void Init(GM_ADDR commArgsGM, GM_ADDR expertOutGM, GM_ADDR assistInfoGM,
        GM_ADDR sendCountsGM, GM_ADDR expertScalesGM, GM_ADDR xActiveMaskGM, GM_ADDR sharedExpertXGM,
        GM_ADDR yOutGM, int64_t bs, int64_t h, int64_t topK, int64_t moeExpertNum,
        int64_t sharedExpertNum, int64_t sharedExpertRankNum, int64_t globalBs,
        int64_t activeMaskType, int64_t quantMode, int64_t magic, TPipe *pipe);
    __aicore__ inline void Process();

private:
    static constexpr bool IsMxfp8 =
        QuantMode == MXFP8_E5M2_COMM_QUANT || QuantMode == MXFP8_E4M3_COMM_QUANT;

    __aicore__ inline void InitInputAndOutput(GM_ADDR expertOutGM, GM_ADDR assistInfoGM,
        GM_ADDR sendCountsGM, GM_ADDR expertScalesGM, GM_ADDR xActiveMaskGM,
        GM_ADDR sharedExpertXGM, GM_ADDR yOutGM);
    __aicore__ inline void InitAttrs(int64_t bs, int64_t h, int64_t topK,
        int64_t moeExpertNum, int64_t sharedExpertNum, int64_t sharedExpertRankNum,
        int64_t activeMaskType);
    __aicore__ inline void ReadSendCount();
    __aicore__ inline void SplitCoreCal();
    __aicore__ inline uint32_t CalcDispatchBufferNum(uint32_t perDispatchBufBytes);
    __aicore__ inline uint32_t CalcReceiveBufferNum() const;
    __aicore__ inline void BuffInit();
    __aicore__ inline void SetWaitTpStatusAndDisPatch();
    __aicore__ inline void ExpertAlltoAllDispatchCopyAdd();
    __aicore__ inline bool IsDoubleGroupRankSize() const;
    __aicore__ inline bool ShouldSendToRankInRound(
        uint32_t logicalRankId, uint32_t roundId) const;
    __aicore__ inline void ExpertAlltoAllDispatchInnerCopyAdd(
        uint32_t toRankId, uint32_t tokenId, uint32_t topkId, uint32_t tokenIndex);
    template <typename Fp8Type>
    __aicore__ inline void QuantMxfp8(
        LocalTensor<uint8_t> quantLocal, LocalTensor<ExpandXType> inputLocal);
    __aicore__ inline void AlltoAllCommBuffInit();
    __aicore__ inline void AlltoAllBuffInitAndMaskCal();
    __aicore__ inline bool CheckPackedFlagRangeArrive(
        GM_ADDR flagBaseAddr, uint16_t blockCount, uint32_t srcStrideBytes);
    __aicore__ inline bool CheckPackedTokenArriveBatch(uint32_t tokenIndex);
    __aicore__ inline bool WaitDispatch(uint32_t tokenIndex);
    __aicore__ inline bool TokenActive(uint32_t tokenIndex);
    __aicore__ inline void ClearPackedTokenFlags(uint32_t tokenIndex, LocalTensor<float> flagTensor);
    __aicore__ inline void ExpertScaleCopy(
        uint32_t beginIndex, uint32_t endIndex, uint32_t tokenPerAivNum);
    __aicore__ inline void ProcessMoeExpert(
        uint32_t tokenIndexOffset, uint32_t topkId, float scaleVal);
    __aicore__ inline void ProcessMoeExpertsLoop(
        uint32_t tokenIndex, uint32_t tokenIndexOffset, uint32_t &scaleIndex);
    __aicore__ inline void ProcessSharedExpertsLoop(
        uint32_t tokenIndexOffset, uint32_t processLen);
    __aicore__ inline void AddSharedExpertX(uint32_t tokenIndex, uint32_t processLen);
    __aicore__ inline void ProcessExpert(uint32_t tokenIndex, uint32_t processLen);
    __aicore__ inline void LocalWindowCopy();

    TPipe *tpipe_{nullptr};
    TileXRCombineMemoryContext context_;

    GlobalTensor<ExpandXType> expandXGM_;
    GlobalTensor<int32_t> assistInfoGM_;
    GlobalTensor<int32_t> sendCountsGM_;
    GlobalTensor<float> expertScalesGM_;
    GlobalTensor<bool> xActiveMaskGM_;
    GlobalTensor<ExpandXType> sharedExpertXGM_;
    GlobalTensor<ExpandXType> expandOutGM_;
    GlobalTensor<ExpandXType> rowTmpGlobal_;
    GM_ADDR epWindowGM_{nullptr};

    uint32_t axisBS_{0};
    uint32_t axisH_{0};
    uint32_t axisK_{0};
    uint32_t aivNum_{0};
    uint32_t epWorldSize_{0};
    uint32_t epRankId_{0};
    uint32_t coreIdx_{0};
    uint32_t sharedExpertNum_{0};
    uint32_t sharedExpertRankNum_{0};
    uint32_t moeExpertPerRankNum_{0};
    uint32_t sendCountNum_{0};
    uint32_t selfSendCnt_{0};
    uint32_t startTokenId_{0};
    uint32_t endTokenId_{0};
    uint32_t sendCntNum_{0};
    uint32_t dispatchBufferNum_{1};
    uint32_t bufferNum_{1};
    uint32_t activeMaskBsCnt_{0};
    uint32_t slotCount_{0};
    uint32_t blockCntPerToken_{0};
    uint32_t rowBytes_{0};
    uint32_t hExpandXAlign32Size_{0};
    uint32_t hFloatAlign32Size_{0};
    uint32_t hFloatAlign256Size_{0};
    uint32_t hAlignWinSize_{0};
    uint32_t compactPayloadBytes_{0};
    uint32_t moeQueueBytes_{0};
    uint32_t quantPayloadBytes_{0};
    uint32_t quantScaleCount_{0};
    uint32_t quantComputeCount_{0};
    uint32_t quantComputeScaleCount_{0};
    uint32_t quantInputBytes_{0};
    uint32_t quantWorkBytes_{0};
    uint32_t expertScaleBeginIdx_{0};
    int64_t activeMaskType_{ACTIVE_MASK_NONE};
    bool hasExpertScalesFlag_{false};
    bool hasSharedExpertX_{false};
    bool isSmallBs_{false};

    TBuf<> calBeginBuf_;
    TBuf<> calEndBuf_;
    TBuf<> readStateBuf_;
    TBuf<> indexCountsBuf_;
    TQue<QuePosition::VECIN, 1> expandXInQueue_;
    TQue<QuePosition::VECOUT, 1> xOutPackageQueue_;
    TBuf<> quantResultBuf_;
    TBuf<> quantWorkBuf_;

    TBuf<> tokenBuf_;
    TBuf<> rowTmpFloatBuf_;
    TBuf<> mulBuf_;
    TBuf<> sumFloatBuf_;
    TQue<QuePosition::VECIN, 1> moeMainSumQueue_;
    TBuf<> dequantScaleBuf_;
    TBuf<> packedCheckFlagBuf_;
    TBuf<> packedCheckReduceBuf_;
    TBuf<> packedClearFlagBuf_;
    TBuf<> activeMaskBuf_;
    TBuf<> expertScalesBuf_;

    LocalTensor<float> rowTmpFloatLocal_;
    LocalTensor<float> mulBufLocal_;
    LocalTensor<float> sumFloatBufLocal_;
    LocalTensor<float> expertScalesLocal_;

};

template <typename ExpandXType, int64_t QuantMode>
__aicore__ inline void MoeCombineMemory<ExpandXType, QuantMode>::InitInputAndOutput(
    GM_ADDR expertOutGM, GM_ADDR assistInfoGM, GM_ADDR sendCountsGM, GM_ADDR expertScalesGM,
    GM_ADDR xActiveMaskGM, GM_ADDR sharedExpertXGM, GM_ADDR yOutGM)
{
    expandXGM_.SetGlobalBuffer(reinterpret_cast<__gm__ ExpandXType *>(expertOutGM));
    assistInfoGM_.SetGlobalBuffer(reinterpret_cast<__gm__ int32_t *>(assistInfoGM));
    sendCountsGM_.SetGlobalBuffer(reinterpret_cast<__gm__ int32_t *>(sendCountsGM));
    expertScalesGM_.SetGlobalBuffer(reinterpret_cast<__gm__ float *>(expertScalesGM));
    xActiveMaskGM_.SetGlobalBuffer(reinterpret_cast<__gm__ bool *>(xActiveMaskGM));
    sharedExpertXGM_.SetGlobalBuffer(reinterpret_cast<__gm__ ExpandXType *>(sharedExpertXGM));
    expandOutGM_.SetGlobalBuffer(reinterpret_cast<__gm__ ExpandXType *>(yOutGM));
    hasExpertScalesFlag_ = expertScalesGM != nullptr;
    hasSharedExpertX_ = sharedExpertXGM != nullptr;
}

template <typename ExpandXType, int64_t QuantMode>
__aicore__ inline void MoeCombineMemory<ExpandXType, QuantMode>::InitAttrs(
    int64_t bs, int64_t h, int64_t topK, int64_t moeExpertNum,
    int64_t sharedExpertNum, int64_t sharedExpertRankNum, int64_t activeMaskType)
{
    axisBS_ = static_cast<uint32_t>(bs);
    axisH_ = static_cast<uint32_t>(h);
    axisK_ = static_cast<uint32_t>(topK);
    sharedExpertNum_ = static_cast<uint32_t>(sharedExpertNum);
    sharedExpertRankNum_ = static_cast<uint32_t>(sharedExpertRankNum);
    activeMaskType_ = activeMaskType;
    slotCount_ = axisK_ + sharedExpertNum_;

    const uint32_t moeRankNum = epWorldSize_ - sharedExpertRankNum_;
    moeExpertPerRankNum_ = static_cast<uint32_t>(moeExpertNum) / moeRankNum;
    sendCountNum_ = epRankId_ < sharedExpertRankNum_ ?
        epWorldSize_ : epWorldSize_ * moeExpertPerRankNum_;

    rowBytes_ = axisH_ * static_cast<uint32_t>(sizeof(ExpandXType));
    hExpandXAlign32Size_ = static_cast<uint32_t>(AlignUp(rowBytes_, UB_ALIGN));
    hFloatAlign32Size_ = static_cast<uint32_t>(AlignUp(axisH_ * sizeof(float), UB_ALIGN));
    hFloatAlign256Size_ = static_cast<uint32_t>(AlignUp(axisH_ * sizeof(float),
        IsMxfp8 ? A5_QUANT_VECTOR_ALIGN : VECTOR_ALIGN));

    uint32_t commDataBytes = rowBytes_;
    if constexpr (IsMxfp8) {
        quantPayloadBytes_ = static_cast<uint32_t>(AlignUp(axisH_, VECTOR_ALIGN));
        quantScaleCount_ = static_cast<uint32_t>(AlignUp(CeilDiv(axisH_, 32U), 2U));
        quantComputeCount_ = quantPayloadBytes_;
        quantComputeScaleCount_ = quantComputeCount_ / 32U;
        quantInputBytes_ = quantComputeCount_ * static_cast<uint32_t>(sizeof(ExpandXType));
        quantWorkBytes_ = static_cast<uint32_t>(AlignUp(
            AlignUp(quantComputeScaleCount_, UB_ALIGN) * sizeof(float) +
                quantComputeScaleCount_ * sizeof(uint16_t), UB_ALIGN));
        commDataBytes = quantPayloadBytes_ + quantScaleCount_;
    }

    blockCntPerToken_ = static_cast<uint32_t>(
        CeilDiv(commDataBytes, TileXR::DATA_AS_FLAG_PAYLOAD_BYTES));
    hAlignWinSize_ = blockCntPerToken_ * TileXR::DATA_AS_FLAG_BLOCK_BYTES;
    compactPayloadBytes_ = blockCntPerToken_ * TileXR::DATA_AS_FLAG_PAYLOAD_BYTES;
    const uint32_t rawPackedDataBytes = static_cast<uint32_t>(
        CeilDiv(rowBytes_, TileXR::DATA_AS_FLAG_PAYLOAD_BYTES) *
        TileXR::DATA_AS_FLAG_PAYLOAD_BYTES);
    moeQueueBytes_ = compactPayloadBytes_ > rawPackedDataBytes ?
        compactPayloadBytes_ : rawPackedDataBytes;
    moeQueueBytes_ = moeQueueBytes_ > hExpandXAlign32Size_ ?
        moeQueueBytes_ : hExpandXAlign32Size_;
    bufferNum_ = CalcReceiveBufferNum();
    isSmallBs_ = axisBS_ < TileXREp::kEpMemorySmallBsThreshold;
}

template <typename ExpandXType, int64_t QuantMode>
__aicore__ inline void MoeCombineMemory<ExpandXType, QuantMode>::Init(
    GM_ADDR commArgsGM, GM_ADDR expertOutGM, GM_ADDR assistInfoGM, GM_ADDR sendCountsGM,
    GM_ADDR expertScalesGM, GM_ADDR xActiveMaskGM, GM_ADDR sharedExpertXGM, GM_ADDR yOutGM,
    int64_t bs, int64_t h, int64_t topK, int64_t moeExpertNum, int64_t sharedExpertNum,
    int64_t sharedExpertRankNum, int64_t globalBs, int64_t activeMaskType,
    int64_t quantMode, int64_t magic, TPipe *pipe)
{
    (void)globalBs;
    (void)quantMode;
    tpipe_ = pipe;
    coreIdx_ = GetBlockIdx();
    aivNum_ = GetBlockNum();
    context_.Init(commArgsGM, magic);
    epRankId_ = context_.Rank();
    epWorldSize_ = context_.RankSize();
    epWindowGM_ = context_.CombineWindowBase(epRankId_);
    InitInputAndOutput(expertOutGM, assistInfoGM, sendCountsGM, expertScalesGM,
        xActiveMaskGM, sharedExpertXGM, yOutGM);
    InitAttrs(bs, h, topK, moeExpertNum, sharedExpertNum, sharedExpertRankNum, activeMaskType);
    ReadSendCount();
    SplitCoreCal();
}

template <typename ExpandXType, int64_t QuantMode>
__aicore__ inline void MoeCombineMemory<ExpandXType, QuantMode>::ReadSendCount()
{
    const uint32_t countIndex = sendCountNum_ - 1U;
    DataCacheCleanAndInvalid<int32_t, CacheLine::SINGLE_CACHE_LINE, DcciDst::CACHELINE_OUT>(
        sendCountsGM_[countIndex]);
    const int32_t count = sendCountsGM_.GetValue(countIndex);
    selfSendCnt_ = count > 0 ? static_cast<uint32_t>(count) : 0U;
}

template <typename ExpandXType, int64_t QuantMode>
__aicore__ inline void MoeCombineMemory<ExpandXType, QuantMode>::SplitCoreCal()
{
    sendCntNum_ = selfSendCnt_ / aivNum_;
    const uint32_t remainder = selfSendCnt_ % aivNum_;
    startTokenId_ = sendCntNum_ * coreIdx_;
    if (coreIdx_ < remainder) {
        sendCntNum_++;
        startTokenId_ += coreIdx_;
    } else {
        startTokenId_ += remainder;
    }
    endTokenId_ = startTokenId_ + sendCntNum_;
}

template <typename ExpandXType, int64_t QuantMode>
__aicore__ inline uint32_t MoeCombineMemory<ExpandXType, QuantMode>::CalcDispatchBufferNum(
    uint32_t perDispatchBufBytes)
{
    tpipe_->InitBuffer(calEndBuf_, UB_ALIGN);
    const uint64_t beginUbAddr = calBeginBuf_.Get<uint8_t>().GetPhyAddr();
    const uint64_t endUbAddr = calEndBuf_.Get<uint8_t>().GetPhyAddr();
    const uint64_t usedUbSize = endUbAddr - beginUbAddr + UB_ALIGN;
    const uint64_t totalUbSize = static_cast<uint64_t>(TileXREp::kEpMemoryFullUbBytes);
    const uint64_t remainUbSize = totalUbSize > usedUbSize ? totalUbSize - usedUbSize : 0U;
    uint32_t bufferNum = static_cast<uint32_t>(remainUbSize / perDispatchBufBytes);
    if (bufferNum == 0U) {
        bufferNum = 1U;
    }
    if (bufferNum > MAX_DISPATCH_BUFFER_NUM) {
        bufferNum = MAX_DISPATCH_BUFFER_NUM;
    }
    return bufferNum;
}

template <typename ExpandXType, int64_t QuantMode>
__aicore__ inline uint32_t MoeCombineMemory<ExpandXType, QuantMode>::CalcReceiveBufferNum() const
{
    const uint64_t maxTokenCountPerCore = CeilDiv(axisBS_, aivNum_);
    const uint64_t mulBufSize = hFloatAlign256Size_;
    const uint64_t checkFlagBlockCount =
        static_cast<uint64_t>(slotCount_) * blockCntPerToken_;
    const uint64_t checkFlagBufSize =
        checkFlagBlockCount * TileXR::DATA_AS_FLAG_ALIGN_BYTES;
    const uint64_t checkReduceBufSize = TileXR::DATA_AS_FLAG_SUM_RESULT_BYTES +
        AlignUp(checkFlagBlockCount * sizeof(float), VECTOR_ALIGN);
    uint64_t fixedBufferSize = 4U * UB_ALIGN + hExpandXAlign32Size_ +
        hFloatAlign32Size_ + mulBufSize + hFloatAlign32Size_ +
        checkFlagBufSize + checkReduceBufSize +
        static_cast<uint64_t>(blockCntPerToken_) * TileXR::DATA_AS_FLAG_FLAG_BYTES +
        AlignUp(static_cast<uint64_t>(axisBS_) * sizeof(bool), UB_ALIGN) +
        AlignUp(maxTokenCountPerCore * sizeof(int32_t), UB_ALIGN);
    if (hasExpertScalesFlag_) {
        fixedBufferSize += AlignUp(maxTokenCountPerCore * axisK_ * sizeof(float), UB_ALIGN);
    }
    if constexpr (IsMxfp8) {
        fixedBufferSize += AlignUp(quantScaleCount_, 128U) * sizeof(float) * 2U;
    }
    const uint64_t doubleBufferSize =
        fixedBufferSize + static_cast<uint64_t>(moeQueueBytes_) * RECEIVE_BUFFER_NUM;
    return doubleBufferSize <= static_cast<uint64_t>(TileXREp::kEpMemoryFullUbBytes) ?
        RECEIVE_BUFFER_NUM : RECEIVE_BUFFER_SINGLE;
}

template <typename ExpandXType, int64_t QuantMode>
__aicore__ inline void MoeCombineMemory<ExpandXType, QuantMode>::BuffInit()
{
    tpipe_->Reset();
    AscendC::SetCtrlSpr<FLOAT_OVERFLOW_MODE_CTRL, FLOAT_OVERFLOW_MODE_CTRL>(0);
    tpipe_->InitBuffer(calBeginBuf_, UB_ALIGN);
    tpipe_->InitBuffer(readStateBuf_, UB_ALIGN);
    if (sendCntNum_ == 0U) {
        tpipe_->InitBuffer(indexCountsBuf_, UB_ALIGN);
    } else {
        tpipe_->InitBuffer(indexCountsBuf_,
            sendCntNum_ * UB_ALIGN);
    }
    if constexpr (IsMxfp8) {
        tpipe_->InitBuffer(quantResultBuf_, compactPayloadBytes_);
        tpipe_->InitBuffer(quantWorkBuf_, quantWorkBytes_);
    }
    const uint32_t inputBufferBytes = IsMxfp8 ? quantInputBytes_ : moeQueueBytes_;
    const uint32_t perDispatchBufBytes = inputBufferBytes + hAlignWinSize_;
    dispatchBufferNum_ = CalcDispatchBufferNum(perDispatchBufBytes);
    tpipe_->InitBuffer(expandXInQueue_, dispatchBufferNum_, inputBufferBytes);
    tpipe_->InitBuffer(xOutPackageQueue_, dispatchBufferNum_, hAlignWinSize_);
}

template <typename ExpandXType, int64_t QuantMode>
template <typename Fp8Type>
__aicore__ inline void MoeCombineMemory<ExpandXType, QuantMode>::QuantMxfp8(
    LocalTensor<uint8_t> quantLocal, LocalTensor<ExpandXType> inputLocal)
{
    __ubuf__ ExpandXType *srcAddr =
        reinterpret_cast<__ubuf__ ExpandXType *>(inputLocal.GetPhyAddr());
    __ubuf__ uint8_t *workAddr =
        reinterpret_cast<__ubuf__ uint8_t *>(quantWorkBuf_.Get<uint8_t>().GetPhyAddr());
    __ubuf__ uint16_t *maxExpAddr = reinterpret_cast<__ubuf__ uint16_t *>(workAddr);
    __ubuf__ uint16_t *halfScaleAddr = reinterpret_cast<__ubuf__ uint16_t *>(
        workAddr + AlignUp(quantComputeScaleCount_, UB_ALIGN) * sizeof(float));
    __ubuf__ int8_t *outAddr = reinterpret_cast<__ubuf__ int8_t *>(quantLocal.GetPhyAddr());
    __ubuf__ uint16_t *scaleAddr = reinterpret_cast<__ubuf__ uint16_t *>(
        quantLocal[quantPayloadBytes_].GetPhyAddr());

    TileXRMxfp8Quant::ComputeMaxExp(srcAddr, maxExpAddr, quantComputeCount_);
    TileXRMxfp8Quant::ComputeScale<Fp8Type>(
        maxExpAddr, scaleAddr, halfScaleAddr, quantComputeScaleCount_);
    TileXRMxfp8Quant::ComputeFp8Data<ExpandXType, Fp8Type,
        RoundMode::CAST_TRUNC, RoundMode::CAST_RINT>(
        srcAddr, halfScaleAddr, outAddr, quantComputeCount_);
}

template <typename ExpandXType, int64_t QuantMode>
__aicore__ inline void
MoeCombineMemory<ExpandXType, QuantMode>::ExpertAlltoAllDispatchInnerCopyAdd(
    uint32_t toRankId, uint32_t tokenId, uint32_t topkId, uint32_t tokenIndex)
{
    const uint64_t slotIndex = static_cast<uint64_t>(tokenId) * slotCount_ + topkId;
    const uint32_t tokenGMOffset = tokenIndex * axisH_;
    GM_ADDR rankGM = context_.CombineWindowBase(toRankId) + slotIndex * hAlignWinSize_;
    const DataCopyPadExtParams<ExpandXType> copyPadParams{false, 0U, 0U, 0U};
    const DataCopyExtParams inputCopyParams{1U, rowBytes_, 0U, 0U, 0U};
    GlobalTensor<float> dstPackedGlobal;
    dstPackedGlobal.SetGlobalBuffer(reinterpret_cast<__gm__ float *>(rankGM));

    LocalTensor<ExpandXType> inputLocal = expandXInQueue_.AllocTensor<ExpandXType>();
    if constexpr (IsMxfp8) {
        Duplicate<uint32_t>(inputLocal.template ReinterpretCast<uint32_t>(), 0U,
            quantInputBytes_ / sizeof(uint32_t));
        SyncFunc<AscendC::HardEvent::V_MTE2>();
    }
    DataCopyPad(inputLocal, expandXGM_[tokenGMOffset], inputCopyParams, copyPadParams);
    expandXInQueue_.EnQue(inputLocal);
    inputLocal = expandXInQueue_.DeQue<ExpandXType>();

    LocalTensor<float> sourceFloat;
    if constexpr (IsMxfp8) {
        LocalTensor<uint8_t> quantLocal = quantResultBuf_.Get<uint8_t>();
        Duplicate<uint32_t>(quantLocal.template ReinterpretCast<uint32_t>(), 0U,
            compactPayloadBytes_ / sizeof(uint32_t));
        PipeBarrier<PIPE_V>();
        if constexpr (QuantMode == MXFP8_E5M2_COMM_QUANT) {
            QuantMxfp8<fp8_e5m2_t>(quantLocal, inputLocal);
        } else {
            QuantMxfp8<fp8_e4m3fn_t>(quantLocal, inputLocal);
        }
        sourceFloat = quantLocal.template ReinterpretCast<float>();
        expandXInQueue_.FreeTensor(inputLocal);
        PipeBarrier<PIPE_V>();
    } else {
        sourceFloat = inputLocal.template ReinterpretCast<float>();
    }

    LocalTensor<ExpandXType> packedLocal = xOutPackageQueue_.AllocTensor<ExpandXType>();
    LocalTensor<float> packedFloat = packedLocal.template ReinterpretCast<float>();
    Duplicate(packedFloat, TileXR::DATA_AS_FLAG_READY_VALUE, hAlignWinSize_ / sizeof(float));
    PipeBarrier<PIPE_V>();
    Copy(packedFloat, sourceFloat, uint64_t(64), static_cast<uint8_t>(blockCntPerToken_),
        {1, 1, 16, 15});
    Copy(packedFloat[64], sourceFloat[64], uint64_t(56),
        static_cast<uint8_t>(blockCntPerToken_), {1, 1, 16, 15});
    xOutPackageQueue_.EnQue(packedLocal);
    packedLocal = xOutPackageQueue_.DeQue<ExpandXType>();
    DataCopy(dstPackedGlobal, packedFloat, hAlignWinSize_ / sizeof(float));
    xOutPackageQueue_.FreeTensor(packedLocal);
    if constexpr (!IsMxfp8) {
        expandXInQueue_.FreeTensor(inputLocal);
    }
}

template <typename ExpandXType, int64_t QuantMode>
__aicore__ inline void
MoeCombineMemory<ExpandXType, QuantMode>::ExpertAlltoAllDispatchCopyAdd()
{
    const bool doubleGroupEnabled =
        !isSmallBs_ && IsDoubleGroupRankSize() && sharedExpertRankNum_ == 0U;
    const uint32_t roundNum = doubleGroupEnabled ? TileXREp::kEpMemoryDoubleGroupRoundNum : 1U;

    if (sendCntNum_ != 0U) {
        LocalTensor<int32_t> assistLocal = indexCountsBuf_.Get<int32_t>();
        constexpr uint32_t localRecordStride = UB_ALIGN / sizeof(int32_t);
        const DataCopyExtParams assistParams{
            static_cast<uint16_t>(sendCntNum_),
            ASSIST_INFO_WIDTH * static_cast<uint32_t>(sizeof(int32_t)),
            (aivNum_ - 1U) * ASSIST_INFO_WIDTH * static_cast<uint32_t>(sizeof(int32_t)),
            0U, 0U};
        const DataCopyPadExtParams<int32_t> assistPadParams{
            true, 0U, static_cast<uint8_t>(localRecordStride - ASSIST_INFO_WIDTH), 0U};
        DataCopyPad(assistLocal, assistInfoGM_[coreIdx_ * ASSIST_INFO_WIDTH],
            assistParams, assistPadParams);
        SyncFunc<AscendC::HardEvent::MTE2_S>();

        uint32_t permStride = 1U;
        if (sendCntNum_ > 2U) {
            permStride = sendCntNum_ / 2U + 1U;
            if (((sendCntNum_ & 1U) == 0U) && ((permStride & 1U) == 0U)) {
                permStride++;
            }
        }
        const uint32_t rankOffset = (epRankId_ * sendCntNum_) / epWorldSize_;
        for (uint32_t roundId = 0U; roundId < roundNum; ++roundId) {
            uint32_t permIdx = rankOffset % sendCntNum_;
            for (uint32_t loop = 0U; loop < sendCntNum_; ++loop) {
                const uint32_t tokenIndex = coreIdx_ + permIdx * aivNum_;
                const uint32_t baseOffset = permIdx * localRecordStride;
                const uint32_t toRankId = static_cast<uint32_t>(assistLocal.GetValue(baseOffset));
                const uint32_t tokenId = static_cast<uint32_t>(assistLocal.GetValue(baseOffset + 1U));
                const uint32_t topkId = static_cast<uint32_t>(assistLocal.GetValue(baseOffset + 2U));
                const bool isCurrentRound = roundNum == 1U ||
                    ShouldSendToRankInRound(toRankId, roundId);
                if (isCurrentRound && toRankId < epWorldSize_ &&
                    tokenId < axisBS_ && topkId < slotCount_) {
                    ExpertAlltoAllDispatchInnerCopyAdd(
                        toRankId, tokenId, topkId, tokenIndex);
                }
                permIdx += permStride;
                if (permIdx >= sendCntNum_) {
                    permIdx -= sendCntNum_;
                }
            }
            if (doubleGroupEnabled && roundId == 0U) {
                PipeBarrier<PIPE_ALL>();
                AscendC::SyncAll<true>();
            }
        }
    } else if (doubleGroupEnabled) {
        PipeBarrier<PIPE_ALL>();
        AscendC::SyncAll<true>();
    }
}

template <typename ExpandXType, int64_t QuantMode>
__aicore__ inline bool
MoeCombineMemory<ExpandXType, QuantMode>::IsDoubleGroupRankSize() const
{
    return epWorldSize_ == TileXREp::kEpMemoryDoubleGroupWorldSize64 ||
        epWorldSize_ == TileXREp::kEpMemoryDoubleGroupWorldSize128;
}

template <typename ExpandXType, int64_t QuantMode>
__aicore__ inline bool
MoeCombineMemory<ExpandXType, QuantMode>::ShouldSendToRankInRound(
    uint32_t logicalRankId, uint32_t roundId) const
{
    const uint32_t localGroupId =
        (epRankId_ / TileXREp::kEpMemoryDoubleGroupStripeRankNum) & 1U;
    const uint32_t targetGroupId =
        (logicalRankId / TileXREp::kEpMemoryDoubleGroupStripeRankNum) & 1U;
    return targetGroupId == (localGroupId ^ roundId);
}

template <typename ExpandXType, int64_t QuantMode>
__aicore__ inline void MoeCombineMemory<ExpandXType, QuantMode>::SetWaitTpStatusAndDisPatch()
{
    PipeBarrier<PIPE_ALL>();
    ExpertAlltoAllDispatchCopyAdd();
    SyncFunc<AscendC::HardEvent::MTE3_S>();
}

template <typename ExpandXType, int64_t QuantMode>
__aicore__ inline void MoeCombineMemory<ExpandXType, QuantMode>::AlltoAllCommBuffInit()
{
    activeMaskBsCnt_ = axisBS_;
    const uint32_t mulBufSize = hFloatAlign256Size_;
    tpipe_->InitBuffer(tokenBuf_, hExpandXAlign32Size_);
    tpipe_->InitBuffer(rowTmpFloatBuf_, hFloatAlign32Size_);
    tpipe_->InitBuffer(mulBuf_, mulBufSize);
    tpipe_->InitBuffer(sumFloatBuf_, hFloatAlign32Size_);
    tpipe_->InitBuffer(moeMainSumQueue_, bufferNum_, moeQueueBytes_);
    if constexpr (IsMxfp8) {
        tpipe_->InitBuffer(dequantScaleBuf_,
            AlignUp(quantScaleCount_, 128U) * sizeof(float) * 2U);
    }

    const uint32_t checkFlagBlockCount = slotCount_ * blockCntPerToken_;
    const uint32_t checkFlagBufSize =
        checkFlagBlockCount * TileXR::DATA_AS_FLAG_ALIGN_BYTES;
    const uint32_t checkReduceWorkBufSize = static_cast<uint32_t>(AlignUp(
        static_cast<uint64_t>(checkFlagBlockCount) * sizeof(float), VECTOR_ALIGN));
    const uint32_t checkReduceBufSize =
        TileXR::DATA_AS_FLAG_SUM_RESULT_BYTES + checkReduceWorkBufSize;
    const uint32_t clearFlagBufSize = static_cast<uint32_t>(AlignUp(
        static_cast<uint64_t>(blockCntPerToken_) * TileXR::DATA_AS_FLAG_FLAG_BYTES,
        UB_ALIGN));
    tpipe_->InitBuffer(packedCheckFlagBuf_, checkFlagBufSize);
    tpipe_->InitBuffer(packedCheckReduceBuf_, checkReduceBufSize);
    tpipe_->InitBuffer(packedClearFlagBuf_, clearFlagBufSize);
    tpipe_->InitBuffer(activeMaskBuf_, AlignUp(
        static_cast<uint64_t>(axisBS_) * sizeof(bool), UB_ALIGN));
}

template <typename ExpandXType, int64_t QuantMode>
__aicore__ inline void
MoeCombineMemory<ExpandXType, QuantMode>::AlltoAllBuffInitAndMaskCal()
{
    tpipe_->Reset();
    AscendC::SetCtrlSpr<FLOAT_OVERFLOW_MODE_CTRL, FLOAT_OVERFLOW_MODE_CTRL>(0);
    AlltoAllCommBuffInit();
    if (activeMaskType_ == ACTIVE_MASK_TOKEN) {
        LocalTensor<bool> maskLocal = activeMaskBuf_.Get<bool>();
        const DataCopyExtParams maskParams{
            1U, axisBS_ * static_cast<uint32_t>(sizeof(bool)), 0U, 0U, 0U};
        const DataCopyPadExtParams<bool> maskPadParams{false, 0U, 0U, 0U};
        DataCopyPad(maskLocal, xActiveMaskGM_, maskParams, maskPadParams);
        SyncFunc<AscendC::HardEvent::MTE2_S>();
    }
}

template <typename ExpandXType, int64_t QuantMode>
__aicore__ inline bool
MoeCombineMemory<ExpandXType, QuantMode>::CheckPackedFlagRangeArrive(
    GM_ADDR flagBaseAddr, uint16_t blockCount, uint32_t srcStrideBytes)
{
    if (blockCount == 0U) {
        return true;
    }
    LocalTensor<float> flagLocal = packedCheckFlagBuf_.Get<float>();
    LocalTensor<float> sumOut = packedCheckReduceBuf_.GetWithOffset<float>(
        TileXR::DATA_AS_FLAG_SUM_RESULT_BYTES / sizeof(float), 0U);
    const uint32_t reduceWorkFloatCount = static_cast<uint32_t>(AlignUp(
        static_cast<uint64_t>(blockCount) * sizeof(float), VECTOR_ALIGN)) / sizeof(float);
    LocalTensor<float> reduceWork = packedCheckReduceBuf_.GetWithOffset<float>(
        reduceWorkFloatCount, TileXR::DATA_AS_FLAG_SUM_RESULT_BYTES);

    GlobalTensor<float> flagGlobal;
    flagGlobal.SetGlobalBuffer(reinterpret_cast<__gm__ float *>(flagBaseAddr));
    const DataCopyParams flagParams{blockCount, 1U,
        static_cast<uint16_t>(srcStrideBytes / TileXR::DATA_AS_FLAG_ALIGN_BYTES), 0U};
    DataCopy(flagLocal, flagGlobal, flagParams);
    SyncFunc<AscendC::HardEvent::MTE2_V>();
    ReduceSum(sumOut, flagLocal, reduceWork, 1U, blockCount, 1U);
    SyncFunc<AscendC::HardEvent::V_S>();
    const bool ready = sumOut.GetValue(0) ==
        TileXR::DATA_AS_FLAG_READY_VALUE * blockCount;
    return ready;
}

template <typename ExpandXType, int64_t QuantMode>
__aicore__ inline bool
MoeCombineMemory<ExpandXType, QuantMode>::CheckPackedTokenArriveBatch(uint32_t tokenIndex)
{
    if (blockCntPerToken_ == 0U) {
        return true;
    }
    const uint64_t tokenOffset =
        static_cast<uint64_t>(tokenIndex) * slotCount_ * hAlignWinSize_;
    GM_ADDR tokenBaseAddr = epWindowGM_ + tokenOffset;
    return CheckPackedFlagRangeArrive(
        tokenBaseAddr + TileXR::DATA_AS_FLAG_FLAG_OFFSET_BYTES,
        static_cast<uint16_t>(slotCount_ * blockCntPerToken_),
        TileXR::DATA_AS_FLAG_PAYLOAD_BYTES);
}

template <typename ExpandXType, int64_t QuantMode>
__aicore__ inline bool MoeCombineMemory<ExpandXType, QuantMode>::WaitDispatch(
    uint32_t tokenIndex)
{
    return CheckPackedTokenArriveBatch(tokenIndex);
}

template <typename ExpandXType, int64_t QuantMode>
__aicore__ inline bool MoeCombineMemory<ExpandXType, QuantMode>::TokenActive(
    uint32_t tokenIndex)
{
    if (activeMaskType_ != ACTIVE_MASK_TOKEN) {
        return true;
    }
    return activeMaskBuf_.Get<bool>().GetValue(tokenIndex);
}

template <typename ExpandXType, int64_t QuantMode>
__aicore__ inline void MoeCombineMemory<ExpandXType, QuantMode>::ClearPackedTokenFlags(
    uint32_t tokenIndex, LocalTensor<float> flagTensor)
{
    if (blockCntPerToken_ == 0U) {
        return;
    }
    const DataCopyExtParams clearFlagParams{
        static_cast<uint16_t>(blockCntPerToken_), TileXR::DATA_AS_FLAG_FLAG_BYTES,
        0U, TileXR::DATA_AS_FLAG_PAYLOAD_BYTES, 0U};
    for (uint32_t slotIndex = 0U; slotIndex < slotCount_; ++slotIndex) {
        const uint64_t slotOffset =
            (static_cast<uint64_t>(tokenIndex) * slotCount_ + slotIndex) * hAlignWinSize_;
        GM_ADDR slotAddr = epWindowGM_ + slotOffset;
        GlobalTensor<float> dstFlagGlobal;
        dstFlagGlobal.SetGlobalBuffer(reinterpret_cast<__gm__ float *>(
            slotAddr + TileXR::DATA_AS_FLAG_FLAG_OFFSET_BYTES));
        DataCopyPad(dstFlagGlobal, flagTensor, clearFlagParams);
    }
}

template <typename ExpandXType, int64_t QuantMode>
__aicore__ inline void MoeCombineMemory<ExpandXType, QuantMode>::ExpertScaleCopy(
    uint32_t beginIndex, uint32_t endIndex, uint32_t tokenPerAivNum)
{
    (void)endIndex;
    expertScaleBeginIdx_ = beginIndex;
    const uint32_t expertScaleCount = tokenPerAivNum * axisK_;
    if (!hasExpertScalesFlag_) {
        return;
    }
    tpipe_->InitBuffer(expertScalesBuf_,
        AlignUp(static_cast<uint64_t>(expertScaleCount) * sizeof(float), UB_ALIGN));
    expertScalesLocal_ = expertScalesBuf_.Get<float>();
    const DataCopyExtParams scaleParams{1U,
        expertScaleCount * static_cast<uint32_t>(sizeof(float)), 0U, 0U, 0U};
    const DataCopyPadExtParams<float> scalePadParams{false, 0U, 0U, 0U};
    DataCopyPad(expertScalesLocal_, expertScalesGM_[beginIndex * axisK_],
        scaleParams, scalePadParams);
    SyncFunc<AscendC::HardEvent::MTE2_S>();
}

template <typename ExpandXType, int64_t QuantMode>
__aicore__ inline void MoeCombineMemory<ExpandXType, QuantMode>::ProcessMoeExpert(
    uint32_t tokenIndexOffset, uint32_t topkId, float scaleVal)
{
    const DataCopyExtParams inputParams{
        static_cast<uint16_t>(blockCntPerToken_), TileXR::DATA_AS_FLAG_PAYLOAD_BYTES,
        TileXR::DATA_AS_FLAG_FLAG_BYTES, 0U, 0U};
    const DataCopyPadExtParams<ExpandXType> inputPadParams{false, 0U, 0U, 0U};
    GM_ADDR slotAddr = epWindowGM_ +
        static_cast<uint64_t>(tokenIndexOffset + topkId) * hAlignWinSize_;
    rowTmpGlobal_.SetGlobalBuffer(reinterpret_cast<__gm__ ExpandXType *>(slotAddr));
    LocalTensor<ExpandXType> inputLocal = moeMainSumQueue_.AllocTensor<ExpandXType>();
    DataCopyPad(inputLocal, rowTmpGlobal_, inputParams, inputPadParams);
    moeMainSumQueue_.EnQue(inputLocal);
    inputLocal = moeMainSumQueue_.DeQue<ExpandXType>();

    if constexpr (IsMxfp8) {
        __ubuf__ uint8_t *tokenAddr =
            reinterpret_cast<__ubuf__ uint8_t *>(inputLocal.GetPhyAddr());
        __ubuf__ fp8_e8m0_t *scaleAddr = reinterpret_cast<__ubuf__ fp8_e8m0_t *>(
            tokenAddr + quantPayloadBytes_);
        __ubuf__ float *scaleWorkAddr = reinterpret_cast<__ubuf__ float *>(
            dequantScaleBuf_.Get<float>().GetPhyAddr());
        __ubuf__ float *sumAddr =
            reinterpret_cast<__ubuf__ float *>(sumFloatBufLocal_.GetPhyAddr());
        if constexpr (QuantMode == MXFP8_E5M2_COMM_QUANT) {
            if (hasExpertScalesFlag_) {
                TileXRMxfp8Quant::DequantizeAndAccumulate<fp8_e5m2_t>(
                    tokenAddr, scaleAddr, scaleWorkAddr, sumAddr,
                    axisH_, quantScaleCount_, scaleVal);
            } else {
                TileXRMxfp8Quant::DequantizeAndAccumulate<fp8_e5m2_t, false>(
                    tokenAddr, scaleAddr, scaleWorkAddr, sumAddr,
                    axisH_, quantScaleCount_, 1.0f);
            }
        } else {
            if (hasExpertScalesFlag_) {
                TileXRMxfp8Quant::DequantizeAndAccumulate<fp8_e4m3fn_t>(
                    tokenAddr, scaleAddr, scaleWorkAddr, sumAddr,
                    axisH_, quantScaleCount_, scaleVal);
            } else {
                TileXRMxfp8Quant::DequantizeAndAccumulate<fp8_e4m3fn_t, false>(
                    tokenAddr, scaleAddr, scaleWorkAddr, sumAddr,
                    axisH_, quantScaleCount_, 1.0f);
            }
        }
    } else {
        Cast(rowTmpFloatLocal_, inputLocal, AscendC::RoundMode::CAST_NONE, axisH_);
        PipeBarrier<PIPE_V>();
        if (hasExpertScalesFlag_) {
            AscendC::Muls(mulBufLocal_, rowTmpFloatLocal_, scaleVal, axisH_);
            PipeBarrier<PIPE_V>();
            AscendC::Add(sumFloatBufLocal_, sumFloatBufLocal_, mulBufLocal_, axisH_);
        } else {
            AscendC::Add(sumFloatBufLocal_, sumFloatBufLocal_, rowTmpFloatLocal_, axisH_);
        }
    }
    moeMainSumQueue_.FreeTensor(inputLocal);
}

template <typename ExpandXType, int64_t QuantMode>
__aicore__ inline void MoeCombineMemory<ExpandXType, QuantMode>::ProcessMoeExpertsLoop(
    uint32_t tokenIndex, uint32_t tokenIndexOffset, uint32_t &scaleIndex)
{
    (void)tokenIndex;
    float scaleVal = 0.0f;
    for (uint32_t topkId = 0U; topkId < axisK_; ++topkId) {
        if (hasExpertScalesFlag_) {
            scaleVal = expertScalesLocal_.GetValue(scaleIndex);
        }
        ProcessMoeExpert(tokenIndexOffset, topkId, scaleVal);
        scaleIndex++;
    }
}

template <typename ExpandXType, int64_t QuantMode>
__aicore__ inline void
MoeCombineMemory<ExpandXType, QuantMode>::ProcessSharedExpertsLoop(
    uint32_t tokenIndexOffset, uint32_t processLen)
{
    const DataCopyExtParams inputParams{
        static_cast<uint16_t>(blockCntPerToken_), TileXR::DATA_AS_FLAG_PAYLOAD_BYTES,
        TileXR::DATA_AS_FLAG_FLAG_BYTES, 0U, 0U};
    const DataCopyPadExtParams<ExpandXType> inputPadParams{false, 0U, 0U, 0U};
    for (uint32_t topkId = axisK_; topkId < slotCount_; ++topkId) {
        GM_ADDR slotAddr = epWindowGM_ +
            static_cast<uint64_t>(tokenIndexOffset + topkId) * hAlignWinSize_;
        rowTmpGlobal_.SetGlobalBuffer(reinterpret_cast<__gm__ ExpandXType *>(slotAddr));
        LocalTensor<ExpandXType> inputLocal = moeMainSumQueue_.AllocTensor<ExpandXType>();
        DataCopyPad(inputLocal, rowTmpGlobal_, inputParams, inputPadParams);
        moeMainSumQueue_.EnQue(inputLocal);
        inputLocal = moeMainSumQueue_.DeQue<ExpandXType>();
        if constexpr (IsMxfp8) {
            __ubuf__ uint8_t *tokenAddr =
                reinterpret_cast<__ubuf__ uint8_t *>(inputLocal.GetPhyAddr());
            __ubuf__ fp8_e8m0_t *scaleAddr = reinterpret_cast<__ubuf__ fp8_e8m0_t *>(
                tokenAddr + quantPayloadBytes_);
            __ubuf__ float *scaleWorkAddr = reinterpret_cast<__ubuf__ float *>(
                dequantScaleBuf_.Get<float>().GetPhyAddr());
            __ubuf__ float *sumAddr =
                reinterpret_cast<__ubuf__ float *>(sumFloatBufLocal_.GetPhyAddr());
            if constexpr (QuantMode == MXFP8_E5M2_COMM_QUANT) {
                TileXRMxfp8Quant::DequantizeAndAccumulate<fp8_e5m2_t>(
                    tokenAddr, scaleAddr, scaleWorkAddr, sumAddr,
                    axisH_, quantScaleCount_, 1.0f);
            } else {
                TileXRMxfp8Quant::DequantizeAndAccumulate<fp8_e4m3fn_t>(
                    tokenAddr, scaleAddr, scaleWorkAddr, sumAddr,
                    axisH_, quantScaleCount_, 1.0f);
            }
        } else {
            Cast(rowTmpFloatLocal_, inputLocal, AscendC::RoundMode::CAST_NONE, processLen);
            PipeBarrier<PIPE_V>();
            AscendC::Add(sumFloatBufLocal_, sumFloatBufLocal_, rowTmpFloatLocal_, processLen);
            PipeBarrier<PIPE_V>();
        }
        moeMainSumQueue_.FreeTensor(inputLocal);
    }
}

template <typename ExpandXType, int64_t QuantMode>
__aicore__ inline void MoeCombineMemory<ExpandXType, QuantMode>::AddSharedExpertX(
    uint32_t tokenIndex, uint32_t processLen)
{
    const DataCopyExtParams inputParams{1U, rowBytes_, 0U, 0U, 0U};
    const DataCopyPadExtParams<ExpandXType> inputPadParams{false, 0U, 0U, 0U};
    LocalTensor<ExpandXType> rowTmpLocal = tokenBuf_.Get<ExpandXType>();
    SyncFunc<AscendC::HardEvent::V_MTE2>();
    DataCopyPad(rowTmpLocal, sharedExpertXGM_[tokenIndex * axisH_],
        inputParams, inputPadParams);
    SyncFunc<AscendC::HardEvent::MTE2_V>();
    Cast(rowTmpFloatLocal_, rowTmpLocal, AscendC::RoundMode::CAST_NONE, processLen);
    PipeBarrier<PIPE_V>();
    AscendC::Add(sumFloatBufLocal_, sumFloatBufLocal_, rowTmpFloatLocal_, processLen);
}

template <typename ExpandXType, int64_t QuantMode>
__aicore__ inline void MoeCombineMemory<ExpandXType, QuantMode>::ProcessExpert(
    uint32_t tokenIndex, uint32_t processLen)
{
    uint32_t scaleIndex = (tokenIndex - expertScaleBeginIdx_) * axisK_;
    SyncFunc<AscendC::HardEvent::MTE3_V>();
    Duplicate(sumFloatBufLocal_, 0.0f, axisH_);
    const uint32_t tokenIndexOffset = tokenIndex * slotCount_;
    ProcessMoeExpertsLoop(tokenIndex, tokenIndexOffset, scaleIndex);
    ProcessSharedExpertsLoop(tokenIndexOffset, processLen);
    if (hasSharedExpertX_) {
        AddSharedExpertX(tokenIndex, processLen);
    }
}

template <typename ExpandXType, int64_t QuantMode>
__aicore__ inline void MoeCombineMemory<ExpandXType, QuantMode>::LocalWindowCopy()
{
    if (activeMaskBsCnt_ == 0U) {
        return;
    }
    uint32_t tokenPerAivNum = activeMaskBsCnt_ / aivNum_;
    const uint32_t remainderToken = activeMaskBsCnt_ % aivNum_;
    uint32_t beginIndex = tokenPerAivNum * coreIdx_;
    if (coreIdx_ < remainderToken) {
        tokenPerAivNum++;
        beginIndex += coreIdx_;
    } else {
        beginIndex += remainderToken;
    }
    if (tokenPerAivNum == 0U) {
        return;
    }
    const uint32_t endIndex = beginIndex + tokenPerAivNum;
    const uint32_t processLen = axisH_;
    rowTmpFloatLocal_ = rowTmpFloatBuf_.Get<float>();
    mulBufLocal_ = mulBuf_.Get<float>();
    sumFloatBufLocal_ = sumFloatBuf_.Get<float>();
    ExpertScaleCopy(beginIndex, endIndex, tokenPerAivNum);

    TBuf<> tokenStatusBuf;
    tpipe_->InitBuffer(tokenStatusBuf,
        AlignUp(static_cast<uint64_t>(tokenPerAivNum) * sizeof(int32_t), UB_ALIGN));
    LocalTensor<int32_t> tokenStatus = tokenStatusBuf.Get<int32_t>();
    Duplicate<int32_t>(tokenStatus, 0, tokenPerAivNum);
    LocalTensor<float> flagTensor = packedClearFlagBuf_.Get<float>();
    Duplicate(flagTensor, 0.0f,
        blockCntPerToken_ * TileXR::DATA_AS_FLAG_FLAG_FLOATS);
    SyncFunc<AscendC::HardEvent::V_S>();
    const DataCopyExtParams outputParams{1U, rowBytes_, 0U, 0U, 0U};

    uint32_t completed = 0U;
    while (completed != tokenPerAivNum) {
        for (uint32_t tokenIndex = beginIndex; tokenIndex < endIndex; ++tokenIndex) {
            const uint32_t localIndex = tokenIndex - beginIndex;
            if (tokenStatus.GetValue(localIndex) == 1) {
                continue;
            }
            const bool tokenActive = TokenActive(tokenIndex);
            if (tokenActive && !WaitDispatch(tokenIndex)) {
                continue;
            }
            completed++;
            tokenStatus.SetValue(localIndex, 1);
            if (tokenActive) {
                ProcessExpert(tokenIndex, processLen);
                ClearPackedTokenFlags(tokenIndex, flagTensor);
            } else {
                Duplicate(sumFloatBufLocal_, 0.0f, processLen);
            }

            PipeBarrier<PIPE_V>();
            LocalTensor<ExpandXType> sumLocal = tokenBuf_.Get<ExpandXType>();
            Cast(sumLocal, sumFloatBufLocal_, AscendC::RoundMode::CAST_RINT, processLen);
            SyncFunc<AscendC::HardEvent::V_MTE3>();
            DataCopyPad(expandOutGM_[tokenIndex * axisH_], sumLocal, outputParams);
        }
    }
}

template <typename ExpandXType, int64_t QuantMode>
__aicore__ inline void MoeCombineMemory<ExpandXType, QuantMode>::Process()
{
    if ASCEND_IS_AIV {
        BuffInit();
        SetWaitTpStatusAndDisPatch();
        PipeBarrier<PIPE_ALL>();
        AlltoAllBuffInitAndMaskCal();
        LocalWindowCopy();
    }
}

template <typename ExpandXType, int64_t QuantMode>
__aicore__ inline void RunCombineMode(GM_ADDR commArgsGM, GM_ADDR expertOutGM,
    GM_ADDR assistInfoGM, GM_ADDR sendCountsGM, GM_ADDR expertScalesGM,
    GM_ADDR xActiveMaskGM, GM_ADDR sharedExpertXGM, GM_ADDR yOutGM,
    int64_t bs, int64_t h, int64_t topK, int64_t moeExpertNum,
    int64_t sharedExpertNum, int64_t sharedExpertRankNum, int64_t globalBs,
    int64_t activeMaskType, int64_t quantMode, int64_t magic)
{
    TPipe pipe;
    MoeCombineMemory<ExpandXType, QuantMode> op;
    op.Init(commArgsGM, expertOutGM, assistInfoGM, sendCountsGM, expertScalesGM,
        xActiveMaskGM, sharedExpertXGM, yOutGM, bs, h, topK, moeExpertNum,
        sharedExpertNum, sharedExpertRankNum, globalBs, activeMaskType, quantMode,
        magic, &pipe);
    op.Process();
}

template <typename ExpandXType>
__aicore__ inline void RunCombine(GM_ADDR commArgsGM, GM_ADDR expertOutGM,
    GM_ADDR assistInfoGM, GM_ADDR sendCountsGM, GM_ADDR expertScalesGM,
    GM_ADDR xActiveMaskGM, GM_ADDR sharedExpertXGM, GM_ADDR yOutGM,
    int64_t bs, int64_t h, int64_t topK, int64_t moeExpertNum,
    int64_t sharedExpertNum, int64_t sharedExpertRankNum, int64_t globalBs,
    int64_t activeMaskType, int64_t quantMode, int64_t magic)
{
    if (quantMode == UNQUANT) {
        RunCombineMode<ExpandXType, UNQUANT>(
            commArgsGM, expertOutGM, assistInfoGM,
            sendCountsGM, expertScalesGM, xActiveMaskGM, sharedExpertXGM, yOutGM,
            bs, h, topK, moeExpertNum, sharedExpertNum, sharedExpertRankNum,
            globalBs, activeMaskType, quantMode, magic);
        return;
    }
    if (quantMode == MXFP8_E5M2_COMM_QUANT) {
        RunCombineMode<ExpandXType, MXFP8_E5M2_COMM_QUANT>(
            commArgsGM, expertOutGM,
            assistInfoGM, sendCountsGM, expertScalesGM, xActiveMaskGM,
            sharedExpertXGM, yOutGM, bs, h, topK, moeExpertNum, sharedExpertNum,
            sharedExpertRankNum, globalBs, activeMaskType, quantMode, magic);
    } else if (quantMode == MXFP8_E4M3_COMM_QUANT) {
        RunCombineMode<ExpandXType, MXFP8_E4M3_COMM_QUANT>(
            commArgsGM, expertOutGM,
            assistInfoGM, sendCountsGM, expertScalesGM, xActiveMaskGM,
            sharedExpertXGM, yOutGM, bs, h, topK, moeExpertNum, sharedExpertNum,
            sharedExpertRankNum, globalBs, activeMaskType, quantMode, magic);
    }
}

} // namespace Mc2Kernel

#endif // TILEXR_EP_KERNELS_TILEXR_EP_COMBINE_MEMORY_KERNEL_H
