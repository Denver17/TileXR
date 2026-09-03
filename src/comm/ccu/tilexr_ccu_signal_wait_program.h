/*
 * Copyright (c) 2026 TileXR Project
 * Licensed under the Apache License, Version 2.0
 */

#ifndef TILEXR_CCU_SIGNAL_WAIT_PROGRAM_H
#define TILEXR_CCU_SIGNAL_WAIT_PROGRAM_H

#include "ccu/tilexr_ccu_barrier_program.h"

namespace TileXR {

enum class TileXRCcuSignalWaitProgramRole {
    Signal = 0,
    Wait = 1,
    SignalAndWait = 2,
};

struct TileXRCcuSignalWaitProgramSpec {
    TileXRCcuSignalWaitProgramRole role = TileXRCcuSignalWaitProgramRole::Signal;
    uint16_t channelId = 0;
    uint16_t remoteXn = 0;
    uint16_t localXn = 0;
    uint16_t localGsa = 0;
    uint16_t remoteNotifyCke = 0;
    uint16_t remoteNotifyMask = 0;
    uint16_t localWaitCke = 0;
    uint16_t localWaitMask = 0;
    uint16_t sourceCke = 0;
    uint16_t sourceCkeMask = 0;
    bool clearLocalWait = true;
};

int TileXRCcuBuildSignalWaitProgram(
    const TileXRCcuSignalWaitProgramSpec& spec,
    std::vector<TileXRCcuInstr>* program,
    TileXRCcuBarrierProgramReport* report);

} // namespace TileXR

#endif // TILEXR_CCU_SIGNAL_WAIT_PROGRAM_H
