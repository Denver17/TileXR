/*
 * 将已校验的 MoonEP Host 参数打包为外置 kernel ABI，通过注册后的 runtime 入口提交。
 */
#ifndef TILEXR_MOONEP_DISPATCH_URMA_LAUNCH_H
#define TILEXR_MOONEP_DISPATCH_URMA_LAUNCH_H

#include "acl/acl_base.h"
#include "../common/dispatch_common.h"
#include "dispatch_layout.h"
#include "tilexr_api.h"

namespace TileXRMoonEp {

struct DispatchUrmaLaunchParams {
    GM_ADDR commArgs = nullptr;
    const void *hiddenInput = nullptr;
    const void *weightInput = nullptr;
    const int32_t *dst = nullptr;
    const int32_t *zeroFillRanges = nullptr;
    void *workspace = nullptr;
    void *hiddenOutput = nullptr;
    void *weightOutput = nullptr;
    int32_t *planStatus = nullptr;
    TileXRCommPtr comm = nullptr;
    aclrtStream stream = nullptr;
    DispatchPeerMode peerMode = DispatchPeerMode::GroupCredit;
    uint32_t groupWidth = kDispatchDefaultGroupWidth;
    int64_t zeroFillRangeCount = 0;
    MoonEpDispatchUrmaLayout layout {};
};

struct DispatchKernelArgs {
    GM_ADDR commArgs;
    GM_ADDR hiddenInput;
    GM_ADDR weightInput;
    GM_ADDR dst;
    GM_ADDR zeroFillRanges;
    GM_ADDR workspace;
    GM_ADDR hiddenOutput;
    GM_ADDR weightOutput;
    GM_ADDR planStatus;
    uint64_t hiddenSourceOffset;
    uint64_t hiddenScratchOffset;
    uint64_t hiddenRowBytes;
    uint64_t weightSourceOffset;
    uint64_t weightScratchOffset;
    uint64_t weightRowBytes;
    uint64_t completionFlagsOffset;
    uint64_t creditOffset;
    uint64_t creditSourceOffset;
    uint64_t signalOffset;
    uint64_t hiddenDfxOffset;
    uint64_t weightDfxOffset;
    uint64_t kernelStatusOffset;
    int64_t s;
    int64_t k;
    int64_t h;
    int64_t routeCount;
    int64_t destinationCapacity;
    int64_t zeroFillRangeCount;
    uint64_t hasWeight;
    int64_t magic;
    uint64_t completionTimeoutTicks;
    uint64_t peerMode;
    uint64_t groupWidth;
};

static_assert(sizeof(DispatchKernelArgs) == 33U * sizeof(uint64_t),
    "MoonEP Dispatch Host/Kernel ABI changed");

int TileXRMoonEpLaunchDispatchUrmaKernel(const DispatchUrmaLaunchParams &params);

uint64_t TileXRMoonEpDispatchCompletionTimeoutTicks();

} // namespace TileXRMoonEp

#endif // TILEXR_MOONEP_DISPATCH_URMA_LAUNCH_H
