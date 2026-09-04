#include "prefetch_weight_launch.h"

#include "moonep_kernel_launch.h"

namespace TileXRMoonEp {
namespace {

constexpr const char *kPrefetchWeightKernelName = "tilexr_moonep_prefetch_weight_kernel";
KernelRegistrationState g_prefetchWeightRegistration;

} // namespace

int TileXRMoonEpLaunchPrefetchWeightKernel(
    const PrefetchWeightParams &params, const PrefetchWeightLaunchContext &context)
{
    struct PrefetchWeightKernelArgs {
        GM_ADDR commArgs;
        GM_ADDR expertsToCopy;
        GM_ADDR gate;
        GM_ADDR up;
        GM_ADDR down;
        GM_ADDR status;
        uint64_t gateOffset;
        uint64_t upOffset;
        uint64_t downOffset;
        uint64_t gateRowBytes;
        uint64_t upRowBytes;
        uint64_t downRowBytes;
        int64_t rank;
        int64_t rankSize;
        int64_t expertsPerRank;
        int64_t prefetchSlots;
        uint64_t qpNum;
        uint64_t physicalQpMap;
    } args {
        context.devArgs,
        reinterpret_cast<GM_ADDR>(const_cast<int32_t *>(params.expertsToCopy)),
        reinterpret_cast<GM_ADDR>(params.gate),
        reinterpret_cast<GM_ADDR>(params.up),
        reinterpret_cast<GM_ADDR>(params.down),
        reinterpret_cast<GM_ADDR>(params.status), context.layout.gate.registryOffset,
        context.layout.up.registryOffset, context.layout.down.registryOffset,
        context.layout.gate.rowBytes, context.layout.up.rowBytes,
        context.layout.down.rowBytes, context.layout.rank,
        context.layout.rankSize, context.layout.expertsPerRank,
        context.layout.prefetchSlots,
        context.layout.qpNum, context.layout.physicalQpMap
    };

    static_assert(sizeof(PrefetchWeightKernelArgs) == 18U * sizeof(uint64_t),
        "PrefetchWeight kernel argument ABI changed");

    return LaunchRegisteredMoonEpKernel(g_prefetchWeightRegistration,
        "tilexr_moonep_prefetch_weight_kernel.o",
        KernelSignature(kPrefetchWeightKernelSignature),
        kPrefetchWeightKernelName, "prefetch-weight",
        static_cast<uint32_t>(context.layout.blockDim), &args, sizeof(args),
        static_cast<rtStream_t>(params.stream));
}

} // namespace TileXRMoonEp
