/*
 * Copyright (c) 2026 TileXR Project
 * Licensed under the Apache License, Version 2.0
 */

#ifndef TILEXR_CCU_DIRECT_ORCHESTRATOR_H
#define TILEXR_CCU_DIRECT_ORCHESTRATOR_H

#include "ccu/tilexr_ccu_install_provider.h"
#include "ccu/tilexr_ccu_lower_layer_plan_builder.h"
#include "ccu/tilexr_ccu_memory_program.h"
#include "ccu/tilexr_ccu_specs.h"

#include <cstdint>
#include <string>
#include <vector>

namespace TileXR {

using TileXRCcuLowerLayerPlanPrepareFn = int (*)(
    const TileXRCcuResourceAllocation& allocation,
    TileXRCcuLowerLayerInstallPlan* plan,
    TileXRCcuLowerLayerPlanBuilderReport* report,
    void* userData);

struct TileXRCcuDirectInstallOptions {
    const TileXRCcuBasicInfo* basicInfo = nullptr;
    uint32_t sqeArgCount = 0;
    uint32_t syncResourceCount = 0;
    uint32_t syncInstructionCount = 0;
    uint32_t bindingsPerSyncResource = 1;
    uint16_t missionStartId = 0;
    uint16_t instructionStartId = 0;
    uint16_t missionInstructionStartId = 0;
    uint16_t xnStartId = 0;
    uint16_t gsaStartId = 0;
    uint16_t remoteXnStartId = 0;
    uint16_t remoteXnCount = 0;
    uint16_t ckeStartId = 0;
    uint16_t channelStartId = 0;
    uint16_t localWaitCkeStartId = 0;
    uint16_t localWaitCkeCount = 0;
    uint16_t remoteNotifyCkeStartId = 0;
    uint16_t remoteNotifyCkeCount = 0;
    TileXRCcuBarrierMode barrierMode = TileXRCcuBarrierMode::SyncXn;
    uint32_t deviceId = 0;
    uint32_t rank = 0;
    std::string provider;
    bool offlineOnly = false;
    const TileXRCcuDriverAdapter* driverAdapter = nullptr;
    TileXRCcuDeviceMemoryOps repositoryMemoryOps;
    void* repositoryMemoryUserData = nullptr;
    TileXRCcuRepositoryInstallOptions repositoryInstallOptions;
    TileXRCcuRepositoryMemoryAllocMode repositoryMemoryAllocMode = TileXRCcuRepositoryMemoryAllocMode::Acl;
    TileXRCcuInstallOrder installOrder = TileXRCcuInstallOrder::InstallLowerLayerFirst;
    const TileXRCcuLowerLayerInstallPlan* lowerLayerPlan = nullptr;
    TileXRCcuLowerLayerPlanPrepareFn prepareLowerLayerPlan = nullptr;
    void* lowerLayerPlanUserData = nullptr;
};

struct TileXRCcuDirectMemoryCopySpec {
    TileXRCcuMemoryCopyDirection direction = TileXRCcuMemoryCopyDirection::RemoteToLocal;
    uint64_t localAddr = 0;
    uint64_t localToken = 0;
    uint64_t remoteAddr = 0;
    uint64_t remoteToken = 0;
    uint64_t lengthBytes = 0;
};

struct TileXRCcuDirectInstallAttempt {
    TileXRCcuSpecInfo specInfo;
    TileXRCcuResourceSpec resourceSpec;
    TileXRCcuResourceRequest resourceRequest;
    TileXRCcuResourceAllocation allocation;
    TileXRCcuProducerPlan plan;
    TileXRCcuLaunchPackage package;
    TileXRCcuInstallManifest manifest;
    TileXRCcuHardwareInstallEvidence evidence;
    TileXRCcuInstallProviderReport installReport;
    TileXRCcuProviderReport providerReport;
    TileXRCcuRepositoryInstallReceipt repositoryReceipt;
    TileXRCcuRepositoryReport repositoryReleaseReport;
    TileXRCcuDeviceMemoryOps repositoryMemoryOps;
    void* repositoryMemoryUserData = nullptr;
    TileXRCcuLowerLayerInstallPlan preparedLowerLayerPlan;
    TileXRCcuLowerLayerPlanBuilderReport lowerLayerPlanReport;
    std::vector<TileXRCcuTask> submitTasks;
};

struct TileXRCcuDirectInstallReport {
    bool pipelineBuilt = false;
    bool installAttempted = false;
    bool installSucceeded = false;
    bool submitReady = false;
    uint32_t requiredInstallSurfaceCount = 0;
    uint32_t publicVerifiedInstallSurfaceCount = 0;
    uint32_t missingInstallSurfaceCount = 0;
    uint32_t taskCount = 0;
    uint32_t submitTaskCount = 0;
    std::string message;
};

struct TileXRCcuDirectSubmitReport {
    bool submitted = false;
    uint32_t taskCount = 0;
    uint32_t submittedTaskCount = 0;
    std::string message;
};

using TileXRCcuTaskSubmitFn = int (*)(
    const TileXRCcuTask& task,
    void* stream,
    void* userData);

int TileXRCcuRunDirectInstallAttempt(
    const TileXRCcuDirectInstallOptions& options,
    TileXRCcuDirectInstallAttempt* attempt,
    TileXRCcuDirectInstallReport* report);

int TileXRCcuRunDirectMemoryCopyInstallAttempt(
    const TileXRCcuDirectInstallOptions& options,
    const TileXRCcuDirectMemoryCopySpec& memoryCopy,
    TileXRCcuDirectInstallAttempt* attempt,
    TileXRCcuDirectInstallReport* report);

int TileXRCcuReleaseDirectInstallAttemptResources(TileXRCcuDirectInstallAttempt& attempt);

int TileXRCcuSubmitPreparedTasks(
    const std::vector<TileXRCcuTask>& submitTasks,
    void* stream,
    TileXRCcuTaskSubmitFn submitFn,
    void* submitUserData,
    TileXRCcuDirectSubmitReport* report);

} // namespace TileXR

#endif // TILEXR_CCU_DIRECT_ORCHESTRATOR_H
