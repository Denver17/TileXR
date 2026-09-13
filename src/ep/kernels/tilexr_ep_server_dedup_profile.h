#ifndef TILEXR_EP_SERVER_DEDUP_DEVICE_PROFILE_H
#define TILEXR_EP_SERVER_DEDUP_DEVICE_PROFILE_H

#include "kernel_operator.h"
#include "ep_memory_server_dedup_profile.h"

namespace TileXREp {
using namespace AscendC;
class ServerDedupProfile {
public:
    __aicore__ inline void Mark(uint32_t point)
    {
#if defined(TILEXR_EP_SERVER_DEDUP_ENABLE_PROFILING) && TILEXR_EP_SERVER_DEDUP_ENABLE_PROFILING
        AscendC::PipeBarrier<PIPE_ALL>();
        time_[point] = static_cast<uint64_t>(AscendC::GetSystemCycle());
#else
        (void)point;
#endif
    }
    __aicore__ inline void Skip(uint32_t point, uint32_t previous)
    {
#if defined(TILEXR_EP_SERVER_DEDUP_ENABLE_PROFILING) && TILEXR_EP_SERVER_DEDUP_ENABLE_PROFILING
        time_[point] = time_[previous];
#else
        (void)point; (void)previous;
#endif
    }
    __aicore__ inline void Flush(const __gm__ TileXR::CommArgs *args, uint64_t generation)
    {
#if defined(TILEXR_EP_SERVER_DEDUP_ENABLE_PROFILING) && TILEXR_EP_SERVER_DEDUP_ENABLE_PROFILING
        const uint32_t core = AscendC::GetBlockIdx();
        for (uint32_t segment = 0; segment < kEpServerDedupProfileSegments; ++segment) {
            __gm__ EpServerDedupProfileRecord *record =
                reinterpret_cast<__gm__ EpServerDedupProfileRecord *>(args->peerMems[args->rank]) +
                    core * kEpServerDedupProfileSegments + segment;
            record->marker = kEpServerDedupProfileMarker;
            record->version = kEpServerDedupProfileVersion;
            record->recordBytes = sizeof(*record);
            record->rank = args->rank;
            record->core = core;
            record->blockDim = kEpServerDedupCoreNum;
            record->segment = segment;
            record->generation = generation;
            for (uint32_t i = 0; i < 4U; ++i) record->time[i] = time_[segment * 4U + i];
            AscendC::GlobalTensor<uint8_t> output;
            output.SetGlobalBuffer(reinterpret_cast<__gm__ uint8_t *>(record), sizeof(*record));
            AscendC::DataCacheCleanAndInvalid<uint8_t, AscendC::CacheLine::SINGLE_CACHE_LINE,
                AscendC::DcciDst::CACHELINE_OUT>(output);
        }
#else
        (void)args; (void)generation;
#endif
    }
private:
#if defined(TILEXR_EP_SERVER_DEDUP_ENABLE_PROFILING) && TILEXR_EP_SERVER_DEDUP_ENABLE_PROFILING
    uint64_t time_[kEpServerDedupProfilePoints] {};
#endif
};
} // namespace TileXREp
#endif
