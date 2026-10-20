/* Copyright (c) 2026 TileXR Project
 * Licensed under the CANN Open Software License Agreement Version 2.0. */
#ifndef TILEXR_PROFILING_DETAIL_DEVICE_H
#define TILEXR_PROFILING_DETAIL_DEVICE_H

#include "../types.h"

#if defined(__CCE__) && defined(__CCE_IS_AICORE__)
#include "kernel_operator.h"
#define TILEXR_PROFILE_DEVICE __attribute__((always_inline)) inline __aicore__
#define TILEXR_PROFILE_GM __gm__
#define TILEXR_PROFILE_AICORE 1
#else
#define TILEXR_PROFILE_DEVICE inline
#define TILEXR_PROFILE_GM
#define TILEXR_PROFILE_AICORE 0
#endif

#if TILEXR_PROFILE_AICORE && defined(TILEXR_ENABLE_PROFILING) && TILEXR_ENABLE_PROFILING == 1
#define TILEXR_PROFILE_ACTIVE 1
#else
#define TILEXR_PROFILE_ACTIVE 0
#endif

namespace TileXR { namespace Profiling { namespace Detail {

#if TILEXR_PROFILE_AICORE
using DeviceArgsAddress = GM_ADDR;
#else
using DeviceArgsAddress = unsigned char *;
#endif

TILEXR_PROFILE_DEVICE uint32_t SaturatingAdd(uint32_t a, uint32_t b)
{
    return a > static_cast<uint32_t>(-1) - b ? static_cast<uint32_t>(-1) : a + b;
}

#if TILEXR_PROFILE_ACTIVE
TILEXR_PROFILE_DEVICE bool AddressRangeValid(uint64_t address, uint64_t elements,
    uint32_t elementBytes)
{
    return address != 0U && address % 32U == 0U &&
        elements <= (static_cast<uint64_t>(-1) - address) / elementBytes;
}

// One invocation owns one lane exclusively. These checks reject sequential
// reuse, not concurrent writers; shared-lane concurrency is outside the ABI.
class LaneWriter {
public:
    TILEXR_PROFILE_DEVICE bool Initialize(DeviceArgsAddress descriptor, uint32_t lane)
    {
        timestamps = nullptr;
        eventIds = nullptr;
        count = nullptr;
        dropped = nullptr;
        state = nullptr;
        capacity = 0U;
        if (descriptor == nullptr ||
            reinterpret_cast<uint64_t>(descriptor) % alignof(DeviceView) != 0U) {
            return false;
        }
        TILEXR_PROFILE_GM const DeviceView *view =
            reinterpret_cast<TILEXR_PROFILE_GM const DeviceView *>(descriptor);
        if (view->abiVersion != DEVICE_VIEW_ABI_VERSION ||
            view->descriptorBytes != sizeof(DeviceView) || view->enabled != 1U ||
            view->reserved != 0U || view->logicalStreamCount == 0U ||
            view->logicalStreamCount > MAX_LOGICAL_STREAMS ||
            lane >= view->logicalStreamCount || view->recordsPerStream < ALIGNMENT_RECORDS ||
            view->recordsPerStream > MAX_RECORDS_PER_STREAM ||
            view->recordsPerStream % ALIGNMENT_RECORDS != 0U ||
            view->recordStride < view->recordsPerStream ||
            view->recordStride % ALIGNMENT_RECORDS != 0U ||
            view->stateStride < COUNT_STRIDE || view->stateStride % COUNT_STRIDE != 0U) {
            return false;
        }
        const uint64_t records = static_cast<uint64_t>(view->logicalStreamCount) * view->recordStride;
        const uint64_t states = static_cast<uint64_t>(view->logicalStreamCount) * view->stateStride;
        if (!AddressRangeValid(view->timestampsAddress, records, sizeof(uint64_t)) ||
            !AddressRangeValid(view->eventIdsAddress, records, sizeof(uint32_t)) ||
            !AddressRangeValid(view->countsAddress, states, sizeof(uint32_t)) ||
            !AddressRangeValid(view->droppedAddress, states, sizeof(uint32_t)) ||
            !AddressRangeValid(view->statesAddress, states, sizeof(uint32_t))) {
            return false;
        }
        const uint64_t statusOffset = static_cast<uint64_t>(lane) * view->stateStride;
        state = reinterpret_cast<TILEXR_PROFILE_GM uint32_t *>(view->statesAddress) + statusOffset;
        if (*state != static_cast<uint32_t>(Completion::EMPTY)) {
            *state = static_cast<uint32_t>(Completion::INVALID);
            return false;
        }
        count = reinterpret_cast<TILEXR_PROFILE_GM uint32_t *>(view->countsAddress) + statusOffset;
        dropped = reinterpret_cast<TILEXR_PROFILE_GM uint32_t *>(view->droppedAddress) + statusOffset;
        if (*count != 0U || *dropped != 0U) {
            *state = static_cast<uint32_t>(Completion::INVALID);
            return false;
        }
        const uint64_t recordOffset = static_cast<uint64_t>(lane) * view->recordStride;
        timestamps = reinterpret_cast<TILEXR_PROFILE_GM uint64_t *>(view->timestampsAddress) + recordOffset;
        eventIds = reinterpret_cast<TILEXR_PROFILE_GM uint32_t *>(view->eventIdsAddress) + recordOffset;
        capacity = view->recordsPerStream;
        *state = static_cast<uint32_t>(Completion::WRITING);
        return true;
    }

    TILEXR_PROFILE_DEVICE void Commit(uint32_t size, uint32_t lost)
    {
        // Host reads only after successful stream synchronization. A scalar
        // state store is not a release primitive or a live polling contract.
        *count = size;
        *dropped = lost;
        *state = static_cast<uint32_t>(Completion::COMMITTED);
    }

    TILEXR_PROFILE_DEVICE void Invalidate()
    {
        *state = static_cast<uint32_t>(Completion::INVALID);
    }

    TILEXR_PROFILE_GM uint64_t *timestamps = nullptr;
    TILEXR_PROFILE_GM uint32_t *eventIds = nullptr;
    TILEXR_PROFILE_GM uint32_t *count = nullptr;
    TILEXR_PROFILE_GM uint32_t *dropped = nullptr;
    TILEXR_PROFILE_GM uint32_t *state = nullptr;
    uint32_t capacity = 0U;
};
#endif

} } } // namespace TileXR::Profiling::Detail
#endif
