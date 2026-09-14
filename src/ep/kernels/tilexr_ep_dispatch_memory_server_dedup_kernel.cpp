#include "tilexr_ep_dispatch_memory_server_dedup_kernel.h"
#include "tilexr_types.h"

// One production entry and one 48/16-core process, including the final hard SyncAll.
extern "C" __global__ __aicore__ __mix__(0, 1) void tilexr_ep_dispatch_memory_server_dedup_kernel(
    GM_ADDR commArgs, GM_ADDR x, GM_ADDR expertIds, GM_ADDR expertScales,
    GM_ADDR expandXOut, GM_ADDR dynamicScalesOut, GM_ADDR expertTokenNumsOut, GM_ADDR sendCountsOut,
    GM_ADDR assistInfoForCombineOut, GM_ADDR assistExOut, GM_ADDR sourceMaskOut, GM_ADDR groupOut,
    GM_ADDR expertScalesOut, int64_t bs, int64_t h, int64_t topK, int64_t moeExpertNum, int64_t globalBs,
    int64_t expertTokenNumsType, int64_t quantMode, int64_t dtype, int64_t expandXOutDtype, int64_t magic, int64_t profilingLaunchSlot)
{
    // Host owns parameter validation and always launches the fixed 48/16 AIV split.
    (void)assistInfoForCombineOut;
    (void)globalBs;
    AscendC::TPipe pipe;
#define TILEXR_RUN_DEDUP(XType, Quantized, E4M3) \
    TileXREp::DispatchMemoryServerDedupKernel<XType, Quantized, E4M3> kernel; \
    kernel.Init(commArgs, x, expertIds, expertScales, expandXOut, dynamicScalesOut, \
        expertTokenNumsOut, sendCountsOut, assistExOut, sourceMaskOut, groupOut, expertScalesOut, \
        uint32_t(bs), uint32_t(h), \
        uint32_t(topK), uint32_t(moeExpertNum), uint32_t(expertTokenNumsType), magic, uint32_t(profilingLaunchSlot), &pipe); \
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
