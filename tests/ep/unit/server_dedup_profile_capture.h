#pragma once
#include <vector>
#include "tilexr_ep_server_dedup_profile.h"
#include "server_dedup_test_checks.h"

namespace ServerDedupTest {
// Exercise the actual standard writer against separate, guarded GM arrays.
struct ProfileCapture {
    static constexpr uint32_t recordsPerLaunch = TileXREp::kEpServerDedupProfilingRecordsPerLaunch;
    uint32_t recordsPerCore;
    std::vector<uint64_t> timestamps;
    std::vector<uint32_t> events, counts, dropped;
    explicit ProfileCapture(TileXR::CommArgs &args, uint32_t launches = 1U) :
        recordsPerCore(recordsPerLaunch * launches),
        timestamps(64ULL * recordsPerCore + 2U, 0U),
        events(64ULL * recordsPerCore + 2U, 0U), counts(64U * 8U, 0U), dropped(64U * 8U, 0U)
    {
        timestamps.front() = timestamps.back() = UINT64_MAX;
        events.front() = events.back() = UINT32_MAX;
        args.extraFlag |= TileXR::ExtraFlag::PROFILING;
        args.tilexrProfilingTimestampsGM = reinterpret_cast<GM_ADDR>(timestamps.data() + 1U);
        args.tilexrProfilingEventIdsGM = reinterpret_cast<GM_ADDR>(events.data() + 1U);
        args.tilexrProfilingRecordCountsGM = reinterpret_cast<GM_ADDR>(counts.data());
        args.tilexrProfilingDroppedCountsGM = reinterpret_cast<GM_ADDR>(dropped.data());
        args.tilexrProfilingCoreCapacity = 64U;
        args.tilexrProfilingRecordsPerCore = recordsPerCore;
    }
    std::vector<uint32_t> Events(uint32_t core, uint32_t launch = 0U) const
    {
        std::vector<uint32_t> result;
        uint64_t previous = 0U;
        const uint64_t base = uint64_t(core) * recordsPerCore + uint64_t(launch) * recordsPerLaunch + 1U;
        for (uint32_t i = 0U; i < recordsPerLaunch; ++i) {
            if (events[base + i] == 0U) continue;
            Check(timestamps[base + i] > previous, "real timestamp samples retain execution order");
            previous = timestamps[base + i];
            result.push_back(events[base + i]);
        }
        Check(timestamps.front() == UINT64_MAX && timestamps.back() == UINT64_MAX &&
            events.front() == UINT32_MAX && events.back() == UINT32_MAX, "standard GM capture guards intact");
        return result;
    }
};
} // namespace ServerDedupTest
