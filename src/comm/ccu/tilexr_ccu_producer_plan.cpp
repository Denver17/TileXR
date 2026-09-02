/*
 * Copyright (c) 2026 TileXR Project
 * Licensed under the Apache License, Version 2.0
 */

#include "ccu/tilexr_ccu_producer_plan.h"

#include <algorithm>
#include <limits>
#include <set>

namespace TileXR {
namespace {

constexpr uint32_t TILEXR_CCU_HCOMM_TASK1_PRELUDE_LOAD_ARG_COUNT = 2U;

void ResetReport(TileXRCcuProducerPlanReport* report)
{
    if (report == nullptr) {
        return;
    }
    report->syncResourceCount = 0;
    report->taskCount = 0;
    report->instructionCount = 0;
    report->message.clear();
}

int Fail(TileXRCcuProducerPlanReport* report, const std::string& message)
{
    if (report != nullptr) {
        report->message = message;
    }
    return TILEXR_ERROR_PARA_CHECK_FAIL;
}

bool ContainsId(const TileXRCcuRange& range, uint8_t dieId, uint16_t id)
{
    if (range.dieId != dieId || range.num == 0) {
        return false;
    }
    const uint32_t begin = range.startId;
    const uint32_t end = begin + range.num;
    return id >= begin && id < end;
}

bool InstructionWindowContains(const TileXRCcuInstructionWindow& window, uint8_t dieId, uint16_t start, uint16_t count)
{
    if (window.dieId != dieId || window.missionCount == 0 || count == 0) {
        return false;
    }
    const uint32_t begin = window.missionStartId;
    const uint32_t end = begin + window.missionCount;
    const uint32_t taskBegin = start;
    const uint32_t taskEnd = taskBegin + count;
    return taskBegin >= begin && taskEnd <= end;
}

uint16_t EffectiveLocalWaitCke(const TileXRCcuSyncResource& resource)
{
    return resource.localWaitCke == 0 ? resource.notifyCke : resource.localWaitCke;
}

uint16_t EffectiveRemoteNotifyMask(const TileXRCcuSyncResource& resource)
{
    return resource.remoteNotifyMask == 0 ? 1U : resource.remoteNotifyMask;
}

uint16_t EffectiveLocalWaitMask(const TileXRCcuSyncResource& resource)
{
    return resource.localWaitMask == 0 ? 1U : resource.localWaitMask;
}

uint16_t EffectiveSourceCkeMask(const TileXRCcuSyncResource& resource)
{
    return resource.sourceCkeMask == 0 ? 0xffffU : resource.sourceCkeMask;
}

bool SyncCkeMode(TileXRCcuBarrierMode mode)
{
    return mode == TileXRCcuBarrierMode::SyncCke ||
        mode == TileXRCcuBarrierMode::SyncCkeSetWait ||
        mode == TileXRCcuBarrierMode::SyncCkePostOnly;
}

bool SyncXnMode(TileXRCcuBarrierMode mode)
{
    return mode == TileXRCcuBarrierMode::SyncXn ||
        mode == TileXRCcuBarrierMode::SyncXnPostOnly ||
        mode == TileXRCcuBarrierMode::SyncXnLoadPostOnly;
}

bool RequiresHcommStyleTask1Prelude(const TileXRCcuProducerPlan& plan)
{
    return plan.taskWindows.size() > 1 && SyncXnMode(plan.barrierMode);
}

uint32_t SqeLoadXnOffset(uint32_t argId)
{
    return argId;
}

uint32_t HcommStyleTask1PreludeLoadXnOffset(uint32_t argId)
{
    return argId;
}

uint16_t HcommStylePreludeReserveXn(const TileXRCcuProducerPlan& plan)
{
    return static_cast<uint16_t>(
        static_cast<uint32_t>(plan.kernelLocalXn.startId) + TILEXR_CCU_SQE_ARGS_LEN);
}

bool HasKernelLocalGsa(const TileXRCcuProducerPlan& plan)
{
    return plan.kernelLocalGsa.dieId == plan.mission.dieId && plan.kernelLocalGsa.startId != 0 &&
        plan.kernelLocalGsa.num != 0;
}

int AppendSqeLoadProgram(
    const TileXRCcuProducerPlan& plan,
    uint32_t argCount,
    std::vector<TileXRCcuInstr>* program,
    TileXRCcuProducerPlanReport* report)
{
    if (program == nullptr || plan.kernelLocalXn.startId == 0 || argCount == 0 || argCount > TILEXR_CCU_SQE_ARGS_LEN) {
        return Fail(report, "invalid SQE argument load microcode request");
    }

    program->clear();
    program->reserve(argCount);
    for (uint32_t argId = 0; argId < argCount; ++argId) {
        const uint32_t xnId = static_cast<uint32_t>(plan.kernelLocalXn.startId) + SqeLoadXnOffset(argId);
        if (xnId > std::numeric_limits<uint16_t>::max() ||
            !ContainsId(plan.kernelLocalXn, plan.mission.dieId, static_cast<uint16_t>(xnId))) {
            program->clear();
            return Fail(report, "SQE argument load XN is outside the kernel-local XN repository range");
        }

        TileXRCcuInstr instr;
        if (TileXRCcuEncodeLoadSqeArgsToX(static_cast<uint16_t>(xnId), argId, &instr) != TILEXR_SUCCESS) {
            program->clear();
            return Fail(report, "failed to encode SQE argument load microcode");
        }
        program->push_back(instr);
    }
    return TILEXR_SUCCESS;
}

int ValidateMission(const TileXRCcuProducerPlan& plan, TileXRCcuProducerPlanReport* report)
{
    if (!plan.mission.installed || plan.mission.key == 0) {
        return Fail(report, "missing installed mission/key for CCU producer plan");
    }
    if (!ContainsId(plan.kernelLocalMission, plan.mission.dieId, plan.mission.missionId)) {
        return Fail(report, "mission id is outside the kernel-local mission repository range");
    }
    return TILEXR_SUCCESS;
}

int ValidateInstructionWindow(const TileXRCcuProducerPlan& plan, TileXRCcuProducerPlanReport* report)
{
    if (plan.instructionWindow.repositoryCount == 0 || plan.instructionWindow.missionCount == 0) {
        return Fail(report, "missing instruction repository window for CCU producer plan");
    }
    if (plan.instructionWindow.dieId != plan.mission.dieId) {
        return Fail(report, "instruction repository die does not match mission die");
    }
    const uint32_t repositoryEnd = plan.instructionWindow.repositoryStartId + plan.instructionWindow.repositoryCount;
    const uint32_t missionEnd = plan.instructionWindow.missionStartId + plan.instructionWindow.missionCount;
    if (plan.instructionWindow.missionStartId < plan.instructionWindow.repositoryStartId || missionEnd > repositoryEnd) {
        return Fail(report, "mission instruction window is outside the instruction repository range");
    }
    return TILEXR_SUCCESS;
}

int ValidateKernelLocalRepositories(const TileXRCcuProducerPlan& plan, TileXRCcuProducerPlanReport* report)
{
    const bool needsLocalXn = SyncXnMode(plan.barrierMode) || plan.taskWindows.size() > 1;
    if (needsLocalXn &&
        (plan.kernelLocalXn.dieId != plan.mission.dieId || plan.kernelLocalXn.num == 0)) {
        return Fail(report, "missing kernel-local XN repository range");
    }
    if (plan.kernelLocalCke.dieId != plan.mission.dieId || plan.kernelLocalCke.num == 0) {
        return Fail(report, "missing kernel-local CKE repository range");
    }
    return TILEXR_SUCCESS;
}

int ValidateSyncResources(const TileXRCcuProducerPlan& plan, TileXRCcuProducerPlanReport* report)
{
    if (plan.syncResources.empty()) {
        return Fail(report, "missing CCU sync resources");
    }
    std::set<uint16_t> channelIds;
    for (const auto& resource : plan.syncResources) {
        if (resource.dieId != plan.mission.dieId) {
            return Fail(report, "sync resource die does not match mission die");
        }
        if (resource.channelId == 0) {
            return Fail(report, "missing channel id for sync resource");
        }
        if (!channelIds.insert(resource.channelId).second) {
            return Fail(report, "duplicate channel id for sync resource");
        }
        if (SyncXnMode(plan.barrierMode)) {
            if (!ContainsId(plan.kernelLocalXn, resource.dieId, resource.localXn)) {
                return Fail(report, "local XN is outside the kernel-local XN repository range");
            }
            if (resource.remoteXn == 0) {
                return Fail(report, "missing channel-bound remote XN");
            }
        }
        if (resource.notifyCke == 0) {
            return Fail(report, "missing remote notify CKE resource");
        }
        if (EffectiveRemoteNotifyMask(resource) == 0) {
            return Fail(report, "missing remote notify CKE mask");
        }
        const uint16_t localWaitCke = EffectiveLocalWaitCke(resource);
        if (localWaitCke == 0) {
            return Fail(report, "missing local wait CKE resource");
        }
        if (EffectiveLocalWaitMask(resource) == 0) {
            return Fail(report, "missing local wait CKE mask");
        }
        if (resource.localWaitCke != 0 && !ContainsId(plan.kernelLocalCke, resource.dieId, localWaitCke)) {
            return Fail(report, "local wait CKE is outside the kernel-local CKE repository range");
        }
        if (SyncCkeMode(plan.barrierMode)) {
            if (resource.sourceCke == 0) {
                return Fail(report, "missing source CKE resource for SyncCKE barrier");
            }
            if (EffectiveSourceCkeMask(resource) == 0) {
                return Fail(report, "missing source CKE mask for SyncCKE barrier");
            }
            if (!ContainsId(plan.kernelLocalCke, resource.dieId, resource.sourceCke)) {
                return Fail(report, "source CKE is outside the kernel-local CKE repository range");
            }
        }
        if (resource.bindingCount == 0) {
            return Fail(report, "missing channel variable binding for sync resource");
        }
    }
    return TILEXR_SUCCESS;
}

int ValidateTasks(const TileXRCcuProducerPlan& plan, TileXRCcuProducerPlanReport* report)
{
    if (plan.taskWindows.empty()) {
        return Fail(report, "missing CCU task windows");
    }
    if (plan.taskWindows.size() > 1 &&
        plan.taskWindows.front().instCnt != TILEXR_CCU_SQE_ARGS_LEN) {
        return Fail(report, "SQE load task must contain the complete CCU SQE argument window");
    }
    for (const auto& window : plan.taskWindows) {
        if (window.dieId != plan.mission.dieId) {
            return Fail(report, "task die does not match mission die");
        }
        if (!InstructionWindowContains(plan.instructionWindow, window.dieId, window.instStartId, window.instCnt)) {
            return Fail(report, "task instruction range is outside the loaded mission instruction window");
        }
        if (window.argSize != 1 && window.argSize != TILEXR_CCU_SQE_ARGS_LEN) {
            return Fail(report, "task argSize must match a supported CCU SQE payload shape");
        }
        if (window.args.size() > TILEXR_CCU_SQE_ARGS_LEN) {
            return Fail(report, "task args exceed the CCU SQE payload capacity");
        }
    }
    return TILEXR_SUCCESS;
}

void FillReport(const TileXRCcuProducerPlan& plan, TileXRCcuProducerPlanReport* report)
{
    if (report == nullptr) {
        return;
    }
    report->syncResourceCount = static_cast<uint32_t>(plan.syncResources.size());
    report->taskCount = static_cast<uint32_t>(plan.taskWindows.size());
    report->instructionCount = plan.instructionWindow.repositoryCount;
    report->message = "ok";
}

int AppendHcommStyleTask1Prelude(
    const TileXRCcuProducerPlan& plan,
    std::vector<TileXRCcuInstr>* program,
    TileXRCcuProducerPlanReport* report)
{
    if (!RequiresHcommStyleTask1Prelude(plan)) {
        return TILEXR_SUCCESS;
    }

    const uint16_t reserveXn = HcommStylePreludeReserveXn(plan);
    if (!ContainsId(plan.kernelLocalXn, plan.mission.dieId, reserveXn)) {
        return Fail(report, "missing reserve XN for hcomm-style task1 prelude");
    }

    for (uint32_t argId = 0; argId < TILEXR_CCU_HCOMM_TASK1_PRELUDE_LOAD_ARG_COUNT; ++argId) {
        const uint32_t xnId =
            static_cast<uint32_t>(plan.kernelLocalXn.startId) + HcommStyleTask1PreludeLoadXnOffset(argId);
        if (xnId > std::numeric_limits<uint16_t>::max() ||
            !ContainsId(plan.kernelLocalXn, plan.mission.dieId, static_cast<uint16_t>(xnId))) {
            return Fail(report, "missing load-arg XN for hcomm-style task1 prelude");
        }
        TileXRCcuInstr instr;
        if (TileXRCcuEncodeLoadSqeArgsToX(static_cast<uint16_t>(xnId), argId, &instr) != TILEXR_SUCCESS) {
            return Fail(report, "failed to encode hcomm-style task1 load-arg prelude");
        }
        program->push_back(instr);
    }

    TileXRCcuInstr reserveXnInstr;
    if (TileXRCcuEncodeLoadImdToXn(reserveXn, 0, 0, &reserveXnInstr) != TILEXR_SUCCESS) {
        return Fail(report, "failed to encode hcomm-style task1 reserve XN prelude");
    }
    program->push_back(reserveXnInstr);

    TileXRCcuInstr reserveAddrInstr;
    if (HasKernelLocalGsa(plan)) {
        if (TileXRCcuEncodeLoadImdToGsa(plan.kernelLocalGsa.startId, 0, &reserveAddrInstr) != TILEXR_SUCCESS) {
            return Fail(report, "failed to encode hcomm-style task1 reserve GSA prelude");
        }
    } else if (TileXRCcuEncodeLoadImdToXn(reserveXn, 0, 0, &reserveAddrInstr) != TILEXR_SUCCESS) {
        return Fail(report, "failed to encode hcomm-style task1 nop prelude");
    }
    program->push_back(reserveAddrInstr);

    const TileXRCcuSyncResource& firstResource = plan.syncResources.front();
    TileXRCcuCkeSpec notifyInit;
    notifyInit.ckeId = firstResource.notifyCke;
    notifyInit.mask = EffectiveRemoteNotifyMask(firstResource);
    notifyInit.clearWait = false;
    TileXRCcuInstr notifyInstr;
    if (TileXRCcuEncodeSetCke(notifyInit, &notifyInstr) != TILEXR_SUCCESS) {
        return Fail(report, "failed to encode hcomm-style task1 notify CKE prelude");
    }
    program->push_back(notifyInstr);
    return TILEXR_SUCCESS;
}

} // namespace

int TileXRCcuValidateProducerPlan(const TileXRCcuProducerPlan& plan, TileXRCcuProducerPlanReport* report)
{
    ResetReport(report);

    int ret = ValidateMission(plan, report);
    if (ret != TILEXR_SUCCESS) {
        return ret;
    }
    ret = ValidateInstructionWindow(plan, report);
    if (ret != TILEXR_SUCCESS) {
        return ret;
    }
    ret = ValidateKernelLocalRepositories(plan, report);
    if (ret != TILEXR_SUCCESS) {
        return ret;
    }
    ret = ValidateSyncResources(plan, report);
    if (ret != TILEXR_SUCCESS) {
        return ret;
    }
    ret = ValidateTasks(plan, report);
    if (ret != TILEXR_SUCCESS) {
        return ret;
    }

    FillReport(plan, report);
    return TILEXR_SUCCESS;
}

int TileXRCcuBuildTasks(
    const TileXRCcuProducerPlan& plan,
    std::vector<TileXRCcuTask>* tasks,
    TileXRCcuProducerPlanReport* report)
{
    if (tasks == nullptr) {
        ResetReport(report);
        return Fail(report, "missing output task vector");
    }

    int ret = TileXRCcuValidateProducerPlan(plan, report);
    if (ret != TILEXR_SUCCESS) {
        return ret;
    }

    tasks->clear();
    tasks->reserve(plan.taskWindows.size());
    for (const auto& window : plan.taskWindows) {
        TileXRCcuTask task;
        task.dieId = window.dieId;
        task.missionId = plan.mission.missionId;
        task.timeout = TILEXR_CCU_DEFAULT_TASK_TIMEOUT_SEC;
        task.instStartId = window.instStartId;
        task.instCnt = window.instCnt;
        task.key = plan.mission.key;
        task.argSize = window.argSize;
        std::copy(window.args.begin(), window.args.end(), task.args);
        tasks->push_back(task);
    }

    return TILEXR_SUCCESS;
}

int TileXRCcuBuildMicrocode(
    const TileXRCcuProducerPlan& plan,
    TileXRCcuProgram* program,
    TileXRCcuProducerPlanReport* report)
{
    if (program == nullptr) {
        ResetReport(report);
        return Fail(report, "missing output CCU program");
    }

    int ret = TileXRCcuValidateProducerPlan(plan, report);
    if (ret != TILEXR_SUCCESS) {
        return ret;
    }

    program->sqeLoad.clear();
    program->sync.clear();
    if (plan.taskWindows.size() > 1) {
        if (AppendSqeLoadProgram(plan, plan.taskWindows.front().instCnt, &program->sqeLoad, report) != TILEXR_SUCCESS) {
            return TILEXR_ERROR_PARA_CHECK_FAIL;
        }
    }

    std::vector<TileXRCcuBarrierSyncSpec> barrierSpecs;
    barrierSpecs.reserve(plan.syncResources.size());
    for (const auto& resource : plan.syncResources) {
        TileXRCcuBarrierSyncSpec spec;
        spec.remoteXn = resource.remoteXn;
        spec.localXn = resource.localXn;
        spec.channelId = resource.channelId;
        spec.remoteNotifyCke = resource.notifyCke;
        spec.remoteNotifyMask = EffectiveRemoteNotifyMask(resource);
        spec.localWaitCke = EffectiveLocalWaitCke(resource);
        spec.localWaitMask = EffectiveLocalWaitMask(resource);
        spec.sourceCke = resource.sourceCke;
        spec.sourceCkeMask = EffectiveSourceCkeMask(resource);
        spec.clearLocalWait = true;
        barrierSpecs.push_back(spec);
    }

    TileXRCcuBarrierProgramReport barrierReport;
    if (AppendHcommStyleTask1Prelude(plan, &program->sync, report) != TILEXR_SUCCESS) {
        program->sqeLoad.clear();
        program->sync.clear();
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }

    std::vector<TileXRCcuInstr> barrierProgram;
    if (TileXRCcuBuildBarrierProgram(barrierSpecs, &barrierProgram, &barrierReport, plan.barrierMode) !=
        TILEXR_SUCCESS) {
        program->sqeLoad.clear();
        program->sync.clear();
        return Fail(report, barrierReport.message.empty() ? "failed to build sync microcode" : barrierReport.message);
    }
    program->sync.insert(program->sync.end(), barrierProgram.begin(), barrierProgram.end());

    return TILEXR_SUCCESS;
}

} // namespace TileXR
