// CPU substitutes validate independent record addresses/state, not AICore timing.
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <string>
#include <vector>
#include <unistd.h>

#include "tilexr_profiling.h"
#include "tilexr_profiling_runtime.h"

namespace {
int failures = 0;
void Check(bool condition, const char *message)
{
    if (!condition) { std::cerr << message << '\n'; ++failures; }
}
void Env(const char *name, const char *value)
{
    if (value) setenv(name, value, 1);
    else unsetenv(name);
}
using Config = TileXR::TileXRProfilingRuntimeConfig;
struct Region {
    static constexpr uint32_t kCapacity = 32U;
    static constexpr uint32_t kCores = 2U;
    std::vector<uint64_t> times = std::vector<uint64_t>(kCapacity * kCores);
    std::vector<uint32_t> events = std::vector<uint32_t>(kCapacity * kCores);
    std::vector<uint32_t> counts = std::vector<uint32_t>(kCores * TileXR::TILEXR_PROFILING_COUNT_STRIDE);
    std::vector<uint32_t> dropped = std::vector<uint32_t>(counts.size());

    bool Same(const Region &other) const
    {
        return times == other.times && events == other.events && counts == other.counts && dropped == other.dropped;
    }
    bool Export(Config config, const std::string &path)
    {
        config.recordsPath = path;
        config.recordsPerCore = kCapacity;
        config.coreCapacity = kCores;
        config.latestLaunch = true;
        std::vector<uint32_t> occupied(kCores), lost(kCores);
        for (uint32_t core = 0U; core < kCores; ++core) {
            occupied[core] = counts[core * TileXR::TILEXR_PROFILING_COUNT_STRIDE];
            lost[core] = dropped[core * TileXR::TILEXR_PROFILING_COUNT_STRIDE];
        }
        std::string error;
        return TileXR::WriteTileXRProfilingRecords(config, 3, occupied, lost, times, events, &error);
    }
};
#if defined(TILEXR_ENABLE_PROFILING)
std::vector<uint8_t> Read(const std::string &path)
{
    std::ifstream input(path, std::ios::binary);
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(input), {});
}
uint32_t U32(const std::vector<uint8_t> &bytes, size_t offset)
{
    if (offset + 4U > bytes.size()) { Check(false, "truncated records file"); return 0; }
    uint32_t value = 0;
    for (uint32_t i = 0; i < 4; ++i) value |= uint32_t(bytes[offset + i]) << (i * 8U);
    return value;
}
#endif
void ConfigurationAndReservations(Config *dispatch, Config *combine)
{
    std::string error;
    Env("TILEXR_PROFILING_RECORDS_PATH", nullptr);
    Env("TILEXR_PROFILING_COMBINE_RECORDS_PATH", nullptr);
    Env("TILEXR_PROFILING_CAPTURE_ID", "malformed");
    Env("TILEXR_PROFILING_RECORDS_PER_CORE", "malformed");
    Check(TileXR::LoadTileXRProfilingRuntimeConfig(3, 1, 8, dispatch, &error) && !dispatch->enabled,
        "disabled Dispatch must ignore malformed shared settings");
    Check(TileXR::LoadTileXRCombineProfilingRuntimeConfig(3, 1, 8, combine, &error) && !combine->enabled,
        "disabled Combine must ignore malformed shared settings");
    Env("TILEXR_PROFILING_COMBINE_RECORDS_PATH", "combine-{rank}-{device}-{comm_domain}.records.v1");
    Check(!TileXR::LoadTileXRCombineProfilingRuntimeConfig(3, 1, 8, combine, &error),
        "enabled Combine must validate capture identity");
    Check(TileXR::LoadTileXRProfilingRuntimeConfig(3, 1, 8, dispatch, &error) && !dispatch->enabled,
        "Combine settings must not enable Dispatch");
    Env("TILEXR_PROFILING_CAPTURE_ID", "12345678-1234-5678-9abc-def012345678");
    Env("TILEXR_PROFILING_RECORDS_PER_CORE", "32");
    Check(TileXR::LoadTileXRCombineProfilingRuntimeConfig(3, 1, 8, combine, &error) && combine->enabled &&
        combine->recordsPath == "combine-3-1-8.records.v1", "Combine path expands its own identity");
    Env("TILEXR_PROFILING_RECORDS_PATH", "dispatch-{rank}-{device}-{comm_domain}.records.v1");
    Check(TileXR::LoadTileXRProfilingRuntimeConfig(3, 1, 8, dispatch, &error) && dispatch->enabled &&
        dispatch->recordsPath == "dispatch-3-1-8.records.v1", "default path remains Dispatch-compatible");
    Check(dispatch->captureId == combine->captureId && dispatch->recordsPerCore == combine->recordsPerCore,
        "regions share capture identity and configured capacity");

    TileXR::TileXRProfilingLaunchState d, c;
    uint32_t slot = 99;
    for (unsigned round = 0; round < 9; ++round) {
        Check(d.ReserveLatest(*dispatch, 16, &slot) && slot == 0, "Dispatch keeps its own latest width");
        Check(c.ReserveLatest(*combine, 8, &slot) && slot == 0, "Combine may use a different latest width");
    }
    Check(!c.ReserveLatest(*combine, 16, &slot) && slot == UINT32_MAX && c.Incomplete(),
        "changing Combine width marks only its region incomplete");
    Check(d.ReserveLatest(*dispatch, 16, &slot) && slot == 0 && !d.Incomplete(),
        "Combine reservation error cannot poison Dispatch");
    c.Reset();
    Check(c.ReserveLatest(*combine, 64, &slot) && slot == UINT32_MAX && c.Incomplete(),
        "Combine capacity exhaustion preserves communication with an invalid slot");
    Check(d.ReserveLatest(*dispatch, 16, &slot) && slot == 0 && !d.Incomplete(),
        "Combine capacity exhaustion cannot consume Dispatch capacity");
    Config disabled;
    Check(c.ReserveLatest(disabled, 8, &slot) && slot == UINT32_MAX,
        "disabled region never reserves a writer slot");
}
void WritersAndExports(const Config &dispatchConfig, const Config &combineConfig, const std::string &directory)
{
    Region d, c;
    TileXR::CommArgs args {};
    args.extraFlag = TileXR::ExtraFlag::PROFILING;
    args.tilexrProfilingTimestampsGM = reinterpret_cast<GM_ADDR>(d.times.data());
    args.tilexrProfilingEventIdsGM = reinterpret_cast<GM_ADDR>(d.events.data());
    args.tilexrProfilingRecordCountsGM = reinterpret_cast<GM_ADDR>(d.counts.data());
    args.tilexrProfilingDroppedCountsGM = reinterpret_cast<GM_ADDR>(d.dropped.data());
    args.tilexrProfilingRecordsPerCore = Region::kCapacity;
    args.tilexrProfilingCoreCapacity = Region::kCores;
    args.tilexrCombineProfilingTimestampsGM = reinterpret_cast<GM_ADDR>(c.times.data());
    args.tilexrCombineProfilingEventIdsGM = reinterpret_cast<GM_ADDR>(c.events.data());
    args.tilexrCombineProfilingRecordCountsGM = reinterpret_cast<GM_ADDR>(c.counts.data());
    args.tilexrCombineProfilingDroppedCountsGM = reinterpret_cast<GM_ADDR>(c.dropped.data());
    args.tilexrCombineProfilingRecordsPerCore = Region::kCapacity;
    args.tilexrCombineProfilingCoreCapacity = Region::kCores;
    std::array<uint8_t, sizeof(args)> argsBefore;
    std::memcpy(argsBefore.data(), &args, sizeof(args));
    const GM_ADDR address = reinterpret_cast<GM_ADDR>(&args);
    TileXR::DirectProfilingWriter dw, cw;
    dw.InitializeLatest(address, 0, 0, 16);
    for (uint32_t i = 0; i < 18; ++i) dw.RecordAt(0x53000001U, 100U + i);
    dw.Publish();
    Region dBefore = d;
    cw.InitializeLatestCombine(address, 0, 0, 8);
    for (uint32_t i = 0; i < 10; ++i) cw.RecordAt(0x54000001U, 200U + i);
    cw.Publish();
    Check(d.Same(dBefore), "Combine records/counts/overflow must not overwrite Dispatch");
#if defined(TILEXR_ENABLE_PROFILING)
    Check(d.counts[0] == 16 && c.counts[0] == 8 && d.dropped[0] == 2 && c.dropped[0] == 2,
        "different width launches maintain independent counts and losses");
    cw.InitializeLatestCombine(address, 0, 0, 8);
    cw.RecordAt(0x54000002U, 500);
    cw.RecordAt(0x54000003U, 550);
    cw.Publish();
    Check(c.counts[0] == 8 && c.dropped[0] == 0 && c.events[0] == 0x54000002U,
        "Combine latest resets its own prior overflow and records");
    Check(d.Same(dBefore), "Combine latest reset must preserve Dispatch capture");
    const Region cBefore = c;
    dw.InitializeLatest(address, 0, 0, 16);
    dw.RecordAt(0x53000002U, 400);
    dw.Publish();
    Check(d.counts[0] == 8 && d.dropped[0] == 0 && d.events[8] == 0x53000001U,
        "Dispatch latest leaves old tail outside valid count");
    Check(c.Same(cBefore), "Dispatch latest reset must preserve Combine capture");
    const uint64_t cycles = AscendC::CycleReads();
    args.tilexrCombineProfilingTimestampsGM = nullptr;
    cw.InitializeLatestCombine(address, 0, 0, 8);
    cw.Record(1);
    cw.Publish();
    Check(c.Same(cBefore) && AscendC::CycleReads() == cycles, "disabled Combine region must not sample/write");
    args.tilexrCombineProfilingTimestampsGM = reinterpret_cast<GM_ADDR>(c.times.data());
    args.tilexrProfilingTimestampsGM = nullptr;
    cw.InitializeLatestCombine(address, 1, 0, 8);
    cw.RecordAt(0x54000004U, 600);
    cw.Publish();
    Check(c.events[Region::kCapacity] == 0x54000004U && c.counts[TileXR::TILEXR_PROFILING_COUNT_STRIDE] == 8,
        "Combine remains active with disabled default region and independent core counters");
    args.tilexrProfilingTimestampsGM = reinterpret_cast<GM_ADDR>(d.times.data());
    const std::string dPath = directory + "/dispatch.records.v1", cPath = directory + "/combine.records.v1";
    Check(d.Export(dispatchConfig, dPath) && c.Export(combineConfig, cPath), "both captures export independently");
    const auto dBytes = Read(dPath), cBytes = Read(cPath);
    Check(U32(dBytes, 52) == 1 && U32(dBytes, 80) == 0x53000002U,
        "Dispatch export includes latest event only, excluding stale tail");
    Check(U32(cBytes, 52) == 2 && U32(cBytes, 80) == 0x54000002U,
        "Combine export uses its own events and occupied counts");
    Check(U32(dBytes, 56) == 0 && U32(cBytes, 56) == 0, "latest resets each export's dropped count");
    Check(dBytes.size() > 40 && cBytes.size() > 40 &&
        std::memcmp(dBytes.data() + 24, cBytes.data() + 24, 16) == 0, "both files retain common capture UUID");
    c.dropped[0] = 3;
    Config incomplete = combineConfig;
    incomplete.incomplete = true;
    const std::string overflow = directory + "/combine-overflow.records.v1";
    Check(c.Export(incomplete, overflow), "incomplete Combine capture remains exportable evidence");
    Check(U32(Read(overflow), 56) == UINT32_MAX && U32(Read(overflow), 60) == 3,
        "incomplete Combine export is explicitly marked overflow");
    Check(Read(dPath) == dBytes, "Combine overflow export leaves Dispatch file unchanged");
    Config disabled;
    disabled.recordsPath = directory + "/disabled.records.v1";
    std::string error;
    Check(TileXR::WriteTileXRProfilingRecords(disabled, 3, {99}, {}, {}, {}, &error) &&
        !std::ifstream(disabled.recordsPath).good(), "disabled region ignores malformed capture and creates no file");
    for (const auto &path : {dPath, cPath, overflow}) std::remove(path.c_str());
#else
    (void)dispatchConfig; (void)combineConfig; (void)directory;
    const Region empty;
    cw.InitializeLatestCombine(address, 1, 0, 8);
    cw.Record(1);
    cw.Publish();
    Check(d.Same(empty) && c.Same(empty) && AscendC::CycleReads() == 0,
        "profiling OFF must preserve both regions and avoid timestamp reads");
    static_assert(sizeof(TileXR::DirectProfilingWriter) == 1, "OFF writer carries no state");
#endif
    Check(std::memcmp(argsBefore.data(), &args, sizeof(args)) == 0, "region selection must never mutate CommArgs");
}
}
int main()
{
    Config dispatch, combine;
    ConfigurationAndReservations(&dispatch, &combine);
    char directory[] = "/tmp/tilexr-combine-profile-regions-XXXXXX";
    if (!mkdtemp(directory)) { std::cerr << "cannot create test directory\n"; return 1; }
    WritersAndExports(dispatch, combine, directory);
    rmdir(directory);
    if (failures != 0) return 1;
    std::cout << "EP_SERVER_DEDUP_COMBINE_PROFILE_REGIONS_PASS\n";
    return 0;
}
