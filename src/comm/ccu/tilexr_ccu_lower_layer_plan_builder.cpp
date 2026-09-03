/*
 * Copyright (c) 2026 TileXR Project
 * Licensed under the Apache License, Version 2.0
 */

#include "ccu/tilexr_ccu_lower_layer_plan_builder.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <map>

namespace TileXR {
namespace {

constexpr uint16_t TILEXR_CCU_DEFAULT_START_JETTY_ID = 1024;
constexpr uint16_t TILEXR_CCU_DEFAULT_START_LOCAL_JETTY_CTX_ID = 0;
constexpr uint16_t TILEXR_CCU_WQE_BASIC_BLOCKS_PER_ROUTE = 4;
constexpr uint16_t TILEXR_CCU_HCOMM_WQE_BASIC_BLOCKS_PER_ROUTE = 256;
constexpr uint32_t TILEXR_CCU_HCOMM_PER_DIE_PFE_RESERVED_NUM = 16;
constexpr uint16_t TILEXR_CCU_HCOMM_INNER_FE_JETTY_NUM = 23;
constexpr uint16_t TILEXR_CCU_HCOMM_PER_DIE_JETTY_NUM = 128;
constexpr uint16_t TILEXR_CCU_HCOMM_OUTER_FE_START_JETTY_CTX_ID = 92;
constexpr uint16_t TILEXR_CCU_HCOMM_OUTER_FE_JETTY_NUM = 36;
constexpr uint32_t TILEXR_CCU_HCOMM_MAX_INNER_FE_ID = 7;

void ResetReport(TileXRCcuLowerLayerPlanBuilderReport* report)
{
    if (report != nullptr) {
        *report = TileXRCcuLowerLayerPlanBuilderReport{};
    }
}

int Fail(
    TileXRCcuLowerLayerInstallPlan* plan,
    TileXRCcuLowerLayerPlanBuilderReport* report,
    const std::string& message)
{
    if (plan != nullptr) {
        *plan = TileXRCcuLowerLayerInstallPlan{};
    }
    if (report != nullptr) {
        report->message = message;
    }
    return TILEXR_ERROR_PARA_CHECK_FAIL;
}

int FailPayload(
    TileXRCcuLowerLayerInstallPlan* plan,
    TileXRCcuLowerLayerPlanBuilderReport* report,
    const TileXRCcuLowerLayerPayloadReport& payloadReport)
{
    return Fail(plan, report, payloadReport.message.empty() ? "invalid lower-layer CCU payload spec" :
        payloadReport.message);
}

uint16_t CheckedU16(uint32_t value)
{
    return static_cast<uint16_t>(std::min<uint32_t>(value, std::numeric_limits<uint16_t>::max()));
}

bool AddOverflowsU16(uint16_t start, uint32_t count)
{
    return static_cast<uint32_t>(start) + count >
        static_cast<uint32_t>(std::numeric_limits<uint16_t>::max()) + 1U;
}

TileXRCcuRange EffectiveLocalWaitCkeRange(const TileXRCcuResourceAllocation& allocation)
{
    TileXRCcuRange local = allocation.localWaitCke.num == 0 ? allocation.notifyCke : allocation.localWaitCke;
    if (allocation.sourceCke.num == 0) {
        return local;
    }
    if (local.dieId == allocation.sourceCke.dieId &&
        static_cast<uint32_t>(local.startId) + local.num == allocation.sourceCke.startId) {
        local.num = CheckedU16(static_cast<uint32_t>(local.num) + allocation.sourceCke.num);
    }
    return local;
}

TileXRCcuRange EffectiveRemoteNotifyCkeRange(const TileXRCcuResourceAllocation& allocation)
{
    return allocation.remoteNotifyCke.num == 0 ? allocation.notifyCke : allocation.remoteNotifyCke;
}

uint16_t SelectLowerLayerWqeBasicBlockStride()
{
    const char* value = std::getenv("TILEXR_CCU_DIRECT_LOWER_LAYER_WQE_MODE");
    if (value != nullptr && std::strcmp(value, "hcomm_cap") == 0) {
        return TILEXR_CCU_HCOMM_WQE_BASIC_BLOCKS_PER_ROUTE;
    }
    return TILEXR_CCU_WQE_BASIC_BLOCKS_PER_ROUTE;
}

bool LowerLayerEnvEquals(const char* name, const char* expected)
{
    const char* value = std::getenv(name);
    return value != nullptr && std::strcmp(value, expected) == 0;
}

bool IsEmptyEndpointEid(const std::array<uint8_t, TILEXR_CCU_EID_BYTES>& eid)
{
    return std::all_of(eid.begin(), eid.end(), [](uint8_t value) {
        return value == 0;
    });
}

bool HasCompleteVerifiedEndpointRoute(const TileXRCcuLowerLayerTransportRoute& route)
{
    return route.endpointRouteVerified &&
        !IsEmptyEndpointEid(route.remoteEid) &&
        route.doorbellVa != 0 &&
        route.doorbellTokenId != 0 &&
        route.sqDepth != 0;
}

bool RangeContains(uint32_t start, uint32_t count, uint32_t value)
{
    return count != 0 && value >= start && value < start + count;
}

void NormalizeVerifiedEndpointRouteJettyWindow(TileXRCcuLowerLayerTransportSnapshot* snapshot)
{
    if (snapshot == nullptr || snapshot->routes.empty()) {
        return;
    }

    uint16_t minExplicitStart = 0;
    uint32_t explicitEnd = 0;
    uint32_t explicitStartCount = 0;
    for (const auto& route : snapshot->routes) {
        if (route.startJettyId == 0) {
            continue;
        }
        if (explicitStartCount == 0) {
            minExplicitStart = route.startJettyId;
        } else {
            minExplicitStart = std::min<uint16_t>(minExplicitStart, route.startJettyId);
        }
        explicitEnd = std::max<uint32_t>(explicitEnd, static_cast<uint32_t>(route.startJettyId) + 1U);
        ++explicitStartCount;
    }

    const bool configuredWindowContainsExplicitJettys =
        snapshot->startJettyId != 0 && snapshot->pfeJettyCount != 0 && minExplicitStart != 0 &&
        minExplicitStart >= snapshot->startJettyId &&
        explicitEnd <= static_cast<uint32_t>(snapshot->startJettyId) + snapshot->pfeJettyCount;
    if (minExplicitStart != 0 && !configuredWindowContainsExplicitJettys) {
        snapshot->startJettyId = minExplicitStart;
    }

    uint32_t requiredJettyCount = static_cast<uint32_t>(snapshot->routes.size());
    if (snapshot->startJettyId != 0) {
        uint32_t end = static_cast<uint32_t>(snapshot->startJettyId) + snapshot->routes.size();
        for (uint32_t i = 0; i < snapshot->routes.size(); ++i) {
            const uint32_t start = snapshot->routes[i].startJettyId == 0 ?
                static_cast<uint32_t>(snapshot->startJettyId) + i :
                snapshot->routes[i].startJettyId;
            end = std::max<uint32_t>(end, start + 1U);
        }
        requiredJettyCount = std::max<uint32_t>(
            requiredJettyCount,
            end - static_cast<uint32_t>(snapshot->startJettyId));
    } else if (explicitEnd != 0 && minExplicitStart != 0) {
        requiredJettyCount = std::max<uint32_t>(
            requiredJettyCount,
            explicitEnd - static_cast<uint32_t>(minExplicitStart));
    }
    snapshot->pfeJettyCount = CheckedU16(std::max<uint32_t>(snapshot->pfeJettyCount, requiredJettyCount));
}

uint32_t SelectLowerLayerPfeOffset(uint8_t dieId, uint32_t pfeId)
{
    if (LowerLayerEnvEquals("TILEXR_CCU_DIRECT_LOWER_LAYER_PFE_OFFSET_SOURCE", "hcomm_die")) {
        return static_cast<uint32_t>(dieId) * TILEXR_CCU_HCOMM_PER_DIE_PFE_RESERVED_NUM + pfeId;
    }
    return pfeId;
}

void ApplyHcommFeIdPfePartition(uint32_t pfeId, TileXRCcuLowerLayerTransportSnapshot* snapshot)
{
    if (snapshot == nullptr) {
        return;
    }
    if (pfeId > TILEXR_CCU_HCOMM_MAX_INNER_FE_ID) {
        snapshot->startLocalJettyCtxId = TILEXR_CCU_HCOMM_OUTER_FE_START_JETTY_CTX_ID;
        snapshot->pfeJettyCount = TILEXR_CCU_HCOMM_OUTER_FE_JETTY_NUM;
    } else {
        snapshot->startLocalJettyCtxId = CheckedU16(pfeId * TILEXR_CCU_HCOMM_INNER_FE_JETTY_NUM);
        snapshot->pfeJettyCount = TILEXR_CCU_HCOMM_INNER_FE_JETTY_NUM;
    }
    snapshot->startJettyId = CheckedU16(TILEXR_CCU_DEFAULT_START_JETTY_ID + snapshot->startLocalJettyCtxId);
}

void ApplyHcommOrderedPfePartition(TileXRCcuLowerLayerTransportSnapshot* snapshot)
{
    if (snapshot == nullptr) {
        return;
    }
    snapshot->startLocalJettyCtxId = 0;
    snapshot->pfeJettyCount = TILEXR_CCU_HCOMM_PER_DIE_JETTY_NUM;
    snapshot->startJettyId = TILEXR_CCU_DEFAULT_START_JETTY_ID;
}

void ApplyLowerLayerPfePartition(uint32_t pfeId, TileXRCcuLowerLayerTransportSnapshot* snapshot)
{
    if (LowerLayerEnvEquals("TILEXR_CCU_DIRECT_LOWER_LAYER_PFE_PARTITION", "hcomm_fe_id")) {
        ApplyHcommFeIdPfePartition(pfeId, snapshot);
        return;
    }
    if (LowerLayerEnvEquals("TILEXR_CCU_DIRECT_LOWER_LAYER_PFE_PARTITION", "hcomm_ordered") ||
        LowerLayerEnvEquals("TILEXR_CCU_DIRECT_LOWER_LAYER_PFE_PARTITION", "hcomm")) {
        ApplyHcommOrderedPfePartition(snapshot);
    }
}

int ValidateSpec(
    const TileXRCcuLowerLayerPlanSpec& spec,
    TileXRCcuLowerLayerInstallPlan* plan,
    TileXRCcuLowerLayerPlanBuilderReport* report)
{
    if (!spec.msidToken.valid) {
        return Fail(plan, report, "missing lower-layer CCU MSID token");
    }
    if (spec.jettys.empty()) {
        return Fail(plan, report, "missing lower-layer CCU local jetty contexts");
    }
    if (spec.channels.empty()) {
        return Fail(plan, report, "missing lower-layer CCU channel contexts");
    }
    if (!spec.xnClear.valid || spec.xnClear.count == 0) {
        return Fail(plan, report, "missing lower-layer CCU local XN clear range");
    }
    if (!spec.ckeClear.valid || spec.ckeClear.count == 0) {
        return Fail(plan, report, "missing lower-layer CCU CKE clear range");
    }
    if (spec.jettys.size() > std::numeric_limits<uint16_t>::max()) {
        return Fail(plan, report, "too many lower-layer CCU local jetty contexts");
    }
    if (spec.pfe.jettyCount != 0 && spec.pfe.jettyCount > 128U) {
        return Fail(plan, report, "lower-layer CCU PFE jetty count is out of range");
    }
    if (spec.pfe.jettyCount != 0 && spec.pfe.jettyCount < spec.jettys.size()) {
        return Fail(plan, report, "lower-layer CCU PFE jetty window is smaller than local jetty contexts");
    }
    if (AddOverflowsU16(spec.pfe.startLocalJettyCtxId, static_cast<uint32_t>(spec.jettys.size()))) {
        return Fail(plan, report, "lower-layer CCU local jetty context range overflows");
    }
    return TILEXR_SUCCESS;
}

void FillReport(const TileXRCcuLowerLayerInstallPlan& plan, TileXRCcuLowerLayerPlanBuilderReport* report)
{
    if (report == nullptr) {
        return;
    }
    report->msidTokenCount = static_cast<uint32_t>(plan.msidTokens.size());
    report->pfeCount = static_cast<uint32_t>(plan.pfes.size());
    report->jettyCount = static_cast<uint32_t>(plan.jettys.size());
    report->localJettyCtxCount = 0;
    for (const auto& jetty : plan.jettys) {
        report->localJettyCtxCount += static_cast<uint32_t>(jetty.ctxs.size());
    }
    report->channelCount = static_cast<uint32_t>(plan.channels.size());
    report->ckeClearCount = static_cast<uint32_t>(plan.ckeClears.size());
    report->message = "ok";
}

void FillTemplateReport(
    const TileXRCcuLowerLayerTransportSnapshot& snapshot,
    TileXRCcuLowerLayerPlanBuilderReport* report)
{
    if (report == nullptr) {
        return;
    }
    report->msidTokenCount = snapshot.msidToken.valid ? 1U : 0U;
    report->pfeCount = snapshot.routes.empty() ? 0U : 1U;
    report->jettyCount = static_cast<uint32_t>(snapshot.routes.size());
    report->localJettyCtxCount = static_cast<uint32_t>(snapshot.routes.size());
    report->channelCount = static_cast<uint32_t>(snapshot.routes.size());
    report->ckeClearCount = snapshot.ckeCount == 0 ? 0U : 1U;
    report->message = "ok";
}

void AppendRemoteNotifyCkeClears(
    const TileXRCcuLowerLayerTransportSnapshot& snapshot,
    TileXRCcuLowerLayerInstallPlan* plan)
{
    if (plan == nullptr) {
        return;
    }
    std::vector<uint32_t> notifyCkes;
    notifyCkes.reserve(snapshot.routes.size());
    for (const auto& route : snapshot.routes) {
        if (route.remoteNotifyCke == 0 ||
            RangeContains(snapshot.ckeStartId, snapshot.ckeCount, route.remoteNotifyCke)) {
            continue;
        }
        notifyCkes.push_back(route.remoteNotifyCke);
    }
    if (notifyCkes.empty()) {
        return;
    }

    std::sort(notifyCkes.begin(), notifyCkes.end());
    notifyCkes.erase(std::unique(notifyCkes.begin(), notifyCkes.end()), notifyCkes.end());

    uint32_t rangeStart = notifyCkes.front();
    uint32_t previous = rangeStart;
    for (size_t i = 1; i <= notifyCkes.size(); ++i) {
        if (i < notifyCkes.size() && notifyCkes[i] == previous + 1U) {
            previous = notifyCkes[i];
            continue;
        }
        plan->ckeClears.push_back({
            snapshot.dieId,
            rangeStart,
            previous - rangeStart + 1U,
        });
        if (i < notifyCkes.size()) {
            rangeStart = notifyCkes[i];
            previous = rangeStart;
        }
    }
}

void AppendRemoteXnClears(
    const TileXRCcuLowerLayerTransportSnapshot& snapshot,
    TileXRCcuLowerLayerInstallPlan* plan)
{
    if (plan == nullptr) {
        return;
    }
    if (snapshot.remoteXnStartId != 0 && snapshot.remoteXnCount != 0) {
        plan->xnClears.push_back({
            snapshot.dieId,
            snapshot.remoteXnStartId,
            snapshot.remoteXnCount,
        });
        return;
    }
    std::vector<uint32_t> remoteXns;
    remoteXns.reserve(snapshot.routes.size());
    for (const auto& route : snapshot.routes) {
        if (route.remoteXnId == 0 ||
            RangeContains(snapshot.xnStartId, snapshot.xnCount, route.remoteXnId)) {
            continue;
        }
        remoteXns.push_back(route.remoteXnId);
    }
    if (remoteXns.empty()) {
        return;
    }

    std::sort(remoteXns.begin(), remoteXns.end());
    remoteXns.erase(std::unique(remoteXns.begin(), remoteXns.end()), remoteXns.end());

    uint32_t rangeStart = remoteXns.front();
    uint32_t previous = rangeStart;
    for (size_t i = 1; i <= remoteXns.size(); ++i) {
        if (i < remoteXns.size() && remoteXns[i] == previous + 1U) {
            previous = remoteXns[i];
            continue;
        }
        plan->xnClears.push_back({
            snapshot.dieId,
            rangeStart,
            previous - rangeStart + 1U,
        });
        if (i < remoteXns.size()) {
            rangeStart = remoteXns[i];
            previous = rangeStart;
        }
    }
}

} // namespace

int TileXRCcuBuildLowerLayerInstallPlan(
    const TileXRCcuLowerLayerPlanSpec& spec,
    TileXRCcuLowerLayerInstallPlan* plan,
    TileXRCcuLowerLayerPlanBuilderReport* report)
{
    ResetReport(report);
    if (plan == nullptr) {
        return Fail(nullptr, report, "missing output lower-layer CCU install plan");
    }
    *plan = TileXRCcuLowerLayerInstallPlan{};

    if (ValidateSpec(spec, plan, report) != TILEXR_SUCCESS) {
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }

    TileXRCcuLowerLayerInstallPlan result;
    result.msidTokens.push_back({
        spec.msidToken.dieId,
        spec.msidToken.msId,
        spec.msidToken.tokenId,
        spec.msidToken.tokenValue,
    });

    TileXRCcuLowerLayerPayloadReport payloadReport;
    TileXRCcuPfeInstall pfe;
    pfe.dieId = spec.pfe.dieId;
    pfe.pfeOffset = spec.pfe.pfeOffset;
    TileXRCcuPfeCtxSpec pfeCtxSpec;
    pfeCtxSpec.startJettyId = spec.pfe.startJettyId;
    pfeCtxSpec.jettyCount = spec.pfe.jettyCount == 0 ?
        CheckedU16(static_cast<uint32_t>(spec.jettys.size())) :
        spec.pfe.jettyCount;
    pfeCtxSpec.startLocalJettyCtxId = spec.pfe.startLocalJettyCtxId;
    if (TileXRCcuBuildPfeCtx(pfeCtxSpec, &pfe.ctx, &payloadReport) != TILEXR_SUCCESS) {
        return FailPayload(plan, report, payloadReport);
    }
    result.pfes.push_back(pfe);

    TileXRCcuJettyInstall jettyInstall;
    jettyInstall.dieId = spec.pfe.dieId;
    jettyInstall.startJettyCtxId = spec.jettys.front().startJettyCtxId == 0 ?
        spec.pfe.startLocalJettyCtxId :
        spec.jettys.front().startJettyCtxId;
    for (const auto& jettySpec : spec.jettys) {
        if (jettySpec.startJettyCtxId != 0 && jettySpec.startJettyCtxId != jettyInstall.startJettyCtxId +
            jettyInstall.ctxs.size()) {
            return Fail(plan, report, "lower-layer CCU local jetty contexts must be contiguous");
        }
        TileXRCcuLocalJettyCtxData ctx;
        TileXRCcuLocalJettyCtxSpec ctxSpec;
        ctxSpec.dieId = jettySpec.dieId;
        ctxSpec.pfeId = jettySpec.pfeId;
        ctxSpec.doorbellVa = jettySpec.doorbellVa;
        ctxSpec.doorbellTokenId = jettySpec.doorbellTokenId;
        ctxSpec.doorbellTokenValue = jettySpec.doorbellTokenValue;
        ctxSpec.sqDepth = jettySpec.sqDepth;
        ctxSpec.wqeBasicBlockStartId = jettySpec.wqeBasicBlockStartId;
        if (TileXRCcuBuildLocalJettyCtx(ctxSpec, &ctx, &payloadReport) != TILEXR_SUCCESS) {
            return FailPayload(plan, report, payloadReport);
        }
        jettyInstall.ctxs.push_back(ctx);
    }
    result.jettys.push_back(jettyInstall);

    for (const auto& channelSpec : spec.channels) {
        TileXRCcuChannelInstall channel;
        channel.dieId = channelSpec.dieId;
        channel.channelId = channelSpec.channelId;
        TileXRCcuChannelCtxV1Spec ctxSpec;
        ctxSpec.remoteEid = channelSpec.remoteEid;
        ctxSpec.tpn = channelSpec.tpn;
        ctxSpec.sourcePfeId = channelSpec.sourcePfeId;
        ctxSpec.startJettyId = channelSpec.startJettyId;
        ctxSpec.jettyCount = channelSpec.jettyCount == 0 ?
            CheckedU16(static_cast<uint32_t>(spec.jettys.size())) :
            channelSpec.jettyCount;
        ctxSpec.dieId = channelSpec.dieId;
        ctxSpec.memoryTokenId = channelSpec.memoryTokenId;
        ctxSpec.memoryTokenValue = channelSpec.memoryTokenValue;
        ctxSpec.remoteCcuVa = channelSpec.remoteCcuVa;
        if (TileXRCcuBuildChannelCtxV1(ctxSpec, &channel.ctx, &payloadReport) != TILEXR_SUCCESS) {
            return FailPayload(plan, report, payloadReport);
        }
        result.channels.push_back(channel);
    }

    result.xnClears.push_back({
        spec.xnClear.dieId,
        spec.xnClear.startXnId,
        spec.xnClear.count,
    });

    result.ckeClears.push_back({
        spec.ckeClear.dieId,
        spec.ckeClear.startCkeId,
        spec.ckeClear.count,
    });
    result.remoteXnBindings = spec.remoteXnBindings;

    *plan = result;
    FillReport(*plan, report);
    return TILEXR_SUCCESS;
}

int TileXRCcuBuildLowerLayerTransportTemplate(
    const TileXRCcuBasicInfo& basicInfo,
    const TileXRCcuResourceAllocation& allocation,
    const std::vector<TileXRCcuRemoteCcuBufferInfo>& remoteCcuBuffers,
    TileXRCcuLowerLayerTransportSnapshot* snapshot,
    TileXRCcuLowerLayerPlanBuilderReport* report)
{
    ResetReport(report);
    if (snapshot == nullptr) {
        return Fail(nullptr, report, "missing output lower-layer CCU transport template");
    }
    *snapshot = TileXRCcuLowerLayerTransportSnapshot{};

    const TileXRCcuRange localWaitCke = EffectiveLocalWaitCkeRange(allocation);
    const TileXRCcuRange remoteNotifyCke = EffectiveRemoteNotifyCkeRange(allocation);
    if (allocation.channels.num == 0 || allocation.localXn.num == 0 || localWaitCke.num == 0 ||
        remoteNotifyCke.num == 0 || allocation.remoteXn.num == 0) {
        return Fail(nullptr, report, "missing lower-layer CCU allocated resources");
    }
    if (remoteCcuBuffers.empty() || remoteCcuBuffers.size() != allocation.remoteXn.num) {
        return Fail(nullptr, report, "remote CCU buffer template count does not match remote XN allocation");
    }
    if (allocation.channels.num < remoteCcuBuffers.size()) {
        return Fail(nullptr, report, "channel allocation count does not match lower-layer route count");
    }
    if (remoteCcuBuffers.size() > std::numeric_limits<uint16_t>::max()) {
        return Fail(nullptr, report, "too many lower-layer CCU remote routes");
    }

    TileXRCcuLowerLayerTransportSnapshot result;
    result.msidToken.dieId = basicInfo.dieId;
    result.msidToken.msId = basicInfo.msId;
    if (basicInfo.msidToken.valid && basicInfo.msidToken.tokenId != 0) {
        result.msidToken.tokenId = basicInfo.msidToken.tokenId;
        result.msidToken.tokenValue = basicInfo.msidToken.tokenValue;
        result.msidToken.valid = true;
    }
    result.dieId = basicInfo.dieId;
    result.pfeId = remoteCcuBuffers.front().localPfeIdValid ?
        remoteCcuBuffers.front().localPfeId :
        allocation.channels.startId;
    result.pfeOffset = SelectLowerLayerPfeOffset(basicInfo.dieId, result.pfeId);
    result.startJettyId = TILEXR_CCU_DEFAULT_START_JETTY_ID;
    result.startLocalJettyCtxId = TILEXR_CCU_DEFAULT_START_LOCAL_JETTY_CTX_ID;
    ApplyLowerLayerPfePartition(result.pfeId, &result);
    result.xnStartId = allocation.localXn.startId;
    result.xnCount = allocation.localXn.num;
    result.remoteXnStartId = allocation.remoteXn.startId;
    result.remoteXnCount = allocation.remoteXn.num;
    result.ckeStartId = localWaitCke.startId;
    result.ckeCount = localWaitCke.num;
    result.routes.reserve(remoteCcuBuffers.size());

    const uint16_t wqeBasicBlockStride = SelectLowerLayerWqeBasicBlockStride();
    std::map<uint16_t, uint16_t> wqeStartByJettyId;
    uint32_t nextVerifiedWqeStartId = 0;
    for (uint32_t i = 0; i < remoteCcuBuffers.size(); ++i) {
        const auto& remoteCcuBuffer = remoteCcuBuffers[i];
        if (remoteCcuBuffer.remoteCcuVa == 0) {
            return Fail(nullptr, report, "missing remote CCU VA for lower-layer route");
        }
        TileXRCcuLowerLayerTransportRoute route;
        route.channelId = allocation.channels.startId + i;
        route.peerRank = remoteCcuBuffer.peerRank == TILEXR_CCU_REMOTE_PEER_RANK_UNKNOWN ?
            i :
            remoteCcuBuffer.peerRank;
        route.remoteXnId = remoteCcuBuffer.remoteXnId == 0 ?
            static_cast<uint16_t>(allocation.remoteXn.startId + i) :
            remoteCcuBuffer.remoteXnId;
        route.remoteNotifyCke = remoteCcuBuffer.remoteNotifyCke == 0 ?
            static_cast<uint16_t>(remoteNotifyCke.startId + i) :
            remoteCcuBuffer.remoteNotifyCke;
        if (i > std::numeric_limits<uint16_t>::max() / wqeBasicBlockStride) {
            return Fail(nullptr, report, "lower-layer CCU WQE basic block start overflows");
        }
        route.remoteCcuVa = remoteCcuBuffer.remoteCcuVa;
        route.memoryTokenId = remoteCcuBuffer.memoryTokenId;
        route.memoryTokenValue = remoteCcuBuffer.memoryTokenValue;
        route.channelResourceOwnerVerified = remoteCcuBuffer.channelResourceOwnerVerified;
        route.transportResourceExchangeVerified = remoteCcuBuffer.transportResourceExchangeVerified;
        if (remoteCcuBuffer.endpointRouteVerified &&
            !IsEmptyEndpointEid(remoteCcuBuffer.remoteEid) &&
            remoteCcuBuffer.doorbellVa != 0 &&
            remoteCcuBuffer.doorbellTokenId != 0 &&
            remoteCcuBuffer.sqDepth != 0) {
            route.remoteEid = remoteCcuBuffer.remoteEid;
            route.tpn = remoteCcuBuffer.tpn;
            route.doorbellVa = remoteCcuBuffer.doorbellVa;
            route.doorbellTokenId = remoteCcuBuffer.doorbellTokenId;
            route.doorbellTokenValue = remoteCcuBuffer.doorbellTokenValue;
            route.sqDepth = remoteCcuBuffer.sqDepth;
            route.localDoorbellVa = remoteCcuBuffer.localDoorbellVa;
            route.localDoorbellTokenId = remoteCcuBuffer.localDoorbellTokenId;
            route.localDoorbellTokenValue = remoteCcuBuffer.localDoorbellTokenValue;
            route.localSqDepth = remoteCcuBuffer.localSqDepth;
            route.startJettyId = remoteCcuBuffer.startJettyId;
            route.endpointRouteVerified = true;
        }
        uint32_t wqeOrdinal = i;
        if (route.startJettyId != 0) {
            const uint32_t sqDepth = route.localSqDepth == 0 ? route.sqDepth : route.localSqDepth;
            const uint32_t wqeBasicBlockCount = sqDepth * TILEXR_CCU_WQE_BASIC_BLOCKS_PER_ROUTE;
            if (wqeBasicBlockCount == 0 || nextVerifiedWqeStartId > std::numeric_limits<uint16_t>::max()) {
                return Fail(nullptr, report, "invalid verified endpoint WQE basic block window");
            }
            uint32_t wqeBasicBlockStartId = nextVerifiedWqeStartId;
            const bool pfeWindowContainsJetty = result.pfeJettyCount != 0 &&
                route.startJettyId >= result.startJettyId &&
                static_cast<uint32_t>(route.startJettyId) <
                    static_cast<uint32_t>(result.startJettyId) + result.pfeJettyCount;
            if (pfeWindowContainsJetty) {
                const uint32_t localJettyOffset =
                    static_cast<uint32_t>(route.startJettyId) - result.startJettyId;
                wqeBasicBlockStartId = localJettyOffset * wqeBasicBlockCount;
            }
            if (wqeBasicBlockStartId > std::numeric_limits<uint16_t>::max()) {
                return Fail(nullptr, report, "verified endpoint WQE basic block start overflows");
            }
            const auto inserted = wqeStartByJettyId.emplace(
                route.startJettyId,
                static_cast<uint16_t>(wqeBasicBlockStartId));
            route.wqeBasicBlockStartId = inserted.first->second;
            if (inserted.second && !pfeWindowContainsJetty) {
                nextVerifiedWqeStartId += wqeBasicBlockCount;
            }
        } else {
            if (wqeOrdinal > std::numeric_limits<uint16_t>::max() / wqeBasicBlockStride) {
                return Fail(nullptr, report, "lower-layer CCU WQE basic block start overflows");
            }
            route.wqeBasicBlockStartId = static_cast<uint16_t>(wqeOrdinal * wqeBasicBlockStride);
        }
        result.routes.push_back(route);
    }
    NormalizeVerifiedEndpointRouteJettyWindow(&result);

    *snapshot = result;
    FillTemplateReport(*snapshot, report);
    return TILEXR_SUCCESS;
}

int TileXRCcuOverlayVerifiedEndpointRoutes(
    const std::vector<TileXRCcuLowerLayerTransportRoute>& verifiedRoutes,
    TileXRCcuLowerLayerTransportSnapshot* snapshot,
    TileXRCcuLowerLayerPlanBuilderReport* report)
{
    ResetReport(report);
    if (snapshot == nullptr) {
        return Fail(nullptr, report, "missing lower-layer CCU transport snapshot for endpoint route overlay");
    }
    if (verifiedRoutes.empty()) {
        FillTemplateReport(*snapshot, report);
        return TILEXR_SUCCESS;
    }

    for (const auto& verified : verifiedRoutes) {
        if (!HasCompleteVerifiedEndpointRoute(verified)) {
            return Fail(nullptr, report, "verified endpoint route is incomplete");
        }
        auto routeIt = std::find_if(
            snapshot->routes.begin(),
            snapshot->routes.end(),
            [&verified](const TileXRCcuLowerLayerTransportRoute& route) {
                return route.channelId == verified.channelId;
            });
        if (routeIt == snapshot->routes.end()) {
            return Fail(nullptr, report, "verified endpoint route does not match an allocated channel");
        }

        routeIt->remoteEid = verified.remoteEid;
        routeIt->tpn = verified.tpn;
        routeIt->doorbellVa = verified.doorbellVa;
        routeIt->doorbellTokenId = verified.doorbellTokenId;
        routeIt->doorbellTokenValue = verified.doorbellTokenValue;
        routeIt->sqDepth = verified.sqDepth;
        routeIt->localDoorbellVa = verified.localDoorbellVa;
        routeIt->localDoorbellTokenId = verified.localDoorbellTokenId;
        routeIt->localDoorbellTokenValue = verified.localDoorbellTokenValue;
        routeIt->localSqDepth = verified.localSqDepth;
        routeIt->startJettyId = verified.startJettyId;
        routeIt->endpointRouteVerified = true;
        routeIt->channelResourceOwnerVerified = verified.channelResourceOwnerVerified;
        routeIt->transportResourceExchangeVerified = verified.transportResourceExchangeVerified;
    }
    NormalizeVerifiedEndpointRouteJettyWindow(snapshot);

    FillTemplateReport(*snapshot, report);
    return TILEXR_SUCCESS;
}

int TileXRCcuBuildLowerLayerInstallPlanFromTransportSnapshot(
    const TileXRCcuLowerLayerTransportSnapshot& snapshot,
    TileXRCcuLowerLayerInstallPlan* plan,
    TileXRCcuLowerLayerPlanBuilderReport* report)
{
    ResetReport(report);
    if (plan == nullptr) {
        return Fail(nullptr, report, "missing output lower-layer CCU install plan");
    }
    *plan = TileXRCcuLowerLayerInstallPlan{};

    if (snapshot.routes.empty()) {
        return Fail(plan, report, "missing lower-layer CCU transport routes");
    }
    if (snapshot.routes.size() > std::numeric_limits<uint16_t>::max()) {
        return Fail(plan, report, "too many lower-layer CCU transport routes");
    }

    TileXRCcuLowerLayerTransportSnapshot normalized = snapshot;
    NormalizeVerifiedEndpointRouteJettyWindow(&normalized);

    TileXRCcuLowerLayerPlanSpec spec;
    spec.msidToken = normalized.msidToken;
    spec.pfe.dieId = normalized.dieId;
    spec.pfe.pfeOffset = normalized.pfeOffset;
    spec.pfe.startJettyId = normalized.startJettyId;
    spec.pfe.jettyCount = normalized.pfeJettyCount;
    spec.pfe.startLocalJettyCtxId = normalized.startLocalJettyCtxId;
    spec.xnClear.dieId = normalized.dieId;
    spec.xnClear.startXnId = normalized.xnStartId;
    spec.xnClear.count = normalized.xnCount;
    spec.xnClear.valid = normalized.xnCount != 0;
    spec.ckeClear.dieId = normalized.dieId;
    spec.ckeClear.startCkeId = normalized.ckeStartId;
    spec.ckeClear.count = normalized.ckeCount;
    spec.ckeClear.valid = normalized.ckeCount != 0;

    std::map<uint16_t, const TileXRCcuLowerLayerTransportRoute*> jettyRoutes;
    for (uint32_t i = 0; i < normalized.routes.size(); ++i) {
        const auto& route = normalized.routes[i];
        const uint16_t jettyId = route.startJettyId == 0 ?
            static_cast<uint16_t>(normalized.startJettyId + i) :
            route.startJettyId;
        jettyRoutes.emplace(jettyId, &route);
    }
    for (const auto& entry : jettyRoutes) {
        if (entry.first < normalized.startJettyId) {
            return Fail(plan, report, "lower-layer CCU endpoint jetty ID precedes the PFE jetty window");
        }
        const uint32_t localJettyOffset = static_cast<uint32_t>(entry.first) - normalized.startJettyId;
        if (localJettyOffset >= normalized.pfeJettyCount ||
            static_cast<uint32_t>(normalized.startLocalJettyCtxId) + localJettyOffset >= 128U) {
            return Fail(plan, report, "lower-layer CCU endpoint jetty ID is outside the PFE jetty window");
        }
        const auto& route = *entry.second;
        TileXRCcuLowerLayerJettySpec jetty;
        jetty.dieId = normalized.dieId;
        jetty.pfeId = normalized.pfeId;
        jetty.startJettyCtxId = static_cast<uint16_t>(normalized.startLocalJettyCtxId + localJettyOffset);
        jetty.doorbellVa = route.localDoorbellVa == 0 ? route.doorbellVa : route.localDoorbellVa;
        jetty.doorbellTokenId = route.localDoorbellTokenId == 0 ?
            route.doorbellTokenId :
            route.localDoorbellTokenId;
        jetty.doorbellTokenValue = route.localDoorbellVa == 0 ?
            route.doorbellTokenValue :
            route.localDoorbellTokenValue;
        jetty.sqDepth = route.localSqDepth == 0 ? route.sqDepth : route.localSqDepth;
        jetty.wqeBasicBlockStartId = route.wqeBasicBlockStartId;
        spec.jettys.push_back(jetty);
    }

    uint32_t routeIndex = 0;
    for (const auto& route : normalized.routes) {
        TileXRCcuLowerLayerChannelSpec channel;
        channel.dieId = normalized.dieId;
        channel.channelId = route.channelId;
        channel.remoteEid = route.remoteEid;
        channel.tpn = route.tpn;
        channel.sourcePfeId = normalized.pfeId;
        channel.startJettyId = route.startJettyId == 0 ?
            static_cast<uint16_t>(normalized.startJettyId + routeIndex) :
            route.startJettyId;
        channel.jettyCount = 1;
        channel.memoryTokenId = route.memoryTokenId;
        channel.memoryTokenValue = route.memoryTokenValue;
        channel.remoteCcuVa = route.remoteCcuVa;
        const auto channelIt = std::find_if(
            spec.channels.begin(),
            spec.channels.end(),
            [&channel](const TileXRCcuLowerLayerChannelSpec& existing) {
                return existing.dieId == channel.dieId && existing.channelId == channel.channelId;
            });
        if (channelIt == spec.channels.end()) {
            spec.channels.push_back(channel);
        }

        TileXRCcuRemoteXnBindingProof remoteXn;
        remoteXn.dieId = normalized.dieId;
        remoteXn.channelId = static_cast<uint16_t>(route.channelId);
        remoteXn.localXn = static_cast<uint16_t>(normalized.xnStartId + routeIndex);
        remoteXn.remoteXn = route.remoteXnId;
        remoteXn.notifyCke = route.remoteNotifyCke == 0 ?
            static_cast<uint16_t>(normalized.ckeStartId + routeIndex) :
            route.remoteNotifyCke;
        remoteXn.peerRank = route.peerRank;
        remoteXn.peerExchangeObserved = route.remoteXnId != 0;
        remoteXn.localWaitCke = static_cast<uint16_t>(normalized.ckeStartId + routeIndex);
        remoteXn.endpointRouteVerified = route.endpointRouteVerified;
        remoteXn.channelResourceOwnerVerified = route.channelResourceOwnerVerified;
        remoteXn.transportResourceExchangeVerified = route.transportResourceExchangeVerified;
        spec.remoteXnBindings.push_back(remoteXn);
        ++routeIndex;
    }

    const int ret = TileXRCcuBuildLowerLayerInstallPlan(spec, plan, report);
    if (ret != TILEXR_SUCCESS) {
        return ret;
    }
    AppendRemoteXnClears(normalized, plan);
    AppendRemoteNotifyCkeClears(normalized, plan);
    FillReport(*plan, report);
    return TILEXR_SUCCESS;
}

} // namespace TileXR
