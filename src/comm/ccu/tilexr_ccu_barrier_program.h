/*
 * Copyright (c) 2026 TileXR Project
 * Licensed under the Apache License, Version 2.0
 */

#ifndef TILEXR_CCU_BARRIER_PROGRAM_H
#define TILEXR_CCU_BARRIER_PROGRAM_H

#include "ccu/tilexr_ccu_microcode.h"

#include <cstdint>
#include <string>
#include <vector>

namespace TileXR {

enum class TileXRCcuBarrierMode {
    SyncXn = 0,
    SyncCke = 1,
    LocalCke = 2,
    SyncXnPostOnly = 3,
    SyncXnLoadPostOnly = 4,
    SyncCkePostOnly = 5,
    LocalCkePostOnly = 6,
    SyncCkeSetWait = 7,
};

struct TileXRCcuBarrierSyncSpec {
    uint16_t remoteXn = 0;
    uint16_t localXn = 0;
    uint16_t channelId = 0;
    uint16_t remoteNotifyCke = 0;
    uint16_t remoteNotifyMask = 0;
    uint16_t localWaitCke = 0;
    uint16_t localWaitMask = 0;
    uint16_t sourceCke = 0;
    uint16_t sourceCkeMask = 0;
    bool clearLocalWait = true;
};

struct TileXRCcuBarrierProgramReport {
    uint32_t postInstructionCount = 0;
    uint32_t waitInstructionCount = 0;
    uint32_t totalInstructionCount = 0;
    std::string message;
};

int TileXRCcuBuildBarrierProgram(
    const std::vector<TileXRCcuBarrierSyncSpec>& specs,
    std::vector<TileXRCcuInstr>* program,
    TileXRCcuBarrierProgramReport* report,
    TileXRCcuBarrierMode mode = TileXRCcuBarrierMode::SyncXn);

} // namespace TileXR

#endif // TILEXR_CCU_BARRIER_PROGRAM_H
