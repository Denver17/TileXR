/*
 * Copyright (c) 2026 TileXR Project
 * Licensed under the Apache License, Version 2.0
 */

#ifndef TILEXR_CCU_DIRECT_RUNTIME_H
#define TILEXR_CCU_DIRECT_RUNTIME_H

#include "ccu/tilexr_ccu_driver_adapter.h"
#include "ccu/tilexr_ccu_hccp_loader.h"
#include "ccu/tilexr_ccu_lower_layer_plan_builder.h"
#include "ccu/tilexr_ccu_ra_custom_channel_provider.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace TileXR {

using TileXRCcuDirectAllGatherFn = int (*)(
    const void* sendBuf,
    size_t sendBytes,
    void* recvBuf,
    void* userData);

struct TileXRCcuLocalResourceWindowInfo;

using TileXRCcuLocalEndpointRouteCollectorFn = int (*)(
    uint32_t devicePhyId,
    const TileXRCcuLocalResourceWindowInfo& localResourceWindow,
    TileXRCcuLowerLayerTransportRoute* route,
    void* userData);

struct TileXRCcuDirectRuntimeOptions {
    int rank = 0;
    int rankSize = 0;
    int devId = 0;
    TileXRCcuDirectAllGatherFn allGather = nullptr;
    void* allGatherUserData = nullptr;
    TileXRCcuLocalEndpointRouteCollectorFn localEndpointRouteCollector = nullptr;
    void* localEndpointRouteCollectorUserData = nullptr;
};

struct TileXRCcuLocalResourceWindowInfo {
    uint64_t addr = 0;
    uint64_t bytes = 0;
    uint32_t tokenId = 0;
    uint32_t rawTokenId = 0;
    uint32_t tokenValue = 0;
    uint64_t targetSegHandle = 0;
    void* raCtxHandle = nullptr;
    void* tokenIdHandle = nullptr;
    void* lmemHandle = nullptr;
    std::array<uint8_t, TILEXR_CCU_EID_BYTES> eid {};
    uint32_t eidIndex = 0;
    bool raCtxRegistered = false;
};

struct TileXRCcuResourceWindowExchange {
    uint64_t addr = 0;
    uint64_t bytes = 0;
    uint32_t tokenId = 0;
    uint32_t rawTokenId = 0;
    uint32_t tokenValue = 0;
    std::array<uint8_t, TILEXR_CCU_EID_BYTES> remoteEid {};
    uint32_t tpn = 0;
    uint64_t doorbellVa = 0;
    uint32_t doorbellTokenId = 0;
    uint32_t doorbellTokenValue = 0;
    uint32_t sqDepth = 0;
    TileXRCcuHccpQpKey qpKey {};
    uint32_t psn = 0;
    bool endpointRouteVerified = false;
    bool channelResourceOwnerVerified = false;
    bool transportResourceExchangeVerified = false;
};

struct TileXRCcuDirectRuntimeReport {
    bool initialized = false;
    bool raInitialized = false;
    bool ccuTlvInitialized = false;
    uint32_t logicDevId = 0;
    uint32_t devicePhyId = 0;
    int hdcType = 0;
    std::string message;
};

class TileXRCcuDirectRuntime {
public:
    int Init(const TileXRCcuDirectRuntimeOptions& options, TileXRCcuDirectRuntimeReport* report);
    void Shutdown();
    bool IsAvailable() const;

    int QueryBasicInfo(uint8_t dieId, TileXRCcuBasicInfo* basicInfo, TileXRCcuDriverAdapterReport* report);
    int CreateDriverAdapter(TileXRCcuDriverAdapter* adapter, TileXRCcuDriverAdapterReport* report);
    int RegisterCcuResourceRmaBuffer(uint64_t resourceAddr);
    int RefreshLocalVerifiedEndpointRoute(TileXRCcuDirectRuntimeReport* report);
    int ConfigureLocalVerifiedEndpointRoute(const TileXRCcuLowerLayerTransportRoute& route);
    int ExportLocalCcuRmaBuffer(TileXRCcuLocalResourceWindowInfo* info) const;
    int ExportRemoteCcuRmaBuffers(std::vector<TileXRCcuRemoteCcuBufferInfo>* buffers);
    int ExportLowerLayerTransportSnapshot(
        const TileXRCcuLowerLayerTransportSnapshot& templateSnapshot,
        TileXRCcuLowerLayerTransportSnapshot* snapshot) const;

private:
    int ResolveDevicePhyId(uint32_t* devicePhyId, TileXRCcuDirectRuntimeReport* report) const;
    int RegisterCcuResourceRmaBufferWithRaCtx(uint64_t resourceAddr, uint64_t resourceBytes);
    int CollectLocalEndpointRouteWithRaCtx(TileXRCcuLowerLayerTransportRoute* route);
    int CollectLocalEndpointRouteWithRaCtxOnce(
        TileXRCcuLowerLayerTransportRoute* route,
        bool* asyncWaitFailed);
    int QueryTpHandleForPeer(
        const std::array<uint8_t, TILEXR_CCU_EID_BYTES>& peerEid,
        uint64_t* tpHandle);
    int ImportPeerEndpointRoute(
        const TileXRCcuResourceWindowExchange& peerWindow,
        uint64_t localTpHandle,
        uint64_t peerTpHandle,
        uint32_t localPsn,
        uint32_t peerPsn,
        TileXRCcuLowerLayerTransportRoute* importedRoute);
    void ReleasePeerEndpointImports();
    void ReleaseRegisteredResourceWindow();
    void ReleaseLocalEndpointRoute();

    TileXRCcuDirectRuntimeOptions options_;
    TileXRCcuHccpLoader loader_;
    TileXRCcuRaCustomChannelProvider raCustomChannelProvider_;
    TileXRCcuBasicInfo cachedBasicInfo_ = {};
    TileXRCcuLocalResourceWindowInfo localResourceWindow_ = {};
    TileXRCcuLowerLayerTransportRoute localVerifiedEndpointRoute_ = {};
    bool cachedBasicInfoValid_ = false;
    bool resourceWindowRegistered_ = false;
    bool localVerifiedEndpointRouteValid_ = false;
    void* endpointChanHandle_ = nullptr;
    void* endpointCqHandle_ = nullptr;
    void* endpointQpHandle_ = nullptr;
    void* endpointRemoteQpHandle_ = nullptr;
    std::vector<void*> endpointPeerRemoteQpHandles_;
    TileXRCcuHccpQpKey endpointQpKey_ = {};
    bool endpointQpKeyValid_ = false;
    bool endpointRouteBound_ = false;
    uint32_t endpointPsn_ = 1;
    uint32_t devicePhyId_ = 0;
    bool initialized_ = false;
};

} // namespace TileXR

#endif // TILEXR_CCU_DIRECT_RUNTIME_H
