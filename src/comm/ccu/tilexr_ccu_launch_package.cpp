/*
 * Copyright (c) 2026 TileXR Project
 * Licensed under the Apache License, Version 2.0
 */

#include "ccu/tilexr_ccu_launch_package.h"

namespace TileXR {
namespace {

constexpr uint64_t TILEXR_CCU_FNV_OFFSET = 1469598103934665603ULL;
constexpr uint64_t TILEXR_CCU_FNV_PRIME = 1099511628211ULL;

void MixByte(uint8_t value, uint64_t* hash)
{
    *hash ^= value;
    *hash *= TILEXR_CCU_FNV_PRIME;
}

void MixU16(uint16_t value, uint64_t* hash)
{
    MixByte(static_cast<uint8_t>(value & 0xffU), hash);
    MixByte(static_cast<uint8_t>((value >> 8U) & 0xffU), hash);
}

void MixU32(uint32_t value, uint64_t* hash)
{
    MixU16(static_cast<uint16_t>(value & 0xffffU), hash);
    MixU16(static_cast<uint16_t>((value >> 16U) & 0xffffU), hash);
}

void MixU64(uint64_t value, uint64_t* hash)
{
    MixU32(static_cast<uint32_t>(value & 0xffffffffULL), hash);
    MixU32(static_cast<uint32_t>((value >> 32ULL) & 0xffffffffULL), hash);
}

void MixRange(const TileXRCcuRange& range, uint64_t* hash)
{
    MixByte(range.dieId, hash);
    MixU16(range.startId, hash);
    MixU16(range.num, hash);
}

void MixMission(const TileXRCcuMission& mission, uint64_t* hash)
{
    MixByte(mission.dieId, hash);
    MixByte(mission.missionId, hash);
    MixU32(mission.key, hash);
    MixByte(mission.installed ? 1U : 0U, hash);
}

void MixInstructionWindow(const TileXRCcuInstructionWindow& window, uint64_t* hash)
{
    MixByte(window.dieId, hash);
    MixU16(window.repositoryStartId, hash);
    MixU16(window.repositoryCount, hash);
    MixU16(window.missionStartId, hash);
    MixU16(window.missionCount, hash);
}

void MixSyncResource(const TileXRCcuSyncResource& resource, uint64_t* hash)
{
    MixByte(resource.dieId, hash);
    MixU16(resource.localXn, hash);
    MixU16(resource.remoteXn, hash);
    MixU16(resource.notifyCke, hash);
    MixU16(resource.channelId, hash);
    MixU16(resource.bindingCount, hash);
    MixU16(resource.localWaitCke, hash);
    MixU16(resource.localWaitMask, hash);
    MixU16(resource.remoteNotifyMask, hash);
    MixU16(resource.sourceCke, hash);
    MixU16(resource.sourceCkeMask, hash);
}

void MixTaskWindow(const TileXRCcuTaskWindow& window, uint64_t* hash)
{
    MixByte(window.dieId, hash);
    MixU16(window.instStartId, hash);
    MixU16(window.instCnt, hash);
    MixU32(window.argSize, hash);
    MixU64(static_cast<uint64_t>(window.args.size()), hash);
    for (uint64_t arg : window.args) {
        MixU64(arg, hash);
    }
}

void MixInstr(const TileXRCcuInstr& instr, uint64_t* hash)
{
    for (uint64_t word : instr.words) {
        MixU64(word, hash);
    }
}

void MixRepository(const TileXRCcuRepositoryImage& repository, uint64_t* hash)
{
    MixByte(repository.dieId, hash);
    MixU16(repository.repositoryStartId, hash);
    MixU16(repository.repositoryCount, hash);
    MixU16(repository.missionStartId, hash);
    MixU16(repository.missionCount, hash);
    MixU16(repository.missionOffset, hash);
    MixU16(repository.sqeLoadOffset, hash);
    MixU16(repository.sqeLoadCount, hash);
    MixU16(repository.syncOffset, hash);
    MixU16(repository.syncCount, hash);
    MixU64(static_cast<uint64_t>(repository.instructions.size()), hash);
    for (const auto& instr : repository.instructions) {
        MixInstr(instr, hash);
    }
}

void MixTask(const TileXRCcuTask& task, uint64_t* hash)
{
    MixByte(task.dieId, hash);
    MixByte(task.missionId, hash);
    MixU16(task.timeout, hash);
    MixU16(task.instStartId, hash);
    MixU16(task.instCnt, hash);
    MixU32(task.key, hash);
    MixU32(task.argSize, hash);
    for (uint64_t arg : task.args) {
        MixU64(arg, hash);
    }
}

void ResetReport(TileXRCcuLaunchPackageReport* report)
{
    if (report == nullptr) {
        return;
    }
    report->taskCount = 0;
    report->repositoryCount = 0;
    report->installedInstructionCount = 0;
    report->message.clear();
}

void ClearPackage(TileXRCcuLaunchPackage* package)
{
    if (package == nullptr) {
        return;
    }
    package->plan = TileXRCcuProducerPlan{};
    package->program = TileXRCcuProgram{};
    package->repository = TileXRCcuRepositoryImage{};
    package->tasks.clear();
    package->installScope = TileXRCcuLaunchInstallScope{};
    package->requiresHardwareInstall = true;
}

int Fail(TileXRCcuLaunchPackageReport* report, const std::string& message)
{
    if (report != nullptr) {
        report->message = message;
    }
    return TILEXR_ERROR_PARA_CHECK_FAIL;
}

void FillReport(const TileXRCcuLaunchPackage& package, TileXRCcuLaunchPackageReport* report)
{
    if (report == nullptr) {
        return;
    }
    report->taskCount = static_cast<uint32_t>(package.tasks.size());
    report->repositoryCount = package.repository.repositoryCount;
    report->installedInstructionCount =
        static_cast<uint32_t>(package.repository.sqeLoadCount + package.repository.syncCount);
    report->message = "ok";
}

} // namespace

int TileXRCcuBuildLaunchPackage(
    const TileXRCcuProducerPlan& plan,
    TileXRCcuLaunchPackage* package,
    TileXRCcuLaunchPackageReport* report)
{
    ResetReport(report);
    if (package == nullptr) {
        return Fail(report, "missing output CCU launch package");
    }
    ClearPackage(package);

    TileXRCcuProducerPlanReport planReport;
    TileXRCcuProgram program;
    if (TileXRCcuBuildMicrocode(plan, &program, &planReport) != TILEXR_SUCCESS) {
        ClearPackage(package);
        return Fail(report, planReport.message);
    }

    TileXRCcuRepositoryImage repository;
    TileXRCcuRepositoryReport repositoryReport;
    if (TileXRCcuBuildRepositoryImage(plan, program, &repository, &repositoryReport) != TILEXR_SUCCESS) {
        ClearPackage(package);
        return Fail(report, repositoryReport.message);
    }

    std::vector<TileXRCcuTask> tasks;
    if (TileXRCcuBuildTasks(plan, &tasks, &planReport) != TILEXR_SUCCESS) {
        ClearPackage(package);
        return Fail(report, planReport.message);
    }

    package->plan = plan;
    package->program = program;
    package->repository = repository;
    package->tasks = tasks;
    package->installScope = TileXRCcuLaunchInstallScope{};
    package->requiresHardwareInstall = true;
    FillReport(*package, report);
    return TILEXR_SUCCESS;
}

uint64_t TileXRCcuComputeLaunchPackageFingerprint(const TileXRCcuLaunchPackage& package)
{
    uint64_t hash = TILEXR_CCU_FNV_OFFSET;
    MixU64(0x54494c4558524343ULL, &hash); // "TILEXRCC"
    MixMission(package.plan.mission, &hash);
    MixRange(package.plan.kernelLocalXn, &hash);
    MixRange(package.plan.kernelLocalGsa, &hash);
    MixRange(package.plan.kernelLocalCke, &hash);
    MixRange(package.plan.kernelLocalMission, &hash);
    MixU32(static_cast<uint32_t>(package.plan.barrierMode), &hash);
    MixInstructionWindow(package.plan.instructionWindow, &hash);
    MixU64(static_cast<uint64_t>(package.plan.syncResources.size()), &hash);
    for (const auto& resource : package.plan.syncResources) {
        MixSyncResource(resource, &hash);
    }
    MixU64(static_cast<uint64_t>(package.plan.taskWindows.size()), &hash);
    for (const auto& window : package.plan.taskWindows) {
        MixTaskWindow(window, &hash);
    }
    MixU64(static_cast<uint64_t>(package.program.sqeLoad.size()), &hash);
    for (const auto& instr : package.program.sqeLoad) {
        MixInstr(instr, &hash);
    }
    MixU64(static_cast<uint64_t>(package.program.sync.size()), &hash);
    for (const auto& instr : package.program.sync) {
        MixInstr(instr, &hash);
    }
    MixRepository(package.repository, &hash);
    MixU64(static_cast<uint64_t>(package.tasks.size()), &hash);
    for (const auto& task : package.tasks) {
        MixTask(task, &hash);
    }
    MixByte(package.requiresHardwareInstall ? 1U : 0U, &hash);
    return hash == 0 ? 1 : hash;
}

int TileXRCcuBindLaunchPackageInstallScope(
    TileXRCcuLaunchPackage* package,
    uint32_t deviceId,
    uint32_t rank,
    const std::string& provider)
{
    if (package == nullptr || provider.empty()) {
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    if (package->tasks.empty() || package->repository.instructions.empty()) {
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    package->installScope.deviceId = deviceId;
    package->installScope.rank = rank;
    package->installScope.provider = provider;
    package->installScope.packageFingerprint = TileXRCcuComputeLaunchPackageFingerprint(*package);
    return TILEXR_SUCCESS;
}

} // namespace TileXR
