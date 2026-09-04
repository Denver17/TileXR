#include "planner_launch.h"

#include <iostream>

#include "moonep_kernel_registration.h"
#include "planner_common.h"
#include "runtime/kernel.h"
#include "tilexr_types.h"

namespace TileXRMoonEpV3 {
namespace {

constexpr const char *kPlannerKernelName = "tilexr_moonep_planner_kernel";
TileXRMoonEp::KernelRegistrationState g_plannerRegistration;

} // namespace

int TileXRMoonEpLaunchKernel(const PlannerParams &params, const PlannerLaunchContext &context)
{
    const int registerRet = TileXRMoonEp::EnsureMoonEpKernelRegistered(g_plannerRegistration,
        "tilexr_moonep_planner_kernel.o",
        TileXRMoonEp::KernelSignature(TileXRMoonEp::kPlannerKernelSignature), kPlannerKernelName);
    if (registerRet != TileXR::TILEXR_SUCCESS) {
        return registerRet;
    }

    int64_t magic = 0;
    const int ret = TileXRCommNextMagic(params.comm, &magic);
    if (ret != TileXR::TILEXR_SUCCESS) {
        return ret;
    }
    const PlannerLayout &layout = context.layout;
    struct PlannerKernelArgs {
        GM_ADDR commArgs;
        GM_ADDR topkExpertIds;
        GM_ADDR tokensPerExpert;
        GM_ADDR workspace;
        GM_ADDR dst;
        GM_ADDR cuSeqlens;
        GM_ADDR expertsToCopy;
        GM_ADDR zeroFillRanges;
        GM_ADDR remoteStats;
        GM_ADDR dupCounts;
        GM_ADDR plannerStatus;
        int64_t s;
        int64_t k;
        int64_t expertCount;
        int64_t expertsPerRank;
        int64_t b;
        int64_t tokenPadding;
        int64_t routeCount;
        int64_t nvS;
        uint64_t waitIterations;
        uint64_t tpePrefixOffset;
        uint64_t blockHistogramOffset;
        uint64_t allocPrefixOffset;
        uint64_t expertOffsetsOffset;
        uint64_t zOffset;
        uint64_t groupTotalsOffset;
        uint64_t dstLocalOffset;
        uint64_t peerDstOffset;
        int64_t magic;
    } args {
        context.devArgs,
        reinterpret_cast<GM_ADDR>(const_cast<int32_t *>(params.topkExpertIds)),
        reinterpret_cast<GM_ADDR>(const_cast<int32_t *>(params.tokensPerExpert)),
        static_cast<GM_ADDR>(params.workspace), reinterpret_cast<GM_ADDR>(params.dst),
        reinterpret_cast<GM_ADDR>(params.cuSeqlens),
        reinterpret_cast<GM_ADDR>(params.expertsToCopy),
        reinterpret_cast<GM_ADDR>(params.zeroFillRanges),
        reinterpret_cast<GM_ADDR>(params.remoteStats),
        reinterpret_cast<GM_ADDR>(params.dupCounts),
        reinterpret_cast<GM_ADDR>(params.plannerStatus), layout.s, layout.k,
        layout.expertCount, layout.expertsPerRank, layout.b, layout.tokenPadding,
        layout.routeCount, layout.nvS, params.waitIterations, layout.tpePrefixOffset,
        layout.blockHistogramOffset, layout.allocPrefixOffset,
        layout.expertOffsetsOffset, layout.zOffset, layout.groupTotalsOffset,
        layout.dstLocalOffset, layout.peerDstOffset, magic
    };

    rtArgsEx_t argsInfo {};
    argsInfo.args = &args;
    argsInfo.argsSize = sizeof(args);
    rtTaskCfgInfo_t cfgInfo {};
    cfgInfo.schemMode = 1;

    const rtError_t launchRet = rtKernelLaunchWithFlagV2(
        TileXRMoonEp::KernelSignature(TileXRMoonEp::kPlannerKernelSignature),
        static_cast<uint32_t>(layout.blockDim),
        &argsInfo, nullptr, static_cast<rtStream_t>(params.stream), 0, &cfgInfo);
    if (launchRet != RT_ERROR_NONE) {
        std::cerr << "TileXR MoonEP planner rtKernelLaunchWithFlagV2 failed, ret="
                  << launchRet << std::endl;
        return TileXR::TILEXR_ERROR_MKIRT;
    }
    return TileXR::TILEXR_SUCCESS;
}

} // namespace TileXRMoonEpV3
