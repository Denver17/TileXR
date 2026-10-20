/* Copyright (c) 2026 TileXR Project
 * Licensed under the CANN Open Software License Agreement Version 2.0. */
#ifndef TILEXR_PROFILING_DEVICE_H
#define TILEXR_PROFILING_DEVICE_H
#include "detail/device.h"

namespace TileXR { namespace Profiling {

template <uint32_t RecordCapacity>
class Recorder {
    static_assert(RecordCapacity > 0U && RecordCapacity <= LOCAL_RECORD_CAPACITY,
        "Profiling recorder capacity must be in [1,256]");
public:
    TILEXR_PROFILE_DEVICE Recorder()
    {
#if TILEXR_PROFILE_ACTIVE
        for (uint32_t i = 0U; i < RecordCapacity; ++i) {
            eventIds_[i] = 0U;
        }
#endif
    }

    template <uint32_t Slot, uint32_t EventId>
    TILEXR_PROFILE_DEVICE void RecordMarker()
    {
        static_assert(Slot < RecordCapacity, "Profiling marker slot exceeds recorder capacity");
        static_assert(EventId != 0U, "Profiling event ID zero is reserved");
#if TILEXR_PROFILE_ACTIVE
        timestamps_[Slot] = static_cast<uint64_t>(AscendC::GetSystemCycle());
        eventIds_[Slot] = EventId;
#endif
    }

    TILEXR_PROFILE_DEVICE void Flush(Detail::DeviceArgsAddress descriptor, uint32_t logicalStream)
    {
#if TILEXR_PROFILE_ACTIVE
        Detail::LaneWriter lane;
        if (!lane.Initialize(descriptor, logicalStream)) {
            return;
        }
        uint32_t count = 0U;
        uint32_t dropped = 0U;
        for (uint32_t slot = 0U; slot < RecordCapacity; ++slot) {
            if (eventIds_[slot] == 0U) {
                continue;
            }
            if (count == lane.capacity) {
                dropped = Detail::SaturatingAdd(dropped, 1U);
                continue;
            }
            lane.timestamps[count] = timestamps_[slot];
            lane.eventIds[count] = eventIds_[slot];
            ++count;
        }
        lane.Commit(count, dropped);
#else
        (void)descriptor;
        (void)logicalStream;
#endif
    }
private:
#if TILEXR_PROFILE_ACTIVE
    uint64_t timestamps_[RecordCapacity];
    uint32_t eventIds_[RecordCapacity];
#endif
};

} } // namespace TileXR::Profiling
#endif
