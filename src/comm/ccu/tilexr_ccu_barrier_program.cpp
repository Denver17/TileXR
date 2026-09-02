/*
 * Copyright (c) 2026 TileXR Project
 * Licensed under the Apache License, Version 2.0
 */

#include "ccu/tilexr_ccu_barrier_program.h"

namespace TileXR {
namespace {

void ResetReport(TileXRCcuBarrierProgramReport* report)
{
    if (report != nullptr) {
        *report = TileXRCcuBarrierProgramReport{};
    }
}

int Fail(
    std::vector<TileXRCcuInstr>* program,
    TileXRCcuBarrierProgramReport* report,
    const std::string& message)
{
    if (program != nullptr) {
        program->clear();
    }
    if (report != nullptr) {
        report->message = message;
    }
    return TILEXR_ERROR_PARA_CHECK_FAIL;
}

bool MissingPostResource(const TileXRCcuBarrierSyncSpec& spec)
{
    return spec.remoteXn == 0 || spec.localXn == 0 || spec.channelId == 0 ||
        spec.remoteNotifyCke == 0 || spec.remoteNotifyMask == 0;
}

bool MissingSyncCkePostResource(const TileXRCcuBarrierSyncSpec& spec)
{
    return spec.channelId == 0 || spec.remoteNotifyCke == 0 || spec.remoteNotifyMask == 0 ||
        spec.sourceCke == 0 || spec.sourceCkeMask == 0;
}

bool MissingWaitResource(const TileXRCcuBarrierSyncSpec& spec)
{
    return spec.localWaitCke == 0 || spec.localWaitMask == 0;
}

void FillReport(
    size_t specCount,
    size_t totalInstructionCount,
    TileXRCcuBarrierProgramReport* report,
    bool hasWaitInstructions = true)
{
    if (report == nullptr) {
        return;
    }
    report->postInstructionCount = static_cast<uint32_t>(specCount);
    report->waitInstructionCount = hasWaitInstructions ? static_cast<uint32_t>(specCount) : 0U;
    report->totalInstructionCount = static_cast<uint32_t>(totalInstructionCount);
    report->message = "ok";
}

bool LoadBeforePostOnly(TileXRCcuBarrierMode mode)
{
    return mode == TileXRCcuBarrierMode::SyncXnLoadPostOnly;
}

bool PostOnly(TileXRCcuBarrierMode mode)
{
    return mode == TileXRCcuBarrierMode::SyncXnPostOnly ||
        mode == TileXRCcuBarrierMode::SyncXnLoadPostOnly ||
        mode == TileXRCcuBarrierMode::SyncCkePostOnly ||
        mode == TileXRCcuBarrierMode::LocalCkePostOnly;
}

bool SyncCkeMode(TileXRCcuBarrierMode mode)
{
    return mode == TileXRCcuBarrierMode::SyncCke ||
        mode == TileXRCcuBarrierMode::SyncCkeSetWait ||
        mode == TileXRCcuBarrierMode::SyncCkePostOnly;
}

bool SyncCkeSetWaitMode(TileXRCcuBarrierMode mode)
{
    return mode == TileXRCcuBarrierMode::SyncCkeSetWait;
}

bool HasConflictingSourceCkeMasks(const std::vector<TileXRCcuBarrierSyncSpec>& specs)
{
    for (size_t i = 0; i < specs.size(); ++i) {
        for (size_t j = i + 1; j < specs.size(); ++j) {
            if (specs[i].sourceCke == specs[j].sourceCke &&
                specs[i].sourceCkeMask != specs[j].sourceCkeMask) {
                return true;
            }
        }
    }
    return false;
}

void AddSourceCkeInitInstructions(
    const std::vector<TileXRCcuBarrierSyncSpec>& specs,
    std::vector<TileXRCcuInstr>* program)
{
    std::vector<uint16_t> initialized;
    for (const auto& spec : specs) {
        bool seen = false;
        for (uint16_t cke : initialized) {
            if (cke == spec.sourceCke) {
                seen = true;
                break;
            }
        }
        if (seen) {
            continue;
        }
        TileXRCcuCkeSpec init;
        init.ckeId = spec.sourceCke;
        init.mask = spec.sourceCkeMask;
        init.clearWait = true;
        TileXRCcuInstr instr;
        if (TileXRCcuEncodeSetCke(init, &instr) == TILEXR_SUCCESS) {
            program->push_back(instr);
            initialized.push_back(spec.sourceCke);
        }
    }
}

int AddLocalCkeDiagnosticInstructions(
    const std::vector<TileXRCcuBarrierSyncSpec>& specs,
    std::vector<TileXRCcuInstr>* program,
    TileXRCcuBarrierProgramReport* report)
{
    for (const auto& spec : specs) {
        TileXRCcuCkeSpec post;
        post.ckeId = spec.localWaitCke;
        post.mask = spec.localWaitMask;
        post.clearWait = false;
        TileXRCcuInstr instr;
        if (TileXRCcuEncodeSetCke(post, &instr) != TILEXR_SUCCESS) {
            return Fail(program, report, "failed to encode local CKE diagnostic set instruction");
        }
        program->push_back(instr);
    }

    for (const auto& spec : specs) {
        TileXRCcuCkeSpec wait;
        wait.waitCkeId = spec.localWaitCke;
        wait.waitMask = spec.localWaitMask;
        wait.clearWait = spec.clearLocalWait;
        TileXRCcuInstr instr;
        if (TileXRCcuEncodeClearCke(wait, &instr) != TILEXR_SUCCESS) {
            return Fail(program, report, "failed to encode local CKE diagnostic wait instruction");
        }
        program->push_back(instr);
    }

    return TILEXR_SUCCESS;
}

int AddLocalCkePostOnlyDiagnosticInstructions(
    const std::vector<TileXRCcuBarrierSyncSpec>& specs,
    std::vector<TileXRCcuInstr>* program,
    TileXRCcuBarrierProgramReport* report)
{
    for (const auto& spec : specs) {
        TileXRCcuCkeSpec post;
        post.ckeId = spec.localWaitCke;
        post.mask = spec.localWaitMask;
        post.clearWait = false;
        TileXRCcuInstr instr;
        if (TileXRCcuEncodeSetCke(post, &instr) != TILEXR_SUCCESS) {
            return Fail(program, report, "failed to encode local CKE post-only diagnostic set instruction");
        }
        program->push_back(instr);
    }
    return TILEXR_SUCCESS;
}

} // namespace

int TileXRCcuBuildBarrierProgram(
    const std::vector<TileXRCcuBarrierSyncSpec>& specs,
    std::vector<TileXRCcuInstr>* program,
    TileXRCcuBarrierProgramReport* report,
    TileXRCcuBarrierMode mode)
{
    ResetReport(report);
    if (program == nullptr) {
        return Fail(program, report, "missing output CCU barrier program");
    }
    program->clear();
    if (specs.empty()) {
        return Fail(program, report, "missing CCU barrier sync specs");
    }

    for (const auto& spec : specs) {
        if (mode != TileXRCcuBarrierMode::LocalCke &&
            mode != TileXRCcuBarrierMode::LocalCkePostOnly &&
            (SyncCkeMode(mode) ? MissingSyncCkePostResource(spec) :
                MissingPostResource(spec))) {
            return Fail(program, report, "missing remote XN post resource for CCU barrier program");
        }
        if (!PostOnly(mode) && MissingWaitResource(spec)) {
            return Fail(program, report, "missing local wait CKE resource for CCU barrier program");
        }
    }
    if (SyncCkeMode(mode) && HasConflictingSourceCkeMasks(specs)) {
        return Fail(program, report, "conflicting source CKE masks for CCU barrier program");
    }

    program->reserve(specs.size() * (SyncCkeMode(mode) ? 3U : 2U));
    if (mode == TileXRCcuBarrierMode::LocalCke) {
        const int ret = AddLocalCkeDiagnosticInstructions(specs, program, report);
        if (ret != TILEXR_SUCCESS) {
            return ret;
        }
        FillReport(specs.size(), program->size(), report);
        return TILEXR_SUCCESS;
    }
    if (mode == TileXRCcuBarrierMode::LocalCkePostOnly) {
        const int ret = AddLocalCkePostOnlyDiagnosticInstructions(specs, program, report);
        if (ret != TILEXR_SUCCESS) {
            return ret;
        }
        FillReport(specs.size(), program->size(), report, false);
        return TILEXR_SUCCESS;
    }
    if (SyncCkeMode(mode)) {
        AddSourceCkeInitInstructions(specs, program);
    }

    for (const auto& spec : specs) {
        TileXRCcuInstr instr;
        if (SyncCkeMode(mode)) {
            TileXRCcuSyncCkeSpec post;
            post.remoteCke = spec.remoteNotifyCke;
            post.localCke = spec.sourceCke;
            post.localCkeMask = spec.sourceCkeMask;
            post.channelId = spec.channelId;
            if (TileXRCcuEncodeSyncCke(post, &instr) != TILEXR_SUCCESS) {
                return Fail(program, report, "failed to encode CCU barrier SyncCKE post instruction");
            }
        } else {
            if (LoadBeforePostOnly(mode)) {
                if (TileXRCcuEncodeLoadImdToXn(spec.localXn, 1U, 0, &instr) != TILEXR_SUCCESS) {
                    return Fail(program, report, "failed to encode CCU barrier local XN load instruction");
                }
                program->push_back(instr);
            }
            TileXRCcuSyncXnSpec post;
            post.remoteXn = spec.remoteXn;
            post.localXn = spec.localXn;
            post.channelId = spec.channelId;
            post.notifyCke = spec.remoteNotifyCke;
            post.notifyMask = spec.remoteNotifyMask;
            if (TileXRCcuEncodeSyncXn(post, &instr) != TILEXR_SUCCESS) {
                return Fail(program, report, "failed to encode CCU barrier post instruction");
            }
        }
        program->push_back(instr);
    }

    if (PostOnly(mode)) {
        FillReport(specs.size(), program->size(), report, false);
        return TILEXR_SUCCESS;
    }

    for (const auto& spec : specs) {
        TileXRCcuCkeSpec wait;
        wait.waitCkeId = spec.localWaitCke;
        wait.waitMask = spec.localWaitMask;
        wait.clearWait = spec.clearLocalWait;
        TileXRCcuInstr instr;
        const int ret = SyncCkeMode(mode) && !SyncCkeSetWaitMode(mode) ?
            TileXRCcuEncodeClearCke(wait, &instr) :
            TileXRCcuEncodeSetCke(wait, &instr);
        if (ret != TILEXR_SUCCESS) {
            return Fail(program, report, "failed to encode CCU barrier wait/clear instruction");
        }
        program->push_back(instr);
    }

    FillReport(specs.size(), program->size(), report);
    return TILEXR_SUCCESS;
}

} // namespace TileXR
