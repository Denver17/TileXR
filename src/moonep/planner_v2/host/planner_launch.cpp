#include "planner_launch.h"

#include "moonep_kernel_launch.h"
#include "moonep_kernel_registration.h"
#include "tilexr_types.h"

namespace TileXREp {
namespace Plan {
namespace {

constexpr const char *kPlannerV2KernelName = "tilexr_ep_plan_kernel";
TileXRMoonEp::KernelRegistrationState g_plannerV2Registration;

} // namespace

int LaunchPlanKernel(
    TileXRCommPtr comm, const PlanHostArguments &arguments, const PlanHostContext &context)
{
    int64_t magic = 0;
    const int ret = TileXRCommNextMagic(comm, &magic);
    if (ret != TileXR::TILEXR_SUCCESS) {
        return ret;
    }

    const TileXRMoonEPPlanDesc &plan = *arguments.plan;
    const TileXRMoonEPPlanConfig &config = *arguments.config;
    struct PlanKernelArgs {
        GM_ADDR commArgs;
        GM_ADDR topkExperts;
        GM_ADDR tokensPerExpert;
        GM_ADDR globalRankIds;
        GM_ADDR dst;
        GM_ADDR cuSeqlens;
        GM_ADDR expertsToCopy;
        GM_ADDR remoteExperts;
        GM_ADDR expertTargets;
        GM_ADDR remoteStats;
        GM_ADDR status;
        GM_ADDR localWorkspace;
        GM_ADDR metaWorkspace;
        int64_t rank;
        int64_t rankSize;
        int64_t s;
        int64_t topK;
        int64_t expertNum;
        int64_t prefetchSlots;
        int64_t rankTokenCapacity;
        int64_t nvS;
        int64_t tokenPadding;
        int64_t tokenRouteLimitPerPair;
        int32_t cardsPerServer;
        int32_t cardsPerCabinet;
        int32_t crossCandidateCount;
        uint64_t epoch;
        uint64_t waitIterations;
        int64_t magic;
    } args {
        context.deviceCommArgs,
        reinterpret_cast<GM_ADDR>(const_cast<int32_t *>(arguments.topkExperts)),
        reinterpret_cast<GM_ADDR>(const_cast<int32_t *>(arguments.tokensPerExpert)),
        reinterpret_cast<GM_ADDR>(const_cast<int32_t *>(arguments.globalRankIds)),
        reinterpret_cast<GM_ADDR>(plan.dst),
        reinterpret_cast<GM_ADDR>(plan.cuSeqlens),
        reinterpret_cast<GM_ADDR>(plan.expertsToCopy),
        reinterpret_cast<GM_ADDR>(arguments.remoteExperts),
        reinterpret_cast<GM_ADDR>(arguments.expertTargets),
        reinterpret_cast<GM_ADDR>(plan.remoteStats),
        reinterpret_cast<GM_ADDR>(plan.status),
        static_cast<GM_ADDR>(arguments.localWorkspace),
        static_cast<GM_ADDR>(arguments.registeredMetaWorkspace),
        context.rank,
        context.callHeader.rankSize,
        arguments.s,
        arguments.topK,
        arguments.expertNum,
        config.prefetchSlots,
        config.rankTokenCapacity,
        config.nvS,
        config.tokenPadding,
        config.tokenRouteLimitPerPair,
        config.cardsPerServer,
        config.cardsPerCabinet,
        config.crossCandidateCount,
        plan.epoch,
        arguments.waitIterations,
        magic,
    };

    constexpr uint32_t kPlanBlockDim = 1;
    return TileXRMoonEp::LaunchRegisteredMoonEpKernel(g_plannerV2Registration,
        "tilexr_moonep_planner_v2_kernel.o",
        TileXRMoonEp::KernelSignature(TileXRMoonEp::kPlannerV2KernelSignature),
        kPlannerV2KernelName, "planner v2", kPlanBlockDim, &args, sizeof(args),
        static_cast<rtStream_t>(arguments.stream));
}

} // namespace Plan
} // namespace TileXREp
