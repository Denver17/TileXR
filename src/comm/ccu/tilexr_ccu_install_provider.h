/*
 * Copyright (c) 2026 TileXR Project
 * Licensed under the Apache License, Version 2.0
 */

#ifndef TILEXR_CCU_INSTALL_PROVIDER_H
#define TILEXR_CCU_INSTALL_PROVIDER_H

#include "ccu/tilexr_ccu_provider.h"

#include <cstdint>
#include <string>
#include <vector>

namespace TileXR {

enum class TileXRCcuInstallRequirementKind : uint8_t {
    MissionKey = 0,
    RepositoryImage = 1,
    LocalXn = 2,
    RemoteXnBinding = 3,
    NotifyCke = 4,
    ChannelBinding = 5,
};

struct TileXRCcuInstallRequirement {
    TileXRCcuInstallRequirementKind kind = TileXRCcuInstallRequirementKind::MissionKey;
    uint64_t packageFingerprint = 0;
    uint8_t dieId = 0;
    uint8_t missionId = 0;
    uint32_t missionKey = 0;
    uint16_t resourceStartId = 0;
    uint16_t resourceCount = 0;
    uint16_t repositoryStartId = 0;
    uint16_t repositoryCount = 0;
    uint16_t missionStartId = 0;
    uint16_t missionCount = 0;
    uint32_t instructionCount = 0;
    uint32_t syncResourceCount = 0;
    uint32_t bindingCount = 0;
    std::string label;
    std::string detail;
};

struct TileXRCcuInstallManifest {
    uint64_t packageFingerprint = 0;
    uint32_t deviceId = 0;
    uint32_t rank = 0;
    std::string provider;
    bool requiresHardwareInstall = true;
    bool installAttemptReceiptRequired = true;
    TileXRCcuEvidenceKind requiredEvidenceKind = TileXRCcuEvidenceKind::PublicVerified;
    TileXRCcuEvidenceSurface requiredEvidenceSurface = TileXRCcuEvidenceSurface::PublicInstallProvider;
    std::vector<TileXRCcuInstallRequirement> requirements;
};

struct TileXRCcuInstallManifestReport {
    uint32_t requirementCount = 0;
    std::string message;
};

struct TileXRCcuMsidTokenInstall {
    uint8_t dieId = 0;
    uint32_t msId = 0;
    uint32_t tokenId = 0;
    uint32_t tokenValue = 0;
};

struct TileXRCcuPfeInstall {
    uint8_t dieId = 0;
    uint32_t pfeOffset = 0;
    TileXRCcuPfeCtx ctx;
};

struct TileXRCcuJettyInstall {
    uint8_t dieId = 0;
    uint16_t startJettyCtxId = 0;
    std::vector<TileXRCcuLocalJettyCtxData> ctxs;
};

struct TileXRCcuChannelInstall {
    uint8_t dieId = 0;
    uint32_t channelId = 0;
    TileXRCcuChannelCtxDataV1 ctx;
};

struct TileXRCcuCkeClearInstall {
    uint8_t dieId = 0;
    uint32_t startCkeId = 0;
    uint32_t count = 0;
};

struct TileXRCcuXnClearInstall {
    uint8_t dieId = 0;
    uint32_t startXnId = 0;
    uint32_t count = 0;
};

struct TileXRCcuRemoteXnBindingProof {
    uint8_t dieId = 0;
    uint16_t channelId = 0;
    uint16_t localXn = 0;
    uint16_t remoteXn = 0;
    uint16_t notifyCke = 0;
    uint32_t peerRank = 0;
    bool peerExchangeObserved = false;
    uint16_t localWaitCke = 0;
    bool endpointRouteVerified = false;
    bool channelResourceOwnerVerified = false;
    bool transportResourceExchangeVerified = false;
};

struct TileXRCcuLowerLayerInstallPlan {
    std::vector<TileXRCcuMsidTokenInstall> msidTokens;
    std::vector<TileXRCcuPfeInstall> pfes;
    std::vector<TileXRCcuJettyInstall> jettys;
    std::vector<TileXRCcuChannelInstall> channels;
    std::vector<TileXRCcuXnClearInstall> xnClears;
    std::vector<TileXRCcuCkeClearInstall> ckeClears;
    std::vector<TileXRCcuRemoteXnBindingProof> remoteXnBindings;
};

enum class TileXRCcuInstallOrder : uint8_t {
    RepositoryFirst = 0,
    InstallLowerLayerFirst = 1,
};

struct TileXRCcuInstallRequest {
    const TileXRCcuLaunchPackage* package = nullptr;
    const TileXRCcuInstallManifest* manifest = nullptr;
    uint32_t deviceId = 0;
    uint32_t rank = 0;
    std::string provider;
    bool offlineOnly = true;
    const TileXRCcuDriverAdapter* driverAdapter = nullptr;
    TileXRCcuDeviceMemoryOps repositoryMemoryOps;
    void* repositoryMemoryUserData = nullptr;
    TileXRCcuRepositoryInstallOptions repositoryInstallOptions;
    TileXRCcuRepositoryInstallReceipt* repositoryReceipt = nullptr;
    TileXRCcuInstallOrder installOrder = TileXRCcuInstallOrder::RepositoryFirst;
    const TileXRCcuLowerLayerInstallPlan* lowerLayerPlan = nullptr;
};

struct TileXRCcuInstallStepEvidence {
    bool satisfied = false;
    TileXRCcuEvidenceSource source;
    std::string message;
};

struct TileXRCcuInstallProviderReport {
    TileXRCcuInstallStepEvidence mission;
    TileXRCcuInstallStepEvidence repository;
    TileXRCcuInstallStepEvidence localXn;
    TileXRCcuInstallStepEvidence remoteXn;
    TileXRCcuInstallStepEvidence notifyCke;
    TileXRCcuInstallStepEvidence channelBinding;
    bool offlineOnly = true;
    bool installAttempted = false;
    bool installSucceeded = false;
    uint32_t requiredInstallSurfaceCount = 0;
    uint32_t publicVerifiedInstallSurfaceCount = 0;
    uint32_t missingInstallSurfaceCount = 0;
    uint64_t installAttemptReceiptId = 0;
    std::string message;
};

int TileXRCcuBuildInstallEvidence(
    const TileXRCcuInstallProviderReport& installReport,
    TileXRCcuHardwareInstallEvidence* evidence);

int TileXRCcuBuildInstallManifest(
    const TileXRCcuLaunchPackage& package,
    TileXRCcuInstallManifest* manifest,
    TileXRCcuInstallManifestReport* report);

int TileXRCcuInstallHardware(
    const TileXRCcuInstallRequest& request,
    TileXRCcuHardwareInstallEvidence* evidence,
    TileXRCcuInstallProviderReport* report);

} // namespace TileXR

#endif // TILEXR_CCU_INSTALL_PROVIDER_H
