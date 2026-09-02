/*
 * Copyright (c) 2026 TileXR Project
 * Licensed under the Apache License, Version 2.0
 */

#ifndef TILEXR_CCU_RUNTIME_H
#define TILEXR_CCU_RUNTIME_H

#include <cstdint>

#include "tilexr_types.h"

namespace TileXR {

constexpr uint32_t TILEXR_CCU_SQE_ARGS_LEN = 13U;
constexpr uint16_t TILEXR_CCU_DEFAULT_TASK_TIMEOUT_SEC = 120U;

struct TileXRCcuTask {
    uint8_t dieId = 0;
    uint8_t missionId = 0;
    uint16_t timeout = 0;
    uint16_t instStartId = 0;
    uint16_t instCnt = 0;
    uint32_t key = 0;
    uint32_t argSize = 0;
    uint64_t args[TILEXR_CCU_SQE_ARGS_LEN] = {};
};

struct TileXRCcuRuntimeSubmitReport {
    bool runtimeLaunchAttempted = false;
    int32_t runtimeRet = 0;
    bool finalTaskCaptured = false;
    TileXRCcuTask finalTask;
};

int TileXRCcuValidateTask(const TileXRCcuTask& task);

int TileXRCcuSubmitTaskWithReport(
    const TileXRCcuTask& task,
    void* stream,
    TileXRCcuRuntimeSubmitReport* report);

int TileXRCcuSubmitTask(const TileXRCcuTask& task, void* stream);

} // namespace TileXR

#endif // TILEXR_CCU_RUNTIME_H
