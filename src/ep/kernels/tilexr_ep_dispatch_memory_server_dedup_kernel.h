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
        GM_ADDR tokenTypeOut, GM_ADDR destinationIndexOut, GM_ADDR relayReadIndexOut,
        GM_ADDR sourceMaskOut, GM_ADDR expertScalesOut,
        uint32_t bs, uint32_t h, uint32_t topK,
        uint32_t experts, uint32_t expertTokenNumsType, int64_t magic, uint32_t profilingLaunchSlot, TPipe *pipe)
    {
        profile_.Init(commArgs, profilingLaunchSlot);
        profile_.Mark(0);
        args_ = reinterpret_cast<const __gm__ TileXR::CommArgs *>(commArgs);
        core_ = GetBlockIdx(); pipe_ = pipe;
        counts_.Init(commArgs, expertIds, expertTokenNumsOut, sendCountsOut, bs, topK, experts, expertTokenNumsType, magic, pipe);
        if (core_ < kEpServerDedupSendCoreNum)
            sender_.Init(args_, x, expertIds, expertScales, sourceMaskOut, expertScalesOut, bs, h, topK, experts, pipe, magic);
        else
            receiver_.Init(args_, expandXOut, dynamicScalesOut, tokenTypeOut, destinationIndexOut, relayReadIndexOut, bs, h, topK, experts, pipe, magic);
        profile_.Mark(1);
    }

    __aicore__ inline void Process()
    {
        counts_.SendCnt(profile_);
        profile_.Mark(2);
        if (core_ < kEpServerDedupSendCoreNum) {
            sender_.Process(profile_, counts_.ScratchPool(), counts_.ReservedBytes());
            profile_.Mark(3);
        } else {
            counts_.RecvCnt(); profile_.Mark(4);
            counts_.CalcCumsum(); profile_.Mark(5);
            counts_.CompleteCumsum(); profile_.Mark(6);
            // CompleteCumsum may still be writing expert counts from its UB.
            // Retire that stage before receiver resets the pipe and reloads GM.
            PipeBarrier<PIPE_ALL>();
            receiver_.Process(profile_); profile_.Mark(7);
        }
        profile_.Mark(EpDedupDetailPoint::FinishBegin);
        counts_.Finish();
        profile_.Mark(EpDedupDetailPoint::LocalEnd);
        // No shared work follows local completion. Same-stream kernel completion
        // orders the next launch after all cores finish their writes and cleanup.
        profile_.Mark(8);
        profile_.Publish();
    }
private:
    ServerDedupCounts counts_;
    ServerDedupSender<XType, Quantized, E4M3> sender_;
    ServerDedupReceiver<XType, Quantized, E4M3> receiver_;
    ServerDedupProfile profile_;
    const __gm__ TileXR::CommArgs *args_ = nullptr;
    TPipe *pipe_ = nullptr;
    uint32_t core_ = 0;
};
} // namespace TileXREp
#endif
