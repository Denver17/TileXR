/*
 * Copyright (c) 2026 TileXR Project
 * Licensed under the Apache License, Version 2.0
 */

#include "ccu/tilexr_ccu_runtime.h"

#include <cstring>

#include <runtime/kernel.h>

namespace TileXR {

static_assert(RT_CCU_SQE_ARGS_LEN == TILEXR_CCU_SQE_ARGS_LEN, "TileXR CCU SQE arg count must match CANN runtime");

namespace {

TileXRCcuTask CopyRuntimeTask(const rtCcuTaskInfo_t& runtimeTask)
{
    TileXRCcuTask task {};
    task.dieId = runtimeTask.dieId;
    task.missionId = runtimeTask.missionId;
    task.timeout = runtimeTask.timeout;
    task.instStartId = runtimeTask.instStartId;
    task.instCnt = runtimeTask.instCnt;
    task.key = runtimeTask.key;
    task.argSize = runtimeTask.argSize;
    std::memcpy(task.args, runtimeTask.args, sizeof(task.args));
    return task;
}

} // namespace

int TileXRCcuValidateTask(const TileXRCcuTask& task)
{
    if (task.instCnt == RT_CCU_INST_CNT_INVALID) {
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    if (task.instStartId >= RT_CCU_INST_START_MAX) {
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    if (static_cast<uint32_t>(task.instStartId) + static_cast<uint32_t>(task.instCnt) > RT_CCU_INST_START_MAX) {
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    if (task.argSize != 1 && task.argSize != TILEXR_CCU_SQE_ARGS_LEN) {
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    return TILEXR_SUCCESS;
}

int TileXRCcuSubmitTaskWithReport(
    const TileXRCcuTask& task,
    void* stream,
    TileXRCcuRuntimeSubmitReport* report)
{
    if (report != nullptr) {
        *report = TileXRCcuRuntimeSubmitReport{};
    }
    if (stream == nullptr) {
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }

    int ret = TileXRCcuValidateTask(task);
    if (ret != TILEXR_SUCCESS) {
        return ret;
    }

    rtCcuTaskInfo_t runtimeTask {};
    runtimeTask.dieId = task.dieId;
    runtimeTask.missionId = task.missionId;
    runtimeTask.timeout = task.timeout;
    runtimeTask.instStartId = task.instStartId;
    runtimeTask.instCnt = task.instCnt;
    runtimeTask.key = task.key;
    runtimeTask.argSize = task.argSize;
    std::memcpy(runtimeTask.args, task.args, sizeof(runtimeTask.args));

    const TileXRCcuTask finalTask = CopyRuntimeTask(runtimeTask);
    if (report != nullptr) {
        report->finalTaskCaptured = true;
        report->finalTask = finalTask;
    }

    rtError_t launchRet = rtCCULaunch(&runtimeTask, stream);
    if (report != nullptr) {
        report->runtimeLaunchAttempted = true;
        report->runtimeRet = static_cast<int32_t>(launchRet);
    }
    return launchRet == RT_ERROR_NONE ? TILEXR_SUCCESS : TILEXR_ERROR_MKIRT;
}

int TileXRCcuSubmitTask(const TileXRCcuTask& task, void* stream)
{
    return TileXRCcuSubmitTaskWithReport(task, stream, nullptr);
}

} // namespace TileXR
