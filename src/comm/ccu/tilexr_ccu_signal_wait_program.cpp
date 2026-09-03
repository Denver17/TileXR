/*
 * Copyright (c) 2026 TileXR Project
 * Licensed under the Apache License, Version 2.0
 */

#include "ccu/tilexr_ccu_signal_wait_program.h"

namespace TileXR {
namespace {

void ResetReport(TileXRCcuBarrierProgramReport* report)
{
    if (report != nullptr) {
        *report = TileXRCcuBarrierProgramReport {};
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

bool HasSignalResources(const TileXRCcuSignalWaitProgramSpec& spec)
{
    return spec.channelId != 0 &&
        spec.remoteNotifyCke != 0 &&
        spec.remoteNotifyMask != 0 &&
        spec.sourceCke != 0 &&
        spec.sourceCkeMask != 0;
}

bool HasWaitResources(const TileXRCcuSignalWaitProgramSpec& spec)
{
    return spec.localWaitCke != 0 &&
        spec.localWaitMask != 0;
}

TileXRCcuBarrierSyncSpec ToBarrierSpec(const TileXRCcuSignalWaitProgramSpec& spec)
{
    TileXRCcuBarrierSyncSpec barrier;
    barrier.channelId = spec.channelId;
    barrier.remoteXn = spec.remoteXn;
    barrier.localXn = spec.localXn;
    barrier.remoteNotifyCke = spec.remoteNotifyCke;
    barrier.remoteNotifyMask = spec.remoteNotifyMask;
    barrier.localWaitCke = spec.localWaitCke;
    barrier.localWaitMask = spec.localWaitMask;
    barrier.sourceCke = spec.sourceCke;
    barrier.sourceCkeMask = spec.sourceCkeMask;
    barrier.clearLocalWait = spec.clearLocalWait;
    return barrier;
}

int AppendWaitInstruction(
    const TileXRCcuSignalWaitProgramSpec& spec,
    std::vector<TileXRCcuInstr>* program,
    TileXRCcuBarrierProgramReport* report,
    bool setWait)
{
    if (!HasWaitResources(spec)) {
        return Fail(program, report, "missing wait CKE resource for direct CCU signal/wait program");
    }
    TileXRCcuCkeSpec wait;
    wait.waitCkeId = spec.localWaitCke;
    wait.waitMask = spec.localWaitMask;
    wait.clearWait = spec.clearLocalWait;
    TileXRCcuInstr instr;
    const int ret = setWait ?
        TileXRCcuEncodeSetCke(wait, &instr) :
        TileXRCcuEncodeClearCke(wait, &instr);
    if (ret != TILEXR_SUCCESS) {
        return Fail(program, report, "failed to encode wait instruction for direct CCU signal/wait program");
    }
    program->push_back(instr);
    return TILEXR_SUCCESS;
}

int AppendCommonPrelude(
    const TileXRCcuSignalWaitProgramSpec& spec,
    std::vector<TileXRCcuInstr>* program,
    TileXRCcuBarrierProgramReport* report)
{
    if (spec.localXn == 0 || spec.sourceCke == 0 || spec.sourceCkeMask == 0) {
        return Fail(program, report, "missing reserve resource for direct CCU signal/wait program");
    }
    TileXRCcuInstr instr;
    if (TileXRCcuEncodeLoadImdToXn(spec.localXn, 0, 0, &instr) != TILEXR_SUCCESS) {
        return Fail(program, report, "failed to encode signal/wait reserve XN prelude");
    }
    program->push_back(instr);

    if (spec.localGsa != 0) {
        if (TileXRCcuEncodeLoadImdToGsa(spec.localGsa, 0, &instr) != TILEXR_SUCCESS) {
            return Fail(program, report, "failed to encode signal/wait reserve GSA prelude");
        }
    } else if (TileXRCcuEncodeLoadImdToXn(spec.localXn, 0, 0, &instr) != TILEXR_SUCCESS) {
        return Fail(program, report, "failed to encode signal/wait reserve address prelude");
    }
    program->push_back(instr);

    TileXRCcuCkeSpec init;
    init.ckeId = spec.sourceCke;
    init.mask = spec.sourceCkeMask;
    init.clearWait = true;
    if (TileXRCcuEncodeSetCke(init, &instr) != TILEXR_SUCCESS) {
        return Fail(program, report, "failed to encode signal/wait reserve CKE prelude");
    }
    program->push_back(instr);
    return TILEXR_SUCCESS;
}

int AppendFinish(
    const TileXRCcuSignalWaitProgramSpec& spec,
    std::vector<TileXRCcuInstr>* program,
    TileXRCcuBarrierProgramReport* report)
{
    TileXRCcuInstr instr;
    if (TileXRCcuEncodeLoadImdToXn(spec.localXn, 0, 0, &instr) != TILEXR_SUCCESS) {
        return Fail(program, report, "failed to encode signal/wait finish instruction");
    }
    program->push_back(instr);
    return TILEXR_SUCCESS;
}

int AppendSignalInstruction(
    const TileXRCcuSignalWaitProgramSpec& spec,
    std::vector<TileXRCcuInstr>* program,
    TileXRCcuBarrierProgramReport* report)
{
    if (!HasSignalResources(spec)) {
        return Fail(program, report, "missing signal resource for direct CCU signal/wait program");
    }
    TileXRCcuSyncCkeSpec post;
    post.remoteCke = spec.remoteNotifyCke;
    post.localCke = spec.sourceCke;
    post.localCkeMask = spec.sourceCkeMask;
    post.channelId = spec.channelId;
    post.clearWait = spec.clearLocalWait;
    TileXRCcuInstr instr;
    if (TileXRCcuEncodeSyncCke(post, &instr) != TILEXR_SUCCESS) {
        return Fail(program, report, "failed to encode signal instruction for direct CCU signal/wait program");
    }
    program->push_back(instr);
    return TILEXR_SUCCESS;
}

int BuildWaitOnly(
    const TileXRCcuSignalWaitProgramSpec& spec,
    std::vector<TileXRCcuInstr>* program,
    TileXRCcuBarrierProgramReport* report)
{
    program->clear();
    const int ret = AppendWaitInstruction(spec, program, report, false);
    if (ret != TILEXR_SUCCESS) {
        return ret;
    }
    if (report != nullptr) {
        report->postInstructionCount = 0;
        report->waitInstructionCount = 1;
        report->totalInstructionCount = static_cast<uint32_t>(program->size());
        report->message = "ok";
    }
    return TILEXR_SUCCESS;
}

} // namespace

int TileXRCcuBuildSignalWaitProgram(
    const TileXRCcuSignalWaitProgramSpec& spec,
    std::vector<TileXRCcuInstr>* program,
    TileXRCcuBarrierProgramReport* report)
{
    ResetReport(report);
    if (program == nullptr) {
        return Fail(program, report, "missing output direct CCU signal/wait program");
    }
    program->clear();

    if (spec.role == TileXRCcuSignalWaitProgramRole::Wait) {
        return BuildWaitOnly(spec, program, report);
    }

    if (!HasSignalResources(spec)) {
        return Fail(program, report, "missing signal resource for direct CCU signal/wait program");
    }
    if (spec.role == TileXRCcuSignalWaitProgramRole::SignalAndWait && !HasWaitResources(spec)) {
        return Fail(program, report, "missing wait CKE resource for direct CCU signal/wait program");
    }

    if (AppendCommonPrelude(spec, program, report) != TILEXR_SUCCESS) {
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    if (AppendSignalInstruction(spec, program, report) != TILEXR_SUCCESS) {
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    if (spec.role == TileXRCcuSignalWaitProgramRole::SignalAndWait &&
        AppendWaitInstruction(spec, program, report, false) != TILEXR_SUCCESS) {
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    if (AppendFinish(spec, program, report) != TILEXR_SUCCESS) {
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    if (report != nullptr) {
        report->postInstructionCount = 1;
        report->waitInstructionCount = spec.role == TileXRCcuSignalWaitProgramRole::SignalAndWait ? 1U : 0U;
        report->totalInstructionCount = static_cast<uint32_t>(program->size());
        report->message = "ok";
    }
    return TILEXR_SUCCESS;
}

} // namespace TileXR
