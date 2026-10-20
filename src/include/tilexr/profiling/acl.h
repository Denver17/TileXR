/* Copyright (c) 2026 TileXR Project. CANN Open Software License Agreement v2.0. */
#ifndef TILEXR_PROFILING_ACL_H
#define TILEXR_PROFILING_ACL_H
#include "file.h"
#include <acl/acl.h>
#include <cstdio>
#include <memory>
#include <random>
#include <utility>
namespace TileXR { namespace Profiling {
namespace Detail {
inline Status AclStatus(aclError error, const char *operation)
{ return error == ACL_SUCCESS ? Status() : Status(StatusCode::Acl,operation,int(error)); }
template<class Random = std::random_device>
inline Status NewCaptureId(std::array<uint8_t,16> *id)
{
    try {
        Random random;
        // entropy() may return zero when the implementation cannot estimate it
        // (including libc++'s Windows rand_s backend). Generation errors throw.
        for (size_t i = 0; i < id->size(); i += 4) {
            const uint32_t bits = random();
            for (size_t j = 0; j < 4; ++j) (*id)[i+j] = uint8_t(bits >> (j*8));
        }
        (*id)[6] = uint8_t(((*id)[6]&15)|64); (*id)[8] = uint8_t(((*id)[8]&63)|128);
        return {};
    } catch (...) { return {StatusCode::Io,"capture UUID generation failed"}; }
}
}
class AclCapture {
public:
    AclCapture() = default;
    AclCapture(const AclCapture &) = delete;
    AclCapture &operator=(const AclCapture &) = delete;
    AclCapture &operator=(AclCapture &&) = delete;
    AclCapture(AclCapture &&other) noexcept
        : config_(other.config_), layout_(other.layout_), stream_(other.stream_), context_(other.context_),
          workspace_(other.workspace_), initialized_(other.initialized_), sealed_(other.sealed_),
          synchronized_(other.synchronized_), snapshot_(std::move(other.snapshot_))
    { other.workspace_ = nullptr; other.initialized_ = false; other.sealed_ = true; }
    ~AclCapture() noexcept
    {
        try { Status s = Close(); if (!s) std::fprintf(stderr,"TileXR profiling cleanup: %s (ACL %d); workspace retained if unsafe\n",s.message.c_str(),s.aclError); }
        catch (...) { std::fputs("TileXR profiling cleanup failed; workspace retained\n",stderr); }
    }
    Status Initialize(aclrtStream stream, uint32_t logicalStreamCount)
    { CaptureConfig c; c.logicalStreamCount = logicalStreamCount; return Initialize(c,stream); }
    Status Initialize(const CaptureConfig &config, aclrtStream stream)
    {
        if (initialized_ || workspace_) return {StatusCode::InvalidState,"Close capture before Initialize"};
        Layout layout; Status status = BuildLayout(config,&layout); if (!status) return status;
        config_ = config; layout_ = layout; stream_ = stream; sealed_ = false; synchronized_ = false;
        snapshot_.reset();
        if (!config.enabled) { initialized_ = true; return {}; }
        if (std::all_of(config_.captureId.begin(),config_.captureId.end(),[](uint8_t b){return b==0;})) {
            status = Detail::NewCaptureId(&config_.captureId); if (!status) return status;
        }
        status = Detail::AclStatus(aclrtGetCurrentContext(&context_),"aclrtGetCurrentContext"); if (!status) return status;
        if (!context_) return {StatusCode::InvalidState,"capture requires a current ACL context"};
        status = Detail::AclStatus(aclrtMalloc(&workspace_,size_t(layout_.bytes),ACL_MEM_MALLOC_HUGE_FIRST),"aclrtMalloc");
        if (!status) { workspace_ = nullptr; return status; }
        initialized_ = true;
        status = Detail::AclStatus(aclrtMemset(workspace_,size_t(layout_.bytes),0,size_t(layout_.bytes)),"aclrtMemset");
        if (status) {
            DeviceView view {};
            status = MakeDeviceView(config_,workspace_,layout_.bytes,&view);
            if (status) status = Detail::AclStatus(aclrtMemcpy(workspace_,size_t(layout_.bytes),&view,sizeof(view),ACL_MEMCPY_HOST_TO_DEVICE),"descriptor H2D");
        }
        if (!status) {
            // No work was exposed to the caller. Cleanup needs no stream wait.
            sealed_ = true; synchronized_ = true;
            const aclError freeError = aclrtFree(workspace_);
            if (freeError == ACL_SUCCESS) { workspace_ = nullptr; initialized_ = false; }
            else status.message += "; partial initialization free failed; call Close to retry";
        }
        return status;
    }
    void *DeviceArgs() const { return initialized_ && !sealed_ && config_.enabled ? workspace_ : nullptr; }
    const CaptureSnapshot *Snapshot() const { return snapshot_.get(); }
    const CaptureConfig &Config() const { return config_; }
    uint64_t WorkspaceBytes() const { return workspace_ ? layout_.bytes : 0; }
    Status Collect()
    {
        if (snapshot_) return {};
        if (!config_.enabled) return {};
        if (!initialized_) return {StatusCode::InvalidState,"capture is not initialized or was closed without collection"};
        sealed_ = true;
        Status status = CheckContext(); if (!status) return status;
        if (!synchronized_) { status = Detail::AclStatus(aclrtSynchronizeStream(stream_),"aclrtSynchronizeStream"); if (!status) return status; synchronized_ = true; }
        std::vector<uint32_t> counts(size_t(layout_.stateElements)), dropped(counts.size()), states(counts.size());
        status = Download(&states,layout_.statesOffset); if (!status) return status;
        status = Download(&counts,layout_.countsOffset); if (!status) return status;
        status = Download(&dropped,layout_.droppedOffset); if (!status) return status;
        std::vector<uint64_t> timestamps(size_t(layout_.recordElements));
        std::vector<uint32_t> eventIds(size_t(layout_.recordElements));
        // Incomplete lanes' count/payload are deliberately never interpreted.
        // Only committed valid prefixes are downloaded, including non-aligned counts.
        for (uint32_t lane = 0; lane < config_.logicalStreamCount; ++lane) {
            const uint32_t state = states[size_t(lane)*layout_.stateStride];
            if (state > uint32_t(Completion::COMMITTED)) return {StatusCode::Snapshot,"invalid lane completion state"};
            if (state != uint32_t(Completion::COMMITTED)) continue;
            const uint32_t count = counts[size_t(lane)*layout_.stateStride];
            if (count > config_.recordsPerStream) return {StatusCode::Snapshot,"count exceeds lane capacity"};
            const size_t at = size_t(lane)*layout_.recordStride;
            if (count) {
                status = CopyFromDevice(timestamps.data()+at,size_t(count)*8,layout_.timestampsOffset+at*8); if (!status) return status;
                status = CopyFromDevice(eventIds.data()+at,size_t(count)*4,layout_.eventIdsOffset+at*4); if (!status) return status;
            }
        }
        std::unique_ptr<CaptureSnapshot> result(new CaptureSnapshot);
        status = BuildSnapshot(config_,counts,dropped,states,timestamps,eventIds,result.get());
        if (status) snapshot_ = std::move(result);
        return status;
    }
    Status Export(const std::string &path) const
    {
        if (!config_.enabled) return {};
        if (!snapshot_) return {StatusCode::InvalidState,"Collect must succeed before Export"};
        return ExportSnapshot(*snapshot_,path);
    }
    Status Save(const std::string &path) { Status s = Collect(); return s ? Export(path) : s; }
    Status Close()
    {
        sealed_ = true;
        if (!workspace_) { initialized_ = false; return {}; }
        Status s = CheckContext(); if (!s) return s;
        if (!synchronized_) { s = Detail::AclStatus(aclrtSynchronizeStream(stream_),"aclrtSynchronizeStream before free"); if (!s) return s; synchronized_ = true; }
        s = Detail::AclStatus(aclrtFree(workspace_),"aclrtFree");
        if (s) { workspace_ = nullptr; initialized_ = false; } return s;
    }
private:
    Status CheckContext() const
    {
        aclrtContext current = nullptr; Status s = Detail::AclStatus(aclrtGetCurrentContext(&current),"aclrtGetCurrentContext");
        if (!s) return s;
        return current == context_ ? Status() : Status(StatusCode::InvalidState,"current ACL context differs from capture owner");
    }
    Status CopyFromDevice(void *output, size_t bytes, uint64_t offset)
    { return Detail::AclStatus(aclrtMemcpy(output,bytes,static_cast<const uint8_t *>(workspace_)+offset,bytes,ACL_MEMCPY_DEVICE_TO_HOST),"snapshot D2H"); }
    template<class T> Status Download(std::vector<T> *out, uint64_t offset)
    { return CopyFromDevice(out->data(),out->size()*sizeof(T),offset); }
    CaptureConfig config_; Layout layout_; aclrtStream stream_ = nullptr; aclrtContext context_ = nullptr;
    void *workspace_ = nullptr; bool initialized_ = false, sealed_ = true, synchronized_ = false;
    std::unique_ptr<CaptureSnapshot> snapshot_;
};
}}
#endif
