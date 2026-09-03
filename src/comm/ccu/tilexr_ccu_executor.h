/*
 * Copyright (c) 2026 TileXR Project
 */
#ifndef TILEXR_CCU_EXECUTOR_H
#define TILEXR_CCU_EXECUTOR_H

#include "acl/acl_base.h"
#include "ccu/tilexr_ccu_backend.h"
#ifdef TILEXR_CCU_TESTING
#include "ccu/tilexr_ccu_driver_adapter.h"
#endif

namespace TileXR {

class TileXRCcuRuntimeSession;

class TileXRCcuExecutor {
public:
    int Submit(const TileXRCcuRuntimeSession &session, const TileXRCcuCollectivePlan &plan, aclrtStream stream) const;
#ifdef TILEXR_CCU_TESTING
    int ReadDirectCcuInstructionsForDebug(
        TileXRCcuRuntimeSession &session,
        uint8_t dieId,
        uint16_t instructionStartId,
        void *instructions,
        uint32_t instructionCount,
        uint32_t instructionBytes,
        TileXRCcuDriverAdapterReport *report) const;
#endif
};

} // namespace TileXR

#endif // TILEXR_CCU_EXECUTOR_H
