#ifndef TILEXR_EP_SERVER_DEDUP_RECV_H
#define TILEXR_EP_SERVER_DEDUP_RECV_H

#include "kernel_operator.h"
#include "adv_api/reduce/sum.h"
#include "ep_memory_server_dedup_protocol.h"
#include "tilexr_ep_server_dedup_receive_tasks.h"
#include "tilexr_ep_server_dedup_receive_packet.h"
#include "tilexr_ep_server_dedup_profile.h"

namespace TileXREp {
using namespace AscendC;

template<class XType, bool Quantized, bool E4M3>
class ServerDedupReceiver {
public:
    __aicore__ inline void Init(const __gm__ TileXR::CommArgs *args,
        GM_ADDR expandX, GM_ADDR dynamicScales, GM_ADDR tokenType, GM_ADDR destinationIndex, GM_ADDR relayReadIndex,
        uint32_t bs, uint32_t h, uint32_t k, uint32_t experts, TPipe *pipe, int64_t magic = 0)
    {
        enableDedup_ = EpServerDedupEnabled(bs);
        args_ = args; pipe_ = pipe; bs_ = bs; k_ = k; experts_ = experts;
        localExperts_ = experts / uint32_t(args_->rankSize);
        receiver_ = GetBlockIdx() - kEpServerDedupSendCoreNum;
        hiddenBytes_ = Quantized ? h : h * uint32_t(sizeof(XType));
        data_ = TileXREpServerDedupDataLayout(bs, h, k, experts, Quantized);
        windowOffset_ = TileXREpServerDedupWindowOffset(magic, args_->peerMemBytes);
        data_.dispatchOffset += windowOffset_;
        expandX_.SetGlobalBuffer(expandX);
        dynamicScales_.SetGlobalBuffer(dynamicScales);
        if (kEpServerDedupEnable) tokenType_.SetGlobalBuffer(reinterpret_cast<__gm__ uint32_t *>(tokenType));
        destinationIndex_.SetGlobalBuffer(reinterpret_cast<__gm__ uint32_t *>(destinationIndex));
        if (kEpServerDedupEnable) relayReadIndex_.SetGlobalBuffer(reinterpret_cast<__gm__ uint32_t *>(relayReadIndex));
    }

    __aicore__ inline void Process(ServerDedupProfile &profile)
    {
        profile.Mark(EpDedupDetailPoint::RecvBegin);
        // Caller has completed count-stage writes before this Reset. The GM
        // prefix belongs to this receive core; old count UB is no longer used.
        pipe_->Reset();
        const uint32_t prefixBytes = uint32_t(TileXREpDedupAlign(uint64_t(experts_) * 4U, 32U));
        pipe_->InitBuffer(prefixBuf_, prefixBytes);
        GlobalTensor<int32_t> prefixGlobal;
        const auto counts = TileXREpServerDedupCountLayout(experts_);
        prefixGlobal.SetGlobalBuffer(reinterpret_cast<__gm__ int32_t *>(
            args_->peerMems[args_->rank] + windowOffset_ + counts.cntOffset + counts.prefixOffset +
            uint64_t(receiver_) * counts.prefixStride));
        auto prefix = prefixBuf_.Get<int32_t>();
        DataCopyPad(prefix, prefixGlobal, {1U, prefixBytes, 0U, 0U, 0U}, {false, 0U, 0U, 0});
        ServerDedupReceiveSync<HardEvent::MTE2_S>();
        profile.Mark(EpDedupDetailPoint::PrefixEnd);
        tasks_.Build(prefix, uint32_t(args_->rank), uint32_t(args_->rankSize), experts_, bs_, receiver_, pipe_);
        profile.Mark(EpDedupDetailPoint::TasksEnd);
        if (tasks_.Count() == 0U) {
            profile.Mark(EpDedupDetailPoint::PrepareEnd);
            return;
        }

        const uint32_t blocks = uint32_t(data_.blockCount);
        const uint32_t flagsBytes = kProbeBatch * blocks * 32U + 256U;
        // CANN 9.1/A5 Sum stores ceil(B*8/64) partials per row at a
        // 32-element stride. Reserve all rows plus a full-vector tail read.
        const uint32_t workBytes = kProbeBatch * uint32_t(TileXREpDedupAlign((blocks + 7U) / 8U, 32U)) * 4U + 256U;
        pipe_->InitBuffer(flagsBuf_, flagsBytes);
        pipe_->InitBuffer(sumBuf_, kProbeBatch * 32U);
        pipe_->InitBuffer(workBuf_, workBytes);
        // LoadAlign may read past the masked tail, including inactive rows.
        Duplicate(flagsBuf_.Get<float>(), 0.0f, flagsBytes / 4U);
        ServerDedupReceiveSync<HardEvent::V_MTE2>();
        pipe_->InitBuffer(probeIdxBuf_, kProbeBatch * 4U);
        const uint32_t tablesBytes = 2U * uint32_t(TileXREpDedupAlign(uint64_t(tasks_.Count()) * 4U, 32U));
        const uint32_t fixed = prefixBytes + tablesBytes + flagsBytes + workBytes + kProbeBatch * 36U;
        laneStride_ = uint32_t(data_.tokenStride) + (kEpServerDedupEnable ? 64U : 32U);
        const uint32_t available = (kEpServerDedupCountUbBytes - fixed) / laneStride_;
        lanes_ = available < 8U ? available : 8U; // Safe inputs guarantee at least one lane.
        pipe_->InitBuffer(lanesBuf_, lanes_ * laneStride_);
        for (uint32_t lane = 0; lane < lanes_; ++lane) SetFlag<HardEvent::MTE3_MTE2>(lane);
        profile.Mark(EpDedupDetailPoint::PrepareEnd);

        auto snapshot = probeIdxBuf_.Get<uint32_t>();
        // Fill every allocated lane (up to eight); reuse still waits for MTE3.
        const uint32_t depth = lanes_;
        uint32_t scan = 0U, issued = 0U, consumed = 0U;
        while (tasks_.PendingCount() != 0U) {
            // Include all empty scans in one wait interval. Recording every
            // retry perturbs the polling loop and can exhaust the trace buffer.
            profile.Begin(EpDedupDetailMetric::Probe);
            uint32_t count, ready;
            do {
                if (scan >= tasks_.PendingCount()) scan = 0U;
                const uint32_t remaining = tasks_.PendingCount() - scan;
                count = remaining < kProbeBatch ? remaining : kProbeBatch;
                ready = Probe(scan, count);
                if (ready == 0U) scan += count;
            } while (ready == 0U);
            profile.End(EpDedupDetailMetric::Probe);
            // Load and consume the immutable snapshot in the same descending
            // order. Higher-index removals cannot change lower pending entries.
            uint32_t nextLoad = count;
            for (uint32_t i = count; i > 0U; --i) {
                const uint32_t index = i - 1U;
                if ((ready & (uint32_t(1) << index)) == 0U) continue;
                while (issued - consumed < depth && nextLoad != 0U) {
                    --nextLoad;
                    if ((ready & (uint32_t(1) << nextLoad)) == 0U) continue;
                    IssueLoad(snapshot.GetValue(nextLoad), issued % lanes_);
                    ++issued;
                }
                ConsumeLoaded(snapshot.GetValue(index), consumed % lanes_, profile);
                tasks_.RemovePending(scan + index);
                ++consumed;
            }
        }
        profile.Mark(EpDedupDetailPoint::LoopEnd);
        for (uint32_t lane = 0; lane < lanes_; ++lane) WaitFlag<HardEvent::MTE3_MTE2>(lane);
        // This is the end of wait instruction submission, not a new Scalar
        // completion barrier. The existing final PIPE_ALL establishes completion.
        profile.Mark(EpDedupDetailPoint::DrainEnd);
        ClearFlags();
        profile.Mark(EpDedupDetailPoint::ClearIssueEnd);
        PipeBarrier<PIPE_ALL>();
        profile.Mark(EpDedupDetailPoint::ClearEnd);
    }

private:
    static constexpr uint32_t kProbeBatch = 16U;
    __aicore__ inline __gm__ uint8_t *Slot(uint32_t j)
    {
        return args_->peerMems[args_->rank] + data_.dispatchOffset + uint64_t(tasks_.Position(j)) * data_.tokenStride;
    }

    __aicore__ inline uint32_t Probe(uint32_t start, uint32_t count)
    {
        auto indices = probeIdxBuf_.Get<uint32_t>();
        auto flags = flagsBuf_.Get<float>();
        auto sums = sumBuf_.Get<float>();
        auto work = workBuf_.Get<uint8_t>();
        const uint32_t blocks = uint32_t(data_.blockCount);
        for (uint32_t i = 0; i < count; ++i) {
            const uint32_t j = tasks_.PendingIndex(start + i);
            indices.SetValue(i, j);
            ReadServerDedupFlags(Slot(j), blocks, flags[i * blocks * 8U]);
        }
        ServerDedupReceiveSync<HardEvent::MTE2_V>();
        Sum<float>(sums, flags, work, SumParams{count, blocks * 8U, blocks * 8U});
        ServerDedupReceiveSync<HardEvent::V_S>();
        uint32_t ready = 0U;
        for (uint32_t i = 0; i < count; ++i)
            if (sums.GetValue(i) == float(blocks * 8U)) ready |= uint32_t(1) << i;
        return ready;
    }

    __aicore__ inline void IssueLoad(uint32_t j, uint32_t lane)
    {
        WaitFlag<HardEvent::MTE3_MTE2>(lane);
        auto wire = lanesBuf_.Get<uint8_t>()[lane * laneStride_];
        GlobalTensor<uint8_t> source;
        source.SetGlobalBuffer(Slot(j));
        DataCopy(wire, source, uint32_t(data_.tokenStride));
        SetFlag<HardEvent::MTE2_MTE3>(lane);
        // Publish immediately after this load, before queuing later loads.
        SetFlag<HardEvent::MTE2_S>(lane);
    }

    __aicore__ inline void ConsumeLoaded(uint32_t j, uint32_t lane, ServerDedupProfile &profile)
    {
        // One interval per consumed token; prefetch issue remains uninstrumented.
        profile.Begin(EpDedupDetailMetric::RecvToken);
        WaitFlag<HardEvent::MTE2_S>(lane);
        auto wire = lanesBuf_.Get<uint8_t>()[lane * laneStride_];
        auto type = wire[uint32_t(data_.tokenStride)].template ReinterpretCast<uint32_t>();
        auto destination = wire[uint32_t(data_.tokenStride) + (kEpServerDedupEnable ? 32U : 0U)].template ReinterpretCast<uint32_t>();
        const uint32_t row = tasks_.Begin() + j;
        bool firstHop = false;
        if (enableDedup_) {
            ServerDedupPacketTail tail(wire, uint32_t(data_.headerWireOffset), uint32_t(data_.relayReadIndexWireOffset));
            firstHop = tail.Header().GetValue(3U) == uint32_t(EpServerDedupTokenType::FirstHop);
            BuildServerDedupMetadata(type, destination, tail, tasks_.Position(j));
            if (firstHop) PrepareServerDedupForward(tail);
        } else {
            auto header = wire[data_.headerWireOffset].template ReinterpretCast<uint32_t>();
            for (uint32_t i = 0; i < 3U; ++i) destination.SetValue(i, header.GetValue(i));
        }
        ServerDedupReceiveSync<HardEvent::S_MTE3>();
        WaitFlag<HardEvent::MTE2_MTE3>(lane);
        WriteServerDedupPayloadRange(expandX_[uint64_t(row) * hiddenBytes_], wire, 0U, hiddenBytes_);
        if constexpr (Quantized)
            WriteServerDedupPayloadRange(dynamicScales_[uint64_t(row) * data_.scaleBytes], wire,
                uint32_t(data_.quantCount), uint32_t(data_.scaleBytes));
        if (enableDedup_) DataCopyPad(tokenType_[row], type, {1U, 4U, 0U, 0U, 0U});
        DataCopyPad(destinationIndex_[uint64_t(row) * 3U], destination, {1U, 12U, 0U, 0U, 0U});
        if (enableDedup_ && firstHop) {
            ServerDedupPacketTail tail(wire, uint32_t(data_.headerWireOffset), uint32_t(data_.relayReadIndexWireOffset));
            DataCopyPad(relayReadIndex_[uint64_t(row) * k_ * 2U], tail.ReadIndex(),
                {1U, k_ * 8U, 0U, 0U, 0U});
            const uint32_t sourceRank = tail.Header().GetValue(0U);
            const uint32_t ranks = uint32_t(args_->rankSize);
            const uint32_t members = tail.ReadIndex().GetValue(0U);
            for (uint32_t i = 0U; i < members; ++i) {
                const uint32_t index = tail.Entry(i).GetValue(0U);
                const uint32_t expertId = index & 0xffffU;
                const uint32_t q = index >> 16U;
                const uint32_t destinationRank = expertId / localExperts_;
                GlobalTensor<uint8_t> output;
                output.SetGlobalBuffer(args_->peerMems[destinationRank] + TileXREpServerDedupDispatchOffset(
                    data_, sourceRank, destinationRank, expertId % localExperts_, q,
                    ranks, localExperts_, bs_));
                DataCopy(output, wire, uint32_t(data_.tokenStride));
            }
        }
        SetFlag<HardEvent::MTE3_MTE2>(lane);
        profile.End(EpDedupDetailMetric::RecvToken);
    }

    __aicore__ inline void ClearFlags()
    {
        auto zeros = flagsBuf_.Get<float>();
        Duplicate(zeros, 0.0f, uint32_t(data_.blockCount) * 8U);
        ServerDedupReceiveSync<HardEvent::V_MTE3>();
        for (uint32_t j = 0; j < tasks_.Count(); ++j) {
            GlobalTensor<float> output;
            output.SetGlobalBuffer(reinterpret_cast<__gm__ float *>(Slot(j) + 480U));
            DataCopyPad(output, zeros, {uint16_t(data_.blockCount), 32U, 0U, 480U, 0U});
        }
    }

    const __gm__ TileXR::CommArgs *args_ = nullptr;
    TPipe *pipe_ = nullptr;
    bool enableDedup_ = false;
    uint64_t windowOffset_ = 0;
    static constexpr uint32_t recvCoreNum_ = kEpServerDedupRecvCoreNum;
    uint32_t bs_ = 0U, k_ = 0U, experts_ = 0U, localExperts_ = 0U, receiver_ = 0U;
    uint32_t hiddenBytes_ = 0U, laneStride_ = 0U, lanes_ = 0U;
    EpServerDedupDataLayout data_ {};
    ServerDedupReceiveTasks tasks_;
    TBuf<> prefixBuf_, flagsBuf_, sumBuf_, workBuf_, probeIdxBuf_, lanesBuf_;
    GlobalTensor<uint8_t> expandX_, dynamicScales_;
    GlobalTensor<uint32_t> tokenType_, destinationIndex_, relayReadIndex_;
};
} // namespace TileXREp
#endif
