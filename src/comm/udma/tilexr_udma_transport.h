/*
 * Copyright (c) 2024-2026 TileXR Project
 * Licensed under the Apache License, Version 2.0
 */

#ifndef TILEXR_UDMA_TRANSPORT_H
#define TILEXR_UDMA_TRANSPORT_H

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <tuple>
#include <vector>

#include "comm_args.h"
#include "tilexr_api.h"
#include "tilexr_types.h"
#include "tilexr_udma_types.h"
#include "tilexr_udma_fullmesh.h"
#include "udma/tilexr_hccp_defs.h"
#include "udma/tilexr_hccp_loader.h"
#include "udma/tilexr_udma_config.h"
#include "udma/tilexr_udma_layout.h"

namespace TileXR {

class TileXRSockExchange;

struct TileXRUDMATransportOptions {
    int rank = 0;
    int rankSize = 0;
    int localRankSize = 0;
    int devId = 0;
    bool nonPinRegistration = false;
    bool enableFullmeshDomain = false;
    TileXRSockExchange* exchange = nullptr;
    UDMAQpConfig qpConfig;
};

class TileXRUDMATransport {
public:
    TileXRUDMATransport();
    ~TileXRUDMATransport();
    TileXRUDMATransport(const TileXRUDMATransport&) = delete;
    TileXRUDMATransport& operator=(const TileXRUDMATransport&) = delete;

    int Init(const TileXRUDMATransportOptions& options);
    int PrepareMemory(GM_ADDR localPtr, size_t bytes);
    GM_ADDR GetPreparedUDMAInfoDev() const;
    GM_ADDR GetPreparedFullmeshInfoDev() const;
    bool PreparedFullmeshReady() const;
    int CommitPreparedMemory();
    int AbortPreparedMemory();
    int CleanupRetiredMemory();
    int CleanupAllMemory();
    int UnregisterMemory(GM_ADDR localPtr);
    int PrepareProfile(const TileXRUDMAProfileDesc& desc);
    GM_ADDR GetPreparedProfileInfoDev() const;
    int CommitPreparedProfile(TileXRUDMAProfileHandle handle);
    int AbortPreparedProfile();
    int CleanupProfile(TileXRUDMAProfileHandle handle);
    int CleanupAllProfiles();
    GM_ADDR GetProfileInfoDev(TileXRUDMAProfileHandle handle) const;
    bool HasProfileCleanupPending() const;
    int Shutdown();

    bool IsAvailable() const;
    GM_ADDR GetUDMAInfoDev() const;
    GM_ADDR GetBaseUDMAInfoDev() const;
    GM_ADDR GetFullmeshInfoDev() const;
    GM_ADDR GetBaseFullmeshInfoDev() const;
    GM_ADDR GetRegisteredMemoryPtr() const;
    size_t GetRegisteredMemoryBytes() const;
    bool HasMemoryCleanupPending() const;
    uint32_t GetQpCount() const;
    bool UsesSharedQps() const;
    bool UsesGroupedQpProfile() const;
    bool HasFullmeshDomain() const;
    uint32_t GetFullmeshValidPeerMask() const;

private:
    struct PerEidState;
    struct PerPeerQpState;
    struct SharedQpState;
    struct RegisteredRegionState;
    struct RegistrationState;

    int AgreeInitStatus(int localStatus) const;
    int AgreeRaOwnership() const;
    int AgreeEidCount() const;
    int OpenDevice();
    int BuildRoutes();
    int BuildLegacyRoutes(const std::vector<DevEidInfo>& devEids);
    int BuildExplicitRoutes(const std::vector<DevEidInfo>& devEids);
    int InitFullmeshDomain();
    int BuildFullmeshRoutes();
    int CreateContexts();
    int CreateQueues();
    int CreateLegacyQueues();
    int CreateExplicitQueues();
    int CreateSharedQueues();
    int CreateFullmeshQueues();
    int ImportQueues();
    int ImportLegacyQueues();
    int ImportExplicitQueues();
    int ImportSharedQueues();
    int ImportFullmeshQueues();
    int RefreshUDMAInfo();
    int RefreshFullmeshInfo();
    int BuildQueueImages(std::vector<UDMAWQCtx>& sq, std::vector<UDMAWQCtx>& rq,
        std::vector<UDMACQCtx>& scq, std::vector<UDMACQCtx>& rcq,
        std::vector<UDMAMemInfo>& mem) const;
    int BuildRegistrationUDMAInfo(RegistrationState& registration);
    int PrepareFullmeshRegistration(RegistrationState& registration);
    int BuildFullmeshRegistrationUDMAInfo(
        RegistrationState& registration);
    int BuildFullmeshQueueImages(std::vector<UDMAWQCtx>& sq,
        std::vector<UDMAWQCtx>& rq, std::vector<UDMACQCtx>& scq,
        std::vector<UDMACQCtx>& rcq,
        std::vector<UDMAMemInfo>& mem) const;
    int RegisterMemoryOnContexts(
        RegistrationState& registration, bool fullmeshDomain);
    int ExchangeAndImportMemory(RegistrationState& registration);
    int PrepareRegistration(const TileXRUDMAProfileDesc& desc,
        std::unique_ptr<RegistrationState>& registration,
        bool prepareFullmesh);
    int AgreeRegistrationStatus(int localStatus) const;
    int CleanupLocalRegistrations(std::map<uint32_t, RegMemResultInfo>& byEid);
    int CleanupRemoteImports(
        RegistrationState& registration, bool fullmeshDomain);
    int CleanupFullmeshRegistration(RegistrationState& registration);
    int CleanupRegistration(RegistrationState& registration);
    int CleanupRegistrationPtr(std::unique_ptr<RegistrationState>& registration);
    int FreeDeviceInfo(GM_ADDR& infoDev) const;
    int AllocDeviceScalar(void** ptr, size_t bytes) const;
    int FreeDeviceScalar(void*& ptr) const;
    int CleanupQueues();
    int CleanupFullmeshQueues();
    int RollbackAddedContexts(
        const std::map<uint32_t, void*>& previousContexts,
        const std::map<uint32_t, void*>& previousTokens);
    int CleanupContexts();
    uint32_t FallbackLocalEid() const;
    const PerPeerQpState* GetPeerQpState(int peer, uint32_t qpIdx) const;
    PerPeerQpState* GetPeerQpState(int peer, uint32_t qpIdx);
    const PerPeerQpState* GetFallbackQpState(uint32_t qpIdx) const;
    const SharedQpState* GetSharedQpState(uint32_t qpIdx) const;
    SharedQpState* GetSharedQpState(uint32_t qpIdx);
    const PerPeerQpState* GetFullmeshQpState(int peer) const;
    PerPeerQpState* GetFullmeshQpState(int peer);
    size_t RouteIndex(int peer, uint32_t qpIdx) const;
    size_t FullmeshEntryIndex(int peer, uint32_t slot) const;

    TileXRHccpLoader loader_;
    TileXRUDMATransportOptions options_ {};
    bool available_ = false;
    bool tsdOpened_ = false;
    bool raInitialized_ = false;
    bool raAttached_ = false;
    pid_t subPid_ = 0;
    uint32_t logicDevId_ = 0;
    uint32_t deviceIdOffset_ = 0;
    uint32_t eidCount_ = 0;
    std::map<uint32_t, void*> ctxHandleByEid_;
    std::map<uint32_t, void*> tokenHandleByEid_;
    std::map<int, uint32_t> peerLocalEid_;
    std::map<int, uint32_t> peerRemoteEid_;
    std::map<uint32_t, PerEidState> states_;
    std::vector<std::unique_ptr<PerPeerQpState>> peerQpStates_;
    std::vector<std::unique_ptr<SharedQpState>> sharedQpStates_;
    std::vector<std::unique_ptr<PerPeerQpState>> fullmeshQpStates_;
    std::vector<uint32_t> localRouteByPeerQp_;
    std::vector<uint32_t> remoteRouteByPeerQp_;
    std::vector<uint32_t> fullmeshLocalRouteByPeer_;
    std::vector<uint32_t> fullmeshRemoteRouteByPeer_;
    std::map<uint32_t, HccpEid> localEidByEid_;
    std::unique_ptr<RegistrationState> activeRegistration_;
    std::unique_ptr<RegistrationState> preparedRegistration_;
    std::vector<std::unique_ptr<RegistrationState>> retiredRegistrations_;
    std::unique_ptr<RegistrationState> preparedProfile_;
    std::map<TileXRUDMAProfileHandle, std::unique_ptr<RegistrationState>> profiles_;
    GM_ADDR udmaInfoDev_ = nullptr;
    GM_ADDR baseUDMAInfoDev_ = nullptr;
    GM_ADDR fullmeshInfoDev_ = nullptr;
    GM_ADDR baseFullmeshInfoDev_ = nullptr;
    GM_ADDR eidTableDev_ = nullptr;
    uint32_t udmaInfoSize_ = 0;
    uint32_t fullmeshInfoSize_ = 0;
    uint32_t qpCount_ = 1;
    bool explicitConfig_ = false;
    bool sharedQp_ = false;
    bool groupedQpProfile_ = false;
    bool fullmeshAvailable_ = false;
    uint32_t fullmeshValidPeerMask_ = 0U;
    bool shutdownComplete_ = true;
};

} // namespace TileXR

#endif // TILEXR_UDMA_TRANSPORT_H
