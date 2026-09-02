/*
 * Copyright (c) 2026 TileXR Project
 * Licensed under the Apache License, Version 2.0
 */

#ifndef TILEXR_CCU_PRODUCER_PLAN_H
#define TILEXR_CCU_PRODUCER_PLAN_H

#include "ccu/tilexr_ccu_barrier_program.h"
#include "ccu/tilexr_ccu_runtime.h"

#include <cstdint>
#include <string>
#include <vector>

namespace TileXR {

struct TileXRCcuRange {
    uint8_t dieId = 0;
    uint16_t startId = 0;
    uint16_t num = 0;
};

struct TileXRCcuMission {
    uint8_t dieId = 0;
    uint8_t missionId = 0;
    uint32_t key = 0;
    bool installed = false;
};

struct TileXRCcuInstructionWindow {
    uint8_t dieId = 0;
    uint16_t repositoryStartId = 0;
    uint16_t repositoryCount = 0;
    uint16_t missionStartId = 0;
    uint16_t missionCount = 0;
};

struct TileXRCcuSyncResource {
    uint8_t dieId = 0;
    uint16_t localXn = 0;
    uint16_t remoteXn = 0;
    uint16_t notifyCke = 0;
    uint16_t channelId = 0;
    uint16_t bindingCount = 0;
    uint16_t localWaitCke = 0;
    uint16_t localWaitMask = 1;
    uint16_t remoteNotifyMask = 1;
    uint16_t sourceCke = 0;
    uint16_t sourceCkeMask = 0xffff;
};

struct TileXRCcuTaskWindow {
    uint8_t dieId = 0;
    uint16_t instStartId = 0;
    uint16_t instCnt = 0;
    uint32_t argSize = 0;
    std::vector<uint64_t> args;
};

struct TileXRCcuProducerPlan {
    TileXRCcuMission mission;
    TileXRCcuRange kernelLocalXn;
    TileXRCcuRange kernelLocalGsa;
    TileXRCcuRange kernelLocalCke;
    TileXRCcuRange kernelLocalMission;
    TileXRCcuInstructionWindow instructionWindow;
    std::vector<TileXRCcuSyncResource> syncResources;
    std::vector<TileXRCcuTaskWindow> taskWindows;
    TileXRCcuBarrierMode barrierMode = TileXRCcuBarrierMode::SyncXn;
};

struct TileXRCcuProgram {
    std::vector<TileXRCcuInstr> sqeLoad;
    std::vector<TileXRCcuInstr> sync;
};

struct TileXRCcuProducerPlanReport {
    uint32_t syncResourceCount = 0;
    uint32_t taskCount = 0;
    uint32_t instructionCount = 0;
    std::string message;
};

int TileXRCcuValidateProducerPlan(const TileXRCcuProducerPlan& plan, TileXRCcuProducerPlanReport* report);

int TileXRCcuBuildTasks(
    const TileXRCcuProducerPlan& plan,
    std::vector<TileXRCcuTask>* tasks,
    TileXRCcuProducerPlanReport* report);

int TileXRCcuBuildMicrocode(
    const TileXRCcuProducerPlan& plan,
    TileXRCcuProgram* program,
    TileXRCcuProducerPlanReport* report);

} // namespace TileXR

#endif // TILEXR_CCU_PRODUCER_PLAN_H
