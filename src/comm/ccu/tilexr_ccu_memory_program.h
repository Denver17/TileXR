/*
 * Copyright (c) 2026 TileXR Project
 * Licensed under the Apache License, Version 2.0
 */

#ifndef TILEXR_CCU_MEMORY_PROGRAM_H
#define TILEXR_CCU_MEMORY_PROGRAM_H

#include "ccu/tilexr_ccu_microcode.h"

#include <cstdint>
#include <string>
#include <vector>

namespace TileXR {

enum class TileXRCcuMemoryCopyDirection {
    RemoteToLocal = 0,
    LocalToRemote = 1,
};

struct TileXRCcuMemoryCopySpec {
    TileXRCcuMemoryCopyDirection direction = TileXRCcuMemoryCopyDirection::RemoteToLocal;
    uint16_t localGsa = 0;
    uint16_t localXn = 0;
    uint16_t remoteGsa = 0;
    uint16_t remoteXn = 0;
    uint16_t lengthXn = 0;
    uint64_t localAddr = 0;
    uint64_t localToken = 0;
    uint64_t remoteAddr = 0;
    uint64_t remoteToken = 0;
    uint64_t lengthBytes = 0;
    uint16_t channelId = 0;
    uint16_t completionCke = 0;
    uint16_t completionMask = 0;
    uint16_t reduceDataType = 0;
    uint16_t reduceOpCode = 0;
    bool reduceEnabled = false;
};

struct TileXRCcuMemoryProgramReport {
    uint32_t loadInstructionCount = 0;
    uint32_t transferInstructionCount = 0;
    uint32_t waitInstructionCount = 0;
    uint32_t totalInstructionCount = 0;
    std::string message;
};

uint64_t TileXRCcuPackMemoryToken(uint32_t tokenId, uint32_t tokenValue, bool valid);

int TileXRCcuBuildMemoryCopyProgram(
    const TileXRCcuMemoryCopySpec& spec,
    std::vector<TileXRCcuInstr>* program,
    TileXRCcuMemoryProgramReport* report);

} // namespace TileXR

#endif // TILEXR_CCU_MEMORY_PROGRAM_H
