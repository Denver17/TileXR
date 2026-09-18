#include "tilexr_ep_dispatch_memory_server_dedup_kernel.h"
#include "tilexr_types.h"
#include "ep_memory_server_dedup_tiling.h"

// One production entry and one 64-core process with a compile-time send/recv split.
extern "C" __global__ __aicore__ __mix__(0, 1) void tilexr_ep_dispatch_memory_server_dedup_kernel(
    GM_ADDR commArgs, GM_ADDR x, GM_ADDR expertIds, GM_ADDR expertScales,
    GM_ADDR expandXOut, GM_ADDR dynamicScalesOut, GM_ADDR expertTokenNumsOut, GM_ADDR sendCountsOut,
    GM_ADDR assistInfoForCombineOut, GM_ADDR assistExOut, GM_ADDR sourceMaskOut, GM_ADDR groupOut,
    GM_ADDR expertScalesOut, GM_ADDR tilingData, int64_t magic)
{
    // Host owns validation; each core reads its configuration once before work.
    (void)assistInfoForCombineOut;
    const auto *tiling = reinterpret_cast<const __gm__ TileXREp::EpServerDedupTilingData *>(tilingData);
    const uint32_t bs = tiling->bs;
    const uint32_t h = tiling->h;
    const uint32_t topK = tiling->topK;
    const uint32_t moeExpertNum = tiling->moeExpertNum;
    const uint32_t expertTokenNumsType = tiling->expertTokenNumsType;
    const uint32_t profilingLaunchSlot = tiling->profilingLaunchSlot;
    const uint8_t quantMode = tiling->quantMode;
    const uint8_t dtype = tiling->dtype;
    const uint8_t expandXOutDtype = tiling->expandXOutDtype;
    AscendC::TPipe pipe;
#define TILEXR_RUN_DEDUP(XType, Quantized, E4M3) \
    TileXREp::DispatchMemoryServerDedupKernel<XType, Quantized, E4M3> kernel; \
    kernel.Init(commArgs, x, expertIds, expertScales, expandXOut, dynamicScalesOut, \
        expertTokenNumsOut, sendCountsOut, assistExOut, sourceMaskOut, groupOut, expertScalesOut, \
        bs, h, \
        topK, moeExpertNum, expertTokenNumsType, magic, profilingLaunchSlot, &pipe); \
    kernel.Process()
    if (dtype == TileXR::TILEXR_DATA_TYPE_FP16) {
        if (quantMode == 0) { TILEXR_RUN_DEDUP(half, false, false); }
        else if (expandXOutDtype == TileXR::TILEXR_DATA_TYPE_FP8E4M3) { TILEXR_RUN_DEDUP(half, true, true); }
        else { TILEXR_RUN_DEDUP(half, true, false); }
    } else {
        if (quantMode == 0) { TILEXR_RUN_DEDUP(bfloat16_t, false, false); }
        else if (expandXOutDtype == TileXR::TILEXR_DATA_TYPE_FP8E4M3) { TILEXR_RUN_DEDUP(bfloat16_t, true, true); }
        else { TILEXR_RUN_DEDUP(bfloat16_t, true, false); }
    }
#undef TILEXR_RUN_DEDUP
}
