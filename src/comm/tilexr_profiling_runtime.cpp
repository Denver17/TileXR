/*
 * Copyright (c) 2026 TileXR Project
 * Licensed under the CANN Open Software License Agreement Version 2.0.
 */

#include "tilexr_profiling_runtime.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <sstream>

#include "tilexr_profiling.h"

namespace TileXR {
namespace {

constexpr uint16_t kRecordsFormatMajor = 1U;
constexpr uint16_t kRecordsFormatMinor = 0U;
constexpr uint16_t kRecordsFileHeaderBytes = 48U;
constexpr uint16_t kRecordsRecordBytes = 16U;
constexpr uint16_t kRecordsStreamHeaderBytes = 24U;
constexpr uint8_t kRecordsLittleEndian = 1U;
constexpr uint32_t kStreamComplete = 1U << 0U;
constexpr uint32_t kStreamOverflow = 1U << 1U;

struct ExportRecord {
    uint64_t timestamp = 0U;
    uint32_t eventId = 0U;
};

struct ExportStream {
    uint32_t id = 0U;
    uint32_t droppedCount = 0U;
    std::vector<ExportRecord> records;
};

void SetError(std::string *error, const std::string &message)
{
    if (error != nullptr) {
        *error = message;
    }
}

bool HexNibble(char value, uint8_t *result)
{
    if (value >= '0' && value <= '9') {
        *result = static_cast<uint8_t>(value - '0');
        return true;
    }
    if (value >= 'a' && value <= 'f') {
        *result = static_cast<uint8_t>(value - 'a' + 10);
        return true;
    }
    if (value >= 'A' && value <= 'F') {
        *result = static_cast<uint8_t>(value - 'A' + 10);
        return true;
    }
    return false;
}

bool ParseCaptureId(const char *value, std::array<uint8_t, 16U> *captureId)
{
    if (value == nullptr || captureId == nullptr) {
        return false;
    }
    const std::string text(value);
    if (text.size() != 36U || text[8] != '-' || text[13] != '-' ||
        text[18] != '-' || text[23] != '-') {
        return false;
    }
    std::array<uint8_t, 16U> parsed {};
    uint32_t output = 0U;
    for (uint32_t index = 0U; index < text.size();) {
        if (text[index] == '-') {
            ++index;
            continue;
        }
        if (index + 1U >= text.size() || output >= parsed.size()) {
            return false;
        }
        uint8_t high = 0U;
        uint8_t low = 0U;
        if (!HexNibble(text[index], &high) || !HexNibble(text[index + 1U], &low)) {
            return false;
        }
        parsed[output++] = static_cast<uint8_t>((high << 4U) | low);
        index += 2U;
    }
    if (output != parsed.size()) {
        return false;
    }
    *captureId = parsed;
    return true;
}

bool ParseRecordsPerCore(const char *value, uint32_t *result)
{
    if (result == nullptr) {
        return false;
    }
    if (value == nullptr || value[0] == '\0') {
        *result = TILEXR_PROFILING_DEFAULT_RECORDS_PER_CORE;
        return true;
    }
    errno = 0;
    char *end = nullptr;
    for (const char *digit = value; *digit != '\0'; ++digit) {
        if (*digit < '0' || *digit > '9') {
            return false;
        }
    }
    const unsigned long long parsed = std::strtoull(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0' || parsed == 0ULL ||
        parsed > TILEXR_PROFILING_MAX_RECORDS_PER_CORE ||
        parsed % TILEXR_PROFILING_GM_ALIGNMENT_RECORDS != 0UL) {
        return false;
    }
    *result = static_cast<uint32_t>(parsed);
    return true;
}

void ReplaceAll(std::string *text, const std::string &token, const std::string &value)
{
    std::size_t position = 0U;
    while ((position = text->find(token, position)) != std::string::npos) {
        text->replace(position, token.size(), value);
        position += value.size();
    }
}

std::string ExpandRecordsPath(const char *path, int rank, int device, int commDomain)
{
    std::string result(path);
    ReplaceAll(&result, "{rank}", std::to_string(rank));
    ReplaceAll(&result, "{device}", std::to_string(device));
    ReplaceAll(&result, "{comm_domain}", std::to_string(commDomain));
    return result;
}

void WriteU16(std::ofstream *output, uint16_t value)
{
    const char bytes[2] = {
        static_cast<char>(value & 0xffU),
        static_cast<char>((value >> 8U) & 0xffU),
    };
    output->write(bytes, sizeof(bytes));
}

void WriteU32(std::ofstream *output, uint32_t value)
{
    char bytes[4] = {};
    for (uint32_t index = 0U; index < 4U; ++index) {
        bytes[index] = static_cast<char>((value >> (index * 8U)) & 0xffU);
    }
    output->write(bytes, sizeof(bytes));
}

void WriteU64(std::ofstream *output, uint64_t value)
{
    char bytes[8] = {};
    for (uint32_t index = 0U; index < 8U; ++index) {
        bytes[index] = static_cast<char>((value >> (index * 8U)) & 0xffU);
    }
    output->write(bytes, sizeof(bytes));
}

bool BuildExportStreams(const TileXRProfilingRuntimeConfig &config, int rank,
    const std::vector<uint32_t> &occupiedCounts,
    const std::vector<uint32_t> &droppedCounts,
    const std::vector<uint64_t> &timestamps,
    const std::vector<uint32_t> &eventIds,
    std::vector<ExportStream> *streams, std::string *error)
{
    const uint64_t expectedSlots =
        static_cast<uint64_t>(config.coreCapacity) * config.recordsPerCore;
    if (occupiedCounts.size() != config.coreCapacity ||
        droppedCounts.size() != config.coreCapacity ||
        timestamps.size() != expectedSlots || eventIds.size() != expectedSlots) {
        SetError(error, "profiling device capture size does not match runtime configuration");
        return false;
    }

    for (uint32_t core = 0U; core < config.coreCapacity; ++core) {
        const uint32_t occupied = occupiedCounts[core];
        if (occupied > config.recordsPerCore ||
            occupied % TILEXR_PROFILING_GM_ALIGNMENT_RECORDS != 0U) {
            SetError(error, "profiling occupied count is invalid for core " +
                std::to_string(core));
            return false;
        }
        ExportStream stream {};
        stream.id = TileXRProfilingStreamId(static_cast<uint32_t>(rank), core);
        // UINT32_MAX denotes saturated/unknown loss when an entire launch was
        // skipped. Export even empty streams so exhaustion cannot look complete.
        stream.droppedCount = config.incomplete ?
            std::numeric_limits<uint32_t>::max() : droppedCounts[core];
        stream.records.reserve(occupied);
        const uint64_t base = static_cast<uint64_t>(core) * config.recordsPerCore;
        for (uint32_t slot = 0U; slot < config.recordsPerCore; ++slot) {
            const uint32_t eventId = eventIds[base + slot];
            if (eventId == 0U) {
                continue;
            }
            const uint64_t timestamp = timestamps[base + slot];
            stream.records.push_back(ExportRecord {timestamp, eventId});
        }
        std::stable_sort(stream.records.begin(), stream.records.end(),
            [](const ExportRecord &left, const ExportRecord &right) {
                return left.timestamp < right.timestamp;
            });
        if (!stream.records.empty() || stream.droppedCount != 0U) {
            streams->push_back(std::move(stream));
        }
    }
    return true;
}

} // namespace

bool TileXRProfilingLaunchState::Reserve(
    const TileXRProfilingRuntimeConfig &config, uint32_t recordsPerLaunch,
    uint32_t *launchSlot)
{
    if (launchSlot == nullptr) {
        return false;
    }
    *launchSlot = std::numeric_limits<uint32_t>::max();
    if (recordsPerLaunch == 0U || recordsPerLaunch %
            TILEXR_PROFILING_GM_ALIGNMENT_RECORDS != 0U) {
        return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (!config.enabled) {
        return true;
    }
    // Different operators may request different slot widths. Align the next
    // reservation to its own width rather than reusing a global slot counter.
    const uint64_t slot = (static_cast<uint64_t>(nextRecord_) +
        recordsPerLaunch - 1U) / recordsPerLaunch;
    const uint64_t end = (slot + 1U) * recordsPerLaunch;
    if (end > config.recordsPerCore) {
        incomplete_ = true;
        return true;
    }
    *launchSlot = static_cast<uint32_t>(slot);
    nextRecord_ = static_cast<uint32_t>(end);
    return true;
}

bool TileXRProfilingLaunchState::Incomplete() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return incomplete_;
}

void TileXRProfilingLaunchState::Reset()
{
    std::lock_guard<std::mutex> lock(mutex_);
    nextRecord_ = 0U;
    incomplete_ = false;
}

bool LoadTileXRProfilingRuntimeConfig(int rank, int device, int commDomain,
    TileXRProfilingRuntimeConfig *config, std::string *error)
{
    if (config == nullptr) {
        SetError(error, "profiling runtime config output is null");
        return false;
    }
    *config = TileXRProfilingRuntimeConfig {};
    const char *recordsPath = std::getenv("TILEXR_PROFILING_RECORDS_PATH");
    if (recordsPath == nullptr || recordsPath[0] == '\0') {
        return true;
    }
    const char *captureId = std::getenv("TILEXR_PROFILING_CAPTURE_ID");
    if (!ParseCaptureId(captureId, &config->captureId)) {
        SetError(error, "TILEXR_PROFILING_CAPTURE_ID must be a canonical UUID");
        return false;
    }
    if (!ParseRecordsPerCore(std::getenv("TILEXR_PROFILING_RECORDS_PER_CORE"),
            &config->recordsPerCore)) {
        SetError(error, "TILEXR_PROFILING_RECORDS_PER_CORE must be an aligned integer in [8, 67108856]");
        return false;
    }
    config->recordsPath = ExpandRecordsPath(recordsPath, rank, device, commDomain);
    if (config->recordsPath.empty()) {
        SetError(error, "expanded profiling records path is empty");
        return false;
    }
    config->coreCapacity = TILEXR_PROFILING_MAX_CORE_COUNT;
    config->enabled = true;
    return true;
}

bool WriteTileXRProfilingRecords(const TileXRProfilingRuntimeConfig &config,
    int rank, const std::vector<uint32_t> &occupiedCounts,
    const std::vector<uint32_t> &droppedCounts,
    const std::vector<uint64_t> &timestamps,
    const std::vector<uint32_t> &eventIds, std::string *error)
{
    if (!config.enabled) {
        return true;
    }
    std::vector<ExportStream> streams;
    if (!BuildExportStreams(config, rank, occupiedCounts, droppedCounts,
            timestamps, eventIds, &streams, error)) {
        return false;
    }
    if (streams.size() > std::numeric_limits<uint32_t>::max()) {
        SetError(error, "profiling stream count exceeds records.v1 capacity");
        return false;
    }

    uint64_t fileBytes = kRecordsFileHeaderBytes;
    for (const auto &stream : streams) {
        const uint64_t streamBytes = kRecordsStreamHeaderBytes +
            static_cast<uint64_t>(stream.records.size()) * kRecordsRecordBytes;
        if (fileBytes > std::numeric_limits<uint64_t>::max() - streamBytes) {
            SetError(error, "profiling records file size overflow");
            return false;
        }
        fileBytes += streamBytes;
    }

    std::ifstream existing(config.recordsPath.c_str(), std::ios::binary);
    if (existing.good()) {
        SetError(error, "profiling records path already exists: " + config.recordsPath);
        return false;
    }
    const std::string temporaryPath = config.recordsPath + ".tmp";
    std::ofstream output(temporaryPath.c_str(), std::ios::binary | std::ios::trunc);
    if (!output) {
        SetError(error, "cannot create profiling records file: " + temporaryPath);
        return false;
    }

    const char magic[8] = {'T', 'X', 'R', 'P', 'R', 'F', '1', '\0'};
    output.write(magic, sizeof(magic));
    WriteU16(&output, kRecordsFormatMajor);
    WriteU16(&output, kRecordsFormatMinor);
    WriteU16(&output, kRecordsFileHeaderBytes);
    WriteU16(&output, kRecordsRecordBytes);
    WriteU16(&output, kRecordsStreamHeaderBytes);
    output.put(static_cast<char>(kRecordsLittleEndian));
    output.put(0);
    WriteU32(&output, static_cast<uint32_t>(streams.size()));
    output.write(reinterpret_cast<const char *>(config.captureId.data()),
        static_cast<std::streamsize>(config.captureId.size()));
    WriteU64(&output, fileBytes);

    for (const auto &stream : streams) {
        const uint64_t blockBytes = kRecordsStreamHeaderBytes +
            static_cast<uint64_t>(stream.records.size()) * kRecordsRecordBytes;
        WriteU32(&output, stream.id);
        WriteU32(&output, static_cast<uint32_t>(stream.records.size()));
        WriteU32(&output, stream.droppedCount);
        WriteU32(&output, kStreamComplete |
            (stream.droppedCount == 0U ? 0U : kStreamOverflow));
        WriteU64(&output, blockBytes);
        for (const auto &record : stream.records) {
            WriteU64(&output, record.timestamp);
            WriteU32(&output, record.eventId);
            WriteU32(&output, 0U);
        }
    }
    output.close();
    if (!output) {
        std::remove(temporaryPath.c_str());
        SetError(error, "failed while writing profiling records file: " + temporaryPath);
        return false;
    }
    if (std::rename(temporaryPath.c_str(), config.recordsPath.c_str()) != 0) {
        const int renameError = errno;
        std::remove(temporaryPath.c_str());
        SetError(error, "cannot publish profiling records file " + config.recordsPath +
            ": errno=" + std::to_string(renameError));
        return false;
    }
    return true;
}

} // namespace TileXR
