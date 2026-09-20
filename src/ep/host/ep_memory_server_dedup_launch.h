#ifndef TILEXR_EP_MEMORY_SERVER_DEDUP_LAUNCH_H
#define TILEXR_EP_MEMORY_SERVER_DEDUP_LAUNCH_H

#include "ep_memory_server_dedup_host.h"
#include "ep_memory_server_dedup_tiling.h"
#include <type_traits>

namespace TileXREp {

// Internal by-value launch ABI, matching the AICore entry; not public metadata.
struct EpServerDedupKernelArgs {
    GM_ADDR commArgs;
    GM_ADDR x;
    GM_ADDR expertIds;
    GM_ADDR expertScales;
    GM_ADDR expandXOut;
    GM_ADDR dynamicScalesOut;
    GM_ADDR expertTokenNumsOut;
    GM_ADDR sendCountsOut;
    GM_ADDR tokenTypeOut;
    GM_ADDR destinationIndexOut;
    GM_ADDR relayReadIndexOut;
    GM_ADDR sourceMaskOut;
    GM_ADDR expertScalesOut;
    GM_ADDR tilingData;
    int64_t magic;
};
static_assert(std::is_standard_layout<EpServerDedupKernelArgs>::value, "launch arguments must be standard layout");
static_assert(sizeof(EpServerDedupKernelArgs) == 120, "launch ABI has fourteen addresses and magic");
static_assert(offsetof(EpServerDedupKernelArgs, dynamicScalesOut) == 40, "quant scale address");
static_assert(offsetof(EpServerDedupKernelArgs, tokenTypeOut) == 64, "token type address");
static_assert(offsetof(EpServerDedupKernelArgs, destinationIndexOut) == 72, "destination index address");
static_assert(offsetof(EpServerDedupKernelArgs, relayReadIndexOut) == 80, "relay read index address");
static_assert(offsetof(EpServerDedupKernelArgs, sourceMaskOut) == 88, "source mask address");
static_assert(offsetof(EpServerDedupKernelArgs, expertScalesOut) == 96, "source weight address");
static_assert(offsetof(EpServerDedupKernelArgs, tilingData) == 104, "tiling GM address");
static_assert(offsetof(EpServerDedupKernelArgs, magic) == 112, "communication magic offset");

// 内部注册入口也供安装包测试调用；不需要创建 communicator。
int TileXREpRegisterDispatchMemoryServerDedupKernel();
int TileXREpSubmitDispatchMemoryServerDedupKernel(EpServerDedupKernelArgs &args,
    uint32_t blockDim, aclrtStream stream);

int TileXREpLaunchDispatchMemoryServerDedupKernel(const EpMemoryServerDedupParams &params,
    const EpHostLaunchContext &context, const EpMemoryServerDedupLayout &layout);

} // namespace TileXREp

#endif // TILEXR_EP_MEMORY_SERVER_DEDUP_LAUNCH_H
