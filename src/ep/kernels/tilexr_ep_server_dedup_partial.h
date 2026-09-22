#ifndef TILEXR_EP_SERVER_DEDUP_PARTIAL_H
#define TILEXR_EP_SERVER_DEDUP_PARTIAL_H

#include "kernel_operator.h"
#include "ep_memory_server_dedup_combine_profile.h"
#if defined(TILEXR_ENABLE_PROFILING)
#include "tilexr_profiling.h"
#endif
#include "comm_args.h"
#include "tilexr_types.h"
#include "ep_memory_server_dedup_buffer_layout.h"
#include "ep_memory_server_dedup_combine_config.h"

namespace TileXREp {
using namespace AscendC;

// Recv-core M4 scans type/routing metadata per destination server. Its output
// addresses and completion events remain live through the M5 handoff.
class ServerDedupPartial {
public:
#if defined(TILEXR_ENABLE_PROFILING)
    __aicore__ inline void SetProfiling(TileXR::DirectProfilingWriter *writer) { profile_ = writer; }
#endif
    __aicore__ inline void Init(const __gm__ TileXR::CommArgs *args, TPipe *pipe,
        LocalTensor<uint8_t> scratch, GM_ADDR expert, GM_ADDR table2, GM_ADDR table3, GM_ADDR types,
        uint32_t tokens, uint32_t bs, uint32_t h, uint32_t k,
        uint32_t experts, uint8_t dtype, uint64_t window, const EpCombineM45WorkspaceLayout &layout)
    {
        args_ = args; pipe_ = pipe; scratch_ = scratch[layout.phaseOffset]; tokens_ = tokens;
        bs_ = bs; h_ = h; k_ = k; localExperts_ = experts / uint32_t(args->rankSize); dtype_ = dtype;
        window_ = window;
        blocks_ = (h * 2U + 479U) / 480U;
        packetBytes_ = blocks_ * 512U;
        inputBytes_ = layout.inputBytes;
        stageStride_ = EpCombineAlign(h * 2U, 512U);
        stageBase_ = window + TILEXR_EP_SERVER_DEDUP_SHARED_DATA_OFFSET + uint64_t(bs) * k * packetBytes_;
        inputLanes_ = layout.inputLanes;
        outputLanes_ = layout.outputLanes;
        if (inputLanes_ == 0U || outputLanes_ == 0U) { Trap(); return; }
        rows_ = Allocate<int32_t>(EpCombineM4TaskBytes(tokens));
        table2Ub_ = Allocate<int32_t>(EpCombineAlign(kEpServerDedupCombineM4TableRows * 12U, 32U));
        table3Ub_ = Allocate<uint32_t>(EpCombineAlign(kEpServerDedupCombineM4TableRows * k * 8U, 32U));
        cast_ = scratch.ReinterpretCast<float>();
        product_ = scratch[layout.computeBytes].ReinterpretCast<float>();
        sum_ = scratch[2U * layout.computeBytes].ReinterpretCast<float>();
        low_ = Allocate<uint16_t>(blocks_ * 480U);
        inputs_ = scratch[layout.inputOffset].ReinterpretCast<uint16_t>();
        outputs_ = scratch[layout.outputOffset];
        expert_.SetGlobalBuffer(reinterpret_cast<__gm__ uint16_t *>(expert));
        table2_.SetGlobalBuffer(reinterpret_cast<__gm__ int32_t *>(table2));
        table3_.SetGlobalBuffer(reinterpret_cast<__gm__ uint32_t *>(table3));
        types_.SetGlobalBuffer(reinterpret_cast<__gm__ int32_t *>(types));
    }

    __aicore__ inline uint32_t Process()
    {
        for (uint32_t lane = 0; lane < inputLanes_; ++lane) SetFlag<HardEvent::V_MTE2>(lane);
        uint64_t flagMask[2] = {0xffU, 0U};
        for (uint32_t lane = 0; lane < outputLanes_; ++lane) {
            Duplicate<float>(outputs_[lane * packetBytes_].ReinterpretCast<float>()[120U],
                1.0f, flagMask, uint8_t(blocks_), 1U, 16U);
        }
        PipeBarrier<PIPE_V>();
        const uint32_t servers = uint32_t(args_->rankSize) / 8U;
        const uint32_t ownServer = uint32_t(args_->rank) / 8U;
        uint32_t owner = 0U, completed = 0U;
        for (uint32_t step = 1U; step <= servers; ++step) {
            const uint32_t count = SelectServer((ownServer + step) % servers, owner);
            for (uint32_t task = 0; task < count; ++task) {
                const uint32_t row = uint32_t(rows_.GetValue(task));
                Mark(kEpServerDedupCombineProfilePartialTokenBegin);
                LoadTask(row);
                const uint32_t source = uint32_t(table2Ub_.GetValue(0U));
                const uint32_t token = uint32_t(table2Ub_.GetValue(1U));
                const uint32_t topk = uint32_t(table2Ub_.GetValue(2U));
                const uint32_t members = table3Ub_.GetValue(0U); // full uint32 N, not uint16
                if (members == 0U || members >= k_) { Trap(); return completed; }
                if (dtype_ == TileXR::TILEXR_DATA_TYPE_FP16) Aggregate<half>(row, source, members + 1U);
                else Aggregate<bfloat16_t>(row, source, members + 1U);
                Send(completed % outputLanes_, source, token, topk);
                ++completed;
                Mark(kEpServerDedupCombineProfilePartialTokenEnd);
            }
            // Scalar metadata may be reused; no server drain or lane reset.
        }
        for (uint32_t lane = 0; lane < inputLanes_; ++lane) WaitFlag<HardEvent::V_MTE2>(lane);
        // Output events and their UB sources remain owned by the shared ring.
        // The caller hands off only MTE2/Vector before M5 reuses phase metadata.
        return completed;
    }

private:
    __aicore__ inline void Mark(uint32_t event)
    {
#if defined(TILEXR_ENABLE_PROFILING)
        if (profile_ != nullptr) profile_->Record(event);
#else
        (void)event;
#endif
    }
#if defined(TILEXR_ENABLE_PROFILING)
    TileXR::DirectProfilingWriter *profile_ = nullptr;
#endif
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
    __aicore__ inline uint32_t SelectServer(uint32_t server, uint32_t &owner)
    {
        uint32_t count = 0U;
        const uint32_t localCore = GetBlockIdx() - kEpServerDedupCombineSendCoreNum;
        // This scan runs only after the previous token's Scalar weights/indices
        // were consumed. Reuse the old multi-row Table3 cache for 32 type tags;
        // no scan reads Table3 GM or aliases compute/input/in-flight output.
        auto tags = table3Ub_.ReinterpretCast<int32_t>();
        for (uint32_t begin = 0U; begin < tokens_; begin += kEpServerDedupCombineM4TableRows) {
            const uint32_t left = tokens_ - begin;
            const uint32_t span = left < kEpServerDedupCombineM4TableRows ? left : kEpServerDedupCombineM4TableRows;
            DataCopyPad(tags, types_[begin], {1U, span * 4U, 0U, 0U, 0U}, {false, 0U, 0U, 0});
            DataCopyPad(table2Ub_, table2_[uint64_t(begin) * 3U],
                {1U, span * 12U, 0U, 0U, 0U}, {false, 0U, 0U, 0});
            Sync<HardEvent::MTE2_S>();
            for (uint32_t offset = 0U; offset < span; ++offset) {
                if (tags.GetValue(offset) != 2) continue;
                const uint32_t source = uint32_t(table2Ub_.GetValue(offset * 3U));
                if (source >= uint32_t(args_->rankSize)) { Trap(); return 0U; }
                if (source / 8U != server) continue;
                if (owner == localCore) rows_.SetValue(count++, int32_t(begin + offset));
                if (++owner == kEpServerDedupCombineRecvCoreNum) owner = 0U;
            }
        }
        return count;
    }
    __aicore__ inline void LoadTask(uint32_t row)
    {
        // Route is reloaded by original row so the list needs only 4B/task.
        // Table3 is exactly one K*8B row, fetched only for an owned type2 task.
        DataCopyPad(table2Ub_, table2_[uint64_t(row) * 3U],
            {1U, 12U, 0U, 0U, 0U}, {false, 0U, 0U, 0});
        DataCopyPad(table3Ub_, table3_[uint64_t(row) * k_ * 2U],
            {1U, k_ * 8U, 0U, 0U, 0U}, {false, 0U, 0U, 0U});
        Sync<HardEvent::MTE2_S>();
    }
    __aicore__ inline void Load(uint32_t row, uint32_t source, uint32_t member)
    {
        const uint32_t lane = (inputSequence_ + member) % inputLanes_;
        WaitFlag<HardEvent::V_MTE2>(lane);
        GlobalTensor<uint16_t> data;
        if (member == 0U) data = expert_[uint64_t(row) * h_];
        else {
            const uint32_t packed = table3Ub_.GetValue(member * 2U);
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
        uint32_t contributions)
    {
        const uint32_t preload = contributions < inputLanes_ ? contributions : inputLanes_;
        for (uint32_t member = 0; member < preload; ++member) Load(row, source, member);
        const auto weights = table3Ub_.ReinterpretCast<float>();
        for (uint32_t member = 0; member < contributions; ++member) {
            const uint32_t lane = (inputSequence_ + member) % inputLanes_;
            const float weight = weights.GetValue(member * 2U + 1U);
            WaitFlag<HardEvent::MTE2_V>(lane);
            Cast(cast_, inputs_[lane * inputBytes_ / 2U].template ReinterpretCast<T>(), RoundMode::CAST_NONE, h_);
            SetFlag<HardEvent::V_MTE2>(lane);
            // Prefetch may reuse this input after Cast, independent of weighted
            // accumulation and the previous token's outstanding partial send.
            if (member + inputLanes_ < contributions) Load(row, source, member + inputLanes_);
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
    GlobalTensor<int32_t> table2_, types_;
    GlobalTensor<uint32_t> table3_;
    uint32_t used_ = 0, tokens_ = 0, bs_ = 0, h_ = 0, k_ = 0, localExperts_ = 0, dtype_ = 0;
    uint32_t blocks_ = 0, packetBytes_ = 0, inputBytes_ = 0, inputLanes_ = 0, outputLanes_ = 0;
    uint32_t inputSequence_ = 0;
    uint64_t stageStride_ = 0, stageBase_ = 0, window_ = 0;
};
} // namespace TileXREp
#endif
