/* Copyright (c) 2026 TileXR Project. CANN Open Software License Agreement v2.0. */
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
    };
public:
    void *Acquire(aclrtStream stream, uint32_t lanes, int rank, int rankSize) noexcept
    {
        try { return AcquirePrepared(stream, lanes, rank, rankSize); }
        catch (const std::exception &error) {
            std::cerr << "TileXR profiling skipped: " << error.what() << "\n";
            return nullptr;
        }
    }
    void *AcquirePrepared(aclrtStream stream, uint32_t lanes, int rank, int rankSize)
    {
#if defined(TILEXR_ENABLE_PROFILING) && TILEXR_ENABLE_PROFILING == 1
        const char *directory = std::getenv("TILEXR_PROFILING_OUTPUT_DIR");
        if (!directory || !*directory) return nullptr;
        bool prepared = false;
        for (const auto &entry : entries_) {
            if (entry.stream == stream && entry.lanes == lanes) prepared = true;
        }
        if (!prepared && entries_.size() < kCaptureBudget) Prepare(stream, lanes, rank, rankSize, directory);
        for (auto &entry : entries_) {
            if (entry.stream == stream && entry.lanes == lanes && !entry.pending && !entry.closed) {
                entry.pending = true;
                return entry.capture->DeviceArgs();
            }
        }
        ++skipped_;
        std::cerr << "TileXR profiling skipped: no prepared capture (skipped=" << skipped_ << ")\n";
#else
        (void)stream; (void)lanes; (void)rank; (void)rankSize;
#endif
        return nullptr;
    }

    // Only call at a business completion/cleanup point. No per-entry rank agreement.
    bool Drain()
    {
        bool success = true;
        for (auto &entry : entries_) {
            if (entry.closed) continue;
            if (entry.pending && !entry.published) {
                auto status = entry.capture->Collect();
                if (status && MakeParents(entry.path)) status = entry.capture->Export(entry.path);
                else if (status) status = {TileXR::Profiling::StatusCode::Io, "cannot create capture directory"};
                if (!status) {
                    std::cerr << "TileXR profiling deferred export: " << status.message << "\n";
                    success = false;
                    continue;
                }
                entry.published = true;
                if (status.code == TileXR::Profiling::StatusCode::PublishedCleanupWarning)
                    std::cerr << "TileXR profiling published with cleanup warning: " << status.message << "\n";
            }
            const auto status = entry.capture->Close();
            if (status) entry.closed = true;
            else { success = false; std::cerr << "TileXR profiling deferred close: " << status.message << "\n"; }
        }
        return success;
    }
private:
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
            std::cerr << "TileXR profiling skipped: distributed capture requires a shared UUID\n"; return;
        }
        const auto seed=config.captureId;
        // One initialization phase prepares the bounded pool; subsequent launch
        // acquisition performs no allocation, synchronization, export, or free.
        while(entries_.size()<kCaptureBudget) {
            config.captureId=seed;
            if(captureId) {
                config.captureId[15]^=uint8_t(entries_.size()+1);
                config.captureId[6]=uint8_t((config.captureId[6]&15)|64);
                config.captureId[8]=uint8_t((config.captureId[8]&63)|128);
            }
            TileXR::Profiling::Layout layout;
            if(!TileXR::Profiling::BuildLayout(config,&layout) || layout.bytes>kByteBudget-bytes_)break;
            Entry entry; entry.capture.reset(new TileXR::Profiling::AclCapture());
            const auto status=entry.capture->Initialize(config,stream);
            if(!status) { std::cerr << "TileXR profiling prepare: " << status.message << "\n";break; }
            entry.stream=stream;entry.lanes=lanes;
            entry.path=std::string(directory)+"/"+IdString(entry.capture->Config().captureId)+"/rank-"+std::to_string(rank)+".records.bin";
            bytes_+=layout.bytes;entries_.push_back(std::move(entry));
        }
    }
    uint64_t bytes_=0,skipped_=0;
    std::vector<Entry> entries_;
};
} }
#endif
