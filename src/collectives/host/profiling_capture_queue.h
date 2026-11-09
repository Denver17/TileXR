/* Copyright (c) 2026 TileXR Project. CANN Open Software License Agreement v2.0. */
/* 由 collective launcher 持有有界 AclCapture 队列，在共同清理阶段导出 records、completion 与 manifest 并释放资源；采集失败不替代业务结果。
 * Own a bounded AclCapture queue for collective launchers; export records, completion and manifest at shared cleanup and release resources without replacing operation results.
 */

#ifndef TILEXR_COLLECTIVES_PROFILING_CAPTURE_QUEUE_H
#define TILEXR_COLLECTIVES_PROFILING_CAPTURE_QUEUE_H
#include <tilexr/profiling.h>
#include <cstdlib>
#include <cerrno>
#include <iostream>
#include <memory>
#include <string>
#include <vector>
#if defined(__linux__)
#include <sys/stat.h>
#endif

namespace TileXRCollectives { namespace Host {
// Business-side ownership only: the SDK has no communicator or launch policy.
class ProfilingCaptureQueue {
    struct Entry {
        std::unique_ptr<TileXR::Profiling::AclCapture> capture;
        std::string path;
        aclrtStream stream = nullptr;
        uint32_t lanes = 0;
        bool pending = false;
        bool published = false;
        bool closed = false;
        uint32_t eventRevision = 0;
        bool completionPublished = false;
        bool sidecarsPublished = false;
        std::unique_ptr<TileXR::Profiling::ManifestProducer> producer;
    };
public:
    // Configure from the operator's execution/workload plan before acquisition.
    // The queue cannot infer shape, stage bytes, backend, or expected ranks.
    TileXR::Profiling::Status ConfigureProducer(const TileXR::Profiling::ManifestProducer &producer)
    {
        if (!entries_.empty()) return {TileXR::Profiling::StatusCode::InvalidState,"configure producer before capture preparation"};
        auto status=TileXR::Profiling::ValidateManifestProducer(producer); if(!status)return status;
        producer_.reset(new TileXR::Profiling::ManifestProducer(producer)); return {};
    }
    const TileXR::Profiling::HostEvents &Events() const { return events_; }
    void *Acquire(aclrtStream stream, uint32_t lanes, int rank, int rankSize,
        const TileXR::Profiling::ManifestProducer *producer = nullptr) noexcept
    {
        try { return AcquirePrepared(stream, lanes, rank, rankSize,producer); }
        catch (const std::exception &error) {
            try {
                events_.Observe("acquire", "prepare", {TileXR::Profiling::StatusCode::Io,error.what()});
                std::cerr << "TileXR profiling skipped: " << error.what() << "\n";
            } catch (...) { events_.Lost(); }
            return nullptr;
        }
    }
    void *AcquirePrepared(aclrtStream stream, uint32_t lanes, int rank, int rankSize,
        const TileXR::Profiling::ManifestProducer *producer = nullptr)
    {
#if defined(TILEXR_ENABLE_PROFILING) && TILEXR_ENABLE_PROFILING == 1
        const char *directory = std::getenv("TILEXR_PROFILING_OUTPUT_DIR");
        if (!directory || !*directory) return nullptr;
        if(!producer) producer=producer_.get();
        if(producer) {
            const auto status=TileXR::Profiling::ValidateManifestProducer(*producer);
            if(!status) {
                events_.Observe("producer","prepare",status,"capture_skipped");
                return nullptr;
            }
        }
        if (events_.AttemptId().empty()) {
            std::array<uint8_t,16> attempt {};
            if (TileXR::Profiling::Detail::NewCaptureId(&attempt))
                events_.Begin(TileXR::Profiling::CaptureIdString(attempt));
            else events_.Lost();
            eventDirectory_ = directory;
            eventRank_ = rank;
        }
        bool prepared = false;
        for (const auto &entry : entries_) {
            if (entry.stream == stream && entry.lanes == lanes) prepared = true;
        }
        if (!prepared && entries_.size() < kCaptureBudget) Prepare(stream, lanes, rank, rankSize, directory);
        for (auto &entry : entries_) {
            if (entry.stream == stream && entry.lanes == lanes && !entry.pending && !entry.closed) {
                if(producer) {
                    entry.producer.reset(new TileXR::Profiling::ManifestProducer(*producer));
                    entry.producer->attemptId=entry.capture->Events().AttemptId();
                }
                entry.pending = true;
                return entry.capture->DeviceArgs();
            }
        }
        ++skipped_;
        events_.Observe("acquire:"+std::to_string(skipped_), "sampling",
            {TileXR::Profiling::StatusCode::InvalidState,"no prepared capture for requested stream/lane shape"}, "capture_skipped");
        std::cerr << "TileXR profiling skipped: no prepared capture (skipped=" << skipped_ << ")\n";
#else
        (void)stream; (void)lanes; (void)rank; (void)rankSize; (void)producer;
#endif
        return nullptr;
    }

    // Only call at a business completion/cleanup point. No per-entry rank agreement.
    bool Drain()
    {
        bool success = true;
        for (auto &entry : entries_) {
            if (entry.closed) {
                if(!entry.sidecarsPublished) { entry.sidecarsPublished=PersistEvents(entry); success=entry.sidecarsPublished && success; }
                continue;
            }
            if (entry.pending && !entry.published) {
                auto status = entry.capture->Collect();
                if (status && MakeParents(entry.path)) status = entry.capture->Export(entry.path);
                else if (status) status = {TileXR::Profiling::StatusCode::Io, "cannot create capture directory"};
                if (!status) {
                    entry.capture->Events().Observe("queue_export", "export",status);
                    PersistEvents(entry);
                    std::cerr << "TileXR profiling deferred export: " << status.message << "\n";
                    success = false;
                    continue;
                }
                entry.published = true;
                entry.capture->Events().Observe("queue_export","export",{});
                if (status.code == TileXR::Profiling::StatusCode::PublishedCleanupWarning)
                    std::cerr << "TileXR profiling published with cleanup warning: " << status.message << "\n";
            }
            if (entry.published && !entry.completionPublished) {
                const auto status=entry.capture->ExportCompletion(entry.path+".completion.json");
                if(status) {
                    entry.capture->Events().Observe("completion_export","export",{});
                    entry.completionPublished=true;
                }
                else {
                    success=false; entry.capture->Events().Observe("completion_export","export",status);
                    PersistEvents(entry); continue;
                }
            }
            const auto status = entry.capture->Close();
            if (status) entry.closed = true;
            else { success = false; std::cerr << "TileXR profiling deferred close: " << status.message << "\n"; }
            entry.sidecarsPublished=PersistEvents(entry);
            success=entry.sidecarsPublished && success;
        }
        if (!eventDirectory_.empty()) {
            const auto path = eventDirectory_+"/attempt-"+events_.AttemptId()+"-rank-"+
                std::to_string(eventRank_)+"-events-"+std::to_string(eventRevision_++)+".json";
            if (MakeParents(path)) {
                const auto status = events_.Export(path);
                success=status.ok() && success;
                if (!status) std::cerr << "TileXR profiling event export: " << status.message << "\n";
            } else success=false;
        }
        return success;
    }
private:
    bool PersistEvents(Entry &entry)
    {
        const auto revision=std::to_string(entry.eventRevision++);
        const auto path = entry.path+".events-"+revision+".json";
        bool success=MakeParents(path);
        if (success) {
            const auto status = entry.capture->Events().Export(path);
            success=status.ok();
            if (!status) std::cerr << "TileXR profiling event export: " << status.message << "\n";
        }
        if(entry.producer && entry.capture->Snapshot()) {
            const auto status=TileXR::Profiling::ExportManifestShard(*entry.capture->Snapshot(),entry.capture->Config(),
                entry.capture->Events(),*entry.producer,entry.path+".manifest-"+revision+".json");
            success=status.ok() && success;
            if(!status) std::cerr << "TileXR profiling manifest export: " << status.message << "\n";
        }
        return success;
    }
    static constexpr size_t kCaptureBudget = 4;
    static constexpr uint64_t kByteBudget = UINT64_C(64) * 1024 * 1024;
    static bool ParseId(const char *text, std::array<uint8_t,16> &id)
    {
        if (!text) return false;
        const std::string value(text);
        if (value.size()!=36) return false;
        size_t out=0;
        for (size_t i=0;i<value.size();) {
            if (i==8 || i==13 || i==18 || i==23) { if(value[i++]!='-')return false; continue; }
            auto nibble=[](char c)->int { return c>='0'&&c<='9'?c-'0':c>='a'&&c<='f'?c-'a'+10:c>='A'&&c<='F'?c-'A'+10:-1; };
            const int hi=nibble(value[i++]),lo=nibble(value[i++]);
            if(hi<0||lo<0||out>=id.size())return false;
            id[out++]=uint8_t((hi<<4)|lo);
        }
        return out==id.size() && std::any_of(id.begin(),id.end(),[](uint8_t b){return b!=0;});
    }
    static std::string IdString(const std::array<uint8_t,16> &id)
    {
        const char hex[]="0123456789abcdef"; std::string result;
        for(size_t i=0;i<id.size();++i) { if(i==4||i==6||i==8||i==10)result+='-';result+=hex[id[i]>>4];result+=hex[id[i]&15]; }
        return result;
    }
    static bool MakeParents(const std::string &path)
    {
#if defined(__linux__)
        for(size_t i=1;i<path.size();++i) if(path[i]=='/') {
            const std::string parent=path.substr(0,i);
            if(::mkdir(parent.c_str(),0700)!=0 && errno!=EEXIST)return false;
        }
        return true;
#else
        (void)path; return false;
#endif
    }
    void Prepare(aclrtStream stream,uint32_t lanes,int rank,int rankSize,const char *directory)
    {
        TileXR::Profiling::CaptureConfig config;
        config.rank=rank; config.logicalStreamCount=lanes;
        const char *captureId=std::getenv("TILEXR_PROFILING_CAPTURE_ID");
        if ((captureId && !ParseId(captureId,config.captureId)) || (!captureId && rankSize>1)) {
            events_.Observe("prepare","prepare",{TileXR::Profiling::StatusCode::InvalidArgument,
                "distributed capture requires a shared UUID"});
            std::cerr << "TileXR profiling skipped: distributed capture requires a shared UUID\n"; return;
        }
        const auto seed=config.captureId;
        // Prepare device storage once. Later launches can copy Host producer
        // metadata but perform no device allocation, sync, D2H, export or free.
        while(entries_.size()<kCaptureBudget) {
            config.captureId=seed;
            if(captureId) {
                config.captureId[15]^=uint8_t(entries_.size()+1);
                config.captureId[6]=uint8_t((config.captureId[6]&15)|64);
                config.captureId[8]=uint8_t((config.captureId[8]&63)|128);
            }
            TileXR::Profiling::Layout layout;
            const auto layoutStatus = TileXR::Profiling::BuildLayout(config,&layout);
            if(!layoutStatus || layout.bytes>kByteBudget-bytes_) {
                events_.Observe("prepare","prepare",layoutStatus ?
                    TileXR::Profiling::Status(TileXR::Profiling::StatusCode::InvalidArgument,"capture byte budget exhausted") : layoutStatus);
                break;
            }
            Entry entry; entry.capture.reset(new TileXR::Profiling::AclCapture());
            if(std::all_of(config.captureId.begin(),config.captureId.end(),[](uint8_t b){return b==0;})) {
                const auto idStatus=TileXR::Profiling::Detail::NewCaptureId(&config.captureId);
                if(!idStatus) { events_.Observe("prepare","initialize",idStatus); break; }
            }
            // Capture UUID is planned from the shared seed before launch, so
            // every rank uses the same attempt binding for this measurement.
            entry.capture->Events().Begin(TileXR::Profiling::CaptureIdString(config.captureId));
            const auto status=entry.capture->Initialize(config,stream);
            if(!status) {
                events_.Observe("prepare","initialize",status);
                std::cerr << "TileXR profiling prepare: " << status.message << "\n";break;
            }
            events_.Observe("prepare","prepare",{});
            entry.stream=stream;entry.lanes=lanes;
            entry.path=std::string(directory)+"/"+IdString(entry.capture->Config().captureId)+"/rank-"+std::to_string(rank)+".records.bin";
            bytes_+=layout.bytes;entries_.push_back(std::move(entry));
        }
    }
    uint64_t bytes_=0,skipped_=0;
    std::vector<Entry> entries_;
    TileXR::Profiling::HostEvents events_;
    std::string eventDirectory_;
    int eventRank_ = 0;
    uint32_t eventRevision_ = 0;
    std::unique_ptr<TileXR::Profiling::ManifestProducer> producer_;
};
} }
#endif
