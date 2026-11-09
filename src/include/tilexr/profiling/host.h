/* Copyright (c) 2026 TileXR Project. Licensed under the CANN Open Software
 * License Agreement Version 2.0. */
// 功能：计算带稀疏 lane 与代次的 workspace 布局，并验证和编码快照；供 ACL 适配器和离线 Host 调用。
// Purpose: Compute workspace layouts with sparse lanes/generations and validate/encode snapshots for ACL adapters and offline Host callers.
#ifndef TILEXR_PROFILING_HOST_H
#define TILEXR_PROFILING_HOST_H
#include "types.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <set>
#include <string>
#include <vector>

namespace TileXR { namespace Profiling {
constexpr uint64_t MAX_RECORD_FILE_BYTES = UINT64_C(256) * 1024 * 1024;
enum class StatusCode { Ok, InvalidArgument, InvalidState, Acl, Snapshot, Io, PublishedCleanupWarning };
struct Status {
    StatusCode code = StatusCode::Ok;
    std::string message;
    int aclError = 0;
    Status() = default;
    Status(StatusCode c, const std::string &m, int a = 0) : code(c), message(m), aclError(a) {}
    bool ok() const { return code == StatusCode::Ok || code == StatusCode::PublishedCleanupWarning; }
    explicit operator bool() const { return ok(); }
};
struct CaptureConfig {
    bool enabled = true;
    int64_t rank = 0;
    uint32_t logicalStreamCount = 1;
    uint32_t recordsPerStream = DEFAULT_RECORDS_PER_STREAM;
    std::array<uint8_t, 16> captureId {};
    uint64_t maxWorkspaceBytes = 1073741824ULL;
    uint64_t maxFileBytes = MAX_RECORD_FILE_BYTES;
    // Empty preserves dense [0,count). Sparse IDs must be strictly increasing.
    std::vector<uint32_t> logicalLaneIds;
    uint64_t generation = 0;
};
struct Layout {
    uint64_t descriptorOffset = 0, timestampsOffset = 0, eventIdsOffset = 0;
    uint64_t countsOffset = 0, droppedOffset = 0, statesOffset = 0, bytes = 0;
    uint32_t recordStride = 0, stateStride = COUNT_STRIDE;
    uint64_t recordElements = 0, stateElements = 0;
    uint64_t logicalLaneIdsOffset = 0;
};
inline Status BuildLayout(const CaptureConfig &config, Layout *out)
{
    if (!out || config.rank < 0 || config.rank > 65535 || config.logicalStreamCount == 0 ||
        config.logicalStreamCount > MAX_LOGICAL_STREAMS || config.recordsPerStream < 8 ||
        config.recordsPerStream > MAX_RECORDS_PER_STREAM || config.recordsPerStream % 8 ||
        config.maxFileBytes > MAX_RECORD_FILE_BYTES)
        return {StatusCode::InvalidArgument, "rank/lane/capacity outside profiling limits"};
    if (!config.logicalLaneIds.empty()) {
        if (config.logicalLaneIds.size() != config.logicalStreamCount)
            return {StatusCode::InvalidArgument, "sparse lane count does not match physical slots"};
        for (size_t i = 0; i < config.logicalLaneIds.size(); ++i)
            if (config.logicalLaneIds[i] > 65535 || (i && config.logicalLaneIds[i-1] >= config.logicalLaneIds[i]))
                return {StatusCode::InvalidArgument, "sparse lane IDs must be unique, sorted uint16 values"};
    }
    Layout l;
    l.recordStride = (config.recordsPerStream + RECORD_STRIDE_ALIGNMENT - 1U) /
        RECORD_STRIDE_ALIGNMENT * RECORD_STRIDE_ALIGNMENT;
    l.recordElements = uint64_t(config.logicalStreamCount) * l.recordStride;
    l.stateElements = uint64_t(config.logicalStreamCount) * l.stateStride;
    l.timestampsOffset = 128;
    l.eventIdsOffset = l.timestampsOffset + l.recordElements * sizeof(uint64_t);
    l.countsOffset = l.eventIdsOffset + l.recordElements * sizeof(uint32_t);
    l.droppedOffset = l.countsOffset + l.stateElements * sizeof(uint32_t);
    l.statesOffset = l.droppedOffset + l.stateElements * sizeof(uint32_t);
    l.bytes = l.statesOffset + l.stateElements * sizeof(uint32_t);
    if (!config.logicalLaneIds.empty()) {
        l.logicalLaneIdsOffset = l.bytes;
        l.bytes += (uint64_t(config.logicalStreamCount) * sizeof(uint32_t) + 63U) / 64U * 64U;
    }
    const uint64_t fileBytes = 48 + uint64_t(config.logicalStreamCount) *
        (24 + uint64_t(config.recordsPerStream) * 16);
    if (l.bytes > config.maxWorkspaceBytes || fileBytes > config.maxFileBytes || fileBytes > MAX_RECORD_FILE_BYTES ||
        l.bytes > std::numeric_limits<size_t>::max() || fileBytes > std::numeric_limits<size_t>::max())
        return {StatusCode::InvalidArgument, "capture exceeds workspace or records byte budget"};
    *out = l;
    return {};
}
// Arithmetic helper for an already validated allocation/layout. External
// workspace users should call the checked overload below.
inline DeviceView MakeDeviceView(const CaptureConfig &config, const Layout &l, void *workspace)
{
    const uint64_t base = reinterpret_cast<uintptr_t>(workspace);
    DeviceView v {};
    v.abiVersion = DEVICE_VIEW_ABI_VERSION; v.descriptorBytes = sizeof(DeviceView);
    v.enabled = config.enabled ? 1 : 0; v.logicalStreamCount = config.logicalStreamCount;
    v.recordsPerStream = config.recordsPerStream; v.recordStride = l.recordStride; v.stateStride = l.stateStride;
    v.timestampsAddress = base + l.timestampsOffset; v.eventIdsAddress = base + l.eventIdsOffset;
    v.countsAddress = base + l.countsOffset; v.droppedAddress = base + l.droppedOffset;
    v.statesAddress = base + l.statesOffset;
    v.logicalLaneIdsAddress = l.logicalLaneIdsOffset ? base + l.logicalLaneIdsOffset : 0;
    v.generation = config.generation;
    return v;
}
// Validate a caller-owned ordinary-device allocation before constructing its
// descriptor. The caller still owns allocation lifetime and stream completion.
inline Status MakeDeviceView(const CaptureConfig &config, void *workspace,
    uint64_t workspaceBytes, DeviceView *out)
{
    Layout layout; Status s = BuildLayout(config,&layout); if (!s) return s;
    const uint64_t base = reinterpret_cast<uintptr_t>(workspace);
    if (!out || !base || base % CACHE_LINE_BYTES || workspaceBytes < layout.bytes ||
        layout.bytes > std::numeric_limits<uintptr_t>::max() ||
        base > std::numeric_limits<uintptr_t>::max() - layout.bytes)
        return {StatusCode::InvalidArgument,"workspace must be aligned, sized, and addressable for capture layout"};
    *out = MakeDeviceView(config,layout,workspace); return {};
}
struct Record { uint64_t timestamp; uint32_t eventId; };
struct SnapshotStream {
    uint32_t id = 0, dropped = 0;
    bool complete = false;
    Completion completion = Completion::EMPTY;
    std::vector<Record> records;
};
struct CaptureSnapshot {
    std::array<uint8_t,16> captureId {};
    uint64_t maxFileBytes = MAX_RECORD_FILE_BYTES;
    std::vector<SnapshotStream> streams;
    uint64_t generation = 0;
};
inline Status BuildSnapshot(const CaptureConfig &config, const std::vector<uint32_t> &counts,
    const std::vector<uint32_t> &dropped, const std::vector<uint32_t> &states,
    const std::vector<uint64_t> &timestamps, const std::vector<uint32_t> &eventIds,
    CaptureSnapshot *out)
{
    Layout l; Status s = BuildLayout(config, &l); if (!s) return s;
    if (!out || counts.size() != l.stateElements || dropped.size() != l.stateElements ||
        states.size() != l.stateElements || timestamps.size() != l.recordElements || eventIds.size() != l.recordElements)
        return {StatusCode::Snapshot, "snapshot buffer lengths do not match layout"};
    CaptureSnapshot result; result.captureId = config.captureId; result.maxFileBytes = config.maxFileBytes;
    result.generation = config.generation;
    for (uint32_t lane = 0; lane < config.logicalStreamCount; ++lane) {
        const size_t stateAt = size_t(lane) * l.stateStride;
        const uint32_t state = states[stateAt];
        if (state > uint32_t(Completion::COMMITTED))
            return {StatusCode::Snapshot, "invalid lane completion state"};
        const uint32_t logical = config.logicalLaneIds.empty() ? lane : config.logicalLaneIds[lane];
        SnapshotStream stream; stream.id = (uint32_t(config.rank) << 16) | logical;
        stream.complete = state == uint32_t(Completion::COMMITTED);
        stream.completion = static_cast<Completion>(state);
        if (stream.complete) {
            const uint32_t count = counts[stateAt];
            if (count > config.recordsPerStream) return {StatusCode::Snapshot, "count exceeds lane capacity"};
            stream.dropped = dropped[stateAt];
            const size_t base = size_t(lane) * l.recordStride;
            for (uint32_t i = 0; i < count; ++i) {
                if (!eventIds[base+i]) return {StatusCode::Snapshot, "event ID zero in committed prefix"};
                stream.records.push_back({timestamps[base+i], eventIds[base+i]});
            }
            std::stable_sort(stream.records.begin(), stream.records.end(),
                [](const Record &a, const Record &b) { return a.timestamp < b.timestamp; });
        }
        result.streams.push_back(std::move(stream));
    }
    *out = std::move(result); return {};
}
namespace Detail {
inline void AppendLE(std::vector<uint8_t> *bytes, uint64_t value, unsigned width)
{ for (unsigned i = 0; i < width; ++i) bytes->push_back(uint8_t(value >> (8*i))); }
}
inline Status EncodeV1(const CaptureSnapshot &snapshot, std::vector<uint8_t> *out)
{
    if (!out || snapshot.streams.size() > 65536 || snapshot.maxFileBytes > MAX_RECORD_FILE_BYTES)
        return {StatusCode::Snapshot, "invalid output/stream count/file budget"};
    uint64_t total = 48;
    std::set<uint32_t> identities;
    for (const auto &stream : snapshot.streams) {
        if (!identities.insert(stream.id).second || stream.records.size() > MAX_RECORDS_PER_STREAM ||
            (!stream.complete && (!stream.records.empty() || stream.dropped)))
            return {StatusCode::Snapshot, "invalid snapshot stream"};
        for (size_t i = 0; i < stream.records.size(); ++i) {
            if (!stream.records[i].eventId || (i && stream.records[i].timestamp < stream.records[i-1].timestamp))
                return {StatusCode::Snapshot, "snapshot records must have nonzero IDs and nondecreasing timestamps"};
        }
        total += 24 + uint64_t(stream.records.size()) * 16;
    }
    if (total > snapshot.maxFileBytes || total > MAX_RECORD_FILE_BYTES || total > std::numeric_limits<size_t>::max())
        return {StatusCode::Snapshot, "encoded capture exceeds byte budget"};
    std::vector<uint8_t> bytes; bytes.reserve(size_t(total));
    const uint8_t magic[8] = {'T','X','R','P','R','F','1',0}; bytes.insert(bytes.end(), magic, magic+8);
    Detail::AppendLE(&bytes,1,2); Detail::AppendLE(&bytes,0,2); Detail::AppendLE(&bytes,48,2);
    Detail::AppendLE(&bytes,16,2); Detail::AppendLE(&bytes,24,2); Detail::AppendLE(&bytes,1,1); Detail::AppendLE(&bytes,0,1);
    Detail::AppendLE(&bytes,snapshot.streams.size(),4); bytes.insert(bytes.end(),snapshot.captureId.begin(),snapshot.captureId.end());
    Detail::AppendLE(&bytes,total,8);
    for (const auto &stream : snapshot.streams) {
        Detail::AppendLE(&bytes,stream.id,4); Detail::AppendLE(&bytes,stream.records.size(),4);
        Detail::AppendLE(&bytes,stream.dropped,4); Detail::AppendLE(&bytes,(stream.complete?1:0)|(stream.dropped?2:0),4);
        Detail::AppendLE(&bytes,24+uint64_t(stream.records.size())*16,8);
        for (const auto &record : stream.records) {
            Detail::AppendLE(&bytes,record.timestamp,8); Detail::AppendLE(&bytes,record.eventId,4); Detail::AppendLE(&bytes,0,4);
        }
    }
    *out = std::move(bytes); return {};
}
}} // namespace TileXR::Profiling
#endif
