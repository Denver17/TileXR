/* Copyright (c) 2026 TileXR Project
 * Licensed under the CANN Open Software License Agreement Version 2.0. */
#ifndef TILEXR_PROFILING_DIRECT_H
#define TILEXR_PROFILING_DIRECT_H
#include "detail/device.h"

namespace TileXR { namespace Profiling {

class DirectWriter {
public:
    TILEXR_PROFILE_DEVICE void Initialize(Detail::DeviceArgsAddress descriptor,
        uint32_t logicalStream)
    {
#if TILEXR_PROFILE_ACTIVE
        // Reset before validation: failed reuse can never write the old lane.
        enabled_ = false;
        published_ = false;
        cursor_ = 0U;
        dropped_ = 0U;
        enabled_ = lane_.Initialize(descriptor, logicalStream);
#else
        (void)descriptor;
        (void)logicalStream;
#endif
    }

    TILEXR_PROFILE_DEVICE void RecordAt(uint32_t eventId, uint64_t timestamp)
    {
#if TILEXR_PROFILE_ACTIVE
        if (!enabled_ || published_) {
            return;
        }
        if (eventId == 0U) {
            lane_.Invalidate();
            enabled_ = false;
            return;
        }
        if (cursor_ == lane_.capacity) {
            dropped_ = Detail::SaturatingAdd(dropped_, 1U);
            return;
        }
        lane_.timestamps[cursor_] = timestamp;
        lane_.eventIds[cursor_] = eventId;
        ++cursor_;
#else
        (void)eventId;
        (void)timestamp;
#endif
    }

    TILEXR_PROFILE_DEVICE void Publish()
    {
#if TILEXR_PROFILE_ACTIVE
        if (!enabled_ || published_) {
            return;
        }
        lane_.Commit(cursor_, dropped_);
        published_ = true;
#endif
    }
private:
#if TILEXR_PROFILE_ACTIVE
    Detail::LaneWriter lane_;
    uint32_t cursor_ = 0U;
    uint32_t dropped_ = 0U;
    bool enabled_ = false;
    bool published_ = false;
#endif
};

} } // namespace TileXR::Profiling
#endif
