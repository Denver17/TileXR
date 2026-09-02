/*
 * Copyright (c) 2026 TileXR Project
 * Licensed under the Apache License, Version 2.0
 */

#ifndef TILEXR_CCU_SPECS_H
#define TILEXR_CCU_SPECS_H

#include "ccu/tilexr_ccu_resource_allocator.h"

#include <cstdint>
#include <string>

namespace TileXR {

constexpr uint64_t TILEXR_CCU_V1_CCUM_OFFSET = 0x800000ULL;
constexpr uint64_t TILEXR_CCU_RESOURCE_INS_RESERVE_SIZE = 0x100000ULL;
constexpr uint64_t TILEXR_CCU_V1_RESOURCE_GSA_RESERVE_SIZE = 0x8000ULL;
constexpr uint64_t TILEXR_CCU_V1_XN_RESOURCE_OFFSET =
    TILEXR_CCU_V1_CCUM_OFFSET + TILEXR_CCU_RESOURCE_INS_RESERVE_SIZE + TILEXR_CCU_V1_RESOURCE_GSA_RESERVE_SIZE;
constexpr uint64_t TILEXR_CCU_RESOURCE_WINDOW_BYTES = 72ULL * 1024ULL * 1024ULL;

struct TileXRCcuCaps {
    uint32_t cap0 = 0;
    uint32_t cap1 = 0;
    uint32_t cap2 = 0;
    uint32_t cap3 = 0;
    uint32_t cap4 = 0;
};

struct TileXRCcuMsidTokenInfo {
    uint32_t tokenId = 0;
    uint32_t tokenValue = 0;
    bool valid = false;
};

struct TileXRCcuBasicInfo {
    uint8_t dieId = 0;
    uint32_t msId = 0;
    TileXRCcuMsidTokenInfo msidToken;
    uint32_t missionKey = 0;
    uint64_t resourceAddr = 0;
    TileXRCcuCaps caps;
};

struct TileXRCcuSpecInfo {
    uint8_t dieId = 0;
    uint32_t msId = 0;
    uint32_t missionKey = 0;
    uint64_t resourceAddr = 0;
    uint64_t xnBaseAddr = 0;
    uint32_t loopEngineNum = 0;
    uint32_t missionNum = 0;
    uint32_t instructionNum = 0;
    uint32_t xnNum = 0;
    uint32_t gsaNum = 0;
    uint32_t msNum = 0;
    uint32_t ckeNum = 0;
    uint32_t jettyNum = 0;
    uint32_t channelNum = 0;
    uint32_t pfeNum = 0;
};

struct TileXRCcuSpecsReport {
    uint32_t instructionNum = 0;
    uint32_t xnNum = 0;
    uint32_t ckeNum = 0;
    uint32_t channelNum = 0;
    uint32_t missionNum = 0;
    std::string message;
};

int TileXRCcuDecodeBasicInfo(
    const TileXRCcuBasicInfo& basicInfo,
    TileXRCcuSpecInfo* specInfo,
    TileXRCcuSpecsReport* report);

int TileXRCcuBuildResourceSpec(
    const TileXRCcuSpecInfo& specInfo,
    uint16_t missionStartId,
    uint16_t instructionStartId,
    uint16_t xnStartId,
    uint16_t ckeStartId,
    uint16_t channelStartId,
    TileXRCcuResourceSpec* resourceSpec,
    TileXRCcuSpecsReport* report,
    uint16_t gsaStartId = 0);

} // namespace TileXR

#endif // TILEXR_CCU_SPECS_H
