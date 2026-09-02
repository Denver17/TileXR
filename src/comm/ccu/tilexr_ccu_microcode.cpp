/*
 * Copyright (c) 2026 TileXR Project
 * Licensed under the Apache License, Version 2.0
 */

#include "ccu/tilexr_ccu_microcode.h"

namespace TileXR {
namespace {

constexpr uint64_t TILEXR_CCU_LOAD_SQE_ARGS_TO_X_HEADER = 0x0001U;
constexpr uint64_t TILEXR_CCU_LOAD_IMD_TO_GSA_HEADER = 0x0002U;
constexpr uint64_t TILEXR_CCU_LOAD_IMD_TO_XN_HEADER = 0x0003U;
constexpr uint64_t TILEXR_CCU_SET_CKE_HEADER = 0x0802U;
constexpr uint64_t TILEXR_CCU_CLEAR_CKE_HEADER = 0x0804U;
constexpr uint64_t TILEXR_CCU_TRANS_RMT_MEM_TO_LOC_MEM_HEADER = 0x1008U;
constexpr uint64_t TILEXR_CCU_TRANS_LOC_MEM_TO_RMT_MEM_HEADER = 0x1009U;
constexpr uint64_t TILEXR_CCU_SYNC_CKE_HEADER = 0x100bU;
constexpr uint64_t TILEXR_CCU_SYNC_XN_HEADER = 0x100dU;
constexpr uint64_t TILEXR_CCU_SYNC_XN_TRACE_FLAG = 0x0001000000000000ULL;

void ClearInstr(TileXRCcuInstr* instr)
{
    for (auto& word : instr->words) {
        word = 0;
    }
}

int ValidateInstrOutput(TileXRCcuInstr* instr)
{
    if (instr == nullptr) {
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    ClearInstr(instr);
    return TILEXR_SUCCESS;
}

uint64_t PackSlots(uint16_t slot0, uint16_t slot1, uint16_t slot2, uint16_t slot3)
{
    return static_cast<uint64_t>(slot0) |
        (static_cast<uint64_t>(slot1) << 16U) |
        (static_cast<uint64_t>(slot2) << 32U) |
        (static_cast<uint64_t>(slot3) << 48U);
}

uint16_t ClearTypeBit(bool clearWait)
{
    return clearWait ? 1U : 0U;
}

uint16_t TransferControlSlot(const TileXRCcuMemTransferSpec& spec)
{
    constexpr uint16_t udfType = 0;
    return static_cast<uint16_t>(udfType |
        (static_cast<uint16_t>(spec.reduceDataType) << 8U) |
        (static_cast<uint16_t>(spec.reduceOpCode) << 12U));
}

uint16_t TransferFlagSlot(const TileXRCcuMemTransferSpec& spec)
{
    return static_cast<uint16_t>(
        (spec.clearWait ? 1U : 0U) |
        (spec.lengthFromXn ? 2U : 0U) |
        (spec.reduceEnabled ? 4U : 0U));
}

int ValidateTransferSpec(const TileXRCcuMemTransferSpec& spec)
{
    if (spec.localGsa == 0 || spec.localXn == 0 || spec.remoteGsa == 0 || spec.remoteXn == 0 ||
        spec.lengthXn == 0 || spec.channelId == 0 || spec.reduceDataType > 0xfU || spec.reduceOpCode > 0xfU) {
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    if ((spec.setCkeId == 0) != (spec.setCkeMask == 0) ||
        (spec.waitCkeId == 0) != (spec.waitCkeMask == 0)) {
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    return TILEXR_SUCCESS;
}

bool CompleteOrAbsentCkePair(uint16_t ckeId, uint16_t mask)
{
    return (ckeId == 0) == (mask == 0);
}

void WriteLe16(uint8_t* bytes, size_t offset, uint16_t value)
{
    bytes[offset] = static_cast<uint8_t>(value & 0xffU);
    bytes[offset + 1U] = static_cast<uint8_t>((value >> 8U) & 0xffU);
}

void WriteLe64(uint8_t* bytes, size_t offset, uint64_t value)
{
    for (size_t i = 0; i < sizeof(uint64_t); ++i) {
        bytes[offset + i] = static_cast<uint8_t>((value >> (i * 8U)) & 0xffU);
    }
}

uint64_t ReadLe64(const uint8_t* bytes, size_t offset)
{
    uint64_t value = 0;
    for (size_t i = 0; i < sizeof(uint64_t); ++i) {
        value |= static_cast<uint64_t>(bytes[offset + i]) << (i * 8U);
    }
    return value;
}

} // namespace

int TileXRCcuEncodeLoadSqeArgsToX(uint16_t xnId, uint32_t sqeArgId, TileXRCcuInstr* instr)
{
    if (ValidateInstrOutput(instr) != TILEXR_SUCCESS) {
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    if (xnId == 0 || sqeArgId >= TILEXR_CCU_SQE_ARGS_LEN) {
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }

    instr->words[0] = PackSlots(static_cast<uint16_t>(TILEXR_CCU_LOAD_SQE_ARGS_TO_X_HEADER), xnId,
        static_cast<uint16_t>(sqeArgId), 0);
    return TILEXR_SUCCESS;
}

int TileXRCcuEncodeLoadImdToXn(uint16_t xnId, uint64_t immediate, uint16_t secFlag, TileXRCcuInstr* instr)
{
    if (ValidateInstrOutput(instr) != TILEXR_SUCCESS) {
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    if (xnId == 0) {
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }

    uint8_t bytes[sizeof(TileXRCcuInstr)] = {};
    WriteLe16(bytes, 0, static_cast<uint16_t>(TILEXR_CCU_LOAD_IMD_TO_XN_HEADER));
    WriteLe16(bytes, 2, xnId);
    WriteLe64(bytes, 4, immediate);
    WriteLe16(bytes, 12, secFlag);
    for (size_t i = 0; i < 4U; ++i) {
        instr->words[i] = ReadLe64(bytes, i * sizeof(uint64_t));
    }
    return TILEXR_SUCCESS;
}

int TileXRCcuEncodeLoadImdToGsa(uint16_t gsaId, uint64_t immediate, TileXRCcuInstr* instr)
{
    if (ValidateInstrOutput(instr) != TILEXR_SUCCESS) {
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    if (gsaId == 0) {
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }

    uint8_t bytes[sizeof(TileXRCcuInstr)] = {};
    WriteLe16(bytes, 0, static_cast<uint16_t>(TILEXR_CCU_LOAD_IMD_TO_GSA_HEADER));
    WriteLe16(bytes, 2, gsaId);
    WriteLe64(bytes, 4, immediate);
    for (size_t i = 0; i < 4U; ++i) {
        instr->words[i] = ReadLe64(bytes, i * sizeof(uint64_t));
    }
    return TILEXR_SUCCESS;
}

int TileXRCcuEncodeSyncXn(const TileXRCcuSyncXnSpec& spec, TileXRCcuInstr* instr)
{
    if (ValidateInstrOutput(instr) != TILEXR_SUCCESS) {
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    if (spec.remoteXn == 0 || spec.localXn == 0 || spec.channelId == 0 || spec.notifyCke == 0 ||
        spec.notifyMask == 0) {
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }

    instr->words[0] = PackSlots(static_cast<uint16_t>(TILEXR_CCU_SYNC_XN_HEADER), spec.remoteXn, spec.localXn, 0);
    instr->words[1] = PackSlots(spec.channelId, spec.notifyCke, spec.notifyMask, 0);
    instr->words[2] = spec.clearWait ? TILEXR_CCU_SYNC_XN_TRACE_FLAG : 0;
    instr->words[3] = PackSlots(spec.setCkeId, spec.setCkeMask, spec.waitCkeId, spec.waitCkeMask);
    return TILEXR_SUCCESS;
}

int TileXRCcuEncodeSyncCke(const TileXRCcuSyncCkeSpec& spec, TileXRCcuInstr* instr)
{
    if (ValidateInstrOutput(instr) != TILEXR_SUCCESS) {
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    if (spec.remoteCke == 0 || spec.localCke == 0 || spec.localCkeMask == 0 || spec.channelId == 0) {
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }

    instr->words[0] = PackSlots(
        static_cast<uint16_t>(TILEXR_CCU_SYNC_CKE_HEADER),
        spec.remoteCke,
        spec.localCke,
        spec.localCkeMask);
    instr->words[1] = PackSlots(spec.channelId, 0, 0, 0);
    instr->words[2] = PackSlots(0, 0, 0, ClearTypeBit(spec.clearWait));
    instr->words[3] = PackSlots(spec.setCkeId, spec.setCkeMask, spec.waitCkeId, spec.waitCkeMask);
    return TILEXR_SUCCESS;
}

int TileXRCcuEncodeSetCke(const TileXRCcuCkeSpec& spec, TileXRCcuInstr* instr)
{
    if (ValidateInstrOutput(instr) != TILEXR_SUCCESS) {
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    if (!CompleteOrAbsentCkePair(spec.ckeId, spec.mask) ||
        !CompleteOrAbsentCkePair(spec.waitCkeId, spec.waitMask) ||
        (spec.ckeId == 0 && spec.waitCkeId == 0)) {
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }

    instr->words[0] = PackSlots(static_cast<uint16_t>(TILEXR_CCU_SET_CKE_HEADER), ClearTypeBit(spec.clearWait),
        spec.ckeId, spec.mask);
    instr->words[1] = PackSlots(spec.waitCkeId, spec.waitMask, 0, 0);
    return TILEXR_SUCCESS;
}

int TileXRCcuEncodeClearCke(const TileXRCcuCkeSpec& spec, TileXRCcuInstr* instr)
{
    if (ValidateInstrOutput(instr) != TILEXR_SUCCESS) {
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    if (!CompleteOrAbsentCkePair(spec.ckeId, spec.mask) ||
        !CompleteOrAbsentCkePair(spec.waitCkeId, spec.waitMask) ||
        (spec.ckeId == 0 && spec.waitCkeId == 0)) {
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }

    instr->words[0] = PackSlots(static_cast<uint16_t>(TILEXR_CCU_CLEAR_CKE_HEADER), ClearTypeBit(spec.clearWait),
        spec.ckeId, spec.mask);
    instr->words[1] = PackSlots(spec.waitCkeId, spec.waitMask, 0, 0);
    return TILEXR_SUCCESS;
}

int TileXRCcuEncodeTransRmtMemToLocMem(const TileXRCcuMemTransferSpec& spec, TileXRCcuInstr* instr)
{
    if (ValidateInstrOutput(instr) != TILEXR_SUCCESS || ValidateTransferSpec(spec) != TILEXR_SUCCESS) {
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }

    instr->words[0] = PackSlots(
        static_cast<uint16_t>(TILEXR_CCU_TRANS_RMT_MEM_TO_LOC_MEM_HEADER),
        spec.localGsa,
        spec.localXn,
        spec.remoteGsa);
    instr->words[1] = PackSlots(spec.remoteXn, spec.lengthXn, spec.channelId, TransferControlSlot(spec));
    instr->words[2] = PackSlots(0, 0, 0, TransferFlagSlot(spec));
    instr->words[3] = PackSlots(spec.setCkeId, spec.setCkeMask, spec.waitCkeId, spec.waitCkeMask);
    return TILEXR_SUCCESS;
}

int TileXRCcuEncodeTransLocMemToRmtMem(const TileXRCcuMemTransferSpec& spec, TileXRCcuInstr* instr)
{
    if (ValidateInstrOutput(instr) != TILEXR_SUCCESS || ValidateTransferSpec(spec) != TILEXR_SUCCESS) {
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }

    instr->words[0] = PackSlots(
        static_cast<uint16_t>(TILEXR_CCU_TRANS_LOC_MEM_TO_RMT_MEM_HEADER),
        spec.remoteGsa,
        spec.remoteXn,
        spec.localGsa);
    instr->words[1] = PackSlots(spec.localXn, spec.lengthXn, spec.channelId, TransferControlSlot(spec));
    instr->words[2] = PackSlots(0, 0, 0, TransferFlagSlot(spec));
    instr->words[3] = PackSlots(spec.setCkeId, spec.setCkeMask, spec.waitCkeId, spec.waitCkeMask);
    return TILEXR_SUCCESS;
}

int TileXRCcuBuildSqeLoadProgram(uint16_t firstXnId, uint32_t argCount, std::vector<TileXRCcuInstr>* program)
{
    if (program == nullptr || firstXnId == 0 || argCount == 0 || argCount > TILEXR_CCU_SQE_ARGS_LEN) {
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }

    program->clear();
    program->reserve(argCount);
    for (uint32_t argId = 0; argId < argCount; ++argId) {
        TileXRCcuInstr instr;
        const uint32_t xnId = static_cast<uint32_t>(firstXnId) + argId;
        if (xnId > UINT16_MAX || TileXRCcuEncodeLoadSqeArgsToX(static_cast<uint16_t>(xnId), argId, &instr) !=
                TILEXR_SUCCESS) {
            program->clear();
            return TILEXR_ERROR_PARA_CHECK_FAIL;
        }
        program->push_back(instr);
    }
    return TILEXR_SUCCESS;
}

int TileXRCcuBuildSyncProgram(const std::vector<TileXRCcuSyncXnSpec>& specs, std::vector<TileXRCcuInstr>* program)
{
    if (program == nullptr || specs.empty()) {
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }

    program->clear();
    program->reserve(specs.size());
    for (const auto& spec : specs) {
        TileXRCcuInstr instr;
        if (TileXRCcuEncodeSyncXn(spec, &instr) != TILEXR_SUCCESS) {
            program->clear();
            return TILEXR_ERROR_PARA_CHECK_FAIL;
        }
        program->push_back(instr);
    }
    return TILEXR_SUCCESS;
}

} // namespace TileXR
