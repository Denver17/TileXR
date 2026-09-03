/*
 * Copyright (c) 2026 TileXR Project
 * Licensed under the Apache License, Version 2.0
 */

#include "ccu/tilexr_ccu_install_provider.h"

#include <sstream>

namespace TileXR {
namespace {

void ResetReport(TileXRCcuInstallProviderReport* report)
{
    if (report == nullptr) {
        return;
    }
    *report = TileXRCcuInstallProviderReport{};
}

void ResetManifestReport(TileXRCcuInstallManifestReport* report)
{
    if (report == nullptr) {
        return;
    }
    report->requirementCount = 0;
    report->message.clear();
}

int FailManifest(TileXRCcuInstallManifest* manifest, TileXRCcuInstallManifestReport* report, const std::string& message)
{
    if (manifest != nullptr) {
        *manifest = TileXRCcuInstallManifest{};
    }
    if (report != nullptr) {
        report->requirementCount = 0;
        report->message = message;
    }
    return TILEXR_ERROR_PARA_CHECK_FAIL;
}

void MarkMissingStep(TileXRCcuInstallStepEvidence* step, const std::string& message)
{
    if (step == nullptr) {
        return;
    }
    step->satisfied = false;
    step->source.kind = TileXRCcuEvidenceKind::Missing;
    step->source.surface = TileXRCcuEvidenceSurface::Unspecified;
    step->source.source.clear();
    step->source.detail = message;
    step->message = message;
}

void FillUnsupportedReport(TileXRCcuInstallProviderReport* report)
{
    if (report == nullptr) {
        return;
    }
    const std::string message = "no public no-hcomm CCU install provider is available";
    report->offlineOnly = true;
    report->installAttempted = false;
    report->installSucceeded = false;
    report->message = message;
    MarkMissingStep(&report->mission, message);
    MarkMissingStep(&report->repository, message);
    MarkMissingStep(&report->localXn, message);
    MarkMissingStep(&report->remoteXn, message);
    MarkMissingStep(&report->notifyCke, message);
    MarkMissingStep(&report->channelBinding, message);
}

void FillScopeFailureReport(TileXRCcuInstallProviderReport* report, const std::string& message)
{
    if (report == nullptr) {
        return;
    }
    report->offlineOnly = true;
    report->installAttempted = false;
    report->installSucceeded = false;
    report->message = message;
    MarkMissingStep(&report->mission, message);
    MarkMissingStep(&report->repository, message);
    MarkMissingStep(&report->localXn, message);
    MarkMissingStep(&report->remoteXn, message);
    MarkMissingStep(&report->notifyCke, message);
    MarkMissingStep(&report->channelBinding, message);
}

TileXRCcuEvidenceSource MissingSource(const std::string& detail)
{
    TileXRCcuEvidenceSource source;
    source.kind = TileXRCcuEvidenceKind::Missing;
    source.surface = TileXRCcuEvidenceSurface::Unspecified;
    source.detail = detail;
    return source;
}

TileXRCcuEvidenceSource SourceOrMissing(const TileXRCcuInstallStepEvidence& step, const std::string& detail)
{
    return step.satisfied ? step.source : MissingSource(detail);
}

uint64_t MixReceiptWord(uint64_t hash, uint64_t value)
{
    constexpr uint64_t prime = 1099511628211ULL;
    for (uint32_t i = 0; i < 8U; ++i) {
        hash ^= static_cast<uint8_t>((value >> (i * 8U)) & 0xffU);
        hash *= prime;
    }
    return hash;
}

uint64_t BuildRepositoryInstallReceiptId(
    const TileXRCcuLaunchPackage& package,
    const TileXRCcuRepositoryInstallReceipt& receipt)
{
    uint64_t hash = 1469598103934665603ULL;
    hash = MixReceiptWord(hash, TileXRCcuComputeLaunchPackageFingerprint(package));
    hash = MixReceiptWord(hash, receipt.dieId);
    hash = MixReceiptWord(hash, receipt.instructionStartId);
    hash = MixReceiptWord(hash, receipt.instructionCount);
    hash = MixReceiptWord(hash, receipt.instructionBytes);
    hash = MixReceiptWord(hash, receipt.deviceInstructionAddr);
    return hash == 0 ? 1 : hash;
}

TileXRCcuInstallStepEvidence PublicVerifiedStep(
    const TileXRCcuLaunchPackage& package,
    const TileXRCcuInstallRequest& request,
    uint64_t receiptId,
    const std::string& source,
    const std::string& detail,
    bool endpointRouteVerified = false)
{
    TileXRCcuInstallStepEvidence step;
    step.satisfied = true;
    step.source.kind = TileXRCcuEvidenceKind::PublicVerified;
    step.source.surface = TileXRCcuEvidenceSurface::PublicInstallProvider;
    step.source.packageFingerprint = TileXRCcuComputeLaunchPackageFingerprint(package);
    step.source.deviceId = request.deviceId;
    step.source.rank = request.rank;
    step.source.provider = request.provider;
    step.source.installAttemptReceiptId = receiptId;
    step.source.endpointRouteVerified = endpointRouteVerified;
    step.source.source = source;
    step.source.detail = detail;
    step.message = detail;
    return step;
}

TileXRCcuInstallRequirement BaseRequirement(
    TileXRCcuInstallRequirementKind kind,
    uint64_t packageFingerprint,
    uint8_t dieId,
    const std::string& label,
    const std::string& detail)
{
    TileXRCcuInstallRequirement requirement;
    requirement.kind = kind;
    requirement.packageFingerprint = packageFingerprint;
    requirement.dieId = dieId;
    requirement.label = label;
    requirement.detail = detail;
    return requirement;
}

uint32_t TotalBindingCount(const std::vector<TileXRCcuSyncResource>& resources)
{
    uint32_t total = 0;
    for (const auto& resource : resources) {
        total += resource.bindingCount;
    }
    return total;
}

bool SameBindingChannel(
    const TileXRCcuRemoteXnBindingProof& lhs,
    const TileXRCcuRemoteXnBindingProof& rhs)
{
    return lhs.dieId == rhs.dieId && lhs.channelId == rhs.channelId;
}

bool HasInstalledChannel(
    const std::vector<TileXRCcuChannelInstall>& channels,
    uint8_t dieId,
    uint16_t channelId)
{
    for (const auto& channel : channels) {
        if (channel.dieId == dieId && channel.channelId == channelId) {
            return true;
        }
    }
    return false;
}

uint32_t CountUniqueRemoteBindingChannels(const std::vector<TileXRCcuRemoteXnBindingProof>& bindings)
{
    uint32_t count = 0;
    for (size_t i = 0; i < bindings.size(); ++i) {
        bool seen = false;
        for (size_t j = 0; j < i; ++j) {
            if (SameBindingChannel(bindings[i], bindings[j])) {
                seen = true;
                break;
            }
        }
        if (!seen) {
            ++count;
        }
    }
    return count;
}

uint32_t CountInstalledRemoteBindingChannels(
    const std::vector<TileXRCcuRemoteXnBindingProof>& bindings,
    const std::vector<TileXRCcuChannelInstall>& channels)
{
    uint32_t count = 0;
    for (size_t i = 0; i < bindings.size(); ++i) {
        bool seen = false;
        for (size_t j = 0; j < i; ++j) {
            if (SameBindingChannel(bindings[i], bindings[j])) {
                seen = true;
                break;
            }
        }
        if (!seen && HasInstalledChannel(channels, bindings[i].dieId, bindings[i].channelId)) {
            ++count;
        }
    }
    return count;
}

const TileXRCcuInstallRequirement* FindRequirement(
    const TileXRCcuInstallManifest& manifest,
    TileXRCcuInstallRequirementKind kind);

bool RangeContainsId(const TileXRCcuRange& range, uint8_t dieId, uint16_t id)
{
    if (range.dieId != dieId || range.num == 0) {
        return false;
    }
    const uint32_t begin = range.startId;
    const uint32_t end = begin + range.num;
    return id >= begin && id < end;
}

bool RepositoryMissionWindowContainsTask(const TileXRCcuRepositoryImage& repository, const TileXRCcuTask& task)
{
    if (repository.dieId != task.dieId || repository.missionCount == 0 || task.instCnt == 0) {
        return false;
    }
    const uint32_t begin = repository.missionStartId;
    const uint32_t end = begin + repository.missionCount;
    const uint32_t taskBegin = task.instStartId;
    const uint32_t taskEnd = taskBegin + task.instCnt;
    return taskBegin >= begin && taskEnd <= end;
}

bool FailMissionLaunchDescriptorProof(std::string* diagnostic, const std::string& reason)
{
    if (diagnostic != nullptr) {
        *diagnostic = "mission/key launch task descriptor proof failed: " + reason;
    }
    return false;
}

bool ValidateMissionLaunchDescriptorProof(
    const TileXRCcuLaunchPackage& package,
    const TileXRCcuInstallManifest* manifest,
    std::string* diagnostic)
{
    if (diagnostic != nullptr) {
        diagnostic->clear();
    }
    if (!package.plan.mission.installed || package.plan.mission.key == 0) {
        return FailMissionLaunchDescriptorProof(diagnostic, "mission key is not installed in the producer plan");
    }
    if (!RangeContainsId(
            package.plan.kernelLocalMission,
            package.plan.mission.dieId,
            package.plan.mission.missionId)) {
        return FailMissionLaunchDescriptorProof(
            diagnostic,
            "mission id is outside the kernel-local mission range");
    }
    if (manifest == nullptr) {
        return FailMissionLaunchDescriptorProof(diagnostic, "install manifest mission requirement is missing");
    }
    const TileXRCcuInstallRequirement* mission =
        FindRequirement(*manifest, TileXRCcuInstallRequirementKind::MissionKey);
    const uint64_t packageFingerprint = TileXRCcuComputeLaunchPackageFingerprint(package);
    if (mission == nullptr ||
        mission->packageFingerprint != packageFingerprint ||
        mission->dieId != package.plan.mission.dieId ||
        mission->missionId != package.plan.mission.missionId ||
        mission->missionKey != package.plan.mission.key ||
        mission->resourceStartId != package.plan.kernelLocalMission.startId ||
        mission->resourceCount != package.plan.kernelLocalMission.num) {
        return FailMissionLaunchDescriptorProof(
            diagnostic,
            "install manifest mission requirement does not match the launch package");
    }
    if (package.repository.instructions.empty() ||
        package.repository.dieId != package.plan.mission.dieId ||
        package.repository.missionCount == 0) {
        return FailMissionLaunchDescriptorProof(diagnostic, "repository mission instruction window is missing");
    }
    if (package.tasks.empty()) {
        return FailMissionLaunchDescriptorProof(diagnostic, "launch task descriptor list is missing");
    }
    for (const auto& task : package.tasks) {
        if (task.dieId != package.plan.mission.dieId ||
            task.missionId != package.plan.mission.missionId ||
            task.key != package.plan.mission.key) {
            return FailMissionLaunchDescriptorProof(
                diagnostic,
                "launch task descriptor mission id or mission key does not match the producer plan");
        }
        if (!RepositoryMissionWindowContainsTask(package.repository, task)) {
            return FailMissionLaunchDescriptorProof(
                diagnostic,
                "launch task descriptor instruction range is outside the repository mission window");
        }
    }
    if (diagnostic != nullptr) {
        *diagnostic =
            "mission/key carried by launch task descriptor mission id and mission key within repository window";
    }
    return true;
}

bool FailRemoteXnExchangeBindingProof(std::string* diagnostic, const std::string& reason)
{
    if (diagnostic != nullptr) {
        *diagnostic = "remote XN peer exchange proof failed: " + reason;
    }
    return false;
}

bool HasInstalledChannelRoute(
    const TileXRCcuLowerLayerInstallPlan& lowerLayerPlan,
    uint8_t dieId,
    uint16_t channelId)
{
    for (const auto& channel : lowerLayerPlan.channels) {
        if (channel.dieId == dieId && channel.channelId == channelId) {
            return true;
        }
    }
    return false;
}

bool HasVerifiedEndpointRoutes(const TileXRCcuLowerLayerInstallPlan* lowerLayerPlan)
{
    if (lowerLayerPlan == nullptr || lowerLayerPlan->remoteXnBindings.empty()) {
        return false;
    }
    for (const auto& proof : lowerLayerPlan->remoteXnBindings) {
        if (!proof.peerExchangeObserved || !proof.endpointRouteVerified ||
            !HasInstalledChannelRoute(*lowerLayerPlan, proof.dieId, proof.channelId)) {
            return false;
        }
    }
    return true;
}

bool HasVerifiedChannelResourceBindings(const TileXRCcuLowerLayerInstallPlan* lowerLayerPlan)
{
    if (lowerLayerPlan == nullptr || lowerLayerPlan->remoteXnBindings.empty()) {
        return false;
    }
    for (const auto& proof : lowerLayerPlan->remoteXnBindings) {
        if (!proof.peerExchangeObserved ||
            !proof.endpointRouteVerified ||
            !proof.channelResourceOwnerVerified ||
            !proof.transportResourceExchangeVerified ||
            !HasInstalledChannelRoute(*lowerLayerPlan, proof.dieId, proof.channelId)) {
            return false;
        }
    }
    return true;
}

bool RequiresRemoteXnBindingEvidence(TileXRCcuBarrierMode mode)
{
    return mode == TileXRCcuBarrierMode::SyncXn ||
        mode == TileXRCcuBarrierMode::SyncXnPostOnly ||
        mode == TileXRCcuBarrierMode::SyncXnLoadPostOnly;
}

bool RequiresChannelBindingEvidence(TileXRCcuBarrierMode mode)
{
    return mode != TileXRCcuBarrierMode::LocalCke &&
        mode != TileXRCcuBarrierMode::LocalCkePostOnly;
}

uint16_t EffectiveResourceLocalWaitCke(const TileXRCcuSyncResource& resource)
{
    return resource.localWaitCke == 0 ? resource.notifyCke : resource.localWaitCke;
}

uint16_t EffectiveProofLocalWaitCke(const TileXRCcuRemoteXnBindingProof& proof)
{
    return proof.localWaitCke == 0 ? proof.notifyCke : proof.localWaitCke;
}

bool ValidateRemoteXnExchangeBindingProof(
    const TileXRCcuLaunchPackage& package,
    const TileXRCcuInstallManifest* manifest,
    const TileXRCcuLowerLayerInstallPlan* lowerLayerPlan,
    std::string* diagnostic)
{
    if (diagnostic != nullptr) {
        diagnostic->clear();
    }
    if (manifest == nullptr) {
        return FailRemoteXnExchangeBindingProof(diagnostic, "install manifest remote XN requirement is missing");
    }
    const TileXRCcuInstallRequirement* remoteXn =
        FindRequirement(*manifest, TileXRCcuInstallRequirementKind::RemoteXnBinding);
    const uint64_t packageFingerprint = TileXRCcuComputeLaunchPackageFingerprint(package);
    if (remoteXn == nullptr ||
        remoteXn->packageFingerprint != packageFingerprint ||
        remoteXn->syncResourceCount != package.plan.syncResources.size()) {
        return FailRemoteXnExchangeBindingProof(
            diagnostic,
            "install manifest remote XN requirement does not match the launch package");
    }
    if (lowerLayerPlan == nullptr || lowerLayerPlan->remoteXnBindings.empty()) {
        return FailRemoteXnExchangeBindingProof(
            diagnostic,
            "remote XN install provider is missing; peer exchange proof is missing");
    }
    if (lowerLayerPlan->remoteXnBindings.size() != package.plan.syncResources.size()) {
        return FailRemoteXnExchangeBindingProof(
            diagnostic,
            "peer exchange proof count does not match sync resource count");
    }

    for (const auto& resource : package.plan.syncResources) {
        bool identityMatched = false;
        bool remoteNotifyCkeMismatch = false;
        bool localWaitCkeMismatch = false;
        bool matched = false;
        for (const auto& proof : lowerLayerPlan->remoteXnBindings) {
            if (proof.dieId != resource.dieId ||
                proof.channelId != resource.channelId ||
                proof.localXn != resource.localXn ||
                proof.remoteXn != resource.remoteXn) {
                continue;
            }
            identityMatched = true;
            if (proof.notifyCke != resource.notifyCke) {
                remoteNotifyCkeMismatch = true;
                continue;
            }
            if (EffectiveProofLocalWaitCke(proof) != EffectiveResourceLocalWaitCke(resource)) {
                localWaitCkeMismatch = true;
                continue;
            }
            if (!proof.peerExchangeObserved) {
                return FailRemoteXnExchangeBindingProof(diagnostic, "peer exchange was not observed");
            }
            if (!HasInstalledChannelRoute(*lowerLayerPlan, proof.dieId, proof.channelId)) {
                return FailRemoteXnExchangeBindingProof(diagnostic, "matching channel route is not installed");
            }
            if (!proof.endpointRouteVerified) {
                return FailRemoteXnExchangeBindingProof(
                    diagnostic,
                    "endpoint route provenance was not verified");
            }
            if (!proof.channelResourceOwnerVerified) {
                return FailRemoteXnExchangeBindingProof(
                    diagnostic,
                    "channel resource owner did not verify channel-bound remote XN allocation");
            }
            if (!proof.transportResourceExchangeVerified) {
                return FailRemoteXnExchangeBindingProof(
                    diagnostic,
                    "transport resource exchange did not verify remote XN and notify CKE binding");
            }
            matched = true;
            break;
        }
        if (!matched) {
            if (identityMatched && remoteNotifyCkeMismatch) {
                return FailRemoteXnExchangeBindingProof(
                    diagnostic,
                    "syncXn remote notify CKE is not covered by peer exchange proof");
            }
            if (identityMatched && localWaitCkeMismatch) {
                return FailRemoteXnExchangeBindingProof(
                    diagnostic,
                    "syncXn local wait CKE is not covered by peer exchange proof");
            }
            return FailRemoteXnExchangeBindingProof(
                diagnostic,
                "syncXn remote XN operand is not covered by peer exchange proof");
        }
    }

    if (diagnostic != nullptr) {
        *diagnostic =
            "remote XN peer exchange proof matches syncXn operands, verified endpoint route channel contexts, "
            "channel resource owner allocation, and transport resource exchange";
    }
    return true;
}

int ValidateInstallManifestScope(
    const TileXRCcuLaunchPackage& package,
    uint64_t packageFingerprint,
    TileXRCcuInstallManifest* manifest,
    TileXRCcuInstallManifestReport* report)
{
    if (package.tasks.empty()) {
        return FailManifest(manifest, report, "missing CCU launch tasks for install manifest");
    }
    if (package.repository.instructions.empty()) {
        return FailManifest(manifest, report, "missing CCU repository image for install manifest");
    }
    if (package.installScope.packageFingerprint == 0 || package.installScope.provider.empty()) {
        return FailManifest(manifest, report, "launch install scope is not bound");
    }
    if (package.installScope.packageFingerprint != packageFingerprint) {
        return FailManifest(manifest, report, "launch install scope is stale");
    }
    return TILEXR_SUCCESS;
}

bool HasRequirementKind(const TileXRCcuInstallManifest& manifest, TileXRCcuInstallRequirementKind kind)
{
    bool found = false;
    for (const auto& requirement : manifest.requirements) {
        if (requirement.kind != kind) {
            continue;
        }
        if (found) {
            return false;
        }
        found = true;
    }
    return found;
}

const TileXRCcuInstallRequirement* FindRequirement(
    const TileXRCcuInstallManifest& manifest,
    TileXRCcuInstallRequirementKind kind)
{
    for (const auto& requirement : manifest.requirements) {
        if (requirement.kind == kind) {
            return &requirement;
        }
    }
    return nullptr;
}

int ValidateRequirementMetadata(
    const TileXRCcuInstallRequirement* requirement,
    uint64_t packageFingerprint,
    TileXRCcuInstallProviderReport* report)
{
    if (requirement == nullptr || requirement->label.empty() || requirement->detail.empty() ||
        requirement->packageFingerprint != packageFingerprint) {
        FillScopeFailureReport(report, "install manifest requirement metadata mismatch");
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    return TILEXR_SUCCESS;
}

bool IsPublicVerifiedStep(const TileXRCcuInstallStepEvidence& step)
{
    return step.satisfied && step.source.kind == TileXRCcuEvidenceKind::PublicVerified;
}

uint32_t CountPublicVerifiedSteps(const TileXRCcuInstallProviderReport& report)
{
    uint32_t count = 0;
    count += IsPublicVerifiedStep(report.mission) ? 1U : 0U;
    count += IsPublicVerifiedStep(report.repository) ? 1U : 0U;
    count += IsPublicVerifiedStep(report.localXn) ? 1U : 0U;
    count += IsPublicVerifiedStep(report.remoteXn) ? 1U : 0U;
    count += IsPublicVerifiedStep(report.notifyCke) ? 1U : 0U;
    count += IsPublicVerifiedStep(report.channelBinding) ? 1U : 0U;
    return count;
}

bool IsRequirementSatisfied(
    TileXRCcuInstallRequirementKind kind,
    const TileXRCcuInstallProviderReport& report)
{
    switch (kind) {
        case TileXRCcuInstallRequirementKind::MissionKey:
            return IsPublicVerifiedStep(report.mission);
        case TileXRCcuInstallRequirementKind::RepositoryImage:
            return IsPublicVerifiedStep(report.repository);
        case TileXRCcuInstallRequirementKind::LocalXn:
            return IsPublicVerifiedStep(report.localXn);
        case TileXRCcuInstallRequirementKind::RemoteXnBinding:
            return IsPublicVerifiedStep(report.remoteXn);
        case TileXRCcuInstallRequirementKind::NotifyCke:
            return IsPublicVerifiedStep(report.notifyCke);
        case TileXRCcuInstallRequirementKind::ChannelBinding:
            return IsPublicVerifiedStep(report.channelBinding);
        default:
            return false;
    }
}

uint32_t CountPublicVerifiedRequiredSteps(
    const TileXRCcuInstallManifest& manifest,
    const TileXRCcuInstallProviderReport& report)
{
    uint32_t count = 0;
    for (const auto& requirement : manifest.requirements) {
        count += IsRequirementSatisfied(requirement.kind, report) ? 1U : 0U;
    }
    return count;
}

void FillManifestInstallSurfaceCounts(
    const TileXRCcuInstallManifest* manifest,
    TileXRCcuInstallProviderReport* report)
{
    if (manifest == nullptr || report == nullptr) {
        return;
    }
    report->requiredInstallSurfaceCount = static_cast<uint32_t>(manifest->requirements.size());
    report->publicVerifiedInstallSurfaceCount = CountPublicVerifiedRequiredSteps(*manifest, *report);
    report->missingInstallSurfaceCount =
        report->requiredInstallSurfaceCount > report->publicVerifiedInstallSurfaceCount ?
        report->requiredInstallSurfaceCount - report->publicVerifiedInstallSurfaceCount :
        0U;
}

bool RejectOfflinePublicVerified(const TileXRCcuInstallProviderReport& installReport)
{
    return installReport.offlineOnly &&
        (IsPublicVerifiedStep(installReport.mission) ||
         IsPublicVerifiedStep(installReport.repository) ||
         IsPublicVerifiedStep(installReport.localXn) ||
         IsPublicVerifiedStep(installReport.remoteXn) ||
         IsPublicVerifiedStep(installReport.notifyCke) ||
         IsPublicVerifiedStep(installReport.channelBinding));
}

bool HasPublicVerifiedStep(const TileXRCcuInstallProviderReport& installReport)
{
    return IsPublicVerifiedStep(installReport.mission) ||
        IsPublicVerifiedStep(installReport.repository) ||
        IsPublicVerifiedStep(installReport.localXn) ||
        IsPublicVerifiedStep(installReport.remoteXn) ||
        IsPublicVerifiedStep(installReport.notifyCke) ||
        IsPublicVerifiedStep(installReport.channelBinding);
}

int ValidateInstallReceiptStep(
    const std::string& label,
    uint64_t expectedReceiptId,
    const TileXRCcuInstallStepEvidence& step)
{
    (void)label;
    if (!IsPublicVerifiedStep(step)) {
        return TILEXR_SUCCESS;
    }
    if (expectedReceiptId == 0 || step.source.installAttemptReceiptId == 0) {
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    if (step.source.installAttemptReceiptId != expectedReceiptId) {
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    return TILEXR_SUCCESS;
}

int ValidatePublicVerifiedStepScope(const std::string& label, const TileXRCcuInstallStepEvidence& step)
{
    (void)label;
    if (!IsPublicVerifiedStep(step)) {
        return TILEXR_SUCCESS;
    }
    if (step.source.surface != TileXRCcuEvidenceSurface::PublicInstallProvider ||
        step.source.packageFingerprint == 0 ||
        step.source.provider.empty() ||
        step.source.source.empty() ||
        step.source.detail.empty()) {
        const std::string reason = "public verified evidence scope is incomplete";
        (void)reason;
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    return TILEXR_SUCCESS;
}

int ValidateInstallReceipt(const TileXRCcuInstallProviderReport& installReport)
{
    if (HasPublicVerifiedStep(installReport) &&
        (!installReport.installAttempted || !installReport.installSucceeded)) {
        const std::string reason = "install attempt did not succeed";
        (void)reason;
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    if (ValidatePublicVerifiedStepScope("mission/key", installReport.mission) != TILEXR_SUCCESS) {
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    if (ValidatePublicVerifiedStepScope("repository", installReport.repository) != TILEXR_SUCCESS) {
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    if (ValidatePublicVerifiedStepScope("local XN", installReport.localXn) != TILEXR_SUCCESS) {
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    if (ValidatePublicVerifiedStepScope("remote XN", installReport.remoteXn) != TILEXR_SUCCESS) {
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    if (ValidatePublicVerifiedStepScope("notify CKE", installReport.notifyCke) != TILEXR_SUCCESS) {
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    if (ValidatePublicVerifiedStepScope("channel binding", installReport.channelBinding) != TILEXR_SUCCESS) {
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    if (ValidateInstallReceiptStep(
            "mission/key", installReport.installAttemptReceiptId, installReport.mission) != TILEXR_SUCCESS) {
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    if (ValidateInstallReceiptStep(
            "repository", installReport.installAttemptReceiptId, installReport.repository) != TILEXR_SUCCESS) {
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    if (ValidateInstallReceiptStep(
            "local XN", installReport.installAttemptReceiptId, installReport.localXn) != TILEXR_SUCCESS) {
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    if (ValidateInstallReceiptStep(
            "remote XN", installReport.installAttemptReceiptId, installReport.remoteXn) != TILEXR_SUCCESS) {
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    if (ValidateInstallReceiptStep(
            "notify CKE", installReport.installAttemptReceiptId, installReport.notifyCke) != TILEXR_SUCCESS) {
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    if (ValidateInstallReceiptStep(
            "channel binding", installReport.installAttemptReceiptId, installReport.channelBinding) !=
        TILEXR_SUCCESS) {
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    return TILEXR_SUCCESS;
}

int ValidateInstallRequestScope(const TileXRCcuInstallRequest& request, TileXRCcuInstallProviderReport* report)
{
    const TileXRCcuLaunchPackage& package = *request.package;
    const uint64_t packageFingerprint = TileXRCcuComputeLaunchPackageFingerprint(package);
    if (package.installScope.packageFingerprint == 0 ||
        package.installScope.packageFingerprint != packageFingerprint ||
        package.installScope.provider.empty()) {
        FillScopeFailureReport(report, "launch install scope is not bound");
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    if (request.deviceId != package.installScope.deviceId) {
        FillScopeFailureReport(report, "device scope mismatch");
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    if (request.rank != package.installScope.rank) {
        FillScopeFailureReport(report, "rank scope mismatch");
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    if (request.provider.empty() || request.provider != package.installScope.provider) {
        FillScopeFailureReport(report, "provider scope mismatch");
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    return TILEXR_SUCCESS;
}

int ValidateInstallRequestManifest(const TileXRCcuInstallRequest& request, TileXRCcuInstallProviderReport* report)
{
    if (request.manifest == nullptr) {
        return TILEXR_SUCCESS;
    }

    const TileXRCcuLaunchPackage& package = *request.package;
    const TileXRCcuInstallManifest& manifest = *request.manifest;
    const uint64_t packageFingerprint = TileXRCcuComputeLaunchPackageFingerprint(package);
    if (manifest.packageFingerprint == 0 || manifest.packageFingerprint != packageFingerprint) {
        FillScopeFailureReport(report, "install manifest fingerprint mismatch");
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    if (manifest.deviceId != request.deviceId) {
        FillScopeFailureReport(report, "install manifest device mismatch");
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    if (manifest.rank != request.rank) {
        FillScopeFailureReport(report, "install manifest rank mismatch");
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    if (manifest.provider.empty() || manifest.provider != request.provider) {
        FillScopeFailureReport(report, "install manifest provider mismatch");
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    if (manifest.requiredEvidenceKind != TileXRCcuEvidenceKind::PublicVerified ||
        manifest.requiredEvidenceSurface != TileXRCcuEvidenceSurface::PublicInstallProvider) {
        FillScopeFailureReport(report, "install manifest evidence contract mismatch");
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    if (!manifest.installAttemptReceiptRequired) {
        FillScopeFailureReport(report, "install manifest receipt contract mismatch");
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    uint32_t expectedRequirementCount = 4U;
    if (RequiresRemoteXnBindingEvidence(package.plan.barrierMode)) {
        ++expectedRequirementCount;
    }
    if (RequiresChannelBindingEvidence(package.plan.barrierMode)) {
        ++expectedRequirementCount;
    }
    if (manifest.requirements.size() != expectedRequirementCount) {
        FillScopeFailureReport(report, "install manifest requirement count mismatch");
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    if (!HasRequirementKind(manifest, TileXRCcuInstallRequirementKind::MissionKey) ||
        !HasRequirementKind(manifest, TileXRCcuInstallRequirementKind::RepositoryImage) ||
        !HasRequirementKind(manifest, TileXRCcuInstallRequirementKind::LocalXn) ||
        !HasRequirementKind(manifest, TileXRCcuInstallRequirementKind::NotifyCke)) {
        FillScopeFailureReport(report, "install manifest requirement kind mismatch");
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    if (RequiresRemoteXnBindingEvidence(package.plan.barrierMode) !=
        HasRequirementKind(manifest, TileXRCcuInstallRequirementKind::RemoteXnBinding)) {
        FillScopeFailureReport(report, "install manifest remote XN requirement kind mismatch");
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    if (RequiresChannelBindingEvidence(package.plan.barrierMode) !=
        HasRequirementKind(manifest, TileXRCcuInstallRequirementKind::ChannelBinding)) {
        FillScopeFailureReport(report, "install manifest channel binding requirement kind mismatch");
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }

    if (manifest.requiresHardwareInstall != package.requiresHardwareInstall) {
        FillScopeFailureReport(report, "install manifest hardware requirement mismatch");
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }

    const TileXRCcuInstallRequirement* mission =
        FindRequirement(manifest, TileXRCcuInstallRequirementKind::MissionKey);
    if (ValidateRequirementMetadata(mission, packageFingerprint, report) != TILEXR_SUCCESS ||
        mission->dieId != package.plan.mission.dieId ||
        mission->missionId != package.plan.mission.missionId ||
        mission->missionKey != package.plan.mission.key ||
        mission->resourceStartId != package.plan.kernelLocalMission.startId ||
        mission->resourceCount != package.plan.kernelLocalMission.num) {
        FillScopeFailureReport(report, "install manifest mission requirement mismatch");
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }

    const TileXRCcuInstallRequirement* repository =
        FindRequirement(manifest, TileXRCcuInstallRequirementKind::RepositoryImage);
    if (ValidateRequirementMetadata(repository, packageFingerprint, report) != TILEXR_SUCCESS ||
        repository->dieId != package.repository.dieId ||
        repository->repositoryStartId != package.repository.repositoryStartId ||
        repository->repositoryCount != package.repository.repositoryCount ||
        repository->missionStartId != package.repository.missionStartId ||
        repository->missionCount != package.repository.missionCount ||
        repository->instructionCount != package.repository.instructions.size()) {
        FillScopeFailureReport(report, "install manifest repository requirement mismatch");
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }

    const TileXRCcuInstallRequirement* localXn =
        FindRequirement(manifest, TileXRCcuInstallRequirementKind::LocalXn);
    if (ValidateRequirementMetadata(localXn, packageFingerprint, report) != TILEXR_SUCCESS ||
        localXn->dieId != package.plan.kernelLocalXn.dieId ||
        localXn->resourceStartId != package.plan.kernelLocalXn.startId ||
        localXn->resourceCount != package.plan.kernelLocalXn.num) {
        FillScopeFailureReport(report, "install manifest local XN requirement mismatch");
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }

    if (RequiresRemoteXnBindingEvidence(package.plan.barrierMode)) {
        const TileXRCcuInstallRequirement* remoteXn =
            FindRequirement(manifest, TileXRCcuInstallRequirementKind::RemoteXnBinding);
        if (ValidateRequirementMetadata(remoteXn, packageFingerprint, report) != TILEXR_SUCCESS ||
            remoteXn->dieId != package.plan.kernelLocalXn.dieId ||
            remoteXn->syncResourceCount != package.plan.syncResources.size()) {
            FillScopeFailureReport(report, "install manifest remote XN requirement mismatch");
            return TILEXR_ERROR_PARA_CHECK_FAIL;
        }
    }

    const TileXRCcuInstallRequirement* notifyCke =
        FindRequirement(manifest, TileXRCcuInstallRequirementKind::NotifyCke);
    if (ValidateRequirementMetadata(notifyCke, packageFingerprint, report) != TILEXR_SUCCESS ||
        notifyCke->dieId != package.plan.kernelLocalCke.dieId ||
        notifyCke->resourceStartId != package.plan.kernelLocalCke.startId ||
        notifyCke->resourceCount != package.plan.kernelLocalCke.num ||
        notifyCke->syncResourceCount != package.plan.syncResources.size()) {
        FillScopeFailureReport(report, "install manifest notify CKE requirement mismatch");
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }

    if (RequiresChannelBindingEvidence(package.plan.barrierMode)) {
        const TileXRCcuInstallRequirement* channel =
            FindRequirement(manifest, TileXRCcuInstallRequirementKind::ChannelBinding);
        if (ValidateRequirementMetadata(channel, packageFingerprint, report) != TILEXR_SUCCESS ||
            channel->dieId != package.plan.kernelLocalXn.dieId ||
            channel->syncResourceCount != package.plan.syncResources.size() ||
            channel->bindingCount != TotalBindingCount(package.plan.syncResources)) {
            FillScopeFailureReport(report, "install manifest channel requirement mismatch");
            return TILEXR_ERROR_PARA_CHECK_FAIL;
        }
    }
    return TILEXR_SUCCESS;
}

bool HasRepositoryInstallProviderInputs(const TileXRCcuInstallRequest& request)
{
    return !request.offlineOnly &&
        request.driverAdapter != nullptr &&
        request.repositoryMemoryOps.alloc != nullptr &&
        request.repositoryMemoryOps.copyHostToDevice != nullptr &&
        request.repositoryMemoryOps.free != nullptr &&
        request.repositoryReceipt != nullptr;
}

struct TileXRCcuLowerLayerInstallResult {
    uint32_t msidTokenCount = 0;
    uint32_t pfeCount = 0;
    uint32_t jettyCount = 0;
    uint32_t channelCount = 0;
    uint32_t xnClearCount = 0;
    uint32_t ckeClearCount = 0;
    bool localXnInstalled = false;
    bool notifyCkeInstalled = false;
    bool channelBindingInstalled = false;
    std::string message;
};

std::string FormatLowerLayerPreconditionSummary(const TileXRCcuLowerLayerInstallResult& result)
{
    std::ostringstream summary;
    summary << "lowerLayerPreconditions{"
            << "msidTokenCount=" << result.msidTokenCount
            << " pfeCount=" << result.pfeCount
            << " jettyCount=" << result.jettyCount
            << " channelCount=" << result.channelCount
            << " xnClearCount=" << result.xnClearCount
            << " ckeClearCount=" << result.ckeClearCount
            << " localXnInstalled=" << (result.localXnInstalled ? 1 : 0)
            << " notifyCkeInstalled=" << (result.notifyCkeInstalled ? 1 : 0)
            << " channelBindingInstalled=" << (result.channelBindingInstalled ? 1 : 0)
            << "}";
    return summary.str();
}

std::string FormatLowerLayerPlanSummary(const TileXRCcuLowerLayerInstallPlan& plan)
{
    std::ostringstream summary;
    if (!plan.msidTokens.empty()) {
        const auto& token = plan.msidTokens.front();
        summary << " msidToken0{dieId=" << static_cast<uint32_t>(token.dieId)
                << " msId=" << token.msId
                << " tokenId=0x" << std::hex << token.tokenId
                << " tokenValue=0x" << token.tokenValue << std::dec
                << "}";
    }
    if (!plan.pfes.empty()) {
        const auto& pfe = plan.pfes.front();
        summary << " pfe0{dieId=" << static_cast<uint32_t>(pfe.dieId)
                << " offset=" << pfe.pfeOffset
                << "}";
    }
    if (!plan.jettys.empty()) {
        const auto& jetty = plan.jettys.front();
        summary << " jetty0{dieId=" << static_cast<uint32_t>(jetty.dieId)
                << " startJettyCtxId=" << jetty.startJettyCtxId
                << " ctxCount=" << jetty.ctxs.size()
                << "}";
    }
    if (!plan.channels.empty()) {
        const auto& channel = plan.channels.front();
        summary << " channel0{dieId=" << static_cast<uint32_t>(channel.dieId)
                << " channelId=" << channel.channelId
                << "}";
    }
    if (!plan.xnClears.empty()) {
        const auto& xn = plan.xnClears.front();
        summary << " xnClear0{dieId=" << static_cast<uint32_t>(xn.dieId)
                << " startXnId=" << xn.startXnId
                << " count=" << xn.count
                << "}";
    }
    if (!plan.ckeClears.empty()) {
        const auto& cke = plan.ckeClears.front();
        summary << " ckeClear0{dieId=" << static_cast<uint32_t>(cke.dieId)
                << " startCkeId=" << cke.startCkeId
                << " count=" << cke.count
                << "}";
    }
    if (!plan.remoteXnBindings.empty()) {
        const auto& remote = plan.remoteXnBindings.front();
        summary << " remoteXn0{dieId=" << static_cast<uint32_t>(remote.dieId)
                << " channelId=" << remote.channelId
                << " localXn=" << remote.localXn
                << " remoteXn=" << remote.remoteXn
                << " notifyCke=" << remote.notifyCke
                << " localWaitCke=" << remote.localWaitCke
                << " peerExchangeObserved=" << (remote.peerExchangeObserved ? 1 : 0)
                << " endpointRouteVerified=" << (remote.endpointRouteVerified ? 1 : 0)
                << " channelResourceOwnerVerified=" << (remote.channelResourceOwnerVerified ? 1 : 0)
                << " transportResourceExchangeVerified=" << (remote.transportResourceExchangeVerified ? 1 : 0)
                << "}";
    }
    return summary.str();
}

int InstallLowerLayerResources(
    const TileXRCcuDriverAdapter& adapter,
    const TileXRCcuLowerLayerInstallPlan& plan,
    TileXRCcuLowerLayerInstallResult* result)
{
    if (result == nullptr) {
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    *result = TileXRCcuLowerLayerInstallResult{};

    TileXRCcuDriverAdapterReport driverReport;
    for (const auto& token : plan.msidTokens) {
        const int ret = adapter.InstallMsidToken(
            token.dieId,
            token.msId,
            token.tokenId,
            token.tokenValue,
            &driverReport);
        if (ret != TILEXR_SUCCESS) {
            result->message = "failed to install CCU MSID token via SET_MSID_TOKEN: " + driverReport.message;
            return ret;
        }
        ++result->msidTokenCount;
    }

    for (const auto& pfe : plan.pfes) {
        const int ret = adapter.InstallPfeCtx(pfe.dieId, pfe.pfeOffset, pfe.ctx, &driverReport);
        if (ret != TILEXR_SUCCESS) {
            result->message = "failed to install CCU PFE context via SET_PFE: " + driverReport.message;
            return ret;
        }
        ++result->pfeCount;
    }

    for (const auto& jetty : plan.jettys) {
        const TileXRCcuLocalJettyCtxData* ctxs = jetty.ctxs.empty() ? nullptr : jetty.ctxs.data();
        const int ret = adapter.InstallJettyCtx(
            jetty.dieId,
            jetty.startJettyCtxId,
            ctxs,
            static_cast<uint32_t>(jetty.ctxs.size()),
            &driverReport);
        if (ret != TILEXR_SUCCESS) {
            result->message = "failed to install CCU local jetty contexts via SET_JETTY_CTX: " +
                driverReport.message;
            return ret;
        }
        ++result->jettyCount;
    }

    for (const auto& channel : plan.channels) {
        const int ret = adapter.InstallChannelCtxV1(channel.dieId, channel.channelId, channel.ctx, &driverReport);
        if (ret != TILEXR_SUCCESS) {
            result->message = "failed to install CCU channel context via SET_CHANNEL: " + driverReport.message;
            return ret;
        }
        ++result->channelCount;
    }

    for (const auto& xn : plan.xnClears) {
        const int ret = adapter.InstallXnRange(xn.dieId, xn.startXnId, xn.count, &driverReport);
        if (ret != TILEXR_SUCCESS) {
            result->message = "failed to install CCU local XN range via SET_XN: " + driverReport.message;
            return ret;
        }
        ++result->xnClearCount;
    }

    for (const auto& cke : plan.ckeClears) {
        const int ret = adapter.ClearCkeRange(cke.dieId, cke.startCkeId, cke.count, &driverReport);
        if (ret != TILEXR_SUCCESS) {
            result->message = "failed to clear CCU CKE range via SET_CKE: " + driverReport.message;
            return ret;
        }
        ++result->ckeClearCount;
    }

    result->localXnInstalled = result->xnClearCount > 0;
    result->notifyCkeInstalled = result->ckeClearCount > 0;
    const uint32_t expectedChannelCount = plan.remoteXnBindings.empty() ?
        static_cast<uint32_t>(plan.channels.size()) :
        CountUniqueRemoteBindingChannels(plan.remoteXnBindings);
    const uint32_t installedChannelCount = plan.remoteXnBindings.empty() ?
        static_cast<uint32_t>(result->channelCount) :
        CountInstalledRemoteBindingChannels(plan.remoteXnBindings, plan.channels);
    result->channelBindingInstalled =
        result->pfeCount > 0 && result->jettyCount > 0 && expectedChannelCount > 0 &&
        installedChannelCount >= expectedChannelCount;
    result->message =
        "lower-layer CCU resources installed via SET_MSID_TOKEN, SET_PFE, SET_JETTY_CTX, SET_CHANNEL, SET_XN, SET_CKE";
    return TILEXR_SUCCESS;
}

int InstallRepositoryImageForRequest(
    const TileXRCcuInstallRequest& request,
    TileXRCcuRepositoryReport* repositoryReport)
{
    return TileXRCcuInstallRepositoryImageWithOptions(
        request.package->repository,
        request.repositoryInstallOptions,
        request.repositoryMemoryOps,
        request.repositoryMemoryUserData,
        *request.driverAdapter,
        request.repositoryReceipt,
        repositoryReport);
}

int InstallRepositoryOnly(
    const TileXRCcuInstallRequest& request,
    TileXRCcuHardwareInstallEvidence* evidence,
    TileXRCcuInstallProviderReport* report)
{
    const TileXRCcuLaunchPackage& package = *request.package;
    TileXRCcuRepositoryReport repositoryReport;
    TileXRCcuLowerLayerInstallResult lowerLayerResult;

    int lowerLayerRet = TILEXR_SUCCESS;
    if (request.installOrder == TileXRCcuInstallOrder::InstallLowerLayerFirst &&
        request.lowerLayerPlan != nullptr) {
        lowerLayerRet = InstallLowerLayerResources(*request.driverAdapter, *request.lowerLayerPlan, &lowerLayerResult);
        if (lowerLayerRet != TILEXR_SUCCESS) {
            TileXRCcuInstallProviderReport nextReport;
            nextReport.offlineOnly = false;
            nextReport.installAttempted = true;
            nextReport.installSucceeded = false;
            nextReport.message = lowerLayerResult.message;
            MarkMissingStep(&nextReport.mission, "mission/key install provider is not implemented");
            MarkMissingStep(&nextReport.repository, "repository install skipped after lower-layer install failure");
            MarkMissingStep(&nextReport.localXn, lowerLayerResult.message);
            MarkMissingStep(&nextReport.remoteXn, "remote XN binding provider is not implemented");
            MarkMissingStep(&nextReport.notifyCke, lowerLayerResult.message);
            MarkMissingStep(&nextReport.channelBinding, lowerLayerResult.message);
            FillManifestInstallSurfaceCounts(request.manifest, &nextReport);
            if (report != nullptr) {
                *report = nextReport;
            }
            if (TileXRCcuBuildInstallEvidence(nextReport, evidence) != TILEXR_SUCCESS) {
                return TILEXR_ERROR_INTERNAL;
            }
            return lowerLayerRet;
        }
    }

    const int installRet = InstallRepositoryImageForRequest(request, &repositoryReport);

    TileXRCcuInstallProviderReport nextReport;
    nextReport.offlineOnly = false;
    nextReport.installAttempted = true;

    if (installRet != TILEXR_SUCCESS) {
        nextReport.installSucceeded = false;
        const bool lowerLayerFirstAttempted =
            request.installOrder == TileXRCcuInstallOrder::InstallLowerLayerFirst &&
            request.lowerLayerPlan != nullptr;
        const std::string lowerLayerSummary = lowerLayerFirstAttempted ?
            FormatLowerLayerPreconditionSummary(lowerLayerResult) +
                FormatLowerLayerPlanSummary(*request.lowerLayerPlan) :
            std::string();
        const std::string repositoryFailureMessage = lowerLayerFirstAttempted ?
            lowerLayerSummary + "; " + repositoryReport.message :
            repositoryReport.message;
        nextReport.message = repositoryFailureMessage;
        MarkMissingStep(&nextReport.mission, "mission/key install provider is not implemented");
        MarkMissingStep(&nextReport.repository, repositoryReport.message);
        MarkMissingStep(
            &nextReport.localXn,
            lowerLayerFirstAttempted ? lowerLayerSummary : "local XN install provider is not implemented");
        MarkMissingStep(
            &nextReport.remoteXn,
            lowerLayerFirstAttempted ? lowerLayerSummary : "remote XN binding provider is not implemented");
        MarkMissingStep(
            &nextReport.notifyCke,
            lowerLayerFirstAttempted ? lowerLayerSummary : "notify CKE install provider is not implemented");
        MarkMissingStep(
            &nextReport.channelBinding,
            lowerLayerFirstAttempted ? lowerLayerSummary : "channel binding provider is not implemented");
        FillManifestInstallSurfaceCounts(request.manifest, &nextReport);
        if (report != nullptr) {
            *report = nextReport;
        }
        if (TileXRCcuBuildInstallEvidence(nextReport, evidence) != TILEXR_SUCCESS) {
            return TILEXR_ERROR_INTERNAL;
        }
        return installRet;
    }

    nextReport.installSucceeded = true;
    nextReport.installAttemptReceiptId = BuildRepositoryInstallReceiptId(package, *request.repositoryReceipt);

    if (request.lowerLayerPlan != nullptr &&
        request.installOrder != TileXRCcuInstallOrder::InstallLowerLayerFirst) {
        lowerLayerRet = InstallLowerLayerResources(*request.driverAdapter, *request.lowerLayerPlan, &lowerLayerResult);
        if (lowerLayerRet != TILEXR_SUCCESS) {
            nextReport.installSucceeded = false;
            nextReport.message = lowerLayerResult.message;
            MarkMissingStep(&nextReport.mission, "mission/key install provider is not implemented");
            MarkMissingStep(
                &nextReport.repository,
                "repository install evidence withheld after lower-layer install failure");
            MarkMissingStep(&nextReport.localXn, lowerLayerResult.message);
            MarkMissingStep(&nextReport.remoteXn, "remote XN binding provider is not implemented");
            MarkMissingStep(&nextReport.notifyCke, lowerLayerResult.message);
            MarkMissingStep(&nextReport.channelBinding, lowerLayerResult.message);
            FillManifestInstallSurfaceCounts(request.manifest, &nextReport);
            if (report != nullptr) {
                *report = nextReport;
            }
            if (TileXRCcuBuildInstallEvidence(nextReport, evidence) != TILEXR_SUCCESS) {
                return TILEXR_ERROR_INTERNAL;
            }
            return lowerLayerRet;
        }
    }

    std::string missionProofMessage;
    if (ValidateMissionLaunchDescriptorProof(package, request.manifest, &missionProofMessage)) {
        nextReport.mission = PublicVerifiedStep(
            package,
            request,
            nextReport.installAttemptReceiptId,
            "ValidateMissionLaunchDescriptorProof",
            missionProofMessage);
    } else {
        MarkMissingStep(&nextReport.mission, missionProofMessage);
    }
    nextReport.repository = PublicVerifiedStep(
        package,
        request,
        nextReport.installAttemptReceiptId,
        "TileXRCcuInstallRepositoryImage",
        "repository instruction image installed via SET_INSTRUCTION");
    if (lowerLayerResult.localXnInstalled) {
        nextReport.localXn = PublicVerifiedStep(
            package,
            request,
            nextReport.installAttemptReceiptId,
            "InstallLowerLayerResources",
            "kernel-local XN resources initialized via SET_XN");
    } else {
        MarkMissingStep(&nextReport.localXn, "local XN install provider is not implemented");
    }
    std::string remoteXnProofMessage;
    if (!RequiresRemoteXnBindingEvidence(package.plan.barrierMode)) {
        MarkMissingStep(
            &nextReport.remoteXn,
            "remote XN binding is not required for this CCU barrier mode");
    } else if (ValidateRemoteXnExchangeBindingProof(
            package,
            request.manifest,
            request.lowerLayerPlan,
            &remoteXnProofMessage)) {
        nextReport.remoteXn = PublicVerifiedStep(
            package,
            request,
            nextReport.installAttemptReceiptId,
            "ValidateRemoteXnExchangeBindingProof",
            remoteXnProofMessage,
            true);
    } else {
        MarkMissingStep(&nextReport.remoteXn, remoteXnProofMessage);
    }
    if (lowerLayerResult.notifyCkeInstalled) {
        nextReport.notifyCke = PublicVerifiedStep(
            package,
            request,
            nextReport.installAttemptReceiptId,
            "InstallLowerLayerResources",
            "notify CKE resources cleared via SET_CKE");
    } else {
        MarkMissingStep(&nextReport.notifyCke, "notify CKE install provider is not implemented");
    }
    if (!RequiresChannelBindingEvidence(package.plan.barrierMode)) {
        MarkMissingStep(
            &nextReport.channelBinding,
            "channel binding is not required for this CCU barrier mode");
    } else if (lowerLayerResult.channelBindingInstalled && HasVerifiedChannelResourceBindings(request.lowerLayerPlan)) {
        nextReport.channelBinding = PublicVerifiedStep(
            package,
            request,
            nextReport.installAttemptReceiptId,
            "InstallLowerLayerResources",
            "channel binding contexts installed via SET_PFE, SET_JETTY_CTX, SET_CHANNEL with verified endpoint routes, "
            "channel resource owner allocation, and transport resource exchange",
            true);
    } else if (lowerLayerResult.channelBindingInstalled && HasVerifiedEndpointRoutes(request.lowerLayerPlan)) {
        MarkMissingStep(
            &nextReport.channelBinding,
            "channel binding channel resource owner or transport resource exchange provenance was not verified");
    } else if (lowerLayerResult.channelBindingInstalled) {
        MarkMissingStep(&nextReport.channelBinding, "channel binding endpoint route provenance was not verified");
    } else {
        MarkMissingStep(&nextReport.channelBinding, "channel binding provider is not implemented");
    }
    if (request.lowerLayerPlan == nullptr) {
        nextReport.message =
            "repository instruction image installed via SET_INSTRUCTION; " +
            (nextReport.mission.satisfied ? missionProofMessage : nextReport.mission.message) +
            "; lower-layer CCU resources are missing";
    } else {
        nextReport.message =
            "repository instruction image installed via SET_INSTRUCTION; " + lowerLayerResult.message +
            "; " + (nextReport.mission.satisfied ? remoteXnProofMessage :
            nextReport.mission.message + "; " + remoteXnProofMessage);
    }
    FillManifestInstallSurfaceCounts(request.manifest, &nextReport);

    if (report != nullptr) {
        *report = nextReport;
    }
    if (TileXRCcuBuildInstallEvidence(nextReport, evidence) != TILEXR_SUCCESS) {
        return TILEXR_ERROR_INTERNAL;
    }
    if (nextReport.missingInstallSurfaceCount == 0) {
        return TILEXR_SUCCESS;
    }
    return TILEXR_ERROR_NOT_FOUND;
}

} // namespace

int TileXRCcuBuildInstallEvidence(
    const TileXRCcuInstallProviderReport& installReport,
    TileXRCcuHardwareInstallEvidence* evidence)
{
    if (evidence == nullptr) {
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    *evidence = TileXRCcuHardwareInstallEvidence{};

    if (RejectOfflinePublicVerified(installReport)) {
        const std::string reason = "offline install evidence cannot be public verified";
        (void)reason;
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    if (ValidateInstallReceipt(installReport) != TILEXR_SUCCESS) {
        const std::string reason = "install attempt receipt mismatch";
        (void)reason;
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }

    TileXRCcuHardwareInstallEvidence result;
    result.missionInstalled = installReport.mission.satisfied;
    result.repositoryInstalled = installReport.repository.satisfied;
    result.localXnInstalled = installReport.localXn.satisfied;
    result.remoteXnBound = installReport.remoteXn.satisfied;
    result.notifyCkeInstalled = installReport.notifyCke.satisfied;
    result.channelBindingsInstalled = installReport.channelBinding.satisfied;
    result.missionSource = SourceOrMissing(installReport.mission, "mission/key install evidence is missing");
    result.repositorySource = SourceOrMissing(installReport.repository, "repository install evidence is missing");
    result.localXnSource = SourceOrMissing(installReport.localXn, "local XN install evidence is missing");
    result.remoteXnSource = SourceOrMissing(installReport.remoteXn, "remote XN binding evidence is missing");
    result.notifyCkeSource = SourceOrMissing(installReport.notifyCke, "notify CKE install evidence is missing");
    result.channelBindingSource =
        SourceOrMissing(installReport.channelBinding, "channel binding install evidence is missing");

    *evidence = result;
    return TILEXR_SUCCESS;
}

int TileXRCcuBuildInstallManifest(
    const TileXRCcuLaunchPackage& package,
    TileXRCcuInstallManifest* manifest,
    TileXRCcuInstallManifestReport* report)
{
    ResetManifestReport(report);
    if (manifest == nullptr) {
        return FailManifest(nullptr, report, "missing output CCU install manifest");
    }
    *manifest = TileXRCcuInstallManifest{};

    const uint64_t packageFingerprint = TileXRCcuComputeLaunchPackageFingerprint(package);
    if (ValidateInstallManifestScope(package, packageFingerprint, manifest, report) != TILEXR_SUCCESS) {
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }

    TileXRCcuInstallManifest result;
    result.packageFingerprint = packageFingerprint;
    result.deviceId = package.installScope.deviceId;
    result.rank = package.installScope.rank;
    result.provider = package.installScope.provider;
    result.requiresHardwareInstall = package.requiresHardwareInstall;
    result.installAttemptReceiptRequired = true;
    result.requiredEvidenceKind = TileXRCcuEvidenceKind::PublicVerified;
    result.requiredEvidenceSurface = TileXRCcuEvidenceSurface::PublicInstallProvider;

    TileXRCcuInstallRequirement mission = BaseRequirement(
        TileXRCcuInstallRequirementKind::MissionKey,
        packageFingerprint,
        package.plan.mission.dieId,
        "mission/key",
        "install CCU mission id and key");
    mission.missionId = package.plan.mission.missionId;
    mission.missionKey = package.plan.mission.key;
    mission.resourceStartId = package.plan.kernelLocalMission.startId;
    mission.resourceCount = package.plan.kernelLocalMission.num;
    result.requirements.push_back(mission);

    TileXRCcuInstallRequirement repository = BaseRequirement(
        TileXRCcuInstallRequirementKind::RepositoryImage,
        packageFingerprint,
        package.repository.dieId,
        "repository",
        "install generated CCU repository image");
    repository.repositoryStartId = package.repository.repositoryStartId;
    repository.repositoryCount = package.repository.repositoryCount;
    repository.missionStartId = package.repository.missionStartId;
    repository.missionCount = package.repository.missionCount;
    repository.instructionCount = static_cast<uint32_t>(package.repository.instructions.size());
    result.requirements.push_back(repository);

    TileXRCcuInstallRequirement localXn = BaseRequirement(
        TileXRCcuInstallRequirementKind::LocalXn,
        packageFingerprint,
        package.plan.kernelLocalXn.dieId,
        "local XN",
        "install kernel-local CCU XN resource window");
    localXn.resourceStartId = package.plan.kernelLocalXn.startId;
    localXn.resourceCount = package.plan.kernelLocalXn.num;
    result.requirements.push_back(localXn);

    if (RequiresRemoteXnBindingEvidence(package.plan.barrierMode)) {
        TileXRCcuInstallRequirement remoteXn = BaseRequirement(
            TileXRCcuInstallRequirementKind::RemoteXnBinding,
            packageFingerprint,
            package.plan.kernelLocalXn.dieId,
            "remote XN binding",
            "bind remote CCU XN resources referenced by sync instructions");
        remoteXn.syncResourceCount = static_cast<uint32_t>(package.plan.syncResources.size());
        result.requirements.push_back(remoteXn);
    }

    TileXRCcuInstallRequirement notifyCke = BaseRequirement(
        TileXRCcuInstallRequirementKind::NotifyCke,
        packageFingerprint,
        package.plan.kernelLocalCke.dieId,
        "notify CKE",
        "install notify CKE resources referenced by sync instructions");
    notifyCke.resourceStartId = package.plan.kernelLocalCke.startId;
    notifyCke.resourceCount = package.plan.kernelLocalCke.num;
    notifyCke.syncResourceCount = static_cast<uint32_t>(package.plan.syncResources.size());
    result.requirements.push_back(notifyCke);

    if (RequiresChannelBindingEvidence(package.plan.barrierMode)) {
        TileXRCcuInstallRequirement channel = BaseRequirement(
            TileXRCcuInstallRequirementKind::ChannelBinding,
            packageFingerprint,
            package.plan.kernelLocalXn.dieId,
            "channel binding",
            "bind CCU channel routes for sync resources");
        channel.syncResourceCount = static_cast<uint32_t>(package.plan.syncResources.size());
        channel.bindingCount = TotalBindingCount(package.plan.syncResources);
        result.requirements.push_back(channel);
    }

    if (report != nullptr) {
        report->requirementCount = static_cast<uint32_t>(result.requirements.size());
        report->message = "ok";
    }
    *manifest = result;
    return TILEXR_SUCCESS;
}

int TileXRCcuInstallHardware(
    const TileXRCcuInstallRequest& request,
    TileXRCcuHardwareInstallEvidence* evidence,
    TileXRCcuInstallProviderReport* report)
{
    ResetReport(report);
    if (evidence == nullptr) {
        FillUnsupportedReport(report);
        if (report != nullptr) {
            report->message = "missing output CCU hardware install evidence";
        }
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    *evidence = TileXRCcuHardwareInstallEvidence{};

    if (request.package == nullptr) {
        FillUnsupportedReport(report);
        if (report != nullptr) {
            report->message = "missing CCU launch package for hardware install";
        }
        TileXRCcuInstallProviderReport missingReport;
        FillUnsupportedReport(&missingReport);
        return TileXRCcuBuildInstallEvidence(missingReport, evidence) == TILEXR_SUCCESS ?
            TILEXR_ERROR_PARA_CHECK_FAIL :
            TILEXR_ERROR_INTERNAL;
    }

    if (ValidateInstallRequestScope(request, report) != TILEXR_SUCCESS) {
        TileXRCcuInstallProviderReport missingReport;
        FillScopeFailureReport(&missingReport, report == nullptr ? "install request scope mismatch" : report->message);
        return TileXRCcuBuildInstallEvidence(missingReport, evidence) == TILEXR_SUCCESS ?
            TILEXR_ERROR_PARA_CHECK_FAIL :
            TILEXR_ERROR_INTERNAL;
    }
    if (ValidateInstallRequestManifest(request, report) != TILEXR_SUCCESS) {
        TileXRCcuInstallProviderReport missingReport;
        FillScopeFailureReport(
            &missingReport,
            report == nullptr ? "install request manifest mismatch" : report->message);
        return TileXRCcuBuildInstallEvidence(missingReport, evidence) == TILEXR_SUCCESS ?
            TILEXR_ERROR_PARA_CHECK_FAIL :
            TILEXR_ERROR_INTERNAL;
    }

    if (HasRepositoryInstallProviderInputs(request)) {
        return InstallRepositoryOnly(request, evidence, report);
    }

    FillUnsupportedReport(report);
    FillManifestInstallSurfaceCounts(request.manifest, report);
    TileXRCcuInstallProviderReport missingReport;
    FillUnsupportedReport(&missingReport);
    if (TileXRCcuBuildInstallEvidence(missingReport, evidence) != TILEXR_SUCCESS) {
        return TILEXR_ERROR_INTERNAL;
    }
    return TILEXR_ERROR_NOT_FOUND;
}

} // namespace TileXR
