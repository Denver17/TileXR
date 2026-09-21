#ifndef TILEXR_EP_COMBINE_MEMORY_SERVER_DEDUP_KERNEL_H
#define TILEXR_EP_COMBINE_MEMORY_SERVER_DEDUP_KERNEL_H

#include "kernel_operator.h"
#if defined(TILEXR_ENABLE_PROFILING)
#include "ep_memory_server_dedup_combine_profile.h"
#include "tilexr_profiling.h"
#endif

namespace TileXREp {
// Phase-one skeleton: no business tensor or peer-memory reads/writes.
class CombineMemoryServerDedupKernel {
public:
    __aicore__ inline void Init(GM_ADDR commArgs, GM_ADDR expertOut, GM_ADDR assistInfoForCombine,
        GM_ADDR sendCounts, GM_ADDR expertScales, GM_ADDR tokenType, GM_ADDR relayReadIndex,
        GM_ADDR sourceMask, GM_ADDR yOut, uint32_t bs, uint32_t h, uint32_t topK,
        uint32_t experts, uint8_t dtype, uint8_t quantMode, int64_t magic, uint8_t profilingEnabled)
    {
        (void)commArgs; (void)expertOut; (void)assistInfoForCombine; (void)sendCounts;
        (void)expertScales; (void)tokenType; (void)relayReadIndex; (void)sourceMask; (void)yOut;
        (void)bs; (void)h; (void)topK; (void)experts; (void)dtype; (void)quantMode;
        (void)magic; (void)profilingEnabled;
#if defined(TILEXR_ENABLE_PROFILING)
        if (profilingEnabled != 0U) {
            profile_.InitializeLatestCombine(commArgs, AscendC::GetBlockIdx(), 0U,
                kEpServerDedupCombineProfilingRecordsPerLaunch);
        }
        profile_.Record(kEpServerDedupCombineProfileStart);
#endif
    }

    __aicore__ inline void Process()
    {
        // A5 / Ascend950 SYS_CNT runs at 1 GHz: 50 us is 50,000 ticks.
        // Keep every core in the empty operator before the next Dispatch round.
        // This delay is not a cross-rank synchronization barrier.
        constexpr uint64_t kEmptyCombineDelayTicks = 50U * 1000U;
        const uint64_t begin = static_cast<uint64_t>(AscendC::GetSystemCycle());
        while (static_cast<uint64_t>(AscendC::GetSystemCycle()) - begin < kEmptyCombineDelayTicks) {}
#if defined(TILEXR_ENABLE_PROFILING)
        profile_.Record(kEpServerDedupCombineProfileEnd);
        profile_.Publish();
#endif
    }

private:
#if defined(TILEXR_ENABLE_PROFILING)
    TileXR::DirectProfilingWriter profile_;
#endif
};
} // namespace TileXREp
#endif
