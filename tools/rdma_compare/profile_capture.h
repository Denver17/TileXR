/*
 * 功能：在启用 Debug profiling 时检查两 rank 的采集开关，共用 rank 0 的 capture ID，并导出 records 文件。
 * 调用关系：bench/multiqp 在非校准 launch 前调用 Prepare，等待 stream 后调用 Export；采集、下载和释放使用 Profiling::AclCapture。
 * 资源与生命周期：每次 launch 的包装对象持有 capture，stream 由 benchmark 持有；capture 析构调用 Close，失败时可能保留 workspace。
 *
 * Purpose: Agree profiling enablement across two ranks in Debug, share rank zero's capture ID and export record files.
 * Calls: bench/multiqp call Prepare before non-calibration launches and Export after stream completion; Profiling::AclCapture collects/downloads/releases.
 * Lifetime: A per-launch wrapper owns the capture and the benchmark owns the stream; the capture destructor calls Close and may retain workspace on failure.
 */
#ifndef TILEXR_RDMA_BENCH_PROFILE_CAPTURE_H
#define TILEXR_RDMA_BENCH_PROFILE_CAPTURE_H
#include "tilexr_rdma_debug.h"
#include <tilexr/profiling.h>
#include "tools/socket/tilexr_sock_exchange.h"
#include <array>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
class WriteProfileCapture {
public:
    void* Prepare(TileXR::TileXRSockExchange& socket, aclrtStream stream,
        uint32_t rank, uint32_t lanes, bool calibration)
    {
#if TILEXR_RDMA_DEBUG && defined(TILEXR_ENABLE_PROFILING) && TILEXR_ENABLE_PROFILING == 1
        if (calibration) { return nullptr; }
        const char* directory = std::getenv("TILEXR_RDMA_PROFILE_DIR");
        const bool enabled = directory != nullptr && *directory != '\0';
        TileXR::Profiling::CaptureConfig config;
        config.rank = rank; config.logicalStreamCount = lanes; config.recordsPerStream = 4096;
        std::array<uint64_t, 3> local{{uint64_t(enabled), 0, 0}};
        if (rank == 0 && enabled) {
            Check(TileXR::Profiling::Detail::NewCaptureId(&config.captureId));
            std::memcpy(local.data() + 1, config.captureId.data(), 16);
        }
        std::array<std::array<uint64_t, 3>, 2> peers{};
        if (socket.AllGather(local.data(), local.size(), peers[0].data(), 20000) != 0 ||
            peers[0][0] != peers[1][0]) { throw std::runtime_error("profiling rank agreement failed"); }
        if (!enabled) { return nullptr; }
        std::memcpy(config.captureId.data(), peers[0].data() + 1, 16);
        Check(capture_.Initialize(config, stream));
        std::ostringstream name;
        name << directory << "/rdma-write-" << std::hex << std::setfill('0');
        for (auto byte : config.captureId) { name << std::setw(2) << unsigned(byte); }
        name << "-rank" << std::dec << rank << ".records.bin";
        path_ = name.str();
        return capture_.DeviceArgs();
#else
        (void)socket; (void)stream; (void)rank; (void)lanes; (void)calibration;
        return nullptr;
#endif
    }
    void Export()
    {
#if TILEXR_RDMA_DEBUG && defined(TILEXR_ENABLE_PROFILING) && TILEXR_ENABLE_PROFILING == 1
        if (path_.empty()) { return; }
        Check(capture_.Save(path_));
        uint64_t dropped = 0; bool complete = true;
        for (const auto& lane : capture_.Snapshot()->streams) {
            dropped += lane.dropped; complete = complete && lane.complete && lane.dropped == 0;
        }
        std::cout << "PROFILE_RECORDS path=" << path_ << " complete=" << complete
            << " dropped=" << dropped << std::endl;
#endif
    }
private:
#if TILEXR_RDMA_DEBUG && defined(TILEXR_ENABLE_PROFILING) && TILEXR_ENABLE_PROFILING == 1
    static void Check(const TileXR::Profiling::Status& status) {
        if (!status) { throw std::runtime_error("profiling: " + status.message); }
    }
    TileXR::Profiling::AclCapture capture_;
    std::string path_;
#endif
};
#endif
