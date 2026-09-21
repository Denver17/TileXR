// Production M1/M2/M3 plus production DirectProfilingWriter; the CPU scheduler
// supplies peer arrivals and does not model AICore visibility or timings.
#include "combine_m12_test_fixture.h"
#include "tilexr_profiling.h"
#include "ep_memory_server_dedup_combine_profile.h"
#include "../../../src/ep/kernels/tilexr_ep_combine_memory_server_dedup_kernel.cpp"

int main()
{
    using ServerDedupTest::Check;
    using namespace TileXREp;
    try {
        CombineM12Test::Fixture f(193U, 17U);
        std::vector<uint64_t> times(64U * 32U, 0U), dispatchTimes(64U * 32U, 0xa5U);
        std::vector<uint32_t> events(64U * 32U, 0U), counts(64U * 16U, 0U), dropped(64U * 16U, 0U);
        f.args.extraFlag = TileXR::ExtraFlag::PROFILING;
        f.args.tilexrProfilingTimestampsGM = reinterpret_cast<GM_ADDR>(dispatchTimes.data());
        f.args.tilexrCombineProfilingTimestampsGM = reinterpret_cast<GM_ADDR>(times.data());
        f.args.tilexrCombineProfilingEventIdsGM = reinterpret_cast<GM_ADDR>(events.data());
        f.args.tilexrCombineProfilingRecordCountsGM = reinterpret_cast<GM_ADDR>(counts.data());
        f.args.tilexrCombineProfilingDroppedCountsGM = reinterpret_cast<GM_ADDR>(dropped.data());
        f.args.tilexrCombineProfilingRecordsPerCore = 32U;
        f.args.tilexrCombineProfilingCoreCapacity = 64U;
        f.tiling.profilingEnabled = 1U;
        const auto originalArgs = f.args;
        auto invoke = [&]() {
            tilexr_ep_combine_memory_server_dedup_kernel(reinterpret_cast<GM_ADDR>(&f.args),
                reinterpret_cast<GM_ADDR>(f.expert.data()), reinterpret_cast<GM_ADDR>(f.table.data()),
                reinterpret_cast<GM_ADDR>(f.counts.data()), f.untouched.data(), reinterpret_cast<GM_ADDR>(f.types.data()),
                f.untouched.data(), f.untouched.data(), f.untouched.data(), reinterpret_cast<GM_ADDR>(&f.tiling), 0);
        };
        for (uint32_t round = 0; round < 3U; ++round) for (uint32_t core : {0U, kEpServerDedupCombineSendCoreNum, 63U}) {
            f.Hooks(core, 0U); dropped[core * 16U] = 9U;
            invoke();
            std::vector<uint32_t> expected {kEpServerDedupCombineProfileStart,
                kEpServerDedupCombineProfileInitBegin, kEpServerDedupCombineProfileInitEnd,
                kEpServerDedupCombineProfileStageBegin, kEpServerDedupCombineProfileStageEnd};
            if (core >= kEpServerDedupCombineSendCoreNum) expected.insert(expected.end(), {
                kEpServerDedupCombineProfileControlBegin, kEpServerDedupCombineProfileControlEnd});
            else expected.insert(expected.end(), {kEpServerDedupCombineProfileDirectBegin,
                kEpServerDedupCombineProfileDirectEnd});
            expected.insert(expected.end(), {kEpServerDedupCombineProfileDrainBegin, kEpServerDedupCombineProfileDrainEnd,
                kEpServerDedupCombineProfileEnd});
            Check(counts[core * 16U] == (expected.size() + 7U) / 8U * 8U && dropped[core * 16U] == 0U, "latest capture publishes actual padded count and resets dropped");
            for (size_t i = 0; i < expected.size(); ++i) Check(events[core * 32U + i] == expected[i], "module event order follows core role");
            for (size_t i = expected.size(); i < counts[core * 16U]; ++i) Check(events[core * 32U + i] == 0U, "unused latest slots padded with zero");
            f.ResetHooks();
        }
        Check(std::all_of(dispatchTimes.begin(), dispatchTimes.end(), [](uint64_t x) { return x == 0xa5U; }), "Combine preserves Dispatch capture");
        const auto savedTimes = times; const auto savedEvents = events;
        f.tiling.profilingEnabled = 0U;
        f.Hooks(0U, 0U); invoke(); f.ResetHooks();
        Check(times == savedTimes && events == savedEvents, "OFF tiling performs M1/M2/M3 without profile writes");
        Check(std::memcmp(&originalArgs, &f.args, sizeof(f.args)) == 0, "kernel never mutates shared CommArgs");
    } catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
    if (ServerDedupTest::Failures() != 0) return 1;
    std::cout << "EP_SERVER_DEDUP_COMBINE_PROFILE_KERNEL_PASS\n";
    return 0;
}
