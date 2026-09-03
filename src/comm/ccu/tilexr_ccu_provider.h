/*
 * Copyright (c) 2026 TileXR Project
 * Licensed under the Apache License, Version 2.0
 */

#ifndef TILEXR_CCU_PROVIDER_H
#define TILEXR_CCU_PROVIDER_H

#include "ccu/tilexr_ccu_launch_package.h"

#include <cstdint>
#include <string>
#include <vector>

namespace TileXR {

enum class TileXRCcuEvidenceKind : uint8_t {
    LegacyBoolean = 0,
    PublicVerified = 1,
    PublicCandidate = 2,
    PrivateObserved = 3,
    Missing = 4,
};

enum class TileXRCcuEvidenceSurface : uint8_t {
    Unspecified = 0,
    PublicInstallProvider = 1,
    LowerLayerResourceHelper = 2,
    PrivateProducerObservation = 3,
};

struct TileXRCcuEvidenceSource {
    TileXRCcuEvidenceKind kind = TileXRCcuEvidenceKind::LegacyBoolean;
    TileXRCcuEvidenceSurface surface = TileXRCcuEvidenceSurface::Unspecified;
    uint64_t packageFingerprint = 0;
    uint32_t deviceId = 0;
    uint32_t rank = 0;
    std::string provider;
    uint64_t installAttemptReceiptId = 0;
    bool endpointRouteVerified = false;
    std::string source;
    std::string detail;
};

struct TileXRCcuHardwareInstallEvidence {
    bool missionInstalled = false;
    bool repositoryInstalled = false;
    bool localXnInstalled = false;
    bool remoteXnBound = false;
    bool notifyCkeInstalled = false;
    bool channelBindingsInstalled = false;
    TileXRCcuEvidenceSource missionSource;
    TileXRCcuEvidenceSource repositorySource;
    TileXRCcuEvidenceSource localXnSource;
    TileXRCcuEvidenceSource remoteXnSource;
    TileXRCcuEvidenceSource notifyCkeSource;
    TileXRCcuEvidenceSource channelBindingSource;
};

struct TileXRCcuProviderReport {
    uint32_t taskCount = 0;
    uint32_t repositoryCount = 0;
    uint32_t installedInstructionCount = 0;
    uint32_t evidenceBitCount = 0;
    uint32_t publicVerifiedEvidenceCount = 0;
    uint32_t legacyEvidenceCount = 0;
    uint32_t publicCandidateEvidenceCount = 0;
    uint32_t privateObservedEvidenceCount = 0;
    uint32_t missingEvidenceCount = 0;
    bool submitReady = false;
    std::string message;
};

int TileXRCcuValidateHardwareInstall(
    const TileXRCcuLaunchPackage& package,
    const TileXRCcuHardwareInstallEvidence& evidence,
    TileXRCcuProviderReport* report);

int TileXRCcuPrepareSubmitTasks(
    const TileXRCcuLaunchPackage& package,
    const TileXRCcuHardwareInstallEvidence& evidence,
    std::vector<TileXRCcuTask>* submitTasks,
    TileXRCcuProviderReport* report);

} // namespace TileXR

#endif // TILEXR_CCU_PROVIDER_H
