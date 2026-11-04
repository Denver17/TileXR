/* Copyright (c) 2026 TileXR Project. Licensed under the Apache License, Version 2.0. */

/*
 * 功能：交换 QP/MR 元数据，校验 region 范围，执行 Host 传输并导出 AIV view。
 * 调用关系：RDMA Host API 调用本文件；RdmaProvider 执行本地资源操作，allGather 回调交换 rank 状态和元数据。
 * 资源与生命周期：transport 持有 provider 和注册记录；Shutdown 先销毁 queue 再注销 MR，控制通道损坏时保留 MR。
 *
 * Purpose: Exchange QP/MR metadata, check region bounds, perform Host transfers and export AIV views.
 * Calls: Serves RDMA Host APIs through RdmaProvider operations and allGather exchanges of rank status and metadata.
 * Lifetime: Owns the provider and registration records. Shutdown destroys queues before deregistering MRs;
 *           a broken control channel retains MRs.
 */
#include "rdma/tilexr_rdma_transport.h"
#include <algorithm>
#include <cstring>
#include <limits>
#include "tilexr_log.h"

namespace TileXR {
namespace {
constexpr size_t ENDPOINT_BYTES = 48;
constexpr size_t MEMORY_BYTES = 40;
constexpr uint32_t WIRE_VERSION = 1;
using EndpointWire = std::array<uint8_t, ENDPOINT_BYTES>;
using MemoryWire = std::array<uint8_t, MEMORY_BYTES>;
void Write(uint8_t* dst, uint64_t value, size_t bytes)
{
    for (size_t i = 0; i < bytes; ++i) {
        dst[bytes - i - 1] = static_cast<uint8_t>(value);
        value >>= 8;
    }
}
uint64_t Read(const uint8_t* src, size_t bytes)
{
    uint64_t value = 0;
    for (size_t i = 0; i < bytes; ++i) { value = (value << 8) | src[i]; }
    return value;
}
bool Contains(uint64_t size, uint64_t offset, uint64_t count)
{
    return offset <= size && count <= size - offset;
}
}

TileXRRDMATransport::TileXRRDMATransport(std::unique_ptr<RdmaProvider> provider)
    : provider_(std::move(provider)) {}

TileXRRDMATransport::~TileXRRDMATransport()
{
    if (started_) {
        // Destructors cannot invent collectives. Normal owners call Shutdown;
        // retain an MR on abnormal destruction rather than release remote-used memory.
        const int ret = provider_->DestroyQueues();
        if (regions_.empty() && ret == TILEXR_SUCCESS) { (void)provider_->Close(); }
        TILEXR_LOG(ERROR) << "RDMA transport destroyed without completed collective Shutdown";
    }
}

int TileXRRDMATransport::ControlFailure(int status)
{
    failed_ = true;
    controlFailed_ = true;
    return status;
}

int TileXRRDMATransport::Agree(int localStatus)
{
    std::array<uint8_t, 4> wire {};
    Write(wire.data(), static_cast<uint32_t>(localStatus), wire.size());
    std::vector<uint8_t> all(static_cast<size_t>(options_.rankSize) * wire.size());
    const int ret = options_.allGather(wire.data(), wire.size(), all.data());
    if (ret != TILEXR_SUCCESS) { return ControlFailure(ret); }
    for (int rank = 0; rank < options_.rankSize; ++rank) {
        const uint32_t value = static_cast<uint32_t>(Read(all.data() + rank * wire.size(), wire.size()));
        if (value != 0) {
            // Decode the signed status without implementation-defined unsigned-to-signed conversion.
            return value <= INT32_MAX ? static_cast<int>(value) :
                -static_cast<int>(UINT32_MAX - value) - 1;
        }
    }
    return TILEXR_SUCCESS;
}

int TileXRRDMATransport::Exchange(const void* send, size_t bytes, void* receive)
{
    const int exchanged = options_.allGather(send, bytes, receive);
    // All ranks enter the same result phase before consuming any metadata.
    // Never let one rank's rollback status acknowledge another rank's success.
    const int ret = Agree(exchanged);
    // A misframed stream can supply stale zero bytes to Agree. Never discard
    // the local I/O failure even when that subsequent result looks successful.
    if (exchanged != TILEXR_SUCCESS) { return ControlFailure(exchanged); }
    return ret == TILEXR_SUCCESS ? ret : ControlFailure(ret);
}

int TileXRRDMATransport::Init(const RdmaOptions& options)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (started_ || provider_ == nullptr || !options.allGather || options.rankSize < 2 ||
        options.rankSize > 128 || options.rank < 0 || options.rank >= options.rankSize) {
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    options_ = options;
    started_ = true;
    // Submission mode must agree before either side allocates provider queues.
    std::array<uint8_t, 4> mode {};
    Write(mode.data(), options_.config.aivDirect ? 1 : 0, mode.size());
    std::vector<std::array<uint8_t, 4>> modes(options_.rankSize);
    int ret = Exchange(mode.data(), mode.size(), modes.data());
    if (ret != TILEXR_SUCCESS) { return ret; }
    for (const auto& other : modes) {
        if (other != mode) { return TILEXR_ERROR_PARA_CHECK_FAIL; }
    }
    ret = Agree(provider_->Open(options_.config));
    if (ret != TILEXR_SUCCESS) { return ret; }
    ret = Agree(provider_->DeviceDirect() == options_.config.aivDirect ?
        TILEXR_SUCCESS : TILEXR_ERROR_NOT_SUPPORT);
    if (ret != TILEXR_SUCCESS) { return ret; }

    ret = ConnectQps(0, 1);
    connected_ = ret == TILEXR_SUCCESS;
    return ret;
}

int TileXRRDMATransport::ConnectQps(uint32_t first, uint32_t count)
{
    for (uint32_t qp = first; qp < first + count; ++qp) {
        std::vector<EndpointWire> local(options_.rankSize);
        int status = TILEXR_SUCCESS;
        for (int peer = 0; peer < options_.rankSize; ++peer) {
            if (peer == options_.rank) { continue; }
            RdmaEndpoint endpoint;
            status = provider_->Create(peer + static_cast<int>(qp) * options_.rankSize, endpoint);
            if (status != TILEXR_SUCCESS) {
                TILEXR_LOG(WARN) << "RDMA queue creation failed rank=" << options_.rank
                                << " peer=" << peer << " status=" << status;
                break;
            }
            auto& wire = local[peer];
            Write(wire.data(), WIRE_VERSION, 4);
            Write(wire.data() + 4, static_cast<uint32_t>(options_.rank), 4);
            Write(wire.data() + 8, static_cast<uint32_t>(peer), 4);
            Write(wire.data() + 12, endpoint.qpn, 4);
            Write(wire.data() + 16, endpoint.psn, 4);
            Write(wire.data() + 20, endpoint.mtu, 4);
            Write(wire.data() + 40, qp, 4);
            std::copy(endpoint.gid.begin(), endpoint.gid.end(), wire.begin() + 24);
        }
        int ret = Agree(status);
        if (ret != TILEXR_SUCCESS) { return ret; }
        std::vector<EndpointWire> all(static_cast<size_t>(options_.rankSize) * options_.rankSize);
        ret = Exchange(local.data(), local.size() * ENDPOINT_BYTES, all.data());
        if (ret != TILEXR_SUCCESS) { return ret; }
        for (int peer = 0; peer < options_.rankSize; ++peer) {
            if (peer == options_.rank) { continue; }
            const auto& wire = all[static_cast<size_t>(peer) * options_.rankSize + options_.rank];
            if (Read(wire.data(), 4) != WIRE_VERSION || Read(wire.data() + 4, 4) != static_cast<uint32_t>(peer) ||
                Read(wire.data() + 8, 4) != static_cast<uint32_t>(options_.rank) || Read(wire.data() + 40, 4) != qp) {
                status = TILEXR_ERROR_PARA_CHECK_FAIL;
                break;
            }
            RdmaEndpoint endpoint;
            endpoint.qpn = static_cast<uint32_t>(Read(wire.data() + 12, 4));
            endpoint.psn = static_cast<uint32_t>(Read(wire.data() + 16, 4));
            endpoint.mtu = static_cast<uint32_t>(Read(wire.data() + 20, 4));
            std::copy(wire.begin() + 24, wire.begin() + 40, endpoint.gid.begin());
            status = provider_->Connect(peer + static_cast<int>(qp) * options_.rankSize, endpoint);
            if (status != TILEXR_SUCCESS) {
                TILEXR_LOG(WARN) << "RDMA QP transition failed rank=" << options_.rank
                                << " peer=" << peer << " status=" << status;
                break;
            }
        }
        ret = Agree(status);
        if (ret != TILEXR_SUCCESS) { return ret; }
    }
    return TILEXR_SUCCESS;
}

TileXRRDMAInfo TileXRRDMATransport::Query() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    TileXRRDMAInfo info {};
    info.version = WIRE_VERSION;
    info.connected = connected_ && !shuttingDown_;
    info.deviceDirect = info.connected && provider_->DeviceDirect();
    info.hostDriven = info.connected && !info.deviceDirect;
    info.failed = failed_;
    info.peerCount = info.connected ? static_cast<uint32_t>(options_.rankSize - 1) : 0;
    for (const auto& entry : regions_) {
        if (!entry.second.remote.empty()) { ++info.registrationCount; }
    }
    return info;
}

int TileXRRDMATransport::ConfigureQps(uint32_t qpCount)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!started_ || controlFailed_) { return TILEXR_ERROR_NOT_INITIALIZED; }
    int status = !connected_ || failed_ || shuttingDown_ ? TILEXR_ERROR_NOT_INITIALIZED : TILEXR_SUCCESS;
    if (status == TILEXR_SUCCESS && !provider_->DeviceDirect()) { status = TILEXR_ERROR_NOT_SUPPORT; }
    if (qpCount < qpCount_ || qpCount > TILEXR_RDMA_MAX_QPS || !regions_.empty()) {
        status = TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    int ret = Agree(status);
    if (ret != TILEXR_SUCCESS) { return ret; }
    std::array<uint8_t, 4> wire {};
    Write(wire.data(), qpCount, wire.size());
    std::vector<std::array<uint8_t, 4>> all(options_.rankSize);
    ret = Exchange(wire.data(), wire.size(), all.data());
    if (ret != TILEXR_SUCCESS) { return ret; }
    for (const auto& other : all) {
        if (other != wire) { return TILEXR_ERROR_PARA_CHECK_FAIL; }
    }
    ret = ConnectQps(qpCount_, qpCount - qpCount_);
    if (ret != TILEXR_SUCCESS) { failed_ = true; return ret; }
    qpCount_ = qpCount;
    return TILEXR_SUCCESS;
}

int TileXRRDMATransport::Register(void* ptr, size_t bytes, TileXRRDMAMemHandle* handle)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (handle != nullptr) { *handle = 0; }
    if (!started_) { return TILEXR_ERROR_NOT_INITIALIZED; }
    if (controlFailed_) { return TILEXR_ERROR_NOT_INITIALIZED; }
    const uint64_t address = reinterpret_cast<uintptr_t>(ptr);
    int status = !connected_ || failed_ || shuttingDown_ ? TILEXR_ERROR_NOT_INITIALIZED : TILEXR_SUCCESS;
    if (ptr == nullptr || bytes == 0 || handle == nullptr || bytes > UINT64_MAX - address ||
        nextHandle_ == UINT64_MAX) { status = TILEXR_ERROR_PARA_CHECK_FAIL; }
    if (status == TILEXR_SUCCESS) {
        for (const auto& entry : regions_) {
            if (entry.second.remote.empty()) {
                status = TILEXR_ERROR_NOT_INITIALIZED;
                break;
            }
            const auto& local = entry.second.local;
            if (address < local.address + local.bytes && local.address < address + bytes) {
                status = TILEXR_ERROR_PARA_CHECK_FAIL;
                break;
            }
        }
    }
    int ret = Agree(status);
    if (ret != TILEXR_SUCCESS) { return ret; }

    const auto id = nextHandle_++;
    Region& region = regions_[id];
    status = provider_->Register(ptr, bytes, region.local);
    ret = Agree(status);
    if (ret == TILEXR_SUCCESS) {
        MemoryWire wire {};
        Write(wire.data(), WIRE_VERSION, 4);
        Write(wire.data() + 4, static_cast<uint32_t>(options_.rank), 4);
        Write(wire.data() + 8, id, 8);
        Write(wire.data() + 16, region.local.address, 8);
        Write(wire.data() + 24, region.local.bytes, 8);
        Write(wire.data() + 32, region.local.rkey, 4);
        std::vector<MemoryWire> all(options_.rankSize);
        ret = Exchange(wire.data(), MEMORY_BYTES, all.data());
        if (ret != TILEXR_SUCCESS) {
            // The channel may be out of frame. Keep the unpublished MR and stop
            // reusing bootstrap; no remote handle or rkey may be exposed.
            return ret;
        }
        region.remote.resize(options_.rankSize);
        for (int rank = 0; rank < options_.rankSize; ++rank) {
            const auto& remote = all[rank];
            const uint64_t base = Read(remote.data() + 16, 8);
            const uint64_t length = Read(remote.data() + 24, 8);
            if (Read(remote.data(), 4) != WIRE_VERSION ||
                Read(remote.data() + 4, 4) != static_cast<uint32_t>(rank) ||
                Read(remote.data() + 8, 8) != id || base == 0 || length == 0 || length > UINT64_MAX - base) {
                status = TILEXR_ERROR_PARA_CHECK_FAIL;
                break;
            }
            region.remote[rank].address = base;
            region.remote[rank].bytes = length;
            region.remote[rank].rkey = static_cast<uint32_t>(Read(remote.data() + 32, 4));
        }
        ret = Agree(status);
    }
    if (ret != TILEXR_SUCCESS) {
        if (controlFailed_) { return ret; } // No cleanup collective on a broken channel.
        const int cleanup = Agree(provider_->Deregister(region.local));
        if (cleanup == TILEXR_SUCCESS) { regions_.erase(id); }
        else { failed_ = true; } // Keep any retained MR until Shutdown.
        return ret;
    }
    *handle = id;
    return TILEXR_SUCCESS;
}

int TileXRRDMATransport::Unregister(TileXRRDMAMemHandle handle)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!started_) { return TILEXR_ERROR_NOT_INITIALIZED; }
    if (controlFailed_) { return TILEXR_ERROR_NOT_INITIALIZED; }
    auto it = regions_.find(handle);
    int status = it == regions_.end() ? TILEXR_ERROR_PARA_CHECK_FAIL : TILEXR_SUCCESS;
    if (failed_ || shuttingDown_) { status = TILEXR_ERROR_NOT_INITIALIZED; }
    int ret = Agree(status);
    if (ret != TILEXR_SUCCESS) { return ret; }
    // Check that each rank is unregistering the same generation before touching MRs.
    std::array<uint8_t, 8> wire {};
    Write(wire.data(), handle, wire.size());
    std::vector<std::array<uint8_t, 8>> all(options_.rankSize);
    ret = Exchange(wire.data(), wire.size(), all.data());
    if (ret != TILEXR_SUCCESS) { return ret; }
    for (const auto& other : all) {
        if (other != wire) { return TILEXR_ERROR_PARA_CHECK_FAIL; }
    }
    // A partial deregistration must prevent further transfers on every rank.
    it->second.remote.clear();
    ret = Agree(provider_->Deregister(it->second.local));
    if (ret == TILEXR_SUCCESS) { regions_.erase(it); }
    return ret;
}

int TileXRRDMATransport::Transfer(bool read, TileXRRDMAMemHandle handle, int peer,
    size_t localOffset, size_t remoteOffset, size_t bytes)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!connected_ || failed_ || shuttingDown_) { return TILEXR_ERROR_NOT_INITIALIZED; }
    if (provider_->DeviceDirect()) { return TILEXR_ERROR_NOT_SUPPORT; }
    const auto it = regions_.find(handle);
    if (it == regions_.end() || it->second.remote.empty() || peer < 0 || peer >= options_.rankSize ||
        peer == options_.rank || bytes == 0 || bytes > UINT32_MAX) { return TILEXR_ERROR_PARA_CHECK_FAIL; }
    const auto& region = it->second;
    const auto& remote = region.remote[peer];
    if (!Contains(region.local.bytes, localOffset, bytes) || !Contains(remote.bytes, remoteOffset, bytes)) {
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    const int ret = provider_->Transfer(read, peer, region.local, localOffset,
        remote, remoteOffset, static_cast<uint32_t>(bytes), options_.config.timeoutMs);
    if (ret != TILEXR_SUCCESS) { failed_ = true; }
    return ret;
}

int TileXRRDMATransport::DeviceView(TileXRRDMAMemHandle handle, int peer, TileXRRDMADeviceView* view)
{
    std::lock_guard<std::mutex> lock(mutex_);
    return DeviceViewAt(handle, peer, 0, view);
}

int TileXRRDMATransport::DeviceViews(TileXRRDMAMemHandle handle, int peer,
    TileXRRDMADeviceView* views, uint32_t qpCount)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (views == nullptr || qpCount == 0 || qpCount > TILEXR_RDMA_MAX_QPS) {
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    std::fill_n(views, qpCount, TileXRRDMADeviceView {});
    if (qpCount != qpCount_) { return TILEXR_ERROR_PARA_CHECK_FAIL; }
    std::array<TileXRRDMADeviceView, TILEXR_RDMA_MAX_QPS> candidates {};
    for (uint32_t qp = 0; qp < qpCount; ++qp) {
        const int ret = DeviceViewAt(handle, peer, qp, &candidates[qp]);
        if (ret != TILEXR_SUCCESS) { return ret; }
        for (uint32_t previous = 0; previous < qp; ++previous) {
            if (candidates[previous].queue.qpn == candidates[qp].queue.qpn ||
                candidates[previous].queue.state == candidates[qp].queue.state) {
                return TILEXR_ERROR_INTERNAL;
            }
        }
    }
    std::copy_n(candidates.begin(), qpCount, views);
    return TILEXR_SUCCESS;
}

int TileXRRDMATransport::DeviceViewAt(TileXRRDMAMemHandle handle, int peer,
    uint32_t qpIndex, TileXRRDMADeviceView* view)
{
    if (view == nullptr) { return TILEXR_ERROR_PARA_CHECK_FAIL; }
    *view = TileXRRDMADeviceView {};
    if (!connected_ || failed_ || shuttingDown_) { return TILEXR_ERROR_NOT_INITIALIZED; }
    if (!provider_->DeviceDirect()) { return TILEXR_ERROR_NOT_SUPPORT; }
    const auto it = regions_.find(handle);
    if (it == regions_.end() || it->second.remote.empty() || peer < 0 || peer >= options_.rankSize ||
        peer == options_.rank) { return TILEXR_ERROR_PARA_CHECK_FAIL; }
    TileXRRDMADeviceView candidate {};
    const int ret = provider_->DeviceQueue(peer + static_cast<int>(qpIndex) * options_.rankSize, candidate.queue);
    if (ret != TILEXR_SUCCESS) { return ret; }
    const auto& local = it->second.local;
    const auto& remote = it->second.remote[peer];
    candidate.version = TILEXR_RDMA_DEVICE_VERSION;
    candidate.structBytes = sizeof(candidate);
    candidate.localAddress = local.address;
    candidate.localBytes = local.bytes;
    candidate.localKey = local.lkey;
    candidate.remoteAddress = remote.address;
    candidate.remoteBytes = remote.bytes;
    candidate.remoteKey = remote.rkey;
    *view = candidate;
    return TILEXR_SUCCESS;
}

int TileXRRDMATransport::CleanupRegions()
{
    int status = TILEXR_SUCCESS;
    for (auto& entry : regions_) {
        const int ret = provider_->Deregister(entry.second.local);
        if (status == TILEXR_SUCCESS) { status = ret; }
    }
    return status;
}

int TileXRRDMATransport::Shutdown()
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!started_) { return TILEXR_SUCCESS; }
    shuttingDown_ = true;
    connected_ = false;
    if (controlFailed_) {
        // Local QP teardown is safe; MR release needs agreement from ALL ranks.
        // A broken bootstrap cannot provide it. Retain MRs until process exit.
        const int ret = provider_->DestroyQueues();
        TILEXR_LOG(ERROR) << "RDMA bootstrap failed; MRs retained until process exit";
        return ret == TILEXR_SUCCESS ? TILEXR_ERROR_NOT_INITIALIZED : ret;
    }
    // All ranks close their send paths before any rank releases registered memory.
    int ret = Agree(provider_->DestroyQueues());
    if (ret != TILEXR_SUCCESS) { return ret; }
    ret = Agree(CleanupRegions());
    if (ret != TILEXR_SUCCESS) { return ret; }
    regions_.clear();
    ret = Agree(provider_->Close());
    if (ret != TILEXR_SUCCESS) { return ret; }
    started_ = false;
    return TILEXR_SUCCESS;
}
} // namespace TileXR
