// Production M1/M2/M3/M4/M5 plus production DirectProfilingWriter; the CPU scheduler
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
        CombineM12Test::Fixture f(5000U, 17U);
        constexpr uint32_t capacity = kEpServerDedupCombineProfilingRecordsPerLaunch;
        // One real DIRECT task exercises the completion boundary, other rows
        // retain the M1 fixture. No M4 rows in this first full-kernel capture.
        f.types[0] = 1; f.table[0] = 8; f.table[1] = 0; f.table[2] = 0;
        std::vector<uint64_t> times(64U * capacity, 0U), dispatchTimes(64U * capacity, 0xa5U);
        std::vector<uint32_t> events(64U * capacity, 0U), counts(64U * 16U, 0U), dropped(64U * 16U, 0U);
        f.args.extraFlag = TileXR::ExtraFlag::PROFILING;
        f.args.tilexrProfilingTimestampsGM = reinterpret_cast<GM_ADDR>(dispatchTimes.data());
        f.args.tilexrCombineProfilingTimestampsGM = reinterpret_cast<GM_ADDR>(times.data());
        f.args.tilexrCombineProfilingEventIdsGM = reinterpret_cast<GM_ADDR>(events.data());
        f.args.tilexrCombineProfilingRecordCountsGM = reinterpret_cast<GM_ADDR>(counts.data());
        f.args.tilexrCombineProfilingDroppedCountsGM = reinterpret_cast<GM_ADDR>(dropped.data());
        f.args.tilexrCombineProfilingRecordsPerCore = capacity;
        f.args.tilexrCombineProfilingCoreCapacity = 64U;
        f.tiling.profilingEnabled = 1U;
        const auto originalArgs = f.args;
        std::vector<uint32_t> sourceMask(f.tiling.bs, 1U);
        std::vector<float> weights(f.tiling.bs * f.tiling.topK, 1.0f);
        std::vector<uint16_t> output(f.tiling.bs * f.h, 0U);
        auto invoke = [&]() {
            tilexr_ep_combine_memory_server_dedup_kernel(reinterpret_cast<GM_ADDR>(&f.args),
                reinterpret_cast<GM_ADDR>(f.expert.data()), reinterpret_cast<GM_ADDR>(f.table.data()),
                reinterpret_cast<GM_ADDR>(f.counts.data()), reinterpret_cast<GM_ADDR>(weights.data()), reinterpret_cast<GM_ADDR>(f.types.data()),
                f.untouched.data(), reinterpret_cast<GM_ADDR>(sourceMask.data()), reinterpret_cast<GM_ADDR>(output.data()), reinterpret_cast<GM_ADDR>(&f.tiling), 0);
        };
        std::vector<uint32_t> synchronizationByCore[2][64];
        auto seedResults = [&]() {
            // Remote return arrivals for all source slots, renewed each round.
            const uint32_t blocks = (f.h * 2U + 479U) / 480U;
            for (uint32_t p = 0; p < f.tiling.bs; ++p) for (uint32_t b = 0; b < blocks; ++b) {
                auto slot = f.peers[f.rank].data() + TILEXR_EP_SERVER_DEDUP_SHARED_DATA_OFFSET +
                    (uint64_t(p) * f.tiling.topK * blocks + b) * 512U;
                std::memset(slot, 0, 480U); const float one = 1.0f;
                std::memcpy(slot + 480U, &one, 4U);
            }
        };
        for (uint32_t round = 0; round < 3U; ++round) for (uint32_t core : {0U, kEpServerDedupCombineSendCoreNum, 63U}) {
            f.Hooks(core, 0U); dropped[core * 16U] = 9U;
            seedResults();
            if (round == 2U) AscendC::EventObserver() = [&](AscendC::HardEvent event, AscendC::TEventID id, bool set) {
                synchronizationByCore[1][core].push_back(uint32_t(event) * 32U + id * 2U + uint32_t(set));
            };
            invoke();
            AscendC::EventObserver() = {};
            std::vector<uint32_t> expected {kEpServerDedupCombineProfileStart,
                kEpServerDedupCombineProfileInitBegin, kEpServerDedupCombineProfileInitEnd,
                kEpServerDedupCombineProfileStageBegin, kEpServerDedupCombineProfileStageEnd};
            if (core >= kEpServerDedupCombineSendCoreNum) {
                expected.push_back(kEpServerDedupCombineProfileControlBegin);
                if (core >= 56U) expected.insert(expected.end(), {
                    kEpServerDedupCombineProfilePrepareNotifyBegin, kEpServerDedupCombineProfilePrepareNotifyEnd});
                expected.insert(expected.end(), {kEpServerDedupCombineProfileServerWaitBegin,
                    kEpServerDedupCombineProfileServerWaitEnd, kEpServerDedupCombineProfileControlEnd,
                    kEpServerDedupCombineProfilePartialBegin,
                    kEpServerDedupCombineProfilePartialEnd, kEpServerDedupCombineProfileFinalBegin});
                const uint32_t local = core - kEpServerDedupCombineSendCoreNum;
                const uint32_t tasks = f.tiling.bs / kEpServerDedupCombineRecvCoreNum +
                    (local < f.tiling.bs % kEpServerDedupCombineRecvCoreNum ? 1U : 0U);
                for (uint32_t task = 0; task < tasks; ++task) expected.insert(expected.end(), {
                    kEpServerDedupCombineProfileFinalTokenBegin, kEpServerDedupCombineProfileFinalTokenEnd});
                expected.push_back(kEpServerDedupCombineProfileFinalEnd);
            } else expected.insert(expected.end(), {kEpServerDedupCombineProfileDirectBegin,
                kEpServerDedupCombineProfileDirectMte3End, kEpServerDedupCombineProfileDirectEnd});
            expected.insert(expected.end(), {kEpServerDedupCombineProfileDrainBegin, kEpServerDedupCombineProfileDrainEnd,
                kEpServerDedupCombineProfileEnd});
            Check(counts[core * 16U] == (expected.size() + 7U) / 8U * 8U && dropped[core * 16U] == 0U, "latest capture publishes actual padded count and resets dropped");
            for (size_t i = 0; i < expected.size(); ++i) Check(events[core * capacity + i] == expected[i], "module event order follows core role");
            for (size_t i = expected.size(); i < counts[core * 16U]; ++i) Check(events[core * capacity + i] == 0U, "unused latest slots padded with zero");
            f.ResetHooks();
        }
        Check(std::all_of(dispatchTimes.begin(), dispatchTimes.end(), [](uint64_t x) { return x == 0xa5U; }), "Combine preserves Dispatch capture");
        const auto savedTimes = times; const auto savedEvents = events;
        f.tiling.profilingEnabled = 0U;
        for (uint32_t core : {0U, kEpServerDedupCombineSendCoreNum, 63U}) {
            f.Hooks(core, 0U);
            seedResults();
            AscendC::EventObserver() = [&](AscendC::HardEvent event, AscendC::TEventID id, bool set) {
                synchronizationByCore[0][core].push_back(uint32_t(event) * 32U + id * 2U + uint32_t(set));
            };
            invoke();
            AscendC::EventObserver() = {}; f.ResetHooks();
            Check(synchronizationByCore[0][core] == synchronizationByCore[1][core],
                "profiling adds no hard-event synchronization on Send, Recv or notifier cores");
        }
        Check(times == savedTimes && events == savedEvents, "OFF tiling performs M1/M2/M3/M4/M5 without profile writes");
        Check(std::memcmp(&originalArgs, &f.args, sizeof(f.args)) == 0, "kernel never mutates shared CommArgs");

        // More than the old 32-record limit on one M4 core. Reuse the real
        // kernel and defer MTE3, comparing ON/OFF hard-event sequences.
        std::vector<uint32_t> relay(f.tokens * f.tiling.topK * 2U, 0U);
        for (uint32_t row = 0; row < f.tokens; ++row) {
            f.types[row] = 2;
            f.table[row * 3U] = 8; f.table[row * 3U + 1U] = row % f.tiling.bs;
            f.table[row * 3U + 2U] = 0;
            relay[row * 6U] = 1; relay[row * 6U + 2U] = 160; // rank 8, slot 0
        }
        const uint32_t reader = kEpServerDedupCombineSendCoreNum;
        std::vector<uint32_t> synchronization[2];
        for (uint32_t enabled = 0; enabled < 2U; ++enabled) {
            f.Hooks(reader, 0U);
            for (uint32_t p = 0; p < f.tiling.bs; ++p) {
                const float one = 1.0f;
                auto slot = f.peers[f.rank].data() + TILEXR_EP_SERVER_DEDUP_SHARED_DATA_OFFSET +
                    uint64_t(p) * f.tiling.topK * 512U;
                std::memcpy(slot + 480U, &one, 4U);
            }
            AscendC::EventObserver() = [&](AscendC::HardEvent event, AscendC::TEventID id, bool set) {
                synchronization[enabled].push_back(uint32_t(event) * 32U + id * 2U + uint32_t(set));
            };
            CombineMemoryServerDedupKernel kernel;
            kernel.Init(reinterpret_cast<GM_ADDR>(&f.args), reinterpret_cast<GM_ADDR>(f.expert.data()),
                reinterpret_cast<GM_ADDR>(f.table.data()), reinterpret_cast<GM_ADDR>(f.counts.data()),
                reinterpret_cast<GM_ADDR>(weights.data()), reinterpret_cast<GM_ADDR>(f.types.data()),
                reinterpret_cast<GM_ADDR>(relay.data()), reinterpret_cast<GM_ADDR>(sourceMask.data()),
                reinterpret_cast<GM_ADDR>(output.data()), f.tiling.bs, f.h, f.tiling.topK,
                f.tiling.moeExpertNum, f.tiling.dtype, 0U, 0, uint8_t(enabled));
            kernel.Process();
            AscendC::EventObserver() = {};
            f.ResetHooks();
        }
        Check(synchronization[0] == synchronization[1], "M4/M5 profiling adds no hard-event synchronization");
        const uint32_t tasks = (f.tokens + kEpServerDedupCombineRecvCoreNum - 1U) / kEpServerDedupCombineRecvCoreNum;
        const auto first = events.begin() + reader * capacity;
        const auto last = first + counts[reader * 16U];
        Check(counts[reader * 16U] > 32U && dropped[reader * 16U] == 0U, "token capture exceeds old budget without loss");
        Check(std::count(first, last, kEpServerDedupCombineProfilePartialTokenBegin) == tasks &&
              std::count(first, last, kEpServerDedupCombineProfilePartialTokenEnd) == tasks,
              "one combined issue interval for every M4 task");
    } catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
    if (ServerDedupTest::Failures() != 0) return 1;
    std::cout << "EP_SERVER_DEDUP_COMBINE_PROFILE_KERNEL_PASS\n";
    return 0;
}
