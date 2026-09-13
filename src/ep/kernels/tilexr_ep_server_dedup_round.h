#ifndef TILEXR_EP_SERVER_DEDUP_ROUND_H
#define TILEXR_EP_SERVER_DEDUP_ROUND_H

#include "kernel_operator.h"
#include "ep_memory_server_dedup_count_layout.h"

namespace TileXREp {

// Called by core 63 after the existing all-core completion and counter reset.
// Each producer owns one 32B epoch record on every peer. No shared flag reset:
// >= accepts a peer already in a later call without missing this completion.
__aicore__ inline void CompleteServerDedupRound(const __gm__ TileXR::CommArgs *args,
    int64_t magic, AscendC::TPipe *pipe)
{
    using namespace AscendC;
    pipe->Reset();
    TBuf<> sendBuf, recvBuf;
    pipe->InitBuffer(sendBuf, 32U);
    pipe->InitBuffer(recvBuf, 32U);
    auto send = sendBuf.Get<int64_t>();
    auto recv = recvBuf.Get<int64_t>();
    for (uint32_t word = 0; word < 4U; ++word) send.SetValue(word, magic);
    const TEventID publish = pipe->FetchEventID(HardEvent::S_MTE3);
    SetFlag<HardEvent::S_MTE3>(publish); WaitFlag<HardEvent::S_MTE3>(publish);
    const uint64_t offset = TileXREpServerDedupRoundOffset(args->peerMemBytes);
    for (int peer = 0; peer < args->rankSize; ++peer) {
        GlobalTensor<int64_t> target;
        target.SetGlobalBuffer(reinterpret_cast<__gm__ int64_t *>(
            args->peerMems[peer] + offset + uint64_t(args->rank) * 32U));
        DataCopy(target, send, 4U);
    }
    const TEventID sent = pipe->FetchEventID(HardEvent::MTE3_S);
    SetFlag<HardEvent::MTE3_S>(sent); WaitFlag<HardEvent::MTE3_S>(sent);
    GlobalTensor<int64_t> inbox;
    inbox.SetGlobalBuffer(reinterpret_cast<__gm__ int64_t *>(args->peerMems[args->rank] + offset));
    for (int peer = 0; peer < args->rankSize; ++peer) {
        bool ready = false;
        while (!ready) {
            DataCopy(recv, inbox[uint64_t(peer) * 4U], 4U);
            const TEventID loaded = pipe->FetchEventID(HardEvent::MTE2_S);
            SetFlag<HardEvent::MTE2_S>(loaded); WaitFlag<HardEvent::MTE2_S>(loaded);
            ready = true;
            for (uint32_t word = 0; word < 4U; ++word)
                ready = ready && recv.GetValue(word) >= magic;
        }
    }
}
} // namespace TileXREp
#endif
