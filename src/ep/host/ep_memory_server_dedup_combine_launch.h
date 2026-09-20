#ifndef TILEXR_EP_MEMORY_SERVER_DEDUP_COMBINE_LAUNCH_H
#define TILEXR_EP_MEMORY_SERVER_DEDUP_COMBINE_LAUNCH_H

#include "ep_memory_server_dedup_combine_host.h"
#include "ep_memory_server_dedup_combine_tiling.h"

namespace TileXREp {
// Internal ABI: ten GM addresses followed by an independent communication magic.
// Keep the order identical to tilexr_ep_combine_memory_server_dedup_kernel.
struct EpServerDedupCombineKernelArgs {
    GM_ADDR commArgs;
    GM_ADDR expertOut;
    GM_ADDR assistInfoForCombine;
    GM_ADDR sendCounts;
    GM_ADDR expertScales;
    GM_ADDR tokenType;
    GM_ADDR relayReadIndex;
    GM_ADDR sourceMask;
    GM_ADDR yOut;
    GM_ADDR tilingData;
    int64_t magic;
};
static_assert(std::is_standard_layout<EpServerDedupCombineKernelArgs>::value, "standard layout launch args");
static_assert(sizeof(EpServerDedupCombineKernelArgs) == 88, "Combine launch is 88 bytes");
static_assert(offsetof(EpServerDedupCombineKernelArgs, commArgs) == 0, "comm offset");
static_assert(offsetof(EpServerDedupCombineKernelArgs, expertOut) == 8, "expertOut offset");
static_assert(offsetof(EpServerDedupCombineKernelArgs, assistInfoForCombine) == 16, "table2 offset");
static_assert(offsetof(EpServerDedupCombineKernelArgs, sendCounts) == 24, "prefix offset");
static_assert(offsetof(EpServerDedupCombineKernelArgs, expertScales) == 32, "table5 offset");
static_assert(offsetof(EpServerDedupCombineKernelArgs, tokenType) == 40, "table1 offset");
static_assert(offsetof(EpServerDedupCombineKernelArgs, relayReadIndex) == 48, "table3 offset");
static_assert(offsetof(EpServerDedupCombineKernelArgs, sourceMask) == 56, "table4 offset");
static_assert(offsetof(EpServerDedupCombineKernelArgs, yOut) == 64, "yOut offset");
static_assert(offsetof(EpServerDedupCombineKernelArgs, tilingData) == 72, "GM tiling offset");
static_assert(offsetof(EpServerDedupCombineKernelArgs, magic) == 80, "magic offset");

int TileXREpRegisterCombineMemoryServerDedupKernel();
// Caller has registered the binary and owns argument lifetimes until stream completion.
int TileXREpSubmitCombineMemoryServerDedupKernel(EpServerDedupCombineKernelArgs &args,
    uint32_t blockDim, aclrtStream stream);
} // namespace TileXREp
#endif
