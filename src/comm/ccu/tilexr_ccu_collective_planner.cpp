/*
 * Copyright (c) 2026 TileXR Project
 */
#include "ccu/tilexr_ccu_collective_planner.h"

#ifdef TILEXR_CCU_TESTING
#include "ccu/tilexr_ccu_memory_program.h"
#endif
#include "ccu/tilexr_ccu_repository.h"
#include "ccu/tilexr_ccu_runtime_session.h"
#include "tilexr_log.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>

#ifdef TILEXR_CCU_TESTING
#include "runtime/dev.h"
#include "runtime/mem.h"
#include "runtime/rts/rts_device.h"
#endif

namespace TileXR {

#ifdef TILEXR_CCU_TESTING
constexpr uint32_t TILEXR_CCU_DIRECT_MEMORY_COPY_INSTRUCTION_COUNT = 7U;
#endif

uint8_t SelectDirectCcuInstallDieId()
{
    const char *text = std::getenv("TILEXR_CCU_DIRECT_INSTALL_DIE_ID");
    if (text == nullptr || text[0] == '\0') {
        return 0;
    }
    char *end = nullptr;
    const unsigned long parsed = std::strtoul(text, &end, 0);
    if (end == text || *end != '\0' || parsed > 1UL) {
        return 0;
    }
    return static_cast<uint8_t>(parsed);
}

uint32_t SelectDirectCcuPeerLocalXnOffset(size_t peerLocalIndex, uint32_t syncIndex, size_t peerRouteCount)
{
    if (peerRouteCount == 0) {
        return 0;
    }
    return static_cast<uint32_t>(peerLocalIndex) +
        static_cast<uint32_t>(syncIndex / peerRouteCount) * static_cast<uint32_t>(peerRouteCount);
}

uint32_t SelectDirectCcuChannelBoundRemoteXnOffset(size_t peerLocalIndex, uint32_t syncIndex, size_t peerRouteCount)
{
    return SelectDirectCcuPeerLocalXnOffset(peerLocalIndex, syncIndex, peerRouteCount);
}

uint16_t DirectCcuRemoteXnProofSpan(uint16_t syncRouteCount)
{
    if (syncRouteCount == 0) {
        return 0;
    }
    return syncRouteCount;
}

uint16_t SelectDirectCcuChannelBoundRemoteXnId(
    uint16_t remoteXnStartId,
    size_t peerLocalIndex,
    uint32_t syncIndex,
    size_t peerRouteCount)
{
    return static_cast<uint16_t>(
        static_cast<uint32_t>(remoteXnStartId) +
        SelectDirectCcuChannelBoundRemoteXnOffset(peerLocalIndex, syncIndex, peerRouteCount));
}

#ifdef TILEXR_CCU_TESTING
struct DirectCcuMemoryCopyEndpoint {
    uint64_t sourceAddr = 0;
    uint64_t sourceToken = 0;
    uint64_t destinationAddr = 0;
    uint64_t destinationToken = 0;
    uint64_t bytes = 0;
    uint32_t rank = 0;
    uint32_t valid = 0;
};

int QueryDirectCcuProcessMemoryToken(uint64_t addr, uint64_t bytes, uint64_t *packedToken)
{
    if (addr == 0 || bytes == 0 || packedToken == nullptr) {
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    *packedToken = 0;
    rtMemUbTokenInfo info {};
    info.va = addr;
    info.size = bytes;
    const rtError_t ret = rtUbDevQueryInfo(QUERY_PROCESS_TOKEN, &info);
    if (ret != RT_ERROR_NONE) {
        return TILEXR_ERROR_MKIRT;
    }
    constexpr uint32_t tokenIdRightShift = 8U;
    const uint32_t tokenId = info.tokenId >> tokenIdRightShift;
    *packedToken = TileXRCcuPackMemoryToken(tokenId, info.tokenValue, true);
    return *packedToken == 0 ? TILEXR_ERROR_NOT_FOUND : TILEXR_SUCCESS;
}

int BuildDirectCcuLocalMemoryCopyEndpoint(
    uint32_t rank,
    uint64_t sourceAddr,
    uint64_t destinationAddr,
    uint64_t bytes,
    DirectCcuMemoryCopyEndpoint *endpoint)
{
    if (endpoint == nullptr) {
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    *endpoint = DirectCcuMemoryCopyEndpoint {};
    endpoint->rank = rank;
    endpoint->bytes = bytes;
    endpoint->sourceAddr = sourceAddr;
    endpoint->destinationAddr = destinationAddr;
    int ret = QueryDirectCcuProcessMemoryToken(sourceAddr, bytes, &endpoint->sourceToken);
    if (ret != TILEXR_SUCCESS) {
        return ret;
    }
    ret = QueryDirectCcuProcessMemoryToken(destinationAddr, bytes, &endpoint->destinationToken);
    if (ret != TILEXR_SUCCESS) {
        return ret;
    }
    endpoint->valid = 1;
    return TILEXR_SUCCESS;
}
#endif

void TileXRCcuCollectivePlanner::Reset()
{
    ResetDirectCcuLowerLayerPlan();
    directCcuLowerLayerTemplateConfigured_ = false;
    directCcuLowerLayerTemplate_ = TileXRCcuLowerLayerTransportSnapshot {};
    directCcuVerifiedEndpointRoutes_.clear();
    directCcuLocalVerifiedEndpointRoute_ = TileXRCcuLowerLayerTransportRoute {};
    directCcuLocalVerifiedEndpointRouteValid_ = false;
}

bool TileXRCcuCollectivePlanner::Supports(
    const TileXRCcuRuntimeSession &session,
    const TileXRCcuCollectiveRequest &request) const
{
    (void)session;
    (void)request;
    return false;
}

int TileXRCcuCollectivePlanner::PrepareCollective(
    const TileXRCcuRuntimeSession &session,
    const TileXRCcuCollectiveRequest &request,
    TileXRCcuCollectivePlan *plan) const
{
    if (plan == nullptr) {
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    *plan = TileXRCcuCollectivePlan {};
    if (!session.Available()) {
        return TILEXR_ERROR_NOT_INITIALIZED;
    }
    (void)request;
    return TILEXR_ERROR_NOT_SUPPORT;
}

void TileXRCcuCollectivePlanner::ResetDirectCcuLowerLayerPlan()
{
    directCcuLowerLayerPlanValid_ = false;
    directCcuLowerLayerPlanStatus_ = TILEXR_ERROR_NOT_FOUND;
    directCcuLowerLayerSnapshot_ = TileXRCcuLowerLayerTransportSnapshot {};
    directCcuLowerLayerPlan_ = TileXRCcuLowerLayerInstallPlan {};
    directCcuLowerLayerPlanReport_ = TileXRCcuLowerLayerPlanBuilderReport {};
}

int TileXRCcuCollectivePlanner::ConfigureDirectCcuLowerLayerTemplate(
    TileXRCcuRuntimeSession &session,
    const TileXRCcuLowerLayerTransportSnapshot &templateSnapshot)
{
    directCcuLowerLayerTemplate_ = templateSnapshot;
    directCcuLowerLayerTemplateConfigured_ = true;
    return RefreshDirectCcuLowerLayerPlan(session);
}

int TileXRCcuCollectivePlanner::ConfigureDirectCcuVerifiedEndpointRoutes(
    TileXRCcuRuntimeSession &session,
    const std::vector<TileXRCcuLowerLayerTransportRoute> &verifiedRoutes)
{
    TileXRCcuLowerLayerTransportSnapshot validationSnapshot;
    validationSnapshot.routes = verifiedRoutes;
    TileXRCcuLowerLayerPlanBuilderReport report;
    int ret = TileXRCcuOverlayVerifiedEndpointRoutes(verifiedRoutes, &validationSnapshot, &report);
    directCcuLowerLayerPlanReport_ = report;
    if (ret != TILEXR_SUCCESS) {
        directCcuLowerLayerPlanStatus_ = ret;
        return ret;
    }

    directCcuVerifiedEndpointRoutes_ = verifiedRoutes;
    if (directCcuLowerLayerTemplateConfigured_) {
        return RefreshDirectCcuLowerLayerPlan(session);
    }
    return TILEXR_SUCCESS;
}

int TileXRCcuCollectivePlanner::ConfigureDirectCcuLocalVerifiedEndpointRoute(
    TileXRCcuRuntimeSession &session,
    const TileXRCcuLowerLayerTransportRoute &route)
{
    TileXRCcuLowerLayerTransportSnapshot validationSnapshot;
    validationSnapshot.routes.push_back(route);
    TileXRCcuLowerLayerPlanBuilderReport report;
    std::vector<TileXRCcuLowerLayerTransportRoute> routes {route};
    int ret = TileXRCcuOverlayVerifiedEndpointRoutes(routes, &validationSnapshot, &report);
    directCcuLowerLayerPlanReport_ = report;
    if (ret != TILEXR_SUCCESS) {
        directCcuLocalVerifiedEndpointRoute_ = TileXRCcuLowerLayerTransportRoute {};
        directCcuLocalVerifiedEndpointRouteValid_ = false;
        directCcuLowerLayerPlanStatus_ = ret;
        return ret;
    }

    directCcuLocalVerifiedEndpointRoute_ = route;
    directCcuLocalVerifiedEndpointRouteValid_ = true;
    if (session.Available()) {
        return session.ConfigureLocalVerifiedEndpointRoute(route);
    }
    return TILEXR_SUCCESS;
}

int TileXRCcuCollectivePlanner::ConfigureDirectCcuLowerLayerTemplateFromAllocation(
    TileXRCcuRuntimeSession &session,
    const TileXRCcuResourceAllocation &allocation,
    const std::vector<TileXRCcuRemoteCcuBufferInfo> &remoteCcuBuffers)
{
    ResetDirectCcuLowerLayerPlan();
    const TileXRCcuBasicInfo *basicInfo = session.GetDirectCcuBasicInfo();
    if (basicInfo == nullptr) {
        directCcuLowerLayerPlanReport_.message =
            "direct CCU basic info is unavailable for lower-layer transport template";
        directCcuLowerLayerPlanStatus_ = TILEXR_ERROR_NOT_FOUND;
        return directCcuLowerLayerPlanStatus_;
    }

    TileXRCcuLowerLayerTransportSnapshot templateSnapshot;
    TileXRCcuLowerLayerPlanBuilderReport report;
    int ret = TileXRCcuBuildLowerLayerTransportTemplate(
        *basicInfo,
        allocation,
        remoteCcuBuffers,
        &templateSnapshot,
        &report);
    directCcuLowerLayerPlanReport_ = report;
    if (ret != TILEXR_SUCCESS) {
        directCcuLowerLayerPlanStatus_ = ret;
        return directCcuLowerLayerPlanStatus_;
    }
    ret = TileXRCcuOverlayVerifiedEndpointRoutes(
        directCcuVerifiedEndpointRoutes_,
        &templateSnapshot,
        &directCcuLowerLayerPlanReport_);
    if (ret != TILEXR_SUCCESS) {
        directCcuLowerLayerPlanStatus_ = ret;
        return directCcuLowerLayerPlanStatus_;
    }

    directCcuLowerLayerTemplate_ = templateSnapshot;
    directCcuLowerLayerTemplateConfigured_ = true;
    return RefreshDirectCcuLowerLayerPlan(session);
}

int TileXRCcuCollectivePlanner::PrepareDirectCcuLowerLayerTemplateFromAllocation(
    TileXRCcuRuntimeSession &session,
    const TileXRCcuResourceAllocation &allocation)
{
    ResetDirectCcuLowerLayerPlan();
    const TileXRCcuBasicInfo *basicInfo = session.GetDirectCcuBasicInfo();
    if (basicInfo == nullptr) {
        directCcuLowerLayerPlanReport_.message =
            "direct CCU basic info is unavailable for lower-layer transport template";
        directCcuLowerLayerPlanStatus_ = TILEXR_ERROR_NOT_FOUND;
        return directCcuLowerLayerPlanStatus_;
    }
    if (!session.Available()) {
        directCcuLowerLayerPlanReport_.message =
            "direct CCU runtime is unavailable for resource window registration";
        directCcuLowerLayerPlanStatus_ = TILEXR_ERROR_NOT_FOUND;
        return directCcuLowerLayerPlanStatus_;
    }

    int ret = session.RegisterCcuResourceRmaBuffer(basicInfo->resourceAddr);
    if (ret != TILEXR_SUCCESS) {
        directCcuLowerLayerPlanReport_.message = "failed to register direct CCU resource window";
        directCcuLowerLayerPlanStatus_ = ret;
        return directCcuLowerLayerPlanStatus_;
    }

    TileXRCcuLocalResourceWindowInfo localCcuResourceWindow;
    ret = session.ExportLocalCcuRmaBuffer(&localCcuResourceWindow);
    if (ret != TILEXR_SUCCESS) {
        directCcuLowerLayerPlanReport_.message = "failed to export direct CCU local resource window token";
        directCcuLowerLayerPlanStatus_ = ret;
        return directCcuLowerLayerPlanStatus_;
    }
    if (directCcuLocalVerifiedEndpointRouteValid_) {
        ret = session.ConfigureLocalVerifiedEndpointRoute(directCcuLocalVerifiedEndpointRoute_);
        if (ret != TILEXR_SUCCESS) {
            directCcuLowerLayerPlanReport_.message = "failed to configure direct CCU local verified endpoint route";
            directCcuLowerLayerPlanStatus_ = ret;
            return directCcuLowerLayerPlanStatus_;
        }
    } else {
        TileXRCcuDirectRuntimeReport endpointRouteReport;
        ret = session.RefreshLocalVerifiedEndpointRoute(&endpointRouteReport);
        if (ret != TILEXR_SUCCESS && ret != TILEXR_ERROR_NOT_FOUND) {
            TILEXR_LOG(WARN) << "direct CCU local endpoint route collection failed closed: "
                             << ret << ", " << endpointRouteReport.message;
        }
    }

    std::vector<TileXRCcuRemoteCcuBufferInfo> remoteCcuBuffers;
    ret = session.ExportRemoteCcuRmaBuffers(&remoteCcuBuffers);
    if (ret != TILEXR_SUCCESS) {
        directCcuLowerLayerPlanReport_.message = "failed to export direct CCU peer resource window tokens";
        directCcuLowerLayerPlanStatus_ = ret;
        return directCcuLowerLayerPlanStatus_;
    }
    ret = ExchangeDirectCcuRemoteNotifyCke(session, allocation, &remoteCcuBuffers, &directCcuLowerLayerPlanReport_);
    if (ret != TILEXR_SUCCESS) {
        directCcuLowerLayerPlanStatus_ = ret;
        return directCcuLowerLayerPlanStatus_;
    }

    TileXRCcuLowerLayerTransportSnapshot templateSnapshot;
    TileXRCcuLowerLayerPlanBuilderReport report;
    ret = TileXRCcuBuildLowerLayerTransportTemplate(
        *basicInfo,
        allocation,
        remoteCcuBuffers,
        &templateSnapshot,
        &report);
    directCcuLowerLayerPlanReport_ = report;
    if (ret != TILEXR_SUCCESS) {
        directCcuLowerLayerPlanStatus_ = ret;
        return directCcuLowerLayerPlanStatus_;
    }
    templateSnapshot.msidToken.dieId = basicInfo->dieId;
    templateSnapshot.msidToken.msId = basicInfo->msId;
    templateSnapshot.msidToken.tokenId = localCcuResourceWindow.tokenId;
    templateSnapshot.msidToken.tokenValue = localCcuResourceWindow.tokenValue;
    templateSnapshot.msidToken.valid = true;
    ret = TileXRCcuOverlayVerifiedEndpointRoutes(
        directCcuVerifiedEndpointRoutes_,
        &templateSnapshot,
        &directCcuLowerLayerPlanReport_);
    if (ret != TILEXR_SUCCESS) {
        directCcuLowerLayerPlanStatus_ = ret;
        return directCcuLowerLayerPlanStatus_;
    }

    directCcuLowerLayerTemplate_ = templateSnapshot;
    directCcuLowerLayerTemplateConfigured_ = true;
    return RefreshDirectCcuLowerLayerPlan(session);
}

int TileXRCcuCollectivePlanner::FillDirectCcuLowerLayerPlanFromAllocation(
    TileXRCcuRuntimeSession &session,
    const TileXRCcuResourceAllocation &allocation,
    TileXRCcuLowerLayerInstallPlan *plan,
    TileXRCcuLowerLayerPlanBuilderReport *report)
{
    if (plan == nullptr || report == nullptr) {
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    int ret = PrepareDirectCcuLowerLayerTemplateFromAllocation(session, allocation);
    if (ret != TILEXR_SUCCESS) {
        *report = directCcuLowerLayerPlanReport_;
        return ret;
    }
    if (!directCcuLowerLayerPlanValid_) {
        *report = directCcuLowerLayerPlanReport_;
        return TILEXR_ERROR_NOT_FOUND;
    }
    *plan = directCcuLowerLayerPlan_;
    *report = directCcuLowerLayerPlanReport_;
    return TILEXR_SUCCESS;
}

int TileXRCcuCollectivePlanner::ExchangeDirectCcuRemoteNotifyCke(
    TileXRCcuRuntimeSession &session,
    const TileXRCcuResourceAllocation &allocation,
    std::vector<TileXRCcuRemoteCcuBufferInfo> *remoteCcuBuffers,
    TileXRCcuLowerLayerPlanBuilderReport *report)
{
    if (remoteCcuBuffers == nullptr) {
        if (report != nullptr) {
            report->message = "missing direct CCU remote notify CKE exchange inputs";
        }
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    const int rank = session.Rank();
    const int rankSize = session.RankSize();
    if (rankSize <= 1 || rank < 0 || rank >= rankSize) {
        if (report != nullptr) {
            report->message = "invalid direct CCU peer XN/CKE exchange shape";
        }
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    const size_t peerRouteCount = static_cast<size_t>(rankSize - 1);
    const size_t syncRouteCount = allocation.remoteXn.num;
    if (allocation.localXn.num == 0 ||
        allocation.localWaitCke.num == 0 ||
        allocation.remoteNotifyCke.num == 0 ||
        allocation.remoteXn.num < static_cast<uint16_t>(rankSize - 1) ||
        allocation.localWaitCke.num < allocation.remoteXn.num ||
        allocation.remoteNotifyCke.num < allocation.remoteXn.num ||
        allocation.channels.num == 0 ||
        remoteCcuBuffers->size() != peerRouteCount) {
        if (report != nullptr) {
            report->message = "invalid direct CCU peer XN/CKE exchange shape";
        }
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }

    struct PeerResourceExchange {
        uint16_t localXnStartId;
        uint16_t localXnCount;
        uint16_t remoteXnStartId;
        uint16_t remoteXnCount;
        uint16_t localWaitCkeStartId;
        uint16_t localWaitCkeCount;
        uint16_t remoteNotifyCkeStartId;
        uint16_t remoteNotifyCkeCount;
        uint16_t channelStartId;
        uint16_t channelCount;
    };
    PeerResourceExchange local {
        allocation.localXn.startId,
        allocation.localXn.num,
        allocation.remoteXn.startId,
        DirectCcuRemoteXnProofSpan(allocation.remoteXn.num),
        allocation.localWaitCke.startId,
        allocation.localWaitCke.num,
        allocation.remoteNotifyCke.startId,
        allocation.remoteNotifyCke.num,
        allocation.channels.startId,
        allocation.channels.num,
    };
    std::vector<PeerResourceExchange> all(rankSize);
    const int ret = session.AllGather(&local, sizeof(local), all.data());
    if (ret != TILEXR_SUCCESS) {
        if (report != nullptr) {
            report->message = "failed to exchange direct CCU peer XN/CKE resources";
        }
        return ret;
    }

    std::vector<int> peerRanks;
    peerRanks.reserve(peerRouteCount);
    for (int peer = 0; peer < rankSize; ++peer) {
        if (peer != rank) {
            peerRanks.push_back(peer);
        }
    }
    if (peerRanks.size() != peerRouteCount) {
        if (report != nullptr) {
            report->message = "invalid direct CCU peer XN/CKE exchange shape";
        }
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    std::vector<TileXRCcuRemoteCcuBufferInfo> peerCcuBuffers = *remoteCcuBuffers;
    remoteCcuBuffers->assign(syncRouteCount, TileXRCcuRemoteCcuBufferInfo{});

    size_t routeIndex = 0;
    for (uint32_t syncIndex = 0; syncIndex < allocation.remoteXn.num; ++syncIndex) {
        const size_t peerBufferIndex = syncIndex % peerRouteCount;
        const int peer = peerRanks[peerBufferIndex];
        const PeerResourceExchange &peerResources = all[peer];
        const size_t peerLocalIndex = static_cast<size_t>(rank < peer ? rank : rank - 1);
        const uint32_t peerLocalXnOffset =
            SelectDirectCcuPeerLocalXnOffset(peerLocalIndex, syncIndex, peerRouteCount);
        const uint32_t selectedRemoteXnOffset =
            SelectDirectCcuChannelBoundRemoteXnOffset(peerLocalIndex, syncIndex, peerRouteCount);
        const uint32_t peerLocalWaitCkeOffset = routeIndex;
        if (peerResources.localXnCount == 0 ||
            peerResources.remoteXnCount == 0 ||
            peerResources.localWaitCkeCount == 0 ||
            peerResources.channelCount == 0 ||
            peerLocalXnOffset >= peerResources.localXnCount ||
            selectedRemoteXnOffset >= peerResources.remoteXnCount ||
            peerLocalIndex >= peerResources.channelCount ||
            peerLocalWaitCkeOffset >= peerResources.localWaitCkeCount) {
            if (report != nullptr) {
                report->message = "peer direct CCU local XN/CKE resources are incomplete";
            }
            return TILEXR_ERROR_NOT_FOUND;
        }
        uint16_t channelBoundRemoteXnId = SelectDirectCcuChannelBoundRemoteXnId(
            peerResources.remoteXnStartId,
            peerLocalIndex,
            syncIndex,
            peerRouteCount);
        const uint16_t peerLocalXnId =
            static_cast<uint16_t>(static_cast<uint32_t>(peerResources.localXnStartId) + peerLocalXnOffset);
        uint16_t remoteNotifyCke =
            static_cast<uint16_t>(static_cast<uint32_t>(peerResources.localWaitCkeStartId) +
                peerLocalWaitCkeOffset);
        (*remoteCcuBuffers)[routeIndex] = peerCcuBuffers[peerBufferIndex];
        (*remoteCcuBuffers)[routeIndex].remoteXnId = channelBoundRemoteXnId;
        (*remoteCcuBuffers)[routeIndex].remoteNotifyCke = remoteNotifyCke;
        const bool peerLocalXnOwnerVerified =
            static_cast<uint32_t>(peerLocalXnId) >= peerResources.localXnStartId &&
            static_cast<uint32_t>(peerLocalXnId) <
                static_cast<uint32_t>(peerResources.localXnStartId) + peerResources.localXnCount;
        const bool notifyCkeOwnerVerified =
            static_cast<uint32_t>(remoteNotifyCke) >= peerResources.localWaitCkeStartId &&
            static_cast<uint32_t>(remoteNotifyCke) <
                static_cast<uint32_t>(peerResources.localWaitCkeStartId) + peerResources.localWaitCkeCount;
        const bool localChannelOwnerVerified =
            allocation.channels.num != 0 &&
            peerLocalXnOwnerVerified &&
            static_cast<uint32_t>(channelBoundRemoteXnId) >= peerResources.remoteXnStartId &&
            static_cast<uint32_t>(channelBoundRemoteXnId) <
                static_cast<uint32_t>(peerResources.remoteXnStartId) + peerResources.remoteXnCount &&
            routeIndex < allocation.channels.num &&
            peerResources.channelStartId != 0 &&
            peerLocalIndex < peerResources.channelCount;
        const bool transportResourceExchangeVerified =
            notifyCkeOwnerVerified &&
            allocation.localWaitCke.num != 0 &&
            routeIndex < allocation.localWaitCke.num &&
            peerLocalWaitCkeOffset < peerResources.localWaitCkeCount;
        (*remoteCcuBuffers)[routeIndex].channelResourceOwnerVerified = localChannelOwnerVerified;
        (*remoteCcuBuffers)[routeIndex].transportResourceExchangeVerified = transportResourceExchangeVerified;
        ++routeIndex;
    }
    return TILEXR_SUCCESS;
}

int TileXRCcuCollectivePlanner::PrepareDirectCcuLowerLayerPlanCallback(
    const TileXRCcuResourceAllocation &allocation,
    TileXRCcuLowerLayerInstallPlan *plan,
    TileXRCcuLowerLayerPlanBuilderReport *report,
    void *userData)
{
    auto *context = static_cast<LowerLayerPlanCallbackContext *>(userData);
    if (context == nullptr || context->planner == nullptr || context->session == nullptr) {
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    return context->planner->FillDirectCcuLowerLayerPlanFromAllocation(
        *context->session,
        allocation,
        plan,
        report);
}

int TileXRCcuCollectivePlanner::PrepareDirectCcuInstallAttempt(
    TileXRCcuRuntimeSession &session,
    const TileXRCcuDirectInstallOptions &options,
    TileXRCcuDirectInstallAttempt *attempt,
    TileXRCcuDirectInstallReport *report)
{
    if (!session.Available()) {
        if (report != nullptr) {
            *report = TileXRCcuDirectInstallReport{};
            report->message = "TileXRCcuBackend is not initialized for direct CCU install attempt";
        }
        return TILEXR_ERROR_NOT_INITIALIZED;
    }
    const std::string processUnavailableMessage =
        TileXRCcuRuntimeSession::ProcessDirectCcuRuntimeUnavailableMessage();
    if (!processUnavailableMessage.empty()) {
        if (report != nullptr) {
            *report = TileXRCcuDirectInstallReport{};
            report->message = processUnavailableMessage;
        }
        return TILEXR_ERROR_NOT_FOUND;
    }
    const uint8_t installDieId = SelectDirectCcuInstallDieId();
    const TileXRCcuBasicInfo *basicInfo = session.GetDirectCcuBasicInfo();
    if (basicInfo == nullptr || basicInfo->dieId != installDieId) {
        const int ret = session.RefreshDirectCcuBasicInfo(installDieId);
        if (ret != TILEXR_SUCCESS) {
            if (report != nullptr) {
                *report = TileXRCcuDirectInstallReport{};
                report->message = session.GetDirectCcuBasicInfoReport().message;
            }
            return ret;
        }
        basicInfo = session.GetDirectCcuBasicInfo();
    }
    if (basicInfo == nullptr || !session.Available()) {
        if (report != nullptr) {
            *report = TileXRCcuDirectInstallReport{};
            report->message = "direct CCU runtime is unavailable for install attempt";
        }
        return TILEXR_ERROR_NOT_FOUND;
    }

    TileXRCcuDriverAdapter adapter;
    TileXRCcuDriverAdapterReport adapterReport;
    int ret = session.CreateDriverAdapter(&adapter, &adapterReport);
    if (ret != TILEXR_SUCCESS) {
        if (report != nullptr) {
            *report = TileXRCcuDirectInstallReport{};
            report->message = adapterReport.message;
        }
        return ret;
    }

    LowerLayerPlanCallbackContext callbackContext {this, &session};
    TileXRCcuDirectInstallOptions next = options;
    next.basicInfo = basicInfo;
    next.offlineOnly = false;
    next.driverAdapter = &adapter;
    next.repositoryMemoryOps = TileXRCcuMakeRepositoryDeviceMemoryOps(next.repositoryMemoryAllocMode);
    next.repositoryMemoryUserData = nullptr;
    next.lowerLayerPlan = nullptr;
    next.prepareLowerLayerPlan = &TileXRCcuCollectivePlanner::PrepareDirectCcuLowerLayerPlanCallback;
    next.lowerLayerPlanUserData = &callbackContext;
    if (next.provider.empty()) {
        next.provider = "tilexr-comm-direct-ccu";
    }

    return TileXRCcuRunDirectInstallAttempt(next, attempt, report);
}

#ifdef TILEXR_CCU_TESTING
int TileXRCcuCollectivePlanner::PrepareDirectCcuMemoryCopyInstallAttempt(
    TileXRCcuRuntimeSession &session,
    const TileXRCcuDirectInstallOptions &options,
    uint64_t localSourceAddr,
    uint64_t localDestinationAddr,
    uint64_t bytes,
    uint32_t peerRank,
    TileXRCcuMemoryCopyDirection direction,
    TileXRCcuDirectInstallAttempt *attempt,
    TileXRCcuDirectInstallReport *report)
{
    if (!session.Available()) {
        if (report != nullptr) {
            *report = TileXRCcuDirectInstallReport {};
            report->message = "TileXRCcuBackend is not initialized for direct CCU memory copy install attempt";
        }
        return TILEXR_ERROR_NOT_INITIALIZED;
    }
    const int rank = session.Rank();
    const int rankSize = session.RankSize();
    if (localSourceAddr == 0 || localDestinationAddr == 0 || bytes == 0 ||
        peerRank >= static_cast<uint32_t>(rankSize) || peerRank == static_cast<uint32_t>(rank)) {
        if (report != nullptr) {
            *report = TileXRCcuDirectInstallReport {};
            report->message = "invalid direct CCU memory copy endpoint";
        }
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    const std::string processUnavailableMessage =
        TileXRCcuRuntimeSession::ProcessDirectCcuRuntimeUnavailableMessage();
    if (!processUnavailableMessage.empty()) {
        if (report != nullptr) {
            *report = TileXRCcuDirectInstallReport {};
            report->message = processUnavailableMessage;
        }
        return TILEXR_ERROR_NOT_FOUND;
    }
    const uint8_t installDieId = SelectDirectCcuInstallDieId();
    const TileXRCcuBasicInfo *basicInfo = session.GetDirectCcuBasicInfo();
    if (basicInfo == nullptr || basicInfo->dieId != installDieId) {
        const int ret = session.RefreshDirectCcuBasicInfo(installDieId);
        if (ret != TILEXR_SUCCESS) {
            if (report != nullptr) {
                *report = TileXRCcuDirectInstallReport {};
                report->message = session.GetDirectCcuBasicInfoReport().message;
            }
            return ret;
        }
        basicInfo = session.GetDirectCcuBasicInfo();
    }
    if (basicInfo == nullptr || !session.Available()) {
        if (report != nullptr) {
            *report = TileXRCcuDirectInstallReport {};
            report->message = "direct CCU runtime is unavailable for memory copy install attempt";
        }
        return TILEXR_ERROR_NOT_FOUND;
    }

    DirectCcuMemoryCopyEndpoint localEndpoint;
    int ret = BuildDirectCcuLocalMemoryCopyEndpoint(
        static_cast<uint32_t>(rank),
        localSourceAddr,
        localDestinationAddr,
        bytes,
        &localEndpoint);
    if (ret != TILEXR_SUCCESS) {
        if (report != nullptr) {
            *report = TileXRCcuDirectInstallReport {};
            report->message = "failed to query direct CCU memory copy local buffer token";
        }
        return ret;
    }

    std::vector<DirectCcuMemoryCopyEndpoint> allEndpoints(static_cast<size_t>(rankSize));
    ret = session.AllGather(
        &localEndpoint,
        sizeof(localEndpoint),
        allEndpoints.data());
    if (ret != TILEXR_SUCCESS) {
        if (report != nullptr) {
            *report = TileXRCcuDirectInstallReport {};
            report->message = "failed to exchange direct CCU memory copy peer endpoints";
        }
        return ret;
    }
    const DirectCcuMemoryCopyEndpoint &peerEndpoint = allEndpoints[peerRank];
    if (peerEndpoint.valid == 0 || peerEndpoint.bytes != bytes) {
        if (report != nullptr) {
            *report = TileXRCcuDirectInstallReport {};
            report->message = "invalid direct CCU memory copy peer endpoint";
        }
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }

    TileXRCcuDirectMemoryCopySpec memoryCopy;
    memoryCopy.direction = direction;
    memoryCopy.lengthBytes = bytes;
    if (direction == TileXRCcuMemoryCopyDirection::RemoteToLocal) {
        memoryCopy.localAddr = localEndpoint.destinationAddr;
        memoryCopy.localToken = localEndpoint.destinationToken;
        memoryCopy.remoteAddr = peerEndpoint.sourceAddr;
        memoryCopy.remoteToken = peerEndpoint.sourceToken;
    } else {
        memoryCopy.localAddr = localEndpoint.sourceAddr;
        memoryCopy.localToken = localEndpoint.sourceToken;
        memoryCopy.remoteAddr = peerEndpoint.destinationAddr;
        memoryCopy.remoteToken = peerEndpoint.destinationToken;
    }

    TileXRCcuDriverAdapter adapter;
    TileXRCcuDriverAdapterReport adapterReport;
    ret = session.CreateDriverAdapter(&adapter, &adapterReport);
    if (ret != TILEXR_SUCCESS) {
        if (report != nullptr) {
            *report = TileXRCcuDirectInstallReport {};
            report->message = adapterReport.message;
        }
        return ret;
    }

    LowerLayerPlanCallbackContext callbackContext {this, &session};
    TileXRCcuDirectInstallOptions next = options;
    next.basicInfo = basicInfo;
    next.offlineOnly = false;
    next.driverAdapter = &adapter;
    next.repositoryMemoryOps = TileXRCcuMakeRepositoryDeviceMemoryOps(next.repositoryMemoryAllocMode);
    next.repositoryMemoryUserData = nullptr;
    next.lowerLayerPlan = nullptr;
    next.prepareLowerLayerPlan = &TileXRCcuCollectivePlanner::PrepareDirectCcuLowerLayerPlanCallback;
    next.lowerLayerPlanUserData = &callbackContext;
    next.sqeArgCount = 0;
    next.syncResourceCount = 1;
    next.syncInstructionCount = std::max<uint32_t>(
        next.syncInstructionCount,
        TILEXR_CCU_DIRECT_MEMORY_COPY_INSTRUCTION_COUNT);
    next.bindingsPerSyncResource = next.bindingsPerSyncResource == 0 ? 1 : next.bindingsPerSyncResource;
    if (next.provider.empty()) {
        next.provider = "tilexr-comm-direct-ccu-memory-copy";
    }

    return TileXRCcuRunDirectMemoryCopyInstallAttempt(next, memoryCopy, attempt, report);
}
#endif

int TileXRCcuCollectivePlanner::RefreshDirectCcuLowerLayerPlan(TileXRCcuRuntimeSession &session)
{
    ResetDirectCcuLowerLayerPlan();
    if (!directCcuLowerLayerTemplateConfigured_) {
        directCcuLowerLayerPlanReport_.message = "direct CCU lower-layer template is not configured";
        directCcuLowerLayerPlanStatus_ = TILEXR_ERROR_NOT_FOUND;
        return directCcuLowerLayerPlanStatus_;
    }
    if (!session.Available()) {
        directCcuLowerLayerPlanReport_.message = "direct CCU runtime is unavailable for lower-layer planning";
        directCcuLowerLayerPlanStatus_ = TILEXR_ERROR_NOT_FOUND;
        return directCcuLowerLayerPlanStatus_;
    }

    TileXRCcuLowerLayerTransportSnapshot snapshot;
    int ret = session.ExportLowerLayerTransportSnapshot(directCcuLowerLayerTemplate_, &snapshot);
    if (ret != TILEXR_SUCCESS) {
        directCcuLowerLayerPlanReport_.message = "failed to export direct CCU lower-layer transport snapshot";
        directCcuLowerLayerPlanStatus_ = ret;
        return directCcuLowerLayerPlanStatus_;
    }
    ret = TileXRCcuOverlayVerifiedEndpointRoutes(
        directCcuVerifiedEndpointRoutes_,
        &snapshot,
        &directCcuLowerLayerPlanReport_);
    if (ret != TILEXR_SUCCESS) {
        directCcuLowerLayerPlanStatus_ = ret;
        return directCcuLowerLayerPlanStatus_;
    }

    TileXRCcuLowerLayerInstallPlan plan;
    TileXRCcuLowerLayerPlanBuilderReport report;
    ret = TileXRCcuBuildLowerLayerInstallPlanFromTransportSnapshot(snapshot, &plan, &report);
    directCcuLowerLayerPlanReport_ = report;
    if (ret != TILEXR_SUCCESS) {
        directCcuLowerLayerPlanStatus_ = ret;
        return directCcuLowerLayerPlanStatus_;
    }

    directCcuLowerLayerSnapshot_ = snapshot;
    directCcuLowerLayerPlan_ = plan;
    directCcuLowerLayerPlanReport_.message = "direct CCU lower-layer install plan cached";
    directCcuLowerLayerPlanValid_ = true;
    directCcuLowerLayerPlanStatus_ = TILEXR_SUCCESS;
    return TILEXR_SUCCESS;
}

bool TileXRCcuCollectivePlanner::HasDirectCcuLowerLayerPlan() const
{
    return directCcuLowerLayerPlanValid_;
}

int TileXRCcuCollectivePlanner::GetDirectCcuLowerLayerPlanStatus() const
{
    return directCcuLowerLayerPlanStatus_;
}

const TileXRCcuLowerLayerPlanBuilderReport &TileXRCcuCollectivePlanner::GetDirectCcuLowerLayerPlanReport() const
{
    return directCcuLowerLayerPlanReport_;
}

const TileXRCcuLowerLayerInstallPlan *TileXRCcuCollectivePlanner::GetDirectCcuLowerLayerPlan() const
{
    return directCcuLowerLayerPlanValid_ ? &directCcuLowerLayerPlan_ : nullptr;
}

} // namespace TileXR
