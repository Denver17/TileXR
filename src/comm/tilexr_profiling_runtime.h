/*
 * Copyright (c) 2026 TileXR Project
 * Licensed under the CANN Open Software License Agreement Version 2.0.
 */

#ifndef TILEXR_PROFILING_RUNTIME_H
#define TILEXR_PROFILING_RUNTIME_H

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace TileXR {

struct TileXRProfilingRuntimeConfig {
    bool enabled = false;
    std::string recordsPath;
    std::array<uint8_t, 16U> captureId {};
    uint32_t recordsPerCore = 0U;
    uint32_t coreCapacity = 0U;
};

bool LoadTileXRProfilingRuntimeConfig(int rank, int device, int commDomain,
    TileXRProfilingRuntimeConfig *config, std::string *error);

bool WriteTileXRProfilingRecords(const TileXRProfilingRuntimeConfig &config,
    int rank, const std::vector<uint32_t> &occupiedCounts,
    const std::vector<uint32_t> &droppedCounts,
    const std::vector<uint64_t> &timestamps,
    const std::vector<uint32_t> &eventIds, std::string *error);

} // namespace TileXR

#endif // TILEXR_PROFILING_RUNTIME_H
