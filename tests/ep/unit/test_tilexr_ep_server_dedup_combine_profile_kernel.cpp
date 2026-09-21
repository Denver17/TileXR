// Execute the production empty kernel with a deterministic CPU clock/writer.
// This validates routing and markers, not AICore cache visibility or scheduling.
#include <array>
#include <cstring>
#include <iostream>
#include "tilexr_profiling.h"
#include "ep_memory_server_dedup_combine_profile.h"
#include "server_dedup_test_checks.h"

#define __global__
#define __mix__(cube, vector)
#include "../../../src/ep/kernels/tilexr_ep_combine_memory_server_dedup_kernel.cpp"

namespace {
struct Region {
    std::array<uint64_t, 32> times {};
    std::array<uint32_t, 32> events {};
    std::array<uint32_t, 32> counts {};
    std::array<uint32_t, 32> dropped {};
    bool Same(const Region &other) const
    {
        return times == other.times && events == other.events && counts == other.counts && dropped == other.dropped;
    }
};
using Business = std::array<std::array<uint8_t, 128>, 8>;
void Invoke(GM_ADDR comm, Business &business, TileXREp::EpServerDedupCombineTilingData *tiling)
{
    tilexr_ep_combine_memory_server_dedup_kernel(comm, business[0].data(), business[1].data(),
        business[2].data(), business[3].data(), business[4].data(), business[5].data(),
        business[6].data(), business[7].data(), reinterpret_cast<GM_ADDR>(tiling), 2);
}
}
int main()
{
    using ServerDedupTest::Check;
    Region dispatch, combine;
    dispatch.times.fill(0xa5);
    dispatch.events.fill(0xa5);
    dispatch.counts.fill(0xa5);
    dispatch.dropped.fill(0xa5);
    const Region dispatchBefore = dispatch;
    TileXR::CommArgs args {};
    args.extraFlag = TileXR::ExtraFlag::PROFILING;
    args.tilexrProfilingTimestampsGM = reinterpret_cast<GM_ADDR>(dispatch.times.data());
    args.tilexrProfilingEventIdsGM = reinterpret_cast<GM_ADDR>(dispatch.events.data());
    args.tilexrProfilingRecordCountsGM = reinterpret_cast<GM_ADDR>(dispatch.counts.data());
    args.tilexrProfilingDroppedCountsGM = reinterpret_cast<GM_ADDR>(dispatch.dropped.data());
    args.tilexrProfilingRecordsPerCore = 16;
    args.tilexrProfilingCoreCapacity = 2;
    args.tilexrCombineProfilingTimestampsGM = reinterpret_cast<GM_ADDR>(combine.times.data());
    args.tilexrCombineProfilingEventIdsGM = reinterpret_cast<GM_ADDR>(combine.events.data());
    args.tilexrCombineProfilingRecordCountsGM = reinterpret_cast<GM_ADDR>(combine.counts.data());
    args.tilexrCombineProfilingDroppedCountsGM = reinterpret_cast<GM_ADDR>(combine.dropped.data());
    args.tilexrCombineProfilingRecordsPerCore = 16;
    args.tilexrCombineProfilingCoreCapacity = 2;
    const auto argsBefore = args;
    Business business {};
    for (size_t i = 0; i < business.size(); ++i) business[i].fill(static_cast<uint8_t>(0xa0 + i));
    const auto businessBefore = business;
    TileXREp::EpServerDedupCombineTilingData tiling {2, 16, 32, 3, 0, 0, 1, {0, 0}};
    const auto tilingBefore = tiling;
    uint64_t previousEnd = 0;
    for (unsigned round = 0; round < 3; ++round) {
        combine.dropped[0] = 9; // Previous launch's loss must not leak forward.
        Invoke(reinterpret_cast<GM_ADDR>(&args), business, &tiling);
        Check(combine.events[0] == TileXREp::kEpServerDedupCombineProfileStart &&
            combine.events[1] == TileXREp::kEpServerDedupCombineProfileEnd,
            "production kernel records Combine start/end in its own region");
        Check(combine.times[0] > previousEnd && combine.times[1] - combine.times[0] >= 50000,
            "latest start/end enclose the required 50,000 tick wait");
        previousEnd = combine.times[1];
        Check(combine.counts[0] == 8 && combine.dropped[0] == 0,
            "latest Combine capture replaces prior metadata");
        Check(combine.counts[16] == 0 && combine.events[16] == 0,
            "core-zero Host fixture preserves neighboring core records");
        Check(dispatch.Same(dispatchBefore), "Combine never modifies Dispatch records or counters");
    }
    const Region combineBefore = combine;
    tiling.profilingEnabled = 0;
    const uint64_t before = AscendC::CycleReads();
    Invoke(reinterpret_cast<GM_ADDR>(uintptr_t(1)), business, &tiling);
    Check(AscendC::CycleReads() - before == 50001,
        "disabled tiling emits no profile timestamps while retaining the empty wait");
    Check(combine.Same(combineBefore) && dispatch.Same(dispatchBefore),
        "disabled tiling must not dereference poison CommArgs or emit either capture");
    tiling.profilingEnabled = 1;
    Invoke(nullptr, business, &tiling);
    Check(combine.Same(combineBefore), "null CommArgs cannot enable a writer");
    const uint64_t nullTilingBefore = AscendC::CycleReads();
    Invoke(reinterpret_cast<GM_ADDR>(uintptr_t(1)), business, nullptr);
    Check(AscendC::CycleReads() == nullTilingBefore, "null tiling returns without reading CommArgs or clock");
    Check(business == businessBefore, "profiling writes leave all business tensor bytes unchanged");
    Check(std::memcmp(&args, &argsBefore, sizeof(args)) == 0, "kernel must never repoint shared CommArgs");
    Check(std::memcmp(&tiling, &tilingBefore, sizeof(tiling)) == 0, "kernel never modifies tiling");
    if (ServerDedupTest::Failures() != 0) return 1;
    std::cout << "EP_SERVER_DEDUP_COMBINE_PROFILE_KERNEL_PASS\n";
    return 0;
}
