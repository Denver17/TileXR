#ifndef TILEXR_EP_COMBINE_MEMORY_SERVER_DEDUP_KERNEL_H
#define TILEXR_EP_COMBINE_MEMORY_SERVER_DEDUP_KERNEL_H

#include "kernel_operator.h"
#include "comm_args.h"
#include "ep_memory_server_dedup_buffer_layout.h"
#include "ep_memory_server_dedup_combine_config.h"
#include "ep_memory_server_dedup_combine_profile.h"
#if defined(TILEXR_ENABLE_PROFILING)
#include "tilexr_profiling.h"
#endif

namespace TileXREp {
using namespace AscendC;

// P2.1: unweighted local type-3 staging and server readiness only. No result
// producer/consumer and no yOut write. The final delay is not a rank barrier.
class CombineMemoryServerDedupKernel {
public:
    __aicore__ inline void Init(GM_ADDR commArgs, GM_ADDR expertOut, GM_ADDR assistInfoForCombine,
        GM_ADDR sendCounts, GM_ADDR expertScales, GM_ADDR tokenType, GM_ADDR relayReadIndex,
        GM_ADDR sourceMask, GM_ADDR yOut, uint32_t bs, uint32_t h, uint32_t topK,
        uint32_t experts, uint8_t dtype, uint8_t quantMode, int64_t magic, uint8_t profilingEnabled)
    {
        (void)expertScales; (void)relayReadIndex; (void)sourceMask; (void)yOut;
        (void)dtype; (void)quantMode; (void)profilingEnabled;
        core_ = GetBlockIdx();
#if defined(TILEXR_ENABLE_PROFILING)
        if (profilingEnabled != 0U) profile_.InitializeLatestCombine(commArgs, core_, 0U,
            kEpServerDedupCombineProfilingRecordsPerLaunch);
#endif
        Mark(kEpServerDedupCombineProfileStart);
        Mark(kEpServerDedupCombineProfileInitBegin);
        args_ = reinterpret_cast<const __gm__ TileXR::CommArgs *>(commArgs);
        h_ = h;
        // Load the device-produced prefix through MTE2 rather than caching a
        // mutable GM scalar across launches. This 32B temporary becomes the
        // independent M2 prepare sum only after the scalar count is consumed.
        pipe_.InitBuffer(arena_, kEpServerDedupCombineUbBytes);
        prepareSum_ = Allocate<float>(32U);
        GlobalTensor<int32_t> prefix;
        prefix.SetGlobalBuffer(reinterpret_cast<__gm__ int32_t *>(sendCounts));
        auto tokenCount = prepareSum_.ReinterpretCast<int32_t>();
        DataCopyPad(tokenCount, prefix[experts - 1U], {1U, 4U, 0U, 0U, 0U}, {false, 0U, 0U, 0});
        Sync<HardEvent::MTE2_S>();
        const int32_t tokens = tokenCount.GetValue(0U);
        const uint32_t localExperts = experts / uint32_t(args_->rankSize);
        const uint32_t copies = topK < localExperts ? topK : localExperts;
        const uint64_t capacity = uint64_t(args_->rankSize) * bs * copies;
        if (tokens < 0 || uint32_t(tokens) > kEpServerDedupCombineMaxTokenNum || uint64_t(tokens) > capacity) {
            Trap(); // Invalid device data must not become a truncated successful launch.
            return;
        }
        tokens_ = uint32_t(tokens);
        lanes_ = EpCombinePayloadLaneCount(tokens_, h_);
        if (lanes_ == 0U) { Trap(); return; } // Also enforced by Host for the maximum valid count.
        laneBytes_ = EpCombineAlign(h_ * 2U, 32U);
        window_ = (uint64_t(magic) % 2U) * (args_->peerMemBytes / 2U);
        control_ = window_ + TILEXR_EP_SERVER_DEDUP_COMBINE_CONTROL_OFFSET;
        const uint64_t resultStride = (uint64_t(h_) * 2U + 479U) / 480U * 512U;
        stageStride_ = (uint64_t(h_) * 2U + 511U) / 512U * 512U;
        stageBase_ = window_ + TILEXR_EP_SERVER_DEDUP_SHARED_DATA_OFFSET + uint64_t(bs) * topK * resultStride;
        expert_.SetGlobalBuffer(reinterpret_cast<__gm__ uint16_t *>(expertOut));
        table_.SetGlobalBuffer(reinterpret_cast<__gm__ int32_t *>(assistInfoForCombine));
        types_.SetGlobalBuffer(reinterpret_cast<__gm__ int32_t *>(tokenType));
        stage_.SetGlobalBuffer(reinterpret_cast<__gm__ uint16_t *>(args_->peerMems[args_->rank] + stageBase_));
        const uint32_t rowBytes = EpCombineAlign(tokens_ == 0U ? 4U : tokens_ * 4U, 256U);
        typeUb_ = Allocate<int32_t>(rowBytes);
        indexUb_ = Allocate<int32_t>(rowBytes);
        packedUb_ = Allocate<int32_t>(rowBytes);
        maskUb_ = Allocate<uint8_t>(EpCombineAlign((tokens_ + 7U) / 8U + 32U, 256U));
        tableUb_ = Allocate<int32_t>(kEpServerDedupCombineTableRows * 12U);
        preparePublish_ = Allocate<float>(256U);
        prepareInput_ = Allocate<float>(2304U);
        prepareWork_ = Allocate<float>(512U);
        prepareZero_ = Allocate<float>(2048U);
        readyInput_ = Allocate<float>(512U);
        readyWork_ = Allocate<float>(256U);
        readySum_ = Allocate<float>(32U);
        readyZero_ = Allocate<float>(512U);
        notifyUb_ = Allocate<float>(kEpServerDedupCombineRecvCoreNum * 32U);
        payloadUb_ = Allocate<uint16_t>(lanes_ * laneBytes_);
        Mark(kEpServerDedupCombineProfileInitEnd);
    }

    __aicore__ inline void Process()
    {
        CopyLocalPendingTokens();
        if (core_ >= 56U) NotifyServerCard();
        if (core_ >= kEpServerDedupCombineSendCoreNum) WaitServerReady();
        Mark(kEpServerDedupCombineProfileDrainBegin);
        // Retires prepare publication, both zero sources and server notification;
        // no arena/pipe reset or exit may precede completion of their DMA reads.
        PipeBarrier<PIPE_ALL>();
        Mark(kEpServerDedupCombineProfileDrainEnd);
        Mark(kEpServerDedupCombineProfileDelayBegin);
        const uint64_t begin = static_cast<uint64_t>(GetSystemCycle());
        while (static_cast<uint64_t>(GetSystemCycle()) - begin < 50000U) {}
        Mark(kEpServerDedupCombineProfileDelayEnd);
        Mark(kEpServerDedupCombineProfileEnd);
#if defined(TILEXR_ENABLE_PROFILING)
        profile_.Publish();
#endif
    }

private:
    __aicore__ inline void Mark(uint32_t event)
    {
#if defined(TILEXR_ENABLE_PROFILING)
        profile_.Record(event);
#else
        (void)event;
#endif
    }
    template<class T> __aicore__ inline LocalTensor<T> Allocate(uint32_t bytes)
    {
        auto result = arena_.Get<uint8_t>()[usedBytes_].template ReinterpretCast<T>();
        usedBytes_ += bytes;
        return result;
    }
    template<HardEvent E> __aicore__ inline void Sync()
    {
        const TEventID id = pipe_.FetchEventID(E);
        SetFlag<E>(id); WaitFlag<E>(id);
    }
    __aicore__ inline GlobalTensor<float> Control(uint32_t rank, uint64_t offset)
    {
        GlobalTensor<float> result;
        result.SetGlobalBuffer(reinterpret_cast<__gm__ float *>(args_->peerMems[rank] + control_ + offset));
        return result;
    }
    __aicore__ inline void SelectRows()
    {
        Mark(kEpServerDedupCombineProfileSelectBegin);
        if (tokens_ != 0U) {
            DataCopyPad(typeUb_, types_, {1U, tokens_ * 4U, 0U, 0U, 0U}, {false, 0U, 0U, 0});
            Sync<HardEvent::MTE2_V>();
            Compares(maskUb_, typeUb_, int32_t(3), CMPMODE::EQ, tokens_);
            CreateVecIndex(indexUb_, int32_t(0), tokens_);
            PipeBarrier<PIPE_V>();
            GatherMask(packedUb_, indexUb_, maskUb_.ReinterpretCast<uint32_t>(), true,
                tokens_, {1U, 1U, 0U, 0U}, selected_);
            Sync<HardEvent::V_S>();
        }
        Mark(kEpServerDedupCombineProfileSelectEnd);
    }
    __aicore__ inline void CopyLocalPendingTokens()
    {
        SelectRows();
        const uint32_t base = uint32_t(selected_) / 64U, extra = uint32_t(selected_) % 64U;
        const uint32_t count = base + (core_ < extra ? 1U : 0U);
        const uint32_t start = core_ * base + (core_ < extra ? core_ : extra);
        Mark(kEpServerDedupCombineProfileCopyBegin);
        for (uint32_t lane = 0; lane < lanes_; ++lane) SetFlag<HardEvent::MTE3_MTE2>(lane);
        uint32_t cacheBegin = 0U, cacheEnd = 0U;
        for (uint32_t task = 0; task < count; ++task) {
            const uint32_t row = uint32_t(packedUb_.GetValue(start + task));
            if (row >= cacheEnd) {
                cacheBegin = row;
                cacheEnd = row + 1U;
                for (uint32_t next = task + 1U; next < count; ++next) {
                    const uint32_t nextRow = uint32_t(packedUb_.GetValue(start + next));
                    if (nextRow - cacheBegin >= kEpServerDedupCombineTableRows) break;
                    cacheEnd = nextRow + 1U;
                }
                // The old metadata has no users after its scalar offset is read.
                // Payload DMA owns separate lanes, so changing this cache needs
                // no payload drain and preserves the lane sequence across batches.
                DataCopyPad(tableUb_, table_[uint64_t(cacheBegin) * 3U],
                    {1U, (cacheEnd - cacheBegin) * 12U, 0U, 0U, 0U}, {false, 0U, 0U, 0});
                Sync<HardEvent::MTE2_S>();
            }
            const uint32_t slot = uint32_t(tableUb_.GetValue((row - cacheBegin) * 3U));
            const uint32_t lane = task % lanes_;
            WaitFlag<HardEvent::MTE3_MTE2>(lane);
            auto payload = payloadUb_[lane * laneBytes_ / 2U];
            DataCopyPad(payload, expert_[uint64_t(row) * h_], {1U, h_ * 2U, 0U, 0U, 0U},
                {false, 0U, 0U, uint16_t(0)});
            SetFlag<HardEvent::MTE2_MTE3>(lane);
            WaitFlag<HardEvent::MTE2_MTE3>(lane);
            DataCopyPad(stage_[uint64_t(slot) * stageStride_ / 2U], payload, {1U, h_ * 2U, 0U, 0U, 0U});
            SetFlag<HardEvent::MTE3_MTE2>(lane);
        }
        // Also consumes seeded, unused lane events. All stage stores complete
        // before publication; individual tokens never wait immediately after store.
        for (uint32_t lane = 0; lane < lanes_; ++lane) WaitFlag<HardEvent::MTE3_MTE2>(lane);
        Mark(kEpServerDedupCombineProfileCopyEnd);
        Mark(kEpServerDedupCombineProfilePrepareBegin);
        Duplicate<float>(preparePublish_, 0.0f, 64U);
        Sync<HardEvent::V_S>();
        for (uint32_t j = 0; j < 8U; ++j) preparePublish_.SetValue(j * 8U, 1.0f);
        Sync<HardEvent::S_MTE3>();
        auto done = Control(args_->rank, TILEXR_EP_SERVER_DEDUP_COMBINE_PREPARE_DONE_OFFSET);
        DataCopyPad(done[core_ * 8U], preparePublish_, {8U, 32U, 0U, 2016U, 0U});
        Mark(kEpServerDedupCombineProfilePrepareEnd);
    }
    __aicore__ inline void WaitFlags(GlobalTensor<float> gm, LocalTensor<float> input,
        LocalTensor<float> work, LocalTensor<float> sum, uint32_t flags)
    {
        while (true) {
            // The previous reduction has completed before MTE2 overwrites input.
            Sync<HardEvent::V_MTE2>();
            DataCopyPad(input, gm, {1U, flags * 32U, 0U, 0U, 0U}, {false, 0U, 0U, 0.0f});
            Sync<HardEvent::MTE2_V>();
            ReduceSum<float>(sum, input, work, 1U, flags, 1U);
            Sync<HardEvent::V_S>();
            if (sum.GetValue(0U) == float(flags)) return;
        }
    }
    __aicore__ inline void NotifyServerCard()
    {
        auto done = Control(args_->rank, TILEXR_EP_SERVER_DEDUP_COMBINE_PREPARE_DONE_OFFSET +
            uint64_t(core_ - 56U) * 2048U);
        Mark(kEpServerDedupCombineProfileLocalWaitBegin);
        WaitFlags(done, prepareInput_, prepareWork_, prepareSum_, 64U);
        Mark(kEpServerDedupCombineProfileLocalWaitEnd);
        Mark(kEpServerDedupCombineProfileNotifyBegin);
        Duplicate<float>(prepareZero_, 0.0f, 512U);
        Duplicate<float>(notifyUb_, 0.0f, kEpServerDedupCombineRecvCoreNum * 8U);
        Sync<HardEvent::V_S>();
        for (uint32_t r = 0; r < kEpServerDedupCombineRecvCoreNum; ++r) notifyUb_.SetValue(r * 8U, 1.0f);
        Sync<HardEvent::V_MTE3>();
        Sync<HardEvent::S_MTE3>();
        DataCopyPad(done, prepareZero_, {1U, 2048U, 0U, 0U, 0U});
        const uint32_t target = uint32_t(args_->rank) / 8U * 8U + core_ - 56U;
        auto ready = Control(target, TILEXR_EP_SERVER_DEDUP_COMBINE_SERVER_READY_OFFSET);
        DataCopyPad(ready[(uint32_t(args_->rank) % 8U) * 8U], notifyUb_,
            {uint16_t(kEpServerDedupCombineRecvCoreNum), 32U, 0U, 480U, 0U});
        Mark(kEpServerDedupCombineProfileNotifyEnd);
    }
    __aicore__ inline void WaitServerReady()
    {
        auto ready = Control(args_->rank, TILEXR_EP_SERVER_DEDUP_COMBINE_SERVER_READY_OFFSET +
            uint64_t(core_ - kEpServerDedupCombineSendCoreNum) * 512U);
        Mark(kEpServerDedupCombineProfileReadyWaitBegin);
        WaitFlags(ready, readyInput_, readyWork_, readySum_, 8U);
        Mark(kEpServerDedupCombineProfileReadyWaitEnd);
        Mark(kEpServerDedupCombineProfileClearBegin);
        Duplicate<float>(readyZero_, 0.0f, 128U);
        Sync<HardEvent::V_MTE3>();
        DataCopyPad(ready, readyZero_, {1U, 512U, 0U, 0U, 0U});
        Mark(kEpServerDedupCombineProfileClearEnd);
    }

    TPipe pipe_;
    TBuf<> arena_;
    const __gm__ TileXR::CommArgs *args_ = nullptr;
    uint32_t core_ = 0, h_ = 0, tokens_ = 0, lanes_ = 0, laneBytes_ = 0, usedBytes_ = 0;
    uint64_t selected_ = 0, window_ = 0, control_ = 0, stageBase_ = 0, stageStride_ = 0;
    GlobalTensor<uint16_t> expert_, stage_;
    GlobalTensor<int32_t> table_, types_;
    LocalTensor<int32_t> typeUb_, indexUb_, packedUb_, tableUb_;
    LocalTensor<uint8_t> maskUb_;
    LocalTensor<uint16_t> payloadUb_;
    LocalTensor<float> preparePublish_, prepareInput_, prepareWork_, prepareSum_, prepareZero_;
    LocalTensor<float> readyInput_, readyWork_, readySum_, readyZero_, notifyUb_;
#if defined(TILEXR_ENABLE_PROFILING)
    TileXR::DirectProfilingWriter profile_;
#endif
};
} // namespace TileXREp
#endif
