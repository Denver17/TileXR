/*
 * Copyright (c) 2024-2026 TileXR Project
 * Licensed under the Apache License, Version 2.0
 */

#include "udma/tilexr_udma_transport.h"

#include <acl/acl_rt.h>
#include <algorithm>
#include <array>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>
#include <string>

#include "tilexr_log.h"
#include "tools/socket/tilexr_sock_exchange.h"
#include "udma/tilexr_udma_root_info.h"

namespace TileXR {
namespace {

constexpr int TILEXR_HCCP_RA_ALREADY_INITIALIZED = 328002;
constexpr int TILEXR_HCCP_RA_QP_UNIMPORT_UNSUPPORTED = 228104;

bool QpUnimportConsumesHostHandle(int ret)
{
    // The b131 client frees rem_qp_handle even when the service rejects the
    // unimport opcode. The server-side QP is reclaimed with the RA context.
    return ret == 0 || ret == TILEXR_HCCP_RA_QP_UNIMPORT_UNSUPPORTED;
}

bool AttachExistingRaEnabled()
{
    const char* value = std::getenv("TILEXR_UDMA_ATTACH_EXISTING_RA");
    return value != nullptr && std::strcmp(value, "1") == 0;
}

uint32_t Log2Uint64(uint64_t value)
{
    uint32_t result = 0;
    while (value > 1) {
        value >>= 1;
        ++result;
    }
    return result;
}

HccpEid SwapEidForDevice(const HccpEid& hccpEid)
{
    HccpEid swapped {};
    uint64_t eidL = 0;
    uint64_t eidH = 0;
    std::memcpy(&eidL, hccpEid.raw, sizeof(uint64_t));
    std::memcpy(&eidH, hccpEid.raw + sizeof(uint64_t), sizeof(uint64_t));
    eidL = __builtin_bswap64(eidL);
    eidH = __builtin_bswap64(eidH);
    std::memcpy(swapped.raw, &eidH, sizeof(uint64_t));
    std::memcpy(swapped.raw + sizeof(uint64_t), &eidL, sizeof(uint64_t));
    return swapped;
}

UDMAMemInfo BuildMemInfo(const RegMemResultInfo& registration)
{
    UDMAMemInfo memInfo {};
    memInfo.tokenValueValid = true;
    memInfo.rmtJettyType = 1;
    memInfo.targetHint = 0;
    memInfo.tpn = 0;
    memInfo.tid = registration.tokenId >> 8;
    memInfo.rmtTokenValue = registration.tokenValue;
    memInfo.len = static_cast<uint32_t>(std::min<uint64_t>(registration.size, UINT32_MAX));
    memInfo.addr = registration.address;
    return memInfo;
}

} // namespace

struct TileXRUDMATransport::PerEidState {
    uint32_t eidIndex = 0;
    void* ctxHandle = nullptr;
    void* tokenHandle = nullptr;
    void* chanHandle = nullptr;
    void* cqHandle = nullptr;
    void* qpHandle = nullptr;
    CqInfoT cqInfo {};
    QpCreateInfo qpInfo {};
    std::vector<void*> remoteQpHandles;
    std::vector<uint32_t> tpnList;
    void* cqPiAddr = nullptr;
    void* cqCiAddr = nullptr;
    void* sqPiAddr = nullptr;
    void* sqCiAddr = nullptr;
    void* wqeCntAddr = nullptr;
    void* amoAddr = nullptr;
    UDMAWQCtx localWq {};
    UDMACQCtx localCq {};
};

struct TileXRUDMATransport::PerPeerQpState {
    int peer = -1;
    uint32_t qpIdx = 0;
    uint32_t localEid = 0;
    uint32_t remoteEid = 0;
    void* ctxHandle = nullptr;
    void* tokenHandle = nullptr;
    void* chanHandle = nullptr;
    void* cqHandle = nullptr;
    void* qpHandle = nullptr;
    void* remoteQpHandle = nullptr;
    CqInfoT cqInfo {};
    QpCreateInfo qpInfo {};
    uint32_t tpn = 0;
    void* cqPiAddr = nullptr;
    void* cqCiAddr = nullptr;
    void* sqPiAddr = nullptr;
    void* sqCiAddr = nullptr;
    void* wqeCntAddr = nullptr;
    void* amoAddr = nullptr;
    UDMAWQCtx localWq {};
    UDMACQCtx localCq {};
};

struct TileXRUDMATransport::SharedQpState {
    uint32_t qpIdx = 0;
    uint32_t localEid = 0;
    void* ctxHandle = nullptr;
    void* tokenHandle = nullptr;
    void* chanHandle = nullptr;
    void* cqHandle = nullptr;
    void* qpHandle = nullptr;
    CqInfoT cqInfo {};
    QpCreateInfo qpInfo {};
    std::vector<void*> remoteQpHandles;
    std::vector<uint32_t> tpnList;
    void* cqPiAddr = nullptr;
    void* cqCiAddr = nullptr;
    void* sqPiAddr = nullptr;
    void* sqCiAddr = nullptr;
    void* wqeCntAddr = nullptr;
    void* amoAddr = nullptr;
    UDMAWQCtx localWq {};
    UDMACQCtx localCq {};
};

struct TileXRUDMATransport::RegisteredRegionState {
    GM_ADDR localPtr = nullptr;
    size_t bytes = 0;
    std::map<uint32_t, RegMemResultInfo> localRegistrations;
    std::map<std::tuple<int, uint32_t, uint32_t>, void*> remoteMemHandles;
    std::map<uint32_t, UDMAMemInfo> localMemInfoByEid;
    std::map<uint32_t, RegMemResultInfo> fullmeshLocalRegistrations;
    std::map<std::tuple<int, uint32_t, uint32_t>, void*>
        fullmeshRemoteMemHandles;
};

struct TileXRUDMATransport::RegistrationState {
    std::vector<RegisteredRegionState> regions;
    std::vector<TileXRUDMAProfileQpBinding> qpBindings;
    std::vector<UDMAMemInfo> memoryImage;
    std::vector<UDMAMemInfo> fullmeshMemoryImage;
    GM_ADDR infoDev = nullptr;
    GM_ADDR fullmeshInfoDev = nullptr;
    uint32_t infoSize = 0;
    uint32_t fullmeshInfoSize = 0;
    bool fullmeshReady = false;
    bool cleanupPending = false;
};

TileXRUDMATransport::TileXRUDMATransport() = default;

TileXRUDMATransport::~TileXRUDMATransport()
{
    const int ret = Shutdown();
    if (ret != TILEXR_SUCCESS) {
        TILEXR_LOG(ERROR) << "TileXR UDMA transport fallback cleanup failed: " << ret;
    }
}

int TileXRUDMATransport::Init(const TileXRUDMATransportOptions& options)
{
    if (available_) {
        return TILEXR_SUCCESS;
    }
    if (options.rankSize <= 1) {
        return TILEXR_SUCCESS;
    }
    if (options.rank < 0 || options.rank >= options.rankSize ||
        options.rankSize > TILEXR_MAX_RANK_SIZE || options.exchange == nullptr) {
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    options_ = options;
    shutdownComplete_ = false;
    explicitConfig_ = options_.qpConfig.explicitConfig;
    sharedQp_ = options_.qpConfig.sharedQp;
    qpCount_ = UDMAQpConfigQpCount(options_.qpConfig);
    std::string sharedQpError;
    if (qpCount_ == 0 || qpCount_ > TILEXR_UDMA_MAX_QP_COUNT ||
        (sharedQp_ && !ValidateUDMASharedQpConfig(
            options_.qpConfig, &sharedQpError)) ||
        (explicitConfig_ && (options_.localRankSize <= 0 ||
            options_.localRankSize > options_.rankSize ||
            options_.rankSize % options_.localRankSize != 0))) {
        if (!sharedQpError.empty()) {
            TILEXR_LOG(ERROR) << sharedQpError;
        }
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    groupedQpProfile_ = IsUDMAGroupedQpProfile(options_.qpConfig);

    int localStatus = loader_.Load() == TILEXR_HCCP_LOADER_SUCCESS
        ? TILEXR_SUCCESS : TILEXR_ERROR_NOT_FOUND;
    int ret = AgreeInitStatus(localStatus);
    if (ret != TILEXR_SUCCESS) {
        Shutdown();
        return ret;
    }
    localStatus = OpenDevice();
    ret = AgreeInitStatus(localStatus);
    if (ret != TILEXR_SUCCESS) {
        Shutdown();
        return ret;
    }
    ret = AgreeRaOwnership();
    if (ret != TILEXR_SUCCESS) {
        Shutdown();
        return ret;
    }
    localStatus = BuildRoutes();
    ret = AgreeInitStatus(localStatus);
    if (ret != TILEXR_SUCCESS) {
        Shutdown();
        return ret;
    }
    localStatus = AgreeEidCount();
    ret = AgreeInitStatus(localStatus);
    if (ret != TILEXR_SUCCESS) {
        Shutdown();
        return ret;
    }
    localStatus = CreateContexts();
    ret = AgreeInitStatus(localStatus);
    if (ret != TILEXR_SUCCESS) {
        Shutdown();
        return ret;
    }
    localStatus = CreateQueues();
    ret = AgreeInitStatus(localStatus);
    if (ret != TILEXR_SUCCESS) {
        Shutdown();
        return ret;
    }
    localStatus = ImportQueues();
    ret = AgreeInitStatus(localStatus);
    if (ret != TILEXR_SUCCESS) {
        Shutdown();
        return ret;
    }
    localStatus = RefreshUDMAInfo();
    ret = AgreeInitStatus(localStatus);
    if (ret != TILEXR_SUCCESS) {
        Shutdown();
        return ret;
    }

    if (options_.enableFullmeshDomain) {
        const int fullmeshRet = InitFullmeshDomain();
        if (fullmeshRet != TILEXR_SUCCESS) {
            TILEXR_LOG(WARN) << "TileXR UDMA Fullmesh domain unavailable: "
                             << fullmeshRet;
        }
    }

    available_ = true;
    return TILEXR_SUCCESS;
}

int TileXRUDMATransport::InitFullmeshDomain()
{
    fullmeshAvailable_ = false;
    fullmeshValidPeerMask_ = 0U;
    if (options_.localRankSize <= 1 ||
        options_.localRankSize >
            static_cast<int>(TILEXR_UDMA_FULLMESH_SLOT_COUNT) ||
        options_.rankSize % options_.localRankSize != 0) {
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    const std::map<uint32_t, void*> previousContexts = ctxHandleByEid_;
    const std::map<uint32_t, void*> previousTokens = tokenHandleByEid_;

    int ret = BuildFullmeshRoutes();
    if (ret == TILEXR_SUCCESS) {
        ret = AgreeInitStatus(CreateContexts());
    }
    if (ret == TILEXR_SUCCESS) {
        ret = AgreeInitStatus(CreateFullmeshQueues());
    }
    if (ret == TILEXR_SUCCESS) {
        ret = AgreeInitStatus(ImportFullmeshQueues());
    }
    if (ret == TILEXR_SUCCESS) {
        ret = AgreeInitStatus(RefreshFullmeshInfo());
    }
    if (ret != TILEXR_SUCCESS) {
        int queueCleanupRet = CleanupFullmeshQueues();
        if (queueCleanupRet != TILEXR_SUCCESS) {
            queueCleanupRet = CleanupFullmeshQueues();
        }
        const int infoCleanupRet = FreeDeviceInfo(baseFullmeshInfoDev_);
        int contextCleanupRet = RollbackAddedContexts(
            previousContexts, previousTokens);
        if (contextCleanupRet != TILEXR_SUCCESS &&
            queueCleanupRet == TILEXR_SUCCESS) {
            contextCleanupRet = RollbackAddedContexts(
                previousContexts, previousTokens);
        }
        fullmeshInfoDev_ = nullptr;
        fullmeshInfoSize_ = 0U;
        fullmeshLocalRouteByPeer_.clear();
        fullmeshRemoteRouteByPeer_.clear();
        fullmeshValidPeerMask_ = 0U;
        if (queueCleanupRet != TILEXR_SUCCESS) {
            return queueCleanupRet;
        }
        if (infoCleanupRet != TILEXR_SUCCESS) {
            return infoCleanupRet;
        }
        if (contextCleanupRet != TILEXR_SUCCESS) {
            return contextCleanupRet;
        }
        return ret;
    }
    fullmeshAvailable_ = true;
    return TILEXR_SUCCESS;
}

int TileXRUDMATransport::BuildFullmeshRoutes()
{
    UDMARootInfo rootInfo {};
    std::vector<UDMATopologyEdge> topoEdges;
    std::string error;
    uint32_t localId = 0U;
    int localStatus = LoadUDMARootInfo(rootInfo, &error) &&
        ResolveUDMALocalId(rootInfo,
            static_cast<uint32_t>(options_.devId), localId) &&
        LoadUDMATopologyFromPath(rootInfo.topoPath, topoEdges, &error)
        ? TILEXR_SUCCESS : TILEXR_ERROR_NOT_FOUND;
    const auto localEids = rootInfo.eidByLocalId.find(localId);
    if (localStatus == TILEXR_SUCCESS &&
        localEids == rootInfo.eidByLocalId.end()) {
        localStatus = TILEXR_ERROR_NOT_FOUND;
    }
    if (localStatus == TILEXR_SUCCESS) {
        localEidByEid_.insert(
            localEids->second.begin(), localEids->second.end());
        eidCount_ = std::max(eidCount_, rootInfo.eidCount);
    }
    int ret = AgreeInitStatus(localStatus);
    if (ret != TILEXR_SUCCESS) {
        TILEXR_LOG(ERROR) << "TileXR Fullmesh RootInfo unavailable: " << error;
        return ret;
    }

    std::vector<uint32_t> allLocalIds(options_.rankSize);
    ret = options_.exchange->AllGather(
        &localId, 1, allLocalIds.data());
    if (ret != TILEXR_SUCCESS) {
        return ret;
    }

    fullmeshLocalRouteByPeer_.assign(options_.rankSize, UINT32_MAX);
    fullmeshRemoteRouteByPeer_.assign(options_.rankSize, UINT32_MAX);
    const int localNode = options_.rank / options_.localRankSize;
    std::map<uint32_t, bool> usedLocalEids;
    localStatus = TILEXR_SUCCESS;
    for (int peer = 0; peer < options_.rankSize; ++peer) {
        if (peer == options_.rank ||
            peer / options_.localRankSize != localNode) {
            continue;
        }
        uint32_t matchingEdges = 0U;
        bool directEdge = false;
        for (const auto& edge : topoEdges) {
            const bool forward = edge.localA == localId &&
                edge.localB == allLocalIds[peer];
            const bool reverse = edge.localB == localId &&
                edge.localA == allLocalIds[peer];
            if (!forward && !reverse) {
                continue;
            }
            ++matchingEdges;
            directEdge = edge.localAPorts.size() == 1U &&
                edge.localBPorts.size() == 1U;
        }
        uint32_t localEid = UINT32_MAX;
        const auto portCounts =
            rootInfo.portCountByEidByLocalId.find(localId);
        if (matchingEdges != 1U || !directEdge ||
            !ResolveUDMATopologyEid(rootInfo, topoEdges, localId,
                allLocalIds[peer], localEid) ||
            localEidByEid_.count(localEid) == 0U ||
            localEid >= eidCount_ ||
            portCounts == rootInfo.portCountByEidByLocalId.end() ||
            portCounts->second.count(localEid) == 0U ||
            portCounts->second.at(localEid) != 1U ||
            !usedLocalEids.emplace(localEid, true).second) {
            TILEXR_LOG(ERROR) << "TileXR Fullmesh route unavailable for peer "
                              << peer;
            localStatus = TILEXR_ERROR_NOT_FOUND;
            break;
        }
        fullmeshLocalRouteByPeer_[peer] = localEid;
    }
    ret = AgreeInitStatus(localStatus);
    if (ret != TILEXR_SUCCESS) {
        return ret;
    }

    std::vector<uint32_t> allRoutes(
        static_cast<size_t>(options_.rankSize) * options_.rankSize,
        UINT32_MAX);
    ret = options_.exchange->AllGather(fullmeshLocalRouteByPeer_.data(),
        fullmeshLocalRouteByPeer_.size(), allRoutes.data());
    if (ret != TILEXR_SUCCESS) {
        return ret;
    }

    uint32_t validMask = 0U;
    localStatus = TILEXR_SUCCESS;
    for (int peer = 0; peer < options_.rankSize; ++peer) {
        if (peer == options_.rank ||
            peer / options_.localRankSize != localNode) {
            continue;
        }
        const uint32_t remoteEid = allRoutes[
            static_cast<size_t>(peer) * options_.rankSize + options_.rank];
        const auto remotePortCounts = rootInfo.portCountByEidByLocalId.find(
            allLocalIds[peer]);
        if (remoteEid == UINT32_MAX || remoteEid >= eidCount_ ||
            remotePortCounts == rootInfo.portCountByEidByLocalId.end() ||
            remotePortCounts->second.count(remoteEid) == 0U ||
            remotePortCounts->second.at(remoteEid) != 1U) {
            localStatus = TILEXR_ERROR_NOT_FOUND;
            break;
        }
        fullmeshRemoteRouteByPeer_[peer] = remoteEid;
        validMask |= 1U << static_cast<uint32_t>(
            peer % options_.localRankSize);
    }
    const uint32_t localRank = static_cast<uint32_t>(
        options_.rank % options_.localRankSize);
    if (localStatus == TILEXR_SUCCESS &&
        validMask != UDMAFullmeshExpectedPeerMask(
            localRank, static_cast<uint32_t>(options_.localRankSize))) {
        localStatus = TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    ret = AgreeInitStatus(localStatus);
    if (ret == TILEXR_SUCCESS) {
        fullmeshValidPeerMask_ = validMask;
    }
    return ret;
}

int TileXRUDMATransport::AgreeInitStatus(int localStatus) const
{
    if (options_.exchange == nullptr || options_.rankSize <= 0 ||
        options_.rankSize > TILEXR_MAX_RANK_SIZE) {
        return TILEXR_ERROR_INTERNAL;
    }
    int32_t local = static_cast<int32_t>(localStatus);
    std::array<int32_t, TILEXR_MAX_RANK_SIZE> allStatus {};
    allStatus.fill(TILEXR_ERROR_INTERNAL);
    const int exchangeRet = options_.exchange->AllGather(&local, 1, allStatus.data());
    if (exchangeRet != TILEXR_SUCCESS) {
        return exchangeRet;
    }
    for (int rank = 0; rank < options_.rankSize; ++rank) {
        if (allStatus[rank] != TILEXR_SUCCESS) {
            return static_cast<int>(allStatus[rank]);
        }
    }
    return TILEXR_SUCCESS;
}

int TileXRUDMATransport::AgreeRaOwnership() const
{
    const int32_t local = raAttached_ ? 1 : 0;
    std::array<int32_t, TILEXR_MAX_RANK_SIZE> allAttached {};
    const int exchangeRet = options_.exchange->AllGather(&local, 1, allAttached.data());
    if (exchangeRet != TILEXR_SUCCESS) {
        return exchangeRet;
    }
    for (int rank = 0; rank < options_.rankSize; ++rank) {
        if (allAttached[rank] != local) {
            TILEXR_LOG(ERROR) << "TileXR UDMA RA ownership mismatch at rank " << rank;
            return TILEXR_ERROR_PARA_CHECK_FAIL;
        }
    }
    return TILEXR_SUCCESS;
}

int TileXRUDMATransport::AgreeEidCount() const
{
    if (options_.exchange == nullptr || options_.rankSize <= 0 ||
        options_.rankSize > TILEXR_MAX_RANK_SIZE || eidCount_ == 0) {
        return TILEXR_ERROR_INTERNAL;
    }
    std::array<uint32_t, TILEXR_MAX_RANK_SIZE> allEidCounts {};
    const int exchangeRet = options_.exchange->AllGather(&eidCount_, 1, allEidCounts.data());
    if (exchangeRet != TILEXR_SUCCESS) {
        return exchangeRet;
    }
    for (int rank = 0; rank < options_.rankSize; ++rank) {
        if (allEidCounts[rank] != eidCount_) {
            TILEXR_LOG(ERROR) << "TileXR UDMA EID count mismatch at rank " << rank
                              << ", local " << eidCount_ << ", remote " << allEidCounts[rank];
            return TILEXR_ERROR_PARA_CHECK_FAIL;
        }
    }
    return TILEXR_SUCCESS;
}

int TileXRUDMATransport::OpenDevice()
{
    logicDevId_ = static_cast<uint32_t>(options_.devId);
    UDMARootInfo rootInfo {};
    if (LoadUDMARootInfo(rootInfo)) {
        deviceIdOffset_ = rootInfo.deviceIdOffset;
    }

    ProcOpenArgs args {};
    args.procType = TSD_SUB_PROC_HCCP;
    char paramInfo[] = "--hdcType=18";
    ProcExtParam extParam {};
    extParam.paramInfo = paramInfo;
    extParam.paramLen = sizeof(paramInfo);
    args.extParamList = &extParam;
    args.extParamCnt = 1UL;
    args.subPid = &subPid_;
    auto tsdRet = loader_.TsdProcessOpen(logicDevId_, &args);
    if (tsdRet != 0) {
        TILEXR_LOG(WARN) << "TileXR UDMA TsdProcessOpen failed: " << tsdRet;
        return TILEXR_ERROR_INTERNAL;
    }
    tsdOpened_ = true;

    RaInitConfig initConfig {};
    initConfig.phyId = logicDevId_ + deviceIdOffset_;
    initConfig.nicPosition = NETWORK_OFFLINE;
    initConfig.hdcType = HDC_SERVICE_TYPE_RDMA_V2;
    initConfig.enableHdcAsync = 1;
    int ret = loader_.RaInit(&initConfig);
    if (ret != 0) {
        if (ret == TILEXR_HCCP_RA_ALREADY_INITIALIZED && AttachExistingRaEnabled()) {
            raAttached_ = true;
            TILEXR_LOG(INFO) << "TileXR UDMA attaching to existing RA initialization";
            return TILEXR_SUCCESS;
        }
        TILEXR_LOG(WARN) << "TileXR UDMA RaInit failed: " << ret;
        return TILEXR_ERROR_INTERNAL;
    }
    raInitialized_ = true;
    raAttached_ = false;
    return TILEXR_SUCCESS;
}

int TileXRUDMATransport::BuildRoutes()
{
    RaInfo info {};
    info.phyId = logicDevId_ + deviceIdOffset_;
    info.mode = NETWORK_OFFLINE;
    unsigned int eidNum = 0;
    int queryRet = loader_.RaGetDevEidInfoNum(info, &eidNum);
    int localStatus = queryRet == 0 && eidNum != 0 ? TILEXR_SUCCESS : TILEXR_ERROR_INTERNAL;
    std::vector<DevEidInfo> devEids(eidNum);
    if (localStatus == TILEXR_SUCCESS) {
        queryRet = loader_.RaGetDevEidInfoList(info, devEids.data(), &eidNum);
        if (queryRet != 0 || eidNum == 0) {
            localStatus = TILEXR_ERROR_INTERNAL;
        }
    }
    const int agreedStatus = AgreeInitStatus(localStatus);
    if (agreedStatus != TILEXR_SUCCESS) {
        TILEXR_LOG(WARN) << "TileXR UDMA EID discovery failed: " << queryRet << ", eidNum=" << eidNum;
        return agreedStatus;
    }
    devEids.resize(eidNum);
    eidCount_ = eidNum;
    for (const auto& eid : devEids) {
        eidCount_ = std::max(eidCount_, eid.eidIndex + 1U);
    }
    return explicitConfig_ ? BuildExplicitRoutes(devEids) : BuildLegacyRoutes(devEids);
}

int TileXRUDMATransport::BuildLegacyRoutes(const std::vector<DevEidInfo>& devEids)
{
    const uint32_t fallbackEid = devEids[0].eidIndex;
    uint32_t localId = static_cast<uint32_t>(options_.devId);
    bool rootReady = false;
    bool topoReady = false;
    UDMARootInfo rootInfo {};
    std::vector<UDMATopologyEdge> topoEdges;
    std::string error;
    if (LoadUDMARootInfo(rootInfo, &error)) {
        eidCount_ = std::max(eidCount_, rootInfo.eidCount);
        rootReady = ResolveUDMALocalId(
            rootInfo, static_cast<uint32_t>(options_.devId), localId);
        topoReady = rootReady &&
            LoadUDMATopologyFromPath(rootInfo.topoPath, topoEdges, &error);
    }
    if (rootReady) {
        const auto localEids = rootInfo.eidByLocalId.find(localId);
        if (localEids != rootInfo.eidByLocalId.end()) {
            localEidByEid_ = localEids->second;
        } else {
            rootReady = false;
            topoReady = false;
        }
    }
    if (!rootReady) {
        for (const auto& eid : devEids) {
            localEidByEid_[eid.eidIndex] = eid.eid;
        }
    }

    std::vector<uint32_t> allLocalIds(options_.rankSize);
    int ret = options_.exchange->AllGather(&localId, 1, allLocalIds.data());
    if (ret != TILEXR_SUCCESS) {
        return ret;
    }

    localRouteByPeerQp_.assign(static_cast<size_t>(options_.rankSize), fallbackEid);
    remoteRouteByPeerQp_.assign(static_cast<size_t>(options_.rankSize), fallbackEid);
    std::vector<int32_t> localRoutes(options_.rankSize, static_cast<int32_t>(fallbackEid));
    const int localNode = options_.localRankSize > 0
        ? options_.rank / options_.localRankSize : 0;
    for (int peer = 0; peer < options_.rankSize; ++peer) {
        if (peer == options_.rank) {
            continue;
        }
        uint32_t localEid = fallbackEid;
        const bool crossNode = options_.localRankSize > 0 &&
            peer / options_.localRankSize != localNode;
        const bool routeResolved = crossNode
            ? rootReady && ResolveUDMAAggregateEid(rootInfo, localId, localEid)
            : topoReady && ResolveUDMATopologyEid(
                rootInfo, topoEdges, localId, allLocalIds[peer], localEid);
        if (!routeResolved) {
            TILEXR_LOG(WARN) << "TileXR UDMA topology route resolution failed, falling back to EID "
                             << fallbackEid;
            localEid = fallbackEid;
        }
        peerLocalEid_[peer] = localEid;
        localRouteByPeerQp_[RouteIndex(peer, 0)] = localEid;
        localRoutes[peer] = static_cast<int32_t>(localEid);
    }

    std::vector<int32_t> allRoutes(static_cast<size_t>(options_.rankSize) * options_.rankSize, -1);
    ret = options_.exchange->AllGather(localRoutes.data(), localRoutes.size(), allRoutes.data());
    if (ret != TILEXR_SUCCESS) {
        return ret;
    }
    for (int peer = 0; peer < options_.rankSize; ++peer) {
        if (peer == options_.rank) {
            continue;
        }
        int32_t remoteEid = allRoutes[static_cast<size_t>(peer) * options_.rankSize + options_.rank];
        if (remoteEid < 0 || static_cast<uint32_t>(remoteEid) >= eidCount_) {
            remoteEid = static_cast<int32_t>(fallbackEid);
        }
        peerRemoteEid_[peer] = static_cast<uint32_t>(remoteEid);
        remoteRouteByPeerQp_[RouteIndex(peer, 0)] = static_cast<uint32_t>(remoteEid);
    }
    return TILEXR_SUCCESS;
}

int TileXRUDMATransport::BuildExplicitRoutes(const std::vector<DevEidInfo>& devEids)
{
    if (options_.rankSize <= 0 || static_cast<size_t>(options_.rankSize) >
        std::numeric_limits<size_t>::max() / qpCount_) {
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    const size_t entryCount = static_cast<size_t>(options_.rankSize) * qpCount_;
    if (static_cast<size_t>(options_.rankSize) > std::numeric_limits<size_t>::max() / entryCount) {
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }

    UDMARootInfo rootInfo {};
    std::vector<UDMATopologyEdge> topoEdges;
    std::string error;
    uint32_t localId = 0;
    int localStatus = LoadUDMARootInfo(rootInfo, &error) &&
        ResolveUDMALocalId(rootInfo, static_cast<uint32_t>(options_.devId), localId)
        ? TILEXR_SUCCESS : TILEXR_ERROR_NOT_FOUND;
    bool needsTopology = !sharedQp_ && options_.localRankSize > 1;
    for (const auto& rule : options_.qpConfig.routes) {
        needsTopology = needsTopology ||
            rule.selector == UDMAQpRouteSelector::TOPOLOGY ||
            rule.selector == UDMAQpRouteSelector::LOCAL_PEER_SLOT;
    }
    if (localStatus == TILEXR_SUCCESS && needsTopology &&
        !LoadUDMATopologyFromPath(rootInfo.topoPath, topoEdges, &error)) {
        localStatus = TILEXR_ERROR_NOT_FOUND;
    }
    const auto localEids = rootInfo.eidByLocalId.find(localId);
    if (localStatus == TILEXR_SUCCESS && localEids == rootInfo.eidByLocalId.end()) {
        localStatus = TILEXR_ERROR_NOT_FOUND;
    }
    if (localStatus == TILEXR_SUCCESS) {
        localEidByEid_ = localEids->second;
        eidCount_ = std::max(eidCount_, rootInfo.eidCount);
    }
    int ret = AgreeInitStatus(localStatus);
    if (ret != TILEXR_SUCCESS) {
        TILEXR_LOG(ERROR) << "TileXR explicit UDMA RootInfo preparation failed: " << error;
        return ret;
    }

    std::vector<uint32_t> allLocalIds(options_.rankSize);
    ret = options_.exchange->AllGather(&localId, 1, allLocalIds.data());
    if (ret != TILEXR_SUCCESS) {
        return ret;
    }

    localRouteByPeerQp_.assign(entryCount, UINT32_MAX);
    remoteRouteByPeerQp_.assign(entryCount, UINT32_MAX);
    localStatus = TILEXR_SUCCESS;
    const int localNode = options_.rank / options_.localRankSize;
    for (int peer = 0; peer < options_.rankSize && localStatus == TILEXR_SUCCESS; ++peer) {
        if (peer == options_.rank) {
            continue;
        }
        const bool sameNode = peer / options_.localRankSize == localNode;
        for (uint32_t qpIdx = 0; qpIdx < qpCount_; ++qpIdx) {
            const auto& rule = options_.qpConfig.routes[qpIdx];
            uint32_t localEid = UINT32_MAX;
            bool resolved = false;
            if (rule.selector == UDMAQpRouteSelector::LOCAL_PEER_SLOT) {
                const int localPeer = UDMAGroupedLocalPeerFromSlot(
                    options_.rank, options_.rankSize, rule.value,
                    static_cast<uint32_t>(options_.localRankSize));
                resolved = localPeer >= 0 && ResolveUDMATopologyEid(
                    rootInfo, topoEdges, localId, allLocalIds[localPeer], localEid);
            } else if ((!sharedQp_ && sameNode) ||
                       rule.selector == UDMAQpRouteSelector::TOPOLOGY) {
                resolved = ResolveUDMATopologyEid(
                    rootInfo, topoEdges, localId, allLocalIds[peer], localEid);
            } else {
                resolved = ResolveUDMAPortCountEid(rootInfo, localId, rule.value, localEid);
            }
            if (!resolved || localEidByEid_.count(localEid) == 0 || localEid >= eidCount_) {
                TILEXR_LOG(ERROR) << "TileXR explicit UDMA route unavailable for peer " << peer
                                  << ", qp " << qpIdx;
                localStatus = TILEXR_ERROR_NOT_FOUND;
                break;
            }
            localRouteByPeerQp_[RouteIndex(peer, qpIdx)] = localEid;
        }
    }
    for (uint32_t qpIdx = 0; qpIdx < qpCount_ && localStatus == TILEXR_SUCCESS; ++qpIdx) {
        uint32_t sharedLocalEid = UINT32_MAX;
        for (int peer = 0; peer < options_.rankSize; ++peer) {
            if (peer != options_.rank) {
                const uint32_t localEid = localRouteByPeerQp_[RouteIndex(peer, qpIdx)];
                if (sharedLocalEid == UINT32_MAX) {
                    sharedLocalEid = localEid;
                } else if (sharedQp_ && localEid != sharedLocalEid) {
                    TILEXR_LOG(ERROR) << "Shared UDMA QP " << qpIdx
                                      << " resolves to multiple local EIDs";
                    localStatus = TILEXR_ERROR_PARA_CHECK_FAIL;
                    break;
                }
            }
        }
        localRouteByPeerQp_[RouteIndex(options_.rank, qpIdx)] = sharedLocalEid;
    }
    ret = AgreeInitStatus(localStatus);
    if (ret != TILEXR_SUCCESS) {
        return ret;
    }

    std::vector<uint32_t> allRoutes(static_cast<size_t>(options_.rankSize) * entryCount, UINT32_MAX);
    ret = options_.exchange->AllGather(
        localRouteByPeerQp_.data(), localRouteByPeerQp_.size(), allRoutes.data());
    if (ret != TILEXR_SUCCESS) {
        return ret;
    }
    localStatus = TILEXR_SUCCESS;
    for (int peer = 0; peer < options_.rankSize && localStatus == TILEXR_SUCCESS; ++peer) {
        if (peer == options_.rank) {
            continue;
        }
        for (uint32_t qpIdx = 0; qpIdx < qpCount_; ++qpIdx) {
            uint32_t remoteQpIdx = qpIdx;
            remoteQpIdx = UDMARemoteQpIndex(groupedQpProfile_,
                options_.rank, peer, options_.rankSize, qpIdx,
                qpCount_, TILEXR_UDMA_GROUPED_AGGREGATE_QP_COUNT / 2U,
                static_cast<uint32_t>(options_.localRankSize));
            if (remoteQpIdx >= qpCount_ &&
                qpIdx >= TILEXR_UDMA_GROUPED_DIRECT_QP_BASE) {
                remoteQpIdx = 0U;
            }
            const uint32_t remoteEid = allRoutes[static_cast<size_t>(peer) * entryCount +
                RouteIndex(options_.rank, remoteQpIdx)];
            if (remoteEid == UINT32_MAX || remoteEid >= eidCount_) {
                localStatus = TILEXR_ERROR_NOT_FOUND;
                break;
            }
            remoteRouteByPeerQp_[RouteIndex(peer, qpIdx)] = remoteEid;
        }
        if (localStatus == TILEXR_SUCCESS) {
            peerLocalEid_[peer] = localRouteByPeerQp_[RouteIndex(peer, 0)];
            peerRemoteEid_[peer] = remoteRouteByPeerQp_[RouteIndex(peer, 0)];
        }
    }
    for (uint32_t qpIdx = 0; qpIdx < qpCount_ && localStatus == TILEXR_SUCCESS; ++qpIdx) {
        remoteRouteByPeerQp_[RouteIndex(options_.rank, qpIdx)] =
            localRouteByPeerQp_[RouteIndex(options_.rank, qpIdx)];
    }
    (void)devEids;
    return AgreeInitStatus(localStatus);
}

int TileXRUDMATransport::CreateContexts()
{
    std::map<uint32_t, bool> requiredEids;
    if (explicitConfig_) {
        for (uint32_t eidIndex : localRouteByPeerQp_) {
            if (eidIndex != UINT32_MAX) {
                requiredEids[eidIndex] = true;
            }
        }
    } else {
        for (const auto& route : peerLocalEid_) {
            requiredEids[route.second] = true;
        }
    }
    for (uint32_t eidIndex : fullmeshLocalRouteByPeer_) {
        if (eidIndex != UINT32_MAX) {
            requiredEids[eidIndex] = true;
        }
    }
    for (const auto& required : requiredEids) {
        const uint32_t eidIndex = required.first;
        if (ctxHandleByEid_.count(eidIndex) != 0) {
            continue;
        }

        RaInfo info {};
        info.phyId = logicDevId_ + deviceIdOffset_;
        info.mode = NETWORK_OFFLINE;
        unsigned int eidNum = 0;
        int ret = loader_.RaGetDevEidInfoNum(info, &eidNum);
        if (ret != 0 || eidNum == 0) {
            return TILEXR_ERROR_INTERNAL;
        }
        std::vector<DevEidInfo> infoList(eidNum);
        ret = loader_.RaGetDevEidInfoList(info, infoList.data(), &eidNum);
        if (ret != 0) {
            return TILEXR_ERROR_INTERNAL;
        }

        bool found = false;
        CtxInitAttr attr {};
        auto targetEidIt = localEidByEid_.find(eidIndex);
        for (unsigned int i = 0; i < eidNum; ++i) {
            bool matched = infoList[i].eidIndex == eidIndex;
            if (targetEidIt != localEidByEid_.end()) {
                matched = std::memcmp(infoList[i].eid.raw, targetEidIt->second.raw, sizeof(infoList[i].eid.raw)) == 0;
            }
            if (!matched) {
                continue;
            }
            attr.phyId = logicDevId_ + deviceIdOffset_;
            attr.ub.eid = infoList[i].eid;
            attr.ub.eidIndex = infoList[i].eidIndex;
            localEidByEid_[eidIndex] = infoList[i].eid;
            found = true;
            break;
        }
        if (!found) {
            return TILEXR_ERROR_INTERNAL;
        }

        CtxInitCfg cfg {};
        cfg.mode = NETWORK_OFFLINE;
        void* ctxHandle = nullptr;
        ret = loader_.RaCtxInit(&cfg, &attr, &ctxHandle);
        if (ret != 0 || ctxHandle == nullptr) {
            TILEXR_LOG(WARN) << "TileXR UDMA RaCtxInit failed: " << ret;
            return TILEXR_ERROR_INTERNAL;
        }
        ctxHandleByEid_[eidIndex] = ctxHandle;
        void* tokenHandle = nullptr;
        HccpTokenId tokenId {};
        ret = loader_.RaCtxTokenIdAlloc(ctxHandle, &tokenId, &tokenHandle);
        if (ret != 0 || tokenHandle == nullptr) {
            return TILEXR_ERROR_INTERNAL;
        }
        tokenHandleByEid_[eidIndex] = tokenHandle;
    }
    return ctxHandleByEid_.empty() ? TILEXR_ERROR_INTERNAL : TILEXR_SUCCESS;
}

int TileXRUDMATransport::AllocDeviceScalar(void** ptr, size_t bytes) const
{
    int ret = aclrtMalloc(ptr, bytes, ACL_MEM_MALLOC_HUGE_FIRST);
    if (ret != ACL_SUCCESS) {
        return TILEXR_ERROR_INTERNAL;
    }
    ret = aclrtMemset(*ptr, bytes, 0, bytes);
    if (ret != ACL_SUCCESS) {
        aclrtFree(*ptr);
        *ptr = nullptr;
        return TILEXR_ERROR_INTERNAL;
    }
    return TILEXR_SUCCESS;
}

int TileXRUDMATransport::FreeDeviceScalar(void*& ptr) const
{
    if (ptr == nullptr) {
        return TILEXR_SUCCESS;
    }
    const aclError ret = aclrtFree(ptr);
    if (ret != ACL_SUCCESS) {
        return TILEXR_ERROR_INTERNAL;
    }
    ptr = nullptr;
    return TILEXR_SUCCESS;
}

int TileXRUDMATransport::CreateQueues()
{
    if (sharedQp_) {
        return CreateSharedQueues();
    }
    return explicitConfig_ ? CreateExplicitQueues() : CreateLegacyQueues();
}

int TileXRUDMATransport::CreateLegacyQueues()
{
    for (const auto& ctxEntry : ctxHandleByEid_) {
        auto inserted = states_.emplace(ctxEntry.first, PerEidState {});
        auto& state = inserted.first->second;
        state.eidIndex = ctxEntry.first;
        state.ctxHandle = ctxEntry.second;
        state.tokenHandle = tokenHandleByEid_[ctxEntry.first];
        state.remoteQpHandles.assign(options_.rankSize, nullptr);
        state.tpnList.assign(options_.rankSize, 0);

        ChanInfoT chanInfo {};
        chanInfo.in.dataPlaneFlag.bs.poolCqCstm = 1;
        int ret = loader_.RaCtxChanCreate(state.ctxHandle, &chanInfo, &state.chanHandle);
        if (ret != 0) {
            return TILEXR_ERROR_INTERNAL;
        }

        state.cqInfo.in.chanHandle = state.chanHandle;
        state.cqInfo.in.depth = TILEXR_UDMA_CQ_DEPTH;
        state.cqInfo.in.ub.mode = JFC_MODE_USER_CTL_NORMAL;
        ret = loader_.RaCtxCqCreate(state.ctxHandle, &state.cqInfo, &state.cqHandle);
        if (ret != 0) {
            return TILEXR_ERROR_INTERNAL;
        }
        state.localCq.cqn = 0;
        state.localCq.bufAddr = state.cqInfo.out.bufAddr;
        state.localCq.baseBkShift = Log2Uint64(state.cqInfo.out.cqeSize);
        state.localCq.depth = state.cqInfo.in.depth;
        if (AllocDeviceScalar(&state.cqPiAddr, sizeof(uint32_t)) != TILEXR_SUCCESS ||
            AllocDeviceScalar(&state.cqCiAddr, sizeof(uint32_t)) != TILEXR_SUCCESS) {
            return TILEXR_ERROR_INTERNAL;
        }
        state.localCq.headAddr = reinterpret_cast<uintptr_t>(state.cqPiAddr);
        state.localCq.tailAddr = reinterpret_cast<uintptr_t>(state.cqCiAddr);
        state.localCq.dbMode = UDMADBMode::SW_DB;
        state.localCq.dbAddr = state.cqInfo.out.swdbAddr;

        QpCreateAttr qpAttr {};
        qpAttr.scqHandle = state.cqHandle;
        qpAttr.rcqHandle = state.cqHandle;
        qpAttr.srqHandle = state.cqHandle;
        qpAttr.sqDepth = TILEXR_UDMA_SQ_DEPTH;
        qpAttr.rqDepth = TILEXR_UDMA_RQ_DEPTH_DEFAULT;
        qpAttr.transportMode = CONN_RM;
        qpAttr.ub.mode = JETTY_MODE_USER_CTL_NORMAL;
        qpAttr.ub.flag.value = 1;
        qpAttr.ub.jfsFlag.value = 2;
        qpAttr.ub.tokenValue = TILEXR_UDMA_TOKEN_VALUE;
        qpAttr.ub.rnrRetry = 7;
        qpAttr.ub.extMode.piType = 0;
        qpAttr.ub.extMode.cstmFlag.bs.sqCstm = 0;
        qpAttr.ub.extMode.sqebbNum = TILEXR_UDMA_SQ_DEPTH;
        qpAttr.ub.tokenIdHandle = state.tokenHandle;
        ret = loader_.RaCtxQpCreate(state.ctxHandle, &qpAttr, &state.qpInfo, &state.qpHandle);
        if (ret != 0) {
            return TILEXR_ERROR_INTERNAL;
        }
        state.localWq.wqn = 0;
        state.localWq.bufAddr = state.qpInfo.ub.sqBuffVa;
        state.localWq.baseBkShift = Log2Uint64(state.qpInfo.ub.wqebbSize);
        state.localWq.depth = TILEXR_UDMA_SQ_BB_COUNT;
        if (AllocDeviceScalar(&state.sqPiAddr, sizeof(uint32_t)) != TILEXR_SUCCESS ||
            AllocDeviceScalar(&state.sqCiAddr, sizeof(uint32_t)) != TILEXR_SUCCESS ||
            AllocDeviceScalar(&state.wqeCntAddr, sizeof(uint32_t)) != TILEXR_SUCCESS ||
            AllocDeviceScalar(&state.amoAddr, sizeof(uint64_t)) != TILEXR_SUCCESS) {
            return TILEXR_ERROR_INTERNAL;
        }
        state.localWq.headAddr = reinterpret_cast<uintptr_t>(state.sqPiAddr);
        state.localWq.tailAddr = reinterpret_cast<uintptr_t>(state.sqCiAddr);
        state.localWq.dbMode = UDMADBMode::SW_DB;
        state.localWq.dbAddr = state.qpInfo.ub.dbAddr;
        state.localWq.wqeCntAddr = reinterpret_cast<uintptr_t>(state.wqeCntAddr);
        state.localWq.amoAddr = reinterpret_cast<uintptr_t>(state.amoAddr);
    }
    return states_.empty() ? TILEXR_ERROR_INTERNAL : TILEXR_SUCCESS;
}

int TileXRUDMATransport::CreateExplicitQueues()
{
    const size_t entryCount = static_cast<size_t>(options_.rankSize) * qpCount_;
    peerQpStates_.clear();
    peerQpStates_.resize(entryCount);
    for (int peer = 0; peer < options_.rankSize; ++peer) {
        if (peer == options_.rank) {
            continue;
        }
        for (uint32_t qpIdx = 0; qpIdx < qpCount_; ++qpIdx) {
            const size_t index = RouteIndex(peer, qpIdx);
            peerQpStates_[index].reset(new (std::nothrow) PerPeerQpState());
            if (peerQpStates_[index] == nullptr) {
                return TILEXR_ERROR_INTERNAL;
            }
            auto& state = *peerQpStates_[index];
            state.peer = peer;
            state.qpIdx = qpIdx;
            state.localEid = localRouteByPeerQp_[index];
            state.remoteEid = remoteRouteByPeerQp_[index];
            const auto ctxIt = ctxHandleByEid_.find(state.localEid);
            const auto tokenIt = tokenHandleByEid_.find(state.localEid);
            if (ctxIt == ctxHandleByEid_.end() || tokenIt == tokenHandleByEid_.end()) {
                return TILEXR_ERROR_NOT_FOUND;
            }
            state.ctxHandle = ctxIt->second;
            state.tokenHandle = tokenIt->second;

            ChanInfoT chanInfo {};
            chanInfo.in.dataPlaneFlag.bs.poolCqCstm = 1;
            int ret = loader_.RaCtxChanCreate(state.ctxHandle, &chanInfo, &state.chanHandle);
            if (ret != 0 || state.chanHandle == nullptr) {
                TILEXR_LOG(ERROR) << "TileXR UDMA channel creation failed for peer " << peer
                                  << ", qp " << qpIdx << ", ret " << ret;
                return TILEXR_ERROR_INTERNAL;
            }

            state.cqInfo.in.chanHandle = state.chanHandle;
            state.cqInfo.in.depth = TILEXR_UDMA_CQ_DEPTH;
            state.cqInfo.in.ub.mode = JFC_MODE_USER_CTL_NORMAL;
            ret = loader_.RaCtxCqCreate(state.ctxHandle, &state.cqInfo, &state.cqHandle);
            if (ret != 0 || state.cqHandle == nullptr) {
                TILEXR_LOG(ERROR) << "TileXR UDMA CQ creation failed for peer " << peer
                                  << ", qp " << qpIdx << ", ret " << ret;
                return TILEXR_ERROR_INTERNAL;
            }
            state.localCq.cqn = 0;
            state.localCq.bufAddr = state.cqInfo.out.bufAddr;
            state.localCq.baseBkShift = Log2Uint64(state.cqInfo.out.cqeSize);
            state.localCq.depth = state.cqInfo.in.depth;
            if (AllocDeviceScalar(&state.cqPiAddr, sizeof(uint32_t)) != TILEXR_SUCCESS ||
                AllocDeviceScalar(&state.cqCiAddr, sizeof(uint32_t)) != TILEXR_SUCCESS) {
                return TILEXR_ERROR_INTERNAL;
            }
            state.localCq.headAddr = reinterpret_cast<uintptr_t>(state.cqPiAddr);
            state.localCq.tailAddr = reinterpret_cast<uintptr_t>(state.cqCiAddr);
            state.localCq.dbMode = UDMADBMode::SW_DB;
            state.localCq.dbAddr = state.cqInfo.out.swdbAddr;

            QpCreateAttr qpAttr {};
            qpAttr.scqHandle = state.cqHandle;
            qpAttr.rcqHandle = state.cqHandle;
            qpAttr.srqHandle = state.cqHandle;
            qpAttr.sqDepth = TILEXR_UDMA_SQ_DEPTH;
            qpAttr.rqDepth = TILEXR_UDMA_RQ_DEPTH_DEFAULT;
            qpAttr.transportMode = CONN_RM;
            qpAttr.ub.mode = JETTY_MODE_USER_CTL_NORMAL;
            qpAttr.ub.flag.value = 1;
            qpAttr.ub.jfsFlag.value = 2;
            qpAttr.ub.tokenValue = TILEXR_UDMA_TOKEN_VALUE;
            qpAttr.ub.rnrRetry = 7;
            qpAttr.ub.extMode.piType = 0;
            qpAttr.ub.extMode.cstmFlag.bs.sqCstm = 0;
            qpAttr.ub.extMode.sqebbNum = TILEXR_UDMA_SQ_DEPTH;
            qpAttr.ub.tokenIdHandle = state.tokenHandle;
            ret = loader_.RaCtxQpCreate(
                state.ctxHandle, &qpAttr, &state.qpInfo, &state.qpHandle);
            if (ret != 0 || state.qpHandle == nullptr) {
                TILEXR_LOG(ERROR) << "TileXR UDMA QP creation failed for peer " << peer
                                  << ", qp " << qpIdx << ", ret " << ret;
                return TILEXR_ERROR_INTERNAL;
            }
            state.localWq.wqn = 0;
            state.localWq.bufAddr = state.qpInfo.ub.sqBuffVa;
            state.localWq.baseBkShift = Log2Uint64(state.qpInfo.ub.wqebbSize);
            state.localWq.depth = TILEXR_UDMA_SQ_BB_COUNT;
            if (AllocDeviceScalar(&state.sqPiAddr, sizeof(uint32_t)) != TILEXR_SUCCESS ||
                AllocDeviceScalar(&state.sqCiAddr, sizeof(uint32_t)) != TILEXR_SUCCESS ||
                AllocDeviceScalar(&state.wqeCntAddr, sizeof(uint32_t)) != TILEXR_SUCCESS ||
                AllocDeviceScalar(&state.amoAddr, sizeof(uint64_t)) != TILEXR_SUCCESS) {
                return TILEXR_ERROR_INTERNAL;
            }
            state.localWq.headAddr = reinterpret_cast<uintptr_t>(state.sqPiAddr);
            state.localWq.tailAddr = reinterpret_cast<uintptr_t>(state.sqCiAddr);
            state.localWq.dbMode = UDMADBMode::SW_DB;
            state.localWq.dbAddr = state.qpInfo.ub.dbAddr;
            state.localWq.wqeCntAddr = reinterpret_cast<uintptr_t>(state.wqeCntAddr);
            state.localWq.amoAddr = reinterpret_cast<uintptr_t>(state.amoAddr);
        }
    }
    return TILEXR_SUCCESS;
}

int TileXRUDMATransport::CreateFullmeshQueues()
{
    fullmeshQpStates_.clear();
    fullmeshQpStates_.resize(options_.rankSize);
    const int localNode = options_.rank / options_.localRankSize;
    for (int peer = 0; peer < options_.rankSize; ++peer) {
        if (peer == options_.rank ||
            peer / options_.localRankSize != localNode) {
            continue;
        }
        fullmeshQpStates_[peer].reset(
            new (std::nothrow) PerPeerQpState());
        if (fullmeshQpStates_[peer] == nullptr) {
            return TILEXR_ERROR_INTERNAL;
        }
        auto& state = *fullmeshQpStates_[peer];
        state.peer = peer;
        state.qpIdx = static_cast<uint32_t>(
            peer % options_.localRankSize);
        state.localEid = fullmeshLocalRouteByPeer_[peer];
        state.remoteEid = fullmeshRemoteRouteByPeer_[peer];
        const auto ctxIt = ctxHandleByEid_.find(state.localEid);
        const auto tokenIt = tokenHandleByEid_.find(state.localEid);
        if (ctxIt == ctxHandleByEid_.end() ||
            tokenIt == tokenHandleByEid_.end()) {
            return TILEXR_ERROR_NOT_FOUND;
        }
        state.ctxHandle = ctxIt->second;
        state.tokenHandle = tokenIt->second;

        ChanInfoT chanInfo {};
        chanInfo.in.dataPlaneFlag.bs.poolCqCstm = 1;
        int ret = loader_.RaCtxChanCreate(
            state.ctxHandle, &chanInfo, &state.chanHandle);
        if (ret != 0 || state.chanHandle == nullptr) {
            TILEXR_LOG(ERROR) << "TileXR Fullmesh channel creation failed for peer "
                              << peer << ", slot " << state.qpIdx
                              << ", ret " << ret;
            return TILEXR_ERROR_INTERNAL;
        }

        state.cqInfo.in.chanHandle = state.chanHandle;
        state.cqInfo.in.depth = TILEXR_UDMA_CQ_DEPTH;
        state.cqInfo.in.ub.mode = JFC_MODE_USER_CTL_NORMAL;
        ret = loader_.RaCtxCqCreate(
            state.ctxHandle, &state.cqInfo, &state.cqHandle);
        if (ret != 0 || state.cqHandle == nullptr) {
            TILEXR_LOG(ERROR) << "TileXR Fullmesh CQ creation failed for peer "
                              << peer << ", slot " << state.qpIdx
                              << ", ret " << ret;
            return TILEXR_ERROR_INTERNAL;
        }
        state.localCq.cqn = 0U;
        state.localCq.bufAddr = state.cqInfo.out.bufAddr;
        state.localCq.baseBkShift = Log2Uint64(state.cqInfo.out.cqeSize);
        state.localCq.depth = state.cqInfo.in.depth;
        if (AllocDeviceScalar(&state.cqPiAddr, sizeof(uint32_t)) !=
                TILEXR_SUCCESS ||
            AllocDeviceScalar(&state.cqCiAddr, sizeof(uint32_t)) !=
                TILEXR_SUCCESS) {
            return TILEXR_ERROR_INTERNAL;
        }
        state.localCq.headAddr = reinterpret_cast<uintptr_t>(state.cqPiAddr);
        state.localCq.tailAddr = reinterpret_cast<uintptr_t>(state.cqCiAddr);
        state.localCq.dbMode = UDMADBMode::SW_DB;
        state.localCq.dbAddr = state.cqInfo.out.swdbAddr;

        QpCreateAttr qpAttr {};
        qpAttr.scqHandle = state.cqHandle;
        qpAttr.rcqHandle = state.cqHandle;
        qpAttr.srqHandle = state.cqHandle;
        qpAttr.sqDepth = TILEXR_UDMA_SQ_DEPTH;
        qpAttr.rqDepth = TILEXR_UDMA_RQ_DEPTH_DEFAULT;
        qpAttr.transportMode = CONN_RM;
        qpAttr.ub.mode = JETTY_MODE_USER_CTL_NORMAL;
        qpAttr.ub.flag.value = 1;
        qpAttr.ub.jfsFlag.value = 2;
        qpAttr.ub.tokenValue = TILEXR_UDMA_TOKEN_VALUE;
        qpAttr.ub.rnrRetry = 7;
        qpAttr.ub.extMode.piType = 0;
        qpAttr.ub.extMode.cstmFlag.bs.sqCstm = 0;
        qpAttr.ub.extMode.sqebbNum = TILEXR_UDMA_SQ_DEPTH;
        qpAttr.ub.tokenIdHandle = state.tokenHandle;
        ret = loader_.RaCtxQpCreate(
            state.ctxHandle, &qpAttr, &state.qpInfo, &state.qpHandle);
        if (ret != 0 || state.qpHandle == nullptr) {
            TILEXR_LOG(ERROR) << "TileXR Fullmesh QP creation failed for peer "
                              << peer << ", slot " << state.qpIdx
                              << ", ret " << ret;
            return TILEXR_ERROR_INTERNAL;
        }
        state.localWq.wqn = 0U;
        state.localWq.bufAddr = state.qpInfo.ub.sqBuffVa;
        state.localWq.baseBkShift = Log2Uint64(state.qpInfo.ub.wqebbSize);
        state.localWq.depth = TILEXR_UDMA_SQ_BB_COUNT;
        if (AllocDeviceScalar(&state.sqPiAddr, sizeof(uint32_t)) !=
                TILEXR_SUCCESS ||
            AllocDeviceScalar(&state.sqCiAddr, sizeof(uint32_t)) !=
                TILEXR_SUCCESS ||
            AllocDeviceScalar(&state.wqeCntAddr, sizeof(uint32_t)) !=
                TILEXR_SUCCESS ||
            AllocDeviceScalar(&state.amoAddr, sizeof(uint64_t)) !=
                TILEXR_SUCCESS) {
            return TILEXR_ERROR_INTERNAL;
        }
        state.localWq.headAddr = reinterpret_cast<uintptr_t>(state.sqPiAddr);
        state.localWq.tailAddr = reinterpret_cast<uintptr_t>(state.sqCiAddr);
        state.localWq.dbMode = UDMADBMode::SW_DB;
        state.localWq.dbAddr = state.qpInfo.ub.dbAddr;
        state.localWq.wqeCntAddr =
            reinterpret_cast<uintptr_t>(state.wqeCntAddr);
        state.localWq.amoAddr = reinterpret_cast<uintptr_t>(state.amoAddr);
    }
    return fullmeshValidPeerMask_ == 0U ?
        TILEXR_ERROR_NOT_FOUND : TILEXR_SUCCESS;
}

int TileXRUDMATransport::CreateSharedQueues()
{
    sharedQpStates_.clear();
    sharedQpStates_.resize(qpCount_);
    for (uint32_t qpIdx = 0; qpIdx < qpCount_; ++qpIdx) {
        sharedQpStates_[qpIdx].reset(new (std::nothrow) SharedQpState());
        if (sharedQpStates_[qpIdx] == nullptr) {
            return TILEXR_ERROR_INTERNAL;
        }
        auto& state = *sharedQpStates_[qpIdx];
        state.qpIdx = qpIdx;
        state.localEid = localRouteByPeerQp_[RouteIndex(options_.rank, qpIdx)];
        const auto ctxIt = ctxHandleByEid_.find(state.localEid);
        const auto tokenIt = tokenHandleByEid_.find(state.localEid);
        if (ctxIt == ctxHandleByEid_.end() || tokenIt == tokenHandleByEid_.end()) {
            return TILEXR_ERROR_NOT_FOUND;
        }
        state.ctxHandle = ctxIt->second;
        state.tokenHandle = tokenIt->second;
        state.remoteQpHandles.assign(options_.rankSize, nullptr);
        state.tpnList.assign(options_.rankSize, 0);

        ChanInfoT chanInfo {};
        chanInfo.in.dataPlaneFlag.bs.poolCqCstm = 1;
        int ret = loader_.RaCtxChanCreate(state.ctxHandle, &chanInfo, &state.chanHandle);
        if (ret != 0 || state.chanHandle == nullptr) {
            TILEXR_LOG(ERROR) << "TileXR shared UDMA channel creation failed for qp "
                              << qpIdx << ", ret " << ret;
            return TILEXR_ERROR_INTERNAL;
        }

        state.cqInfo.in.chanHandle = state.chanHandle;
        state.cqInfo.in.depth = TILEXR_UDMA_CQ_DEPTH;
        state.cqInfo.in.ub.mode = JFC_MODE_USER_CTL_NORMAL;
        ret = loader_.RaCtxCqCreate(state.ctxHandle, &state.cqInfo, &state.cqHandle);
        if (ret != 0 || state.cqHandle == nullptr) {
            TILEXR_LOG(ERROR) << "TileXR shared UDMA CQ creation failed for qp "
                              << qpIdx << ", ret " << ret;
            return TILEXR_ERROR_INTERNAL;
        }
        state.localCq.cqn = 0;
        state.localCq.bufAddr = state.cqInfo.out.bufAddr;
        state.localCq.baseBkShift = Log2Uint64(state.cqInfo.out.cqeSize);
        state.localCq.depth = state.cqInfo.in.depth;
        if (AllocDeviceScalar(&state.cqPiAddr, sizeof(uint32_t)) != TILEXR_SUCCESS ||
            AllocDeviceScalar(&state.cqCiAddr, sizeof(uint32_t)) != TILEXR_SUCCESS) {
            return TILEXR_ERROR_INTERNAL;
        }
        state.localCq.headAddr = reinterpret_cast<uintptr_t>(state.cqPiAddr);
        state.localCq.tailAddr = reinterpret_cast<uintptr_t>(state.cqCiAddr);
        state.localCq.dbMode = UDMADBMode::SW_DB;
        state.localCq.dbAddr = state.cqInfo.out.swdbAddr;

        QpCreateAttr qpAttr {};
        qpAttr.scqHandle = state.cqHandle;
        qpAttr.rcqHandle = state.cqHandle;
        qpAttr.srqHandle = state.cqHandle;
        qpAttr.sqDepth = TILEXR_UDMA_SQ_DEPTH;
        qpAttr.rqDepth = TILEXR_UDMA_RQ_DEPTH_DEFAULT;
        qpAttr.transportMode = CONN_RM;
        qpAttr.ub.mode = JETTY_MODE_USER_CTL_NORMAL;
        qpAttr.ub.flag.value = 1;
        qpAttr.ub.jfsFlag.value = 2;
        qpAttr.ub.tokenValue = TILEXR_UDMA_TOKEN_VALUE;
        qpAttr.ub.rnrRetry = 7;
        qpAttr.ub.extMode.piType = 0;
        qpAttr.ub.extMode.cstmFlag.bs.sqCstm = 0;
        qpAttr.ub.extMode.sqebbNum = TILEXR_UDMA_SQ_DEPTH;
        qpAttr.ub.tokenIdHandle = state.tokenHandle;
        ret = loader_.RaCtxQpCreate(
            state.ctxHandle, &qpAttr, &state.qpInfo, &state.qpHandle);
        if (ret != 0 || state.qpHandle == nullptr) {
            TILEXR_LOG(ERROR) << "TileXR shared UDMA QP creation failed for qp "
                              << qpIdx << ", ret " << ret;
            return TILEXR_ERROR_INTERNAL;
        }
        state.localWq.wqn = 0;
        state.localWq.bufAddr = state.qpInfo.ub.sqBuffVa;
        state.localWq.baseBkShift = Log2Uint64(state.qpInfo.ub.wqebbSize);
        state.localWq.depth = TILEXR_UDMA_SQ_BB_COUNT;
        if (AllocDeviceScalar(&state.sqPiAddr, sizeof(uint32_t)) != TILEXR_SUCCESS ||
            AllocDeviceScalar(&state.sqCiAddr, sizeof(uint32_t)) != TILEXR_SUCCESS ||
            AllocDeviceScalar(&state.wqeCntAddr, sizeof(uint32_t)) != TILEXR_SUCCESS ||
            AllocDeviceScalar(&state.amoAddr, sizeof(uint64_t)) != TILEXR_SUCCESS) {
            return TILEXR_ERROR_INTERNAL;
        }
        state.localWq.headAddr = reinterpret_cast<uintptr_t>(state.sqPiAddr);
        state.localWq.tailAddr = reinterpret_cast<uintptr_t>(state.sqCiAddr);
        state.localWq.dbMode = UDMADBMode::SW_DB;
        state.localWq.dbAddr = state.qpInfo.ub.dbAddr;
        state.localWq.wqeCntAddr = reinterpret_cast<uintptr_t>(state.wqeCntAddr);
        state.localWq.amoAddr = reinterpret_cast<uintptr_t>(state.amoAddr);
    }
    return TILEXR_SUCCESS;
}

int TileXRUDMATransport::ImportQueues()
{
    if (sharedQp_) {
        return ImportSharedQueues();
    }
    return explicitConfig_ ? ImportExplicitQueues() : ImportLegacyQueues();
}

int TileXRUDMATransport::ImportLegacyQueues()
{
    std::vector<QpImportInfoT> localImports(eidCount_);
    std::vector<QpKeyT> localKeys(eidCount_);
    for (const auto& stateEntry : states_) {
        const auto& state = stateEntry.second;
        if (state.eidIndex >= eidCount_) {
            return TILEXR_ERROR_INTERNAL;
        }
        localImports[state.eidIndex].in.ub.mode = JETTY_IMPORT_MODE_NORMAL;
        localImports[state.eidIndex].in.ub.tokenValue = TILEXR_UDMA_TOKEN_VALUE;
        localImports[state.eidIndex].in.ub.policy = JETTY_GRP_POLICY_RR;
        localImports[state.eidIndex].in.ub.type = TARGET_TYPE_JETTY;
        localImports[state.eidIndex].in.ub.flag.bs.tokenPolicy = TOKEN_POLICY_PLAIN_TEXT;
        localImports[state.eidIndex].in.ub.tpType = 1;
        localKeys[state.eidIndex] = state.qpInfo.key;
    }

    std::vector<QpImportInfoT> allImports(options_.rankSize * eidCount_);
    int ret = options_.exchange->AllGather(localImports.data(), localImports.size(), allImports.data());
    if (ret != TILEXR_SUCCESS) {
        return ret;
    }
    std::vector<QpKeyT> allKeys(options_.rankSize * eidCount_);
    ret = options_.exchange->AllGather(localKeys.data(), localKeys.size(), allKeys.data());
    if (ret != TILEXR_SUCCESS) {
        return ret;
    }

    for (auto& stateEntry : states_) {
        auto& state = stateEntry.second;
        for (int peer = 0; peer < options_.rankSize; ++peer) {
            if (peer == options_.rank) {
                continue;
            }
            const auto localRoute = peerLocalEid_.find(peer);
            if (localRoute == peerLocalEid_.end() || localRoute->second != state.eidIndex) {
                continue;
            }
            const uint32_t remoteEid = peerRemoteEid_[peer];
            if (remoteEid >= eidCount_) {
                return TILEXR_ERROR_INTERNAL;
            }
            QpImportInfoT importInfo = allImports[peer * eidCount_ + remoteEid];
            importInfo.in.key = allKeys[peer * eidCount_ + remoteEid];
            ret = loader_.RaCtxQpImport(state.ctxHandle, &importInfo, &state.remoteQpHandles[peer]);
            if (ret != 0) {
                return TILEXR_ERROR_INTERNAL;
            }
            state.tpnList[peer] = importInfo.out.ub.tpn;
        }
    }
    return TILEXR_SUCCESS;
}

int TileXRUDMATransport::ImportExplicitQueues()
{
    const size_t entryCount = static_cast<size_t>(options_.rankSize) * qpCount_;
    std::vector<QpImportInfoT> localImports(entryCount);
    std::vector<QpKeyT> localKeys(entryCount);
    for (int peer = 0; peer < options_.rankSize; ++peer) {
        if (peer == options_.rank) {
            continue;
        }
        for (uint32_t qpIdx = 0; qpIdx < qpCount_; ++qpIdx) {
            const size_t index = RouteIndex(peer, qpIdx);
            const auto* state = GetPeerQpState(peer, qpIdx);
            if (state == nullptr || state->qpHandle == nullptr) {
                return TILEXR_ERROR_NOT_INITIALIZED;
            }
            localImports[index].in.ub.mode = JETTY_IMPORT_MODE_NORMAL;
            localImports[index].in.ub.tokenValue = TILEXR_UDMA_TOKEN_VALUE;
            localImports[index].in.ub.policy = JETTY_GRP_POLICY_RR;
            localImports[index].in.ub.type = TARGET_TYPE_JETTY;
            localImports[index].in.ub.flag.bs.tokenPolicy = TOKEN_POLICY_PLAIN_TEXT;
            localImports[index].in.ub.tpType = 1;
            localKeys[index] = state->qpInfo.key;
        }
    }

    if (static_cast<size_t>(options_.rankSize) >
        std::numeric_limits<size_t>::max() / entryCount) {
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    std::vector<QpImportInfoT> allImports(static_cast<size_t>(options_.rankSize) * entryCount);
    int ret = options_.exchange->AllGather(localImports.data(), localImports.size(), allImports.data());
    if (ret != TILEXR_SUCCESS) {
        return ret;
    }
    std::vector<QpKeyT> allKeys(static_cast<size_t>(options_.rankSize) * entryCount);
    ret = options_.exchange->AllGather(localKeys.data(), localKeys.size(), allKeys.data());
    if (ret != TILEXR_SUCCESS) {
        return ret;
    }

    for (int peer = 0; peer < options_.rankSize; ++peer) {
        if (peer == options_.rank) {
            continue;
        }
        for (uint32_t qpIdx = 0; qpIdx < qpCount_; ++qpIdx) {
            auto* state = GetPeerQpState(peer, qpIdx);
            if (state == nullptr || state->ctxHandle == nullptr) {
                return TILEXR_ERROR_NOT_INITIALIZED;
            }
            const size_t remoteIndex = static_cast<size_t>(peer) * entryCount +
                RouteIndex(options_.rank, qpIdx);
            QpImportInfoT importInfo = allImports[remoteIndex];
            importInfo.in.key = allKeys[remoteIndex];
            ret = loader_.RaCtxQpImport(
                state->ctxHandle, &importInfo, &state->remoteQpHandle);
            if (ret != 0 || state->remoteQpHandle == nullptr) {
                TILEXR_LOG(ERROR) << "TileXR UDMA QP import failed for peer " << peer
                                  << ", qp " << qpIdx << ", ret " << ret;
                return TILEXR_ERROR_INTERNAL;
            }
            state->tpn = importInfo.out.ub.tpn;
        }
    }
    return TILEXR_SUCCESS;
}

int TileXRUDMATransport::ImportFullmeshQueues()
{
    std::vector<QpImportInfoT> localImports(
        TILEXR_UDMA_FULLMESH_SLOT_COUNT);
    std::vector<QpKeyT> localKeys(TILEXR_UDMA_FULLMESH_SLOT_COUNT);
    const int localNode = options_.rank / options_.localRankSize;
    for (int peer = 0; peer < options_.rankSize; ++peer) {
        if (peer == options_.rank ||
            peer / options_.localRankSize != localNode) {
            continue;
        }
        const auto* state = GetFullmeshQpState(peer);
        if (state == nullptr || state->qpHandle == nullptr ||
            state->qpIdx >= TILEXR_UDMA_FULLMESH_SLOT_COUNT) {
            return TILEXR_ERROR_NOT_INITIALIZED;
        }
        QpImportInfoT& importInfo = localImports[state->qpIdx];
        importInfo.in.ub.mode = JETTY_IMPORT_MODE_NORMAL;
        importInfo.in.ub.tokenValue = TILEXR_UDMA_TOKEN_VALUE;
        importInfo.in.ub.policy = JETTY_GRP_POLICY_RR;
        importInfo.in.ub.type = TARGET_TYPE_JETTY;
        importInfo.in.ub.flag.bs.tokenPolicy = TOKEN_POLICY_PLAIN_TEXT;
        importInfo.in.ub.tpType = 1;
        localKeys[state->qpIdx] = state->qpInfo.key;
    }

    const size_t allEntryCount = static_cast<size_t>(options_.rankSize) *
        TILEXR_UDMA_FULLMESH_SLOT_COUNT;
    std::vector<QpImportInfoT> allImports(allEntryCount);
    int ret = options_.exchange->AllGather(localImports.data(),
        localImports.size(), allImports.data());
    if (ret != TILEXR_SUCCESS) {
        return ret;
    }
    std::vector<QpKeyT> allKeys(allEntryCount);
    ret = options_.exchange->AllGather(localKeys.data(),
        localKeys.size(), allKeys.data());
    if (ret != TILEXR_SUCCESS) {
        return ret;
    }

    const uint32_t remoteSlot = static_cast<uint32_t>(
        options_.rank % options_.localRankSize);
    for (int peer = 0; peer < options_.rankSize; ++peer) {
        if (peer == options_.rank ||
            peer / options_.localRankSize != localNode) {
            continue;
        }
        auto* state = GetFullmeshQpState(peer);
        if (state == nullptr || state->ctxHandle == nullptr) {
            return TILEXR_ERROR_NOT_INITIALIZED;
        }
        const size_t remoteIndex = static_cast<size_t>(peer) *
            TILEXR_UDMA_FULLMESH_SLOT_COUNT + remoteSlot;
        QpImportInfoT importInfo = allImports[remoteIndex];
        importInfo.in.key = allKeys[remoteIndex];
        ret = loader_.RaCtxQpImport(
            state->ctxHandle, &importInfo, &state->remoteQpHandle);
        if (ret != 0 || state->remoteQpHandle == nullptr) {
            TILEXR_LOG(ERROR) << "TileXR Fullmesh QP import failed for peer "
                              << peer << ", local slot " << state->qpIdx
                              << ", remote slot " << remoteSlot
                              << ", ret " << ret;
            return TILEXR_ERROR_INTERNAL;
        }
        state->tpn = importInfo.out.ub.tpn;
    }
    return TILEXR_SUCCESS;
}

int TileXRUDMATransport::ImportSharedQueues()
{
    std::vector<QpImportInfoT> localImports(qpCount_);
    std::vector<QpKeyT> localKeys(qpCount_);
    for (uint32_t qpIdx = 0; qpIdx < qpCount_; ++qpIdx) {
        const auto* state = GetSharedQpState(qpIdx);
        if (state == nullptr || state->qpHandle == nullptr) {
            return TILEXR_ERROR_NOT_INITIALIZED;
        }
        localImports[qpIdx].in.ub.mode = JETTY_IMPORT_MODE_NORMAL;
        localImports[qpIdx].in.ub.tokenValue = TILEXR_UDMA_TOKEN_VALUE;
        localImports[qpIdx].in.ub.policy = JETTY_GRP_POLICY_RR;
        localImports[qpIdx].in.ub.type = TARGET_TYPE_JETTY;
        localImports[qpIdx].in.ub.flag.bs.tokenPolicy = TOKEN_POLICY_PLAIN_TEXT;
        localImports[qpIdx].in.ub.tpType = 1;
        localKeys[qpIdx] = state->qpInfo.key;
    }

    if (static_cast<size_t>(options_.rankSize) >
        std::numeric_limits<size_t>::max() / qpCount_) {
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    const size_t allEntryCount = static_cast<size_t>(options_.rankSize) * qpCount_;
    std::vector<QpImportInfoT> allImports(allEntryCount);
    int ret = options_.exchange->AllGather(
        localImports.data(), localImports.size(), allImports.data());
    if (ret != TILEXR_SUCCESS) {
        return ret;
    }
    std::vector<QpKeyT> allKeys(allEntryCount);
    ret = options_.exchange->AllGather(
        localKeys.data(), localKeys.size(), allKeys.data());
    if (ret != TILEXR_SUCCESS) {
        return ret;
    }

    for (uint32_t qpIdx = 0; qpIdx < qpCount_; ++qpIdx) {
        auto* state = GetSharedQpState(qpIdx);
        if (state == nullptr || state->ctxHandle == nullptr ||
            state->remoteQpHandles.size() != static_cast<size_t>(options_.rankSize) ||
            state->tpnList.size() != static_cast<size_t>(options_.rankSize)) {
            return TILEXR_ERROR_NOT_INITIALIZED;
        }
        for (int peer = 0; peer < options_.rankSize; ++peer) {
            if (peer == options_.rank) {
                continue;
            }
            uint32_t remoteQpIdx = qpIdx;
            if (groupedQpProfile_) {
                remoteQpIdx = UDMARemoteQpIndex(true,
                    options_.rank, peer, options_.rankSize, qpIdx,
                    qpCount_, TILEXR_UDMA_GROUPED_AGGREGATE_QP_COUNT / 2U,
                    static_cast<uint32_t>(options_.localRankSize));
                if (remoteQpIdx >= qpCount_) {
                    if (qpIdx >= TILEXR_UDMA_GROUPED_DIRECT_QP_BASE) {
                        continue;
                    }
                    return TILEXR_ERROR_INTERNAL;
                }
            }
            const size_t remoteIndex =
                static_cast<size_t>(peer) * qpCount_ + remoteQpIdx;
            QpImportInfoT importInfo = allImports[remoteIndex];
            importInfo.in.key = allKeys[remoteIndex];
            ret = loader_.RaCtxQpImport(
                state->ctxHandle, &importInfo, &state->remoteQpHandles[peer]);
            if (ret != 0 || state->remoteQpHandles[peer] == nullptr) {
                TILEXR_LOG(ERROR) << "TileXR shared UDMA QP import failed for peer "
                                  << peer << ", qp " << qpIdx << ", ret " << ret;
                return TILEXR_ERROR_INTERNAL;
            }
            state->tpnList[peer] = importInfo.out.ub.tpn;
        }
    }
    return TILEXR_SUCCESS;
}

uint32_t TileXRUDMATransport::FallbackLocalEid() const
{
    if (!states_.empty()) {
        return states_.begin()->first;
    }
    return 0;
}

size_t TileXRUDMATransport::RouteIndex(int peer, uint32_t qpIdx) const
{
    return static_cast<size_t>(peer) * qpCount_ + qpIdx;
}

size_t TileXRUDMATransport::FullmeshEntryIndex(
    int peer, uint32_t slot) const
{
    return static_cast<size_t>(peer) *
        TILEXR_UDMA_FULLMESH_SLOT_COUNT + slot;
}

const TileXRUDMATransport::PerPeerQpState* TileXRUDMATransport::GetPeerQpState(
    int peer, uint32_t qpIdx) const
{
    if (peer < 0 || peer >= options_.rankSize || qpIdx >= qpCount_) {
        return nullptr;
    }
    const size_t index = RouteIndex(peer, qpIdx);
    return index < peerQpStates_.size() ? peerQpStates_[index].get() : nullptr;
}

TileXRUDMATransport::PerPeerQpState* TileXRUDMATransport::GetPeerQpState(
    int peer, uint32_t qpIdx)
{
    return const_cast<PerPeerQpState*>(
        static_cast<const TileXRUDMATransport*>(this)->GetPeerQpState(peer, qpIdx));
}

const TileXRUDMATransport::PerPeerQpState*
TileXRUDMATransport::GetFullmeshQpState(int peer) const
{
    if (peer < 0 || peer >= options_.rankSize ||
        static_cast<size_t>(peer) >= fullmeshQpStates_.size()) {
        return nullptr;
    }
    return fullmeshQpStates_[peer].get();
}

TileXRUDMATransport::PerPeerQpState*
TileXRUDMATransport::GetFullmeshQpState(int peer)
{
    return const_cast<PerPeerQpState*>(
        static_cast<const TileXRUDMATransport*>(this)->
            GetFullmeshQpState(peer));
}

const TileXRUDMATransport::PerPeerQpState* TileXRUDMATransport::GetFallbackQpState(
    uint32_t qpIdx) const
{
    if (qpIdx >= qpCount_) {
        return nullptr;
    }
    for (int peer = 0; peer < options_.rankSize; ++peer) {
        if (peer != options_.rank) {
            const auto* state = GetPeerQpState(peer, qpIdx);
            if (state != nullptr) {
                return state;
            }
        }
    }
    return nullptr;
}

const TileXRUDMATransport::SharedQpState* TileXRUDMATransport::GetSharedQpState(
    uint32_t qpIdx) const
{
    return qpIdx < sharedQpStates_.size() ? sharedQpStates_[qpIdx].get() : nullptr;
}

TileXRUDMATransport::SharedQpState* TileXRUDMATransport::GetSharedQpState(
    uint32_t qpIdx)
{
    return const_cast<SharedQpState*>(
        static_cast<const TileXRUDMATransport*>(this)->GetSharedQpState(qpIdx));
}

int TileXRUDMATransport::RefreshUDMAInfo()
{
    const size_t entryCount = static_cast<size_t>(options_.rankSize) * qpCount_;
    if (eidCount_ == 0 || entryCount == 0 ||
        (sharedQp_ ? sharedQpStates_.empty() :
            (explicitConfig_ ? peerQpStates_.empty() : states_.empty())) ||
        static_cast<size_t>(options_.rankSize) >
            std::numeric_limits<size_t>::max() / eidCount_) {
        return TILEXR_ERROR_INTERNAL;
    }
    const size_t eidEntryCount = static_cast<size_t>(options_.rankSize) * eidCount_;
    if (eidEntryCount > std::numeric_limits<size_t>::max() / sizeof(HccpEid)) {
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    const size_t eidTableBytes = eidEntryCount * sizeof(HccpEid);
    int localStatus = TILEXR_SUCCESS;
    if (eidTableDev_ == nullptr) {
        const int allocRet = aclrtMalloc(
            reinterpret_cast<void**>(&eidTableDev_), eidTableBytes, ACL_MEM_MALLOC_HUGE_FIRST);
        if (allocRet != ACL_SUCCESS) {
            localStatus = TILEXR_ERROR_INTERNAL;
        }
    }
    int ret = AgreeInitStatus(localStatus);
    if (ret != TILEXR_SUCCESS) {
        return ret;
    }

    std::vector<HccpEid> localEids(eidCount_);
    for (const auto& eidEntry : localEidByEid_) {
        if (eidEntry.first < eidCount_) {
            localEids[eidEntry.first] = SwapEidForDevice(eidEntry.second);
        }
    }

    std::vector<HccpEid> allEids(eidEntryCount);
    ret = options_.exchange->AllGather(localEids.data(), localEids.size(), allEids.data());
    if (ret != TILEXR_SUCCESS) {
        return ret;
    }
    ret = aclrtMemcpy(
        eidTableDev_, eidTableBytes, allEids.data(), eidTableBytes, ACL_MEMCPY_HOST_TO_DEVICE);
    if (ret != ACL_SUCCESS) {
        return TILEXR_ERROR_INTERNAL;
    }

    std::vector<UDMAWQCtx> sq;
    std::vector<UDMAWQCtx> rq;
    std::vector<UDMACQCtx> scq;
    std::vector<UDMACQCtx> rcq;
    std::vector<UDMAMemInfo> mem(entryCount);
    ret = BuildQueueImages(sq, rq, scq, rcq, mem);
    if (ret != TILEXR_SUCCESS) {
        return ret;
    }

    if (baseUDMAInfoDev_ == nullptr) {
        const size_t oneEntrySize =
            2 * sizeof(UDMAWQCtx) + 2 * sizeof(UDMACQCtx) + sizeof(UDMAMemInfo);
        if (entryCount > (UINT32_MAX - sizeof(UDMAInfo)) / oneEntrySize) {
            return TILEXR_ERROR_PARA_CHECK_FAIL;
        }
        udmaInfoSize_ = static_cast<uint32_t>(sizeof(UDMAInfo) + oneEntrySize * entryCount);
        ret = aclrtMalloc(reinterpret_cast<void**>(&baseUDMAInfoDev_), udmaInfoSize_, ACL_MEM_MALLOC_HUGE_FIRST);
        if (ret != ACL_SUCCESS) {
            return TILEXR_ERROR_INTERNAL;
        }
    }

    UDMAInfo info {};
    std::vector<uint8_t> image;
    ret = BuildUDMAInfoImage(
        reinterpret_cast<uintptr_t>(baseUDMAInfoDev_), qpCount_, sq, rq, scq, rcq, mem, info, image);
    if (ret != TILEXR_UDMA_LAYOUT_SUCCESS) {
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    ret = aclrtMemcpy(baseUDMAInfoDev_, udmaInfoSize_, image.data(), image.size(), ACL_MEMCPY_HOST_TO_DEVICE);
    if (ret != ACL_SUCCESS) {
        return TILEXR_ERROR_INTERNAL;
    }
    udmaInfoDev_ = baseUDMAInfoDev_;
    return TILEXR_SUCCESS;
}

int TileXRUDMATransport::RefreshFullmeshInfo()
{
    const size_t entryCount = static_cast<size_t>(options_.rankSize) *
        TILEXR_UDMA_FULLMESH_SLOT_COUNT;
    if (eidTableDev_ == nullptr || fullmeshQpStates_.empty()) {
        return TILEXR_ERROR_NOT_INITIALIZED;
    }
    std::vector<UDMAWQCtx> sq;
    std::vector<UDMAWQCtx> rq;
    std::vector<UDMACQCtx> scq;
    std::vector<UDMACQCtx> rcq;
    std::vector<UDMAMemInfo> mem(entryCount);
    int ret = BuildFullmeshQueueImages(sq, rq, scq, rcq, mem);
    if (ret != TILEXR_SUCCESS) {
        return ret;
    }

    if (baseFullmeshInfoDev_ == nullptr) {
        const size_t oneEntrySize = 2U * sizeof(UDMAWQCtx) +
            2U * sizeof(UDMACQCtx) + sizeof(UDMAMemInfo);
        if (entryCount >
            (UINT32_MAX - sizeof(UDMAInfo)) / oneEntrySize) {
            return TILEXR_ERROR_PARA_CHECK_FAIL;
        }
        fullmeshInfoSize_ = static_cast<uint32_t>(
            sizeof(UDMAInfo) + oneEntrySize * entryCount);
        ret = aclrtMalloc(reinterpret_cast<void**>(&baseFullmeshInfoDev_),
            fullmeshInfoSize_, ACL_MEM_MALLOC_HUGE_FIRST);
        if (ret != ACL_SUCCESS) {
            return TILEXR_ERROR_INTERNAL;
        }
    }

    UDMAInfo info {};
    std::vector<uint8_t> image;
    ret = BuildUDMAInfoImage(
        reinterpret_cast<uintptr_t>(baseFullmeshInfoDev_),
        TILEXR_UDMA_FULLMESH_SLOT_COUNT,
        sq, rq, scq, rcq, mem, info, image);
    if (ret != TILEXR_UDMA_LAYOUT_SUCCESS) {
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    ret = aclrtMemcpy(baseFullmeshInfoDev_, fullmeshInfoSize_,
        image.data(), image.size(), ACL_MEMCPY_HOST_TO_DEVICE);
    if (ret != ACL_SUCCESS) {
        return TILEXR_ERROR_INTERNAL;
    }
    fullmeshInfoDev_ = baseFullmeshInfoDev_;
    return TILEXR_SUCCESS;
}

int TileXRUDMATransport::BuildFullmeshQueueImages(
    std::vector<UDMAWQCtx>& sq, std::vector<UDMAWQCtx>& rq,
    std::vector<UDMACQCtx>& scq, std::vector<UDMACQCtx>& rcq,
    std::vector<UDMAMemInfo>& mem) const
{
    const size_t entryCount = static_cast<size_t>(options_.rankSize) *
        TILEXR_UDMA_FULLMESH_SLOT_COUNT;
    if (eidTableDev_ == nullptr || mem.size() != entryCount) {
        return TILEXR_ERROR_INTERNAL;
    }
    sq.assign(entryCount, UDMAWQCtx {});
    rq.assign(entryCount, UDMAWQCtx {});
    scq.assign(entryCount, UDMACQCtx {});
    rcq.assign(entryCount, UDMACQCtx {});

    const int localNode = options_.rank / options_.localRankSize;
    uint32_t populatedMask = 0U;
    for (int peer = 0; peer < options_.rankSize; ++peer) {
        if (peer == options_.rank ||
            peer / options_.localRankSize != localNode) {
            continue;
        }
        const PerPeerQpState* state = GetFullmeshQpState(peer);
        const uint32_t slot = static_cast<uint32_t>(
            peer % options_.localRankSize);
        if (state == nullptr || state->qpIdx != slot ||
            state->remoteEid >= eidCount_) {
            return TILEXR_ERROR_NOT_INITIALIZED;
        }
        const size_t index = FullmeshEntryIndex(peer, slot);
        sq[index] = state->localWq;
        rq[index] = state->localWq;
        scq[index] = state->localCq;
        rcq[index] = state->localCq;
        mem[index].eidAddr = reinterpret_cast<uint64_t>(
            eidTableDev_ +
            (static_cast<size_t>(peer) * eidCount_ + state->remoteEid) *
                sizeof(HccpEid));
        populatedMask |= 1U << slot;
    }
    return populatedMask == fullmeshValidPeerMask_ ?
        TILEXR_SUCCESS : TILEXR_ERROR_NOT_INITIALIZED;
}

int TileXRUDMATransport::BuildQueueImages(
    std::vector<UDMAWQCtx>& sq, std::vector<UDMAWQCtx>& rq,
    std::vector<UDMACQCtx>& scq, std::vector<UDMACQCtx>& rcq,
    std::vector<UDMAMemInfo>& mem) const
{
    const size_t entryCount = static_cast<size_t>(options_.rankSize) * qpCount_;
    if (eidTableDev_ == nullptr || mem.size() != entryCount) {
        return TILEXR_ERROR_INTERNAL;
    }
    sq.resize(entryCount);
    rq.resize(entryCount);
    scq.resize(entryCount);
    rcq.resize(entryCount);

    if (sharedQp_) {
        for (int rank = 0; rank < options_.rankSize; ++rank) {
            for (uint32_t qpIdx = 0; qpIdx < qpCount_; ++qpIdx) {
                const size_t index = RouteIndex(rank, qpIdx);
                const SharedQpState* state = GetSharedQpState(qpIdx);
                if (state == nullptr) {
                    return TILEXR_ERROR_NOT_INITIALIZED;
                }
                sq[index] = state->localWq;
                rq[index] = state->localWq;
                scq[index] = state->localCq;
                rcq[index] = state->localCq;
                const uint32_t remoteEid = remoteRouteByPeerQp_[index];
                if (remoteEid >= eidCount_) {
                    return TILEXR_ERROR_NOT_FOUND;
                }
                mem[index].eidAddr = reinterpret_cast<uint64_t>(
                    eidTableDev_ + (static_cast<size_t>(rank) * eidCount_ +
                        remoteEid) * sizeof(HccpEid));
            }
        }
        return TILEXR_SUCCESS;
    }

    if (explicitConfig_) {
        for (int rank = 0; rank < options_.rankSize; ++rank) {
            for (uint32_t qpIdx = 0; qpIdx < qpCount_; ++qpIdx) {
                const size_t index = RouteIndex(rank, qpIdx);
                const PerPeerQpState* state = rank == options_.rank
                    ? GetFallbackQpState(qpIdx) : GetPeerQpState(rank, qpIdx);
                if (state == nullptr) {
                    return TILEXR_ERROR_NOT_INITIALIZED;
                }
                sq[index] = state->localWq;
                rq[index] = state->localWq;
                scq[index] = state->localCq;
                rcq[index] = state->localCq;
                const uint32_t remoteEid = remoteRouteByPeerQp_[index];
                if (remoteEid >= eidCount_) {
                    return TILEXR_ERROR_NOT_FOUND;
                }
                mem[index].eidAddr = reinterpret_cast<uint64_t>(
                    eidTableDev_ + (static_cast<size_t>(rank) * eidCount_ + remoteEid) * sizeof(HccpEid));
            }
        }
        return TILEXR_SUCCESS;
    }

    const uint32_t fallbackEid = FallbackLocalEid();
    auto fallbackIt = states_.find(fallbackEid);
    if (fallbackIt == states_.end()) {
        return TILEXR_ERROR_NOT_INITIALIZED;
    }
    for (int rank = 0; rank < options_.rankSize; ++rank) {
        uint32_t localEid = fallbackEid;
        uint32_t remoteEid = fallbackEid;
        if (rank != options_.rank) {
            localEid = peerLocalEid_.at(rank);
            remoteEid = peerRemoteEid_.at(rank);
        }
        auto stateIt = states_.find(localEid);
        if (stateIt == states_.end()) {
            stateIt = fallbackIt;
        }
        const size_t index = RouteIndex(rank, 0);
        sq[index] = stateIt->second.localWq;
        rq[index] = stateIt->second.localWq;
        scq[index] = stateIt->second.localCq;
        rcq[index] = stateIt->second.localCq;
        mem[index].eidAddr = reinterpret_cast<uint64_t>(
            eidTableDev_ + (static_cast<size_t>(rank) * eidCount_ + remoteEid) * sizeof(HccpEid));
    }
    return TILEXR_SUCCESS;
}

int TileXRUDMATransport::BuildRegistrationUDMAInfo(RegistrationState& registration)
{
    const size_t entryCount = static_cast<size_t>(options_.rankSize) * qpCount_;
    if (eidCount_ == 0 || eidTableDev_ == nullptr ||
        registration.memoryImage.size() != entryCount ||
        registration.qpBindings.size() != qpCount_) {
        return TILEXR_ERROR_INTERNAL;
    }

    std::vector<UDMAWQCtx> sq;
    std::vector<UDMAWQCtx> rq;
    std::vector<UDMACQCtx> scq;
    std::vector<UDMACQCtx> rcq;
    int ret = BuildQueueImages(sq, rq, scq, rcq, registration.memoryImage);
    if (ret != TILEXR_SUCCESS) {
        return ret;
    }

    const uint32_t fallbackEid = FallbackLocalEid();
    for (int peer = 0; peer < options_.rankSize; ++peer) {
        for (uint32_t qpIdx = 0; qpIdx < qpCount_; ++qpIdx) {
            const size_t index = RouteIndex(peer, qpIdx);
            uint32_t localEid = fallbackEid;
            if (explicitConfig_) {
                if (peer == options_.rank) {
                    if (sharedQp_) {
                        const auto* state = GetSharedQpState(qpIdx);
                        if (state == nullptr) {
                            return TILEXR_ERROR_NOT_INITIALIZED;
                        }
                        localEid = state->localEid;
                    } else {
                        const auto* state = GetFallbackQpState(qpIdx);
                        if (state == nullptr) {
                            return TILEXR_ERROR_NOT_INITIALIZED;
                        }
                        localEid = state->localEid;
                    }
                } else {
                    localEid = localRouteByPeerQp_[index];
                }
            } else if (peer != options_.rank) {
                localEid = peerLocalEid_.at(peer);
            }
            const uint32_t localRegion = registration.qpBindings[qpIdx].localRegion;
            if (localRegion >= registration.regions.size()) {
                return TILEXR_ERROR_PARA_CHECK_FAIL;
            }
            const auto& region = registration.regions[localRegion];
            const auto registrationIt = region.localRegistrations.find(localEid);
            if (registrationIt == region.localRegistrations.end()) {
                return TILEXR_ERROR_NOT_INITIALIZED;
            }
            sq[index].localTokenId = registrationIt->second.tokenId;
            rq[index].localTokenId = registrationIt->second.tokenId;
        }
    }

    registration.infoSize = udmaInfoSize_;
    ret = aclrtMalloc(
        reinterpret_cast<void**>(&registration.infoDev), registration.infoSize, ACL_MEM_MALLOC_HUGE_FIRST);
    if (ret != ACL_SUCCESS) {
        TILEXR_LOG(ERROR) << "aclrtMalloc candidate UDMA info failed: " << ret;
        return TILEXR_ERROR_INTERNAL;
    }

    UDMAInfo info {};
    std::vector<uint8_t> image;
    ret = BuildUDMAInfoImage(reinterpret_cast<uintptr_t>(registration.infoDev), qpCount_,
        sq, rq, scq, rcq, registration.memoryImage, info, image);
    if (ret != TILEXR_UDMA_LAYOUT_SUCCESS) {
        (void)FreeDeviceInfo(registration.infoDev);
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    ret = aclrtMemcpy(registration.infoDev, registration.infoSize,
        image.data(), image.size(), ACL_MEMCPY_HOST_TO_DEVICE);
    if (ret != ACL_SUCCESS) {
        TILEXR_LOG(ERROR) << "aclrtMemcpy candidate UDMA info failed: " << ret;
        (void)FreeDeviceInfo(registration.infoDev);
        return TILEXR_ERROR_INTERNAL;
    }
    return TILEXR_SUCCESS;
}

int TileXRUDMATransport::RegisterMemoryOnContexts(
    RegistrationState& registration, bool fullmeshDomain)
{
    std::map<uint32_t, bool> requiredEids;
    if (fullmeshDomain) {
        if (!fullmeshAvailable_) {
            return TILEXR_ERROR_NOT_SUPPORT;
        }
        for (uint32_t eidIndex : fullmeshLocalRouteByPeer_) {
            if (eidIndex != UINT32_MAX) {
                requiredEids[eidIndex] = true;
            }
        }
    } else if (explicitConfig_) {
        for (uint32_t eidIndex : localRouteByPeerQp_) {
            if (eidIndex != UINT32_MAX) {
                requiredEids[eidIndex] = true;
            }
        }
    } else {
        for (const auto& route : peerLocalEid_) {
            requiredEids[route.second] = true;
        }
    }

    for (size_t regionIndex = 0; regionIndex < registration.regions.size(); ++regionIndex) {
        auto& region = registration.regions[regionIndex];
        auto& localRegistrations = fullmeshDomain ?
            region.fullmeshLocalRegistrations : region.localRegistrations;
        for (const auto& required : requiredEids) {
            const uint32_t eidIndex = required.first;
            const auto ctxIt = ctxHandleByEid_.find(eidIndex);
            const auto tokenIt = tokenHandleByEid_.find(eidIndex);
            if (ctxIt == ctxHandleByEid_.end() || ctxIt->second == nullptr ||
                tokenIt == tokenHandleByEid_.end() || tokenIt->second == nullptr) {
                return TILEXR_ERROR_NOT_INITIALIZED;
            }
            void* tokenHandle = tokenIt->second;
            MrRegInfoT mrInfo {};
            mrInfo.in.mem.addr = reinterpret_cast<uint64_t>(region.localPtr);
            mrInfo.in.mem.size = region.bytes;
            mrInfo.in.ub.tokenValue = TILEXR_UDMA_TOKEN_VALUE;
            mrInfo.in.ub.tokenIdHandle = tokenHandle;
            mrInfo.in.ub.flags.bs.cacheable = 0;
            mrInfo.in.ub.flags.bs.access = MEM_SEG_ACCESS_DEFAULT;
            mrInfo.in.ub.flags.bs.nonPin = options_.nonPinRegistration ? 1 : 0;
            mrInfo.in.ub.flags.bs.userIova = 0;
            mrInfo.in.ub.flags.bs.tokenIdValid = 1;
            mrInfo.in.ub.flags.bs.tokenPolicy = MEM_SEG_TOKEN_PLAIN_TEXT;
            void* lmemHandle = nullptr;
            int ret = loader_.RaCtxLmemRegister(
                ctxIt->second, &mrInfo, &lmemHandle);
            if (ret != 0 || lmemHandle == nullptr) {
                constexpr uintptr_t twoMiB = UINT64_C(2) << 20;
                TILEXR_LOG(ERROR) << "RaCtxLmemRegister failed for region " << regionIndex
                                  << ", eid " << eidIndex
                                  << ", ret " << ret
                                  << ", bytes " << region.bytes
                                  << ", ptr " << reinterpret_cast<uintptr_t>(region.localPtr)
                                  << ", handle " << lmemHandle
                                  << ", ptr modulo 2 MiB "
                                  << (reinterpret_cast<uintptr_t>(region.localPtr) % twoMiB);
                return TILEXR_ERROR_INTERNAL;
            }

            RegMemResultInfo result {};
            result.address = reinterpret_cast<uint64_t>(region.localPtr);
            result.size = region.bytes;
            result.lmemHandle = lmemHandle;
            result.key = mrInfo.out.key;
            result.tokenId = mrInfo.out.ub.tokenId;
            result.tokenValue = TILEXR_UDMA_TOKEN_VALUE;
            result.targetSegHandle = mrInfo.out.ub.targetSegHandle;
            result.tokenIdHandle = tokenHandle;
            result.cacheable = 0;
            result.access = MEM_SEG_ACCESS_DEFAULT;
            localRegistrations[eidIndex] = result;
            if (!fullmeshDomain) {
                region.localMemInfoByEid[eidIndex] = BuildMemInfo(result);
            }
        }
    }
    return registration.regions.empty() ? TILEXR_ERROR_NOT_FOUND : TILEXR_SUCCESS;
}

int TileXRUDMATransport::ExchangeAndImportMemory(RegistrationState& registration)
{
    if (registration.regions.empty() || registration.qpBindings.size() != qpCount_) {
        return TILEXR_ERROR_NOT_FOUND;
    }
    size_t localCountValue = 0;
    for (const auto& region : registration.regions) {
        if (region.localRegistrations.size() >
            std::numeric_limits<size_t>::max() - localCountValue) {
            return TILEXR_ERROR_PARA_CHECK_FAIL;
        }
        localCountValue += region.localRegistrations.size();
    }
    if (localCountValue == 0 || localCountValue > std::numeric_limits<uint32_t>::max()) {
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    const uint32_t localCount = static_cast<uint32_t>(localCountValue);
    std::vector<uint32_t> allCounts(options_.rankSize);
    int ret = options_.exchange->AllGather(&localCount, 1, allCounts.data());
    if (ret != TILEXR_SUCCESS) {
        return ret;
    }
    const uint32_t maxCount = *std::max_element(allCounts.begin(), allCounts.end());
    if (maxCount == 0) {
        return TILEXR_ERROR_INTERNAL;
    }

    struct ExchangedMrInfo {
        uint32_t regionIndex;
        uint32_t eidIndex;
        uint32_t valid;
        RegMemResultInfo mr;
    };

    std::vector<ExchangedMrInfo> local(maxCount);
    uint32_t idx = 0;
    for (uint32_t regionIndex = 0; regionIndex < registration.regions.size(); ++regionIndex) {
        for (const auto& entry : registration.regions[regionIndex].localRegistrations) {
            local[idx].regionIndex = regionIndex;
            local[idx].eidIndex = entry.first;
            local[idx].valid = 1;
            local[idx].mr = entry.second;
            ++idx;
        }
    }
    if (static_cast<size_t>(options_.rankSize) >
        std::numeric_limits<size_t>::max() / maxCount) {
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    std::vector<ExchangedMrInfo> all(static_cast<size_t>(options_.rankSize) * maxCount);
    ret = options_.exchange->AllGather(local.data(), local.size(), all.data());
    if (ret != TILEXR_SUCCESS) {
        return ret;
    }

    const size_t entryCount = static_cast<size_t>(options_.rankSize) * qpCount_;
    registration.memoryImage.assign(entryCount, UDMAMemInfo {});
    const uint32_t fallbackEid = FallbackLocalEid();
    for (uint32_t qpIdx = 0; qpIdx < qpCount_; ++qpIdx) {
        const size_t selfIndex = RouteIndex(options_.rank, qpIdx);
        const uint32_t localEid = explicitConfig_ ? localRouteByPeerQp_[selfIndex] : fallbackEid;
        const uint32_t remoteRegion = registration.qpBindings[qpIdx].remoteRegion;
        if (remoteRegion >= registration.regions.size()) {
            return TILEXR_ERROR_PARA_CHECK_FAIL;
        }
        const auto& region = registration.regions[remoteRegion];
        const auto localMemIt = region.localMemInfoByEid.find(localEid);
        if (localMemIt == region.localMemInfoByEid.end()) {
            return TILEXR_ERROR_INTERNAL;
        }
        registration.memoryImage[selfIndex] = localMemIt->second;
        registration.memoryImage[selfIndex].eidAddr = reinterpret_cast<uint64_t>(eidTableDev_ +
            (static_cast<size_t>(options_.rank) * eidCount_ + localEid) * sizeof(HccpEid));
    }

    for (int peer = 0; peer < options_.rankSize; ++peer) {
        if (peer == options_.rank) {
            continue;
        }
        for (uint32_t qpIdx = 0; qpIdx < qpCount_; ++qpIdx) {
            const size_t imageIndex = RouteIndex(peer, qpIdx);
            if (groupedQpProfile_ &&
                qpIdx >= TILEXR_UDMA_GROUPED_DIRECT_QP_BASE &&
                UDMAGroupedDirectQpIndex(
                    options_.rank, peer, options_.rankSize, qpCount_,
                    static_cast<uint32_t>(options_.localRankSize)) != qpIdx) {
                registration.memoryImage[imageIndex] =
                    registration.memoryImage[RouteIndex(peer, 0U)];
                continue;
            }
            const uint32_t localEid = explicitConfig_
                ? localRouteByPeerQp_[imageIndex] : peerLocalEid_[peer];
            const uint32_t remoteEid = explicitConfig_
                ? remoteRouteByPeerQp_[imageIndex] : peerRemoteEid_[peer];
            const uint32_t remoteRegion = registration.qpBindings[qpIdx].remoteRegion;
            if (remoteRegion >= registration.regions.size()) {
                return TILEXR_ERROR_PARA_CHECK_FAIL;
            }
            const ExchangedMrInfo* remote = nullptr;
            for (uint32_t i = 0; i < allCounts[peer]; ++i) {
                const auto& candidate = all[static_cast<size_t>(peer) * maxCount + i];
                if (candidate.valid != 0 && candidate.regionIndex == remoteRegion &&
                    candidate.eidIndex == remoteEid) {
                    remote = &candidate;
                    break;
                }
            }
            if (remote == nullptr) {
                return TILEXR_ERROR_INTERNAL;
            }
            const auto ctxIt = ctxHandleByEid_.find(localEid);
            if (ctxIt == ctxHandleByEid_.end()) {
                return TILEXR_ERROR_INTERNAL;
            }
            const auto importKey = std::make_tuple(peer, localEid, remoteEid);
            auto& region = registration.regions[remoteRegion];
            if (region.remoteMemHandles.count(importKey) == 0) {
                MrImportInfoT importInfo {};
                importInfo.in.key = remote->mr.key;
                importInfo.in.ub.tokenValue = remote->mr.tokenValue;
                importInfo.in.ub.flags.bs.cacheable = remote->mr.cacheable;
                importInfo.in.ub.flags.bs.access = remote->mr.access;
                void* remoteHandle = nullptr;
                ret = loader_.RaCtxRmemImport(ctxIt->second, &importInfo, &remoteHandle);
                if (ret != 0 || remoteHandle == nullptr) {
                    TILEXR_LOG(ERROR) << "RaCtxRmemImport failed for peer " << peer
                                      << ", local eid " << localEid
                                      << ", remote eid " << remoteEid
                                      << ", ret " << ret << ", handle " << remoteHandle;
                    return TILEXR_ERROR_INTERNAL;
                }
                region.remoteMemHandles[importKey] = remoteHandle;
            }

            uint32_t tpn = 0;
            if (explicitConfig_) {
                if (sharedQp_) {
                    const auto* state = GetSharedQpState(qpIdx);
                    if (state == nullptr ||
                        state->tpnList.size() != static_cast<size_t>(options_.rankSize)) {
                        return TILEXR_ERROR_NOT_INITIALIZED;
                    }
                    tpn = state->tpnList[peer];
                } else {
                    const auto* state = GetPeerQpState(peer, qpIdx);
                    if (state == nullptr) {
                        return TILEXR_ERROR_NOT_INITIALIZED;
                    }
                    tpn = state->tpn;
                }
            } else {
                const auto stateIt = states_.find(localEid);
                if (stateIt == states_.end()) {
                    return TILEXR_ERROR_NOT_INITIALIZED;
                }
                tpn = stateIt->second.tpnList[peer];
            }
            registration.memoryImage[imageIndex] = BuildMemInfo(remote->mr);
            registration.memoryImage[imageIndex].tpn = tpn;
            registration.memoryImage[imageIndex].eidAddr = reinterpret_cast<uint64_t>(eidTableDev_ +
                (static_cast<size_t>(peer) * eidCount_ + remoteEid) * sizeof(HccpEid));
        }
    }
    return TILEXR_SUCCESS;
}

int TileXRUDMATransport::PrepareFullmeshRegistration(
    RegistrationState& registration)
{
    if (!fullmeshAvailable_ || registration.regions.size() != 1U) {
        return TILEXR_ERROR_NOT_SUPPORT;
    }
    auto& region = registration.regions[0];
    if (region.fullmeshLocalRegistrations.empty()) {
        return TILEXR_ERROR_NOT_INITIALIZED;
    }

    struct ExchangedMrInfo {
        uint32_t eidIndex;
        uint32_t valid;
        RegMemResultInfo mr;
    };
    const uint32_t localCount = static_cast<uint32_t>(
        region.fullmeshLocalRegistrations.size());
    std::vector<uint32_t> allCounts(options_.rankSize);
    int ret = options_.exchange->AllGather(
        &localCount, 1, allCounts.data());
    if (ret != TILEXR_SUCCESS) {
        return ret;
    }
    const uint32_t maxCount =
        *std::max_element(allCounts.begin(), allCounts.end());
    if (maxCount == 0U) {
        return TILEXR_ERROR_INTERNAL;
    }
    std::vector<ExchangedMrInfo> local(maxCount);
    uint32_t index = 0U;
    for (const auto& entry : region.fullmeshLocalRegistrations) {
        local[index].eidIndex = entry.first;
        local[index].valid = 1U;
        local[index].mr = entry.second;
        ++index;
    }
    std::vector<ExchangedMrInfo> all(
        static_cast<size_t>(options_.rankSize) * maxCount);
    ret = options_.exchange->AllGather(
        local.data(), local.size(), all.data());
    if (ret != TILEXR_SUCCESS) {
        return ret;
    }

    registration.fullmeshMemoryImage.assign(
        static_cast<size_t>(options_.rankSize) *
            TILEXR_UDMA_FULLMESH_SLOT_COUNT,
        UDMAMemInfo {});
    const int localNode = options_.rank / options_.localRankSize;
    for (int peer = 0; peer < options_.rankSize; ++peer) {
        if (peer == options_.rank ||
            peer / options_.localRankSize != localNode) {
            continue;
        }
        const uint32_t localEid = fullmeshLocalRouteByPeer_[peer];
        const uint32_t remoteEid = fullmeshRemoteRouteByPeer_[peer];
        const ExchangedMrInfo* remote = nullptr;
        for (uint32_t remoteIndex = 0U;
            remoteIndex < allCounts[peer]; ++remoteIndex) {
            const auto& candidate = all[
                static_cast<size_t>(peer) * maxCount + remoteIndex];
            if (candidate.valid != 0U &&
                candidate.eidIndex == remoteEid) {
                remote = &candidate;
                break;
            }
        }
        if (remote == nullptr) {
            return TILEXR_ERROR_NOT_INITIALIZED;
        }
        const auto ctxIt = ctxHandleByEid_.find(localEid);
        if (ctxIt == ctxHandleByEid_.end()) {
            return TILEXR_ERROR_NOT_INITIALIZED;
        }
        const auto importKey = std::make_tuple(peer, localEid, remoteEid);
        if (region.fullmeshRemoteMemHandles.count(importKey) == 0U) {
            MrImportInfoT importInfo {};
            importInfo.in.key = remote->mr.key;
            importInfo.in.ub.tokenValue = remote->mr.tokenValue;
            importInfo.in.ub.flags.bs.cacheable = remote->mr.cacheable;
            importInfo.in.ub.flags.bs.access = remote->mr.access;
            void* remoteHandle = nullptr;
            ret = loader_.RaCtxRmemImport(
                ctxIt->second, &importInfo, &remoteHandle);
            if (ret != 0 || remoteHandle == nullptr) {
                TILEXR_LOG(ERROR) << "TileXR Fullmesh remote memory import failed for peer "
                                  << peer << ", local eid " << localEid
                                  << ", remote eid " << remoteEid
                                  << ", ret " << ret;
                return TILEXR_ERROR_INTERNAL;
            }
            region.fullmeshRemoteMemHandles[importKey] = remoteHandle;
        }
        const PerPeerQpState* state = GetFullmeshQpState(peer);
        if (state == nullptr || state->remoteQpHandle == nullptr) {
            return TILEXR_ERROR_NOT_INITIALIZED;
        }
        const uint32_t slot = static_cast<uint32_t>(
            peer % options_.localRankSize);
        UDMAMemInfo& mem = registration.fullmeshMemoryImage[
            FullmeshEntryIndex(peer, slot)];
        mem = BuildMemInfo(remote->mr);
        mem.tpn = state->tpn;
        mem.eidAddr = reinterpret_cast<uint64_t>(eidTableDev_ +
            (static_cast<size_t>(peer) * eidCount_ + remoteEid) *
                sizeof(HccpEid));
    }
    return TILEXR_SUCCESS;
}

int TileXRUDMATransport::BuildFullmeshRegistrationUDMAInfo(
    RegistrationState& registration)
{
    const size_t entryCount = static_cast<size_t>(options_.rankSize) *
        TILEXR_UDMA_FULLMESH_SLOT_COUNT;
    if (!fullmeshAvailable_ || registration.regions.size() != 1U ||
        registration.fullmeshMemoryImage.size() != entryCount) {
        return TILEXR_ERROR_NOT_INITIALIZED;
    }
    std::vector<UDMAWQCtx> sq;
    std::vector<UDMAWQCtx> rq;
    std::vector<UDMACQCtx> scq;
    std::vector<UDMACQCtx> rcq;
    int ret = BuildFullmeshQueueImages(sq, rq, scq, rcq,
        registration.fullmeshMemoryImage);
    if (ret != TILEXR_SUCCESS) {
        return ret;
    }

    const auto& region = registration.regions[0];
    const int localNode = options_.rank / options_.localRankSize;
    for (int peer = 0; peer < options_.rankSize; ++peer) {
        if (peer == options_.rank ||
            peer / options_.localRankSize != localNode) {
            continue;
        }
        const uint32_t localEid = fullmeshLocalRouteByPeer_[peer];
        const auto registrationIt =
            region.fullmeshLocalRegistrations.find(localEid);
        if (registrationIt == region.fullmeshLocalRegistrations.end()) {
            return TILEXR_ERROR_NOT_INITIALIZED;
        }
        const uint32_t slot = static_cast<uint32_t>(
            peer % options_.localRankSize);
        const size_t entry = FullmeshEntryIndex(peer, slot);
        sq[entry].localTokenId = registrationIt->second.tokenId;
        rq[entry].localTokenId = registrationIt->second.tokenId;
    }

    registration.fullmeshInfoSize = fullmeshInfoSize_;
    ret = aclrtMalloc(
        reinterpret_cast<void**>(&registration.fullmeshInfoDev),
        registration.fullmeshInfoSize, ACL_MEM_MALLOC_HUGE_FIRST);
    if (ret != ACL_SUCCESS) {
        return TILEXR_ERROR_INTERNAL;
    }
    UDMAInfo info {};
    std::vector<uint8_t> image;
    ret = BuildUDMAInfoImage(
        reinterpret_cast<uintptr_t>(registration.fullmeshInfoDev),
        TILEXR_UDMA_FULLMESH_SLOT_COUNT,
        sq, rq, scq, rcq, registration.fullmeshMemoryImage, info, image);
    if (ret != TILEXR_UDMA_LAYOUT_SUCCESS) {
        (void)FreeDeviceInfo(registration.fullmeshInfoDev);
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    ret = aclrtMemcpy(registration.fullmeshInfoDev,
        registration.fullmeshInfoSize, image.data(), image.size(),
        ACL_MEMCPY_HOST_TO_DEVICE);
    if (ret != ACL_SUCCESS) {
        (void)FreeDeviceInfo(registration.fullmeshInfoDev);
        return TILEXR_ERROR_INTERNAL;
    }
    registration.fullmeshReady = true;
    return TILEXR_SUCCESS;
}

int TileXRUDMATransport::AgreeRegistrationStatus(int localStatus) const
{
    if (options_.exchange == nullptr || options_.rankSize <= 0) {
        return TILEXR_ERROR_INTERNAL;
    }
    int32_t local = static_cast<int32_t>(localStatus);
    std::vector<int32_t> allStatus(options_.rankSize, TILEXR_ERROR_INTERNAL);
    const int exchangeRet = options_.exchange->AllGather(&local, 1, allStatus.data());
    if (exchangeRet != TILEXR_SUCCESS) {
        return exchangeRet;
    }
    for (int32_t status : allStatus) {
        if (status != TILEXR_SUCCESS) {
            return static_cast<int>(status);
        }
    }
    return TILEXR_SUCCESS;
}

int TileXRUDMATransport::PrepareRegistration(const TileXRUDMAProfileDesc& desc,
    std::unique_ptr<RegistrationState>& registration, bool prepareFullmesh)
{
    if (!available_ || !UDMAProfileDescValid(&desc, qpCount_) || registration != nullptr) {
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }

    registration.reset(new (std::nothrow) RegistrationState());
    int localStatus = registration == nullptr ? TILEXR_ERROR_INTERNAL : TILEXR_SUCCESS;
    if (registration != nullptr) {
        registration->regions.resize(desc.regionCount);
        for (uint32_t region = 0; region < desc.regionCount; ++region) {
            registration->regions[region].localPtr =
                UDMAProfileRegistrationBase(desc.regions[region]);
            registration->regions[region].bytes = static_cast<size_t>(
                UDMAProfileRegistrationBytes(desc.regions[region]));
        }
        registration->qpBindings.assign(desc.qpBindings,
            desc.qpBindings + desc.qpBindingCount);
        localStatus = RegisterMemoryOnContexts(*registration, false);
    }
    int agreedStatus = AgreeRegistrationStatus(localStatus);
    if (agreedStatus != TILEXR_SUCCESS) {
        const int cleanupRet = CleanupRegistrationPtr(registration);
        return cleanupRet == TILEXR_SUCCESS ? agreedStatus : cleanupRet;
    }

    localStatus = ExchangeAndImportMemory(*registration);
    agreedStatus = AgreeRegistrationStatus(localStatus);
    if (agreedStatus != TILEXR_SUCCESS) {
        const int cleanupRet = CleanupRegistrationPtr(registration);
        return cleanupRet == TILEXR_SUCCESS ? agreedStatus : cleanupRet;
    }

    localStatus = BuildRegistrationUDMAInfo(*registration);
    agreedStatus = AgreeRegistrationStatus(localStatus);
    if (agreedStatus != TILEXR_SUCCESS) {
        const int cleanupRet = CleanupRegistrationPtr(registration);
        return cleanupRet == TILEXR_SUCCESS ? agreedStatus : cleanupRet;
    }

    if (!prepareFullmesh || !fullmeshAvailable_) {
        return TILEXR_SUCCESS;
    }

    localStatus = RegisterMemoryOnContexts(*registration, true);
    agreedStatus = AgreeRegistrationStatus(localStatus);
    if (agreedStatus == TILEXR_SUCCESS) {
        localStatus = PrepareFullmeshRegistration(*registration);
        agreedStatus = AgreeRegistrationStatus(localStatus);
    }
    if (agreedStatus == TILEXR_SUCCESS) {
        localStatus = BuildFullmeshRegistrationUDMAInfo(*registration);
        agreedStatus = AgreeRegistrationStatus(localStatus);
    }
    if (agreedStatus != TILEXR_SUCCESS) {
        const int fullmeshStatus = agreedStatus;
        const int localCleanupStatus =
            CleanupFullmeshRegistration(*registration);
        const int cleanupStatus =
            AgreeRegistrationStatus(localCleanupStatus);
        if (cleanupStatus != TILEXR_SUCCESS) {
            const int candidateCleanupStatus =
                CleanupRegistrationPtr(registration);
            const int agreedCandidateCleanupStatus =
                AgreeRegistrationStatus(candidateCleanupStatus);
            return agreedCandidateCleanupStatus == TILEXR_SUCCESS ?
                cleanupStatus : agreedCandidateCleanupStatus;
        }
        TILEXR_LOG(WARN) << "TileXR Fullmesh registration unavailable for this generation: "
                         << fullmeshStatus;
        return TILEXR_SUCCESS;
    }
    return TILEXR_SUCCESS;
}

int TileXRUDMATransport::PrepareMemory(GM_ADDR localPtr, size_t bytes)
{
    if (!available_ || localPtr == nullptr || bytes == 0) {
        return TILEXR_ERROR_NOT_FOUND;
    }
    if (preparedRegistration_ != nullptr || !retiredRegistrations_.empty()) {
        TILEXR_LOG(ERROR) << "TileXR UDMA cannot prepare memory while cleanup is pending";
        return TILEXR_ERROR_INTERNAL;
    }
    TileXRUDMAProfileDesc desc {};
    desc.regionCount = 1;
    desc.qpBindingCount = qpCount_;
    desc.regions[0].base = localPtr;
    desc.regions[0].bytes = bytes;
    return PrepareRegistration(desc, preparedRegistration_, true);
}

GM_ADDR TileXRUDMATransport::GetPreparedUDMAInfoDev() const
{
    return preparedRegistration_ == nullptr ? nullptr : preparedRegistration_->infoDev;
}

GM_ADDR TileXRUDMATransport::GetPreparedFullmeshInfoDev() const
{
    return preparedRegistration_ == nullptr ||
            !preparedRegistration_->fullmeshReady ?
        nullptr : preparedRegistration_->fullmeshInfoDev;
}

bool TileXRUDMATransport::PreparedFullmeshReady() const
{
    return preparedRegistration_ != nullptr &&
        preparedRegistration_->fullmeshReady &&
        preparedRegistration_->fullmeshInfoDev != nullptr;
}

int TileXRUDMATransport::CommitPreparedMemory()
{
    if (preparedRegistration_ == nullptr || preparedRegistration_->infoDev == nullptr) {
        return TILEXR_ERROR_NOT_INITIALIZED;
    }
    if (activeRegistration_ != nullptr) {
        retiredRegistrations_.push_back(std::move(activeRegistration_));
    }
    activeRegistration_ = std::move(preparedRegistration_);
    udmaInfoDev_ = activeRegistration_->infoDev;
    fullmeshInfoDev_ = activeRegistration_->fullmeshReady ?
        activeRegistration_->fullmeshInfoDev : baseFullmeshInfoDev_;
    return TILEXR_SUCCESS;
}

int TileXRUDMATransport::AbortPreparedMemory()
{
    return CleanupRegistrationPtr(preparedRegistration_);
}

int TileXRUDMATransport::PrepareProfile(const TileXRUDMAProfileDesc& desc)
{
    if (preparedProfile_ != nullptr) {
        TILEXR_LOG(ERROR) << "TileXR UDMA cannot prepare a profile while profile cleanup is pending";
        return TILEXR_ERROR_INTERNAL;
    }
    return PrepareRegistration(desc, preparedProfile_, false);
}

GM_ADDR TileXRUDMATransport::GetPreparedProfileInfoDev() const
{
    return preparedProfile_ == nullptr ? nullptr : preparedProfile_->infoDev;
}

int TileXRUDMATransport::CommitPreparedProfile(TileXRUDMAProfileHandle handle)
{
    if (handle == 0 || preparedProfile_ == nullptr || preparedProfile_->infoDev == nullptr ||
        profiles_.count(handle) != 0) {
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    profiles_.emplace(handle, std::move(preparedProfile_));
    return TILEXR_SUCCESS;
}

int TileXRUDMATransport::AbortPreparedProfile()
{
    return CleanupRegistrationPtr(preparedProfile_);
}

int TileXRUDMATransport::CleanupLocalRegistrations(std::map<uint32_t, RegMemResultInfo>& byEid)
{
    int firstError = TILEXR_SUCCESS;
    for (auto it = byEid.begin(); it != byEid.end();) {
        const uint32_t eidIndex = it->first;
        if (it->second.lmemHandle == nullptr) {
            it = byEid.erase(it);
            continue;
        }
        const auto ctxIt = ctxHandleByEid_.find(eidIndex);
        if (ctxIt == ctxHandleByEid_.end() || ctxIt->second == nullptr) {
            TILEXR_LOG(ERROR) << "Cannot unregister UDMA local memory for missing eid context " << eidIndex;
            if (firstError == TILEXR_SUCCESS) {
                firstError = TILEXR_ERROR_INTERNAL;
            }
            ++it;
            continue;
        }
        const int ret = loader_.RaCtxLmemUnregister(ctxIt->second, it->second.lmemHandle);
        if (ret != 0) {
            TILEXR_LOG(ERROR) << "RaCtxLmemUnregister failed for eid " << eidIndex << ", ret " << ret;
            if (firstError == TILEXR_SUCCESS) {
                firstError = TILEXR_ERROR_INTERNAL;
            }
            ++it;
            continue;
        }
        it = byEid.erase(it);
    }
    return firstError;
}

int TileXRUDMATransport::CleanupRemoteImports(
    RegistrationState& registration, bool fullmeshDomain)
{
    int firstError = TILEXR_SUCCESS;
    for (size_t regionIndex = 0; regionIndex < registration.regions.size(); ++regionIndex) {
        auto& region = registration.regions[regionIndex];
        auto& handles = fullmeshDomain ?
            region.fullmeshRemoteMemHandles : region.remoteMemHandles;
        for (auto it = handles.begin(); it != handles.end();) {
            if (it->second == nullptr) {
                it = handles.erase(it);
                continue;
            }
            const int peer = std::get<0>(it->first);
            const uint32_t localEid = std::get<1>(it->first);
            const auto ctxIt = ctxHandleByEid_.find(localEid);
            if (ctxIt == ctxHandleByEid_.end() || ctxIt->second == nullptr) {
                TILEXR_LOG(ERROR) << "Cannot unimport UDMA remote memory for region "
                                  << regionIndex << ", peer " << peer
                                  << ", Fullmesh " << fullmeshDomain
                                  << " without local eid context " << localEid;
                if (firstError == TILEXR_SUCCESS) {
                    firstError = TILEXR_ERROR_INTERNAL;
                }
                ++it;
                continue;
            }
            const int ret = loader_.RaCtxRmemUnimport(ctxIt->second, it->second);
            if (ret != 0) {
                TILEXR_LOG(ERROR) << "RaCtxRmemUnimport failed for region " << regionIndex
                                  << ", peer " << peer
                                  << ", Fullmesh " << fullmeshDomain
                                  << ", ret " << ret;
                if (firstError == TILEXR_SUCCESS) {
                    firstError = TILEXR_ERROR_INTERNAL;
                }
                ++it;
                continue;
            }
            it = handles.erase(it);
        }
    }
    return firstError;
}

int TileXRUDMATransport::FreeDeviceInfo(GM_ADDR& infoDev) const
{
    if (infoDev == nullptr) {
        return TILEXR_SUCCESS;
    }
    const aclError ret = aclrtFree(infoDev);
    if (ret != ACL_SUCCESS) {
        TILEXR_LOG(ERROR) << "Free UDMA info failed: " << ret
                          << ", ptr " << reinterpret_cast<uintptr_t>(infoDev);
        return TILEXR_ERROR_INTERNAL;
    }
    infoDev = nullptr;
    return TILEXR_SUCCESS;
}

int TileXRUDMATransport::CleanupFullmeshRegistration(
    RegistrationState& registration)
{
    registration.fullmeshReady = false;
    int firstError = CleanupRemoteImports(registration, true);
    for (auto& region : registration.regions) {
        const int localRet = CleanupLocalRegistrations(
            region.fullmeshLocalRegistrations);
        if (firstError == TILEXR_SUCCESS && localRet != TILEXR_SUCCESS) {
            firstError = localRet;
        }
    }
    const int infoRet = FreeDeviceInfo(registration.fullmeshInfoDev);
    if (firstError == TILEXR_SUCCESS && infoRet != TILEXR_SUCCESS) {
        firstError = infoRet;
    }
    const bool resourcesClean = std::all_of(
        registration.regions.begin(), registration.regions.end(),
        [](const RegisteredRegionState& region) {
            return region.fullmeshRemoteMemHandles.empty() &&
                region.fullmeshLocalRegistrations.empty();
        });
    if (resourcesClean && registration.fullmeshInfoDev == nullptr) {
        registration.fullmeshMemoryImage.clear();
        registration.fullmeshInfoSize = 0U;
    }
    return firstError;
}

int TileXRUDMATransport::CleanupRegistration(RegistrationState& registration)
{
    registration.cleanupPending = true;
    registration.fullmeshReady = false;

    int localStatus = CleanupRemoteImports(registration, true);
    const int remoteRet = CleanupRemoteImports(registration, false);
    if (localStatus == TILEXR_SUCCESS && remoteRet != TILEXR_SUCCESS) {
        localStatus = remoteRet;
    }
    int agreedStatus = AgreeRegistrationStatus(localStatus);
    int firstError = agreedStatus;

    localStatus = firstError;
    if (firstError == TILEXR_SUCCESS) {
        for (auto& region : registration.regions) {
            int cleanupRet = CleanupLocalRegistrations(
                region.fullmeshLocalRegistrations);
            if (localStatus == TILEXR_SUCCESS &&
                cleanupRet != TILEXR_SUCCESS) {
                localStatus = cleanupRet;
            }
            cleanupRet = CleanupLocalRegistrations(
                region.localRegistrations);
            if (localStatus == TILEXR_SUCCESS &&
                cleanupRet != TILEXR_SUCCESS) {
                localStatus = cleanupRet;
            }
        }
    }
    agreedStatus = AgreeRegistrationStatus(localStatus);
    if (firstError == TILEXR_SUCCESS && agreedStatus != TILEXR_SUCCESS) {
        firstError = agreedStatus;
    }

    localStatus = firstError;
    if (firstError == TILEXR_SUCCESS) {
        localStatus = FreeDeviceInfo(registration.fullmeshInfoDev);
        const int infoRet = FreeDeviceInfo(registration.infoDev);
        if (localStatus == TILEXR_SUCCESS && infoRet != TILEXR_SUCCESS) {
            localStatus = infoRet;
        }
    }
    agreedStatus = AgreeRegistrationStatus(localStatus);
    if (firstError == TILEXR_SUCCESS && agreedStatus != TILEXR_SUCCESS) {
        firstError = agreedStatus;
    }
    if (firstError != TILEXR_SUCCESS) {
        return firstError;
    }
    const bool resourcesClean = std::all_of(registration.regions.begin(), registration.regions.end(),
        [](const RegisteredRegionState& region) {
            return region.remoteMemHandles.empty() &&
                region.localRegistrations.empty() &&
                region.fullmeshRemoteMemHandles.empty() &&
                region.fullmeshLocalRegistrations.empty();
        });
    if (resourcesClean && registration.infoDev == nullptr &&
        registration.fullmeshInfoDev == nullptr) {
        for (auto& region : registration.regions) {
            region.localMemInfoByEid.clear();
        }
        registration.regions.clear();
        registration.qpBindings.clear();
        registration.memoryImage.clear();
        registration.fullmeshMemoryImage.clear();
        registration.fullmeshInfoSize = 0U;
        registration.fullmeshReady = false;
        registration.cleanupPending = false;
    }
    return TILEXR_SUCCESS;
}

int TileXRUDMATransport::CleanupRegistrationPtr(std::unique_ptr<RegistrationState>& registration)
{
    if (registration == nullptr) {
        int ret = AgreeRegistrationStatus(TILEXR_SUCCESS);
        ret = AgreeRegistrationStatus(ret);
        return AgreeRegistrationStatus(ret);
    }
    const int ret = CleanupRegistration(*registration);
    if (ret == TILEXR_SUCCESS && registration->regions.empty() &&
        registration->infoDev == nullptr &&
        registration->fullmeshInfoDev == nullptr) {
        registration.reset();
    }
    return ret;
}

int TileXRUDMATransport::CleanupRetiredMemory()
{
    int firstError = TILEXR_SUCCESS;
    for (auto it = retiredRegistrations_.begin(); it != retiredRegistrations_.end();) {
        const int ret = CleanupRegistration(**it);
        if (firstError == TILEXR_SUCCESS && ret != TILEXR_SUCCESS) {
            firstError = ret;
        }
        if (ret == TILEXR_SUCCESS && (*it)->regions.empty() &&
            (*it)->infoDev == nullptr && (*it)->fullmeshInfoDev == nullptr) {
            it = retiredRegistrations_.erase(it);
        } else {
            ++it;
        }
    }
    return firstError;
}

int TileXRUDMATransport::CleanupAllMemory()
{
    udmaInfoDev_ = baseUDMAInfoDev_;
    fullmeshInfoDev_ = baseFullmeshInfoDev_;
    int firstError = CleanupRegistrationPtr(preparedRegistration_);
    const int activeRet = CleanupRegistrationPtr(activeRegistration_);
    if (firstError == TILEXR_SUCCESS && activeRet != TILEXR_SUCCESS) {
        firstError = activeRet;
    }
    const int retiredRet = CleanupRetiredMemory();
    if (firstError == TILEXR_SUCCESS && retiredRet != TILEXR_SUCCESS) {
        firstError = retiredRet;
    }
    return firstError;
}

int TileXRUDMATransport::CleanupProfile(TileXRUDMAProfileHandle handle)
{
    const auto it = profiles_.find(handle);
    if (handle == 0) {
        return TILEXR_ERROR_NOT_FOUND;
    }
    if (it == profiles_.end()) {
        int ret = AgreeRegistrationStatus(TILEXR_SUCCESS);
        ret = AgreeRegistrationStatus(ret);
        ret = AgreeRegistrationStatus(ret);
        return ret == TILEXR_SUCCESS ? TILEXR_ERROR_NOT_FOUND : ret;
    }
    const int ret = CleanupRegistration(*it->second);
    if (ret == TILEXR_SUCCESS && it->second->regions.empty() &&
        it->second->infoDev == nullptr) {
        profiles_.erase(it);
    }
    return ret;
}

int TileXRUDMATransport::CleanupAllProfiles()
{
    int firstError = CleanupRegistrationPtr(preparedProfile_);
    for (auto it = profiles_.begin(); it != profiles_.end();) {
        const int ret = CleanupRegistration(*it->second);
        if (firstError == TILEXR_SUCCESS && ret != TILEXR_SUCCESS) {
            firstError = ret;
        }
        if (ret == TILEXR_SUCCESS && it->second->regions.empty() &&
            it->second->infoDev == nullptr) {
            it = profiles_.erase(it);
        } else {
            ++it;
        }
    }
    return firstError;
}

GM_ADDR TileXRUDMATransport::GetProfileInfoDev(TileXRUDMAProfileHandle handle) const
{
    const auto it = profiles_.find(handle);
    return it == profiles_.end() || it->second->cleanupPending ? nullptr : it->second->infoDev;
}

bool TileXRUDMATransport::HasProfileCleanupPending() const
{
    if (preparedProfile_ != nullptr && preparedProfile_->cleanupPending) {
        return true;
    }
    return std::any_of(profiles_.begin(), profiles_.end(),
        [](const std::pair<const TileXRUDMAProfileHandle,
               std::unique_ptr<RegistrationState>>& entry) {
            return entry.second != nullptr && entry.second->cleanupPending;
        });
}

int TileXRUDMATransport::UnregisterMemory(GM_ADDR localPtr)
{
    if (localPtr == nullptr) {
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    const GM_ADDR ownedPtr = GetRegisteredMemoryPtr();
    if (ownedPtr != nullptr && localPtr != ownedPtr && activeRegistration_ != nullptr) {
        return TILEXR_ERROR_NOT_FOUND;
    }
    return CleanupAllMemory();
}

int TileXRUDMATransport::CleanupQueues()
{
    int firstError = TILEXR_SUCCESS;
    bool unimportUnsupported = false;
    const auto cleanupScalars = [this](auto& state) {
        int ret = FreeDeviceScalar(state.cqPiAddr);
        const int cqCiRet = FreeDeviceScalar(state.cqCiAddr);
        if (ret == TILEXR_SUCCESS) {
            ret = cqCiRet;
        }
        const int sqPiRet = FreeDeviceScalar(state.sqPiAddr);
        if (ret == TILEXR_SUCCESS) {
            ret = sqPiRet;
        }
        const int sqCiRet = FreeDeviceScalar(state.sqCiAddr);
        if (ret == TILEXR_SUCCESS) {
            ret = sqCiRet;
        }
        const int wqeRet = FreeDeviceScalar(state.wqeCntAddr);
        if (ret == TILEXR_SUCCESS) {
            ret = wqeRet;
        }
        const int amoRet = FreeDeviceScalar(state.amoAddr);
        if (ret == TILEXR_SUCCESS) {
            ret = amoRet;
        }
        return ret;
    };
    for (auto& statePtr : peerQpStates_) {
        if (statePtr == nullptr || statePtr->remoteQpHandle == nullptr) {
            continue;
        }
        if (statePtr->ctxHandle == nullptr) {
            firstError = TILEXR_ERROR_INTERNAL;
            continue;
        }
        const int ret = loader_.RaCtxQpUnimport(
            statePtr->ctxHandle, statePtr->remoteQpHandle);
        if (QpUnimportConsumesHostHandle(ret)) {
            statePtr->remoteQpHandle = nullptr;
            unimportUnsupported = unimportUnsupported ||
                ret == TILEXR_HCCP_RA_QP_UNIMPORT_UNSUPPORTED;
        } else if (firstError == TILEXR_SUCCESS) {
            firstError = TILEXR_ERROR_INTERNAL;
        }
    }
    for (auto& stateEntry : states_) {
        auto& state = stateEntry.second;
        for (void*& remoteQp : state.remoteQpHandles) {
            if (remoteQp == nullptr) {
                continue;
            }
            if (state.ctxHandle == nullptr) {
                firstError = TILEXR_ERROR_INTERNAL;
                continue;
            }
            const int ret = loader_.RaCtxQpUnimport(
                state.ctxHandle, remoteQp);
            if (QpUnimportConsumesHostHandle(ret)) {
                remoteQp = nullptr;
                unimportUnsupported = unimportUnsupported ||
                    ret == TILEXR_HCCP_RA_QP_UNIMPORT_UNSUPPORTED;
            } else if (firstError == TILEXR_SUCCESS) {
                firstError = TILEXR_ERROR_INTERNAL;
            }
        }
    }
    for (auto& statePtr : fullmeshQpStates_) {
        if (statePtr == nullptr || statePtr->remoteQpHandle == nullptr) {
            continue;
        }
        if (statePtr->ctxHandle == nullptr) {
            firstError = TILEXR_ERROR_INTERNAL;
            continue;
        }
        const int ret = loader_.RaCtxQpUnimport(
            statePtr->ctxHandle, statePtr->remoteQpHandle);
        if (QpUnimportConsumesHostHandle(ret)) {
            statePtr->remoteQpHandle = nullptr;
            unimportUnsupported = unimportUnsupported ||
                ret == TILEXR_HCCP_RA_QP_UNIMPORT_UNSUPPORTED;
        } else if (firstError == TILEXR_SUCCESS) {
            firstError = TILEXR_ERROR_INTERNAL;
        }
    }
    std::map<std::pair<void*, void*>, int> sharedUnimportResults;
    for (auto& statePtr : sharedQpStates_) {
        if (statePtr == nullptr) {
            continue;
        }
        auto& state = *statePtr;
        for (size_t remoteIndex = 0U;
             remoteIndex < state.remoteQpHandles.size(); ++remoteIndex) {
            void*& remoteQp = state.remoteQpHandles[remoteIndex];
            if (remoteQp == nullptr) {
                continue;
            }
            if (state.ctxHandle == nullptr) {
                firstError = TILEXR_ERROR_INTERNAL;
                continue;
            }
            const auto key = std::make_pair(state.ctxHandle, remoteQp);
            auto result = sharedUnimportResults.find(key);
            const int ret = result == sharedUnimportResults.end() ?
                loader_.RaCtxQpUnimport(state.ctxHandle, remoteQp) :
                result->second;
            if (result == sharedUnimportResults.end()) {
                sharedUnimportResults.emplace(key, ret);
            }
            if (QpUnimportConsumesHostHandle(ret)) {
                remoteQp = nullptr;
                unimportUnsupported = unimportUnsupported ||
                    ret == TILEXR_HCCP_RA_QP_UNIMPORT_UNSUPPORTED;
            } else {
                TILEXR_LOG(ERROR) << "TileXR shared QP unimport failed, qp "
                                  << state.qpIdx << ", remote " << remoteIndex
                                  << ", ret " << ret;
                if (firstError == TILEXR_SUCCESS) {
                    firstError = TILEXR_ERROR_INTERNAL;
                }
            }
        }
    }
    if (unimportUnsupported) {
        TILEXR_LOG(WARN)
            << "RaCtxQpUnimport is unsupported by the current HCCP service; "
            << "the b131 client consumed the Host handles and cleanup will "
            << "continue through RA context teardown";
    }
    const int sharedCleanupAgreement = AgreeInitStatus(firstError);
    if (firstError == TILEXR_SUCCESS &&
        sharedCleanupAgreement != TILEXR_SUCCESS) {
        firstError = sharedCleanupAgreement;
    }
    if (sharedCleanupAgreement != TILEXR_SUCCESS) {
        return sharedCleanupAgreement;
    }

    for (auto& statePtr : sharedQpStates_) {
        if (statePtr == nullptr) {
            continue;
        }
        auto& state = *statePtr;
        const bool remoteClean = std::all_of(
            state.remoteQpHandles.begin(), state.remoteQpHandles.end(),
            [](void* handle) { return handle == nullptr; });
        if (sharedCleanupAgreement == TILEXR_SUCCESS &&
            remoteClean && state.qpHandle != nullptr) {
            const int ret = loader_.RaCtxQpDestroy(state.qpHandle);
            if (ret == 0) {
                state.qpHandle = nullptr;
            } else {
                TILEXR_LOG(ERROR) << "TileXR shared QP destroy failed, qp "
                                  << state.qpIdx << ", ret " << ret;
                if (firstError == TILEXR_SUCCESS) {
                    firstError = TILEXR_ERROR_INTERNAL;
                }
            }
        }
        if (sharedCleanupAgreement == TILEXR_SUCCESS &&
            remoteClean && state.qpHandle == nullptr &&
            state.cqHandle != nullptr && state.ctxHandle != nullptr) {
            const int ret = loader_.RaCtxCqDestroy(state.ctxHandle, state.cqHandle);
            if (ret == 0) {
                state.cqHandle = nullptr;
            } else {
                TILEXR_LOG(ERROR) << "TileXR shared CQ destroy failed, qp "
                                  << state.qpIdx << ", ret " << ret;
                if (firstError == TILEXR_SUCCESS) {
                    firstError = TILEXR_ERROR_INTERNAL;
                }
            }
        }
        if (sharedCleanupAgreement == TILEXR_SUCCESS &&
            remoteClean && state.qpHandle == nullptr &&
            state.cqHandle == nullptr && state.chanHandle != nullptr &&
            state.ctxHandle != nullptr) {
            const int ret = loader_.RaCtxChanDestroy(state.ctxHandle, state.chanHandle);
            if (ret == 0) {
                state.chanHandle = nullptr;
            } else {
                TILEXR_LOG(ERROR) << "TileXR shared channel destroy failed, qp "
                                  << state.qpIdx << ", ret " << ret;
                if (firstError == TILEXR_SUCCESS) {
                    firstError = TILEXR_ERROR_INTERNAL;
                }
            }
        }
        const bool hardwareClean = sharedCleanupAgreement == TILEXR_SUCCESS &&
            remoteClean && state.qpHandle == nullptr &&
            state.cqHandle == nullptr && state.chanHandle == nullptr;
        if (hardwareClean) {
            const int ret = cleanupScalars(state);
            if (ret == TILEXR_SUCCESS) {
                statePtr.reset();
            } else if (firstError == TILEXR_SUCCESS) {
                firstError = ret;
            }
        }
    }
    const bool sharedClean = std::all_of(sharedQpStates_.begin(),
        sharedQpStates_.end(),
        [](const std::unique_ptr<SharedQpState>& state) {
            return state == nullptr;
        });
    if (firstError == TILEXR_SUCCESS && !sharedClean) {
        firstError = TILEXR_ERROR_INTERNAL;
    }
    if (sharedClean) {
        sharedQpStates_.clear();
    }

    for (auto& statePtr : peerQpStates_) {
        if (statePtr == nullptr) {
            continue;
        }
        auto& state = *statePtr;
        if (state.remoteQpHandle != nullptr && state.ctxHandle != nullptr) {
            const int ret = loader_.RaCtxQpUnimport(
                state.ctxHandle, state.remoteQpHandle);
            if (QpUnimportConsumesHostHandle(ret)) {
                state.remoteQpHandle = nullptr;
                unimportUnsupported = unimportUnsupported ||
                    ret == TILEXR_HCCP_RA_QP_UNIMPORT_UNSUPPORTED;
            } else if (firstError == TILEXR_SUCCESS) {
                firstError = TILEXR_ERROR_INTERNAL;
            }
        }
        if (state.remoteQpHandle == nullptr && state.qpHandle != nullptr) {
            const int ret = loader_.RaCtxQpDestroy(state.qpHandle);
            if (ret == 0) {
                state.qpHandle = nullptr;
            } else if (firstError == TILEXR_SUCCESS) {
                firstError = TILEXR_ERROR_INTERNAL;
            }
        }
        if (state.remoteQpHandle == nullptr && state.qpHandle == nullptr &&
            state.cqHandle != nullptr && state.ctxHandle != nullptr) {
            const int ret = loader_.RaCtxCqDestroy(state.ctxHandle, state.cqHandle);
            if (ret == 0) {
                state.cqHandle = nullptr;
            } else if (firstError == TILEXR_SUCCESS) {
                firstError = TILEXR_ERROR_INTERNAL;
            }
        }
        if (state.remoteQpHandle == nullptr && state.qpHandle == nullptr &&
            state.cqHandle == nullptr && state.chanHandle != nullptr &&
            state.ctxHandle != nullptr) {
            const int ret = loader_.RaCtxChanDestroy(state.ctxHandle, state.chanHandle);
            if (ret == 0) {
                state.chanHandle = nullptr;
            } else if (firstError == TILEXR_SUCCESS) {
                firstError = TILEXR_ERROR_INTERNAL;
            }
        }
        if (state.remoteQpHandle == nullptr && state.qpHandle == nullptr &&
            state.cqHandle == nullptr && state.chanHandle == nullptr) {
            const int ret = cleanupScalars(state);
            if (ret == TILEXR_SUCCESS) {
                statePtr.reset();
            } else if (firstError == TILEXR_SUCCESS) {
                firstError = ret;
            }
        }
    }
    const bool peerClean = std::all_of(peerQpStates_.begin(),
        peerQpStates_.end(),
        [](const std::unique_ptr<PerPeerQpState>& state) {
            return state == nullptr;
        });
    if (firstError == TILEXR_SUCCESS && !peerClean) {
        firstError = TILEXR_ERROR_INTERNAL;
    }
    if (peerClean) {
        peerQpStates_.clear();
    }

    for (auto stateIt = states_.begin(); stateIt != states_.end();) {
        auto& state = stateIt->second;
        for (void*& remoteQp : state.remoteQpHandles) {
            if (remoteQp == nullptr || state.ctxHandle == nullptr) {
                continue;
            }
            const int ret = loader_.RaCtxQpUnimport(
                state.ctxHandle, remoteQp);
            if (QpUnimportConsumesHostHandle(ret)) {
                remoteQp = nullptr;
                unimportUnsupported = unimportUnsupported ||
                    ret == TILEXR_HCCP_RA_QP_UNIMPORT_UNSUPPORTED;
            } else if (firstError == TILEXR_SUCCESS) {
                firstError = TILEXR_ERROR_INTERNAL;
            }
        }
        const bool remoteClean = std::all_of(state.remoteQpHandles.begin(),
            state.remoteQpHandles.end(),
            [](void* handle) { return handle == nullptr; });
        if (remoteClean && state.qpHandle != nullptr) {
            const int ret = loader_.RaCtxQpDestroy(state.qpHandle);
            if (ret == 0) {
                state.qpHandle = nullptr;
            } else if (firstError == TILEXR_SUCCESS) {
                firstError = TILEXR_ERROR_INTERNAL;
            }
        }
        if (remoteClean && state.qpHandle == nullptr &&
            state.cqHandle != nullptr && state.ctxHandle != nullptr) {
            const int ret = loader_.RaCtxCqDestroy(state.ctxHandle, state.cqHandle);
            if (ret == 0) {
                state.cqHandle = nullptr;
            } else if (firstError == TILEXR_SUCCESS) {
                firstError = TILEXR_ERROR_INTERNAL;
            }
        }
        if (remoteClean && state.qpHandle == nullptr &&
            state.cqHandle == nullptr && state.chanHandle != nullptr &&
            state.ctxHandle != nullptr) {
            const int ret = loader_.RaCtxChanDestroy(state.ctxHandle, state.chanHandle);
            if (ret == 0) {
                state.chanHandle = nullptr;
            } else if (firstError == TILEXR_SUCCESS) {
                firstError = TILEXR_ERROR_INTERNAL;
            }
        }
        if (remoteClean && state.qpHandle == nullptr && state.cqHandle == nullptr && state.chanHandle == nullptr) {
            const int ret = cleanupScalars(state);
            if (ret == TILEXR_SUCCESS) {
                stateIt = states_.erase(stateIt);
            } else {
                if (firstError == TILEXR_SUCCESS) {
                    firstError = ret;
                }
                ++stateIt;
            }
        } else {
            ++stateIt;
        }
    }
    if (firstError == TILEXR_SUCCESS && !states_.empty()) {
        firstError = TILEXR_ERROR_INTERNAL;
    }
    return firstError;
}

int TileXRUDMATransport::CleanupFullmeshQueues()
{
    int firstError = TILEXR_SUCCESS;
    bool unimportUnsupported = false;
    const auto cleanupScalars = [this](PerPeerQpState& state) {
        int ret = FreeDeviceScalar(state.cqPiAddr);
        const int cqCiRet = FreeDeviceScalar(state.cqCiAddr);
        if (ret == TILEXR_SUCCESS) {
            ret = cqCiRet;
        }
        const int sqPiRet = FreeDeviceScalar(state.sqPiAddr);
        if (ret == TILEXR_SUCCESS) {
            ret = sqPiRet;
        }
        const int sqCiRet = FreeDeviceScalar(state.sqCiAddr);
        if (ret == TILEXR_SUCCESS) {
            ret = sqCiRet;
        }
        const int wqeRet = FreeDeviceScalar(state.wqeCntAddr);
        if (ret == TILEXR_SUCCESS) {
            ret = wqeRet;
        }
        const int amoRet = FreeDeviceScalar(state.amoAddr);
        if (ret == TILEXR_SUCCESS) {
            ret = amoRet;
        }
        return ret;
    };
    for (size_t stateIndex = 0U;
         stateIndex < fullmeshQpStates_.size(); ++stateIndex) {
        auto& statePtr = fullmeshQpStates_[stateIndex];
        if (statePtr == nullptr || statePtr->remoteQpHandle == nullptr) {
            continue;
        }
        if (statePtr->ctxHandle == nullptr) {
            firstError = TILEXR_ERROR_INTERNAL;
            continue;
        }
        const int ret = loader_.RaCtxQpUnimport(
            statePtr->ctxHandle, statePtr->remoteQpHandle);
        if (QpUnimportConsumesHostHandle(ret)) {
            statePtr->remoteQpHandle = nullptr;
            unimportUnsupported = unimportUnsupported ||
                ret == TILEXR_HCCP_RA_QP_UNIMPORT_UNSUPPORTED;
        } else {
            TILEXR_LOG(ERROR) << "TileXR Fullmesh QP unimport failed, slot "
                              << stateIndex << ", peer " << statePtr->peer
                              << ", ret " << ret;
            if (firstError == TILEXR_SUCCESS) {
                firstError = TILEXR_ERROR_INTERNAL;
            }
        }
    }
    if (unimportUnsupported) {
        TILEXR_LOG(WARN)
            << "RaCtxQpUnimport is unsupported by the current HCCP service; "
            << "the b131 client consumed the Host handles and cleanup will "
            << "continue through RA context teardown";
    }
    const int remoteAgreement = AgreeInitStatus(firstError);
    if (remoteAgreement != TILEXR_SUCCESS) {
        return remoteAgreement;
    }
    for (size_t stateIndex = 0U; stateIndex < fullmeshQpStates_.size(); ++stateIndex) {
        auto& statePtr = fullmeshQpStates_[stateIndex];
        if (statePtr == nullptr) {
            continue;
        }
        auto& state = *statePtr;
        if (state.remoteQpHandle != nullptr && state.ctxHandle != nullptr) {
            const int ret = loader_.RaCtxQpUnimport(
                state.ctxHandle, state.remoteQpHandle);
            if (QpUnimportConsumesHostHandle(ret)) {
                state.remoteQpHandle = nullptr;
                unimportUnsupported = unimportUnsupported ||
                    ret == TILEXR_HCCP_RA_QP_UNIMPORT_UNSUPPORTED;
            } else {
                TILEXR_LOG(ERROR) << "TileXR Fullmesh QP unimport failed, slot "
                                  << stateIndex << ", peer " << state.peer
                                  << ", ret " << ret;
                if (firstError == TILEXR_SUCCESS) {
                    firstError = TILEXR_ERROR_INTERNAL;
                }
            }
        }
        if (state.remoteQpHandle == nullptr && state.qpHandle != nullptr) {
            const int ret = loader_.RaCtxQpDestroy(state.qpHandle);
            if (ret == 0) {
                state.qpHandle = nullptr;
            } else {
                TILEXR_LOG(ERROR) << "TileXR Fullmesh QP destroy failed, slot "
                                  << stateIndex << ", peer " << state.peer
                                  << ", ret " << ret;
                if (firstError == TILEXR_SUCCESS) {
                    firstError = TILEXR_ERROR_INTERNAL;
                }
            }
        }
        if (state.remoteQpHandle == nullptr && state.qpHandle == nullptr &&
            state.cqHandle != nullptr && state.ctxHandle != nullptr) {
            const int ret = loader_.RaCtxCqDestroy(
                state.ctxHandle, state.cqHandle);
            if (ret == 0) {
                state.cqHandle = nullptr;
            } else {
                TILEXR_LOG(ERROR) << "TileXR Fullmesh CQ destroy failed, slot "
                                  << stateIndex << ", peer " << state.peer
                                  << ", ret " << ret;
                if (firstError == TILEXR_SUCCESS) {
                    firstError = TILEXR_ERROR_INTERNAL;
                }
            }
        }
        if (state.remoteQpHandle == nullptr && state.qpHandle == nullptr &&
            state.cqHandle == nullptr && state.chanHandle != nullptr &&
            state.ctxHandle != nullptr) {
            const int ret = loader_.RaCtxChanDestroy(
                state.ctxHandle, state.chanHandle);
            if (ret == 0) {
                state.chanHandle = nullptr;
            } else {
                TILEXR_LOG(ERROR) << "TileXR Fullmesh channel destroy failed, slot "
                                  << stateIndex << ", peer " << state.peer
                                  << ", ret " << ret;
                if (firstError == TILEXR_SUCCESS) {
                    firstError = TILEXR_ERROR_INTERNAL;
                }
            }
        }
        if (state.remoteQpHandle == nullptr && state.qpHandle == nullptr &&
            state.cqHandle == nullptr && state.chanHandle == nullptr) {
            const int ret = cleanupScalars(state);
            if (ret == TILEXR_SUCCESS) {
                statePtr.reset();
            } else if (firstError == TILEXR_SUCCESS) {
                firstError = ret;
            }
        }
    }
    if (std::all_of(fullmeshQpStates_.begin(), fullmeshQpStates_.end(),
            [](const std::unique_ptr<PerPeerQpState>& state) {
                return state == nullptr;
            })) {
        fullmeshQpStates_.clear();
    } else if (firstError == TILEXR_SUCCESS) {
        firstError = TILEXR_ERROR_INTERNAL;
    }
    return firstError;
}

int TileXRUDMATransport::RollbackAddedContexts(
    const std::map<uint32_t, void*>& previousContexts,
    const std::map<uint32_t, void*>& previousTokens)
{
    const auto contextInUse = [this](uint32_t eidIndex) {
        return std::any_of(fullmeshQpStates_.begin(),
            fullmeshQpStates_.end(),
            [eidIndex](const std::unique_ptr<PerPeerQpState>& state) {
                return state != nullptr && state->localEid == eidIndex;
            });
    };
    int firstError = TILEXR_SUCCESS;
    for (auto it = tokenHandleByEid_.begin();
        it != tokenHandleByEid_.end();) {
        const uint32_t eidIndex = it->first;
        if (previousTokens.count(eidIndex) != 0U) {
            ++it;
            continue;
        }
        const auto ctxIt = ctxHandleByEid_.find(eidIndex);
        if (contextInUse(eidIndex) || ctxIt == ctxHandleByEid_.end() ||
            ctxIt->second == nullptr || it->second == nullptr) {
            if (firstError == TILEXR_SUCCESS) {
                firstError = TILEXR_ERROR_INTERNAL;
            }
            ++it;
            continue;
        }
        const int ret = loader_.RaCtxTokenIdFree(
            ctxIt->second, it->second);
        if (ret != 0) {
            if (firstError == TILEXR_SUCCESS) {
                firstError = TILEXR_ERROR_INTERNAL;
            }
            ++it;
            continue;
        }
        it = tokenHandleByEid_.erase(it);
    }

    for (auto it = ctxHandleByEid_.begin(); it != ctxHandleByEid_.end();) {
        const uint32_t eidIndex = it->first;
        if (previousContexts.count(eidIndex) != 0U) {
            ++it;
            continue;
        }
        if (contextInUse(eidIndex) ||
            tokenHandleByEid_.count(eidIndex) != 0U ||
            it->second == nullptr) {
            if (firstError == TILEXR_SUCCESS) {
                firstError = TILEXR_ERROR_INTERNAL;
            }
            ++it;
            continue;
        }
        const int ret = loader_.RaCtxDeinit(it->second);
        if (ret != 0) {
            if (firstError == TILEXR_SUCCESS) {
                firstError = TILEXR_ERROR_INTERNAL;
            }
            ++it;
            continue;
        }
        it = ctxHandleByEid_.erase(it);
    }
    return firstError;
}

int TileXRUDMATransport::CleanupContexts()
{
    int localRet = TILEXR_SUCCESS;
    for (auto it = tokenHandleByEid_.begin(); it != tokenHandleByEid_.end();) {
        const auto ctxIt = ctxHandleByEid_.find(it->first);
        if (it->second == nullptr) {
            it = tokenHandleByEid_.erase(it);
            continue;
        }
        if (ctxIt == ctxHandleByEid_.end() || ctxIt->second == nullptr ||
            loader_.RaCtxTokenIdFree(ctxIt->second, it->second) != 0) {
            localRet = TILEXR_ERROR_INTERNAL;
            ++it;
            continue;
        }
        it = tokenHandleByEid_.erase(it);
    }
    int ret = AgreeInitStatus(localRet);
    if (ret != TILEXR_SUCCESS) {
        return ret;
    }

    localRet = TILEXR_SUCCESS;
    for (auto it = ctxHandleByEid_.begin(); it != ctxHandleByEid_.end();) {
        if (it->second != nullptr && loader_.RaCtxDeinit(it->second) != 0) {
            localRet = TILEXR_ERROR_INTERNAL;
            ++it;
            continue;
        }
        it = ctxHandleByEid_.erase(it);
    }
    ret = AgreeInitStatus(localRet);
    if (ret != TILEXR_SUCCESS) {
        return ret;
    }

    localRet = TILEXR_SUCCESS;
    if (raInitialized_) {
        RaInitConfig deinitConfig {};
        deinitConfig.phyId = logicDevId_ + deviceIdOffset_;
        deinitConfig.nicPosition = NETWORK_OFFLINE;
        deinitConfig.hdcType = HDC_SERVICE_TYPE_RDMA_V2;
        deinitConfig.enableHdcAsync = 1;
        if (loader_.RaDeinit(&deinitConfig) == 0) {
            raInitialized_ = false;
        } else {
            localRet = TILEXR_ERROR_INTERNAL;
        }
    }
    if (localRet == TILEXR_SUCCESS && tsdOpened_) {
        if (loader_.TsdProcessClose(logicDevId_, subPid_) == 0) {
            tsdOpened_ = false;
            subPid_ = 0;
        } else {
            localRet = TILEXR_ERROR_INTERNAL;
        }
    }
    ret = AgreeInitStatus(localRet);
    if (ret == TILEXR_SUCCESS) {
        raAttached_ = false;
    }
    return ret;
}

int TileXRUDMATransport::Shutdown()
{
    if (shutdownComplete_) {
        return TILEXR_SUCCESS;
    }
    available_ = false;
    udmaInfoDev_ = baseUDMAInfoDev_;
    fullmeshInfoDev_ = baseFullmeshInfoDev_;
    int ret = AgreeInitStatus(TILEXR_SUCCESS);
    if (ret != TILEXR_SUCCESS) {
        return ret;
    }

    int localRet = CleanupAllMemory();
    ret = AgreeInitStatus(localRet);
    if (ret != TILEXR_SUCCESS || HasMemoryCleanupPending()) {
        return ret == TILEXR_SUCCESS ? TILEXR_ERROR_INTERNAL : ret;
    }
    localRet = CleanupAllProfiles();
    ret = AgreeInitStatus(localRet);
    if (ret != TILEXR_SUCCESS || HasProfileCleanupPending()) {
        return ret == TILEXR_SUCCESS ? TILEXR_ERROR_INTERNAL : ret;
    }
    localRet = CleanupQueues();
    ret = AgreeInitStatus(localRet);
    if (ret != TILEXR_SUCCESS) {
        return ret;
    }
    localRet = CleanupFullmeshQueues();
    ret = AgreeInitStatus(localRet);
    if (ret != TILEXR_SUCCESS) {
        return ret;
    }
    ret = CleanupContexts();
    if (ret != TILEXR_SUCCESS) {
        return ret;
    }

    localRet = FreeDeviceInfo(baseUDMAInfoDev_);
    const int baseFullmeshInfoRet = FreeDeviceInfo(baseFullmeshInfoDev_);
    if (localRet == TILEXR_SUCCESS && baseFullmeshInfoRet != TILEXR_SUCCESS) {
        localRet = baseFullmeshInfoRet;
    }
    const int eidTableRet = FreeDeviceInfo(eidTableDev_);
    if (localRet == TILEXR_SUCCESS && eidTableRet != TILEXR_SUCCESS) {
        localRet = eidTableRet;
    }
    ret = AgreeInitStatus(localRet);
    if (ret != TILEXR_SUCCESS) {
        return ret;
    }
    udmaInfoDev_ = nullptr;
    fullmeshInfoDev_ = nullptr;
    udmaInfoSize_ = 0;
    fullmeshInfoSize_ = 0U;
    localEidByEid_.clear();
    peerLocalEid_.clear();
    peerRemoteEid_.clear();
    localRouteByPeerQp_.clear();
    remoteRouteByPeerQp_.clear();
    peerQpStates_.clear();
    sharedQpStates_.clear();
    fullmeshQpStates_.clear();
    fullmeshLocalRouteByPeer_.clear();
    fullmeshRemoteRouteByPeer_.clear();
    explicitConfig_ = false;
    sharedQp_ = false;
    groupedQpProfile_ = false;
    fullmeshAvailable_ = false;
    fullmeshValidPeerMask_ = 0U;
    qpCount_ = 1;
    loader_.Unload();
    shutdownComplete_ = true;
    return TILEXR_SUCCESS;
}

bool TileXRUDMATransport::IsAvailable() const
{
    return available_ && udmaInfoDev_ != nullptr;
}

GM_ADDR TileXRUDMATransport::GetUDMAInfoDev() const
{
    return udmaInfoDev_;
}

GM_ADDR TileXRUDMATransport::GetBaseUDMAInfoDev() const
{
    return baseUDMAInfoDev_;
}

GM_ADDR TileXRUDMATransport::GetFullmeshInfoDev() const
{
    return fullmeshInfoDev_;
}

GM_ADDR TileXRUDMATransport::GetBaseFullmeshInfoDev() const
{
    return baseFullmeshInfoDev_;
}

GM_ADDR TileXRUDMATransport::GetRegisteredMemoryPtr() const
{
    if (activeRegistration_ != nullptr && !activeRegistration_->regions.empty()) {
        return activeRegistration_->regions[0].localPtr;
    }
    if (preparedRegistration_ != nullptr && !preparedRegistration_->regions.empty()) {
        return preparedRegistration_->regions[0].localPtr;
    }
    for (const auto& registration : retiredRegistrations_) {
        if (registration != nullptr && !registration->regions.empty()) {
            return registration->regions[0].localPtr;
        }
    }
    return nullptr;
}

size_t TileXRUDMATransport::GetRegisteredMemoryBytes() const
{
    if (activeRegistration_ != nullptr && !activeRegistration_->regions.empty()) {
        return activeRegistration_->regions[0].bytes;
    }
    if (preparedRegistration_ != nullptr && !preparedRegistration_->regions.empty()) {
        return preparedRegistration_->regions[0].bytes;
    }
    for (const auto& registration : retiredRegistrations_) {
        if (registration != nullptr && !registration->regions.empty()) {
            return registration->regions[0].bytes;
        }
    }
    return 0;
}

bool TileXRUDMATransport::HasMemoryCleanupPending() const
{
    if (preparedRegistration_ != nullptr || !retiredRegistrations_.empty()) {
        return true;
    }
    return activeRegistration_ != nullptr &&
        (udmaInfoDev_ != activeRegistration_->infoDev ||
        (activeRegistration_->fullmeshReady &&
            fullmeshInfoDev_ != activeRegistration_->fullmeshInfoDev));
}

uint32_t TileXRUDMATransport::GetQpCount() const
{
    return IsAvailable() ? qpCount_ : 0U;
}

bool TileXRUDMATransport::UsesSharedQps() const
{
    return IsAvailable() && sharedQp_;
}

bool TileXRUDMATransport::UsesGroupedQpProfile() const
{
    return IsAvailable() && groupedQpProfile_;
}

bool TileXRUDMATransport::HasFullmeshDomain() const
{
    return IsAvailable() && fullmeshAvailable_ &&
        baseFullmeshInfoDev_ != nullptr &&
        fullmeshValidPeerMask_ != 0U;
}

uint32_t TileXRUDMATransport::GetFullmeshValidPeerMask() const
{
    return HasFullmeshDomain() ? fullmeshValidPeerMask_ : 0U;
}

} // namespace TileXR
