#ifndef TILEXR_EP_SERVER_DEDUP_SOURCE_AGGREGATE_H
#define TILEXR_EP_SERVER_DEDUP_SOURCE_AGGREGATE_H

#include "kernel_operator.h"
#include "ep_memory_server_dedup_combine_config.h"
#include "tilexr_types.h"

namespace TileXREp {
using namespace AscendC;

// M5 owns only the scratch beyond resident controls. Its caller drains M3
// before Init; the zero flag source remains live until the final MTE3 drain.
class ServerDedupSourceAggregate {
public:
    __aicore__ inline void Init(LocalTensor<uint8_t> scratch, GM_ADDR result,
        GM_ADDR masks, GM_ADDR weights, GM_ADDR yOut, uint32_t bs, uint32_t h,
        uint32_t k, uint8_t dtype, uint32_t participant, uint32_t participants)
    {
        scratch_ = scratch; h_ = h; k_ = k; dtype_ = dtype;
        const uint32_t base = bs / participants, extra = bs % participants;
        count_ = base + (participant < extra ? 1U : 0U);
        begin_ = participant * base + (participant < extra ? participant : extra);
        blocks_ = (h * 2U + 479U) / 480U;
        inputBytes_ = blocks_ * 480U; packetBytes_ = blocks_ * 512U;
        outputBytes_ = EpCombineAlign(h * 2U, 32U);
        inputLanes_ = EpCombineM5InputLaneCount(bs, h, k);
        outputLanes_ = EpCombineM5OutputLaneCount(bs, h, k);
        if (inputLanes_ == 0U || outputLanes_ == 0U) { Trap(); return; }
        // The shared Host budget uses the largest contiguous Send-core segment.
        const uint32_t owned = EpCombineM5TokenCount(bs);
        masks_ = Allocate<uint32_t>(EpCombineAlign(owned * 4U, 32U));
        pending_ = Allocate<uint32_t>(EpCombineAlign(owned * 4U, 32U));
        weights_ = Allocate<float>(EpCombineAlign(owned * k * 4U, 32U));
        flags_ = Allocate<float>(EpCombineM5FlagBytes(h, k));
        work_ = Allocate<float>(EpCombineM5WorkBytes(h, k));
        probeSum_ = Allocate<float>(32U);
        cast_ = Allocate<float>(EpCombineAlign(h * 4U, 256U));
        product_ = Allocate<float>(EpCombineAlign(h * 4U, 256U));
        sum_ = Allocate<float>(EpCombineAlign(h * 4U, 256U));
        inputs_ = Allocate<uint16_t>(inputLanes_ * inputBytes_);
        outputs_ = Allocate<uint16_t>(outputLanes_ * outputBytes_);
        result_.SetGlobalBuffer(reinterpret_cast<__gm__ uint16_t *>(result));
        resultFlags_.SetGlobalBuffer(reinterpret_cast<__gm__ float *>(result));
        output_.SetGlobalBuffer(reinterpret_cast<__gm__ uint16_t *>(yOut));
        if (count_ == 0U) return;
        GlobalTensor<uint32_t> maskGm;
        GlobalTensor<float> weightGm;
        maskGm.SetGlobalBuffer(reinterpret_cast<__gm__ uint32_t *>(masks));
        weightGm.SetGlobalBuffer(reinterpret_cast<__gm__ float *>(weights));
        DataCopyPad(masks_, maskGm[begin_], {1U, count_ * 4U, 0U, 0U, 0U}, {false, 0U, 0U, 0U});
        DataCopyPad(weights_, weightGm[uint64_t(begin_) * k],
            {1U, count_ * k * 4U, 0U, 0U, 0U}, {false, 0U, 0U, 0.0f});
        Sync<HardEvent::MTE2_S>();
        for (uint32_t p = 0; p < count_; ++p) pending_.SetValue(p, begin_ + p);
    }

    __aicore__ inline void Process()
    {
        if (count_ == 0U) return;
        for (uint32_t lane = 0; lane < inputLanes_; ++lane) SetFlag<HardEvent::V_MTE2>(lane);
        for (uint32_t lane = 0; lane < outputLanes_; ++lane) SetFlag<HardEvent::MTE3_V>(lane);
        uint32_t remaining = count_, scan = 0U, completed = 0U;
        while (remaining != 0U) {
            const uint32_t token = pending_.GetValue(scan);
            const uint32_t mask = masks_.GetValue(token - begin_);
            if (mask == 0U || (k_ < 32U && (mask >> k_) != 0U)) { Trap(); return; }
            if (!Probe(token, mask)) {
                if (++scan == remaining) scan = 0U;
                continue;
            }
            // No further probes after this point: clear every original token's
            // complete K*N flags, including mask-inactive slots. This source is
            // independent of the final token's in-flight payload and yOut.
            if (remaining == 1U) ClearFlags();
            if (dtype_ == TileXR::TILEXR_DATA_TYPE_FP16) Aggregate<half>(token, mask, completed);
            else Aggregate<bfloat16_t>(token, mask, completed);
            pending_.SetValue(scan, pending_.GetValue(remaining - 1U));
            --remaining; ++completed;
            if (scan == remaining) scan = 0U; // re-probe the swapped real TokenId
        }
        for (uint32_t lane = 0; lane < inputLanes_; ++lane) WaitFlag<HardEvent::V_MTE2>(lane);
        for (uint32_t lane = 0; lane < outputLanes_; ++lane) WaitFlag<HardEvent::MTE3_V>(lane);
    }

private:
    template<class T> __aicore__ inline LocalTensor<T> Allocate(uint32_t bytes)
    {
        auto tensor = scratch_[used_].template ReinterpretCast<T>();
        used_ += bytes;
        return tensor;
    }
    template<HardEvent E> __aicore__ inline void Sync()
    {
        SetFlag<E>(7U); WaitFlag<E>(7U);
    }
    __aicore__ inline bool Probe(uint32_t token, uint32_t mask)
    {
        Sync<HardEvent::V_MTE2>();
        uint32_t flags = 0U;
        for (uint32_t k = 0; k < k_; ++k) if ((mask >> k) & 1U) {
            auto src = resultFlags_[(uint64_t(token) * k_ + k) * packetBytes_ / 4U + 120U];
            DataCopy(flags_[flags * 8U], src,
                {uint16_t(blocks_), 1U, 15U, 0U});
            flags += blocks_;
        }
        Sync<HardEvent::MTE2_V>();
        ReduceSum(probeSum_, flags_, work_, 1U, flags, 1U);
        Sync<HardEvent::V_S>();
        return probeSum_.GetValue(0U) == float(flags);
    }
    __aicore__ inline void ClearFlags()
    {
        const uint32_t flags = k_ * blocks_;
        Duplicate<float>(flags_, 0.0f, flags * 8U);
        Sync<HardEvent::V_MTE3>();
        for (uint32_t p = begin_; p < begin_ + count_; ++p) {
            auto target = resultFlags_[uint64_t(p) * k_ * packetBytes_ / 4U + 120U];
            DataCopyPad(target, flags_,
                {uint16_t(flags), 32U, 0U, 480U, 0U});
        }
    }
    __aicore__ inline void Load(uint32_t token, uint32_t k, uint32_t member)
    {
        const uint32_t lane = (inputSequence_ + member) % inputLanes_;
        WaitFlag<HardEvent::V_MTE2>(lane);
        DataCopy(inputs_[lane * inputBytes_ / 2U],
            result_[(uint64_t(token) * k_ + k) * packetBytes_ / 2U],
            {uint16_t(blocks_), 15U, 1U, 0U});
        SetFlag<HardEvent::MTE2_V>(lane);
    }
    template<class T> __aicore__ inline void Aggregate(uint32_t token, uint32_t mask, uint32_t completed)
    {
        uint32_t nextK = 0U, loaded = 0U;
        for (; nextK < k_ && loaded < inputLanes_; ++nextK)
            if ((mask >> nextK) & 1U) Load(token, nextK, loaded++);
        uint32_t member = 0U;
        for (uint32_t k = 0U; k < k_; ++k) if ((mask >> k) & 1U) {
            const uint32_t lane = (inputSequence_ + member) % inputLanes_;
            WaitFlag<HardEvent::MTE2_V>(lane);
            Cast(cast_, inputs_[lane * inputBytes_ / 2U].template ReinterpretCast<T>(), RoundMode::CAST_NONE, h_);
            SetFlag<HardEvent::V_MTE2>(lane);
            while (nextK < k_ && ((mask >> nextK) & 1U) == 0U) ++nextK;
            if (nextK < k_) Load(token, nextK++, loaded++);
            PipeBarrier<PIPE_V>();
            const float weight = weights_.GetValue((token - begin_) * k_ + k);
            if (member == 0U) Muls(sum_, cast_, weight, h_);
            else {
                Muls(product_, cast_, weight, h_);
                PipeBarrier<PIPE_V>();
                Add(sum_, sum_, product_, h_);
            }
            PipeBarrier<PIPE_V>();
            ++member;
        }
        inputSequence_ += member;
        const uint32_t outputLane = completed % outputLanes_;
        WaitFlag<HardEvent::MTE3_V>(outputLane);
        auto output = outputs_[outputLane * outputBytes_ / 2U];
        Cast(output.template ReinterpretCast<T>(), sum_, RoundMode::CAST_RINT, h_);
        SetFlag<HardEvent::V_MTE3>(outputLane); WaitFlag<HardEvent::V_MTE3>(outputLane);
        DataCopyPad(output_[uint64_t(token) * h_], output, {1U, h_ * 2U, 0U, 0U, 0U});
        SetFlag<HardEvent::MTE3_V>(outputLane);
    }

    LocalTensor<uint8_t> scratch_;
    LocalTensor<uint32_t> masks_, pending_;
    LocalTensor<float> weights_, flags_, work_, probeSum_, cast_, product_, sum_;
    LocalTensor<uint16_t> inputs_, outputs_;
    GlobalTensor<uint16_t> result_, output_;
    GlobalTensor<float> resultFlags_;
    uint32_t used_ = 0U, h_ = 0U, k_ = 0U, dtype_ = 0U, begin_ = 0U, count_ = 0U;
    uint32_t blocks_ = 0U, inputBytes_ = 0U, packetBytes_ = 0U, outputBytes_ = 0U;
    uint32_t inputLanes_ = 0U, outputLanes_ = 0U, inputSequence_ = 0U;
};
} // namespace TileXREp
#endif
