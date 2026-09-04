#include "dispatch_launch.h"

#include "moonep_kernel_launch.h"

namespace TileXRMoonEp {
namespace {

constexpr const char *kDispatchKernelName = "tilexr_moonep_dispatch_kernel";
KernelRegistrationState g_dispatchRegistration;

} // namespace

int TileXRMoonEpLaunchDispatchKernel(
    const DispatchParams &params, const DispatchLaunchContext &context)
{
    struct DispatchKernelArgs {
        GM_ADDR commArgs;
        GM_ADDR dst;
        GM_ADDR zeroFillRanges;
        GM_ADDR dupGroups;
        GM_ADDR dupLoffs;
        GM_ADDR dupCounts;
        GM_ADDR hiddenSh;
        GM_ADDR routeWeightsSk;
        GM_ADDR hiddenNvsh;
        GM_ADDR routeWeightsNvs;
        GM_ADDR status;
        int64_t s;
        int64_t k;
        int64_t n;
        int64_t nvS;
        int64_t hiddenRowBytes;
        int64_t hiddenChunkBytes;
        int64_t hiddenChunkStride;
        int64_t chunkCount;
        uint64_t hiddenPayloadBytes;
        uint64_t routeWeightsOffset;
        uint64_t routeWeightsBytes;
        uint64_t dedupParentsOffset;
        uint64_t dedupParentsBytes;
        uint64_t dedupGroupMapOffset;
        uint64_t dedupGroupMapBytes;
        uint64_t waitIterations;
        uint64_t flags;
        int64_t magic;
    } args {
        static_cast<GM_ADDR>(context.devArgs),
        reinterpret_cast<GM_ADDR>(const_cast<int32_t *>(params.dst)),
        reinterpret_cast<GM_ADDR>(const_cast<int32_t *>(params.zeroFillRanges)),
        reinterpret_cast<GM_ADDR>(params.dupGroups),
        reinterpret_cast<GM_ADDR>(params.dupLoffs),
        reinterpret_cast<GM_ADDR>(params.dupCounts),
        reinterpret_cast<GM_ADDR>(const_cast<void *>(params.hiddenSh)),
        reinterpret_cast<GM_ADDR>(const_cast<float *>(params.routeWeightsSk)),
        reinterpret_cast<GM_ADDR>(params.hiddenNvsh),
        reinterpret_cast<GM_ADDR>(params.routeWeightsNvs),
        reinterpret_cast<GM_ADDR>(params.status), context.layout.s, context.layout.k,
        context.layout.n, context.layout.nvS,
        static_cast<int64_t>(context.layout.hiddenRowBytes),
        static_cast<int64_t>(context.layout.hiddenChunkBytes),
        static_cast<int64_t>(context.layout.hiddenChunkStride), context.layout.chunkCount,
        context.layout.hiddenPayloadBytes, context.layout.routeWeightsOffset,
        context.layout.routeWeightsBytes, context.layout.dedupParentsOffset,
        context.layout.dedupParentsBytes, context.layout.dedupGroupMapOffset,
        context.layout.dedupGroupMapBytes, context.waitIterations, params.flags,
        context.magic
    };

    return LaunchRegisteredMoonEpKernel(g_dispatchRegistration,
        "tilexr_moonep_dispatch_kernel.o",
        KernelSignature(kDispatchKernelSignature),
        kDispatchKernelName, "dispatch", static_cast<uint32_t>(context.layout.blockDim),
        &args, sizeof(args), static_cast<rtStream_t>(params.stream));
}

} // namespace TileXRMoonEp
