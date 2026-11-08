/* Copyright (c) 2026 TileXR Project. Licensed under the Apache License, Version 2.0. */

/*
 * 功能：检查各 rank 的 RDMA 启用配置，初始化可选 transport，并实现公共 Host API 的参数检查与转发。
 * 调用关系：TileXRComm 调用初始化和 teardown；Host API 转给 TileXRRDMATransport，rank 配置通过 socket AllGather 交换。
 * 资源与生命周期：communicator 持有 transport；FinalizeRDMA 仅在 Shutdown 成功后销毁它，失败时保留对象。
 *
 * Purpose: Check RDMA enablement across ranks, initialize the optional transport and validate/forward public Host API calls.
 * Calls: TileXRComm invokes setup/teardown; Host APIs use TileXRRDMATransport and socket AllGather exchanges rank settings.
 * Lifetime: The communicator owns the transport; FinalizeRDMA destroys it only after successful Shutdown, retaining it on failure.
 */
#include "rdma/tilexr_rdma_transport.h"
#include <algorithm>
#include <cstdlib>
#include <new>
#include "tilexr_comm.h"
#include "tilexr_log.h"
#include "tools/socket/tilexr_sock_exchange.h"

namespace TileXR {
int TileXRComm::InitRDMA()
{
    if (memoryOnlyDomain_ || rankSize_ <= 1 || socketExchange_ == nullptr) { return TILEXR_SUCCESS; }
    const char* enable = std::getenv("TILEXR_ENABLE_RDMA");
    const std::string setting = enable == nullptr ? "0" : enable;
    const int local = setting == "1" ? 1 : (setting == "0" ? 0 : -1);
    std::vector<int> all(rankSize_);
    int ret = socketExchange_->AllGather(&local, 1, all.data());
    if (ret != TILEXR_SUCCESS) { return ret; }
    if (local < 0 || std::any_of(all.begin(), all.end(), [local](int value) { return value != local; })) {
        TILEXR_LOG(ERROR) << "TILEXR_ENABLE_RDMA must be 0 or 1 and match on every rank";
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    if (local == 0) { return TILEXR_SUCCESS; }

    RdmaConfig config;
    std::string error;
    ret = AgreeInternalStatus(LoadRdmaConfig(config, error));
    if (ret != TILEXR_SUCCESS) {
        TILEXR_LOG(WARN) << "RDMA configuration unavailable on at least one rank: " << error;
        return TILEXR_SUCCESS;
    }
    config.deviceId = static_cast<uint32_t>(devId_);
    auto provider = MakeRdmaVerbsProvider();
    if (provider != nullptr) {
        rdmaTransport_.reset(new (std::nothrow) TileXRRDMATransport(std::move(provider)));
    }
    ret = AgreeInternalStatus(rdmaTransport_ != nullptr ? TILEXR_SUCCESS : TILEXR_ERROR_INTERNAL);
    if (ret != TILEXR_SUCCESS) {
        rdmaTransport_.reset();
        return TILEXR_SUCCESS;
    }
    RdmaOptions options;
    options.rank = rank_;
    options.rankSize = rankSize_;
    options.config = config;
    options.allGather = [this](const void* send, size_t bytes, void* receive) {
        return socketExchange_->AllGather(static_cast<const uint8_t*>(send), bytes,
            static_cast<uint8_t*>(receive));
    };
    ret = rdmaTransport_->Init(options);
    if (ret == TILEXR_SUCCESS) { return TILEXR_SUCCESS; }
    TILEXR_LOG(WARN) << "RDMA setup failed collectively: " << ret << "; optional capability disabled";
    const int cleanup = rdmaTransport_->Shutdown();
    if (cleanup != TILEXR_SUCCESS) { return cleanup; }
    rdmaTransport_.reset();
    return TILEXR_SUCCESS;
}

int TileXRComm::FinalizeRDMA()
{
    if (rdmaTransport_ == nullptr) { return TILEXR_SUCCESS; }
    const int ret = rdmaTransport_->Shutdown();
    // Keep the object on every rank until all teardown phases succeed.
    if (ret == TILEXR_SUCCESS) { rdmaTransport_.reset(); }
    return ret;
}
TileXRRDMATransport* TileXRComm::GetRDMATransport() { return rdmaTransport_.get(); }
} // namespace TileXR

namespace {
TileXR::TileXRRDMATransport* Transport(void* comm)
{
    return comm == nullptr ? nullptr : static_cast<TileXR::TileXRComm*>(comm)->GetRDMATransport();
}
}

int TileXRRDMAQuery(void* comm, TileXRRDMAInfo* info)
{
    if (info != nullptr) { *info = TileXRRDMAInfo {}; info->version = 1; }
    if (comm == nullptr || info == nullptr) { return TileXR::TILEXR_ERROR_PARA_CHECK_FAIL; }
    const auto* transport = Transport(comm);
    if (transport != nullptr) { *info = transport->Query(); }
    return TileXR::TILEXR_SUCCESS;
}
int TileXRRDMARegister(void* comm, void* localPtr, size_t bytes, TileXRRDMAMemHandle* handle)
{
    if (handle != nullptr) { *handle = 0; }
    if (comm == nullptr) { return TileXR::TILEXR_ERROR_PARA_CHECK_FAIL; }
    auto* transport = Transport(comm);
    return transport == nullptr ? TileXR::TILEXR_ERROR_NOT_SUPPORT : transport->Register(localPtr, bytes, handle);
}
int TileXRRDMAConfigureQps(void* comm, uint32_t qpCount)
{
    if (comm == nullptr) { return TileXR::TILEXR_ERROR_PARA_CHECK_FAIL; }
    auto* transport = Transport(comm);
    return transport == nullptr ? TileXR::TILEXR_ERROR_NOT_SUPPORT : transport->ConfigureQps(qpCount);
}
int TileXRRDMAUnregister(void* comm, TileXRRDMAMemHandle handle)
{
    if (comm == nullptr) { return TileXR::TILEXR_ERROR_PARA_CHECK_FAIL; }
    auto* transport = Transport(comm);
    return transport == nullptr ? TileXR::TILEXR_ERROR_NOT_SUPPORT : transport->Unregister(handle);
}
int TileXRRDMAShutdown(void* comm)
{
    if (comm == nullptr) { return TileXR::TILEXR_ERROR_PARA_CHECK_FAIL; }
    return static_cast<TileXR::TileXRComm*>(comm)->FinalizeRDMA();
}
int TileXRRDMAPut(void* comm, TileXRRDMAMemHandle handle, int peer,
    size_t localOffset, size_t remoteOffset, size_t bytes)
{
    if (comm == nullptr) { return TileXR::TILEXR_ERROR_PARA_CHECK_FAIL; }
    auto* transport = Transport(comm);
    return transport == nullptr ? TileXR::TILEXR_ERROR_NOT_SUPPORT :
        transport->Transfer(false, handle, peer, localOffset, remoteOffset, bytes);
}
int TileXRRDMAGet(void* comm, TileXRRDMAMemHandle handle, int peer,
    size_t localOffset, size_t remoteOffset, size_t bytes)
{
    if (comm == nullptr) { return TileXR::TILEXR_ERROR_PARA_CHECK_FAIL; }
    auto* transport = Transport(comm);
    return transport == nullptr ? TileXR::TILEXR_ERROR_NOT_SUPPORT :
        transport->Transfer(true, handle, peer, localOffset, remoteOffset, bytes);
}

int TileXRRDMAGetDeviceViewV3(void* comm, TileXRRDMAMemHandle handle, int peer,
    TileXRRDMADeviceView* view)
{
    if (view != nullptr) { *view = TileXRRDMADeviceView {}; }
    if (comm == nullptr || view == nullptr) { return TileXR::TILEXR_ERROR_PARA_CHECK_FAIL; }
    auto* transport = Transport(comm);
    return transport == nullptr ? TileXR::TILEXR_ERROR_NOT_SUPPORT : transport->DeviceView(handle, peer, view);
}
int TileXRRDMAGetDeviceViewsV3(void* comm, TileXRRDMAMemHandle handle, int peer,
    TileXRRDMADeviceView* views, uint32_t qpCount)
{
    if (views != nullptr && qpCount >= 1 && qpCount <= TILEXR_RDMA_MAX_QPS) {
        std::fill_n(views, qpCount, TileXRRDMADeviceView {});
    }
    if (comm == nullptr || views == nullptr || qpCount == 0 || qpCount > TILEXR_RDMA_MAX_QPS) {
        return TileXR::TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    auto* transport = Transport(comm);
    return transport == nullptr ? TileXR::TILEXR_ERROR_NOT_SUPPORT :
        transport->DeviceViews(handle, peer, views, qpCount);
}
