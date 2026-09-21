#ifndef TILEXR_EP_SERVER_DEDUP_PARTIAL_H
#define TILEXR_EP_SERVER_DEDUP_PARTIAL_H

#include "kernel_operator.h"
#include "comm_args.h"
#include "tilexr_types.h"
#include "ep_memory_server_dedup_buffer_layout.h"
#include "ep_memory_server_dedup_combine_config.h"

namespace TileXREp {
using namespace AscendC;

// Read-core M4 owns this scratch only after M1/M2 completion. Task rows are
// compacted before Init; all other views and event state are local to M4.
class ServerDedupPartial {
public:
    __aicore__ inline void Init(const __gm__ TileXR::CommArgs *args, TPipe *pipe,
        LocalTensor<uint8_t> scratch, GM_ADDR expert, GM_ADDR table2, GM_ADDR table3,
        uint32_t tokens, uint32_t count, uint32_t bs, uint32_t h, uint32_t k,
        uint32_t experts, uint8_t dtype, uint64_t window)
    {
        args_ = args; pipe_ = pipe; scratch_ = scratch; count_ = count;
        bs_ = bs; h_ = h; k_ = k; localExperts_ = experts / uint32_t(args->rankSize); dtype_ = dtype;
        window_ = window;
        blocks_ = (h * 2U + 479U) / 480U;
        packetBytes_ = blocks_ * 512U;
        inputBytes_ = EpCombineAlign(h * 2U, 32U);
        stageStride_ = EpCombineAlign(h * 2U, 512U);
        stageBase_ = window + TILEXR_EP_SERVER_DEDUP_SHARED_DATA_OFFSET + uint64_t(bs) * k * packetBytes_;
        inputLanes_ = EpCombineM4InputLaneCount(tokens, h, k);
        outputLanes_ = EpCombineM4OutputLaneCount(tokens, h, k);
        if (inputLanes_ == 0U || outputLanes_ == 0U) { Trap(); return; }
        rows_ = Allocate<int32_t>(EpCombineM4TaskBytes(tokens));
        table2Ub_ = Allocate<int32_t>(EpCombineAlign(kEpServerDedupCombineM4TableRows * 12U, 32U));
        table3Ub_ = Allocate<uint32_t>(EpCombineAlign(kEpServerDedupCombineM4TableRows * k * 8U, 32U));
        cast_ = Allocate<float>(EpCombineAlign(h * 4U, 256U));
        product_ = Allocate<float>(EpCombineAlign(h * 4U, 256U));
        sum_ = Allocate<float>(EpCombineAlign(h * 4U, 256U));
        low_ = Allocate<uint16_t>(blocks_ * 480U);
        inputs_ = Allocate<uint16_t>(inputLanes_ * inputBytes_);
        outputs_ = Allocate<uint8_t>(outputLanes_ * packetBytes_);
        expert_.SetGlobalBuffer(reinterpret_cast<__gm__ uint16_t *>(expert));
        table2_.SetGlobalBuffer(reinterpret_cast<__gm__ int32_t *>(table2));
        table3_.SetGlobalBuffer(reinterpret_cast<__gm__ uint32_t *>(table3));
    }

    __aicore__ inline void Process()
    {
        for (uint32_t lane = 0; lane < inputLanes_; ++lane) SetFlag<HardEvent::V_MTE2>(lane);
        uint64_t flagMask[2] = {0xffU, 0U};
        for (uint32_t lane = 0; lane < outputLanes_; ++lane) {
            Duplicate<float>(outputs_[lane * packetBytes_].ReinterpretCast<float>()[120U],
                1.0f, flagMask, uint8_t(blocks_), 1U, 16U);
            SetFlag<HardEvent::MTE3_V>(lane);
        }
        PipeBarrier<PIPE_V>();
        for (uint32_t task = 0; task < count_; ++task) {
            const uint32_t row = uint32_t(rows_.GetValue(task));
            CacheTables(task, row);
            const uint32_t localRow = row - cacheBegin_;
            const uint32_t destinationEntry = localRow * 3U;
            const uint32_t source = uint32_t(table2Ub_.GetValue(destinationEntry));
            const uint32_t token = uint32_t(table2Ub_.GetValue(destinationEntry + 1U));
            const uint32_t topk = uint32_t(table2Ub_.GetValue(destinationEntry + 2U));
            const uint32_t relay = localRow * k_ * 2U;
            const uint32_t members = table3Ub_.GetValue(relay); // full uint32 N, not uint16
            if (members == 0U || members >= k_) { Trap(); return; }
            if (dtype_ == TileXR::TILEXR_DATA_TYPE_FP16) Aggregate<half>(row, source, relay, members + 1U);
            else Aggregate<bfloat16_t>(row, source, relay, members + 1U);
            Send(task % outputLanes_, source, token, topk);
        }
        for (uint32_t lane = 0; lane < inputLanes_; ++lane) WaitFlag<HardEvent::V_MTE2>(lane);
        for (uint32_t lane = 0; lane < outputLanes_; ++lane) WaitFlag<HardEvent::MTE3_V>(lane);
        // Events are paired before the boundary. A future M5 may now overwrite
        // scratch and establish its own views, without inheriting M4 state.
        PipeBarrier<PIPE_ALL>();
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
        const TEventID id = pipe_->FetchEventID(E);
        SetFlag<E>(id); WaitFlag<E>(id);
    }
    __aicore__ inline void CacheTables(uint32_t task, uint32_t row)
    {
        if (row < cacheEnd_) return;
        cacheBegin_ = row; cacheEnd_ = row + 1U;
        for (uint32_t next = task + 1U; next < count_; ++next) {
            const uint32_t nextRow = uint32_t(rows_.GetValue(next));
            if (nextRow - row >= kEpServerDedupCombineM4TableRows) break;
            cacheEnd_ = nextRow + 1U;
        }
        const uint32_t span = cacheEnd_ - cacheBegin_;
        // Both tables share the exact original-row span. Prior scalar metadata
        // is consumed; outstanding MTE3 refers only to independent output lanes.
        DataCopyPad(table2Ub_, table2_[uint64_t(row) * 3U],
            {1U, span * 12U, 0U, 0U, 0U}, {false, 0U, 0U, 0});
        DataCopyPad(table3Ub_, table3_[uint64_t(row) * k_ * 2U],
            {1U, span * k_ * 8U, 0U, 0U, 0U}, {false, 0U, 0U, 0U});
        Sync<HardEvent::MTE2_S>();
    }
    __aicore__ inline void Load(uint32_t row, uint32_t source, uint32_t relay, uint32_t member)
    {
        const uint32_t lane = (inputSequence_ + member) % inputLanes_;
        WaitFlag<HardEvent::V_MTE2>(lane);
        GlobalTensor<uint16_t> data;
        if (member == 0U) data = expert_[uint64_t(row) * h_];
        else {
            const uint32_t packed = table3Ub_.GetValue(relay + member * 2U);
            const uint32_t expert = packed & 0xffffU, q = packed >> 16U;
            const uint32_t memberRank = expert / localExperts_, localExpert = expert % localExperts_;
            const uint64_t slot = (((uint64_t(source) + memberRank) % uint32_t(args_->rankSize)) *
                localExperts_ + localExpert) * bs_ + q;
            data.SetGlobalBuffer(reinterpret_cast<__gm__ uint16_t *>(args_->peerMems[memberRank] +
                stageBase_ + slot * stageStride_));
        }
        DataCopyPad(inputs_[lane * inputBytes_ / 2U], data,
            {1U, h_ * 2U, 0U, 0U, 0U}, {false, 0U, 0U, uint16_t(0)});
        SetFlag<HardEvent::MTE2_V>(lane);
    }
    template<class T> __aicore__ inline void Aggregate(uint32_t row, uint32_t source,
        uint32_t relay, uint32_t contributions)
    {
        const uint32_t preload = contributions < inputLanes_ ? contributions : inputLanes_;
        for (uint32_t member = 0; member < preload; ++member) Load(row, source, relay, member);
        const auto weights = table3Ub_.ReinterpretCast<float>();
        for (uint32_t member = 0; member < contributions; ++member) {
            const uint32_t lane = (inputSequence_ + member) % inputLanes_;
            const float weight = weights.GetValue(relay + member * 2U + 1U);
            WaitFlag<HardEvent::MTE2_V>(lane);
            Cast(cast_, inputs_[lane * inputBytes_ / 2U].template ReinterpretCast<T>(), RoundMode::CAST_NONE, h_);
            SetFlag<HardEvent::V_MTE2>(lane);
            // Prefetch may reuse this input after Cast, independent of weighted
            // accumulation and the previous token's outstanding partial send.
            if (member + inputLanes_ < contributions) Load(row, source, relay, member + inputLanes_);
            PipeBarrier<PIPE_V>();
            if (member == 0U) Muls(sum_, cast_, weight, h_);
            else {
                Muls(product_, cast_, weight, h_);
                PipeBarrier<PIPE_V>();
                Add(sum_, sum_, product_, h_);
            }
            PipeBarrier<PIPE_V>();
        }
        inputSequence_ += contributions;
        Cast(low_.template ReinterpretCast<T>(), sum_, RoundMode::CAST_RINT, h_);
        PipeBarrier<PIPE_V>();
    }
    __aicore__ inline void Send(uint32_t lane, uint32_t source, uint32_t token, uint32_t topk)
    {
        // Only output-lane reuse waits for MTE3; aggregation entry never does.
        WaitFlag<HardEvent::MTE3_V>(lane);
        auto output = outputs_[lane * packetBytes_];
        auto dst = output.ReinterpretCast<int32_t>();
        auto src = low_.ReinterpretCast<int32_t>();
        Copy(dst, src, uint64_t(64U), uint8_t(blocks_), {1U, 1U, 16U, 15U});
        Copy(dst[64U], src[64U], uint64_t(56U), uint8_t(blocks_), {1U, 1U, 16U, 15U});
        SetFlag<HardEvent::V_MTE3>(lane); WaitFlag<HardEvent::V_MTE3>(lane);
        GlobalTensor<uint8_t> target;
        target.SetGlobalBuffer(args_->peerMems[source] + window_ + TILEXR_EP_SERVER_DEDUP_SHARED_DATA_OFFSET +
            (uint64_t(token) * k_ + topk) * packetBytes_);
        DataCopy(target, output, packetBytes_);
        SetFlag<HardEvent::MTE3_V>(lane);
    }

    const __gm__ TileXR::CommArgs *args_ = nullptr;
    TPipe *pipe_ = nullptr;
    LocalTensor<uint8_t> scratch_, outputs_;
    LocalTensor<int32_t> rows_, table2Ub_;
    LocalTensor<uint32_t> table3Ub_;
    LocalTensor<float> cast_, product_, sum_;
    LocalTensor<uint16_t> low_, inputs_;
    GlobalTensor<uint16_t> expert_;
    GlobalTensor<int32_t> table2_;
    GlobalTensor<uint32_t> table3_;
    uint32_t used_ = 0, count_ = 0, bs_ = 0, h_ = 0, k_ = 0, localExperts_ = 0, dtype_ = 0;
    uint32_t blocks_ = 0, packetBytes_ = 0, inputBytes_ = 0, inputLanes_ = 0, outputLanes_ = 0;
    uint32_t cacheBegin_ = 0, cacheEnd_ = 0, inputSequence_ = 0;
    uint64_t stageStride_ = 0, stageBase_ = 0, window_ = 0;
};
} // namespace TileXREp
#endif
