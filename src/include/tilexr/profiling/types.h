/* Copyright (c) 2026 TileXR Project
 * Licensed under the CANN Open Software License Agreement Version 2.0. */
#ifndef TILEXR_PROFILING_TYPES_H
#define TILEXR_PROFILING_TYPES_H

#include <cstddef>
#include <cstdint>

// A numeric switch is deliberate: defining the macro to zero disables markers.
#if defined(TILEXR_ENABLE_PROFILING)
static_assert(TILEXR_ENABLE_PROFILING == 0 || TILEXR_ENABLE_PROFILING == 1,
    "TILEXR_ENABLE_PROFILING must be 0 or 1");
#endif

namespace TileXR { namespace Profiling {

constexpr uint32_t DEVICE_VIEW_ABI_VERSION = 1U;
constexpr uint32_t ALIGNMENT_RECORDS = 8U;
constexpr uint32_t COUNT_STRIDE = 8U;
constexpr uint32_t MAX_LOGICAL_STREAMS = 65536U;
constexpr uint32_t DEFAULT_RECORDS_PER_STREAM = 4096U;
constexpr uint32_t MAX_RECORDS_PER_STREAM = 65536U;
constexpr uint32_t LOCAL_RECORD_CAPACITY = 256U;

enum class Completion : uint32_t {
    EMPTY = 0U, WRITING = 1U, COMMITTED = 2U, INVALID = 3U
};

// Copied to GM by the Host. Addresses are fixed-width integers, never Host
// pointers. Strides are element counts: records and uint32 status words.
struct alignas(8) DeviceView {
    uint32_t abiVersion;
    uint32_t descriptorBytes;
    uint32_t enabled;
    uint32_t logicalStreamCount;
    uint32_t recordsPerStream;
    uint32_t recordStride;
    uint32_t stateStride;
    uint32_t reserved;
    uint64_t timestampsAddress;
    uint64_t eventIdsAddress;
    uint64_t countsAddress;
    uint64_t droppedAddress;
    uint64_t statesAddress;
};

static_assert(sizeof(uint32_t) == 4 && sizeof(uint64_t) == 8,
    "Profiling requires fixed-width integers");
static_assert(sizeof(DeviceView) == 72 && alignof(DeviceView) == 8,
    "Unsupported profiling DeviceView ABI");
static_assert(offsetof(DeviceView, abiVersion) == 0 &&
    offsetof(DeviceView, descriptorBytes) == 4 &&
    offsetof(DeviceView, enabled) == 8 &&
    offsetof(DeviceView, logicalStreamCount) == 12 &&
    offsetof(DeviceView, recordsPerStream) == 16 &&
    offsetof(DeviceView, recordStride) == 20 &&
    offsetof(DeviceView, stateStride) == 24 &&
    offsetof(DeviceView, reserved) == 28 &&
    offsetof(DeviceView, timestampsAddress) == 32 &&
    offsetof(DeviceView, eventIdsAddress) == 40 &&
    offsetof(DeviceView, countsAddress) == 48 &&
    offsetof(DeviceView, droppedAddress) == 56 &&
    offsetof(DeviceView, statesAddress) == 64,
    "Unsupported profiling DeviceView field offsets");

} } // namespace TileXR::Profiling
#endif
