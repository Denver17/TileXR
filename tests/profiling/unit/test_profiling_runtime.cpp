#include <array>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

#include "tilexr_profiling_runtime.h"

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

} // namespace

int main()
{
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
    return gFailures == 0 ? 0 : 1;
}
