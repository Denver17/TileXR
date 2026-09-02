/*
 * Copyright (c) 2026 TileXR Project
 * Licensed under the Apache License, Version 2.0
 */

#include "ccu/tilexr_ccu_memory_program.h"

namespace TileXR {
namespace {

constexpr uint64_t TILEXR_CCU_TOKEN_VALID_SHIFT = 52ULL;
constexpr uint64_t TILEXR_CCU_TOKEN_ID_SHIFT = 32ULL;
constexpr uint64_t TILEXR_CCU_TOKEN_ID_MASK = 0xfffffULL;
constexpr uint64_t TILEXR_CCU_TOKEN_VALUE_MASK = 0xffffffffULL;

void ResetReport(TileXRCcuMemoryProgramReport* report)
{
    if (report != nullptr) {
        *report = TileXRCcuMemoryProgramReport{};
    }
}

int Fail(
    std::vector<TileXRCcuInstr>* program,
    TileXRCcuMemoryProgramReport* report,
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

int ValidateSpec(
    const TileXRCcuMemoryCopySpec& spec,
    std::vector<TileXRCcuInstr>* program,
    TileXRCcuMemoryProgramReport* report)
{
    if (program == nullptr) {
        return Fail(program, report, "missing output CCU memory copy program");
    }
    if (spec.direction != TileXRCcuMemoryCopyDirection::RemoteToLocal &&
        spec.direction != TileXRCcuMemoryCopyDirection::LocalToRemote) {
        return Fail(program, report, "invalid CCU memory copy direction");
    }
    if (spec.localGsa == 0 || spec.localXn == 0 || spec.remoteGsa == 0 || spec.remoteXn == 0 ||
        spec.lengthXn == 0) {
        return Fail(program, report, "missing CCU memory copy GSA/XN resources");
    }
    if (spec.localAddr == 0 || spec.localToken == 0 || spec.remoteAddr == 0 || spec.remoteToken == 0) {
        return Fail(program, report, "missing CCU memory copy address/token inputs");
    }
    if (spec.lengthBytes == 0) {
        return Fail(program, report, "missing CCU memory copy length");
    }
    if (spec.channelId == 0) {
        return Fail(program, report, "missing CCU memory copy channel");
    }
    if (spec.completionCke == 0 || spec.completionMask == 0) {
        return Fail(program, report, "missing CCU memory copy completion CKE");
    }
    if (spec.reduceDataType > 0xfU || spec.reduceOpCode > 0xfU) {
        return Fail(program, report, "CCU memory copy reduce fields exceed v1 encoding width");
    }
    return TILEXR_SUCCESS;
}

int AppendLoadImmediates(
    const TileXRCcuMemoryCopySpec& spec,
    std::vector<TileXRCcuInstr>* program,
    TileXRCcuMemoryProgramReport* report)
{
    TileXRCcuInstr instr;
    if (TileXRCcuEncodeLoadImdToGsa(spec.localGsa, spec.localAddr, &instr) != TILEXR_SUCCESS) {
        return Fail(program, report, "failed to encode local memory address GSA load");
    }
    program->push_back(instr);

    if (TileXRCcuEncodeLoadImdToXn(spec.localXn, spec.localToken, 1U, &instr) != TILEXR_SUCCESS) {
        return Fail(program, report, "failed to encode local memory token XN load");
    }
    program->push_back(instr);

    if (TileXRCcuEncodeLoadImdToGsa(spec.remoteGsa, spec.remoteAddr, &instr) != TILEXR_SUCCESS) {
        return Fail(program, report, "failed to encode remote memory address GSA load");
    }
    program->push_back(instr);

    if (TileXRCcuEncodeLoadImdToXn(spec.remoteXn, spec.remoteToken, 1U, &instr) != TILEXR_SUCCESS) {
        return Fail(program, report, "failed to encode remote memory token XN load");
    }
    program->push_back(instr);

    if (TileXRCcuEncodeLoadImdToXn(spec.lengthXn, spec.lengthBytes, 0, &instr) != TILEXR_SUCCESS) {
        return Fail(program, report, "failed to encode memory copy length XN load");
    }
    program->push_back(instr);
    return TILEXR_SUCCESS;
}

int AppendTransfer(
    const TileXRCcuMemoryCopySpec& spec,
    std::vector<TileXRCcuInstr>* program,
    TileXRCcuMemoryProgramReport* report)
{
    TileXRCcuMemTransferSpec transfer;
    transfer.localGsa = spec.localGsa;
    transfer.localXn = spec.localXn;
    transfer.remoteGsa = spec.remoteGsa;
    transfer.remoteXn = spec.remoteXn;
    transfer.lengthXn = spec.lengthXn;
    transfer.channelId = spec.channelId;
    transfer.reduceDataType = spec.reduceDataType;
    transfer.reduceOpCode = spec.reduceOpCode;
    transfer.setCkeId = spec.completionCke;
    transfer.setCkeMask = spec.completionMask;
    transfer.clearWait = true;
    transfer.lengthFromXn = true;
    transfer.reduceEnabled = spec.reduceEnabled;

    TileXRCcuInstr instr;
    const int ret = spec.direction == TileXRCcuMemoryCopyDirection::RemoteToLocal ?
        TileXRCcuEncodeTransRmtMemToLocMem(transfer, &instr) :
        TileXRCcuEncodeTransLocMemToRmtMem(transfer, &instr);
    if (ret != TILEXR_SUCCESS) {
        return Fail(program, report, "failed to encode CCU memory transfer instruction");
    }
    program->push_back(instr);
    return TILEXR_SUCCESS;
}

int AppendCompletionWait(
    const TileXRCcuMemoryCopySpec& spec,
    std::vector<TileXRCcuInstr>* program,
    TileXRCcuMemoryProgramReport* report)
{
    TileXRCcuCkeSpec wait;
    wait.waitCkeId = spec.completionCke;
    wait.waitMask = spec.completionMask;
    wait.clearWait = true;

    TileXRCcuInstr instr;
    if (TileXRCcuEncodeClearCke(wait, &instr) != TILEXR_SUCCESS) {
        return Fail(program, report, "failed to encode CCU memory copy completion wait");
    }
    program->push_back(instr);
    return TILEXR_SUCCESS;
}

void FillReport(const std::vector<TileXRCcuInstr>& program, TileXRCcuMemoryProgramReport* report)
{
    if (report == nullptr) {
        return;
    }
    report->loadInstructionCount = 5;
    report->transferInstructionCount = 1;
    report->waitInstructionCount = 1;
    report->totalInstructionCount = static_cast<uint32_t>(program.size());
    report->message = "ok";
}

} // namespace

uint64_t TileXRCcuPackMemoryToken(uint32_t tokenId, uint32_t tokenValue, bool valid)
{
    const uint64_t validBits = valid ? 1ULL : 0ULL;
    return (validBits << TILEXR_CCU_TOKEN_VALID_SHIFT) |
        ((static_cast<uint64_t>(tokenId) & TILEXR_CCU_TOKEN_ID_MASK) << TILEXR_CCU_TOKEN_ID_SHIFT) |
        (static_cast<uint64_t>(tokenValue) & TILEXR_CCU_TOKEN_VALUE_MASK);
}

int TileXRCcuBuildMemoryCopyProgram(
    const TileXRCcuMemoryCopySpec& spec,
    std::vector<TileXRCcuInstr>* program,
    TileXRCcuMemoryProgramReport* report)
{
    ResetReport(report);
    if (program != nullptr) {
        program->clear();
    }
    int ret = ValidateSpec(spec, program, report);
    if (ret != TILEXR_SUCCESS) {
        return ret;
    }

    program->reserve(7);
    ret = AppendLoadImmediates(spec, program, report);
    if (ret != TILEXR_SUCCESS) {
        return ret;
    }
    ret = AppendTransfer(spec, program, report);
    if (ret != TILEXR_SUCCESS) {
        return ret;
    }
    ret = AppendCompletionWait(spec, program, report);
    if (ret != TILEXR_SUCCESS) {
        return ret;
    }

    FillReport(*program, report);
    return TILEXR_SUCCESS;
}

} // namespace TileXR
