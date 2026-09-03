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
    uint32_t funcId = 0;
    bool funcIdValid = false;
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
    uint16_t startJettyId = 0;
    uint32_t funcId = 0;
    bool funcIdValid = false;
    TileXRCcuHccpQpKey qpKey {};
    uint32_t psn = 0;
    bool endpointRouteVerified = false;
    bool channelResourceOwnerVerified = false;
    bool transportResourceExchangeVerified = false;
};

struct TileXRCcuRegisteredMemoryBufferInfo {
    uint64_t addr = 0;
    uint64_t bytes = 0;
    uint64_t alignedAddr = 0;
    uint64_t alignedBytes = 0;
    uint64_t targetSegVa = 0;
    uint32_t tokenId = 0;
    uint32_t rawTokenId = 0;
    uint32_t tokenValue = 0;
    TileXRCcuHccpMemKey key {};
    void* tokenIdHandle = nullptr;
    void* lmemHandle = nullptr;
    bool valid = false;
};

struct TileXRCcuRemoteMemoryBufferImportRequest {
    uint64_t addr = 0;
    uint64_t bytes = 0;
    uint64_t alignedAddr = 0;
    uint64_t offset = 0;
    uint32_t tokenId = 0;
    uint32_t rawTokenId = 0;
    uint32_t tokenValue = 0;
    TileXRCcuHccpMemKey key {};
    bool valid = false;
};

struct TileXRCcuImportedRemoteMemoryBufferInfo {
    uint64_t addr = 0;
    uint64_t bytes = 0;
    uint64_t targetSegVa = 0;
    void* rmemHandle = nullptr;
    bool valid = false;
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

struct TileXRCcuPeerEndpointState {
    uint32_t peerRank = 0;
    uint32_t peerDevicePhyId = 0;
    TileXRCcuLocalResourceWindowInfo resourceWindow;
    TileXRCcuHccpDevEidInfo eidInfo {};
    void* cqHandle = nullptr;
    void* qpHandle = nullptr;
    void* remoteQpHandle = nullptr;
    TileXRCcuHccpQpCreateInfo qpInfo {};
    TileXRCcuLowerLayerTransportRoute route;
    uint32_t jettyTokenValue = 0;
    uint32_t tpType = TILEXR_CCU_HCCP_TP_TYPE_RTP;
    uint32_t psn = 0;
    uint64_t localTpHandle = 0;
    uint8_t mappedJettyPriority = 0;
};

class TileXRCcuDirectRuntime {
public:
    int Init(const TileXRCcuDirectRuntimeOptions& options, TileXRCcuDirectRuntimeReport* report);
    void Shutdown();
    bool IsAvailable() const;

    int QueryBasicInfo(uint8_t dieId, TileXRCcuBasicInfo* basicInfo, TileXRCcuDriverAdapterReport* report);
    int CreateDriverAdapter(TileXRCcuDriverAdapter* adapter, TileXRCcuDriverAdapterReport* report);
    int RegisterCcuResourceRmaBuffer(uint64_t resourceAddr);
    int RegisterMemoryBuffer(uint64_t addr, uint64_t bytes, TileXRCcuRegisteredMemoryBufferInfo* info);
    int ImportRemoteMemoryBuffer(
        const TileXRCcuRemoteMemoryBufferImportRequest& request,
        TileXRCcuImportedRemoteMemoryBufferInfo* info);
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
    int PreparePeerEndpointRoutes(TileXRCcuDirectRuntimeReport* report);
    int CreatePeerEndpointState(
        uint32_t peerRank,
        uint32_t peerDevicePhyId,
        const std::array<uint8_t, TILEXR_CCU_EID_BYTES>& localEid,
        const std::array<uint8_t, TILEXR_CCU_EID_BYTES>& peerEid,
        uint32_t tpType,
        uint32_t peerOrdinal,
        TileXRCcuPeerEndpointState* state);
    int SelectTpRouteForPeer(
        void* ctxHandle,
        const std::array<uint8_t, TILEXR_CCU_EID_BYTES>& localEid,
        const std::array<uint8_t, TILEXR_CCU_EID_BYTES>& peerEid,
        uint32_t tpType,
        uint64_t* tpHandle,
        uint8_t* mappedJettyPriority);
    int QueryTpHandleForPeer(
        const std::array<uint8_t, TILEXR_CCU_EID_BYTES>& peerEid,
        uint64_t* tpHandle);
    int QueryTpHandleForPeer(
        void* ctxHandle,
        const std::array<uint8_t, TILEXR_CCU_EID_BYTES>& localEid,
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
    void ReleaseImportedRemoteMemoryBuffers();
    void ReleaseRegisteredMemoryBuffers();
    void ReleaseRegisteredResourceWindow();
    void ReleaseLocalEndpointRoute();
    void ReleasePeerEndpointState(TileXRCcuPeerEndpointState* state);
    void ReleasePeerEndpointRoutes();

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
    std::vector<TileXRCcuPeerEndpointState> peerEndpointStates_;
    std::vector<TileXRCcuRegisteredMemoryBufferInfo> registeredMemoryBuffers_;
    std::vector<TileXRCcuImportedRemoteMemoryBufferInfo> importedRemoteMemoryBuffers_;
    TileXRCcuHccpQpKey endpointQpKey_ = {};
    bool endpointQpKeyValid_ = false;
    bool endpointRouteBound_ = false;
    uint32_t endpointPsn_ = 1;
    uint32_t devicePhyId_ = 0;
    bool initialized_ = false;
};

} // namespace TileXR

#endif // TILEXR_CCU_DIRECT_RUNTIME_H
