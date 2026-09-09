#ifndef TILEXR_EP_MEMORY_SERVER_DEDUP_LAUNCH_H
#define TILEXR_EP_MEMORY_SERVER_DEDUP_LAUNCH_H

#include "ep_memory_server_dedup_host.h"
#include <type_traits>

namespace TileXREp {

// 内部按值 launch ABI；顺序与空 AICore 入口逐项对应，不是公共 metadata。
struct EpServerDedupKernelArgs {
    GM_ADDR commArgs;
    GM_ADDR x;
    GM_ADDR expertIds;
    GM_ADDR expertScales;
    GM_ADDR expandXOut;
    GM_ADDR expertTokenNumsOut;
    GM_ADDR sendCountsOut;
    GM_ADDR assistInfoForCombineOut;
    GM_ADDR assistExOut;
    GM_ADDR sourceMaskOut;
    GM_ADDR groupOut;
    int64_t bs;
    int64_t h;
    int64_t topK;
    int64_t moeExpertNum;
    int64_t globalBs;
    int64_t expertTokenNumsType;
    int64_t dtype;
    int64_t expandXOutDtype;
    int64_t magic;
};
static_assert(std::is_standard_layout<EpServerDedupKernelArgs>::value, "launch arguments must be standard layout");
static_assert(sizeof(EpServerDedupKernelArgs) == 160, "launch ABI has twenty 64-bit fields");
static_assert(offsetof(EpServerDedupKernelArgs, bs) == 88, "shape follows eleven GM addresses");
static_assert(offsetof(EpServerDedupKernelArgs, magic) == 152, "magic is the final argument");

// 内部注册入口也供安装包测试调用；不需要创建 communicator。
int TileXREpRegisterDispatchMemoryServerDedupKernel();
int TileXREpSubmitDispatchMemoryServerDedupKernel(EpServerDedupKernelArgs &args,
    uint32_t blockDim, aclrtStream stream);

int TileXREpLaunchDispatchMemoryServerDedupKernel(const EpMemoryServerDedupParams &params,
    const EpHostLaunchContext &context, const EpMemoryServerDedupLayout &layout);

} // namespace TileXREp

#endif // TILEXR_EP_MEMORY_SERVER_DEDUP_LAUNCH_H
