#include <array>
#include <cstdint>
#include <iostream>
#include <limits>

#include "tilexr_profiling.h"

namespace {
int failures = 0;
void Check(bool ok, const char *message)
{
    if (!ok) {
        std::cerr << message << '\n';
        ++failures;
    }
}

struct Buffers {
    static constexpr uint32_t kCapacity = 24U;
    static constexpr uint32_t kGuard = 0xabcd1234U;
    // Extra entries on each side expose any overflow into a neighboring region.
    std::array<uint64_t, kCapacity * 2U + 2U> stamps {};
    std::array<uint32_t, kCapacity * 2U + 2U> events {};
    std::array<uint32_t, 18U> counts {};
    std::array<uint32_t, 18U> dropped {};
    TileXR::CommArgs args {};

    Buffers()
    {
        stamps.front() = stamps.back() = kGuard;
        events.front() = events.back() = kGuard;
        counts.front() = counts.back() = kGuard;
        dropped.front() = dropped.back() = kGuard;
        args.extraFlag = TileXR::ExtraFlag::PROFILING;
        args.tilexrProfilingTimestampsGM = reinterpret_cast<GM_ADDR>(stamps.data() + 1U);
        args.tilexrProfilingEventIdsGM = reinterpret_cast<GM_ADDR>(events.data() + 1U);
        args.tilexrProfilingRecordCountsGM = reinterpret_cast<GM_ADDR>(counts.data() + 1U);
        args.tilexrProfilingDroppedCountsGM = reinterpret_cast<GM_ADDR>(dropped.data() + 1U);
        args.tilexrProfilingRecordsPerCore = kCapacity;
        args.tilexrProfilingCoreCapacity = 2U;
    }
    GM_ADDR Address() { return reinterpret_cast<GM_ADDR>(&args); }
    void CheckGuards()
    {
        Check(stamps.front() == kGuard && stamps.back() == kGuard &&
            events.front() == kGuard && events.back() == kGuard &&
            counts.front() == kGuard && counts.back() == kGuard &&
            dropped.front() == kGuard && dropped.back() == kGuard,
            "profiling writes must stay inside GM arrays");
        for (uint32_t index = 2U; index < 9U; ++index) {
            Check(counts[index] == 0U && dropped[index] == 0U,
                "profiling counters must preserve per-core padding");
        }
    }
};
}

int main()
{
    Buffers buffers;
    TileXR::DirectProfilingWriter writer;
    writer.Record(1U);
    writer.Publish();
    Check(AscendC::CycleReads() == 0U, "uninitialized writer must not read cycles");
    writer.Initialize(buffers.Address(), 1U, 0U, 8U);
    writer.Record(1U);
    writer.RecordAt(2U, 200U);
    writer.Publish();
#if defined(TILEXR_ENABLE_PROFILING)
    Check(buffers.events[25U] == 1U && buffers.stamps[25U] == 1U &&
        buffers.events[26U] == 2U && buffers.stamps[26U] == 200U,
        "direct marker must store timestamp/event pair in requested core");
    Check(buffers.counts[9U] == 8U && buffers.events[27U] == 0U,
        "Publish must align valid records and pad with zero events");
    writer.Initialize(buffers.Address(), 1U, 1U, 8U);
    for (uint32_t index = 0U; index < 10U; ++index) {
        writer.RecordAt(3U, 300U + index);
    }
    writer.Publish();
    writer.Publish();
    Check(buffers.events[25U] == 1U && buffers.events[33U] == 3U &&
        buffers.events[41U] == 0U && buffers.dropped[9U] == 2U,
        "launch overflow must preserve earlier/later slots and Publish is idempotent");
    writer.Initialize(buffers.Address(), 1U, 2U, 8U);
    for (uint32_t index = 0U; index < 9U; ++index) {
        writer.RecordAt(4U, 400U + index);
    }
    writer.Publish();
    Check(buffers.dropped[9U] == 3U && buffers.counts[9U] == 24U,
        "later launches must accumulate rather than erase prior dropped counts");
    // A failed reinitialization must disable the previous live writer.
    const uint64_t before = AscendC::CycleReads();
    const auto reject = [&](GM_ADDR address, uint32_t core, uint32_t slot, uint32_t width) {
        writer.Initialize(address, core, slot, width);
        writer.Record(5U);
        writer.Publish();
        Check(AscendC::CycleReads() == before, "invalid writer must not sample or write");
    };
    reject(nullptr, 0U, 0U, 8U);
    reject(buffers.Address(), 2U, 0U, 8U);
    reject(buffers.Address(), 0U, 3U, 8U);
    reject(buffers.Address(), 0U, std::numeric_limits<uint32_t>::max(), 8U);
    reject(buffers.Address(), 0U, 0U, 0U);
    reject(buffers.Address(), 0U, 0U, 7U);
    buffers.args.extraFlag = 0U;
    reject(buffers.Address(), 0U, 0U, 8U);
    buffers.args.extraFlag = TileXR::ExtraFlag::PROFILING;
    buffers.args.tilexrProfilingRecordsPerCore = 23U;
    reject(buffers.Address(), 0U, 0U, 8U);
    buffers.args.tilexrProfilingRecordsPerCore = 24U;
    buffers.args.tilexrProfilingEventIdsGM = nullptr;
    reject(buffers.Address(), 0U, 0U, 8U);
    buffers.args.tilexrProfilingEventIdsGM = reinterpret_cast<GM_ADDR>(buffers.events.data() + 1U);
    buffers.dropped[1U] = std::numeric_limits<uint32_t>::max() - 1U;
    writer.Initialize(buffers.Address(), 0U, 0U, 8U);
    writer.RecordAt(0U, 1U);
    writer.RecordAt(1U, 0U);
    writer.Record(0U);
    Check(AscendC::CycleReads() == before, "reserved event zero must not sample");
    for (uint32_t index = 0U; index < 10U; ++index) {
        writer.RecordAt(6U, 600U + index);
    }
    writer.Publish();
    Check(buffers.dropped[1U] == std::numeric_limits<uint32_t>::max(),
        "dropped count must saturate without wrapping");

    writer.InitializeLatest(buffers.Address(), 1U, 0U, 24U);
    for (uint32_t index = 0U; index < 27U; ++index) {
        writer.RecordAt(7U, 700U + index);
    }
    writer.Publish();
    writer.Publish();
    Check(buffers.counts[9U] == 24U && buffers.dropped[9U] == 3U,
        "latest overflow must describe this launch only and Publish must be idempotent");
    writer.InitializeLatest(buffers.Address(), 1U, 0U, 24U);
    Check(buffers.counts[9U] == 0U && buffers.dropped[9U] == 0U,
        "latest launch must reset its own count and loss without scanning GM");
    writer.RecordAt(8U, 800U);
    writer.Publish();
    Check(buffers.counts[9U] == 8U && buffers.dropped[9U] == 0U &&
        buffers.events[25U] == 8U && buffers.events[26U] == 0U &&
        buffers.events[33U] == 7U,
        "shorter latest launch must replace valid range while leaving stale tail unexported");
    Check(buffers.counts[1U] == 8U &&
        buffers.dropped[1U] == std::numeric_limits<uint32_t>::max(),
        "latest launch must not reset a neighboring core");
    for (uint32_t slot : {1U, std::numeric_limits<uint32_t>::max()}) {
        writer.InitializeLatest(buffers.Address(), 1U, slot, 24U);
        writer.RecordAt(9U, 900U);
        writer.Publish();
        Check(buffers.counts[9U] == 8U && buffers.events[25U] == 8U,
            "invalid latest slot must not reset or overwrite the preceding capture");
    }
    writer.InitializeLatest(buffers.Address(), 1U, 0U, 24U);
    writer.Publish();
    Check(buffers.counts[9U] == 0U && buffers.dropped[9U] == 0U,
        "empty latest launch must hide all previous records");
#else
    writer.InitializeLatest(buffers.Address(), 1U, 0U, 24U);
    writer.RecordAt(8U, 800U);
    writer.Publish();
    Check(AscendC::CycleReads() == 0U && buffers.counts[9U] == 0U &&
        buffers.events[25U] == 0U && buffers.stamps[25U] == 0U,
        "OFF build must not sample cycles or touch GM");
    static_assert(sizeof(TileXR::DirectProfilingWriter) == 1U,
        "OFF writer must have no per-core state");
#endif
    buffers.CheckGuards();
    return failures == 0 ? 0 : 1;
}
