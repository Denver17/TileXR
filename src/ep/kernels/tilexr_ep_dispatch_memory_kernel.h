/*
 * 功能：执行 Memory Dispatch 的 token 发送、接收计数和 staging 数据到 output 的复制。
 * 调用关系：普通及 compact AICore 入口实例化 MoeDispatchMemory。compact 模式由最终
 * 计数核申请实际大小的 bundle，发布本地地址并汇聚 rank 错误；output 核读取地址后复制。
 * 资源与生命周期：入口提供 TPipe 和 communicator window；普通模式由调用者提供 output，
 * compact 模式的 output 由 ElasticMemory Pool 持有。本类使用并 reset TPipe 中的 scratch。
 */
#ifndef TILEXR_EP_KERNELS_TILEXR_EP_DISPATCH_MEMORY_KERNEL_H
#define TILEXR_EP_KERNELS_TILEXR_EP_DISPATCH_MEMORY_KERNEL_H

#include <cstdint>

#include "adv_api/reduce/sum.h"
#include "comm_args.h"
#include "ep_memory_window.h"
#include "kernel_operator.h"
#include "tilexr_ep_mxfp8_quant.h"
#include "tilexr_types.h"
#ifdef TILEXR_EP_COMPACT_ENABLED
#include "ep_compact_layout.h"
#include "device/tilexr_elastic_memory_device.h"
#endif

#define FLOAT_OVERFLOW_MODE_CTRL 60

namespace Mc2Kernel {
#ifdef TILEXR_EP_COMPACT_ENABLED
namespace ElasticProfile = TileXR::Elastic::Profile;
#endif
constexpr uint64_t CUMSUM_CAL_OFFSET = TileXREp::kEpMemoryDispatchCumsumOffset;
constexpr uint64_t CUMSUM_FLAG_OFFSET = TileXREp::kEpMemoryDispatchCumsumFlagOffset;
constexpr uint64_t SPLIT_BLOCK_SIZE = 512UL;
constexpr uint64_t SPLIT_BLOCK_COUNT = 128UL;
constexpr int32_t FULL_MESH_MAX_UB_SIZE = 190 * 1024;
constexpr uint32_t SPLIT_BLOCK_DATA_SIZE = 480U;
constexpr uint32_t SPLIT_BLOCK_DATA_COUNT = 120U;
constexpr uint32_t SIZE_ALIGN_256 = 256U;
constexpr uint32_t CUMSUM_MAX_CORE_NUM = TileXREp::kEpMemoryMaxCountCoreNum;
constexpr uint32_t CUMSUM_STATUS_PER_CORE = TileXREp::kEpMemoryCumsumStatusPerCore;
constexpr uint8_t VALID_EVENT_FLAG_NUM = 8U;
constexpr uint8_t LOCAL_COPY_BUFFER_NUM = 2U;
constexpr uint32_t DATA_COPY_MAX_BLOCK_COUNT = 4095U;
constexpr uint8_t UB_ALIGN_DATA_COUNT = 8U;
constexpr uint32_t UB_ALIGN = 32U;
constexpr uint8_t BUFFER_NUM = 2U;
constexpr uint32_t STATE_OFFSET = 32U;
constexpr uint8_t COMBINE_IN_DATA_SIZE = 2U;
constexpr int32_t BITS_PER_BYTE = 8;
constexpr uint32_t VECTOR_COUNT_MIN_ROUTE_NUM = SIZE_ALIGN_256 / sizeof(int32_t);
constexpr uint64_t A5_MTE_STATE_WIN_SIZE = TileXREp::kEpMemoryStateWindowBytes;
constexpr int64_t ACTIVE_MASK_NONE = 0;
constexpr int64_t ACTIVE_MASK_TOKEN = 1;
constexpr int64_t MX_QUANT = 4;

using namespace AscendC;

template <AscendC::HardEvent event>
__aicore__ inline void SyncFunc()
{
    AscendC::TEventID eventId = GetTPipePtr()->FetchEventID(event);
    AscendC::SetFlag<event>(eventId);
    AscendC::WaitFlag<event>(eventId);
}

class TileXRDispatchMemoryContext {
public:
    __aicore__ inline void Init(GM_ADDR commArgsGM, int64_t magic)
    {
        args_ = reinterpret_cast<__gm__ TileXR::CommArgs *>(commArgsGM);
        rank_ = args_->rank;
        rankSize_ = args_->rankSize;
        magic_ = magic;
    }

    __aicore__ inline uint32_t GetEpRankId()
    {
        return static_cast<uint32_t>(rank_);
    }

    __aicore__ inline uint32_t GetEpWorldSize()
    {
        return static_cast<uint32_t>(rankSize_);
    }

    __aicore__ inline GM_ADDR GetStatusDataSpaceGm()
    {
        return GetPeerStateBase(rank_);
    }

    __aicore__ inline GM_ADDR GetWindAddrByRankId(int32_t rankId)
    {
        return GetPeerStateBase(rankId) + A5_MTE_STATE_WIN_SIZE;
    }

    __aicore__ inline GM_ADDR GetWindStateAddrByRankId(int32_t rankId)
    {
        return GetPeerStateBase(rankId);
    }

private:
    __aicore__ inline GM_ADDR GetPeerStateBase(int32_t rankId)
    {
        return TileXREp::TileXREpMemoryRoundDataBase(args_->peerMems[rankId], magic_);
    }

    __gm__ TileXR::CommArgs *args_{nullptr};
    int32_t rank_{0};
    int32_t rankSize_{0};
    int64_t magic_{0};
};

template <typename XType, bool IsMxfp8, bool IsMxfp8E4M3>
class MoeDispatchMemory {
public:
    __aicore__ inline MoeDispatchMemory() {}
    __aicore__ inline void Init(GM_ADDR commArgs, GM_ADDR x, GM_ADDR expertIds, GM_ADDR xActiveMask,
        GM_ADDR expandXOut, GM_ADDR dynamicScalesOut, GM_ADDR expertTokenNumsOut, GM_ADDR sendCountsOut,
        GM_ADDR assistInfoForCombineOut, int64_t bs, int64_t h, int64_t topK, int64_t moeExpertNum,
        int64_t sharedExpertNum, int64_t sharedExpertRankNum, int64_t globalBs,
        int64_t expertTokenNumsType, int64_t activeMaskType, int64_t quantMode,
        int64_t expandXOutDtype, int64_t magic, TPipe *pipe);
    __aicore__ inline void Run();
#ifdef TILEXR_EP_COMPACT_ENABLED
    __aicore__ inline void SetCompactView(TileXR::Elastic::DeviceView view) { compactView_ = view; }
    __aicore__ inline void SetCompactProfiler(ElasticProfile::Recorder* profiler) { compactProfiler_ = profiler; }
#endif

private:
    __aicore__ inline void ExpIdsCopyAndMaskCal();
    __aicore__ inline bool IsDoubleGroupRankSize() const;
    __aicore__ inline void CalSendRound();
    __aicore__ inline void SyncBeforeSendRound(uint32_t curRound);
    __aicore__ inline void TokenActiveMaskCal();
    __aicore__ inline void SetDataStatus();
    __aicore__ inline void SetTilingDataAndCal();
    __aicore__ inline void SendToSharedExpert(TQue<QuePosition::VECIN, 1> inQueue, TBuf<> outBuf);
    __aicore__ inline void SendToMoeExpert(TQue<QuePosition::VECIN, 1> inQueue, TBuf<> outBuf);
    __aicore__ inline void CalcSendTokenBufNum(TBuf<>& outBuf);
    __aicore__ inline void AllToAllDispatchA5(TQue<QuePosition::VECIN, 1> inQueue, TBuf<> outBuf);
    __aicore__ inline void AllToAllDispatch();
    __aicore__ inline void CalCumSum();
    __aicore__ inline void WaitCumSumFlag();
    __aicore__ inline int32_t CalExpertCnt(uint32_t expertId, uint32_t maskCnt);
    __aicore__ inline void CalAndSendCnt();
    __aicore__ inline void CalAndSendCntByRankAndExpert();
    __aicore__ inline void CalAndSendCntByLegacyReduce(
        uint32_t startExpertId, uint32_t endExpertId, uint32_t maskCnt);
    __aicore__ inline void CalAndSendCntByVectorRange(
        uint32_t startExpertId, uint32_t endExpertId, uint32_t sendExpertNum,
        uint32_t maskCnt, uint32_t maskAllocBytes);
    __aicore__ inline void SendExpertCntRange(
        uint32_t startExpertId, uint32_t endExpertId, uint32_t curRound);
    __aicore__ inline void BufferInit();
    __aicore__ inline void WaitDispatchClearStatus();
    __aicore__ inline void GatherSumRecvCnt(LocalTensor<float> &gatherMaskOutTensor,
         LocalTensor<uint32_t> &gatherTmpTensor, LocalTensor<float> &statusSumOutTensor);
    __aicore__ inline void CalRecvAndSetFlag();
    __aicore__ inline void WaitDispatch();
    __aicore__ inline void GetCumSum(LocalTensor<int32_t> &outLocal, uint32_t newAivId);

    __aicore__ inline void LocalWindowCopy();
    __aicore__ inline void SetValidExpertInfo(uint32_t expInfoSize, uint32_t &validNum);
    __aicore__ inline uint32_t CheckDataArriveWithFlag(uint32_t srcExpDataIdx, int32_t beginIdx, int32_t copyCnt);
    __aicore__ inline void CopyInAndOut(LocalTensor<uint8_t> xTmpTensor, GM_ADDR wAddr,
        uint32_t index, uint32_t dstPosition, uint32_t arriveCount, uint32_t dataBufferId,
        bool isDataBufferReused);
    __aicore__ inline void WaitAndFormatOutput(TBuf<> tBuf, uint32_t validNum);
    __aicore__ inline uint32_t GetLocalWindowSrcDataBlockIdx(
        uint32_t srcExpertId, uint32_t localExpertNum);
    __aicore__ inline void ClearLocalWindowDataFlags(
        TBuf<> tBuf, uint32_t validNum, uint32_t localExpertNum);
    __aicore__ inline void SplitToCore(uint32_t curSendCnt, uint32_t curUseAivNum, uint32_t &startTokenId,
                                       uint32_t &endTokenId, uint32_t &sendTokenNum, bool isFront = true);
    __aicore__ inline void FillTriple(LocalTensor<uint8_t> &xOutTensor, uint32_t tokenIndex, uint32_t k);
    __aicore__ inline void CalTokenSendExpertCnt(uint32_t dstExpertId, int32_t calCnt, int32_t &curExpertCnt);
    __aicore__ inline void TokenToExpert(GlobalTensor<uint8_t> dstWinGMTensor, TQue<QuePosition::VECIN, 1> inQueue,
                                        uint32_t srcTokenIndex, uint32_t toExpertIndex);
    __aicore__ inline void TokenToExpertInQuant(GlobalTensor<uint8_t> dstWinGMTensor,
        TQue<QuePosition::VECIN, 1> inQueue, uint32_t srcTokenIndex, uint32_t toExpertIndex);
    template <typename Fp8Type>
    __aicore__ inline void QuantMxfp8(LocalTensor<uint8_t> &outLocal, LocalTensor<XType> &inLocal);
    __aicore__ inline GM_ADDR GetWindAddrByRankId(const int32_t rankId)
    {
        return ctx_.GetWindAddrByRankId(rankId) + winDataSizeOffset_;
    }


    __aicore__ inline GM_ADDR GetWindStateAddrByRankId(const int32_t rankId)
    {
        return ctx_.GetWindStateAddrByRankId(rankId);
    }

#ifdef TILEXR_EP_COMPACT_ENABLED
    __aicore__ inline bool PrepareCompactOutput();
    __aicore__ inline bool WaitCompactOutput();
    TileXR::Elastic::DeviceView compactView_ {};
    ElasticProfile::Recorder* compactProfiler_ = nullptr;
    uint64_t compactMagic_ = 0;
    int32_t compactRows_ = 0;
#endif
    TPipe *tpipe_{nullptr};
    GlobalTensor<XType> xGMTensor_;
    GlobalTensor<int32_t> expertIdsGMTensor_;
    GlobalTensor<int64_t> expertTokenNumsOutGMTensor_;
    GlobalTensor<uint8_t> dynamicScalesOutGMTensor_;
    GlobalTensor<float> windowInstatusFp32Tensor_;
    GlobalTensor<bool> xActiveMaskGMTensor_;
    GlobalTensor<float> selfRankWinInGMTensor_;

    LocalTensor<int32_t> statusTensor_;
    LocalTensor<int32_t> waitStatusTensor_;
    LocalTensor<float> workLocalTensor_;
    LocalTensor<int32_t> validExpertIdsTensor_;
    LocalTensor<int32_t> validBsIndexTensor_;
    LocalTensor<float> statusFp32Tensor_;
    LocalTensor<float> statusCleanFp32Tensor_;
    LocalTensor<int32_t> sendCntTensor_;
    LocalTensor<uint8_t> outTensor_;
    LocalTensor<uint32_t> expertMapTensor_;
    LocalTensor<uint32_t> expertFinishNumTensor_;
    LocalTensor<uint32_t> expertLeftNumTensor_;
    LocalTensor<uint8_t> quantTensor_;
    LocalTensor<float> quantWorkTensor_;

    LocalTensor<float> flagReduceWorkTensor_;
    LocalTensor<float> flagRecvTensor_;

    TBuf<> statusBuf_;
    TBuf<> tokenNumBuf_;
    TBuf<> dstExpBuf_;
    TBuf<> subExpBuf_;
    TBuf<> gatherMaskTBuf_;
    TBuf<> expertIdsBuf_;
    TBuf<> waitStatusBuf_;
    TBuf<> gatherMaskOutBuf_;
    TBuf<> sumCoreBuf_;
    TBuf<> sumLocalBuf_;
    TBuf<> sumContinueBuf_;
    TBuf<> scalarBuf_;
    TBuf<> calBeginBuf_;
    TBuf<> calEndBuf_;
    GM_ADDR expandXOutGM_;
    GM_ADDR assistInfoForCombineOutGM_;
    GM_ADDR sendCountsOutGM_;
    GM_ADDR statusSpaceGM_;
    GM_ADDR windowGM_;
    GM_ADDR recvCntWorkspaceGM_;
    GM_ADDR statusDataSpaceGM_;


    uint32_t syncFlagId_{0};
    uint8_t sendTokenBufNum_{0};
    uint32_t axisBS_{0};
    uint32_t axisMaxBS_{0};
    uint32_t axisH_{0};
    uint32_t axisK_{0};
    uint32_t aivNum_{0};
    uint32_t sharedUsedAivNum_{0};
    uint32_t moeUsedAivNum_{0};
    uint32_t epWorldSize_{0};

    int32_t epRankId_{0};
    uint32_t aivId_{0};
    uint32_t sharedExpertNum_{0};
    uint32_t sharedExpertRankNum_{0};
    uint32_t rankNumPerSharedExpert_{0};
    uint32_t moeExpertNum_{0};
    uint32_t moeExpertRankNum_{0};
    uint32_t moeExpertNumPerRank_{0};
    uint32_t totalExpertNum_{0};
    uint32_t hOutSize_{0};
    uint32_t hOutSizeAlign_{0};
    uint32_t hAlignSize_{0};
    uint32_t inputTensorBytes_{0};
    uint32_t packedPayloadBytes_{0};
    uint32_t quantComputeCount_{0};
    uint32_t quantComputeScaleCount_{0};
    uint32_t startId_;
    uint32_t endId_;
    uint32_t sendNum_;
    uint32_t statusCntAlign_;
    uint32_t statusHistogramOffset_{0};
    uint32_t statusBufSize_{0};
    uint32_t tBufRealSize_{0};
    uint64_t winDataSizeOffset_{0};
    uint64_t expertPerSizeOnWin_{0};
    uint64_t activeMaskBsCnt_{0};
    uint64_t sendToMoeExpTokenCnt_{0};
    bool isTokenMaskFlag_ = false;
    bool isShareExpertRankFlag_ = false;
    bool isSmallBs_ = false;
    bool useAllCoreCount_ = false;
    uint32_t expertTokenNumsType_{1};
    int32_t expertIdsCnt_{0};
    int32_t tokenQuantAlign_{0};
    uint32_t blockCntPerToken_{0};
    uint32_t axisHCommu_{0};
    uint32_t hCommuSize_{0};
    uint32_t scaleOutBytes_{0};
    uint32_t quantWorkBytes_{0};
    uint32_t quantTensorBytes_{0};

    uint32_t expertIdsBufSize_{0};
    uint32_t rscvStatusNum_{0};
    uint32_t startStatusIndex_{0};
    uint32_t endStatusIndex_{0};
    uint32_t recStatusNumPerCore_{0};
    uint32_t aivUsedCumSum_{0};
    uint32_t aivUsedAllToAll_{0};
    uint32_t maxSize_{0};
    uint32_t expertIdsSize_{0};
    uint32_t globalBS_{0};
    uint32_t copyInAxisH_{0};
    uint32_t copyOutAxisH_{0};
    uint64_t totalUbSize_{0};
    uint32_t sendRoundNum_{1U};
    int64_t originalOverflowMode_{0};
    TileXRDispatchMemoryContext ctx_;

    DataCopyParams hCopyParams_;
};

template <typename XType, bool IsMxfp8, bool IsMxfp8E4M3>
__aicore__ inline void MoeDispatchMemory<XType, IsMxfp8, IsMxfp8E4M3>::SetTilingDataAndCal()
{
    copyInAxisH_ = axisH_;
    copyOutAxisH_ = axisH_;
    isShareExpertRankFlag_ = epRankId_ < static_cast<int32_t>(sharedExpertRankNum_);
    if (sharedExpertNum_ > 0) {
        rankNumPerSharedExpert_ = sharedExpertRankNum_ / sharedExpertNum_;
    }
    moeExpertRankNum_ = epWorldSize_ - sharedExpertRankNum_;
    moeExpertNumPerRank_ = moeExpertNum_ / moeExpertRankNum_;
    expertIdsCnt_ = axisBS_ * axisK_;
    hOutSize_ = copyOutAxisH_ * (IsMxfp8 ? sizeof(uint8_t) : sizeof(XType));
    if constexpr (IsMxfp8) {
        scaleOutBytes_ = ((Ceil(axisH_, 32U) + 1U) / 2U) * 2U;
        quantComputeCount_ = Ceil(axisH_, 256U) * 256U;
        quantComputeScaleCount_ = quantComputeCount_ / 32U;
        hAlignSize_ = quantComputeCount_ * sizeof(XType);
        const uint32_t quantPayloadBytes = Ceil(hOutSize_, 256U) * 256U + scaleOutBytes_;
        tokenQuantAlign_ = Ceil(quantPayloadBytes, UB_ALIGN) * UB_ALIGN / sizeof(int32_t);
        quantWorkBytes_ = Ceil(
            Ceil(quantComputeScaleCount_, 32U) * 32U * sizeof(float) +
                quantComputeScaleCount_ * sizeof(uint16_t), UB_ALIGN) * UB_ALIGN;
    } else {
        hAlignSize_ = Ceil(axisH_ * sizeof(XType), UB_ALIGN) * UB_ALIGN;
        tokenQuantAlign_ = hAlignSize_ / sizeof(int32_t);
    }
    hOutSizeAlign_ = tokenQuantAlign_ * sizeof(int32_t) + UB_ALIGN;
    blockCntPerToken_ = Ceil(hOutSizeAlign_, SPLIT_BLOCK_DATA_SIZE);
    packedPayloadBytes_ = blockCntPerToken_ * SPLIT_BLOCK_DATA_SIZE;
    inputTensorBytes_ = IsMxfp8 ? hAlignSize_ : packedPayloadBytes_;
    quantTensorBytes_ = packedPayloadBytes_;
    hCommuSize_ = blockCntPerToken_ * SPLIT_BLOCK_SIZE;
    axisHCommu_ = hCommuSize_;
    expertPerSizeOnWin_ = axisMaxBS_ * hCommuSize_;
    rscvStatusNum_ = isShareExpertRankFlag_ ? epWorldSize_ : epWorldSize_ * moeExpertNumPerRank_;
    totalExpertNum_ = sharedExpertRankNum_ + moeExpertNum_;
    isSmallBs_ = axisBS_ < TileXREp::kEpMemorySmallBsThreshold;
    useAllCoreCount_ = isSmallBs_;
    if (useAllCoreCount_) {
        aivUsedCumSum_ = Ceil(rscvStatusNum_, CUMSUM_STATUS_PER_CORE);
    } else {
        aivUsedCumSum_ = totalExpertNum_ / 16U;
    }
    aivUsedCumSum_ = aivUsedCumSum_ == 0 ? 1 : aivUsedCumSum_;
    aivUsedCumSum_ = aivUsedCumSum_ >= aivNum_ / 2 ? aivNum_ / 2 : aivUsedCumSum_;
    aivUsedCumSum_ = aivUsedCumSum_ >= CUMSUM_MAX_CORE_NUM ? CUMSUM_MAX_CORE_NUM : aivUsedCumSum_;
    aivUsedCumSum_ = aivUsedCumSum_ >= rscvStatusNum_ ? rscvStatusNum_ : aivUsedCumSum_;
    uint32_t countCoreNum = useAllCoreCount_ ? aivNum_ : aivUsedCumSum_;
    uint32_t maxCountPerCore = Ceil(totalExpertNum_, countCoreNum);
    statusCntAlign_ = Ceil(maxCountPerCore, UB_ALIGN_DATA_COUNT) * UB_ALIGN_DATA_COUNT;
    aivUsedAllToAll_ = aivNum_ - aivUsedCumSum_;
    if (sharedExpertRankNum_ != 0U) {
        sharedUsedAivNum_ = aivUsedAllToAll_ * sharedExpertNum_ / (axisK_ + sharedExpertNum_);
        if (sharedUsedAivNum_ == 0) {
            sharedUsedAivNum_ = 1;
        }
    }
    moeUsedAivNum_ = aivUsedAllToAll_ - sharedUsedAivNum_;

    expertIdsSize_ = Ceil(expertIdsCnt_ * sizeof(int32_t), UB_ALIGN) * UB_ALIGN;
    maxSize_ = expertIdsSize_;
    if constexpr (IsMxfp8) {
        if (maxSize_ < quantWorkBytes_) {
            maxSize_ = quantWorkBytes_;
        }
    }
    totalUbSize_ = FULL_MESH_MAX_UB_SIZE;
    statusHistogramOffset_ = statusCntAlign_ * UB_ALIGN_DATA_COUNT;
    const uint32_t histogramBytes =
        Ceil(statusCntAlign_ * sizeof(int32_t), UB_ALIGN) * UB_ALIGN;
    statusBufSize_ = statusCntAlign_ * STATE_OFFSET + histogramBytes;
}

template <typename XType, bool IsMxfp8, bool IsMxfp8E4M3>
__aicore__ inline void MoeDispatchMemory<XType, IsMxfp8, IsMxfp8E4M3>::SetDataStatus()
{
    statusDataSpaceGM_ = ctx_.GetStatusDataSpaceGm();
    const uint64_t hSizeAlignCombine =
        Ceil(axisH_ * COMBINE_IN_DATA_SIZE, SPLIT_BLOCK_DATA_SIZE) * SPLIT_BLOCK_SIZE;
    winDataSizeOffset_ = axisMaxBS_ * (axisK_ + sharedExpertNum_) * hSizeAlignCombine;
}

template <typename XType, bool IsMxfp8, bool IsMxfp8E4M3>
__aicore__ inline void MoeDispatchMemory<XType, IsMxfp8, IsMxfp8E4M3>::Init(GM_ADDR commArgs, GM_ADDR x,
    GM_ADDR expertIds, GM_ADDR xActiveMask, GM_ADDR expandXOut, GM_ADDR dynamicScalesOut,
    GM_ADDR expertTokenNumsOut,
    GM_ADDR sendCountsOut, GM_ADDR assistInfoForCombineOut, int64_t bs, int64_t h,
    int64_t topK, int64_t moeExpertNum, int64_t sharedExpertNum,
    int64_t sharedExpertRankNum, int64_t globalBs,
    int64_t expertTokenNumsType, int64_t activeMaskType, int64_t quantMode,
    int64_t expandXOutDtype, int64_t magic, TPipe *pipe)
{
    originalOverflowMode_ = AscendC::GetCtrlSpr<FLOAT_OVERFLOW_MODE_CTRL, FLOAT_OVERFLOW_MODE_CTRL>();
    AscendC::SetCtrlSpr<FLOAT_OVERFLOW_MODE_CTRL, FLOAT_OVERFLOW_MODE_CTRL>(0);
    tpipe_ = pipe;
#ifdef TILEXR_EP_COMPACT_ENABLED
    compactMagic_ = static_cast<uint64_t>(magic);
#endif
    (void)quantMode;
    (void)expandXOutDtype;
    tpipe_->InitBuffer(calBeginBuf_, UB_ALIGN);
    aivId_ = GetBlockIdx();
    ctx_.Init(commArgs, magic);
    epRankId_ = static_cast<int32_t>(ctx_.GetEpRankId());
    epWorldSize_ = ctx_.GetEpWorldSize();
    axisBS_ = static_cast<uint32_t>(bs);
    axisH_ = static_cast<uint32_t>(h);
    axisK_ = static_cast<uint32_t>(topK);
    moeExpertNum_ = static_cast<uint32_t>(moeExpertNum);
    sharedExpertNum_ = static_cast<uint32_t>(sharedExpertNum);
    sharedExpertRankNum_ = static_cast<uint32_t>(sharedExpertRankNum);
    globalBS_ = static_cast<uint32_t>(globalBs);
    expertTokenNumsType_ = static_cast<uint32_t>(expertTokenNumsType);
    aivNum_ = GetBlockNum();
    axisMaxBS_ = globalBS_ / epWorldSize_;
    isTokenMaskFlag_ = activeMaskType == ACTIVE_MASK_TOKEN;

    xGMTensor_.SetGlobalBuffer(reinterpret_cast<__gm__ XType *>(x));
    xActiveMaskGMTensor_.SetGlobalBuffer(reinterpret_cast<__gm__ bool *>(xActiveMask));
    expertIdsGMTensor_.SetGlobalBuffer(reinterpret_cast<__gm__ int32_t *>(expertIds));
    expertTokenNumsOutGMTensor_.SetGlobalBuffer(reinterpret_cast<__gm__ int64_t *>(expertTokenNumsOut));
    dynamicScalesOutGMTensor_.SetGlobalBuffer(reinterpret_cast<__gm__ uint8_t *>(dynamicScalesOut));

    SetTilingDataAndCal();
    const uint64_t workspaceBytes = Ceil(
        static_cast<uint64_t>(aivNum_) * rscvStatusNum_ * sizeof(int32_t),
        static_cast<uint64_t>(UB_ALIGN)) * UB_ALIGN;
    SetDataStatus();

    expandXOutGM_ = expandXOut;
    assistInfoForCombineOutGM_ = assistInfoForCombineOut;
    sendCountsOutGM_ = sendCountsOut;
    recvCntWorkspaceGM_ = statusDataSpaceGM_ + TileXREp::kEpMemoryRecvCountWorkspaceEnd - workspaceBytes;
    statusSpaceGM_ = GetWindStateAddrByRankId(epRankId_);
    windowInstatusFp32Tensor_.SetGlobalBuffer(reinterpret_cast<__gm__ float *>(statusSpaceGM_));
    selfRankWinInGMTensor_.SetGlobalBuffer(reinterpret_cast<__gm__ float *>(statusDataSpaceGM_));
    windowGM_ = GetWindAddrByRankId(epRankId_);
    hCopyParams_ = {1U, static_cast<uint32_t>(copyInAxisH_ * sizeof(XType)), 0U, 0U};
}

template <typename XType, bool IsMxfp8, bool IsMxfp8E4M3>
__aicore__ inline void MoeDispatchMemory<XType, IsMxfp8, IsMxfp8E4M3>::FillTriple(
    LocalTensor<uint8_t> &xOutTensor, uint32_t tokenIndex, uint32_t k)
{
    LocalTensor<int32_t> xOutTint32 = xOutTensor.template ReinterpretCast<int32_t>();
    xOutTint32(tokenQuantAlign_) = epRankId_;
    xOutTint32(tokenQuantAlign_ + 1) = tokenIndex;
    xOutTint32(tokenQuantAlign_ + 2) = k;
}

template <typename XType, bool IsMxfp8, bool IsMxfp8E4M3>
template <typename Fp8Type>
__aicore__ inline void MoeDispatchMemory<XType, IsMxfp8, IsMxfp8E4M3>::QuantMxfp8(
    LocalTensor<uint8_t> &outLocal, LocalTensor<XType> &inLocal)
{
    __ubuf__ XType *srcAddr = reinterpret_cast<__ubuf__ XType *>(inLocal.GetPhyAddr());
    __ubuf__ uint16_t *maxExpAddr = reinterpret_cast<__ubuf__ uint16_t *>(quantWorkTensor_.GetPhyAddr());
    __ubuf__ uint16_t *halfScaleAddr = reinterpret_cast<__ubuf__ uint16_t *>(
        quantWorkTensor_[Ceil(quantComputeScaleCount_, 32U) * 32U].GetPhyAddr());
    __ubuf__ int8_t *outAddr = reinterpret_cast<__ubuf__ int8_t *>(outLocal.GetPhyAddr());
    __ubuf__ uint16_t *scaleAddr = reinterpret_cast<__ubuf__ uint16_t *>(
        outLocal[Ceil(axisH_, 256U) * 256U].GetPhyAddr());

    TileXRMxfp8Quant::ComputeMaxExp(srcAddr, maxExpAddr, quantComputeCount_);
    TileXRMxfp8Quant::ComputeScale<Fp8Type>(
        maxExpAddr, scaleAddr, halfScaleAddr, quantComputeScaleCount_);
    TileXRMxfp8Quant::ComputeFp8Data<XType, Fp8Type, RoundMode::CAST_TRUNC, RoundMode::CAST_RINT>(
        srcAddr, halfScaleAddr, outAddr, quantComputeCount_);
}

template <typename XType, bool IsMxfp8, bool IsMxfp8E4M3>
__aicore__ inline void MoeDispatchMemory<XType, IsMxfp8, IsMxfp8E4M3>::TokenToExpertInQuant(
    GlobalTensor<uint8_t> dstWinGMTensor, TQue<QuePosition::VECIN, 1> inQueue,
    uint32_t srcTokenIndex, uint32_t toExpertIndex)
{
    DataCopyPadParams copyPadParams{true, 0U, 0U, 0U};
    LocalTensor<XType> xInTensor = inQueue.AllocTensor<XType>();
    DataCopyPad(xInTensor, xGMTensor_[srcTokenIndex * axisH_], hCopyParams_, copyPadParams);
    inQueue.EnQue(xInTensor);
    xInTensor = inQueue.DeQue<XType>();
    if constexpr (IsMxfp8E4M3) {
        QuantMxfp8<fp8_e4m3fn_t>(quantTensor_, xInTensor);
    } else {
        QuantMxfp8<fp8_e5m2_t>(quantTensor_, xInTensor);
    }
    inQueue.FreeTensor<XType>(xInTensor);
    SyncFunc<AscendC::HardEvent::V_S>();
    FillTriple(quantTensor_, srcTokenIndex, toExpertIndex);
    SyncFunc<AscendC::HardEvent::S_V>();
    AscendC::WaitFlag<AscendC::HardEvent::MTE3_V>(syncFlagId_ % sendTokenBufNum_);
    LocalTensor<int32_t> quantTensorInt32 = quantTensor_.template ReinterpretCast<int32_t>();
    LocalTensor<int32_t> outTensorInt32 =
        outTensor_[(syncFlagId_ % sendTokenBufNum_) * axisHCommu_].template ReinterpretCast<int32_t>();
    Copy(outTensorInt32, quantTensorInt32, uint64_t(64), uint8_t(blockCntPerToken_), {1, 1, 16, 15});
    Copy(outTensorInt32[64], quantTensorInt32[64], uint64_t(56),
        uint8_t(blockCntPerToken_), {1, 1, 16, 15});
    AscendC::SetFlag<AscendC::HardEvent::V_MTE3>(syncFlagId_ % sendTokenBufNum_);
    AscendC::WaitFlag<AscendC::HardEvent::V_MTE3>(syncFlagId_ % sendTokenBufNum_);
    DataCopy(dstWinGMTensor, outTensor_[(syncFlagId_ % sendTokenBufNum_) * axisHCommu_], axisHCommu_);
    AscendC::SetFlag<AscendC::HardEvent::MTE3_V>(syncFlagId_ % sendTokenBufNum_);
    ++syncFlagId_;
}

template <typename XType, bool IsMxfp8, bool IsMxfp8E4M3>
__aicore__ inline void MoeDispatchMemory<XType, IsMxfp8, IsMxfp8E4M3>::TokenToExpert(
    GlobalTensor<uint8_t> dstWinGMTensor, TQue<QuePosition::VECIN, 1> inQueue,
    uint32_t srcTokenIndex, uint32_t toExpertIndex)
{
    DataCopyPadParams copyPadParams{false, 0U, 0U, 0U};
    LocalTensor<XType> xInTensor = inQueue.AllocTensor<XType>();
    DataCopyPad(xInTensor, xGMTensor_[srcTokenIndex * axisH_], hCopyParams_, copyPadParams);
    inQueue.EnQue(xInTensor);
    xInTensor = inQueue.DeQue<XType>();
    SyncFunc<AscendC::HardEvent::MTE2_S>();
    LocalTensor<uint8_t> xInTensorBytes = xInTensor.template ReinterpretCast<uint8_t>();
    FillTriple(xInTensorBytes, srcTokenIndex, toExpertIndex);
    SyncFunc<AscendC::HardEvent::S_V>();
    AscendC::WaitFlag<AscendC::HardEvent::MTE3_V>(syncFlagId_ % sendTokenBufNum_);
    LocalTensor<int32_t> xInTensorInt32 = xInTensorBytes.template ReinterpretCast<int32_t>();
    LocalTensor<int32_t> outTensorInt32 =
        (outTensor_[(syncFlagId_ % sendTokenBufNum_) * axisHCommu_]).template ReinterpretCast<int32_t>();

    Copy(outTensorInt32, xInTensorInt32, uint64_t(64), uint8_t(blockCntPerToken_), {1, 1, 16, 15});

    Copy(outTensorInt32[64], xInTensorInt32[64], uint64_t(56), uint8_t(blockCntPerToken_), {1, 1, 16, 15});
    inQueue.FreeTensor<XType>(xInTensor);
    AscendC::SetFlag<AscendC::HardEvent::V_MTE3>(syncFlagId_ % sendTokenBufNum_);
    AscendC::WaitFlag<AscendC::HardEvent::V_MTE3>(syncFlagId_ % sendTokenBufNum_);
    DataCopy(dstWinGMTensor, outTensor_[(syncFlagId_ % sendTokenBufNum_) * axisHCommu_], axisHCommu_);
    AscendC::SetFlag<AscendC::HardEvent::MTE3_V>(syncFlagId_ % sendTokenBufNum_);
    syncFlagId_ ++;
}

template <typename XType, bool IsMxfp8, bool IsMxfp8E4M3>
__aicore__ inline void MoeDispatchMemory<XType, IsMxfp8, IsMxfp8E4M3>::SplitToCore(
    uint32_t curSendCnt, uint32_t curUseAivNum, uint32_t &startTokenId,
    uint32_t &endTokenId, uint32_t &sendTokenNum, bool isFront)

{
    sendTokenNum = curSendCnt / curUseAivNum;
    uint32_t remainderTokenNum = curSendCnt % curUseAivNum;
    uint32_t newAivId;
    if (isFront) {
        newAivId = aivId_;
    } else if (aivId_ >= aivUsedAllToAll_) {
        newAivId = aivId_ - aivUsedAllToAll_;
    } else {
        newAivId = aivId_ - moeUsedAivNum_;
    }
    startTokenId = sendTokenNum * newAivId;
    if (newAivId < remainderTokenNum) {
        sendTokenNum += 1;
        startTokenId += newAivId;
    } else {
        startTokenId += remainderTokenNum;
    }
    endTokenId = startTokenId + sendTokenNum;
}

template <typename XType, bool IsMxfp8, bool IsMxfp8E4M3>
__aicore__ inline void MoeDispatchMemory<XType, IsMxfp8, IsMxfp8E4M3>::SendToSharedExpert(TQue<QuePosition::VECIN, 1> inQueue, TBuf<> outBuf)
{

    LocalTensor<float> outTensorFp32 = outBuf.Get<float>();
    Duplicate<float>(outTensorFp32, float(1), hCommuSize_ * sendTokenBufNum_ / sizeof(float));
    PipeBarrier<PIPE_V>();

    uint32_t startTokenId, endTokenId, sendTokenNum;
    uint32_t curSendCnt = activeMaskBsCnt_ * sharedExpertNum_;
    SplitToCore(curSendCnt, sharedUsedAivNum_, startTokenId, endTokenId, sendTokenNum, false);
    if (startTokenId >= curSendCnt) {return;}

    GlobalTensor<uint8_t> dstWinGMTensor;
    uint32_t idInSharedGroup = epRankId_ % rankNumPerSharedExpert_;
    syncFlagId_ = 0;
    for (int i = 0; i < sendTokenBufNum_; i ++) {
        AscendC::SetFlag<AscendC::HardEvent::MTE3_V>(i % sendTokenBufNum_);
    }
    for (uint32_t virtualTokenIndex = startTokenId; virtualTokenIndex < endTokenId; ++virtualTokenIndex) {
        const uint32_t activeTokenIndex = virtualTokenIndex % activeMaskBsCnt_;
        uint32_t sendTokenIndex = isTokenMaskFlag_ ?
            static_cast<uint32_t>(validBsIndexTensor_.GetValue(activeTokenIndex)) :
            activeTokenIndex;
        uint32_t toSharedExpertIndex = virtualTokenIndex / activeMaskBsCnt_;
        int32_t toRankId = idInSharedGroup + toSharedExpertIndex * rankNumPerSharedExpert_;
        dstWinGMTensor.SetGlobalBuffer((__gm__ uint8_t *)(uint64_t(GetWindAddrByRankId(toRankId))
            + expertPerSizeOnWin_ * epRankId_ + sendTokenIndex * hCommuSize_));
        if constexpr (IsMxfp8) {
            TokenToExpertInQuant(dstWinGMTensor, inQueue, sendTokenIndex, axisK_ + toSharedExpertIndex);
        } else {
            TokenToExpert(dstWinGMTensor, inQueue, sendTokenIndex, axisK_ + toSharedExpertIndex);
        }
    }
    for (int i = 0; i < sendTokenBufNum_; i ++) {
        AscendC::WaitFlag<AscendC::HardEvent::MTE3_V>(i % sendTokenBufNum_);
    }
}

template <typename XType, bool IsMxfp8, bool IsMxfp8E4M3>
__aicore__ inline void MoeDispatchMemory<XType, IsMxfp8, IsMxfp8E4M3>::SendToMoeExpert(TQue<QuePosition::VECIN, 1> inQueue,
    TBuf<> outBuf)
{

    LocalTensor<float> outTensorFp32 = outBuf.Get<float>();
    Duplicate<float>(outTensorFp32, float(1), hCommuSize_ * sendTokenBufNum_ / sizeof(float));
    uint32_t validTokenNum = isTokenMaskFlag_ ? (activeMaskBsCnt_ * axisK_) : expertIdsCnt_;
    GlobalTensor<uint8_t> dstWinGMTensor;

    int32_t dstTokenIdx = 0;
    syncFlagId_ = 0;

    for (int i = 0; i < sendTokenBufNum_; i ++) {
        AscendC::SetFlag<AscendC::HardEvent::MTE3_V>(i % sendTokenBufNum_);
    }

    // // Rotate the global route order per rank while preserving the original core partition.
    // const uint32_t routeOffset = static_cast<uint32_t>(epRankId_) % validTokenNum;
    // for (uint32_t position = aivId_; position < validTokenNum; position += moeUsedAivNum_) {
    //     uint32_t index = position + routeOffset;
    //     if (index >= validTokenNum) {
    //         index -= validTokenNum;
    //     }
    for (uint32_t curRound = 0U; curRound < sendRoundNum_; ++curRound) {
        SyncBeforeSendRound(curRound);
        for (int32_t index = aivId_; index < validTokenNum; index += moeUsedAivNum_) {
            const uint32_t activeTokenIndex = index / axisK_;
            int32_t tokenId = isTokenMaskFlag_ ?
                static_cast<int32_t>(validBsIndexTensor_.GetValue(activeTokenIndex)) :
                static_cast<int32_t>(activeTokenIndex);
            int32_t topKId = index % axisK_;
            int32_t expertId = validExpertIdsTensor_(index);
            if (expertId >= moeExpertNum_ || expertId < 0) {
                continue;
            }
            int32_t toRankId = expertId / moeExpertNumPerRank_ + sharedExpertRankNum_;
            const uint32_t targetGroupId = (static_cast<uint32_t>(toRankId) /
                TileXREp::kEpMemoryDoubleGroupStripeRankNum) & 1U;
            const uint32_t localGroupId = (static_cast<uint32_t>(epRankId_) /
                TileXREp::kEpMemoryDoubleGroupStripeRankNum) & 1U;
            if (sendRoundNum_ > 1U &&
                targetGroupId != (localGroupId ^ curRound)) {
                continue;
            }
            CalTokenSendExpertCnt(expertId, index, dstTokenIdx);

            dstWinGMTensor.SetGlobalBuffer((__gm__ uint8_t *)(uint64_t(GetWindAddrByRankId(toRankId))
                + expertPerSizeOnWin_ * ((epRankId_ + toRankId) % epWorldSize_ * moeExpertNumPerRank_
                + expertId % moeExpertNumPerRank_) + dstTokenIdx * hCommuSize_));
            if constexpr (IsMxfp8) {
                TokenToExpertInQuant(dstWinGMTensor, inQueue, tokenId, topKId);
            } else {
                TokenToExpert(dstWinGMTensor, inQueue, tokenId, topKId);
            }
        }
    }

    for (int i = 0; i < sendTokenBufNum_; i ++) {
        AscendC::WaitFlag<AscendC::HardEvent::MTE3_V>(i % sendTokenBufNum_);
    }
}

template <typename XType, bool IsMxfp8, bool IsMxfp8E4M3>
__aicore__ inline void MoeDispatchMemory<XType, IsMxfp8, IsMxfp8E4M3>::CalcSendTokenBufNum(TBuf<>& outBuf)
{
    tpipe_->InitBuffer(calEndBuf_, UB_ALIGN);
    uint64_t beiginUbAddr = (calBeginBuf_.Get<uint8_t>()).GetPhyAddr();
    uint64_t endUbAddr = (calEndBuf_.Get<uint8_t>()).GetPhyAddr();
    const uint64_t usedUbSize = endUbAddr - beiginUbAddr + UB_ALIGN;
    const uint64_t remainUbSize = totalUbSize_ > usedUbSize ? totalUbSize_ - usedUbSize : 0U;
    uint64_t sendTokenBufNum = remainUbSize / hCommuSize_;
    if (sendTokenBufNum > VALID_EVENT_FLAG_NUM) {
        sendTokenBufNum = VALID_EVENT_FLAG_NUM;
    }
    sendTokenBufNum_ = static_cast<uint8_t>(sendTokenBufNum);
    if (sendTokenBufNum_ == 0) { return; }


    tpipe_->InitBuffer(outBuf, hCommuSize_ * sendTokenBufNum_);
    outTensor_ = outBuf.Get<uint8_t>();
}

template <typename XType, bool IsMxfp8, bool IsMxfp8E4M3>
__aicore__ inline void MoeDispatchMemory<XType, IsMxfp8, IsMxfp8E4M3>::AllToAllDispatch()
{
    TQue<QuePosition::VECIN, 1> inQueue;
    TBuf<> outBuf, inQueueCleanBuf, calTempBuf;
    TBuf<> quantBuf, quantWorkBuf, quantAuxBuf;
    expertIdsBufSize_ = Ceil(expertIdsCnt_ * sizeof(int32_t), SIZE_ALIGN_256) * SIZE_ALIGN_256;
    AscendC::TBufPool<AscendC::TPosition::VECIN> tbufPool0, tbufPool1;
    tpipe_->InitBufPool(tbufPool0, BUFFER_NUM * inputTensorBytes_);
    tpipe_->InitBufPool(tbufPool1, BUFFER_NUM * hAlignSize_, tbufPool0);
    tbufPool0.InitBuffer(inQueue, BUFFER_NUM, inputTensorBytes_);
    tbufPool1.InitBuffer(inQueueCleanBuf, BUFFER_NUM * hAlignSize_);
    if constexpr (IsMxfp8) {
        LocalTensor<uint8_t> cleanTensor = inQueueCleanBuf.Get<uint8_t>();
        Duplicate<uint8_t>(cleanTensor, static_cast<uint8_t>(0U), BUFFER_NUM * hAlignSize_);
    }

    uint32_t calTokenIdxBuffSize = Ceil(axisBS_ * axisK_ * sizeof(int32_t), UB_ALIGN) * UB_ALIGN;
    tpipe_->InitBuffer(expertIdsBuf_, expertIdsBufSize_);
    if (isTokenMaskFlag_) {
        tpipe_->InitBuffer(gatherMaskTBuf_, expertIdsBufSize_);
    }
    if constexpr (IsMxfp8) {
        tpipe_->InitBuffer(quantBuf, quantTensorBytes_);
        tpipe_->InitBuffer(quantWorkBuf, maxSize_);
        tpipe_->InitBuffer(quantAuxBuf, maxSize_);
        quantTensor_ = quantBuf.Get<uint8_t>();
        Duplicate<uint32_t>(quantTensor_.ReinterpretCast<uint32_t>(), 0U,
            quantTensorBytes_ / sizeof(uint32_t));
        PipeBarrier<PIPE_V>();
        quantWorkTensor_ = quantWorkBuf.Get<float>();
        dstExpBuf_ = quantWorkBuf;
        subExpBuf_ = quantAuxBuf;
    } else if (isTokenMaskFlag_) {
        tpipe_->InitBuffer(dstExpBuf_, maxSize_);
        tpipe_->InitBuffer(subExpBuf_, maxSize_);
    } else {
        tpipe_->InitBuffer(dstExpBuf_, calTokenIdxBuffSize);
        tpipe_->InitBuffer(subExpBuf_, calTokenIdxBuffSize);
    }
    tpipe_->InitBuffer(calTempBuf, calTokenIdxBuffSize);
    workLocalTensor_ = calTempBuf.Get<float>();
    ExpIdsCopyAndMaskCal();
    CalSendRound();
    if (useAllCoreCount_) {
        tpipe_->InitBuffer(statusBuf_, statusBufSize_);
        statusTensor_ = statusBuf_.Get<int32_t>();
        CalAndSendCnt();
    }
    if (activeMaskBsCnt_ == 0) {
        for (uint32_t curRound = 0U; curRound < sendRoundNum_; ++curRound) {
            SyncBeforeSendRound(curRound);
        }
        return;
    }
    AllToAllDispatchA5(inQueue, outBuf);
}

template <typename XType, bool IsMxfp8, bool IsMxfp8E4M3>
__aicore__ inline void MoeDispatchMemory<XType, IsMxfp8, IsMxfp8E4M3>::AllToAllDispatchA5(
    TQue<QuePosition::VECIN, 1> inQueue, TBuf<> outBuf)
{
    CalcSendTokenBufNum(outBuf);
    if (sendTokenBufNum_ == 0U) {
        return;
    }
    if ((aivId_ >= moeUsedAivNum_) && (sharedExpertRankNum_ != 0)) {
        SendToSharedExpert(inQueue, outBuf);
    } else {
        SendToMoeExpert(inQueue, outBuf);
    }
}

template <typename XType, bool IsMxfp8, bool IsMxfp8E4M3>
__aicore__ inline void MoeDispatchMemory<XType, IsMxfp8, IsMxfp8E4M3>::CalTokenSendExpertCnt(uint32_t dstExpertId, int32_t calCnt, int32_t &curExpertCnt)
{
    if (calCnt < axisK_) {
        curExpertCnt = 0;
        return;
    }
    LocalTensor<int32_t> dstExpIdTensor = dstExpBuf_.Get<int32_t>();
    LocalTensor<int32_t> subExpIdTensor = subExpBuf_.Get<int32_t>();
    Duplicate<int32_t>(dstExpIdTensor, dstExpertId, calCnt);
    PipeBarrier<PIPE_V>();
    Sub(subExpIdTensor, validExpertIdsTensor_, dstExpIdTensor, calCnt);
    PipeBarrier<PIPE_V>();
    LocalTensor<float> tmpFp32 = subExpIdTensor.ReinterpretCast<float>();
    LocalTensor<float> tmpoutFp32 = dstExpIdTensor.ReinterpretCast<float>();
    Abs(tmpoutFp32, tmpFp32, calCnt);
    PipeBarrier<PIPE_V>();
    Mins(subExpIdTensor, dstExpIdTensor, 1, calCnt);
    PipeBarrier<PIPE_V>();
    ReduceSum<float>(tmpoutFp32, tmpFp32, workLocalTensor_, calCnt);
    SyncFunc<AscendC::HardEvent::V_S>();
    int32_t curOtherExpertCnt = dstExpIdTensor(0);
    if (calCnt >= curOtherExpertCnt) {
        curExpertCnt = calCnt - curOtherExpertCnt;
    } else {
        curExpertCnt = 0;
    }
}

template <typename XType, bool IsMxfp8, bool IsMxfp8E4M3>
__aicore__ inline int32_t MoeDispatchMemory<XType, IsMxfp8, IsMxfp8E4M3>::CalExpertCnt(
    uint32_t expertId, uint32_t maskCnt)
{
    int32_t expertCnt = 0;
    if ((expertId < sharedExpertRankNum_) && (activeMaskBsCnt_ > 0)) {
        if (expertId % rankNumPerSharedExpert_ ==
            static_cast<uint32_t>(epRankId_) % rankNumPerSharedExpert_) {
            expertCnt = static_cast<int32_t>(activeMaskBsCnt_);
        }
    } else if (sendToMoeExpTokenCnt_ > 0) {
        uint32_t moeExpertId = expertId - sharedExpertRankNum_;
        CalTokenSendExpertCnt(moeExpertId, maskCnt, expertCnt);
    }
    return expertCnt;
}

template <typename XType, bool IsMxfp8, bool IsMxfp8E4M3>
__aicore__ inline void MoeDispatchMemory<XType, IsMxfp8, IsMxfp8E4M3>::CalAndSendCnt()
{
    CalAndSendCntByRankAndExpert();
}

template <typename XType, bool IsMxfp8, bool IsMxfp8E4M3>
__aicore__ inline void MoeDispatchMemory<XType, IsMxfp8, IsMxfp8E4M3>::SendExpertCntRange(
    uint32_t startExpertId, uint32_t endExpertId, uint32_t curRound)
{
    GlobalTensor<int32_t> rankGMTensor;
    for (uint32_t expertId = startExpertId; expertId < endExpertId;) {
        bool isSharedExpert = expertId < sharedExpertRankNum_;
        uint32_t moeExpertId = isSharedExpert ? 0U : expertId - sharedExpertRankNum_;
        uint32_t dstRankId = isSharedExpert ?
            expertId : moeExpertId / moeExpertNumPerRank_ + sharedExpertRankNum_;
        uint32_t localExpertId = isSharedExpert ? 0U : moeExpertId % moeExpertNumPerRank_;
        uint32_t sendExpertNum = 1U;
        if (!isSharedExpert) {
            sendExpertNum = endExpertId - expertId;
            uint32_t expertNumLeftInRank = moeExpertNumPerRank_ - localExpertId;
            sendExpertNum = sendExpertNum < expertNumLeftInRank ? sendExpertNum : expertNumLeftInRank;
        }

        const uint32_t targetGroupId =
            (dstRankId / TileXREp::kEpMemoryDoubleGroupStripeRankNum) & 1U;
        const uint32_t localGroupId = (static_cast<uint32_t>(epRankId_) /
            TileXREp::kEpMemoryDoubleGroupStripeRankNum) & 1U;
        const bool isCurrentRound = sendRoundNum_ == 1U ||
            targetGroupId == (localGroupId ^ curRound);
        if (isCurrentRound) {
            uint32_t offset = STATE_OFFSET * static_cast<uint32_t>(epRankId_) +
                localExpertId * epWorldSize_ * STATE_OFFSET;
            GM_ADDR rankGM = reinterpret_cast<__gm__ uint8_t *>(GetWindStateAddrByRankId(dstRankId)) + offset;
            rankGMTensor.SetGlobalBuffer(reinterpret_cast<__gm__ int32_t *>(rankGM));
            DataCopyParams cntCopyParams{static_cast<uint16_t>(sendExpertNum), 1U, 0U,
                static_cast<uint16_t>(epWorldSize_ - 1U)};
            DataCopy<int32_t>(rankGMTensor,
                statusTensor_[(expertId - startExpertId) * UB_ALIGN_DATA_COUNT], cntCopyParams);
        }
        expertId += sendExpertNum;
    }
}

template <typename XType, bool IsMxfp8, bool IsMxfp8E4M3>
__aicore__ inline void MoeDispatchMemory<XType, IsMxfp8, IsMxfp8E4M3>::CalAndSendCntByRankAndExpert()
{
    uint32_t startExpertId = 0;
    uint32_t endExpertId = 0;
    uint32_t sendExpertNum = 0;
    uint32_t countCoreNum = useAllCoreCount_ ? aivNum_ : aivUsedCumSum_;
    SplitToCore(totalExpertNum_, countCoreNum, startExpertId, endExpertId,
        sendExpertNum, useAllCoreCount_);
    if (sendExpertNum == 0) {
        for (uint32_t curRound = 0U; curRound < sendRoundNum_; ++curRound) {
            SyncBeforeSendRound(curRound);
        }
        return;
    }

    uint32_t maskCnt = isTokenMaskFlag_ ? activeMaskBsCnt_ * axisK_ : expertIdsCnt_;
    bool useVectorRange = false;
    uint32_t moeRecordBegin = startExpertId > sharedExpertRankNum_ ?
        startExpertId : sharedExpertRankNum_;
    uint32_t moeRecordEnd = endExpertId < totalExpertNum_ ? endExpertId : totalExpertNum_;
    uint32_t maskDataBytes = Ceil(maskCnt, static_cast<uint32_t>(BITS_PER_BYTE));
    uint32_t maskAllocBytes = Ceil(maskDataBytes, UB_ALIGN) * UB_ALIGN;
    useVectorRange = maskCnt >= VECTOR_COUNT_MIN_ROUTE_NUM &&
        moeRecordEnd > moeRecordBegin &&
        2U * maskAllocBytes <= maxSize_ &&
        maskCnt * sizeof(int32_t) <= maxSize_;
    if (useVectorRange) {
        CalAndSendCntByVectorRange(startExpertId, endExpertId, sendExpertNum,
            maskCnt, maskAllocBytes);
    }
    if (!useVectorRange) {
        CalAndSendCntByLegacyReduce(startExpertId, endExpertId, maskCnt);
    }
    SyncFunc<AscendC::HardEvent::S_MTE3>();
    for (uint32_t curRound = 0U; curRound < sendRoundNum_; ++curRound) {
        SyncBeforeSendRound(curRound);
        SendExpertCntRange(startExpertId, endExpertId, curRound);
    }
}

template <typename XType, bool IsMxfp8, bool IsMxfp8E4M3>
__aicore__ inline void MoeDispatchMemory<XType, IsMxfp8, IsMxfp8E4M3>::CalAndSendCntByLegacyReduce(
    uint32_t startExpertId, uint32_t endExpertId, uint32_t maskCnt)
{
    uint64_t flagMask[2] = {0x0101010101010101ULL, 0ULL};
    Duplicate<int32_t>(statusTensor_, 0, statusCntAlign_ * UB_ALIGN_DATA_COUNT);
    PipeBarrier<PIPE_V>();
    Duplicate<int32_t>(statusTensor_, 0x3F800000, flagMask, statusCntAlign_ / 8U, 1U, 8U);
    SyncFunc<AscendC::HardEvent::V_S>();

    for (uint32_t expertId = startExpertId; expertId < endExpertId; ++expertId) {
        int32_t expertCnt = CalExpertCnt(expertId, maskCnt);
        uint32_t cntPosIndex = (expertId - startExpertId) * UB_ALIGN_DATA_COUNT + 1U;
        statusTensor_.SetValue(cntPosIndex, expertCnt);
    }
}

template <typename XType, bool IsMxfp8, bool IsMxfp8E4M3>
__aicore__ inline void MoeDispatchMemory<XType, IsMxfp8, IsMxfp8E4M3>::CalAndSendCntByVectorRange(
    uint32_t startExpertId, uint32_t endExpertId, uint32_t sendExpertNum,
    uint32_t maskCnt, uint32_t maskAllocBytes)
{
    LocalTensor<int32_t> localCountTensor = statusTensor_[statusHistogramOffset_];
    LocalTensor<int32_t> selectedExpertIds = dstExpBuf_.Get<int32_t>();
    LocalTensor<uint8_t> lowerMask = subExpBuf_.Get<uint8_t>();
    LocalTensor<uint8_t> upperMask = lowerMask[maskAllocBytes];

    Duplicate<int32_t>(statusTensor_, 0, statusCntAlign_ * UB_ALIGN_DATA_COUNT);
    Duplicate<int32_t>(localCountTensor, 0, statusCntAlign_);

    uint32_t moeRecordBegin = startExpertId > sharedExpertRankNum_ ?
        startExpertId : sharedExpertRankNum_;
    uint32_t moeRecordEnd = endExpertId < totalExpertNum_ ? endExpertId : totalExpertNum_;
    uint32_t moeIdBegin = moeRecordBegin - sharedExpertRankNum_;
    uint32_t moeIdEnd = moeRecordEnd - sharedExpertRankNum_;

    Compares(lowerMask, validExpertIdsTensor_, static_cast<int32_t>(moeIdBegin),
        AscendC::CMPMODE::GE, maskCnt);
    Compares(upperMask, validExpertIdsTensor_, static_cast<int32_t>(moeIdEnd),
        AscendC::CMPMODE::LT, maskCnt);
    PipeBarrier<PIPE_V>();
    And(lowerMask.ReinterpretCast<uint16_t>(), lowerMask.ReinterpretCast<uint16_t>(),
        upperMask.ReinterpretCast<uint16_t>(),
        static_cast<int32_t>(maskAllocBytes / sizeof(uint16_t)));
    PipeBarrier<PIPE_V>();

    uint64_t selectedCount = 0U;
    GatherMask(selectedExpertIds, validExpertIdsTensor_,
        lowerMask.ReinterpretCast<uint32_t>(), true, maskCnt,
        {1U, 1U, 0U, 0U}, selectedCount);
    SyncFunc<AscendC::HardEvent::V_S>();

    for (uint32_t expertId = startExpertId;
         expertId < moeRecordBegin; ++expertId) {
        int32_t expertCnt = 0;
        if (activeMaskBsCnt_ > 0U &&
            expertId % rankNumPerSharedExpert_ ==
                static_cast<uint32_t>(epRankId_) % rankNumPerSharedExpert_) {
            expertCnt = static_cast<int32_t>(activeMaskBsCnt_);
        }
        localCountTensor.SetValue(expertId - startExpertId, expertCnt);
    }

    for (uint32_t index = 0U; index < static_cast<uint32_t>(selectedCount); ++index) {
        int32_t moeExpertId = selectedExpertIds.GetValue(index);
        uint32_t localRecord = static_cast<uint32_t>(moeExpertId) +
            sharedExpertRankNum_ - startExpertId;
        localCountTensor.SetValue(localRecord,
            localCountTensor.GetValue(localRecord) + 1);
    }

    for (uint32_t localRecord = 0U; localRecord < sendExpertNum; ++localRecord) {
        uint32_t recordOffset = localRecord * UB_ALIGN_DATA_COUNT;
        statusTensor_.SetValue(recordOffset, 0x3F800000);
        statusTensor_.SetValue(recordOffset + 1U,
            localCountTensor.GetValue(localRecord));
    }
}

template <typename XType, bool IsMxfp8, bool IsMxfp8E4M3>
__aicore__ inline void MoeDispatchMemory<XType, IsMxfp8, IsMxfp8E4M3>::BufferInit()
{
    uint32_t waitStatusBufSize = Ceil((recStatusNumPerCore_ * UB_ALIGN), SIZE_ALIGN_256) * SIZE_ALIGN_256;
    tpipe_->InitBuffer(waitStatusBuf_, waitStatusBufSize);
    uint64_t recStatusNumPerCoreSpace = Ceil(recStatusNumPerCore_ * sizeof(float), UB_ALIGN) * UB_ALIGN;
    uint64_t recvWinBlockNumSpace = epWorldSize_ * moeExpertNumPerRank_ * sizeof(float);
    uint64_t gatherMaskOutSize = (recStatusNumPerCoreSpace > recvWinBlockNumSpace) ? recStatusNumPerCoreSpace : recvWinBlockNumSpace;
    uint64_t sumContinueAlignSize = Ceil((aivNum_ * sizeof(float)), UB_ALIGN) * UB_ALIGN;
    tpipe_->InitBuffer(gatherMaskOutBuf_, gatherMaskOutSize);
    // Duplicate emits complete 256B vector repeats, including for blockDim 2/4
    // or a non-multiple of eight. Reserve its full span, not just the copied rows.
    const uint32_t coreFlagBytes = Ceil(aivNum_, 8U) * SIZE_ALIGN_256;
    tpipe_->InitBuffer(sumCoreBuf_, coreFlagBytes);
    tpipe_->InitBuffer(sumLocalBuf_, coreFlagBytes);
    tpipe_->InitBuffer(sumContinueBuf_, sumContinueAlignSize);
    tpipe_->InitBuffer(scalarBuf_, UB_ALIGN * 3);
    uint32_t tokenNumBufSize = Ceil(moeExpertNumPerRank_ * sizeof(int64_t), UB_ALIGN) * UB_ALIGN;
    tpipe_->InitBuffer(tokenNumBuf_, tokenNumBufSize);
}

template <typename XType, bool IsMxfp8, bool IsMxfp8E4M3>
__aicore__ inline void MoeDispatchMemory<XType, IsMxfp8, IsMxfp8E4M3>::WaitDispatchClearStatus()
{
    SyncFunc<AscendC::HardEvent::MTE3_S>();
    DataCopyParams intriOutParams{static_cast<uint16_t>(recStatusNumPerCore_), 1, 0, 0};
    uint64_t duplicateMask[2] = {0x0101010101010101ULL, 0ULL};
    LocalTensor<int32_t> cleanStateTensor = waitStatusBuf_.Get<int32_t>();
    SyncFunc<AscendC::HardEvent::S_V>();
    Duplicate<int32_t>(cleanStateTensor, 0, duplicateMask, Ceil(recStatusNumPerCore_, 8), 1, 8);
    SyncFunc<AscendC::HardEvent::V_MTE3>();
    DataCopy(windowInstatusFp32Tensor_[startStatusIndex_ * STATE_OFFSET / sizeof(float)],
             cleanStateTensor.ReinterpretCast<float>(), intriOutParams);
    SyncFunc<AscendC::HardEvent::MTE3_S>();
}

template <typename XType, bool IsMxfp8, bool IsMxfp8E4M3>
__aicore__ inline void MoeDispatchMemory<XType, IsMxfp8, IsMxfp8E4M3>::GatherSumRecvCnt(
    LocalTensor<float> &gatherMaskOutTensor, LocalTensor<uint32_t> &gatherTmpTensor,
    LocalTensor<float> &statusSumOutTensor)
{
    gatherTmpTensor.SetValue(0, 2U);
    uint32_t mask = 2U;
    SyncFunc<AscendC::HardEvent::S_V>();



    uint64_t recvCnt = 0;
    GatherMask(gatherMaskOutTensor, statusFp32Tensor_, gatherTmpTensor, true, mask,
        {1, (uint16_t)recStatusNumPerCore_, 1, 0}, recvCnt);
    PipeBarrier<PIPE_V>();


    uint32_t recStatusNumPerCoreInner = Ceil(recStatusNumPerCore_ * sizeof(float), UB_ALIGN)
        * UB_ALIGN / sizeof(float);
    SumParams sumParams{1, recStatusNumPerCoreInner, recStatusNumPerCore_};
    Sum(statusSumOutTensor, gatherMaskOutTensor, sumParams);
    SyncFunc<AscendC::HardEvent::V_S>();
    float sumOfRecvCnt = statusSumOutTensor.ReinterpretCast<float>().GetValue(0);


    uint32_t newAivId = aivId_ - aivUsedAllToAll_;

    LocalTensor<float> sumCoreFP32Tensor = sumCoreBuf_.Get<float>();
    uint64_t maskArrayCount[2] = {0x0101010101010101, 0};
    uint8_t repeatTimes = Ceil(aivUsedCumSum_, 8);

    Duplicate<float>(sumCoreFP32Tensor, sumOfRecvCnt, maskArrayCount, repeatTimes, 1, 8);
    uint64_t maskArrayFlag[2] = {0x0202020202020202, 0};
    Duplicate<float>(sumCoreFP32Tensor, static_cast<float>(1.0), maskArrayFlag, repeatTimes, 1, 8);
    DataCopyParams sumIntriParams{static_cast<uint16_t>(aivUsedCumSum_), 1, 0, 0};
    SyncFunc<AscendC::HardEvent::V_MTE3>();
    DataCopy(selfRankWinInGMTensor_[(CUMSUM_CAL_OFFSET + newAivId * aivUsedCumSum_ * UB_ALIGN) / sizeof(float)], sumCoreFP32Tensor, sumIntriParams);
    SyncFunc<AscendC::HardEvent::MTE3_V>();
}

template <typename XType, bool IsMxfp8, bool IsMxfp8E4M3>
__aicore__ inline void MoeDispatchMemory<XType, IsMxfp8, IsMxfp8E4M3>::GetCumSum(LocalTensor<int32_t> &outLocal, uint32_t newAivId)

{
    outLocal = gatherMaskOutBuf_.Get<int32_t>();
    DataCopyParams sumIntriParams{static_cast<uint16_t>(aivUsedCumSum_), 1, static_cast<uint16_t>(aivUsedCumSum_ - 1), 0};
    LocalTensor<float> sumLocalTensor = sumLocalBuf_.Get<float>();
    LocalTensor<uint32_t> gatherSumPattern = scalarBuf_.GetWithOffset<uint32_t>(UB_ALIGN / sizeof(uint32_t), 0);
    LocalTensor<float> sumContinueTensor = sumContinueBuf_.Get<float>();
    LocalTensor<float> recvCntSumOutTensor = scalarBuf_.GetWithOffset<float>(UB_ALIGN / sizeof(float), UB_ALIGN);

    uint32_t mask = 2;
    uint64_t recvCnt = 0;
    uint32_t innerSumParams = Ceil(aivUsedCumSum_ * sizeof(float), UB_ALIGN) * UB_ALIGN / sizeof(float);
    SumParams sumParams{1, innerSumParams, aivUsedCumSum_};
    int32_t cumSumFlag = 0;
    gatherSumPattern.SetValue(0, 2);
    SyncFunc<AscendC::HardEvent::S_V>();

    while (true) {
        DataCopy(sumLocalTensor, selfRankWinInGMTensor_[(CUMSUM_CAL_OFFSET + newAivId * UB_ALIGN) / sizeof(float)], sumIntriParams);
        SyncFunc<AscendC::HardEvent::MTE2_V>();
        GatherMask(sumContinueTensor, sumLocalTensor, gatherSumPattern, true, mask, {1, static_cast<uint16_t>(aivUsedCumSum_), 1, 0}, recvCnt);
        PipeBarrier<PIPE_V>();
        Sum(recvCntSumOutTensor, sumContinueTensor, sumParams);
        SyncFunc<AscendC::HardEvent::V_S>();
        cumSumFlag = static_cast<int32_t>(recvCntSumOutTensor.GetValue(0));
        if (cumSumFlag == aivUsedCumSum_) {
            break;
        }
    }
    if (newAivId == 0) {
        outLocal.SetValue(0, 0);
    } else {
        mask = 1;
        recvCnt = 0;
        gatherSumPattern.SetValue(0, 1);
        SyncFunc<AscendC::HardEvent::S_V>();
        GatherMask(sumContinueTensor, sumLocalTensor, gatherSumPattern, true, mask, {1, static_cast<uint16_t>(newAivId), 1, 0}, recvCnt);
        PipeBarrier<PIPE_V>();
        uint32_t innerCumSumParams = Ceil(newAivId * sizeof(float), UB_ALIGN) * UB_ALIGN / sizeof(float);
        SumParams cumSumParams{1, innerCumSumParams, newAivId};
        Sum(recvCntSumOutTensor, sumContinueTensor, cumSumParams);
        SyncFunc<AscendC::HardEvent::V_S>();
        outLocal.SetValue(0, recvCntSumOutTensor.ReinterpretCast<int32_t>().GetValue(0));
    }

    LocalTensor<float> sumCoreFp32Tensor = sumLocalBuf_.Get<float>();

    uint8_t repeatTimes = Ceil(aivUsedCumSum_, 8);

    Duplicate<float>(sumCoreFp32Tensor, static_cast<float>(0), 64, repeatTimes, 1, 8);
    DataCopyParams cleanParams{static_cast<uint16_t>(aivUsedCumSum_), 1, 0, static_cast<uint16_t>(aivUsedCumSum_ - 1)};
    SyncFunc<AscendC::HardEvent::V_MTE3>();
    DataCopy(selfRankWinInGMTensor_[(CUMSUM_CAL_OFFSET + newAivId * UB_ALIGN) / sizeof(float)], sumCoreFp32Tensor, cleanParams);
}

template <typename XType, bool IsMxfp8, bool IsMxfp8E4M3>
__aicore__ inline void MoeDispatchMemory<XType, IsMxfp8, IsMxfp8E4M3>::WaitDispatch()
{
    LocalTensor<float> gatherMaskOutTensor = gatherMaskOutBuf_.Get<float>();
    LocalTensor<uint32_t> gatherTmpTensor = scalarBuf_.GetWithOffset<uint32_t>(UB_ALIGN / sizeof(uint32_t), 0);
    LocalTensor<float> statusSumOutTensor = scalarBuf_.GetWithOffset<float>(UB_ALIGN / sizeof(float), UB_ALIGN);
    statusFp32Tensor_ = waitStatusBuf_.Get<float>();
    uint32_t mask = 1U;
    gatherTmpTensor.SetValue(0, 1U);
    float compareTarget = static_cast<float>(1.0) * recStatusNumPerCore_;
    float sumOfFlag = static_cast<float>(-1.0);

    DataCopyParams intriParams{static_cast<uint16_t>(recStatusNumPerCore_), 1, 0, 0};
    SyncFunc<AscendC::HardEvent::S_V>();
    while (sumOfFlag != compareTarget) {
        DataCopy(statusFp32Tensor_, windowInstatusFp32Tensor_[startStatusIndex_ * STATE_OFFSET / sizeof(float)], intriParams);
        SyncFunc<AscendC::HardEvent::MTE2_V>();
        ReduceSum(statusSumOutTensor, statusFp32Tensor_, gatherMaskOutTensor,
            mask, recStatusNumPerCore_, 1U);
        SyncFunc<AscendC::HardEvent::V_S>();
        sumOfFlag = statusSumOutTensor.GetValue(0);
    }

    WaitDispatchClearStatus();
    GatherSumRecvCnt(gatherMaskOutTensor, gatherTmpTensor, statusSumOutTensor);
}

template <typename XType, bool IsMxfp8, bool IsMxfp8E4M3>
__aicore__ inline void MoeDispatchMemory<XType, IsMxfp8, IsMxfp8E4M3>::CalRecvAndSetFlag()
{

    LocalTensor<int32_t> outCountLocal;
    uint32_t newAivId = aivId_ - aivUsedAllToAll_;
    GetCumSum(outCountLocal, newAivId);

    uint32_t preSum = outCountLocal.GetValue(0);
    uint32_t curCnt = preSum;
    waitStatusTensor_ = waitStatusBuf_.Get<int32_t>();
    for (uint32_t index = startStatusIndex_; index < endStatusIndex_; index++) {
        uint32_t i = index - startStatusIndex_;
        uint32_t count = waitStatusTensor_.GetValue(i * UB_ALIGN_DATA_COUNT + 1U);
        curCnt += count;
        outCountLocal.SetValue(i, curCnt);
    }
#ifdef TILEXR_EP_COMPACT_ENABLED
    if (endStatusIndex_ == rscvStatusNum_) compactRows_ = static_cast<int32_t>(curCnt);
#endif
    SyncFunc<AscendC::HardEvent::S_V>();
    GM_ADDR wAddr = (__gm__ uint8_t*)(recvCntWorkspaceGM_);
    GlobalTensor<int32_t> sendCountsGlobal, workspaceGlobal;
    sendCountsGlobal.SetGlobalBuffer(reinterpret_cast<__gm__ int32_t*>(sendCountsOutGM_));
    workspaceGlobal.SetGlobalBuffer((__gm__ int32_t*)wAddr);
    DataCopyExtParams dataCopyOutParams{1U, static_cast<uint32_t>(recStatusNumPerCore_ * sizeof(int32_t)), 0U, 0U, 0U};
    DataCopyPad(sendCountsGlobal[startStatusIndex_], outCountLocal, dataCopyOutParams);

    uint32_t outputCores = aivNum_;
#ifdef TILEXR_EP_COMPACT_ENABLED
    // SplitToCore(..., true) assigns output only to this leading prefix. Do not
    // produce count rows/flags for lanes that will never consume them.
    outputCores = rscvStatusNum_ < aivNum_ ? rscvStatusNum_ : aivNum_;
#endif
    for (uint32_t index = 0; index < outputCores; index++) {
        DataCopyPad(workspaceGlobal[index * rscvStatusNum_ + startStatusIndex_], outCountLocal, dataCopyOutParams);
    }
    uint8_t repeatTimes = Ceil(outputCores, 8);
    DataCopyParams sumIntriParams{static_cast<uint16_t>(outputCores), 1, 0, static_cast<uint16_t>(aivUsedCumSum_ - 1)};
    LocalTensor<int32_t> syncOnCoreTensor = sumCoreBuf_.Get<int32_t>();
    LocalTensor<float> syncOnCoreFP32Tensor = sumCoreBuf_.Get<float>();

    Duplicate<int32_t>(syncOnCoreTensor, static_cast<int32_t>(1), SIZE_ALIGN_256 / sizeof(int32_t), repeatTimes, 1, 8);
    SyncFunc<AscendC::HardEvent::V_MTE3>();
    PipeBarrier<PIPE_MTE3>();

    DataCopy(selfRankWinInGMTensor_[(CUMSUM_FLAG_OFFSET + newAivId * UB_ALIGN) / sizeof(float)], syncOnCoreFP32Tensor, sumIntriParams);
}

template <typename XType, bool IsMxfp8, bool IsMxfp8E4M3>
__aicore__ inline void MoeDispatchMemory<XType, IsMxfp8, IsMxfp8E4M3>::CalCumSum()
{

    expertIdsBufSize_ = Ceil(expertIdsCnt_ * sizeof(int32_t), SIZE_ALIGN_256) * SIZE_ALIGN_256;
    tpipe_->InitBuffer(dstExpBuf_, maxSize_);
    tpipe_->InitBuffer(subExpBuf_, maxSize_);
    tpipe_->InitBuffer(gatherMaskTBuf_, expertIdsBufSize_);
    tpipe_->InitBuffer(expertIdsBuf_, expertIdsBufSize_);
    tpipe_->InitBuffer(statusBuf_, statusBufSize_);
    workLocalTensor_ = gatherMaskTBuf_.Get<float>();
    statusTensor_ = statusBuf_.Get<int32_t>();
    ExpIdsCopyAndMaskCal();
    CalSendRound();
    CalAndSendCnt();
    SplitToCore(rscvStatusNum_, aivUsedCumSum_, startStatusIndex_, endStatusIndex_, recStatusNumPerCore_, false);
    BufferInit();
    WaitDispatch();
    CalRecvAndSetFlag();

}

template <typename XType, bool IsMxfp8, bool IsMxfp8E4M3>
__aicore__ inline void MoeDispatchMemory<XType, IsMxfp8, IsMxfp8E4M3>::WaitCumSumFlag()
{

    int32_t cumSumFlag = 0;
    int32_t targetFlag = aivUsedCumSum_ * UB_ALIGN_DATA_COUNT;
    uint32_t cumSumFlagOffset = (CUMSUM_FLAG_OFFSET + aivId_ * aivUsedCumSum_ * UB_ALIGN) / sizeof(float);
    uint32_t innerSumParams = aivUsedCumSum_ * UB_ALIGN / sizeof(float);
    SumParams sumFlagParams{1, innerSumParams, aivUsedCumSum_ * UB_ALIGN_DATA_COUNT};
    LocalTensor<float> statusSumOutTensor = scalarBuf_.Get<float>();

    while (true) {
        DataCopy(statusFp32Tensor_, selfRankWinInGMTensor_[cumSumFlagOffset], aivUsedCumSum_ * UB_ALIGN_DATA_COUNT);
        SyncFunc<AscendC::HardEvent::MTE2_V>();
        Sum(statusSumOutTensor, statusFp32Tensor_, sumFlagParams);
        SyncFunc<AscendC::HardEvent::V_S>();
        cumSumFlag = statusSumOutTensor.ReinterpretCast<int32_t>().GetValue(0);
        if (cumSumFlag == targetFlag) {
            break;
        }
    }
    Duplicate<float>(statusCleanFp32Tensor_, static_cast<float>(0), aivUsedCumSum_ * UB_ALIGN_DATA_COUNT);
    SyncFunc<AscendC::HardEvent::S_MTE3>();

    SyncFunc<AscendC::HardEvent::V_MTE3>();
    DataCopy(selfRankWinInGMTensor_[cumSumFlagOffset], statusCleanFp32Tensor_, aivUsedCumSum_ * UB_ALIGN_DATA_COUNT);
}

template <typename XType, bool IsMxfp8, bool IsMxfp8E4M3>
__aicore__ inline void MoeDispatchMemory<XType, IsMxfp8, IsMxfp8E4M3>::SetValidExpertInfo(uint32_t expInfoSize, uint32_t &validNum)
{

    GlobalTensor<int32_t> workspaceGlobal;
    workspaceGlobal.SetGlobalBuffer(reinterpret_cast<__gm__ int32_t *>(recvCntWorkspaceGM_));
    DataCopyExtParams scalesCopyInParams{1U, static_cast<uint32_t>(rscvStatusNum_ * sizeof(int32_t)), 0U, 0U, 0U};
    DataCopyPadExtParams<int32_t> copyPadExtParams{false, 0U, 0U, 0U};
    DataCopyPad(sendCntTensor_, workspaceGlobal[aivId_ * rscvStatusNum_], scalesCopyInParams, copyPadExtParams);
    PipeBarrier<PIPE_ALL>();

    if (aivId_ == 0) {
        uint32_t localExpertNum = isShareExpertRankFlag_ ? 1 : moeExpertNumPerRank_;
        int64_t lastVal = 0;
        uint32_t tokenNumBufSize = Ceil(moeExpertNumPerRank_ * sizeof(int64_t), UB_ALIGN) * UB_ALIGN;
        tpipe_->InitBuffer(tokenNumBuf_, tokenNumBufSize);
        LocalTensor<int64_t> expertTokenNumsLocalTensor = tokenNumBuf_.Get<int64_t>();
        for (uint32_t localExpertIdx = 0; localExpertIdx < localExpertNum; ++localExpertIdx) {
            if (expertTokenNumsType_ == 0) {
                expertTokenNumsLocalTensor(localExpertIdx) = int64_t(
                    sendCntTensor_(localExpertIdx * epWorldSize_ + epWorldSize_ - 1));
            } else {
                expertTokenNumsLocalTensor(localExpertIdx) = int64_t(
                    sendCntTensor_(localExpertIdx * epWorldSize_ + epWorldSize_ - 1)) - lastVal;
                lastVal = int64_t(
                    sendCntTensor_(localExpertIdx * epWorldSize_ + epWorldSize_ - 1));
            }
        }
        SyncFunc<AscendC::HardEvent::S_MTE3>();
        DataCopyExtParams expertTokenNumsCopyParams{1U, static_cast<uint32_t>(localExpertNum * sizeof(int64_t)),
            0U, 0U, 0U};
        DataCopyPad(expertTokenNumsOutGMTensor_, expertTokenNumsLocalTensor, expertTokenNumsCopyParams);
    }

    Duplicate<uint32_t>(expertFinishNumTensor_, 0, expInfoSize / sizeof(uint32_t));
    for (uint32_t index = startId_; index < endId_; index++) {
        expertMapTensor_(validNum) = index;
        if (index == 0) {
            expertLeftNumTensor_(validNum) = sendCntTensor_(index);
        } else {
            expertLeftNumTensor_(validNum) = sendCntTensor_(index) - sendCntTensor_(index - 1);
        }
        if (expertLeftNumTensor_(validNum) != 0) {
            validNum += 1;
        }
    }
}

template <typename XType, bool IsMxfp8, bool IsMxfp8E4M3>
__aicore__ inline uint32_t MoeDispatchMemory<XType, IsMxfp8, IsMxfp8E4M3>::CheckDataArriveWithFlag(uint32_t srcExpDataIdx,
    int32_t beginIdx, int32_t copyCnt)
{
    uint32_t flagNum = blockCntPerToken_ * uint32_t(copyCnt);
    DataCopyParams expFlagCopyParams{static_cast<uint16_t>(flagNum), 1U,
        static_cast<uint16_t>(SPLIT_BLOCK_DATA_SIZE / UB_ALIGN), 0U};
    GlobalTensor<float> dataFlagGlobal;
    GM_ADDR wAddr = (__gm__ uint8_t*)(windowGM_) + srcExpDataIdx * expertPerSizeOnWin_ +
        beginIdx * hCommuSize_ + SPLIT_BLOCK_DATA_SIZE;
    dataFlagGlobal.SetGlobalBuffer((__gm__ float *)(wAddr));
    DataCopy(flagRecvTensor_, dataFlagGlobal, expFlagCopyParams);
    SyncFunc<AscendC::HardEvent::MTE2_V>();
    LocalTensor<float> flagSumOutTensor = scalarBuf_.Get<float>();
    ReduceSum(flagSumOutTensor, flagRecvTensor_, flagReduceWorkTensor_, 1U, flagNum, 1U);
    SyncFunc<AscendC::HardEvent::V_S>();
    const bool arrived = flagSumOutTensor.GetValue(0) == 1.0f * flagNum;
    return arrived ? static_cast<uint32_t>(copyCnt) : 0U;
}

template <typename XType, bool IsMxfp8, bool IsMxfp8E4M3>
__aicore__ inline void MoeDispatchMemory<XType, IsMxfp8, IsMxfp8E4M3>::CopyInAndOut(
    LocalTensor<uint8_t> xTmpTensor, GM_ADDR wAddr, uint32_t index, uint32_t dstPosition,
    uint32_t arriveCount, uint32_t dataBufferId, bool isDataBufferReused)
{
    GlobalTensor<uint8_t> dataFlagGlobal;
    GlobalTensor<uint8_t> expandXOutGlobal;
    GlobalTensor<int32_t> assistGlobal;
    dataFlagGlobal.SetGlobalBuffer(reinterpret_cast<__gm__ uint8_t *>(wAddr));
    expandXOutGlobal.SetGlobalBuffer(
        reinterpret_cast<__gm__ uint8_t *>(expandXOutGM_) + dstPosition * hOutSize_);
    assistGlobal.SetGlobalBuffer(reinterpret_cast<__gm__ int32_t *>(assistInfoForCombineOutGM_));

    DataCopyParams srcTokenCopyParams{static_cast<uint16_t>(blockCntPerToken_ * arriveCount),
        static_cast<uint16_t>(SPLIT_BLOCK_DATA_SIZE), static_cast<uint16_t>(UB_ALIGN), 0U};
    DataCopyExtParams tokenCopyParams{static_cast<uint16_t>(arriveCount), hOutSize_,
        static_cast<uint32_t>((packedPayloadBytes_ - hOutSize_) / UB_ALIGN), 0U, 0U};
    DataCopyExtParams scaleCopyParams{static_cast<uint16_t>(arriveCount), scaleOutBytes_,
        static_cast<uint32_t>((packedPayloadBytes_ - scaleOutBytes_) / UB_ALIGN), 0U, 0U};
    DataCopyExtParams assistCopyParams{static_cast<uint16_t>(arriveCount),
        TileXREp::kEpMemoryAssistInfoWidth * static_cast<uint32_t>(sizeof(int32_t)),
        packedPayloadBytes_ / UB_ALIGN - 1U, 0U, 0U};
    DataCopyPadParams srcTokenPadParams{false, 0U, 0U, 0U};

    if (isDataBufferReused) {
        AscendC::WaitFlag<AscendC::HardEvent::MTE3_V>(dataBufferId);
        AscendC::SetFlag<AscendC::HardEvent::V_MTE2>(dataBufferId);
        AscendC::WaitFlag<AscendC::HardEvent::V_MTE2>(dataBufferId);
    }
    DataCopyPad(xTmpTensor,
        dataFlagGlobal[expertFinishNumTensor_(index) * hCommuSize_],
        srcTokenCopyParams, srcTokenPadParams);
    AscendC::SetFlag<AscendC::HardEvent::MTE2_MTE3>(dataBufferId);
    AscendC::WaitFlag<AscendC::HardEvent::MTE2_MTE3>(dataBufferId);

    if constexpr (IsMxfp8) {
        LocalTensor<uint8_t> scaleLocal = xTmpTensor[Ceil(axisH_, 256U) * 256U];
        DataCopyPad(dynamicScalesOutGMTensor_[dstPosition * scaleOutBytes_],
            scaleLocal, scaleCopyParams);
    }
    DataCopyPad(expandXOutGlobal, xTmpTensor, tokenCopyParams);
    LocalTensor<int32_t> xTmpInt32 = xTmpTensor.ReinterpretCast<int32_t>();
    DataCopyPad(assistGlobal[dstPosition * TileXREp::kEpMemoryAssistInfoWidth],
        xTmpInt32[tokenQuantAlign_], assistCopyParams);
    AscendC::SetFlag<AscendC::HardEvent::MTE3_V>(dataBufferId);
}

template <typename XType, bool IsMxfp8, bool IsMxfp8E4M3>
__aicore__ inline uint32_t
MoeDispatchMemory<XType, IsMxfp8, IsMxfp8E4M3>::GetLocalWindowSrcDataBlockIdx(
    uint32_t srcExpertId, uint32_t localExpertNum)
{
    uint32_t srcDataBlockIdx =
        srcExpertId % epWorldSize_ * localExpertNum + srcExpertId / epWorldSize_;
    if (!isShareExpertRankFlag_) {
        srcDataBlockIdx =
            (srcExpertId + static_cast<uint32_t>(epRankId_)) % epWorldSize_ *
                localExpertNum +
            srcExpertId / epWorldSize_;
    }
    return srcDataBlockIdx;
}

template <typename XType, bool IsMxfp8, bool IsMxfp8E4M3>
__aicore__ inline void MoeDispatchMemory<XType, IsMxfp8, IsMxfp8E4M3>::ClearLocalWindowDataFlags(
    TBuf<> tBuf, uint32_t validNum, uint32_t localExpertNum)
{
    uint32_t maxCleanRecordNum = 0U;
    for (uint32_t index = 0U; index < validNum; ++index) {
        uint32_t cleanRecordNum = expertFinishNumTensor_(index) * blockCntPerToken_;
        maxCleanRecordNum =
            cleanRecordNum > maxCleanRecordNum ? cleanRecordNum : maxCleanRecordNum;
    }

    uint32_t cleanRecordCapacity = tBufRealSize_ / UB_ALIGN;
    cleanRecordCapacity = cleanRecordCapacity > DATA_COPY_MAX_BLOCK_COUNT ?
        DATA_COPY_MAX_BLOCK_COUNT : cleanRecordCapacity;
    uint32_t cleanRecordNum = maxCleanRecordNum > cleanRecordCapacity ?
        cleanRecordCapacity : maxCleanRecordNum;
    if (cleanRecordNum == 0U) {
        return;
    }

    LocalTensor<float> cleanTensor = tBuf.Get<float>();
    Duplicate<float>(cleanTensor, 0.0F, cleanRecordNum * UB_ALIGN_DATA_COUNT);
    SyncFunc<AscendC::HardEvent::V_MTE3>();

    GlobalTensor<float> cleanGlobal;
    DataCopyParams cleanParams{1U, 1U, 0U, SPLIT_BLOCK_DATA_SIZE / UB_ALIGN};
    for (uint32_t index = 0U; index < validNum; ++index) {
        uint32_t srcExpertId = expertMapTensor_(index);
        uint32_t srcDataBlockIdx =
            GetLocalWindowSrcDataBlockIdx(srcExpertId, localExpertNum);
        GM_ADDR wAddr = reinterpret_cast<__gm__ uint8_t *>(windowGM_) +
            srcDataBlockIdx * expertPerSizeOnWin_;
        cleanGlobal.SetGlobalBuffer(reinterpret_cast<__gm__ float *>(wAddr));

        uint32_t leftRecordNum = expertFinishNumTensor_(index) * blockCntPerToken_;
        uint32_t recordOffset = 0U;
        while (leftRecordNum > 0U) {
            uint32_t curCleanRecordNum =
                leftRecordNum > cleanRecordNum ? cleanRecordNum : leftRecordNum;
            cleanParams.blockCount = static_cast<uint16_t>(curCleanRecordNum);
            uint32_t flagIndex =
                recordOffset * SPLIT_BLOCK_COUNT + SPLIT_BLOCK_DATA_COUNT;
            DataCopy(cleanGlobal[flagIndex], cleanTensor, cleanParams);
            recordOffset += curCleanRecordNum;
            leftRecordNum -= curCleanRecordNum;
        }
    }
}

template <typename XType, bool IsMxfp8, bool IsMxfp8E4M3>
__aicore__ inline void MoeDispatchMemory<XType, IsMxfp8, IsMxfp8E4M3>::WaitAndFormatOutput(
    TBuf<> tBuf, uint32_t validNum)
{
    uint32_t index = 0U;
    uint32_t finishNum = 0U;
    uint32_t localExpertNum = isShareExpertRankFlag_ ? 1U : moeExpertNumPerRank_;
    uint64_t packedTokenSize =
        static_cast<uint64_t>(blockCntPerToken_) * SPLIT_BLOCK_DATA_SIZE;
    uint64_t minUbSizePerToken =
        static_cast<uint64_t>(LOCAL_COPY_BUFFER_NUM) * packedTokenSize +
        static_cast<uint64_t>(blockCntPerToken_) * (UB_ALIGN + sizeof(float));
    uint32_t maxCopyTokenCnt =
        static_cast<uint32_t>(tBufRealSize_ / minUbSizePerToken);
    uint32_t dataBufferSize = 0U;
    uint32_t reduceWorkSize = 0U;
    uint32_t flagRecvSize = 0U;
    while (maxCopyTokenCnt > 0U) {
        dataBufferSize =
            static_cast<uint32_t>(packedTokenSize * maxCopyTokenCnt);
        reduceWorkSize =
            Ceil(blockCntPerToken_ * maxCopyTokenCnt * sizeof(float),
                SIZE_ALIGN_256) *
            SIZE_ALIGN_256;
        flagRecvSize = blockCntPerToken_ * maxCopyTokenCnt * UB_ALIGN;
        uint64_t totalBufferSize =
            static_cast<uint64_t>(LOCAL_COPY_BUFFER_NUM) * dataBufferSize +
            reduceWorkSize + flagRecvSize;
        if (totalBufferSize <= tBufRealSize_) {
            break;
        }
        --maxCopyTokenCnt;
    }

    LocalTensor<uint8_t> xTmpPingTensor =
        tBuf.GetWithOffset<uint8_t>(dataBufferSize, 0U);
    LocalTensor<uint8_t> xTmpPongTensor =
        tBuf.GetWithOffset<uint8_t>(dataBufferSize, dataBufferSize);
    uint32_t flagWorkOffset = LOCAL_COPY_BUFFER_NUM * dataBufferSize;
    flagReduceWorkTensor_ =
        tBuf.GetWithOffset<float>(reduceWorkSize / sizeof(float), flagWorkOffset);
    flagRecvTensor_ = tBuf.GetWithOffset<float>(
        flagRecvSize / sizeof(float), flagWorkOffset + reduceWorkSize);

    uint32_t copyBatchId = 0U;
    while (true) {
        if (expertLeftNumTensor_(index) == 0U) {
            index = (index + 1U) % validNum;
            continue;
        }

        uint32_t srcExpertId = expertMapTensor_(index);
        uint32_t copyCnt = expertLeftNumTensor_(index) > maxCopyTokenCnt ?
            maxCopyTokenCnt : expertLeftNumTensor_(index);
        uint32_t srcDataBlockIdx =
            GetLocalWindowSrcDataBlockIdx(srcExpertId, localExpertNum);
        uint32_t arriveCount = CheckDataArriveWithFlag(
            srcDataBlockIdx, expertFinishNumTensor_(index), copyCnt);
        if (arriveCount == copyCnt) {
            uint32_t dstPosition =
                srcExpertId != 0U ? sendCntTensor_(srcExpertId - 1U) : 0U;
            dstPosition += expertFinishNumTensor_(index);
            GM_ADDR wAddr = reinterpret_cast<__gm__ uint8_t *>(windowGM_) +
                srcDataBlockIdx * expertPerSizeOnWin_;
            uint32_t dataBufferId = copyBatchId % LOCAL_COPY_BUFFER_NUM;
            LocalTensor<uint8_t> xTmpTensor =
                dataBufferId == 0U ? xTmpPingTensor : xTmpPongTensor;
            CopyInAndOut(xTmpTensor, wAddr, index, dstPosition, arriveCount,
                dataBufferId, copyBatchId >= LOCAL_COPY_BUFFER_NUM);
            ++copyBatchId;
            expertFinishNumTensor_(index) += arriveCount;
            expertLeftNumTensor_(index) -= arriveCount;
            if (expertLeftNumTensor_(index) == 0U) {
                ++finishNum;
            }
        } else {
            index = (index + 1U) % validNum;
        }

        if (finishNum == validNum) {
            break;
        }
    }

    uint32_t usedDataBufferNum = copyBatchId > LOCAL_COPY_BUFFER_NUM ?
        LOCAL_COPY_BUFFER_NUM : copyBatchId;
    for (uint32_t dataBufferId = 0U;
         dataBufferId < usedDataBufferNum; ++dataBufferId) {
        AscendC::WaitFlag<AscendC::HardEvent::MTE3_V>(dataBufferId);
    }
    ClearLocalWindowDataFlags(tBuf, validNum, localExpertNum);
}

template <typename XType, bool IsMxfp8, bool IsMxfp8E4M3>
__aicore__ inline void MoeDispatchMemory<XType, IsMxfp8, IsMxfp8E4M3>::LocalWindowCopy()
{
#ifdef TILEXR_EP_COMPACT_ENABLED
    ElasticProfile::Mark<ElasticProfile::OutputSetupBegin>(compactProfiler_);
    if (aivId_ >= rscvStatusNum_) {
        // Count producers omit these lanes, so there is no round flag to consume.
        ElasticProfile::Mark<ElasticProfile::OutputSetupEnd>(compactProfiler_);
        return;
    }
#endif
    tpipe_->Reset();
    TBuf<> cumSumBuf;
    TBuf<> statusWaitBuf;
    TBuf<> statusCleanBuf;
    uint32_t rscvNumAlign =
        Ceil(rscvStatusNum_ * sizeof(int32_t), UB_ALIGN) * UB_ALIGN;
    tpipe_->InitBuffer(scalarBuf_, UB_ALIGN);
    tpipe_->InitBuffer(statusWaitBuf, aivUsedCumSum_ * UB_ALIGN);
    tpipe_->InitBuffer(cumSumBuf, rscvNumAlign);
    tpipe_->InitBuffer(statusCleanBuf, aivUsedCumSum_ * UB_ALIGN);
    statusFp32Tensor_ = statusWaitBuf.Get<float>();
    statusCleanFp32Tensor_ = statusCleanBuf.Get<float>();
    sendCntTensor_ = cumSumBuf.Get<int32_t>();
    SplitToCore(rscvStatusNum_, aivNum_, startId_, endId_, sendNum_, true);
#ifdef TILEXR_EP_COMPACT_ENABLED
    ElasticProfile::Mark<ElasticProfile::CountWaitBegin>(compactProfiler_);
#endif
    WaitCumSumFlag();
#ifdef TILEXR_EP_COMPACT_ENABLED
    ElasticProfile::Mark<ElasticProfile::CountWaitEnd>(compactProfiler_);
#endif
    if (sendNum_ == 0U) {
#ifdef TILEXR_EP_COMPACT_ENABLED
        ElasticProfile::Mark<ElasticProfile::OutputSetupEnd>(compactProfiler_);
#endif
        return;
    }

    TBuf<> expertMapBuf;
    TBuf<> expertFinishBuf;
    TBuf<> expertLeftBuf;
    TBuf<> tBuf;
    uint32_t validNum = 0U;
    uint32_t expInfoSize =
        Ceil(sendNum_ * sizeof(uint32_t), UB_ALIGN) * UB_ALIGN;
    tpipe_->InitBuffer(expertMapBuf, expInfoSize);
    tpipe_->InitBuffer(expertFinishBuf, expInfoSize);
    tpipe_->InitBuffer(expertLeftBuf, expInfoSize);
    tBufRealSize_ = FULL_MESH_MAX_UB_SIZE -
        (UB_ALIGN + rscvNumAlign + 2U * aivUsedCumSum_ * UB_ALIGN) -
        expInfoSize * 3U;
    tpipe_->InitBuffer(tBuf, tBufRealSize_);
    expertMapTensor_ = expertMapBuf.Get<uint32_t>();
    expertFinishNumTensor_ = expertFinishBuf.Get<uint32_t>();
    expertLeftNumTensor_ = expertLeftBuf.Get<uint32_t>();
    SetValidExpertInfo(expInfoSize, validNum);
    if (validNum == 0U) {
#ifdef TILEXR_EP_COMPACT_ENABLED
        ElasticProfile::Mark<ElasticProfile::OutputSetupEnd>(compactProfiler_);
#endif
        return;
    }

    SyncFunc<AscendC::HardEvent::V_S>();
#ifdef TILEXR_EP_COMPACT_ENABLED
    // Prefix loading, expert-map construction and count output do not depend on
    // the allocation. Finish them while the owner allocates; wait only before copy.
    if (aivId_ != aivNum_ - 1U && !WaitCompactOutput()) {
        ElasticProfile::Mark<ElasticProfile::OutputSetupEnd>(compactProfiler_);
        return;
    }
    ElasticProfile::Mark<ElasticProfile::OutputSetupEnd>(compactProfiler_);
    ElasticProfile::Mark<ElasticProfile::LocalOutputCopyBegin>(compactProfiler_);
#endif
    WaitAndFormatOutput(tBuf, validNum);
#ifdef TILEXR_EP_COMPACT_ENABLED
    ElasticProfile::Mark<ElasticProfile::LocalOutputCopyEnd>(compactProfiler_);
#endif
}

template <typename XType, bool IsMxfp8, bool IsMxfp8E4M3>
__aicore__ inline void MoeDispatchMemory<XType, IsMxfp8, IsMxfp8E4M3>::TokenActiveMaskCal()
{
    const uint32_t calCount =
        Ceil(axisBS_ * sizeof(half), SIZE_ALIGN_256) * SIZE_ALIGN_256 / sizeof(half);
    LocalTensor<bool> maskInputTensor = dstExpBuf_.Get<bool>();
    LocalTensor<half> maskTmpTensor = subExpBuf_.Get<half>();
    LocalTensor<uint8_t> maskTensor = expertIdsBuf_.Get<uint8_t>();
    LocalTensor<uint32_t> maskTensorInt32 = expertIdsBuf_.Get<uint32_t>();
    LocalTensor<int32_t> bsIndexTensor = dstExpBuf_.Get<int32_t>();
    validBsIndexTensor_ = gatherMaskTBuf_.Get<int32_t>();

    Duplicate<half>(maskTmpTensor, static_cast<half>(0), calCount);
    Duplicate<uint32_t>(maskTensorInt32, 0U, Ceil(calCount, 32U));
    SyncFunc<AscendC::HardEvent::V_MTE2>();
    const DataCopyExtParams maskParams{
        1U, axisBS_ * static_cast<uint32_t>(sizeof(bool)), 0U, 0U, 0U};
    DataCopyPadExtParams<bool> maskCopyPadParams{false, 0U, 0U, 0U};
    DataCopyPad(maskInputTensor, xActiveMaskGMTensor_, maskParams, maskCopyPadParams);
    SyncFunc<AscendC::HardEvent::MTE2_V>();
    LocalTensor<int8_t> maskInputInt8Tensor = maskInputTensor.ReinterpretCast<int8_t>();
    Cast(maskTmpTensor, maskInputInt8Tensor, RoundMode::CAST_NONE, axisBS_);
    PipeBarrier<PIPE_V>();
    CompareScalar(maskTensor, maskTmpTensor, static_cast<half>(1),
        AscendC::CMPMODE::EQ, calCount);
    CreateVecIndex(bsIndexTensor, 0, axisBS_);
    PipeBarrier<PIPE_V>();
    uint32_t mask = axisBS_;
    activeMaskBsCnt_ = 0U;
    GatherMask(validBsIndexTensor_, bsIndexTensor, maskTensorInt32, true, mask,
        {1, 1, 0, 0}, activeMaskBsCnt_);
    SyncFunc<AscendC::HardEvent::V_S>();
    sendToMoeExpTokenCnt_ = activeMaskBsCnt_ * axisK_;
}

template <typename XType, bool IsMxfp8, bool IsMxfp8E4M3>
__aicore__ inline void MoeDispatchMemory<XType, IsMxfp8, IsMxfp8E4M3>::ExpIdsCopyAndMaskCal()
{
    activeMaskBsCnt_ = axisBS_;
    sendToMoeExpTokenCnt_ = axisBS_ * axisK_;
    validExpertIdsTensor_ = expertIdsBuf_.Get<int32_t>();

    if (isTokenMaskFlag_) {
        TokenActiveMaskCal();
    }
    if (activeMaskBsCnt_ == 0U) {
        return;
    }

    Duplicate<int32_t>(validExpertIdsTensor_, -1,
        static_cast<int32_t>(expertIdsBufSize_ / sizeof(int32_t)));
    SyncFunc<AscendC::HardEvent::V_MTE2>();
    const uint32_t expertIdsCount = axisBS_ * axisK_;
    const uint32_t expertIdsAlignCount =
        Ceil(expertIdsCount, static_cast<uint32_t>(BITS_PER_BYTE)) * BITS_PER_BYTE;
    const uint32_t rightPadding = expertIdsAlignCount - expertIdsCount;
    const DataCopyPadExtParams<int32_t> allExpertIdsPadParams{
        true, 0U, static_cast<uint8_t>(rightPadding), -1};
    const DataCopyExtParams expertIdsCopyParams{
        1U, expertIdsCount * static_cast<uint32_t>(sizeof(int32_t)), 0U, 0U, 0U};
    DataCopyPad(validExpertIdsTensor_, expertIdsGMTensor_, expertIdsCopyParams,
        allExpertIdsPadParams);
    SyncFunc<AscendC::HardEvent::MTE2_S>();
    if (isTokenMaskFlag_) {
        for (uint32_t activeTokenIndex = 0U;
             activeTokenIndex < activeMaskBsCnt_; ++activeTokenIndex) {
            const uint32_t tokenIndex =
                static_cast<uint32_t>(validBsIndexTensor_.GetValue(activeTokenIndex));
            if (tokenIndex == activeTokenIndex) {
                continue;
            }
            for (uint32_t k = 0U; k < axisK_; ++k) {
                validExpertIdsTensor_.SetValue(activeTokenIndex * axisK_ + k,
                    validExpertIdsTensor_.GetValue(tokenIndex * axisK_ + k));
            }
        }
        SyncFunc<AscendC::HardEvent::S_V>();
    }
    SyncFunc<AscendC::HardEvent::MTE2_S>();
}

template <typename XType, bool IsMxfp8, bool IsMxfp8E4M3>
__aicore__ inline bool
MoeDispatchMemory<XType, IsMxfp8, IsMxfp8E4M3>::IsDoubleGroupRankSize() const
{
    return epWorldSize_ == TileXREp::kEpMemoryDoubleGroupWorldSize64 ||
        epWorldSize_ == TileXREp::kEpMemoryDoubleGroupWorldSize128;
}

template <typename XType, bool IsMxfp8, bool IsMxfp8E4M3>
__aicore__ inline void MoeDispatchMemory<XType, IsMxfp8, IsMxfp8E4M3>::CalSendRound()
{
    const bool doubleGroupEnabled = !isSmallBs_ && IsDoubleGroupRankSize() &&
        sharedExpertRankNum_ == 0U;
    sendRoundNum_ = doubleGroupEnabled ? TileXREp::kEpMemoryDoubleGroupRoundNum : 1U;
}

template <typename XType, bool IsMxfp8, bool IsMxfp8E4M3>
__aicore__ inline void
MoeDispatchMemory<XType, IsMxfp8, IsMxfp8E4M3>::SyncBeforeSendRound(uint32_t curRound)
{
    if (sendRoundNum_ > 1U && curRound > 0U) {
        PipeBarrier<PIPE_ALL>();
        SyncAll();
    }
}

#ifdef TILEXR_EP_COMPACT_ENABLED
template <typename XType, bool IsMxfp8, bool IsMxfp8E4M3>
__aicore__ inline bool MoeDispatchMemory<XType, IsMxfp8, IsMxfp8E4M3>::PrepareCompactOutput()
{
    // Only the last count core enters here, after its own send/count work. It
    // already owns the final prefix, so it can allocate without
    // waiting for unrelated senders. LocalWindowCopy retains the original per-core
    // count and per-payload waits; no producer is cancelled on allocation failure.
    uint64_t localError = TileXR::Elastic::Success;
    uint64_t hidden = 0;
    uint64_t rows = 0;
    const int32_t signedRows = compactRows_;
    rows = signedRows > 0 ? static_cast<uint64_t>(signedRows) : 0;
    uint64_t offset = 0;
    uint64_t bytes = 0;
    const uint64_t limit = ~uint64_t(0);
    if (signedRows < 0 || uint64_t(axisH_) > limit / 2 ||
        (rows != 0 && uint64_t(axisH_) * 2 > (limit - 63) / rows)) {
        localError = TileXR::Elastic::Invalid;
    } else {
        offset = (rows * uint64_t(axisH_) * 2 + 63) & ~uint64_t(63);
        if (rows > (limit - offset) / 12) localError = TileXR::Elastic::Invalid;
        else bytes = offset + rows * 12;
    }
    // Run completed this core's count/workspace/flag MTE3 before this Reset.
    tpipe_->Reset();
    if (localError == TileXR::Elastic::Success) {
        localError = TileXR::Elastic::AllocateBundle<128>(
            compactView_, bytes, 64, rows, &hidden, compactProfiler_);
    }
    ElasticProfile::Mark<ElasticProfile::RankGateBegin>(compactProfiler_);
    tpipe_->Reset();
    TBuf<> gateBuffer;
    tpipe_->InitBuffer(gateBuffer, 64);
    LocalTensor<uint64_t> words = gateBuffer.Get<uint64_t>();
    for (uint32_t i = 0; i < 8; ++i) words.SetValue(i, 0);
    words.SetValue(0, localError);
    words.SetValue(1, hidden);
    words.SetValue(2, rows);
    words.SetValue(4, compactMagic_);
    GlobalTensor<uint64_t> localGate;
    localGate.SetGlobalBuffer(reinterpret_cast<__gm__ uint64_t *>(
        GetWindStateAddrByRankId(epRankId_) + TileXREp::kCompactGateOffset));
    SyncFunc<HardEvent::S_MTE3>();
    DataCopy(localGate, words, 4);
    SyncFunc<HardEvent::MTE3_S>();
    DataCopy(localGate[4], words[4], 4);
    SyncFunc<HardEvent::MTE3_S>();
    // This descriptor remains immutable for the round, including on peer
    // failure. Other cores can copy local output while this owner checks peers.
    uint64_t finalError = localError;
    for (uint32_t peer = 0; peer < epWorldSize_; ++peer) {
        if (peer == epRankId_) continue;
        GlobalTensor<uint64_t> peerGate;
        peerGate.SetGlobalBuffer(reinterpret_cast<__gm__ uint64_t *>(
            GetWindStateAddrByRankId(peer) + TileXREp::kCompactGateOffset));
        do {
            DataCopy(words[4], peerGate[4], 4);
            SyncFunc<HardEvent::MTE2_S>();
        } while (words.GetValue(4) != compactMagic_);
        DataCopy(words, peerGate, 4);
        SyncFunc<HardEvent::MTE2_S>();
        if (finalError == TileXR::Elastic::Success && words.GetValue(0) != TileXR::Elastic::Success)
            finalError = words.GetValue(0);
    }
    if (finalError != TileXR::Elastic::Success) {
        // Metadata may already back an ordinary Tensor. Preserve it and report
        // the peer error separately for the final producer completion check.
        TileXR::Elastic::DeviceDetail::Invalidate(compactView_.mailbox,
            offsetof(TileXR::Elastic::Mailbox, requestId) / sizeof(uint64_t));
        const uint64_t sequence = TileXR::Elastic::DeviceDetail::ReadWord(
            compactView_.mailbox, offsetof(TileXR::Elastic::Mailbox, requestId) / sizeof(uint64_t));
        uint64_t failure[8] = {0, finalError, 0, 0, 0, 0, 0, 0};
        TileXR::Elastic::DeviceDetail::Publish(compactView_.mailbox +
            offsetof(TileXR::Elastic::Mailbox, finalStatus), failure, sequence, words);
    }
    ElasticProfile::Mark<ElasticProfile::RankGateEnd>(compactProfiler_);
    if (localError != TileXR::Elastic::Success) return false;
    expandXOutGM_ = reinterpret_cast<GM_ADDR>(hidden);
    assistInfoForCombineOutGM_ = reinterpret_cast<GM_ADDR>(hidden +
        ((rows * uint64_t(axisH_) * 2 + 63) & ~uint64_t(63)));
    return true;
}

template <typename XType, bool IsMxfp8, bool IsMxfp8E4M3>
__aicore__ inline bool MoeDispatchMemory<XType, IsMxfp8, IsMxfp8E4M3>::WaitCompactOutput()
{
    // Called after count flags are consumed, and only by lanes assigned output.
    ElasticProfile::Mark<ElasticProfile::LocalReadyWaitBegin>(compactProfiler_);
    auto words = statusFp32Tensor_.ReinterpretCast<uint64_t>();
    GlobalTensor<uint64_t> localGate;
    localGate.SetGlobalBuffer(reinterpret_cast<__gm__ uint64_t *>(
        GetWindStateAddrByRankId(epRankId_) + TileXREp::kCompactGateOffset));
    do {
        DataCopy(words, localGate[4], 4);
        SyncFunc<HardEvent::MTE2_S>();
    } while (words.GetValue(0) != compactMagic_);
    // Observe the release flag before fetching its immutable body; a single
    // 64B DMA is not assumed to take an atomic flag+body snapshot.
    DataCopy(words, localGate, 4);
    SyncFunc<HardEvent::MTE2_S>();
    const uint64_t localError = words.GetValue(0);
    const uint64_t hidden = words.GetValue(1);
    const uint64_t rows = words.GetValue(2);
    ElasticProfile::Mark<ElasticProfile::LocalReadyWaitEnd>(compactProfiler_);
    if (localError != TileXR::Elastic::Success) return false;
    expandXOutGM_ = reinterpret_cast<GM_ADDR>(hidden);
    assistInfoForCombineOutGM_ = reinterpret_cast<GM_ADDR>(hidden +
        ((rows * uint64_t(axisH_) * 2 + 63) & ~uint64_t(63)));
    return true;
}

#endif

template <typename XType, bool IsMxfp8, bool IsMxfp8E4M3>
__aicore__ inline void MoeDispatchMemory<XType, IsMxfp8, IsMxfp8E4M3>::Run()
{
    if ASCEND_IS_AIV {
#ifdef TILEXR_EP_COMPACT_ENABLED
        ElasticProfile::Mark<ElasticProfile::SendAndCountBegin>(compactProfiler_);
#endif
        if (aivId_ < aivUsedAllToAll_) {
            AllToAllDispatch();
        } else {
            CalCumSum();
        }

        PipeBarrier<PIPE_ALL>();
#ifdef TILEXR_EP_COMPACT_ENABLED
        ElasticProfile::Mark<ElasticProfile::SendAndCountEnd>(compactProfiler_);
        if (aivId_ != aivNum_ - 1U || PrepareCompactOutput()) {
            // Only output lanes receive count flags and wait for their local allocation.
            LocalWindowCopy();
        }
#else
        LocalWindowCopy();
#endif
    }
    AscendC::SetCtrlSpr<FLOAT_OVERFLOW_MODE_CTRL, FLOAT_OVERFLOW_MODE_CTRL>(originalOverflowMode_);
}

template <typename XType, bool IsMxfp8, bool IsMxfp8E4M3>
__aicore__ inline void RunMoeDispatchMemoryTyped(
    GM_ADDR commArgsGM, GM_ADDR xGM, GM_ADDR expertIdsGM, GM_ADDR xActiveMaskGM,
    GM_ADDR expandXOutGM, GM_ADDR dynamicScalesOutGM, GM_ADDR expertTokenNumsOutGM,
    GM_ADDR sendCountsOutGM, GM_ADDR assistInfoForCombineOutGM, int64_t bs, int64_t h,
    int64_t topK, int64_t moeExpertNum, int64_t sharedExpertNum,
    int64_t sharedExpertRankNum,
    int64_t globalBs, int64_t expertTokenNumsType, int64_t activeMaskType, int64_t quantMode,
    int64_t expandXOutDtype, int64_t magic, TPipe *pipe)
{
    MoeDispatchMemory<XType, IsMxfp8, IsMxfp8E4M3> op;
    op.Init(commArgsGM, xGM, expertIdsGM, xActiveMaskGM, expandXOutGM, dynamicScalesOutGM,
        expertTokenNumsOutGM, sendCountsOutGM, assistInfoForCombineOutGM, bs, h, topK,
        moeExpertNum, sharedExpertNum, sharedExpertRankNum, globalBs, expertTokenNumsType,
        activeMaskType, quantMode, expandXOutDtype, magic, pipe);
    op.Run();
}

__aicore__ inline void RunMoeDispatchMemory(
    GM_ADDR commArgsGM, GM_ADDR xGM, GM_ADDR expertIdsGM, GM_ADDR xActiveMaskGM,
    GM_ADDR expandXOutGM, GM_ADDR dynamicScalesOutGM, GM_ADDR expertTokenNumsOutGM,
    GM_ADDR sendCountsOutGM, GM_ADDR assistInfoForCombineOutGM, int64_t bs, int64_t h,
    int64_t topK, int64_t moeExpertNum, int64_t sharedExpertNum,
    int64_t sharedExpertRankNum,
    int64_t globalBs, int64_t expertTokenNumsType, int64_t activeMaskType, int64_t quantMode,
    int64_t dtype, int64_t expandXOutDtype, int64_t magic, TPipe *pipe)
{
    if ((activeMaskType != ACTIVE_MASK_NONE && activeMaskType != ACTIVE_MASK_TOKEN) ||
        (quantMode != 0 && quantMode != MX_QUANT)) {
        return;
    }
    if (dtype == TileXR::TILEXR_DATA_TYPE_FP16) {
        if (quantMode == 0 && expandXOutDtype == TileXR::TILEXR_DATA_TYPE_FP16) {
            RunMoeDispatchMemoryTyped<half, false, false>(commArgsGM, xGM,
                expertIdsGM, xActiveMaskGM, expandXOutGM, dynamicScalesOutGM,
                expertTokenNumsOutGM, sendCountsOutGM, assistInfoForCombineOutGM, bs, h, topK,
                moeExpertNum, sharedExpertNum, sharedExpertRankNum, globalBs, expertTokenNumsType,
                activeMaskType, quantMode, expandXOutDtype, magic, pipe);
        } else if (quantMode == MX_QUANT && expandXOutDtype == TileXR::TILEXR_DATA_TYPE_FP8E4M3) {
            RunMoeDispatchMemoryTyped<half, true, true>(commArgsGM, xGM,
                expertIdsGM, xActiveMaskGM, expandXOutGM, dynamicScalesOutGM,
                expertTokenNumsOutGM, sendCountsOutGM, assistInfoForCombineOutGM, bs, h, topK,
                moeExpertNum, sharedExpertNum, sharedExpertRankNum, globalBs, expertTokenNumsType,
                activeMaskType, quantMode, expandXOutDtype, magic, pipe);
        } else if (quantMode == MX_QUANT && expandXOutDtype == TileXR::TILEXR_DATA_TYPE_FP8E5M2) {
            RunMoeDispatchMemoryTyped<half, true, false>(commArgsGM, xGM,
                expertIdsGM, xActiveMaskGM, expandXOutGM, dynamicScalesOutGM,
                expertTokenNumsOutGM, sendCountsOutGM, assistInfoForCombineOutGM, bs, h, topK,
                moeExpertNum, sharedExpertNum, sharedExpertRankNum, globalBs, expertTokenNumsType,
                activeMaskType, quantMode, expandXOutDtype, magic, pipe);
        }
    } else if (dtype == TileXR::TILEXR_DATA_TYPE_BFP16) {
        if (quantMode == 0 && expandXOutDtype == TileXR::TILEXR_DATA_TYPE_BFP16) {
            RunMoeDispatchMemoryTyped<bfloat16_t, false, false>(commArgsGM, xGM,
                expertIdsGM, xActiveMaskGM, expandXOutGM, dynamicScalesOutGM,
                expertTokenNumsOutGM, sendCountsOutGM, assistInfoForCombineOutGM, bs, h, topK,
                moeExpertNum, sharedExpertNum, sharedExpertRankNum, globalBs, expertTokenNumsType,
                activeMaskType, quantMode, expandXOutDtype, magic, pipe);
        } else if (quantMode == MX_QUANT && expandXOutDtype == TileXR::TILEXR_DATA_TYPE_FP8E4M3) {
            RunMoeDispatchMemoryTyped<bfloat16_t, true, true>(commArgsGM, xGM,
                expertIdsGM, xActiveMaskGM, expandXOutGM, dynamicScalesOutGM,
                expertTokenNumsOutGM, sendCountsOutGM, assistInfoForCombineOutGM, bs, h, topK,
                moeExpertNum, sharedExpertNum, sharedExpertRankNum, globalBs, expertTokenNumsType,
                activeMaskType, quantMode, expandXOutDtype, magic, pipe);
        } else if (quantMode == MX_QUANT && expandXOutDtype == TileXR::TILEXR_DATA_TYPE_FP8E5M2) {
            RunMoeDispatchMemoryTyped<bfloat16_t, true, false>(commArgsGM, xGM,
                expertIdsGM, xActiveMaskGM, expandXOutGM, dynamicScalesOutGM,
                expertTokenNumsOutGM, sendCountsOutGM, assistInfoForCombineOutGM, bs, h, topK,
                moeExpertNum, sharedExpertNum, sharedExpertRankNum, globalBs, expertTokenNumsType,
                activeMaskType, quantMode, expandXOutDtype, magic, pipe);
        }
    }
}

} // namespace Mc2Kernel

#endif // TILEXR_EP_KERNELS_TILEXR_EP_DISPATCH_MEMORY_KERNEL_H
