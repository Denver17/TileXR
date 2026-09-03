/*
 * Copyright (c) 2026 TileXR Project
 */
#ifndef TILEXR_CCU_COLLECTIVE_PLANNER_H
#define TILEXR_CCU_COLLECTIVE_PLANNER_H

#include <cstdint>
#include <vector>

#include "ccu/tilexr_ccu_backend.h"
#include "ccu/tilexr_ccu_direct_orchestrator.h"
#ifdef TILEXR_CCU_TESTING
#include "ccu/tilexr_ccu_memory_program.h"
#endif

namespace TileXR {

class TileXRCcuRuntimeSession;

class TileXRCcuCollectivePlanner {
public:
    void Reset();
    bool Supports(const TileXRCcuRuntimeSession &session, const TileXRCcuCollectiveRequest &request) const;
    int PrepareCollective(
        const TileXRCcuRuntimeSession &session,
        const TileXRCcuCollectiveRequest &request,
        TileXRCcuCollectivePlan *plan) const;

    int ConfigureDirectCcuLowerLayerTemplate(
        TileXRCcuRuntimeSession &session,
        const TileXRCcuLowerLayerTransportSnapshot &templateSnapshot);
    int ConfigureDirectCcuVerifiedEndpointRoutes(
        TileXRCcuRuntimeSession &session,
        const std::vector<TileXRCcuLowerLayerTransportRoute> &verifiedRoutes);
    int ConfigureDirectCcuLocalVerifiedEndpointRoute(
        TileXRCcuRuntimeSession &session,
        const TileXRCcuLowerLayerTransportRoute &route);
    int ConfigureDirectCcuLowerLayerTemplateFromAllocation(
        TileXRCcuRuntimeSession &session,
        const TileXRCcuResourceAllocation &allocation,
        const std::vector<TileXRCcuRemoteCcuBufferInfo> &remoteCcuBuffers);
    int PrepareDirectCcuLowerLayerTemplateFromAllocation(
        TileXRCcuRuntimeSession &session,
        const TileXRCcuResourceAllocation &allocation);
    int PrepareDirectCcuInstallAttempt(
        TileXRCcuRuntimeSession &session,
        const TileXRCcuDirectInstallOptions &options,
        TileXRCcuDirectInstallAttempt *attempt,
        TileXRCcuDirectInstallReport *report);
#ifdef TILEXR_CCU_TESTING
    int PrepareDirectCcuMemoryCopyInstallAttempt(
        TileXRCcuRuntimeSession &session,
        const TileXRCcuDirectInstallOptions &options,
        uint64_t localSourceAddr,
        uint64_t localDestinationAddr,
        uint64_t bytes,
        uint32_t peerRank,
        TileXRCcuMemoryCopyDirection direction,
        TileXRCcuDirectInstallAttempt *attempt,
        TileXRCcuDirectInstallReport *report);
#endif
    int RefreshDirectCcuLowerLayerPlan(TileXRCcuRuntimeSession &session);
    bool HasDirectCcuLowerLayerPlan() const;
    int GetDirectCcuLowerLayerPlanStatus() const;
    const TileXRCcuLowerLayerPlanBuilderReport &GetDirectCcuLowerLayerPlanReport() const;
    const TileXRCcuLowerLayerInstallPlan *GetDirectCcuLowerLayerPlan() const;

private:
    struct LowerLayerPlanCallbackContext {
        TileXRCcuCollectivePlanner *planner = nullptr;
        TileXRCcuRuntimeSession *session = nullptr;
    };

    void ResetDirectCcuLowerLayerPlan();
    int FillDirectCcuLowerLayerPlanFromAllocation(
        TileXRCcuRuntimeSession &session,
        const TileXRCcuResourceAllocation &allocation,
        TileXRCcuLowerLayerInstallPlan *plan,
        TileXRCcuLowerLayerPlanBuilderReport *report);
    int ExchangeDirectCcuRemoteNotifyCke(
        TileXRCcuRuntimeSession &session,
        const TileXRCcuResourceAllocation &allocation,
        std::vector<TileXRCcuRemoteCcuBufferInfo> *remoteCcuBuffers,
        TileXRCcuLowerLayerPlanBuilderReport *report);
    static int PrepareDirectCcuLowerLayerPlanCallback(
        const TileXRCcuResourceAllocation &allocation,
        TileXRCcuLowerLayerInstallPlan *plan,
        TileXRCcuLowerLayerPlanBuilderReport *report,
        void *userData);

    bool directCcuLowerLayerTemplateConfigured_ = false;
    bool directCcuLowerLayerPlanValid_ = false;
    int directCcuLowerLayerPlanStatus_ = TILEXR_ERROR_NOT_FOUND;
    TileXRCcuLowerLayerTransportSnapshot directCcuLowerLayerTemplate_ = {};
    TileXRCcuLowerLayerTransportSnapshot directCcuLowerLayerSnapshot_ = {};
    TileXRCcuLowerLayerInstallPlan directCcuLowerLayerPlan_ = {};
    TileXRCcuLowerLayerPlanBuilderReport directCcuLowerLayerPlanReport_ = {};
    std::vector<TileXRCcuLowerLayerTransportRoute> directCcuVerifiedEndpointRoutes_ = {};
    TileXRCcuLowerLayerTransportRoute directCcuLocalVerifiedEndpointRoute_ = {};
    bool directCcuLocalVerifiedEndpointRouteValid_ = false;
};

} // namespace TileXR

#endif // TILEXR_CCU_COLLECTIVE_PLANNER_H
