/* Copyright (c) 2026 TileXR Project. CANN Open Software License Agreement v2.0. */
// 功能：AclCapture 管理自有/借用 workspace 的同步、快照和导出，并将重试写入 HostEvents。
// Purpose: AclCapture manages owned/borrowed workspace synchronization, snapshots and export, recording retries in HostEvents.
#ifndef TILEXR_PROFILING_ACL_H
#define TILEXR_PROFILING_ACL_H
#include "events.h"
#include "workspace.h"
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
    // The optional Host-only sink must outlive this capture, including Close in
    // its destructor. It does not own or change any ACL resource lifetime.
    explicit AclCapture(HostEvents &events) noexcept : eventsSink_(&events) {}
    AclCapture(const AclCapture &) = delete;
    AclCapture &operator=(const AclCapture &) = delete;
    AclCapture &operator=(AclCapture &&) = delete;
    AclCapture(AclCapture &&other) noexcept
        : config_(std::move(other.config_)), layout_(other.layout_), stream_(other.stream_), context_(other.context_),
          workspace_(other.workspace_), initialized_(other.initialized_), sealed_(other.sealed_),
          synchronized_(other.synchronized_), snapshot_(std::move(other.snapshot_)), events_(std::move(other.events_)),
          eventsSink_(other.eventsSink_), lease_(std::move(other.lease_))
    { other.workspace_ = nullptr; other.initialized_ = false; other.sealed_ = true; other.eventsSink_ = nullptr; }
    ~AclCapture() noexcept
    {
        try { Status s = Close(); if (!s) std::fprintf(stderr,"TileXR profiling cleanup: %s (ACL %d); workspace retained if unsafe\n",s.message.c_str(),s.aclError); }
        catch (...) { std::fputs("TileXR profiling cleanup failed; workspace retained\n",stderr); }
    }
    Status Initialize(aclrtStream stream, uint32_t logicalStreamCount)
    { CaptureConfig c; c.logicalStreamCount = logicalStreamCount; return Initialize(c,stream); }
    HostEvents &Events() { return EventSink(); }
    const HostEvents &Events() const { return EventSink(); }
    Status Initialize(const CaptureConfig &config, aclrtStream stream)
    {
        if (EventSink().AttemptId().empty() || (!initialized_ && !workspace_ && EventSink().HasCapture())) {
            try {
                std::array<uint8_t,16> attempt {};
                if (Detail::NewCaptureId(&attempt)) EventSink().Begin(CaptureIdString(attempt));
                else EventSink().Lost();
            } catch (...) { EventSink().Lost(); }
        }
        Status status = InitializeImpl(config,stream);
        if (initialized_ && config_.enabled) EventSink().BindCapture(config_.captureId);
        ObserveStatus("initialize","initialize",status);
        return status;
    }
    // Transfer a reservation, never allocation ownership. A failed Initialize
    // keeps the lease in this capture until Close safely returns it.
    Status InitializeBorrowed(const CaptureConfig &config, aclrtStream stream, WorkspaceLease &&lease)
    {
        if (initialized_ || workspace_ || lease_ || !lease)
            return {StatusCode::InvalidState,"capture must be closed and lease valid"};
        Layout layout; Status status=BuildLayout(config,&layout); if(!status) return status;
        if (!config.enabled || lease.Bytes()<layout.bytes)
            return {StatusCode::InvalidArgument,"borrowed capture must be enabled and fit workspace"};
        aclrtContext context=nullptr;
        status=Detail::AclStatus(aclrtGetCurrentContext(&context),"aclrtGetCurrentContext");
        if(!status) return status;
        if(reinterpret_cast<uintptr_t>(context)!=lease.Context())
            return {StatusCode::InvalidState,"workspace context differs from current ACL context"};
        lease_=std::move(lease);
        CaptureConfig borrowed=config; borrowed.generation=lease_.Generation();
        return Initialize(borrowed,stream);
    }
    Status Collect()
    {
        Status status = CollectImpl();
        const char *phase = status.code == StatusCode::Snapshot ? "snapshot" :
            status.message.find("D2H") != std::string::npos ? "d2h" :
            status.message.find("Synchronize") != std::string::npos ? "synchronize" : "collect";
        ObserveStatus("collect",phase,status); return status;
    }
    Status Export(const std::string &path) const
    {
        Status status = ExportImpl(path); ObserveStatus("export","export",status); return status;
    }
    Status ExportCompletion(const std::string &path) const
    {
        if (!snapshot_) return {StatusCode::InvalidState,"Collect must succeed before completion export"};
        return ExportCompletionSnapshot(*snapshot_,path);
    }
    Status Close()
    {
        Status status = CloseImpl(); ObserveStatus("close","close",status); return status;
    }
private:
    HostEvents &EventSink() const { return eventsSink_ ? *eventsSink_ : events_; }
    void ObserveStatus(const char *operation, const char *phase, const Status &status) const noexcept
    {
        try { EventSink().Observe(operation,phase,status); }
        catch (...) { EventSink().Lost(); }
    }
    Status InitializeImpl(const CaptureConfig &config, aclrtStream stream)
    {
        if (initialized_ || workspace_) return {StatusCode::InvalidState,"Close capture before Initialize"};
        Layout layout; Status status = BuildLayout(config,&layout); if (!status) return status;
        if (lease_ && (!config.enabled || lease_.Bytes()<layout.bytes))
            return {StatusCode::InvalidArgument,"borrowed capture must be enabled and fit workspace"};
        config_ = config; layout_ = layout; stream_ = stream; sealed_ = false; synchronized_ = false;
        if(lease_) config_.generation=lease_.Generation();
        snapshot_.reset();
        if (!config.enabled) { initialized_ = true; return {}; }
        if (std::all_of(config_.captureId.begin(),config_.captureId.end(),[](uint8_t b){return b==0;})) {
            status = Detail::NewCaptureId(&config_.captureId); if (!status) return status;
        }
        status = Detail::AclStatus(aclrtGetCurrentContext(&context_),"aclrtGetCurrentContext"); if (!status) return status;
        if (!context_) return {StatusCode::InvalidState,"capture requires a current ACL context"};
        if (lease_ && reinterpret_cast<uintptr_t>(context_)!=lease_.Context())
            return {StatusCode::InvalidState,"workspace context differs from current ACL context"};
        if (lease_) workspace_=lease_.Address();
        else status = Detail::AclStatus(aclrtMalloc(&workspace_,size_t(layout_.bytes),ACL_MEM_MALLOC_HUGE_FIRST),"aclrtMalloc");
        if (!status) { workspace_ = nullptr; return status; }
        initialized_ = true;
        status = Detail::AclStatus(aclrtMemset(workspace_,size_t(layout_.bytes),0,size_t(layout_.bytes)),"aclrtMemset");
        if (status) {
            DeviceView view {};
            status = MakeDeviceView(config_,workspace_,layout_.bytes,&view);
            if (status) status = Detail::AclStatus(aclrtMemcpy(workspace_,size_t(layout_.bytes),&view,sizeof(view),ACL_MEMCPY_HOST_TO_DEVICE),"descriptor H2D");
            if (status && !config_.logicalLaneIds.empty())
                status = Detail::AclStatus(aclrtMemcpy(static_cast<uint8_t *>(workspace_)+layout_.logicalLaneIdsOffset,
                    config_.logicalLaneIds.size()*sizeof(uint32_t),config_.logicalLaneIds.data(),
                    config_.logicalLaneIds.size()*sizeof(uint32_t),ACL_MEMCPY_HOST_TO_DEVICE),"sparse lanes H2D");
        }
        if (!status) {
            // No work was exposed to the caller. Cleanup needs no stream wait.
            sealed_ = true; synchronized_ = true;
            const aclError freeError = lease_ ? ACL_SUCCESS : aclrtFree(workspace_);
            if (freeError == ACL_SUCCESS) { workspace_ = nullptr; initialized_ = false; }
            else status.message += "; partial initialization free failed; call Close to retry";
        }
        return status;
    }
public:
    void *DeviceArgs() const
    {
        if (!initialized_ || sealed_ || !config_.enabled) return nullptr;
        if (lease_) lease_.exposed_=true;
        return workspace_;
    }
    const CaptureSnapshot *Snapshot() const { return snapshot_.get(); }
    const CaptureConfig &Config() const { return config_; }
    uint64_t WorkspaceBytes() const { return workspace_ ? layout_.bytes : 0; }
private:
    Status CollectImpl()
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
    Status ExportImpl(const std::string &path) const
    {
        if (!config_.enabled) return {};
        if (!snapshot_) return {StatusCode::InvalidState,"Collect must succeed before Export"};
        return ExportSnapshot(*snapshot_,path);
    }
public:
    Status Save(const std::string &path) { Status s = Collect(); return s ? Export(path) : s; }
private:
    Status CloseImpl()
    {
        sealed_ = true;
        if (!workspace_) { initialized_ = false; lease_.exposed_=false; lease_.Reset(); return {}; }
        Status s = CheckContext(); if (!s) return s;
        if (!synchronized_) { s = Detail::AclStatus(aclrtSynchronizeStream(stream_),"aclrtSynchronizeStream before free"); if (!s) return s; synchronized_ = true; }
        s = lease_ ? Status() : Detail::AclStatus(aclrtFree(workspace_),"aclrtFree");
        if (s) {
            workspace_ = nullptr; initialized_ = false;
            lease_.exposed_=false; lease_.Reset();
        } return s;
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
    mutable HostEvents events_;
    HostEvents *eventsSink_ = nullptr;
    mutable WorkspaceLease lease_;
};
}}
#endif
