#ifndef TILEXR_EP_SERVER_DEDUP_RECV_H
#define TILEXR_EP_SERVER_DEDUP_RECV_H

#include "kernel_operator.h"
#include "ep_memory_server_dedup_data_layout.h"
#include "ep_memory_server_dedup_protocol.h"
#include "tilexr_ep_server_dedup_receive_tasks.h"
#include "tilexr_ep_server_dedup_receive_packet.h"

namespace TileXREp {
using namespace AscendC;

// The receive side owns the Dispatch slots of this rank.  Its work is kept
// separate from ServerDedupCounts because the prefix table is the only
// synchronization boundary between count exchange and payload processing.
template<class XType, bool Quantized, bool E4M3>
class ServerDedupReceiver {
public:
    __aicore__ inline void Init(const __gm__ TileXR::CommArgs *args,
        GM_ADDR expandX, GM_ADDR dynamicScales, GM_ADDR assistEx, GM_ADDR group,
        uint32_t bs, uint32_t h,
        uint32_t k, uint32_t experts, TPipe *pipe)
    {
        args_ = args;
        pipe_ = pipe;
        bs_ = bs;
        h_ = h;
        k_ = k;
        experts_ = experts;
        localExperts_ = experts / static_cast<uint32_t>(args_->rankSize);
        receiver_ = GetBlockIdx() - kEpServerDedupSendCoreNum;
        hiddenBytes_ = uint32_t(Quantized ? h : h * sizeof(XType));
        data_ = TileXREpServerDedupDataLayout(bs, h, k, experts, Quantized);
        expandX_.SetGlobalBuffer(reinterpret_cast<__gm__ uint8_t *>(expandX));
        dynamicScales_.SetGlobalBuffer(reinterpret_cast<__gm__ uint8_t *>(dynamicScales));
        assistEx_.SetGlobalBuffer(reinterpret_cast<__gm__ uint32_t *>(assistEx));
        group_.SetGlobalBuffer(reinterpret_cast<__gm__ uint32_t *>(group));
    }

    __aicore__ inline void Process()
    {
        // Prefix copies are published by CompleteCumsum.  Keep one complete
        // copy local so slot enumeration never depends on a mutable pending
        // index or on the arrival order of payloads.
        const uint32_t prefixBytes = uint32_t(TileXREpDedupAlign(
            uint64_t(experts_) * sizeof(int32_t), 32U));
        const uint32_t packetBytes = uint32_t(data_.tokenStride);
        const uint32_t blockCount = uint32_t(data_.blockCount);

        pipe_->Reset();
        TBuf<> prefixBuf, packetBuf, payloadBuf, flagBuf, relayBuf, groupBuf, metaBuf, sumBuf, reduceBuf;
        pipe_->InitBuffer(prefixBuf, prefixBytes);
        pipe_->InitBuffer(packetBuf, packetBytes);
        pipe_->InitBuffer(payloadBuf, uint32_t(data_.packedPayloadBytes));
        pipe_->InitBuffer(flagBuf, blockCount * 32U);
        pipe_->InitBuffer(sumBuf, 32U);
        pipe_->InitBuffer(reduceBuf, blockCount * 32U);
        pipe_->InitBuffer(relayBuf, packetBytes);
        pipe_->InitBuffer(groupBuf, uint32_t(TileXREpDedupAlign(
            kEpServerDedupGroupMembersOffset + uint64_t(k_ - 1U) * sizeof(EpServerDedupGroupMember), 32U)));
        pipe_->InitBuffer(metaBuf, 32U);

        auto prefix = prefixBuf.Get<int32_t>();
        GlobalTensor<int32_t> prefixGlobal;
        const auto countLayout = TileXREpServerDedupCountLayout(experts_);
        prefixGlobal.SetGlobalBuffer(reinterpret_cast<__gm__ int32_t *>(
            args_->peerMems[args_->rank] + countLayout.cntOffset + countLayout.prefixOffset +
            uint64_t(receiver_) * countLayout.prefixStride));
        DataCopyPad(prefix, prefixGlobal,
            {1U, prefixBytes, 0U, 0U, 0U}, {false, 0U, 0U, 0});
        Sync<HardEvent::MTE2_S>();

        ServerDedupReceiveTasks tasks;
        tasks.Build(prefix, uint32_t(args_->rank), uint32_t(args_->rankSize),
            experts_, bs_, receiver_, pipe_);
        if (tasks.Count() == 0U) return;

        auto packet = packetBuf.Get<uint8_t>();
        auto payload = payloadBuf.Get<uint8_t>();
        auto flags = flagBuf.Get<float>();
        auto relay = relayBuf.Get<uint8_t>();
        auto group = groupBuf.Get<uint32_t>();
        auto meta = metaBuf.Get<uint32_t>();
        auto sum = sumBuf.Get<float>();
        auto reduce = reduceBuf.Get<float>();

        for (uint32_t pending = 0U; pending < tasks.PendingCount(); ++pending) {
            const uint32_t stable = tasks.PendingIndex(pending);
            const uint32_t encoded = tasks.Position(stable);
            const uint32_t region = encoded / bs_;
            const uint32_t q = encoded % bs_;
            const uint32_t source = (region / localExperts_ + uint32_t(args_->rankSize) -
                uint32_t(args_->rank)) % uint32_t(args_->rankSize);
            ReceiveOne(source, region % localExperts_, q,
                tasks.Begin() + stable, packet, payload, flags, relay, group, meta, sum, reduce);
        }
    }

private:
    template<HardEvent event> __aicore__ inline void Sync()
    {
        const TEventID id = pipe_->FetchEventID(event);
        SetFlag<event>(id);
        WaitFlag<event>(id);
    }

    __aicore__ inline __gm__ uint8_t *SlotAddress(uint32_t source, uint32_t localExpert, uint32_t q) const
    {
        const uint32_t destination = uint32_t(args_->rank);
        return args_->peerMems[destination] + TileXREpServerDedupDispatchOffset(
            data_, source, destination, localExpert, q, uint32_t(args_->rankSize),
            localExperts_, bs_);
    }

    __aicore__ inline void ClearFlags(__gm__ uint8_t *slot, LocalTensor<float> flags)
    {
        Duplicate<float>(flags, 0.0f, uint32_t(data_.blockCount) * 8U);
        Sync<HardEvent::V_MTE3>();
        GlobalTensor<float> flagGlobal;
        flagGlobal.SetGlobalBuffer(reinterpret_cast<__gm__ float *>(slot + 480U));
        DataCopy(flagGlobal, flags, DataCopyParams{
            uint16_t(data_.blockCount), 1U, 0U, 15U});
        Sync<HardEvent::MTE3_S>();
    }

    __aicore__ inline void WriteAssist(uint32_t row, LocalTensor<uint32_t> tail)
    {
        Sync<HardEvent::S_MTE3>();
        DataCopyPad(assistEx_[uint64_t(row) * 8U], tail,
            {1U, uint32_t(sizeof(EpServerDedupAssistEx)), 0U, 0U, 0U});
        Sync<HardEvent::MTE3_S>();
    }

    __aicore__ inline uint32_t AllocateRelayIndex()
    {
        auto layout = TileXREpServerDedupCountLayout(experts_);
        auto *counter = reinterpret_cast<__gm__ uint32_t *>(
            args_->peerMems[args_->rank] + layout.counterOffset);
        return AtomicAdd(counter, 1U);
    }

    __aicore__ inline void SendForward(LocalTensor<uint8_t> relay,
        uint32_t srcRank, uint32_t destinationExpert, uint32_t destinationQ)
    {
        const uint32_t destination = destinationExpert / localExperts_;
        GlobalTensor<uint8_t> destinationGm;
        destinationGm.SetGlobalBuffer(args_->peerMems[destination] +
            TileXREpServerDedupDispatchOffset(data_, srcRank, destination,
                destinationExpert % localExperts_, destinationQ,
                uint32_t(args_->rankSize), localExperts_, bs_));
        DataCopy(destinationGm, relay, uint32_t(data_.tokenStride));
    }

    __aicore__ inline void ReceiveOne(uint32_t source, uint32_t localExpert, uint32_t q,
        uint32_t row, LocalTensor<uint8_t> packet, LocalTensor<uint8_t> payload,
        LocalTensor<float> flags, LocalTensor<uint8_t> relay, LocalTensor<uint32_t> group,
        LocalTensor<uint32_t> meta, LocalTensor<float> sum, LocalTensor<float> reduce)
    {
        __gm__ uint8_t *const slot = SlotAddress(source, localExpert, q);
        while (!CheckServerDedupPayloadReady(slot, uint32_t(data_.blockCount), flags, sum, reduce, pipe_)) {}
        LoadServerDedupPayload(slot, uint32_t(data_.blockCount), packet, payload, pipe_);
        Sync<HardEvent::V_MTE3>();
        DataCopyPad(expandX_[uint64_t(row) * hiddenBytes_], payload,
            {1U, hiddenBytes_, 0U, 0U, 0U});
        if constexpr (Quantized) {
            DataCopyPad(dynamicScales_[uint64_t(row) * data_.scaleBytes],
                payload[data_.quantCount],
                {1U, uint32_t(data_.scaleBytes), 0U, 0U, 0U});
        }
        Sync<HardEvent::MTE3_S>();

        auto tail = payload[data_.tailOffset].template ReinterpretCast<uint32_t>();
        const int32_t forwardCount = int32_t(tail.GetValue(4U));
        if (forwardCount > 0) {
            const uint32_t relayIndex = AllocateRelayIndex();
            BuildServerDedupAssist(meta, tail, uint32_t(args_->rank) * localExperts_ + localExpert,
                uint32_t(args_->rank), relayIndex);
            WriteAssist(row, meta);
            BuildServerDedupGroup(group, tail, row, relayIndex, k_);
            PrepareServerDedupForward(relay, payload, data_, uint32_t(args_->rank), relayIndex, pipe_);

            for (uint32_t i = 0U; i < uint32_t(forwardCount); ++i) {
                const uint32_t entry = 8U + i * 4U;
                const uint32_t destinationExpert = uint32_t(tail.GetValue(entry));
                const uint32_t destinationQ = uint32_t(tail.GetValue(entry + 1U));
                SendForward(relay, tail.GetValue(0U), destinationExpert, destinationQ);
            }
            const uint32_t groupStride = uint32_t(TileXREpDedupAlign(
                kEpServerDedupGroupMembersOffset + uint64_t(k_ - 1U) * sizeof(EpServerDedupGroupMember), 32U));
            DataCopyPad(group_[uint64_t(relayIndex) * groupStride / sizeof(uint32_t)], group,
                {1U, groupStride, 0U, 0U, 0U});
            Sync<HardEvent::MTE3_S>();
        } else {
            BuildServerDedupAssist(meta, tail, uint32_t(args_->rank) * localExperts_ + localExpert, 0U, 0U);
            WriteAssist(row, meta);
        }
        ClearFlags(slot, flags);
    }

    const __gm__ TileXR::CommArgs *args_ = nullptr;
    TPipe *pipe_ = nullptr;
    uint32_t bs_ = 0U, h_ = 0U, k_ = 0U, experts_ = 0U, localExperts_ = 0U;
    uint32_t receiver_ = 0U;
    uint32_t hiddenBytes_ = 0U;
    EpServerDedupDataLayout data_ {};
    GlobalTensor<uint8_t> expandX_;
    GlobalTensor<uint8_t> dynamicScales_;
    GlobalTensor<uint32_t> assistEx_, group_;
};
} // namespace TileXREp

#endif
