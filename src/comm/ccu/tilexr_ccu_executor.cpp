/*
 * Copyright (c) 2026 TileXR Project
 */
#include "ccu/tilexr_ccu_executor.h"

#include "ccu/tilexr_ccu_runtime_session.h"

namespace TileXR {

int TileXRCcuExecutor::Submit(
    const TileXRCcuRuntimeSession &session,
    const TileXRCcuCollectivePlan &plan,
    aclrtStream) const
{
    if (!session.Available()) {
        return TILEXR_ERROR_NOT_INITIALIZED;
    }
    return plan.ready ? TILEXR_ERROR_NOT_SUPPORT : TILEXR_ERROR_PARA_CHECK_FAIL;
}

#ifdef TILEXR_CCU_TESTING
int TileXRCcuExecutor::ReadDirectCcuInstructionsForDebug(
    TileXRCcuRuntimeSession &session,
    uint8_t dieId,
    uint16_t instructionStartId,
    void *instructions,
    uint32_t instructionCount,
    uint32_t instructionBytes,
    TileXRCcuDriverAdapterReport *report) const
{
    if (report != nullptr) {
        *report = TileXRCcuDriverAdapterReport{};
    }
    if (!session.Available()) {
        if (report != nullptr) {
            report->message = "TileXRCcuBackend is not initialized for direct CCU instruction readback";
        }
        return TILEXR_ERROR_NOT_INITIALIZED;
    }

    TileXRCcuDriverAdapter adapter;
    int ret = session.CreateDriverAdapter(&adapter, report);
    if (ret != TILEXR_SUCCESS) {
        if (report != nullptr && report->message.empty()) {
            report->message = "direct CCU runtime is unavailable for instruction readback";
        }
        return ret;
    }
    return adapter.ReadInstructions(dieId, instructionStartId, instructions, instructionCount, instructionBytes, report);
}
#endif

} // namespace TileXR
