/*
 * Copyright (c) 2026 TileXR Project
 * Licensed under the Apache License, Version 2.0
 */

#include "ccu/tilexr_ccu_provider.h"

namespace TileXR {
namespace {

void ResetReport(TileXRCcuProviderReport* report)
{
    if (report == nullptr) {
        return;
    }
    report->taskCount = 0;
    report->repositoryCount = 0;
    report->installedInstructionCount = 0;
    report->evidenceBitCount = 0;
    report->publicVerifiedEvidenceCount = 0;
    report->legacyEvidenceCount = 0;
    report->publicCandidateEvidenceCount = 0;
    report->privateObservedEvidenceCount = 0;
    report->missingEvidenceCount = 0;
    report->submitReady = false;
    report->message.clear();
}

int Fail(TileXRCcuProviderReport* report, const std::string& message)
{
    if (report != nullptr) {
        report->message = message;
    }
    return TILEXR_ERROR_PARA_CHECK_FAIL;
}

const char* EvidenceKindName(TileXRCcuEvidenceKind kind)
{
    switch (kind) {
        case TileXRCcuEvidenceKind::LegacyBoolean:
            return "legacy";
        case TileXRCcuEvidenceKind::PublicVerified:
            return "public verified";
        case TileXRCcuEvidenceKind::PublicCandidate:
            return "public candidate";
        case TileXRCcuEvidenceKind::PrivateObserved:
            return "private observed";
        case TileXRCcuEvidenceKind::Missing:
            return "missing";
        default:
            return "unknown";
    }
}

const char* EvidenceSurfaceName(TileXRCcuEvidenceSurface surface)
{
    switch (surface) {
        case TileXRCcuEvidenceSurface::Unspecified:
            return "unspecified surface";
        case TileXRCcuEvidenceSurface::PublicInstallProvider:
            return "public install provider";
        case TileXRCcuEvidenceSurface::LowerLayerResourceHelper:
            return "lower-layer resource helper";
        case TileXRCcuEvidenceSurface::PrivateProducerObservation:
            return "private producer observation";
        default:
            return "unknown surface";
    }
}

std::string EvidenceMessage(
    const std::string& label,
    const TileXRCcuEvidenceSource& source,
    const std::string& suffix)
{
    std::string message = label + " " + suffix;
    if (source.kind != TileXRCcuEvidenceKind::LegacyBoolean) {
        message += " (" + std::string(EvidenceKindName(source.kind));
        message += ", " + std::string(EvidenceSurfaceName(source.surface));
        if (!source.source.empty()) {
            message += ": " + source.source;
        }
        if (!source.detail.empty()) {
            message += "; " + source.detail;
        }
        message += ")";
    }
    return message;
}

int ValidateEvidenceSource(
    const std::string& label,
    bool installed,
    const TileXRCcuEvidenceSource& source,
    TileXRCcuProviderReport* report)
{
    if (!installed || source.kind == TileXRCcuEvidenceKind::Missing) {
        return Fail(report, EvidenceMessage(label, source, "hardware install evidence is missing"));
    }
    if (source.kind == TileXRCcuEvidenceKind::LegacyBoolean ||
        source.kind == TileXRCcuEvidenceKind::PublicVerified) {
        return TILEXR_SUCCESS;
    }
    if (source.kind == TileXRCcuEvidenceKind::PublicCandidate) {
        return Fail(report, EvidenceMessage(label, source, "evidence is only a public candidate"));
    }
    if (source.kind == TileXRCcuEvidenceKind::PrivateObserved) {
        return Fail(report, EvidenceMessage(label, source, "evidence is private observed"));
    }
    return Fail(report, EvidenceMessage(label, source, "evidence kind is not accepted"));
}

void CountEvidenceKind(bool installed, const TileXRCcuEvidenceSource& source, TileXRCcuProviderReport* report)
{
    if (report == nullptr) {
        return;
    }
    ++report->evidenceBitCount;
    if (!installed || source.kind == TileXRCcuEvidenceKind::Missing) {
        ++report->missingEvidenceCount;
        return;
    }
    switch (source.kind) {
        case TileXRCcuEvidenceKind::LegacyBoolean:
            ++report->legacyEvidenceCount;
            break;
        case TileXRCcuEvidenceKind::PublicVerified:
            ++report->publicVerifiedEvidenceCount;
            break;
        case TileXRCcuEvidenceKind::PublicCandidate:
            ++report->publicCandidateEvidenceCount;
            break;
        case TileXRCcuEvidenceKind::PrivateObserved:
            ++report->privateObservedEvidenceCount;
            break;
        case TileXRCcuEvidenceKind::Missing:
            ++report->missingEvidenceCount;
            break;
        default:
            ++report->missingEvidenceCount;
            break;
    }
}

void CountInstallEvidence(const TileXRCcuHardwareInstallEvidence& evidence, TileXRCcuProviderReport* report)
{
    CountEvidenceKind(evidence.missionInstalled, evidence.missionSource, report);
    CountEvidenceKind(evidence.repositoryInstalled, evidence.repositorySource, report);
    CountEvidenceKind(evidence.localXnInstalled, evidence.localXnSource, report);
    CountEvidenceKind(evidence.remoteXnBound, evidence.remoteXnSource, report);
    CountEvidenceKind(evidence.notifyCkeInstalled, evidence.notifyCkeSource, report);
    CountEvidenceKind(evidence.channelBindingsInstalled, evidence.channelBindingSource, report);
}

void FillReadyReport(const TileXRCcuLaunchPackage& package, TileXRCcuProviderReport* report)
{
    if (report == nullptr) {
        return;
    }
    report->taskCount = static_cast<uint32_t>(package.tasks.size());
    report->repositoryCount = package.repository.repositoryCount;
    report->installedInstructionCount =
        static_cast<uint32_t>(package.repository.sqeLoadCount + package.repository.syncCount);
    report->submitReady = true;
    report->message = "ok";
}

int ValidatePackageShape(const TileXRCcuLaunchPackage& package, TileXRCcuProviderReport* report)
{
    if (package.tasks.empty()) {
        return Fail(report, "missing CCU launch tasks in provider package");
    }
    if (package.repository.instructions.empty()) {
        return Fail(report, "missing CCU repository image in provider package");
    }
    if (package.program.sync.empty()) {
        return Fail(report, "missing generated CCU microcode in provider package");
    }
    return TILEXR_SUCCESS;
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

int ValidateInstallEvidence(
    const TileXRCcuLaunchPackage& package,
    const TileXRCcuHardwareInstallEvidence& evidence,
    TileXRCcuProviderReport* report)
{
    CountInstallEvidence(evidence, report);
    if (ValidateEvidenceSource("mission/key", evidence.missionInstalled, evidence.missionSource, report) !=
        TILEXR_SUCCESS) {
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    if (ValidateEvidenceSource("repository", evidence.repositoryInstalled, evidence.repositorySource, report) !=
        TILEXR_SUCCESS) {
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    if (ValidateEvidenceSource("local XN", evidence.localXnInstalled, evidence.localXnSource, report) !=
        TILEXR_SUCCESS) {
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    if (RequiresRemoteXnBindingEvidence(package.plan.barrierMode) &&
        ValidateEvidenceSource("remote XN binding", evidence.remoteXnBound, evidence.remoteXnSource, report) !=
        TILEXR_SUCCESS) {
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    if (ValidateEvidenceSource("notify CKE install", evidence.notifyCkeInstalled, evidence.notifyCkeSource, report) !=
        TILEXR_SUCCESS) {
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    if (RequiresChannelBindingEvidence(package.plan.barrierMode) &&
        ValidateEvidenceSource(
            "channel binding", evidence.channelBindingsInstalled, evidence.channelBindingSource, report) !=
        TILEXR_SUCCESS) {
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    return TILEXR_SUCCESS;
}

int ValidateSubmitEvidenceSource(
    const std::string& label,
    const TileXRCcuLaunchPackage& package,
    bool installed,
    const TileXRCcuEvidenceSource& source,
    bool endpointRouteRequired,
    TileXRCcuProviderReport* report)
{
    if (!installed || source.kind != TileXRCcuEvidenceKind::PublicVerified) {
        return Fail(report, EvidenceMessage(label, source, "submit requires public verified evidence"));
    }
    if (source.source.empty() || source.detail.empty()) {
        return Fail(report, EvidenceMessage(label, source, "public verified evidence source/detail required"));
    }
    if (source.surface != TileXRCcuEvidenceSurface::PublicInstallProvider) {
        return Fail(report, EvidenceMessage(
            label, source, "submit requires public install provider evidence"));
    }
    const uint64_t packageFingerprint = TileXRCcuComputeLaunchPackageFingerprint(package);
    if (package.installScope.packageFingerprint == 0 ||
        package.installScope.packageFingerprint != packageFingerprint ||
        package.installScope.provider.empty()) {
        return Fail(report, EvidenceMessage(label, source, "launch install scope is not bound"));
    }
    if (source.packageFingerprint == 0 || source.packageFingerprint != packageFingerprint) {
        return Fail(report, EvidenceMessage(label, source, "package fingerprint mismatch"));
    }
    if (source.deviceId != package.installScope.deviceId) {
        return Fail(report, EvidenceMessage(label, source, "device scope mismatch"));
    }
    if (source.rank != package.installScope.rank) {
        return Fail(report, EvidenceMessage(label, source, "rank scope mismatch"));
    }
    if (source.provider.empty() || source.provider != package.installScope.provider) {
        return Fail(report, EvidenceMessage(label, source, "provider scope mismatch"));
    }
    if (source.installAttemptReceiptId == 0) {
        return Fail(report, EvidenceMessage(label, source, "install attempt receipt is missing"));
    }
    if (endpointRouteRequired && !source.endpointRouteVerified) {
        return Fail(report, EvidenceMessage(label, source, "submit requires verified endpoint route evidence"));
    }
    return TILEXR_SUCCESS;
}

int ValidateSameInstallAttemptReceipt(
    const TileXRCcuEvidenceSource& expected,
    const std::string& label,
    const TileXRCcuEvidenceSource& source,
    TileXRCcuProviderReport* report)
{
    if (source.installAttemptReceiptId != expected.installAttemptReceiptId) {
        return Fail(report, EvidenceMessage(label, source, "install attempt receipt mismatch"));
    }
    return TILEXR_SUCCESS;
}

int ValidateSubmitEvidence(
    const TileXRCcuLaunchPackage& package,
    const TileXRCcuHardwareInstallEvidence& evidence,
    TileXRCcuProviderReport* report)
{
    if (ValidateSubmitEvidenceSource(
            "mission/key", package, evidence.missionInstalled, evidence.missionSource, false, report) !=
        TILEXR_SUCCESS) {
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    if (ValidateSubmitEvidenceSource(
            "repository", package, evidence.repositoryInstalled, evidence.repositorySource, false, report) !=
        TILEXR_SUCCESS) {
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    if (ValidateSubmitEvidenceSource(
            "local XN", package, evidence.localXnInstalled, evidence.localXnSource, false, report) !=
        TILEXR_SUCCESS) {
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    if (RequiresRemoteXnBindingEvidence(package.plan.barrierMode) &&
        ValidateSubmitEvidenceSource(
            "remote XN binding", package, evidence.remoteXnBound, evidence.remoteXnSource, true, report) !=
        TILEXR_SUCCESS) {
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    if (ValidateSubmitEvidenceSource(
            "notify CKE install", package, evidence.notifyCkeInstalled, evidence.notifyCkeSource, false, report) !=
        TILEXR_SUCCESS) {
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    if (RequiresChannelBindingEvidence(package.plan.barrierMode) &&
        ValidateSubmitEvidenceSource(
            "channel binding", package, evidence.channelBindingsInstalled, evidence.channelBindingSource, true, report) !=
        TILEXR_SUCCESS) {
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    const TileXRCcuEvidenceSource& expected = evidence.missionSource;
    if (ValidateSameInstallAttemptReceipt(expected, "repository", evidence.repositorySource, report) !=
        TILEXR_SUCCESS) {
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    if (ValidateSameInstallAttemptReceipt(expected, "local XN", evidence.localXnSource, report) != TILEXR_SUCCESS) {
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    if (RequiresRemoteXnBindingEvidence(package.plan.barrierMode) &&
        ValidateSameInstallAttemptReceipt(expected, "remote XN binding", evidence.remoteXnSource, report) !=
        TILEXR_SUCCESS) {
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    if (ValidateSameInstallAttemptReceipt(expected, "notify CKE install", evidence.notifyCkeSource, report) !=
        TILEXR_SUCCESS) {
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    if (RequiresChannelBindingEvidence(package.plan.barrierMode) &&
        ValidateSameInstallAttemptReceipt(expected, "channel binding", evidence.channelBindingSource, report) !=
        TILEXR_SUCCESS) {
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    return TILEXR_SUCCESS;
}

} // namespace

int TileXRCcuValidateHardwareInstall(
    const TileXRCcuLaunchPackage& package,
    const TileXRCcuHardwareInstallEvidence& evidence,
    TileXRCcuProviderReport* report)
{
    ResetReport(report);
    if (ValidatePackageShape(package, report) != TILEXR_SUCCESS) {
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    if (ValidateInstallEvidence(package, evidence, report) != TILEXR_SUCCESS) {
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    if (report != nullptr) {
        report->taskCount = static_cast<uint32_t>(package.tasks.size());
        report->repositoryCount = package.repository.repositoryCount;
        report->installedInstructionCount =
            static_cast<uint32_t>(package.repository.sqeLoadCount + package.repository.syncCount);
        report->submitReady = false;
        report->message = "hardware install evidence is validate-compatible";
    }
    return TILEXR_SUCCESS;
}

int TileXRCcuPrepareSubmitTasks(
    const TileXRCcuLaunchPackage& package,
    const TileXRCcuHardwareInstallEvidence& evidence,
    std::vector<TileXRCcuTask>* submitTasks,
    TileXRCcuProviderReport* report)
{
    ResetReport(report);
    if (submitTasks == nullptr) {
        return Fail(report, "missing output submit task vector");
    }
    submitTasks->clear();

    if (TileXRCcuValidateHardwareInstall(package, evidence, report) != TILEXR_SUCCESS) {
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    if (report != nullptr) {
        report->submitReady = false;
    }
    if (ValidateSubmitEvidence(package, evidence, report) != TILEXR_SUCCESS) {
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    *submitTasks = package.tasks;
    FillReadyReport(package, report);
    return TILEXR_SUCCESS;
}

} // namespace TileXR
