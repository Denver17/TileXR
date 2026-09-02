/*
 * Copyright (c) 2026 TileXR Project
 * Licensed under the Apache License, Version 2.0
 */

#ifndef TILEXR_CCU_MICROCODE_H
#define TILEXR_CCU_MICROCODE_H

#include "ccu/tilexr_ccu_runtime.h"

#include <cstdint>
#include <vector>

namespace TileXR {

struct TileXRCcuInstr {
    uint64_t words[4] = {};
};

struct TileXRCcuSyncXnSpec {
    uint16_t remoteXn = 0;
    uint16_t localXn = 0;
    uint16_t channelId = 0;
    uint16_t notifyCke = 0;
    uint16_t notifyMask = 0;
    uint16_t setCkeId = 0;
    uint16_t setCkeMask = 0;
    uint16_t waitCkeId = 0;
    uint16_t waitCkeMask = 0;
    bool clearWait = true;
};

struct TileXRCcuSyncCkeSpec {
    uint16_t remoteCke = 0;
    uint16_t localCke = 0;
    uint16_t localCkeMask = 0;
    uint16_t channelId = 0;
    uint16_t setCkeId = 0;
    uint16_t setCkeMask = 0;
    uint16_t waitCkeId = 0;
    uint16_t waitCkeMask = 0;
    bool clearWait = true;
};

struct TileXRCcuCkeSpec {
    uint16_t ckeId = 0;
    uint16_t mask = 0;
    uint16_t waitCkeId = 0;
    uint16_t waitMask = 0;
    bool clearWait = true;
};

struct TileXRCcuMemTransferSpec {
    uint16_t localGsa = 0;
    uint16_t localXn = 0;
    uint16_t remoteGsa = 0;
    uint16_t remoteXn = 0;
    uint16_t lengthXn = 0;
    uint16_t channelId = 0;
    uint16_t reduceDataType = 0;
    uint16_t reduceOpCode = 0;
    uint16_t setCkeId = 0;
    uint16_t setCkeMask = 0;
    uint16_t waitCkeId = 0;
    uint16_t waitCkeMask = 0;
    bool clearWait = true;
    bool lengthFromXn = true;
    bool reduceEnabled = false;
};

int TileXRCcuEncodeLoadSqeArgsToX(uint16_t xnId, uint32_t sqeArgId, TileXRCcuInstr* instr);

int TileXRCcuEncodeLoadImdToXn(uint16_t xnId, uint64_t immediate, uint16_t secFlag, TileXRCcuInstr* instr);

int TileXRCcuEncodeLoadImdToGsa(uint16_t gsaId, uint64_t immediate, TileXRCcuInstr* instr);

int TileXRCcuEncodeSyncXn(const TileXRCcuSyncXnSpec& spec, TileXRCcuInstr* instr);

int TileXRCcuEncodeSyncCke(const TileXRCcuSyncCkeSpec& spec, TileXRCcuInstr* instr);

int TileXRCcuEncodeSetCke(const TileXRCcuCkeSpec& spec, TileXRCcuInstr* instr);

int TileXRCcuEncodeClearCke(const TileXRCcuCkeSpec& spec, TileXRCcuInstr* instr);

int TileXRCcuEncodeTransRmtMemToLocMem(const TileXRCcuMemTransferSpec& spec, TileXRCcuInstr* instr);

int TileXRCcuEncodeTransLocMemToRmtMem(const TileXRCcuMemTransferSpec& spec, TileXRCcuInstr* instr);

int TileXRCcuBuildSqeLoadProgram(
    uint16_t firstXnId,
    uint32_t argCount,
    std::vector<TileXRCcuInstr>* program);

int TileXRCcuBuildSyncProgram(
    const std::vector<TileXRCcuSyncXnSpec>& specs,
    std::vector<TileXRCcuInstr>* program);

} // namespace TileXR

#endif // TILEXR_CCU_MICROCODE_H
