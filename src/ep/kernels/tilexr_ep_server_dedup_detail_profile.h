#ifndef TILEXR_EP_SERVER_DEDUP_DETAIL_PROFILE_H
#define TILEXR_EP_SERVER_DEDUP_DETAIL_PROFILE_H

#include "kernel_operator.h"
#include "ep_memory_server_dedup_profile.h"

namespace TileXREp {
// Passive Scalar timestamps. In particular, timing a WaitFlag targeting MTE2
// alone would not measure its wait; AcquireLoad ends at the existing MTE2_S.
class ServerDedupDetailProfile {
public:
    __aicore__ inline void Init(const __gm__ TileXR::CommArgs *args, uint32_t bs, uint32_t k)
    {
#if defined(TILEXR_EP_SERVER_DEDUP_ENABLE_PROFILING) && TILEXR_EP_SERVER_DEDUP_ENABLE_PROFILING
        offset_ = TileXREpServerDedupDetailOffset(TileXREpServerDedupSendProfileLayout(args->peerMemBytes, bs, k).offset);
#else
        (void)args; (void)bs; (void)k;
#endif
    }
    __aicore__ inline uint64_t Now()
    {
#if defined(TILEXR_EP_SERVER_DEDUP_ENABLE_PROFILING) && TILEXR_EP_SERVER_DEDUP_ENABLE_PROFILING
        return uint64_t(AscendC::GetSystemCycle());
#else
        return 0U;
#endif
    }
    __aicore__ inline void Mark(EpDedupDetailPoint point)
    {
#if defined(TILEXR_EP_SERVER_DEDUP_ENABLE_PROFILING) && TILEXR_EP_SERVER_DEDUP_ENABLE_PROFILING
        record_.points[uint32_t(point)] = Now();
#else
        (void)point;
#endif
    }
    __aicore__ inline void CopyPoints(uint32_t first, uint32_t last, EpDedupDetailPoint source)
    {
#if defined(TILEXR_EP_SERVER_DEDUP_ENABLE_PROFILING) && TILEXR_EP_SERVER_DEDUP_ENABLE_PROFILING
        for (uint32_t i = first; i <= last; ++i) record_.points[i] = record_.points[uint32_t(source)];
#else
        (void)first; (void)last; (void)source;
#endif
    }
    __aicore__ inline void Count(EpDedupDetailCounter counter, uint64_t count = 1U)
    {
#if defined(TILEXR_EP_SERVER_DEDUP_ENABLE_PROFILING) && TILEXR_EP_SERVER_DEDUP_ENABLE_PROFILING
        record_.counters[uint32_t(counter)] += count;
#else
        (void)counter; (void)count;
#endif
    }
    __aicore__ inline void Set(EpDedupDetailCounter counter, uint64_t value)
    {
#if defined(TILEXR_EP_SERVER_DEDUP_ENABLE_PROFILING) && TILEXR_EP_SERVER_DEDUP_ENABLE_PROFILING
        record_.counters[uint32_t(counter)] = value;
#else
        (void)counter; (void)value;
#endif
    }
    __aicore__ inline bool Sample(EpDedupDetailMetric metric, uint64_t begin)
    {
#if defined(TILEXR_EP_SERVER_DEDUP_ENABLE_PROFILING) && TILEXR_EP_SERVER_DEDUP_ENABLE_PROFILING
        const uint64_t end = Now();
        auto &value = record_.metrics[uint32_t(metric)];
        const bool longest = value.count == 0U || end - begin > value.maxEnd - value.maxBegin;
        value.totalTicks += end - begin;
        ++value.count;
        if (longest) { value.maxBegin = begin; value.maxEnd = end; }
        return longest;
#else
        (void)metric; (void)begin;
        return false;
#endif
    }
    __aicore__ inline void Flush(const __gm__ TileXR::CommArgs *args, uint64_t generation)
    {
#if defined(TILEXR_EP_SERVER_DEDUP_ENABLE_PROFILING) && TILEXR_EP_SERVER_DEDUP_ENABLE_PROFILING
        const uint32_t core = AscendC::GetBlockIdx();
        auto *output = reinterpret_cast<__gm__ EpServerDedupDetailRecord *>(args->peerMems[args->rank] + offset_) + core;
        output->marker = kEpServerDedupProfileMarker;
        output->version = kEpServerDedupProfileVersion;
        output->recordBytes = sizeof(EpServerDedupDetailRecord);
        output->rank = args->rank; output->core = core;
        output->generation = generation; output->reserved = 0U;
        for (uint32_t i = 0; i < 16U; ++i) {
            output->points[i] = record_.points[i];
            output->counters[i] = record_.counters[i];
        }
        for (uint32_t i = 0; i < 7U; ++i) {
            output->metrics[i].totalTicks = record_.metrics[i].totalTicks;
            output->metrics[i].maxBegin = record_.metrics[i].maxBegin;
            output->metrics[i].maxEnd = record_.metrics[i].maxEnd;
            output->metrics[i].count = record_.metrics[i].count;
        }
        AscendC::GlobalTensor<uint8_t> bytes;
        // The indexed DCCI overload performs scalar internal-buffer bounds
        // checking. Set the tensor extent before using bytes[offset]; without
        // it, profiling-only execution can raise AIV error 271 at stream sync.
        bytes.SetGlobalBuffer(reinterpret_cast<__gm__ uint8_t *>(output), sizeof(*output));
        for (uint32_t offset = 0; offset < sizeof(EpServerDedupDetailRecord); offset += 64U)
            AscendC::DataCacheCleanAndInvalid<uint8_t, AscendC::CacheLine::SINGLE_CACHE_LINE,
                AscendC::DcciDst::CACHELINE_OUT>(bytes[offset]);
#else
        (void)args; (void)generation;
#endif
    }
private:
#if defined(TILEXR_EP_SERVER_DEDUP_ENABLE_PROFILING) && TILEXR_EP_SERVER_DEDUP_ENABLE_PROFILING
    EpServerDedupDetailRecord record_ {};
    uint64_t offset_ = 0U;
#endif
};
} // namespace TileXREp
#endif
