/*
 * Copyright (c) 2026 TileXR Project
 * Licensed under the Apache License, Version 2.0
 */

#ifndef TILEXR_CCU_LAUNCH_PACKAGE_H
#define TILEXR_CCU_LAUNCH_PACKAGE_H

#include "ccu/tilexr_ccu_repository.h"

#include <cstdint>
#include <string>
#include <vector>

namespace TileXR {

struct TileXRCcuLaunchInstallScope {
    uint32_t deviceId = 0;
    uint32_t rank = 0;
    uint64_t packageFingerprint = 0;
    std::string provider;
};

struct TileXRCcuLaunchPackage {
    TileXRCcuProducerPlan plan;
    TileXRCcuProgram program;
    TileXRCcuRepositoryImage repository;
    std::vector<TileXRCcuTask> tasks;
    TileXRCcuLaunchInstallScope installScope;
    bool requiresHardwareInstall = true;
};

struct TileXRCcuLaunchPackageReport {
    uint32_t taskCount = 0;
    uint32_t repositoryCount = 0;
    uint32_t installedInstructionCount = 0;
    std::string message;
};

int TileXRCcuBuildLaunchPackage(
    const TileXRCcuProducerPlan& plan,
    TileXRCcuLaunchPackage* package,
    TileXRCcuLaunchPackageReport* report);

uint64_t TileXRCcuComputeLaunchPackageFingerprint(const TileXRCcuLaunchPackage& package);

int TileXRCcuBindLaunchPackageInstallScope(
    TileXRCcuLaunchPackage* package,
    uint32_t deviceId,
    uint32_t rank,
    const std::string& provider);

} // namespace TileXR

#endif // TILEXR_CCU_LAUNCH_PACKAGE_H
