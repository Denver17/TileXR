/* Copyright (c) 2026 TileXR Project. CANN Open Software License Agreement v2.0. */
// 功能：记录 Host 捕获事件和重试结果，由 AclCapture 写入并供 manifest 导出。
// Purpose: Record Host capture events and retry results written by AclCapture for manifest export.
#ifndef TILEXR_PROFILING_EVENTS_H
#define TILEXR_PROFILING_EVENTS_H
#include "file.h"
#include <sstream>

namespace TileXR { namespace Profiling {
// Host-only observation. It never synchronizes a device or owns an ACL object.
struct HostEvent {
    std::string id, operation, phase, code, message;
    uint32_t retry = 0;
    StatusCode status = StatusCode::Ok;
    int aclError = 0;
    bool resolved = false, warning = false, info = false;
};
inline std::string CaptureIdString(const std::array<uint8_t,16> &id)
{
    const char hex[] = "0123456789abcdef"; std::string result;
    for (size_t i=0; i<id.size(); ++i) {
        if (i==4 || i==6 || i==8 || i==10) result += '-';
        result += hex[id[i] >> 4]; result += hex[id[i] & 15];
    }
    return result;
}
inline std::string HostJsonString(const std::string &value)
{
    const char hex[] = "0123456789abcdef"; std::string result = "\"";
    for (unsigned char c : value) {
        if (c == '"' || c == '\\') { result += '\\'; result += char(c); }
        else if (c < 32) {
            result += "\\u00"; result += hex[c >> 4]; result += hex[c & 15];
        } else result += char(c);
    }
    return result + '"';
}
inline Status ExportCompletionSnapshot(const CaptureSnapshot &snapshot, const std::string &path)
{
    try {
        std::ostringstream out;
        out << "{\"schema\":\"tilexr_profiling_completion.v1\",\"capture_id\":"
            << HostJsonString(CaptureIdString(snapshot.captureId))
            << ",\"generation\":" << snapshot.generation << ",\"streams\":[";
        bool first=true;
        for (const auto &stream:snapshot.streams) {
            if (!first) out << ','; first=false;
            out << "{\"stream_id\":" << stream.id << ",\"completion\":" << uint32_t(stream.completion) << '}';
        }
        const std::string json=out.str()+"]}\n";
        return PublishRecords(path,std::vector<uint8_t>(json.begin(),json.end()));
    } catch (...) { return {StatusCode::Io,"completion sidecar export failed"}; }
}
class HostEvents {
public:
    explicit HostEvents(size_t capacity = 128) : capacity_(std::min(size_t(4096), capacity)) {}
    void Begin(const std::string &attempt) noexcept
    {
        try { attempt_ = attempt.substr(0, 256); capture_.clear(); events_.clear(); dropped_ = 0; sequence_ = 0; }
        catch (...) { Lost(); }
    }
    void BindCapture(const std::array<uint8_t,16> &id) noexcept
    {
        try { capture_ = CaptureIdString(id); } catch (...) { Lost(); }
    }
    const std::string &AttemptId() const { return attempt_; }
    bool HasCapture() const { return !capture_.empty(); }
    const std::vector<HostEvent> &Snapshot() const { return events_; }
    uint32_t DroppedEvents() const { return dropped_; }
    void Lost() noexcept { if (dropped_ != UINT32_MAX) ++dropped_; }
    // Success resolves earlier failures of this operation; history is retained.
    // Recording errors, including allocation failure, never replace SDK errors.
    void Observe(const std::string &operation, const std::string &phase,
                 const Status &status, const std::string &code = "") noexcept
    {
        try {
            uint32_t retry = 0;
            for (auto &event : events_) if (event.operation == operation) {
                if (retry != UINT32_MAX) ++retry;
                if (status) event.resolved = true;
            }
            const bool success = status.code == StatusCode::Ok;
            if (success && operation != "initialize" && operation != "collect" &&
                operation != "export" && operation != "close") return;
            if (events_.size() >= capacity_) { Lost(); return; }
            HostEvent event;
            event.id = attempt_ + ":" + std::to_string(sequence_++);
            event.operation = operation.substr(0,256); event.phase = phase.substr(0,256);
            event.code = code.empty() ? event.phase + (success ? "_succeeded" : "_failed") : code.substr(0,256);
            event.message = status.message.substr(0,4096); event.retry = retry;
            event.status = status.code; event.aclError = status.aclError;
            event.warning = status.code == StatusCode::PublishedCleanupWarning;
            event.info = success; event.resolved = success;
            events_.push_back(std::move(event));
        } catch (...) { Lost(); }
    }
    std::string Json() const
    {
        std::ostringstream out;
        out << "{\"schema\":\"tilexr_profiling_host_events.v1\",\"attempt_id\":" << HostJsonString(attempt_)
            << ",\"capture_id\":" << (capture_.empty() ? "null" : HostJsonString(capture_))
            << ",\"dropped_events\":" << dropped_ << ",\"events\":[";
        bool first = true;
        for (const auto &event : events_) {
            if (!first) out << ','; first = false;
            out << "{\"event_id\":" << HostJsonString(event.id)
                << ",\"code\":" << HostJsonString(event.code)
                << ",\"severity\":\"" << (event.info ? "info" : event.warning ? "warning" : "error")
                << "\",\"phase\":" << HostJsonString(event.phase)
                << ",\"scope\":\"attempt\",\"count\":1,\"source\":\"TileXR::Profiling::HostEvents\""
                << ",\"message\":" << HostJsonString(event.message)
                << ",\"status_code\":" << int(event.status) << ",\"acl_error\":" << event.aclError
                << ",\"operation_id\":" << HostJsonString(event.operation) << ",\"retry\":" << event.retry
                << ",\"resolved\":" << (event.resolved ? "true" : "false") << '}';
        }
        return out.str() + "]}\n";
    }
    Status Export(const std::string &path) const
    {
        try {
            if (attempt_.empty()) return {StatusCode::InvalidState,"Host events require an attempt ID"};
            const auto json = Json();
            return PublishRecords(path, std::vector<uint8_t>(json.begin(),json.end()));
        } catch (...) { return {StatusCode::Io,"Host event export failed"}; }
    }
private:
    size_t capacity_;
    uint64_t sequence_ = 0;
    uint32_t dropped_ = 0;
    std::string attempt_, capture_;
    std::vector<HostEvent> events_;
};
}}
#endif
