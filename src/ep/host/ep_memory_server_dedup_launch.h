#ifndef TILEXR_EP_MEMORY_SERVER_DEDUP_LAUNCH_H
#define TILEXR_EP_MEMORY_SERVER_DEDUP_LAUNCH_H

#include "ep_memory_server_dedup_host.h"
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
    GM_ADDR assistInfoForCombineOut;
    GM_ADDR assistExOut;
    GM_ADDR sourceMaskOut;
    GM_ADDR groupOut;
    GM_ADDR expertScalesOut;
    int64_t bs;
    int64_t h;
    int64_t topK;
    int64_t moeExpertNum;
    int64_t globalBs;
    int64_t expertTokenNumsType;
    int64_t quantMode;
    int64_t dtype;
    int64_t expandXOutDtype;
    int64_t magic;
    int64_t profilingLaunchSlot;
    int64_t sendCoreNum;
};
static_assert(std::is_standard_layout<EpServerDedupKernelArgs>::value, "launch arguments must be standard layout");
static_assert(sizeof(EpServerDedupKernelArgs) == 200, "launch ABI has thirteen addresses and twelve int64 fields");
static_assert(offsetof(EpServerDedupKernelArgs, dynamicScalesOut) == 40, "quant scale address");
static_assert(offsetof(EpServerDedupKernelArgs, expertScalesOut) == 96, "source weight address");
static_assert(offsetof(EpServerDedupKernelArgs, bs) == 104, "shape follows thirteen GM addresses");
static_assert(offsetof(EpServerDedupKernelArgs, quantMode) == 152, "quant mode");
static_assert(offsetof(EpServerDedupKernelArgs, magic) == 176, "communication magic offset");
static_assert(offsetof(EpServerDedupKernelArgs, profilingLaunchSlot) == 184, "profiling slot offset");
static_assert(offsetof(EpServerDedupKernelArgs, sendCoreNum) == 192, "runtime send core count is the final argument");

// 内部注册入口也供安装包测试调用；不需要创建 communicator。
int TileXREpRegisterDispatchMemoryServerDedupKernel();
int TileXREpSubmitDispatchMemoryServerDedupKernel(EpServerDedupKernelArgs &args,
    uint32_t blockDim, aclrtStream stream);

int TileXREpLaunchDispatchMemoryServerDedupKernel(const EpMemoryServerDedupParams &params,
    const EpHostLaunchContext &context, const EpMemoryServerDedupLayout &layout);

} // namespace TileXREp

#endif // TILEXR_EP_MEMORY_SERVER_DEDUP_LAUNCH_H
