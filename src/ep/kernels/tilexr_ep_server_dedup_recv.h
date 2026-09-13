#ifndef TILEXR_EP_SERVER_DEDUP_RECV_H
#define TILEXR_EP_SERVER_DEDUP_RECV_H

#include "kernel_operator.h"
#include "ep_memory_server_dedup_data_layout.h"
#include "ep_memory_server_dedup_protocol.h"

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
        const uint32_t hiddenBytes = uint32_t(Quantized ? h_ : h_ * sizeof(XType));
        const uint32_t blockCount = uint32_t(data_.blockCount);
        const uint32_t payloadDataBytes = uint32_t(Quantized ? data_.quantCount + data_.scaleBytes : hiddenBytes);
        const uint32_t hiddenBlocks = uint32_t((payloadDataBytes + 479U) / 480U);
        const uint32_t hiddenPackedBytes = hiddenBlocks * 480U;

        pipe_->Reset();
        TBuf<> prefixBuf, packetBuf, hiddenBuf, flagBuf, relayBuf, groupBuf, scalarBuf;
        pipe_->InitBuffer(prefixBuf, prefixBytes);
        pipe_->InitBuffer(packetBuf, packetBytes);
        pipe_->InitBuffer(hiddenBuf, hiddenPackedBytes);
        pipe_->InitBuffer(flagBuf, blockCount * 32U);
        pipe_->InitBuffer(relayBuf, packetBytes);
        pipe_->InitBuffer(groupBuf, uint32_t(TileXREpDedupAlign(
            kEpServerDedupGroupMembersOffset + uint64_t(k_ - 1U) * sizeof(EpServerDedupGroupMember), 32U)));
        pipe_->InitBuffer(scalarBuf, 64U);

        auto prefix = prefixBuf.Get<int32_t>();
        GlobalTensor<int32_t> prefixGlobal;
        const auto countLayout = TileXREpServerDedupCountLayout(experts_);
        prefixGlobal.SetGlobalBuffer(reinterpret_cast<__gm__ int32_t *>(
            args_->peerMems[args_->rank] + countLayout.prefixOffset +
            uint64_t(receiver_) * countLayout.prefixStride));
        DataCopyPad(prefix, prefixGlobal,
            {1U, prefixBytes, 0U, 0U, 0U}, {false, 0U, 0U, 0});
        Sync<HardEvent::MTE2_S>();

        const uint32_t total = prefix.GetValue(experts_ - 1U) < 0 ? 0U :
            uint32_t(prefix.GetValue(experts_ - 1U));
        uint32_t base = total / kEpServerDedupRecvCoreNum;
        uint32_t rem = total % kEpServerDedupRecvCoreNum;
        const uint32_t begin = receiver_ * base + (receiver_ < rem ? receiver_ : rem);
        const uint32_t count = base + (receiver_ < rem ? 1U : 0U);
        const uint32_t end = begin + count;
        if (count == 0U) return;

        auto packet = packetBuf.Get<uint8_t>();
        auto hidden = hiddenBuf.Get<uint8_t>();
        auto flags = flagBuf.Get<float>();
        auto relay = relayBuf.Get<uint8_t>();
        auto group = groupBuf.Get<uint32_t>();
        auto scalar = scalarBuf.Get<int32_t>();

        const uint32_t tableBytes = uint32_t(TileXREpDedupAlign(uint64_t(count) * sizeof(uint32_t), 32U));
        TBuf<> tokenPosBuf, pendingIdxBuf;
        pipe_->InitBuffer(tokenPosBuf, tableBytes);
        pipe_->InitBuffer(pendingIdxBuf, tableBytes);
        auto tokenPos = tokenPosBuf.Get<uint32_t>();
        auto pendingIdx = pendingIdxBuf.Get<uint32_t>();

        uint32_t row = 0U;
        uint32_t localCount = 0U;
        uint32_t previous = 0U;
        for (uint32_t record = 0U; record < experts_; ++record) {
            const uint32_t cumulative = prefix.GetValue(record) < 0 ? previous :
                uint32_t(prefix.GetValue(record));
            const uint32_t recordCount = cumulative >= previous ? cumulative - previous : 0U;
            const uint32_t source = record % uint32_t(args_->rankSize);
            const uint32_t localExpert = record / uint32_t(args_->rankSize);
            if (localExpert >= localExperts_) {
                previous = cumulative;
                continue;
            }
            for (uint32_t q = 0U; q < recordCount; ++q) {
                if (row >= begin && row < end) {
                    // Stable token position is independent of pendingIdx's
                    // compacting order.  The record index is localExpert*R+source.
                    tokenPos.SetValue(localCount, record * bs_ + q);
                    pendingIdx.SetValue(localCount, localCount);
                    ++localCount;
                }
                ++row;
            }
            previous = cumulative;
            if (row >= end) break;
        }
        for (uint32_t pending = 0U; pending < localCount; ++pending) {
            const uint32_t stable = pendingIdx.GetValue(pending);
            const uint32_t encoded = tokenPos.GetValue(stable);
            const uint32_t record = encoded / bs_;
            const uint32_t q = encoded % bs_;
            ReceiveOne(record % uint32_t(args_->rankSize), record / uint32_t(args_->rankSize), q,
                begin + stable, packet, hidden, flags, relay, group, scalar);
        }
    }

private:
    template<HardEvent event> __aicore__ inline void Sync()
    {
        const TEventID id = pipe_->FetchEventID(event);
        SetFlag<event>(id);
        WaitFlag<event>(id);
    }

    __aicore__ inline GM_ADDR SlotAddress(uint32_t source, uint32_t localExpert, uint32_t q) const
    {
        const uint32_t destination = uint32_t(args_->rank);
        return args_->peerMems[destination] + TileXREpServerDedupDispatchOffset(
            data_, source, destination, localExpert, q, uint32_t(args_->rankSize),
            localExperts_, bs_);
    }

    __aicore__ inline bool PayloadReady(__gm__ uint8_t *slot, LocalTensor<float> flags)
    {
        GlobalTensor<float> flagGlobal;
        flagGlobal.SetGlobalBuffer(reinterpret_cast<__gm__ float *>(slot + 480U));
        DataCopy(flags, flagGlobal, DataCopyParams{
            uint16_t(data_.blockCount), 1U, 15U, 0U});
        Sync<HardEvent::MTE2_V>();
        for (uint32_t block = 0U; block < data_.blockCount; ++block) {
            for (uint32_t word = 0U; word < 8U; ++word) {
                if (flags.GetValue(block * 8U + word) != 1.0f) return false;
            }
        }
        return true;
    }

    __aicore__ inline void ClearFlags(__gm__ uint8_t *slot, LocalTensor<float> flags)
    {
        Duplicate<float>(flags, 0.0f, uint32_t(data_.blockCount) * 8U);
        Sync<HardEvent::S_MTE3>();
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

    __aicore__ inline void SendForward(LocalTensor<uint8_t> packet, LocalTensor<uint8_t> relay,
        LocalTensor<int32_t> tail, uint32_t srcRank, uint32_t destinationExpert,
        uint32_t destinationQ, uint32_t originalK, uint32_t relayIndex, float weight)
    {
        auto packetWords = packet.template ReinterpretCast<int32_t>();
        auto relayWords = relay.template ReinterpretCast<int32_t>();
        Copy(relayWords, packetWords, uint64_t(64), uint8_t(data_.blockCount), {1, 1, 16, 15});
        Copy(relayWords[64], packetWords[64], uint64_t(56), uint8_t(data_.blockCount), {1, 1, 16, 15});
        Sync<HardEvent::V_S>();
        auto relayTail = relay[data_.tailOffset].template ReinterpretCast<int32_t>();
        for (uint32_t i = 0U; i < uint32_t(data_.payloadBytes - data_.tailOffset) / 4U; ++i) {
            relayTail.SetValue(i, tail.GetValue(i));
        }
        auto relayWeights = relayTail.template ReinterpretCast<float>();
        relayTail.SetValue(2U, int32_t(originalK));
        relayWeights.SetValue(3U, weight);
        relayTail.SetValue(4U, -1);
        relayTail.SetValue(5U, 0);
        relayTail.SetValue(6U, int32_t(args_->rank));
        relayTail.SetValue(7U, int32_t(relayIndex));
        Sync<HardEvent::S_MTE3>();

        const uint32_t destination = destinationExpert / localExperts_;
        GlobalTensor<uint8_t> destinationGm;
        destinationGm.SetGlobalBuffer(args_->peerMems[destination] +
            TileXREpServerDedupDispatchOffset(data_, srcRank, destination,
                destinationExpert % localExperts_, destinationQ,
                uint32_t(args_->rankSize), localExperts_, bs_));
        DataCopy(destinationGm, relay, uint32_t(data_.tokenStride));
        Sync<HardEvent::MTE3_S>();
        (void)packet;
    }

    __aicore__ inline void ReceiveOne(uint32_t source, uint32_t localExpert, uint32_t q,
        uint32_t row, LocalTensor<uint8_t> packet, LocalTensor<uint8_t> hidden,
        LocalTensor<float> flags, LocalTensor<uint8_t> relay, LocalTensor<uint32_t> group,
        LocalTensor<int32_t> scalar)
    {
        __gm__ uint8_t *const slot = SlotAddress(source, localExpert, q);
        while (!PayloadReady(slot, flags)) {}

        GlobalTensor<uint8_t> sourceGlobal;
        sourceGlobal.SetGlobalBuffer(slot);
        DataCopy(packet, sourceGlobal, uint32_t(data_.tokenStride));
        Sync<HardEvent::MTE2_S>();

        // Strip each 32B DataAsFlag block into a contiguous hidden tensor.
        // Local-to-local DataCopy is not available on all CANN 9.1 headers,
        // so use the vector Copy primitive with a 16-block source stride.
        const uint32_t hiddenBlocks = uint32_t((hiddenBytes_ + 479U) / 480U);
        Copy(hidden, packet, uint64_t(32), uint8_t(hiddenBlocks * 15U), {1, 1, 1, 16});
        Sync<HardEvent::MTE2_S>();
        DataCopyPad(expandX_[uint64_t(row) * hiddenBytes_], hidden,
            {1U, hiddenBytes_, 0U, 0U, 0U});
        if constexpr (Quantized) {
            DataCopyPad(dynamicScales_[uint64_t(row) * data_.scaleBytes],
                hidden[data_.quantCount],
                {1U, uint32_t(data_.scaleBytes), 0U, 0U, 0U});
        }
        Sync<HardEvent::MTE3_S>();

        auto tail = packet[data_.tailOffset].template ReinterpretCast<int32_t>();
        const int32_t forwardCount = tail.GetValue(4U);
        if (forwardCount > 0) {
            const uint32_t relayIndex = AllocateRelayIndex();
            auto assist = tail.template ReinterpretCast<uint32_t>();
            assist.SetValue(6U, int32_t(args_->rank));
            assist.SetValue(7U, int32_t(relayIndex));
            WriteAssist(row, assist);

            const uint32_t strideWords = uint32_t(data_.payloadBytes - data_.tailOffset) / 4U;
            for (uint32_t i = 0U; i < uint32_t(forwardCount); ++i) {
                const uint32_t entry = 8U + i * 4U;
                const uint32_t destinationExpert = uint32_t(tail.GetValue(entry));
                const uint32_t destinationQ = uint32_t(tail.GetValue(entry + 1U));
                const uint32_t originalK = uint32_t(tail.GetValue(entry + 2U));
                const float weight = tail.template ReinterpretCast<float>().GetValue(entry + 3U);
                group.SetValue(0U, row);
                group.SetValue(1U + i * 2U, relayIndex * k_ + originalK);
                group.SetValue(2U + i * 2U, uint32_t(tail.GetValue(entry + 3U)));
                SendForward(packet, relay, tail, uint32_t(tail.GetValue(0U)),
                    destinationExpert, destinationQ, originalK, relayIndex, weight);
            }
            const uint32_t groupStride = uint32_t(TileXREpDedupAlign(
                kEpServerDedupGroupMembersOffset + uint64_t(k_ - 1U) * sizeof(EpServerDedupGroupMember), 32U));
            DataCopyPad(group_[uint64_t(relayIndex) * groupStride], group,
                {1U, groupStride, 0U, 0U, 0U});
            Sync<HardEvent::MTE3_S>();
        } else {
            WriteAssist(row, tail.template ReinterpretCast<uint32_t>());
        }
        ClearFlags(slot, flags);
        (void)scalar;
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
