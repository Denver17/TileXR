#ifndef TILEXR_EP_SERVER_DEDUP_RECV_H
#define TILEXR_EP_SERVER_DEDUP_RECV_H

#include "kernel_operator.h"
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
        GM_ADDR expandX, GM_ADDR dynamicScales, GM_ADDR assistEx, GM_ADDR group,
        uint32_t bs, uint32_t h, uint32_t k, uint32_t experts, TPipe *pipe, int64_t magic = 0)
    {
        args_ = args; pipe_ = pipe; bs_ = bs; k_ = k; experts_ = experts;
        localExperts_ = experts / uint32_t(args_->rankSize);
        receiver_ = GetBlockIdx() - kEpServerDedupSendCoreNum;
        hiddenBytes_ = Quantized ? h : h * uint32_t(sizeof(XType));
        data_ = TileXREpServerDedupDataLayout(bs, h, k, experts, Quantized);
        windowOffset_ = TileXREpServerDedupWindowOffset(magic);
        data_.dispatchOffset += windowOffset_;
        groupStride_ = uint32_t(TileXREpDedupAlign(
            kEpServerDedupGroupMembersOffset + uint64_t(k - 1U) * sizeof(EpServerDedupGroupMember), 32U));
        expandX_.SetGlobalBuffer(expandX);
        dynamicScales_.SetGlobalBuffer(dynamicScales);
        assistEx_.SetGlobalBuffer(reinterpret_cast<__gm__ uint32_t *>(assistEx));
        group_.SetGlobalBuffer(reinterpret_cast<__gm__ uint32_t *>(group));
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
        pipe_->InitBuffer(flagsBuf_, kProbeBatch * blocks * 32U);
        pipe_->InitBuffer(sumBuf_, kProbeBatch * 32U);
        pipe_->InitBuffer(workBuf_, blocks * 32U);
        pipe_->InitBuffer(probeIdxBuf_, kProbeBatch * 4U);
        const uint32_t tablesBytes = 2U * uint32_t(TileXREpDedupAlign(uint64_t(tasks_.Count()) * 4U, 32U));
        const uint32_t fixed = prefixBytes + tablesBytes + (kProbeBatch + 1U) * blocks * 32U + kProbeBatch * 36U;
        laneStride_ = uint32_t(data_.tokenStride) + 32U + groupStride_;
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
            if (scan >= tasks_.PendingCount()) scan = 0U;
            const uint32_t remaining = tasks_.PendingCount() - scan;
            const uint32_t count = remaining < kProbeBatch ? remaining : kProbeBatch;
            profile.Begin(EpDedupDetailMetric::Probe);
            const uint32_t ready = Probe(scan, count, profile);
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
                    IssueLoad(snapshot.GetValue(nextLoad), issued % lanes_, profile);
                    ++issued;
                }
                ConsumeLoaded(snapshot.GetValue(index), consumed % lanes_, profile);
                tasks_.RemovePending(scan + index);
                ++consumed;
            }
            if (ready == 0U) scan += count;
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

    __aicore__ inline uint32_t Probe(uint32_t start, uint32_t count, ServerDedupProfile &profile)
    {
        auto indices = probeIdxBuf_.Get<uint32_t>();
        auto flags = flagsBuf_.Get<float>();
        auto sums = sumBuf_.Get<float>();
        auto work = workBuf_.Get<float>();
        const uint32_t blocks = uint32_t(data_.blockCount);
        profile.Begin(EpDedupDetailMetric::ProbeLoadFlag);
        for (uint32_t i = 0; i < count; ++i) {
            const uint32_t j = tasks_.PendingIndex(start + i);
            indices.SetValue(i, j);
            ReadServerDedupFlags(Slot(j), blocks, flags[i * blocks * 8U]);
        }
        ServerDedupReceiveSync<HardEvent::MTE2_V>();
        profile.End(EpDedupDetailMetric::ProbeLoadFlag);
        profile.Begin(EpDedupDetailMetric::ProbeCalSum);
        for (uint32_t i = 0; i < count; ++i) {
            ReduceSum(sums[i * 8U], flags[i * blocks * 8U], work, 8U, uint8_t(blocks), 1U);
            PipeBarrier<PIPE_V>(); // Reuse only the shared reduction scratch.
        }
        ServerDedupReceiveSync<HardEvent::V_S>();
        uint32_t ready = 0U;
        for (uint32_t i = 0; i < count; ++i)
            if (sums.GetValue(i * 8U) == float(blocks * 8U)) ready |= uint32_t(1) << i;
        profile.End(EpDedupDetailMetric::ProbeCalSum);
        return ready;
    }

    __aicore__ inline void IssueLoad(uint32_t j, uint32_t lane, ServerDedupProfile &profile)
    {
        profile.Begin(EpDedupDetailMetric::LoadIssue);
        WaitFlag<HardEvent::MTE3_MTE2>(lane);
        auto wire = lanesBuf_.Get<uint8_t>()[lane * laneStride_];
        GlobalTensor<uint8_t> source;
        source.SetGlobalBuffer(Slot(j));
        DataCopy(wire, source, uint32_t(data_.tokenStride));
        SetFlag<HardEvent::MTE2_MTE3>(lane);
        // Publish immediately after this load, before queuing later loads.
        SetFlag<HardEvent::MTE2_S>(lane);
        profile.End(EpDedupDetailMetric::LoadIssue);
    }

    __aicore__ inline void ConsumeLoaded(uint32_t j, uint32_t lane, ServerDedupProfile &profile)
    {
        profile.Begin(EpDedupDetailMetric::LoadWait);
        WaitFlag<HardEvent::MTE2_S>(lane);
        profile.End(EpDedupDetailMetric::LoadWait);
        profile.Begin(EpDedupDetailMetric::Metadata);
        auto wire = lanesBuf_.Get<uint8_t>()[lane * laneStride_];
        auto meta = wire[uint32_t(data_.tokenStride)].template ReinterpretCast<uint32_t>();
        auto group = wire[uint32_t(data_.tokenStride) + 32U].template ReinterpretCast<uint32_t>();
        ServerDedupPacketTail tail(wire, uint32_t(data_.tailOffset));
        const int32_t forwardCount = int32_t(tail.Header().GetValue(4U));
        const uint32_t row = tasks_.Begin() + j;
        const uint32_t rank = uint32_t(args_->rank);
        // Only terminal packets search the tail for this expert's k/weight.
        const uint32_t expert = forwardCount == -1 ? rank * localExperts_ + tasks_.Position(j) / bs_ % localExperts_ : 0U;
        uint32_t t = 0U;
        if (forwardCount > 0) {
            auto *counter = reinterpret_cast<__gm__ uint32_t *>(args_->peerMems[rank] + windowOffset_ +
                TileXREpServerDedupCountLayout(experts_).counterOffset);
            profile.Begin(EpDedupDetailMetric::Atomic);
            t = AtomicAdd(counter, uint32_t(1));
            profile.End(EpDedupDetailMetric::Atomic);
        }
        BuildServerDedupAssist(meta, tail, forwardCount, expert, rank, t);
        if (forwardCount > 0) {
            BuildServerDedupGroup(group, tail, row, t, k_);
            PrepareServerDedupForward(tail, rank, t);
        }
        ServerDedupReceiveSync<HardEvent::S_MTE3>();
        profile.End(EpDedupDetailMetric::Metadata);
        profile.Begin(EpDedupDetailMetric::OutputIssue);
        WaitFlag<HardEvent::MTE2_MTE3>(lane);
        WriteServerDedupPayloadRange(expandX_[uint64_t(row) * hiddenBytes_], wire, 0U, hiddenBytes_);
        if constexpr (Quantized)
            WriteServerDedupPayloadRange(dynamicScales_[uint64_t(row) * data_.scaleBytes], wire,
                uint32_t(data_.quantCount), uint32_t(data_.scaleBytes));
        DataCopyPad(assistEx_[uint64_t(row) * 8U], meta, {1U, 32U, 0U, 0U, 0U});
        if (forwardCount > 0)
            DataCopyPad(group_[uint64_t(t) * groupStride_ / 4U], group, {1U, groupStride_, 0U, 0U, 0U});
        profile.End(EpDedupDetailMetric::OutputIssue);
        if (forwardCount > 0) {
            profile.Begin(EpDedupDetailMetric::RelayIssue);
            const uint32_t sourceRank = tail.Header().GetValue(0U);
            const uint32_t ranks = uint32_t(args_->rankSize);
            for (uint32_t i = 0; i < uint32_t(forwardCount); ++i) {
                auto entry = tail.Entry(i);
                const uint32_t expertId = entry.GetValue(0U);
                const uint32_t destination = expertId / localExperts_;
                GlobalTensor<uint8_t> output;
                output.SetGlobalBuffer(args_->peerMems[destination] + TileXREpServerDedupDispatchOffset(
                    data_, sourceRank, destination, expertId % localExperts_, entry.GetValue(1U),
                    ranks, localExperts_, bs_));
                DataCopy(output, wire, uint32_t(data_.tokenStride));
            }
            profile.End(EpDedupDetailMetric::RelayIssue);
        }
        SetFlag<HardEvent::MTE3_MTE2>(lane);
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
    uint64_t windowOffset_ = 0;
    uint32_t bs_ = 0U, k_ = 0U, experts_ = 0U, localExperts_ = 0U, receiver_ = 0U;
    uint32_t hiddenBytes_ = 0U, groupStride_ = 0U, laneStride_ = 0U, lanes_ = 0U;
    EpServerDedupDataLayout data_ {};
    ServerDedupReceiveTasks tasks_;
    TBuf<> prefixBuf_, flagsBuf_, sumBuf_, workBuf_, probeIdxBuf_, lanesBuf_;
    GlobalTensor<uint8_t> expandX_, dynamicScales_;
    GlobalTensor<uint32_t> assistEx_, group_;
};
} // namespace TileXREp
#endif
