/*
 * Copyright (c) 2026 TileXR Project
 * Licensed under the CANN Open Software License Agreement Version 2.0.
 */

#ifndef TILEXR_PROFILING_RUNTIME_H
#define TILEXR_PROFILING_RUNTIME_H

#include <array>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace TileXR {

struct TileXRProfilingRuntimeConfig {
    bool enabled = false;
    std::string recordsPath;
    std::array<uint8_t, 16U> captureId {};
    uint32_t recordsPerCore = 0U;
    uint32_t coreCapacity = 0U;
    // A launch that had no GM reservation cannot report device dropped counts.
    bool incomplete = false;
    // Explicit opt-in: legacy capture may contain records beyond its counters.
    // Latest writers publish the only valid range and may leave old GM tails.
    bool latestLaunch = false;
};

// One allocator per communicator capture region. Reservation is Host-only and never waits for
// device execution. Kernels sharing the capture must execute in order.
class TileXRProfilingLaunchState {
public:
    bool Reserve(const TileXRProfilingRuntimeConfig &config,
        uint32_t recordsPerLaunch, uint32_t *launchSlot);
    bool ReserveLatest(const TileXRProfilingRuntimeConfig &config,
        uint32_t recordsPerLaunch, uint32_t *launchSlot);
    bool Incomplete() const;
    bool LatestLaunch() const;
    void Reset();

private:
    enum class Mode { Unset, Reserved, Latest };
    bool ReserveImpl(const TileXRProfilingRuntimeConfig &config,
        uint32_t recordsPerLaunch, uint32_t *launchSlot, Mode mode);
    mutable std::mutex mutex_;
    Mode mode_ = Mode::Unset;
    uint32_t nextRecord_ = 0U;
    uint32_t latestWidth_ = 0U;
    bool incomplete_ = false;
};

bool LoadTileXRProfilingRuntimeConfig(int rank, int device, int commDomain,
    TileXRProfilingRuntimeConfig *config, std::string *error);

bool LoadTileXRCombineProfilingRuntimeConfig(int rank, int device, int commDomain,
    TileXRProfilingRuntimeConfig *config, std::string *error);

bool WriteTileXRProfilingRecords(const TileXRProfilingRuntimeConfig &config,
    int rank, const std::vector<uint32_t> &occupiedCounts,
    const std::vector<uint32_t> &droppedCounts,
    const std::vector<uint64_t> &timestamps,
    const std::vector<uint32_t> &eventIds, std::string *error);

} // namespace TileXR

#endif // TILEXR_PROFILING_RUNTIME_H
