#include "tilexr_ep_combine_memory_server_dedup_kernel.h"
#include "ep_memory_server_dedup_combine_tiling.h"

// Phase-one empty operator: no business tensor or peer-memory reads/writes.
// C6 optionally writes only the independent Combine profiling region.
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
    uint8_t profilingEnabled = 0;
#if defined(TILEXR_ENABLE_PROFILING)
    profilingEnabled = tiling->profilingEnabled;
#endif
    TileXREp::CombineMemoryServerDedupKernel kernel;
    kernel.Init(commArgs, expertOut, assistInfoForCombine, sendCounts, expertScales,
        tokenType, relayReadIndex, sourceMask, yOut, bs, h, topK, experts, dtype, quantMode,
        magic, profilingEnabled);
    kernel.Process();
}
