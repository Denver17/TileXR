#ifndef TILEXR_EP_DISPATCH_MEMORY_SERVER_DEDUP_KERNEL_H
#define TILEXR_EP_DISPATCH_MEMORY_SERVER_DEDUP_KERNEL_H

#include "kernel_operator.h"
#include "tilexr_types.h"
#include "comm_args.h"
#include "ep_memory_server_dedup_profile.h"

namespace TileXREp {
class DispatchMemoryServerDedupKernel {
public:
    __aicore__ inline void Init(GM_ADDR commArgs, int64_t magic, uint32_t blockDim)
    {
        commArgs_ = reinterpret_cast<const __gm__ TileXR::CommArgs *>(commArgs);
        magic_ = magic;
        blockDim_ = blockDim;
        start_ = static_cast<uint64_t>(AscendC::GetSystemCycle());
    }

    __aicore__ inline void Process()
    {
#if defined(TILEXR_EP_SERVER_DEDUP_ENABLE_PROFILING) && TILEXR_EP_SERVER_DEDUP_ENABLE_PROFILING
        if (commArgs_ == nullptr || blockDim_ == 0U || blockDim_ > kEpServerDedupProfileMaxCores) return;
        const uint32_t core = AscendC::GetBlockIdx();
        if (core >= blockDim_ || commArgs_->rank < 0 || commArgs_->rank >= commArgs_->rankSize) return;
        __gm__ uint8_t *const localData = commArgs_->peerMems[commArgs_->rank];
        const uint64_t bytes = static_cast<uint64_t>(blockDim_) * sizeof(EpServerDedupProfileRecord);
        if (localData == nullptr || reinterpret_cast<uint64_t>(localData) % 64U != 0U ||
            commArgs_->peerMemBytes < kEpServerDedupProfileOffset + bytes) return;
        __gm__ EpServerDedupProfileRecord *record = reinterpret_cast<__gm__ EpServerDedupProfileRecord *>(
            localData + kEpServerDedupProfileOffset + static_cast<uint64_t>(core) * sizeof(*record));
        record->marker = kEpServerDedupProfileMarker;
        record->version = kEpServerDedupProfileVersion;
        record->recordBytes = sizeof(*record);
        record->rank = static_cast<uint32_t>(commArgs_->rank);
        record->core = core;
        record->blockDim = blockDim_;
        record->reserved = 0U;
        record->generation = static_cast<uint64_t>(magic_);
        record->start = start_;
        record->checked = start_;
        record->end = static_cast<uint64_t>(AscendC::GetSystemCycle());
        record->reserved2 = 0U;
        AscendC::GlobalTensor<uint8_t> global;
        global.SetGlobalBuffer(reinterpret_cast<__gm__ uint8_t *>(record), sizeof(*record));
        AscendC::DataCacheCleanAndInvalid<uint8_t, AscendC::CacheLine::SINGLE_CACHE_LINE,
            AscendC::DcciDst::CACHELINE_OUT>(global[0]);
#endif
    }
private:
    const __gm__ TileXR::CommArgs *commArgs_ = nullptr;
    int64_t magic_ = 0;
    uint32_t blockDim_ = 0;
    uint64_t start_ = 0;
};
}
#endif
