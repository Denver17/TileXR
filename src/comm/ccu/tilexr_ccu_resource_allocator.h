/*
 * Copyright (c) 2026 TileXR Project
 * Licensed under the Apache License, Version 2.0
 */

#ifndef TILEXR_CCU_RESOURCE_ALLOCATOR_H
#define TILEXR_CCU_RESOURCE_ALLOCATOR_H

#include "ccu/tilexr_ccu_producer_plan.h"

#include <cstdint>
#include <map>
#include <string>

namespace TileXR {

struct TileXRCcuResourceSpec {
    uint8_t dieId = 0;
    uint32_t missionKey = 0;
    uint16_t missionStartId = 0;
    uint16_t missionCount = 0;
    uint16_t instructionStartId = 0;
    uint16_t missionInstructionStartId = 0;
    uint16_t instructionCount = 0;
    uint16_t xnStartId = 0;
    uint16_t xnCount = 0;
    uint16_t gsaStartId = 0;
    uint16_t gsaCount = 0;
    uint16_t remoteXnStartId = 0;
    uint16_t remoteXnCount = 0;
    uint16_t ckeStartId = 0;
    uint16_t ckeCount = 0;
    uint16_t channelStartId = 0;
    uint16_t channelCount = 0;
    uint16_t localWaitCkeStartId = 0;
    uint16_t localWaitCkeCount = 0;
    uint16_t remoteNotifyCkeStartId = 0;
    uint16_t remoteNotifyCkeCount = 0;
};

struct TileXRCcuResourceRequest {
    uint32_t sqeArgCount = TILEXR_CCU_SQE_ARGS_LEN;
    uint32_t syncResourceCount = 0;
    uint32_t syncInstructionCount = 0;
    uint32_t bindingsPerSyncResource = 1;
    TileXRCcuBarrierMode barrierMode = TileXRCcuBarrierMode::SyncXn;
};

struct TileXRCcuResourceAllocation {
    uint64_t receiptId = 0;
    std::string packageProvider;
    TileXRCcuRange mission;
    TileXRCcuRange repository;
    TileXRCcuRange localXn;
    TileXRCcuRange localGsa;
    TileXRCcuRange remoteXn;
    TileXRCcuRange notifyCke;
    TileXRCcuRange channels;
    TileXRCcuRange localWaitCke;
    TileXRCcuRange remoteNotifyCke;
    TileXRCcuRange sourceCke;
};

struct TileXRCcuResourceAllocatorReport {
    uint32_t missionAllocated = 0;
    uint32_t repositoryAllocated = 0;
    uint32_t localXnAllocated = 0;
    uint32_t localGsaAllocated = 0;
    uint32_t remoteXnAllocated = 0;
    uint32_t notifyCkeAllocated = 0;
    uint32_t channelBindingsAllocated = 0;
    uint32_t localWaitCkeAllocated = 0;
    uint32_t remoteNotifyCkeAllocated = 0;
    uint32_t sourceCkeAllocated = 0;
    std::string message;
};

class TileXRCcuResourceAllocator {
public:
    int Init(const TileXRCcuResourceSpec& spec);

    int Allocate(
        const TileXRCcuResourceRequest& request,
        TileXRCcuProducerPlan* plan,
        TileXRCcuResourceAllocation* allocation,
        TileXRCcuResourceAllocatorReport* report);

    int Release(uint64_t receiptId);

private:
    struct Cursor {
        uint16_t start = 0;
        uint16_t count = 0;
        uint16_t used = 0;
    };

    bool HasCapacity(const Cursor& cursor, uint32_t count) const;
    uint16_t CursorNext(const Cursor& cursor) const;

    struct ActiveAllocation {
        TileXRCcuResourceAllocation allocation;
        uint16_t missionUsed = 0;
        uint16_t repositoryUsed = 0;
        uint16_t localXnUsed = 0;
        uint16_t localGsaUsed = 0;
        uint16_t remoteXnUsed = 0;
        uint16_t notifyCkeUsed = 0;
        uint16_t channelUsed = 0;
        uint16_t localWaitCkeUsed = 0;
        uint16_t remoteNotifyCkeUsed = 0;
        uint16_t sourceCkeUsed = 0;
    };

    TileXRCcuResourceSpec spec_;
    Cursor mission_;
    Cursor repository_;
    Cursor xn_;
    Cursor gsa_;
    Cursor remoteXn_;
    Cursor localWaitCke_;
    Cursor remoteNotifyCke_;
    Cursor channel_;
    uint64_t nextReceiptId_ = 1;
    bool initialized_ = false;
    std::map<uint64_t, ActiveAllocation> active_;
};

} // namespace TileXR

#endif // TILEXR_CCU_RESOURCE_ALLOCATOR_H
