#include "kernel_operator.h"
#include "ep_memory_server_dedup_combine_tiling.h"

// Phase-one empty operator: no tensor, CommArgs or peer-memory reads/writes.
// No profiling writes until C6 provides a separate Combine record region.
extern "C" __global__ __aicore__ __mix__(0, 1) void tilexr_ep_combine_memory_server_dedup_kernel(
    GM_ADDR commArgs, GM_ADDR expertOut, GM_ADDR assistInfoForCombine, GM_ADDR sendCounts,
    GM_ADDR expertScales, GM_ADDR tokenType, GM_ADDR relayReadIndex, GM_ADDR sourceMask,
    GM_ADDR yOut, GM_ADDR tilingData, int64_t magic)
{
    if (tilingData == nullptr) { return; }
    // Read the independent scalar configuration once per core. Volatile keeps
    // these reads observable in the otherwise empty phase-one entry.
    const volatile __gm__ TileXREp::EpServerDedupCombineTilingData *tiling =
        reinterpret_cast<const volatile __gm__ TileXREp::EpServerDedupCombineTilingData *>(tilingData);
    const uint32_t bs = tiling->bs;
    const uint32_t h = tiling->h;
    const uint16_t experts = tiling->moeExpertNum;
    const uint8_t topK = tiling->topK;
    const uint8_t dtype = tiling->dtype;
    const uint8_t quantMode = tiling->quantMode;
    (void)bs; (void)h; (void)experts; (void)topK; (void)dtype; (void)quantMode;

    // A5 / Ascend950 SYS_CNT runs at 1 GHz: 50 us is 50,000 ticks.
    // Keep every core in the empty operator for this interval before the next
    // Dispatch round. This delay is not a cross-rank synchronization barrier.
    constexpr uint64_t kEmptyCombineDelayTicks = 50U * 1000U;
    const uint64_t begin = static_cast<uint64_t>(AscendC::GetSystemCycle());
    while (static_cast<uint64_t>(AscendC::GetSystemCycle()) - begin < kEmptyCombineDelayTicks) {}

    (void)commArgs; (void)expertOut; (void)assistInfoForCombine; (void)sendCounts;
    (void)expertScales; (void)tokenType; (void)relayReadIndex; (void)sourceMask;
    (void)yOut; (void)magic;
}
