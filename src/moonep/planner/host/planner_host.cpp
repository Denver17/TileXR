#include "planner_host.h"

#include "comm_args.h"
#include "planner_common.h"
#include "tilexr_types.h"
#include "tilexr_udma_reg.h"

namespace TileXRMoonEpV3 {
namespace {

bool IsA5(const TileXR::CommArgs &commArgs)
{
    return (commArgs.extraFlag & TileXR::ExtraFlag::TOPO_910A5) != 0;
}

bool LocalityValid(const TileXR::CommArgs &commArgs)
{
    return commArgs.rankSize > 0 &&
        commArgs.rankSize <= TileXR::TILEXR_MAX_RANK_SIZE &&
        commArgs.rank >= 0 && commArgs.rank < commArgs.rankSize &&
        commArgs.localRankSize > 0 && commArgs.localRankSize <= commArgs.rankSize &&
        commArgs.rankSize % commArgs.localRankSize == 0 &&
        commArgs.localRank >= 0 && commArgs.localRank < commArgs.localRankSize;
}

bool PeerWindowsReady(const TileXR::CommArgs &commArgs)
{
    for (int32_t rank = 0; rank < commArgs.rankSize; ++rank) {
        if (commArgs.peerMems[rank] == nullptr) {
            return false;
        }
    }
    return true;
}

bool RegisteredMailboxReady(TileXRCommPtr comm, const TileXR::CommArgs &commArgs,
    const PlannerLayout &layout)
{
    if (commArgs.rankSize == 1) {
        return true;
    }
    if ((commArgs.extraFlag & TileXR::ExtraFlag::UDMA) == 0U ||
        commArgs.udmaInfoPtr == nullptr || commArgs.udmaRegistryPtr == nullptr) {
        return false;
    }
    const TileXR::TileXRUDMARegistry *registry = nullptr;
    if (TileXRGetUDMARegistryHost(comm, &registry) != TileXR::TILEXR_SUCCESS ||
        !TileXR::UDMARegistryValid(registry, commArgs.rankSize)) {
        return false;
    }
    for (int32_t rank = 0; rank < commArgs.rankSize; ++rank) {
        if (registry->regions[rank].base == nullptr ||
            registry->regions[rank].bytes < layout.peerWorkspaceBytes) {
            return false;
        }
    }
    return true;
}

} // namespace

int TileXRMoonEpPrepareLayout(TileXRCommPtr comm, int64_t s, int64_t k,
    int64_t expertCount, int64_t b, int64_t tokenPadding, PlannerLayout *layout)
{
    if (comm == nullptr || layout == nullptr) {
        return TileXR::TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    TileXR::CommArgs *commArgs = nullptr;
    int ret = TileXRGetCommArgsHost(comm, commArgs);
    if (ret != TileXR::TILEXR_SUCCESS) {
        return ret;
    }
    if (commArgs == nullptr || commArgs->rankSize <= 0) {
        return TileXR::TILEXR_ERROR_NOT_INITIALIZED;
    }
    if (!IsA5(*commArgs)) {
        return TileXR::TILEXR_ERROR_NOT_SUPPORT;
    }
    if (!LocalityValid(*commArgs)) {
        return TileXR::TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    if (!PeerWindowsReady(*commArgs)) {
        return TileXR::TILEXR_ERROR_NOT_INITIALIZED;
    }
    return TileXRMoonEpBuildPlannerLayout(
        commArgs->rankSize, s, k, expertCount, b, tokenPadding, layout);
}

int TileXRMoonEpValidateParams(const PlannerParams &params, const TileXR::CommArgs &commArgs,
    const PlannerLayout &layout)
{
    if (params.topkExpertIds == nullptr || params.tokensPerExpert == nullptr ||
        params.comm == nullptr || params.workspace == nullptr || params.dst == nullptr ||
        params.cuSeqlens == nullptr || params.expertsToCopy == nullptr ||
        params.zeroFillRanges == nullptr || params.remoteStats == nullptr ||
        params.dupCounts == nullptr || params.plannerStatus == nullptr ||
        params.waitIterations == 0 || params.stream == nullptr) {
        return TileXR::TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    if (!IsA5(commArgs)) {
        return TileXR::TILEXR_ERROR_NOT_SUPPORT;
    }
    if (!LocalityValid(commArgs)) {
        return TileXR::TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    if (!PeerWindowsReady(commArgs)) {
        return TileXR::TILEXR_ERROR_NOT_INITIALIZED;
    }
    if (!RegisteredMailboxReady(params.comm, commArgs, layout)) {
        return TileXR::TILEXR_ERROR_NOT_INITIALIZED;
    }
    if ((reinterpret_cast<uintptr_t>(params.workspace) % kPlannerWorkspaceAlignment) != 0 ||
        params.workspaceBytes < layout.workspaceBytes) {
        return TileXR::TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    return TileXR::TILEXR_SUCCESS;
}

int TileXRMoonEpPrepareLaunchContext(const PlannerParams &params, PlannerLaunchContext *context)
{
    if (context == nullptr) {
        return TileXR::TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    *context = PlannerLaunchContext {};

    int ret = TileXRGetCommArgsHost(params.comm, context->hostArgs);
    if (ret != TileXR::TILEXR_SUCCESS) {
        return ret;
    }
    if (context->hostArgs == nullptr) {
        *context = PlannerLaunchContext {};
        return TileXR::TILEXR_ERROR_NOT_INITIALIZED;
    }

    ret = TileXRMoonEpBuildPlannerLayout(context->hostArgs->rankSize, params.s, params.k,
        params.expertCount, params.b, params.tokenPadding, &context->layout);
    if (ret != TileXR::TILEXR_SUCCESS) {
        *context = PlannerLaunchContext {};
        return ret;
    }
    ret = TileXRMoonEpValidateParams(params, *context->hostArgs, context->layout);
    if (ret != TileXR::TILEXR_SUCCESS) {
        *context = PlannerLaunchContext {};
        return ret;
    }

    ret = TileXRGetCommArgsDev(params.comm, context->devArgs);
    if (ret != TileXR::TILEXR_SUCCESS || context->devArgs == nullptr) {
        *context = PlannerLaunchContext {};
        return ret == TileXR::TILEXR_SUCCESS ? TileXR::TILEXR_ERROR_NOT_INITIALIZED : ret;
    }
    return TileXR::TILEXR_SUCCESS;
}

} // namespace TileXRMoonEpV3
