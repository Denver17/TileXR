#include "combine_v2_launch.h"

#include <cstddef>
#include <cstdint>

#include "acl/acl_rt.h"
#include "moonep_kernel_launch.h"

namespace TileXRMoonEp {
namespace {

constexpr const char *kCombineV2KernelName =
    "tilexr_moonep_combine_v2_kernel";
constexpr uint32_t kCombineV2SimtDCacheBytes = 32U * 1024U;
constexpr uint32_t kCombineV2MinDynamicUbBytes = 216U * 1024U;
KernelRegistrationState g_combineV2Registration;

struct CombineV2KernelArgs {
    GM_ADDR commArgs;
    GM_ADDR registeredWorkspace;
    GM_ADDR dstLocal;
    uint64_t profileOffset;
    uint64_t scratchEpoch0Offset;
    uint64_t scratchEpoch1Offset;
    uint64_t doneOffset;
    uint64_t reservedOffset0;
    uint64_t controlSourceOffset;
    uint64_t failureOffset;
    uint64_t reservedSyncReceiveOffset;
    uint64_t reservedSyncSourceOffset;
    uint64_t collectiveStatusOffset;
    uint64_t outputOffset;
    int64_t bs;
    int64_t h;
    int64_t topK;
    int64_t nvS;
    uint64_t rowBytes;
    uint64_t reduceHidden;
    int64_t magic;
    GM_ADDR weightMemoryCommArgs;
    GM_ADDR routeWeightsNvs;
    GM_ADDR routeWeightsSk;
    uint64_t weightRecordOffset;
    uint64_t weightDoneOffset;
    uint64_t weightWindowBytes;
    uint64_t weightOutputElements;
    uint64_t hasRouteWeight;
};

static_assert(sizeof(CombineV2KernelArgs) == 29U * sizeof(uint64_t),
    "Combine V2 kernel argument ABI changed");

int ConfigureCombineV2SimtMemory(rtTaskCfgInfo_t &cfgInfo)
{
    int32_t deviceId = 0;
    if (aclrtGetDevice(&deviceId) != ACL_SUCCESS) {
        return TileXR::TILEXR_ERROR_INTERNAL;
    }

    int64_t dynamicUbBytes = 0;
    if (aclrtGetDeviceInfo(static_cast<uint32_t>(deviceId),
            ACL_DEV_ATTR_UBUF_PER_VECTOR_CORE, &dynamicUbBytes) != ACL_SUCCESS ||
        dynamicUbBytes < static_cast<int64_t>(kCombineV2MinDynamicUbBytes) ||
        dynamicUbBytes > static_cast<int64_t>(
            UINT32_MAX - kCombineV2SimtDCacheBytes)) {
        return TileXR::TILEXR_ERROR_PARA_CHECK_FAIL;
    }

    cfgInfo.localMemorySize = static_cast<uint32_t>(dynamicUbBytes);
    return TileXR::TILEXR_SUCCESS;
}

} // namespace

int TileXRMoonEpLaunchCombineV2Kernel(
    const CombineV2Params &params,
    const CombineV2LaunchContext &context)
{
    CombineV2KernelArgs args {
        context.devArgs,
        static_cast<GM_ADDR>(params.registeredWorkspace),
        reinterpret_cast<GM_ADDR>(const_cast<int32_t *>(params.dstLocal)),
        context.layout.profileOffset,
        context.layout.scratchOffset[0],
        context.layout.scratchOffset[1],
        context.layout.doneOffset,
        0U,
        context.layout.controlSourceOffset,
        context.layout.failureOffset,
        0U,
        0U,
        context.layout.collectiveStatusOffset,
        context.layout.outputOffset,
        params.bs,
        params.h,
        params.topK,
        params.nvS,
        context.layout.rowBytes,
        params.reduceHidden ? 1U : 0U,
        context.magic,
        context.weightMemoryDevArgs,
        reinterpret_cast<GM_ADDR>(const_cast<float *>(
            params.routeWeightsNvs)),
        reinterpret_cast<GM_ADDR>(params.routeWeightsSk),
        context.weightLayout.recordOffset,
        context.weightLayout.doneOffset,
        context.weightLayout.totalBytes,
        context.weightOutputElements,
        params.routeWeightsNvs != nullptr ? 1U : 0U
    };
    rtTaskCfgInfo_t cfgInfo {};
    const int memoryRet = ConfigureCombineV2SimtMemory(cfgInfo);
    if (memoryRet != TileXR::TILEXR_SUCCESS) {
        return memoryRet;
    }
    const int ret = LaunchRegisteredMoonEpKernel(g_combineV2Registration,
        "tilexr_moonep_combine_v2_kernel.o",
        KernelSignature(kCombineV2KernelSignature),
        kCombineV2KernelName, "combine_v2",
        kMoonEpCombineV2CoreCount, &args, sizeof(args),
        static_cast<rtStream_t>(params.stream), &cfgInfo);
    if (ret == TILEXR_MOONEP_SUCCESS) {
        *params.activeOutputOffset = context.layout.scratchOffset[
            MoonEpCombineV2Epoch(static_cast<uint64_t>(context.magic))];
    }
    return ret;
}

} // namespace TileXRMoonEp
