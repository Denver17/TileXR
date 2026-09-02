/*
 * Copyright (c) 2026 TileXR Project
 * Licensed under the Apache License, Version 2.0
 */

#include "ccu/tilexr_ccu_resource_allocator.h"

#include <algorithm>
#include <limits>

namespace TileXR {
namespace {

constexpr const char* TILEXR_CCU_HCOMM_DERIVED_PROVIDER = "tilexr-hcomm-derived-resource-allocator";
constexpr uint32_t TILEXR_CCU_HCOMM_TASK1_PRELUDE_INSTRUCTION_COUNT = 5U;
constexpr uint32_t TILEXR_CCU_HCOMM_TASK1_PRELUDE_RESERVED_XN_COUNT = 1U;

void ResetReport(TileXRCcuResourceAllocatorReport* report)
{
    if (report != nullptr) {
        *report = TileXRCcuResourceAllocatorReport{};
    }
}

int Fail(TileXRCcuResourceAllocatorReport* report, const std::string& message)
{
    if (report != nullptr) {
        report->message = message;
    }
    return TILEXR_ERROR_PARA_CHECK_FAIL;
}

bool AddWouldOverflow(uint16_t start, uint16_t count)
{
    return static_cast<uint32_t>(start) + count > static_cast<uint32_t>(std::numeric_limits<uint16_t>::max()) + 1U;
}

bool ContainsRange(uint16_t outerStart, uint16_t outerCount, uint16_t innerStart, uint32_t innerCount)
{
    if (outerCount == 0 || innerCount == 0) {
        return false;
    }
    const uint32_t outerBegin = outerStart;
    const uint32_t outerEnd = outerBegin + outerCount;
    const uint32_t innerBegin = innerStart;
    const uint32_t innerEnd = innerBegin + innerCount;
    return innerBegin >= outerBegin && innerEnd <= outerEnd;
}

TileXRCcuRange MakeRange(uint8_t dieId, uint16_t startId, uint16_t count)
{
    TileXRCcuRange range;
    range.dieId = dieId;
    range.startId = startId;
    range.num = count;
    return range;
}

void FillReport(
    const TileXRCcuResourceAllocation& allocation,
    const TileXRCcuResourceRequest& request,
    TileXRCcuResourceAllocatorReport* report)
{
    if (report == nullptr) {
        return;
    }
    report->missionAllocated = allocation.mission.num;
    report->repositoryAllocated = allocation.repository.num;
    report->localXnAllocated = allocation.localXn.num;
    report->localGsaAllocated = allocation.localGsa.num;
    report->remoteXnAllocated = allocation.remoteXn.num;
    report->notifyCkeAllocated = allocation.notifyCke.num;
    report->channelBindingsAllocated = request.syncResourceCount * request.bindingsPerSyncResource;
    report->localWaitCkeAllocated = allocation.localWaitCke.num;
    report->remoteNotifyCkeAllocated = allocation.remoteNotifyCke.num;
    report->sourceCkeAllocated = allocation.sourceCke.num;
    report->message = "ok";
}

uint16_t CheckedU16(uint32_t value)
{
    return static_cast<uint16_t>(std::min<uint32_t>(value, std::numeric_limits<uint16_t>::max()));
}

bool SyncXnMode(TileXRCcuBarrierMode mode)
{
    return mode == TileXRCcuBarrierMode::SyncXn ||
        mode == TileXRCcuBarrierMode::SyncXnPostOnly ||
        mode == TileXRCcuBarrierMode::SyncXnLoadPostOnly;
}

uint32_t RequiredSqeLoadXnCount(uint32_t sqeArgCount, bool hcommStyleTask1Prelude)
{
    if (sqeArgCount == 0) {
        return 0;
    }

    uint32_t count = hcommStyleTask1Prelude ?
        std::max<uint32_t>(sqeArgCount, TILEXR_CCU_SQE_ARGS_LEN) +
            TILEXR_CCU_HCOMM_TASK1_PRELUDE_RESERVED_XN_COUNT :
        sqeArgCount;
    return count;
}

} // namespace

bool TileXRCcuResourceAllocator::HasCapacity(const Cursor& cursor, uint32_t count) const
{
    return count <= std::numeric_limits<uint16_t>::max() &&
        static_cast<uint32_t>(cursor.used) + count <= cursor.count;
}

uint16_t TileXRCcuResourceAllocator::CursorNext(const Cursor& cursor) const
{
    return static_cast<uint16_t>(cursor.start + cursor.used);
}

int TileXRCcuResourceAllocator::Init(const TileXRCcuResourceSpec& spec)
{
    const uint16_t missionInstructionStart =
        spec.missionInstructionStartId == 0 ? spec.instructionStartId : spec.missionInstructionStartId;
    const uint16_t localWaitCkeStart =
        spec.localWaitCkeCount == 0 ? spec.ckeStartId : spec.localWaitCkeStartId;
    const uint16_t localWaitCkeCount =
        spec.localWaitCkeCount == 0 ? spec.ckeCount : spec.localWaitCkeCount;
    const uint16_t remoteNotifyCkeStart =
        spec.remoteNotifyCkeCount == 0 ? spec.ckeStartId : spec.remoteNotifyCkeStartId;
    const uint16_t remoteNotifyCkeCount =
        spec.remoteNotifyCkeCount == 0 ? spec.ckeCount : spec.remoteNotifyCkeCount;
    const bool splitRemoteXn = spec.remoteXnCount != 0;

    if (spec.missionKey == 0 || spec.missionCount == 0 || spec.instructionCount == 0 ||
        localWaitCkeCount == 0 || remoteNotifyCkeCount == 0 ||
        spec.channelCount == 0 ||
        AddWouldOverflow(spec.missionStartId, spec.missionCount) ||
        AddWouldOverflow(spec.instructionStartId, spec.instructionCount) ||
        !ContainsRange(spec.instructionStartId, spec.instructionCount, missionInstructionStart, 1) ||
        AddWouldOverflow(spec.xnStartId, spec.xnCount) ||
        (spec.gsaCount != 0 && (spec.gsaStartId == 0 || AddWouldOverflow(spec.gsaStartId, spec.gsaCount))) ||
        (splitRemoteXn && (spec.remoteXnStartId == 0 || AddWouldOverflow(spec.remoteXnStartId, spec.remoteXnCount))) ||
        AddWouldOverflow(localWaitCkeStart, localWaitCkeCount) ||
        AddWouldOverflow(remoteNotifyCkeStart, remoteNotifyCkeCount) ||
        AddWouldOverflow(spec.channelStartId, spec.channelCount)) {
        initialized_ = false;
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }

    spec_ = spec;
    mission_ = {spec.missionStartId, spec.missionCount, 0};
    repository_ = {spec.instructionStartId, spec.instructionCount, 0};
    xn_ = {spec.xnStartId, spec.xnCount, 0};
    gsa_ = {spec.gsaStartId, spec.gsaCount, 0};
    remoteXn_ = splitRemoteXn ? Cursor{spec.remoteXnStartId, spec.remoteXnCount, 0} : Cursor{};
    localWaitCke_ = {localWaitCkeStart, localWaitCkeCount, 0};
    remoteNotifyCke_ = {remoteNotifyCkeStart, remoteNotifyCkeCount, 0};
    channel_ = {spec.channelStartId, spec.channelCount, 0};
    nextReceiptId_ = 1;
    active_.clear();
    initialized_ = true;
    return TILEXR_SUCCESS;
}

int TileXRCcuResourceAllocator::Allocate(
    const TileXRCcuResourceRequest& request,
    TileXRCcuProducerPlan* plan,
    TileXRCcuResourceAllocation* allocation,
    TileXRCcuResourceAllocatorReport* report)
{
    ResetReport(report);
    if (!initialized_) {
        return Fail(report, "CCU resource allocator is not initialized");
    }
    if (plan == nullptr || allocation == nullptr) {
        return Fail(report, "missing CCU resource allocation output");
    }
    *plan = TileXRCcuProducerPlan{};
    *allocation = TileXRCcuResourceAllocation{};

    if (request.sqeArgCount != 0 && request.sqeArgCount != TILEXR_CCU_SQE_ARGS_LEN) {
        return Fail(report, "invalid SQE argument count");
    }
    if (request.syncResourceCount == 0 || request.syncInstructionCount == 0 ||
        request.bindingsPerSyncResource == 0) {
        return Fail(report, "invalid CCU sync resource request");
    }
    if (request.bindingsPerSyncResource > std::numeric_limits<uint16_t>::max()) {
        return Fail(report, "channel binding count exceeds CCU plan encoding width");
    }
    if (request.syncResourceCount > std::numeric_limits<uint32_t>::max() / 2U) {
        return Fail(report, "barrier sync instruction window is too small for CCU post and wait microcode");
    }
    const bool syncCkeMode = request.barrierMode == TileXRCcuBarrierMode::SyncCke ||
        request.barrierMode == TileXRCcuBarrierMode::SyncCkeSetWait ||
        request.barrierMode == TileXRCcuBarrierMode::SyncCkePostOnly;
    const bool hcommStyleTask1Prelude = request.sqeArgCount != 0 && SyncXnMode(request.barrierMode);
    const bool syncXnMode = SyncXnMode(request.barrierMode);
    const bool postOnly = request.barrierMode == TileXRCcuBarrierMode::SyncXnPostOnly ||
        request.barrierMode == TileXRCcuBarrierMode::SyncCkePostOnly ||
        request.barrierMode == TileXRCcuBarrierMode::LocalCkePostOnly;
    const uint32_t requiredPostWaitInstructionCount =
        postOnly ? request.syncResourceCount : request.syncResourceCount * 2U;
    const uint32_t sourceCkeInitCount = syncCkeMode ? 1U : 0U;
    const uint32_t sourceCkeResourceCount = syncCkeMode ? 1U : 0U;
    const uint32_t task1PreludeInstructionCount =
        hcommStyleTask1Prelude ? TILEXR_CCU_HCOMM_TASK1_PRELUDE_INSTRUCTION_COUNT : 0U;
    const uint32_t requiredBarrierInstructionCount =
        requiredPostWaitInstructionCount + sourceCkeInitCount + task1PreludeInstructionCount;
    if (request.syncInstructionCount < requiredBarrierInstructionCount) {
        return Fail(report,
            hcommStyleTask1Prelude ?
                "barrier sync instruction window is too small for hcomm-style task1 prelude and CCU post/wait microcode" :
                "barrier sync instruction window is too small for CCU post and wait microcode");
    }

    const uint32_t localSqeXnCount = RequiredSqeLoadXnCount(request.sqeArgCount, hcommStyleTask1Prelude);
    const uint32_t localXnCount = std::max(localSqeXnCount, syncXnMode ? request.syncResourceCount : 0U);
    const uint32_t remoteXnCount = syncXnMode ? request.syncResourceCount : 0U;
    const uint32_t localGsaCount = hcommStyleTask1Prelude && spec_.gsaCount != 0 ? 1U : 0U;
    const uint32_t totalXnCount = localXnCount + remoteXnCount;
    const uint32_t localWaitCkeCount = request.syncResourceCount;
    const uint32_t remoteNotifyCkeCount = request.syncResourceCount;
    const uint32_t localCkeCount = localWaitCkeCount + sourceCkeResourceCount;
    const uint16_t repositoryStart = CursorNext(repository_);
    const uint16_t missionInstructionStart =
        spec_.missionInstructionStartId == 0 ? repositoryStart : spec_.missionInstructionStartId;
    if (!ContainsRange(repositoryStart, repository_.count, missionInstructionStart, 1)) {
        return Fail(report, "mission instruction start is outside instruction repository resources");
    }
    const uint32_t repositoryPrefixCount =
        static_cast<uint32_t>(missionInstructionStart) - static_cast<uint32_t>(repositoryStart);
    const uint32_t missionInstructionCount = request.sqeArgCount + request.syncInstructionCount;
    const uint32_t repositoryCount = repositoryPrefixCount + missionInstructionCount;
    const uint32_t channelCount = request.syncResourceCount;

    if (!HasCapacity(mission_, 1)) {
        return Fail(report, "insufficient mission resources");
    }
    if (!HasCapacity(repository_, repositoryCount)) {
        return Fail(report, "insufficient instruction repository resources");
    }
    const bool splitRemoteXn = remoteXn_.count != 0;
    if (!HasCapacity(xn_, splitRemoteXn ? localXnCount : totalXnCount)) {
        return Fail(report, "insufficient XN resources");
    }
    if (localGsaCount != 0 && !HasCapacity(gsa_, localGsaCount)) {
        return Fail(report, "insufficient GSA resources");
    }
    if (splitRemoteXn && !HasCapacity(remoteXn_, remoteXnCount)) {
        return Fail(report, "insufficient remote XN resources");
    }
    if (!HasCapacity(localWaitCke_, localCkeCount)) {
        return Fail(report, "insufficient CKE resources");
    }
    if (!HasCapacity(remoteNotifyCke_, remoteNotifyCkeCount)) {
        return Fail(report, "insufficient remote notify CKE resources");
    }
    if (!HasCapacity(channel_, channelCount)) {
        return Fail(report, "insufficient channel resources");
    }

    const uint16_t missionStart = CursorNext(mission_);
    const uint16_t localXnStart = CursorNext(xn_);
    const uint16_t localGsaStart = CursorNext(gsa_);
    const uint16_t remoteXnStart = splitRemoteXn ?
        CursorNext(remoteXn_) :
        static_cast<uint16_t>(localXnStart + localXnCount);
    const uint16_t localWaitCkeStart = CursorNext(localWaitCke_);
    const uint16_t sourceCkeStart = static_cast<uint16_t>(localWaitCkeStart + localWaitCkeCount);
    const uint16_t remoteNotifyCkeStart = CursorNext(remoteNotifyCke_);
    const uint16_t channelStart = CursorNext(channel_);

    TileXRCcuResourceAllocation result;
    result.receiptId = nextReceiptId_++;
    result.packageProvider = TILEXR_CCU_HCOMM_DERIVED_PROVIDER;
    result.mission = MakeRange(spec_.dieId, missionStart, 1);
    result.repository = MakeRange(spec_.dieId, repositoryStart, CheckedU16(repositoryCount));
    result.localXn = MakeRange(spec_.dieId, localXnStart, CheckedU16(localXnCount));
    result.localGsa = MakeRange(spec_.dieId, localGsaStart, CheckedU16(localGsaCount));
    result.remoteXn = MakeRange(spec_.dieId, remoteXnStart, CheckedU16(remoteXnCount));
    result.notifyCke = MakeRange(spec_.dieId, remoteNotifyCkeStart, CheckedU16(remoteNotifyCkeCount));
    result.channels = MakeRange(spec_.dieId, channelStart, CheckedU16(channelCount));
    result.localWaitCke = MakeRange(spec_.dieId, localWaitCkeStart, CheckedU16(localWaitCkeCount));
    result.remoteNotifyCke = result.notifyCke;
    result.sourceCke = MakeRange(spec_.dieId, sourceCkeStart, CheckedU16(sourceCkeResourceCount));

    TileXRCcuProducerPlan generated;
    generated.barrierMode = request.barrierMode;
    generated.mission = {spec_.dieId, static_cast<uint8_t>(missionStart), spec_.missionKey, true};
    generated.kernelLocalMission = result.mission;
    generated.kernelLocalXn = result.localXn;
    generated.kernelLocalGsa = result.localGsa;
    generated.kernelLocalCke = MakeRange(spec_.dieId, localWaitCkeStart, CheckedU16(localCkeCount));
    generated.instructionWindow = {
        spec_.dieId,
        result.repository.startId,
        result.repository.num,
        missionInstructionStart,
        CheckedU16(missionInstructionCount),
    };

    for (uint32_t i = 0; i < request.syncResourceCount; ++i) {
        TileXRCcuSyncResource resource;
        resource.dieId = spec_.dieId;
        if (syncXnMode) {
            resource.localXn = static_cast<uint16_t>(result.localXn.startId + i);
            resource.remoteXn = static_cast<uint16_t>(static_cast<uint32_t>(result.remoteXn.startId) + i);
        }
        resource.notifyCke = static_cast<uint16_t>(static_cast<uint32_t>(result.remoteNotifyCke.startId) + i);
        resource.channelId = static_cast<uint16_t>(static_cast<uint32_t>(result.channels.startId) + i);
        resource.bindingCount = CheckedU16(request.bindingsPerSyncResource);
        resource.localWaitCke = static_cast<uint16_t>(result.localWaitCke.startId + i);
        resource.localWaitMask = 1;
        resource.remoteNotifyMask = 1;
        if (syncCkeMode) {
            resource.sourceCke = result.sourceCke.startId;
            resource.sourceCkeMask = 0xffff;
        }
        generated.syncResources.push_back(resource);
    }

    if (request.sqeArgCount != 0) {
        TileXRCcuTaskWindow sqeLoadTask;
        sqeLoadTask.dieId = spec_.dieId;
        sqeLoadTask.instStartId = missionInstructionStart;
        sqeLoadTask.instCnt = CheckedU16(request.sqeArgCount);
        sqeLoadTask.argSize = TILEXR_CCU_SQE_ARGS_LEN;
        generated.taskWindows.push_back(sqeLoadTask);
    }

    TileXRCcuTaskWindow syncTask;
    syncTask.dieId = spec_.dieId;
    syncTask.instStartId = static_cast<uint16_t>(missionInstructionStart + request.sqeArgCount);
    syncTask.instCnt = CheckedU16(request.syncInstructionCount);
    syncTask.argSize = TILEXR_CCU_SQE_ARGS_LEN;
    generated.taskWindows.push_back(syncTask);

    TileXRCcuProducerPlanReport planReport;
    if (TileXRCcuValidateProducerPlan(generated, &planReport) != TILEXR_SUCCESS) {
        return Fail(report, planReport.message);
    }

    mission_.used = static_cast<uint16_t>(mission_.used + result.mission.num);
    repository_.used = static_cast<uint16_t>(repository_.used + result.repository.num);
    xn_.used = static_cast<uint16_t>(xn_.used + result.localXn.num + (splitRemoteXn ? 0 : result.remoteXn.num));
    gsa_.used = static_cast<uint16_t>(gsa_.used + result.localGsa.num);
    if (splitRemoteXn) {
        remoteXn_.used = static_cast<uint16_t>(remoteXn_.used + result.remoteXn.num);
    }
    localWaitCke_.used = static_cast<uint16_t>(localWaitCke_.used + result.localWaitCke.num + result.sourceCke.num);
    remoteNotifyCke_.used = static_cast<uint16_t>(remoteNotifyCke_.used + result.remoteNotifyCke.num);
    channel_.used = static_cast<uint16_t>(channel_.used + result.channels.num);

    ActiveAllocation active;
    active.allocation = result;
    active.missionUsed = result.mission.num;
    active.repositoryUsed = result.repository.num;
    active.localXnUsed = result.localXn.num;
    active.localGsaUsed = result.localGsa.num;
    active.remoteXnUsed = result.remoteXn.num;
    active.notifyCkeUsed = result.notifyCke.num;
    active.channelUsed = result.channels.num;
    active.localWaitCkeUsed = result.localWaitCke.num;
    active.remoteNotifyCkeUsed = result.remoteNotifyCke.num;
    active.sourceCkeUsed = result.sourceCke.num;
    active_[result.receiptId] = active;

    *plan = generated;
    *allocation = result;
    FillReport(result, request, report);
    return TILEXR_SUCCESS;
}

int TileXRCcuResourceAllocator::Release(uint64_t receiptId)
{
    const auto it = active_.find(receiptId);
    if (it == active_.end()) {
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    const auto latest = active_.rbegin();
    if (latest == active_.rend() || latest->first != receiptId) {
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    const ActiveAllocation& active = it->second;
    const uint32_t xnUsed = static_cast<uint32_t>(active.localXnUsed) +
        (remoteXn_.count == 0 ? active.remoteXnUsed : 0U);
    const uint32_t localCkeUsed =
        static_cast<uint32_t>(active.localWaitCkeUsed) + active.sourceCkeUsed;
    if (mission_.used < active.missionUsed ||
        repository_.used < active.repositoryUsed ||
        xn_.used < xnUsed ||
        gsa_.used < active.localGsaUsed ||
        localWaitCke_.used < localCkeUsed ||
        remoteNotifyCke_.used < active.remoteNotifyCkeUsed ||
        channel_.used < active.channelUsed ||
        (remoteXn_.count != 0 && remoteXn_.used < active.remoteXnUsed)) {
        return TILEXR_ERROR_INTERNAL;
    }
    mission_.used = static_cast<uint16_t>(mission_.used - active.missionUsed);
    repository_.used = static_cast<uint16_t>(repository_.used - active.repositoryUsed);
    xn_.used = static_cast<uint16_t>(xn_.used - xnUsed);
    if (remoteXn_.count != 0) {
        remoteXn_.used = static_cast<uint16_t>(remoteXn_.used - active.remoteXnUsed);
    }
    gsa_.used = static_cast<uint16_t>(gsa_.used - active.localGsaUsed);
    localWaitCke_.used = static_cast<uint16_t>(localWaitCke_.used - localCkeUsed);
    remoteNotifyCke_.used = static_cast<uint16_t>(remoteNotifyCke_.used - active.remoteNotifyCkeUsed);
    channel_.used = static_cast<uint16_t>(channel_.used - active.channelUsed);
    active_.erase(it);
    return TILEXR_SUCCESS;
}

} // namespace TileXR
