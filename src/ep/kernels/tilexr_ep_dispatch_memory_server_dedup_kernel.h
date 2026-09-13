#ifndef TILEXR_EP_DISPATCH_MEMORY_SERVER_DEDUP_KERNEL_H
#define TILEXR_EP_DISPATCH_MEMORY_SERVER_DEDUP_KERNEL_H

#include "tilexr_ep_server_dedup_counts.h"
#include "tilexr_ep_server_dedup_send.h"
#include "tilexr_ep_server_dedup_recv.h"
#include "tilexr_ep_server_dedup_profile.h"

namespace TileXREp {
template<class XType, bool Quantized, bool E4M3>
class DispatchMemoryServerDedupKernel {
public:
    __aicore__ inline void Init(GM_ADDR commArgs, GM_ADDR x, GM_ADDR expertIds, GM_ADDR expertScales,
        GM_ADDR expandXOut, GM_ADDR dynamicScalesOut, GM_ADDR expertTokenNumsOut, GM_ADDR sendCountsOut,
        GM_ADDR assistExOut, GM_ADDR sourceMaskOut, GM_ADDR groupOut, GM_ADDR expertScalesOut,
        uint32_t bs, uint32_t h, uint32_t topK,
        uint32_t experts, uint32_t expertTokenNumsType, int64_t magic, TPipe *pipe)
    {
        profile_.Mark(0);
        args_ = reinterpret_cast<const __gm__ TileXR::CommArgs *>(commArgs);
        core_ = GetBlockIdx(); magic_ = magic; pipe_ = pipe;
        counterOffset_ = TileXREpServerDedupCountLayout(experts).counterOffset;
        counts_.Init(commArgs, expertIds, expertTokenNumsOut, sendCountsOut, bs, topK, experts, expertTokenNumsType, magic, pipe);
        // Bind the complete output ABI; final expert rows are owned by the future receiver.
        expandX_.SetGlobalBuffer(reinterpret_cast<__gm__ uint8_t *>(expandXOut));
        dynamicScales_.SetGlobalBuffer(reinterpret_cast<__gm__ uint8_t *>(dynamicScalesOut));
        sourceWeights_.SetGlobalBuffer(reinterpret_cast<__gm__ float *>(expertScalesOut));
        if (core_ < kEpServerDedupSendCoreNum)
            sender_.Init(args_, x, expertIds, expertScales, sourceMaskOut, expertScalesOut, bs, h, topK, experts, pipe);
        else
            receiver_.Init(args_, expandXOut, dynamicScalesOut, assistExOut, groupOut, bs, h, topK, experts, pipe);
        profile_.Mark(1);
    }

    __aicore__ inline void Process()
    {
        counts_.SendCnt();
        profile_.Mark(2);
        if (core_ < kEpServerDedupSendCoreNum) {
            sender_.Process();
            profile_.Mark(3);
            profile_.Skip(4, 3); profile_.Skip(5, 3); profile_.Skip(6, 3);
        } else {
            profile_.Skip(3, 2);
            counts_.RecvCnt(); profile_.Mark(4);
            counts_.CalcCumsum(); profile_.Mark(5);
            counts_.CompleteCumsum(); profile_.Mark(6);
            receiver_.Process();
        }
        counts_.Finish();
        // All 64 cores participate exactly once, after their GM writes complete.
        SyncAll();
        if (core_ == kEpServerDedupCoreNum - 1U) {
            pipe_->Reset();
            TBuf<> counter;
            pipe_->InitBuffer(counter, 32U);
            auto zero = counter.Get<uint32_t>();
            zero.SetValue(0, 0U);
            const TEventID before = pipe_->FetchEventID(HardEvent::S_MTE3);
            SetFlag<HardEvent::S_MTE3>(before); WaitFlag<HardEvent::S_MTE3>(before);
            GlobalTensor<uint32_t> output;
            output.SetGlobalBuffer(reinterpret_cast<__gm__ uint32_t *>(args_->peerMems[args_->rank] + counterOffset_));
            DataCopyPad(output, zero, {1U, 4U, 0U, 0U, 0U});
            const TEventID after = pipe_->FetchEventID(HardEvent::MTE3_S);
            SetFlag<HardEvent::MTE3_S>(after); WaitFlag<HardEvent::MTE3_S>(after);
        }
        profile_.Mark(7);
        profile_.Flush(args_, magic_);
        if (core_ < kEpServerDedupSendCoreNum) sender_.FlushProfile(magic_);
    }
private:
    ServerDedupCounts counts_;
    ServerDedupSender<XType, Quantized, E4M3> sender_;
    ServerDedupReceiver<XType, Quantized, E4M3> receiver_;
    ServerDedupProfile profile_;
    const __gm__ TileXR::CommArgs *args_ = nullptr;
    TPipe *pipe_ = nullptr;
    uint32_t core_ = 0;
    int64_t magic_ = 0;
    uint64_t counterOffset_ = 0;
    GlobalTensor<uint8_t> expandX_, dynamicScales_;
    GlobalTensor<float> sourceWeights_;
};
} // namespace TileXREp
#endif
