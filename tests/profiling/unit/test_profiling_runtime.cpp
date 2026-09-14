#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "tilexr_profiling_runtime.h"
#include "tilexr_profiling.h"

namespace {

int gFailures = 0;

void Check(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << message << std::endl;
        ++gFailures;
    }
}

uint32_t ReadU32(const std::vector<uint8_t>& bytes, std::size_t offset)
{
    return static_cast<uint32_t>(bytes[offset]) |
        (static_cast<uint32_t>(bytes[offset + 1U]) << 8U) |
        (static_cast<uint32_t>(bytes[offset + 2U]) << 16U) |
        (static_cast<uint32_t>(bytes[offset + 3U]) << 24U);
}

uint64_t ReadU64(const std::vector<uint8_t>& bytes, std::size_t offset)
{
    uint64_t value = 0U;
    for (uint32_t index = 0U; index < 8U; ++index) {
        value |= static_cast<uint64_t>(bytes[offset + index]) << (index * 8U);
    }
    return value;
}

void SetEnv(const char *name, const char *value)
{
#if defined(_WIN32)
    _putenv_s(name, value == nullptr ? "" : value);
#else
    if (value == nullptr) {
        unsetenv(name);
    } else {
        setenv(name, value, 1);
    }
#endif
}

void TestRuntimeConfig()
{
    TileXR::TileXRProfilingRuntimeConfig config;
    std::string error;
    SetEnv("TILEXR_PROFILING_RECORDS_PATH", nullptr);
    Check(TileXR::LoadTileXRProfilingRuntimeConfig(0, 0, 0, &config, &error) &&
        !config.enabled, "profiling must be disabled without a capture path");
    SetEnv("TILEXR_PROFILING_RECORDS_PATH", "rank-{rank}-device-{device}-domain-{comm_domain}.bin");
    SetEnv("TILEXR_PROFILING_CAPTURE_ID", "00000000-0000-4000-8000-000000000001");
    SetEnv("TILEXR_PROFILING_RECORDS_PER_CORE", "196608");
    Check(TileXR::LoadTileXRProfilingRuntimeConfig(2, 3, 4, &config, &error) &&
        config.enabled && config.recordsPerCore == 3U * 65536U &&
        config.recordsPath == "rank-2-device-3-domain-4.bin",
        "capture config must support several large launch reservations");
    const std::string maximum = std::to_string(TileXR::TILEXR_PROFILING_MAX_RECORDS_PER_CORE);
    SetEnv("TILEXR_PROFILING_RECORDS_PER_CORE", maximum.c_str());
    Check(TileXR::LoadTileXRProfilingRuntimeConfig(0, 0, 0, &config, &error),
        "maximum configured capacity must parse without uint32 overflow");
    const uint64_t maxExport = 48U + 64U * (24U +
        static_cast<uint64_t>(config.recordsPerCore) * 16U);
    Check(maxExport <= (64ULL << 30U), "maximum records.v1 export must fit parser limit");
    for (const char *invalid : {"0", "7", "8", "24", "67108864", "4294967296",
            "18446744073709551616", "-8", "+8", " 8", "8x"}) {
        SetEnv("TILEXR_PROFILING_RECORDS_PER_CORE", invalid);
        Check(!TileXR::LoadTileXRProfilingRuntimeConfig(0, 0, 0, &config, &error),
            "invalid/overflowing capacity must be rejected");
    }
    SetEnv("TILEXR_PROFILING_RECORDS_PER_CORE", nullptr);
    Check(TileXR::LoadTileXRProfilingRuntimeConfig(0, 0, 0, &config, &error) &&
        config.recordsPerCore == 4096U, "default capture capacity must stay compatible");
    SetEnv("TILEXR_PROFILING_RECORDS_PATH", nullptr);
    SetEnv("TILEXR_PROFILING_CAPTURE_ID", nullptr);
}

void TestLaunchReservations()
{
    TileXR::TileXRProfilingRuntimeConfig config;
    TileXR::TileXRProfilingLaunchState launches;
    const uint32_t disabled = std::numeric_limits<uint32_t>::max();
    uint32_t slot = 0U;
    Check(launches.Reserve(config, 65536U, &slot) && slot == disabled &&
        !launches.Incomplete(), "disabled capture must not consume launch slots");
    config.enabled = true;
    config.recordsPerCore = 2U * 65536U;
    Check(!launches.Reserve(config, 8U, nullptr), "null launch output must be rejected");
    Check(!launches.Reserve(config, 0U, &slot) && slot == disabled,
        "zero launch budget must be rejected");
    Check(!launches.Reserve(config, 7U, &slot), "unaligned launch budget must be rejected");
    Check(launches.Reserve(config, 65536U, &slot) && slot == 0U,
        "first launch reservation must start at zero");
    Check(launches.Reserve(config, 65536U, &slot) && slot == 1U,
        "second launch must not reuse first launch memory");
    Check(launches.Reserve(config, 65536U, &slot) && slot == disabled &&
        launches.Incomplete(), "exhaustion must disable capture only and mark incomplete");
    Check(launches.Reserve(config, 65536U, &slot) && slot == disabled,
        "repeated exhaustion must not wrap the launch counter");
    launches.Reset();
    Check(!launches.Incomplete(), "reset must clear earlier capture exhaustion");
    config.recordsPerCore = 40U;
    Check(launches.Reserve(config, 8U, &slot) && slot == 0U,
        "small first reservation must succeed");
    Check(launches.Reserve(config, 16U, &slot) && slot == 1U,
        "changed launch width must align beyond previous reservation");
    Check(launches.Reserve(config, 8U, &slot) && slot == 4U,
        "smaller subsequent launch must not overlap larger launch");
    launches.Reset();
    config.recordsPerCore = TileXR::TILEXR_PROFILING_MAX_RECORDS_PER_CORE;
    Check(launches.Reserve(config, 0xfffffff8U, &slot) && slot == disabled &&
        launches.Incomplete(), "oversized launch arithmetic must not overflow");
    Check(launches.Reserve(config, config.recordsPerCore, &slot) && slot == 0U &&
        launches.Incomplete(), "successful later reservation must preserve incomplete state");
    Check(launches.Reserve(config, config.recordsPerCore, &slot) && slot == disabled,
        "near-limit repeated reservation must not wrap");
    launches.Reset();
    config.recordsPerCore = 800U;
    std::set<uint32_t> reserved;
    std::mutex reservedMutex;
    std::vector<std::thread> threads;
    for (uint32_t worker = 0U; worker < 4U; ++worker) {
        threads.emplace_back([&] {
            for (uint32_t index = 0U; index < 25U; ++index) {
                uint32_t threadSlot = disabled;
                const bool ok = launches.Reserve(config, 8U, &threadSlot);
                std::lock_guard<std::mutex> lock(reservedMutex);
                Check(ok && threadSlot != disabled && reserved.insert(threadSlot).second,
                    "concurrent Host reservations must allocate unique slots");
            }
        });
    }
    for (auto &thread : threads) {
        thread.join();
    }
    Check(reserved.size() == 100U && !launches.Incomplete(),
        "all concurrent reservations must fit the expected capture");
}

void TestLatestReservations()
{
    TileXR::TileXRProfilingRuntimeConfig config;
    TileXR::TileXRProfilingLaunchState launches;
    const uint32_t disabled = std::numeric_limits<uint32_t>::max();
    uint32_t slot = 0U;
    Check(launches.ReserveLatest(config, 65536U, &slot) && slot == disabled &&
        !launches.LatestLaunch(), "disabled latest capture must not select a reservation mode");
    config.enabled = true;
    config.recordsPerCore = 65536U;
    Check(!launches.ReserveLatest(config, 8U, nullptr), "null latest output must fail");
    Check(!launches.ReserveLatest(config, 0U, &slot) && slot == disabled,
        "zero latest width must fail safely");
    Check(!launches.ReserveLatest(config, 7U, &slot), "unaligned latest width must fail");
    for (uint32_t launch = 0U; launch < 10000U; ++launch) {
        Check(launches.ReserveLatest(config, 65536U, &slot) && slot == 0U &&
            launches.LatestLaunch() && !launches.Incomplete(),
            "latest capture must keep constant storage beyond the old launch limit");
    }
    Check(!launches.Reserve(config, 65536U, &slot) && slot == disabled &&
        launches.Incomplete(), "reserved writer must not mix with latest capture");
    launches.Reset();
    Check(!launches.LatestLaunch() && !launches.Incomplete(), "reset must clear capture mode");
    Check(launches.Reserve(config, 65536U, &slot) && slot == 0U,
        "reset must allow ordinary reservations again");
    Check(!launches.ReserveLatest(config, 65536U, &slot) && slot == disabled &&
        !launches.LatestLaunch() && launches.Incomplete(),
        "latest writer must not overwrite an ordinary reserved capture");
    launches.Reset();
    Check(launches.ReserveLatest(config, 8U, &slot) && slot == 0U,
        "fresh latest capture must accept a smaller fixed budget");
    Check(!launches.ReserveLatest(config, 16U, &slot) && slot == disabled &&
        launches.Incomplete(), "latest capture must reject changed launch budgets");
    launches.Reset();
    Check(launches.ReserveLatest(config, 0xfffffff8U, &slot) && slot == disabled &&
        launches.LatestLaunch() && launches.Incomplete(),
        "oversized latest budget must disable capture without integer overflow");
}

void TestLatestExport()
{
    const std::string path = "tilexr-profiling-latest-test.records.bin";
    (void)std::remove(path.c_str());
    TileXR::TileXRProfilingRuntimeConfig config;
    config.enabled = config.latestLaunch = true;
    config.recordsPath = path;
    config.recordsPerCore = 24U;
    config.coreCapacity = 2U;
    // Both cores retain nonzero old data. Core zero's final launch is shorter;
    // core one's final launch is empty. Only the first two events remain valid.
    std::vector<uint64_t> timestamps(48U, 100U);
    std::vector<uint32_t> eventIds(48U, 99U);
    timestamps[0] = 300U;
    timestamps[1] = 200U;
    eventIds[0] = 2U;
    eventIds[1] = 1U;
    std::fill(eventIds.begin() + 2U, eventIds.begin() + 8U, 0U);
    std::string error;
    Check(TileXR::WriteTileXRProfilingRecords(config, 1, {8U, 0U}, {0U, 0U},
        timestamps, eventIds, &error), "latest capture must export a shorter valid range");
    std::ifstream input(path.c_str(), std::ios::binary);
    const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(input)),
        std::istreambuf_iterator<char>());
    Check(bytes.size() == 104U && ReadU32(bytes, 20U) == 1U &&
        ReadU32(bytes, 48U) == TileXR::TileXRProfilingStreamId(1U, 0U) &&
        ReadU32(bytes, 52U) == 2U && ReadU32(bytes, 56U) == 0U &&
        ReadU64(bytes, 72U) == 200U && ReadU32(bytes, 80U) == 1U &&
        ReadU64(bytes, 88U) == 300U && ReadU32(bytes, 96U) == 2U,
        "latest export must exclude stale tails and empty cores, preserving sorted valid events");
    input.close();
    (void)std::remove(path.c_str());
    Check(!TileXR::WriteTileXRProfilingRecords(config, 1, {32U, 0U}, {0U, 0U},
        timestamps, eventIds, &error), "latest exporter must reject count beyond GM capacity");
    Check(!TileXR::WriteTileXRProfilingRecords(config, 1, {7U, 0U}, {0U, 0U},
        timestamps, eventIds, &error), "latest exporter must reject unaligned publication");
}

} // namespace

int main()
{
    TestRuntimeConfig();
    TestLaunchReservations();
    TestLatestReservations();
    TestLatestExport();
    const std::string path = "tilexr-profiling-runtime-test.records.bin";
    (void)std::remove(path.c_str());
    TileXR::TileXRProfilingRuntimeConfig config {};
    config.enabled = true;
    config.recordsPath = path;
    config.recordsPerCore = 8U;
    config.coreCapacity = 2U;
    config.captureId = {{
        0xadU, 0xd4U, 0x00U, 0x00U, 0x00U, 0x00U, 0x40U, 0x00U,
        0x80U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x01U,
    }};
    const std::vector<uint32_t> occupied {0U, 0U};
    const std::vector<uint32_t> dropped {0U, 0U};
    std::vector<uint64_t> timestamps(16U, 0U);
    std::vector<uint32_t> eventIds(16U, 0U);
    timestamps[6] = 200U;
    timestamps[7] = 100U;
    eventIds[6] = 2U;
    eventIds[7] = 1U;

    std::string error;
    Check(TileXR::WriteTileXRProfilingRecords(config, 0, occupied, dropped,
        timestamps, eventIds, &error), error.c_str());
    std::ifstream input(path.c_str(), std::ios::binary);
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(input)),
        std::istreambuf_iterator<char>());
    Check(bytes.size() == 104U, "records.v1 output size mismatch");
    Check(bytes.size() >= 56U && std::string(reinterpret_cast<const char*>(bytes.data()), 7U) ==
        "TXRPRF1", "records.v1 magic mismatch");
    Check(bytes.size() >= 24U && ReadU32(bytes, 20U) == 1U,
        "records.v1 stream count mismatch");
    Check(bytes.size() >= 56U && ReadU32(bytes, 48U) == 0U,
        "records.v1 stream ID mismatch");
    Check(bytes.size() >= 60U && ReadU32(bytes, 52U) == 2U,
        "records.v1 record count must exclude aligned padding");
    Check(bytes.size() >= 104U && ReadU64(bytes, 72U) == 100U &&
        ReadU32(bytes, 80U) == 1U && ReadU64(bytes, 88U) == 200U &&
        ReadU32(bytes, 96U) == 2U,
        "records exporter must sort direct-writer timestamps");
    input.close();
    Check(!TileXR::WriteTileXRProfilingRecords(config, 0, occupied, dropped,
        timestamps, eventIds, &error), "records exporter must not overwrite an existing capture");
    (void)std::remove(path.c_str());
    config.incomplete = true;
    std::fill(timestamps.begin(), timestamps.end(), 0U);
    std::fill(eventIds.begin(), eventIds.end(), 0U);
    Check(TileXR::WriteTileXRProfilingRecords(config, 0, occupied, dropped,
        timestamps, eventIds, &error), "empty exhausted capture must still be exported");
    std::ifstream incompleteInput(path.c_str(), std::ios::binary);
    bytes.assign(std::istreambuf_iterator<char>(incompleteInput),
        std::istreambuf_iterator<char>());
    Check(bytes.size() == 96U && ReadU32(bytes, 20U) == 2U &&
        ReadU32(bytes, 56U) == std::numeric_limits<uint32_t>::max() &&
        (ReadU32(bytes, 60U) & 2U) != 0U &&
        ReadU32(bytes, 80U) == std::numeric_limits<uint32_t>::max(),
        "slot exhaustion must be visible as overflow on every exported core stream");
    incompleteInput.close();
    (void)std::remove(path.c_str());
    return gFailures == 0 ? 0 : 1;
}
