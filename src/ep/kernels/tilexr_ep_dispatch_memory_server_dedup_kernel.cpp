#include "tilexr_ep_dispatch_memory_server_dedup_kernel.h"

// Count-only increment: hidden, route weights and data/metadata outputs are not consumed.
extern "C" __global__ __aicore__ void tilexr_ep_dispatch_memory_server_dedup_kernel(
    GM_ADDR commArgs, GM_ADDR x, GM_ADDR expertIds, GM_ADDR expertScales,
    GM_ADDR expandXOut, GM_ADDR expertTokenNumsOut, GM_ADDR sendCountsOut,
    GM_ADDR assistInfoForCombineOut, GM_ADDR assistExOut, GM_ADDR sourceMaskOut, GM_ADDR groupOut,
    int64_t bs, int64_t h, int64_t topK, int64_t moeExpertNum, int64_t globalBs,
    int64_t expertTokenNumsType, int64_t dtype, int64_t expandXOutDtype, int64_t magic)
{
    // Host owns parameter validation and always launches the fixed 48/16 AIV split.
    (void)x; (void)expertScales; (void)expandXOut; (void)assistInfoForCombineOut;
    (void)assistExOut; (void)sourceMaskOut; (void)groupOut;
    (void)h; (void)globalBs; (void)dtype; (void)expandXOutDtype;
    AscendC::TPipe pipe;
    TileXREp::DispatchMemoryServerDedupKernel kernel;
    kernel.ProfileMark(0);
    kernel.Init(commArgs, expertIds, expertTokenNumsOut, sendCountsOut,
        static_cast<uint32_t>(bs), static_cast<uint32_t>(topK), static_cast<uint32_t>(moeExpertNum),
        static_cast<uint32_t>(expertTokenNumsType), magic, &pipe);
    kernel.ProfileMark(1);
    kernel.Process();
}
