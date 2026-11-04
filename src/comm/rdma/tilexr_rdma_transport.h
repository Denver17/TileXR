/* Copyright (c) 2026 TileXR Project. Licensed under the Apache License, Version 2.0. */

/*
 * 功能：声明 RDMA 配置、provider 接口和 transport 的 QP/region 操作及状态。
 * 调用关系：Host API 使用 TileXRRDMATransport；verbs 实现 RdmaProvider，调用方注入 allGather 回调。
 * 资源与生命周期：transport 持有 provider 和 region 注册记录；payload buffer 仍由调用方持有。
 *
 * Purpose: Declare RDMA configuration, provider interfaces and transport QP/region operations and state.
 * Calls: Host APIs use TileXRRDMATransport; verbs implements RdmaProvider and callers supply allGather.
 * Lifetime: The transport owns its provider and registration records; payload buffers remain caller-owned.
 */
#ifndef TILEXR_RDMA_TRANSPORT_H
#define TILEXR_RDMA_TRANSPORT_H

#include <array>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include "tilexr_rdma.h"
#include "tilexr_types.h"

namespace TileXR {

struct RdmaConfig {
    std::string device;
    uint32_t port = 1;
    int gidIndex = -1; // Discover an active, nonzero RoCEv2 GID.
    uint32_t mtu = 4096;
    uint32_t trafficClass = 0;
    uint32_t hopLimit = 64;
    uint32_t timeoutMs = 5000;
    uint32_t deviceId = 0; // Local ACL logical device, used by UBNIC segment registration.
    bool indepUb = false;
    bool aivDirect = false;
    uint32_t doorbellMode = 0; // AIV mode requires explicit UBNIC soft(1)/hard(2) ABI.
};
int LoadRdmaConfig(RdmaConfig& config, std::string& error);

struct RdmaEndpoint {
    std::array<uint8_t, 16> gid {};
    uint32_t qpn = 0;
    uint32_t psn = 0;
    uint32_t mtu = 0; // Bytes.
};
struct RdmaMemory {
    uint64_t address = 0;
    uint64_t bytes = 0;
    uint32_t lkey = 0;
    uint32_t rkey = 0;
    void* resource = nullptr;
    bool segmentRegistered = false;
};

// Provider owns context/PD and one QP/CQ per peer. QPs must be destroyed
// collectively before any MR is released. Nonzero release keeps ownership.
class RdmaProvider {
public:
    virtual ~RdmaProvider() = default;
    virtual int Open(const RdmaConfig& config) = 0;
    virtual int Create(int peer, RdmaEndpoint& endpoint) = 0;
    virtual int Connect(int peer, const RdmaEndpoint& endpoint) = 0;
    virtual int Register(void* ptr, size_t bytes, RdmaMemory& memory) = 0;
    virtual int Deregister(RdmaMemory& memory) = 0;
    virtual int Transfer(bool read, int peer, const RdmaMemory& local,
        uint64_t localOffset, const RdmaMemory& remote, uint64_t remoteOffset,
        uint32_t bytes, uint32_t timeoutMs) = 0;
    virtual int DestroyQueues() = 0;
    virtual int Close() = 0;
    virtual bool DeviceDirect() const { return false; }
    virtual int DeviceQueue(int, TileXRRDMADeviceQueue&) { return TILEXR_ERROR_NOT_SUPPORT; }
};
std::unique_ptr<RdmaProvider> MakeRdmaVerbsProvider();

struct RdmaOptions {
    int rank = 0;
    int rankSize = 0;
    RdmaConfig config;
    // Fixed send bytes per rank. All callers must use the same collective order.
    std::function<int(const void*, size_t, void*)> allGather;
};

class TileXRRDMATransport {
public:
    explicit TileXRRDMATransport(std::unique_ptr<RdmaProvider> provider);
    ~TileXRRDMATransport();
    TileXRRDMATransport(const TileXRRDMATransport&) = delete;
    TileXRRDMATransport& operator=(const TileXRRDMATransport&) = delete;
    int Init(const RdmaOptions& options);
    TileXRRDMAInfo Query() const;
    int ConfigureQps(uint32_t qpCount);
    int Register(void* ptr, size_t bytes, TileXRRDMAMemHandle* handle);
    int Unregister(TileXRRDMAMemHandle handle);
    int Transfer(bool read, TileXRRDMAMemHandle handle, int peer,
        size_t localOffset, size_t remoteOffset, size_t bytes);
    int DeviceView(TileXRRDMAMemHandle handle, int peer, TileXRRDMADeviceView* view);
    int DeviceViews(TileXRRDMAMemHandle handle, int peer, TileXRRDMADeviceView* views, uint32_t qpCount);
    int Shutdown();
private:
    struct Region {
        RdmaMemory local;
        std::vector<RdmaMemory> remote;
    };
    int Agree(int localStatus);
    int Exchange(const void* send, size_t bytes, void* receive);
    int ControlFailure(int status);
    int CleanupRegions();
    int ConnectQps(uint32_t first, uint32_t count);
    int DeviceViewAt(TileXRRDMAMemHandle handle, int peer, uint32_t qpIndex, TileXRRDMADeviceView* view);
    std::unique_ptr<RdmaProvider> provider_;
    RdmaOptions options_;
    std::map<TileXRRDMAMemHandle, Region> regions_;
    TileXRRDMAMemHandle nextHandle_ = 1;
    bool connected_ = false;
    bool failed_ = false;
    bool controlFailed_ = false;
    bool started_ = false;
    bool shuttingDown_ = false;
    uint32_t qpCount_ = 1;
    mutable std::mutex mutex_;
};
} // namespace TileXR
#endif
