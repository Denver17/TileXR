/*
 * Copyright (c) 2024-2026 TileXR Project
 * Licensed under the Apache License, Version 2.0
 */

#include "udma/tilexr_udma_context.h"

#include <acl/acl_rt.h>

#include <algorithm>
#include <array>
#include <limits>
#include <new>
#include <string>
#include <vector>

#include "tilexr_api.h"
#include "tilexr_log.h"
#include "tools/socket/tilexr_sock_exchange.h"
#include "udma/tilexr_udma_transport.h"

namespace TileXR {

struct TileXRUDMAContext::ProfileRecord {
    TileXRUDMAProfileRegistry registry {};
    GM_ADDR registryDev = nullptr;
    bool cleanupPending = false;
};

TileXRUDMAContext::TileXRUDMAContext() = default;

TileXRUDMAContext::~TileXRUDMAContext()
{
    const int ret = Shutdown();
    if (ret != TILEXR_SUCCESS) {
        TILEXR_LOG(ERROR) << "TileXR UDMA context fallback cleanup failed: " << ret;
    }
}

int TileXRUDMAContext::Init(const TileXRUDMAContextOptions& options)
{
    options_ = options;
    if (options_.rankSize <= 1) {
        TILEXR_LOG(INFO) << "InitUDMA skipped for single-rank communicator";
        return TILEXR_SUCCESS;
    }

    UDMAQpConfig qpConfig;
    const int configRet = LoadAndAgreeQpConfig(qpConfig);
    if (configRet != TILEXR_SUCCESS) {
        TILEXR_LOG(WARN) << "TileXR UDMA QP configuration unavailable: " << configRet
                         << ", UDMA disabled";
        return TILEXR_SUCCESS;
    }

    transport_.reset(new (std::nothrow) TileXRUDMATransport());
    const int transportAllocationStatus = AgreeStatus(
        transport_ == nullptr ? TILEXR_ERROR_INTERNAL : TILEXR_SUCCESS);
    if (transportAllocationStatus != TILEXR_SUCCESS) {
        transport_.reset();
        TILEXR_LOG(WARN) << "TileXRUDMATransport allocation failed on at least one rank, UDMA disabled";
        return TILEXR_SUCCESS;
    }

    TileXRUDMATransportOptions transportOptions {};
    transportOptions.rank = options_.rank;
    transportOptions.rankSize = options_.rankSize;
    transportOptions.localRankSize = options_.localRankSize;
    transportOptions.devId = options_.devId;
    transportOptions.nonPinRegistration = options_.nonPinRegistration;
    transportOptions.enableFullmeshDomain = options_.sharedQpDomain;
    transportOptions.exchange = options_.exchange;
    transportOptions.qpConfig = qpConfig;
    int ret = transport_->Init(transportOptions);
    if (ret != TILEXR_SUCCESS || !transport_->IsAvailable()) {
        TILEXR_LOG(WARN) << "TileXR UDMA init failed: " << ret << ", UDMA disabled";
        transport_.reset();
        return TILEXR_SUCCESS;
    }

    udmaInfoDev_ = transport_->GetUDMAInfoDev();
    if (udmaInfoDev_ == nullptr) {
        TILEXR_LOG(WARN) << "TileXR UDMA info is null, UDMA disabled";
        transport_->Shutdown();
        transport_.reset();
        return TILEXR_SUCCESS;
    }

    lifecycle_ = Lifecycle::TransportReady;
    ret = ApplyCommArgsState(GetCommArgsState());
    if (ret != TILEXR_SUCCESS) {
        Shutdown();
        return ret;
    }

    TILEXR_LOG(INFO) << "InitUDMA success, rank " << options_.rank << "/" << options_.rankSize;
    return TILEXR_SUCCESS;
}

int TileXRUDMAContext::Shutdown()
{
    if (lifecycle_ == Lifecycle::Unavailable && transport_ == nullptr) {
        return TILEXR_SUCCESS;
    }
    shutdownStarted_ = true;

    TileXRUDMACommArgsState disabledState {};
    int ret = AgreeStatus(ApplyCommArgsState(disabledState));
    if (ret != TILEXR_SUCCESS) {
        EnterCleanupPending("communicator arguments could not be disabled on every rank");
        return ret;
    }

    ret = AgreeStatus(CleanupAllProfiles());
    if (ret != TILEXR_SUCCESS || !profiles_.empty()) {
        EnterCleanupPending("profile cleanup did not complete on every rank");
        return ret == TILEXR_SUCCESS ? TILEXR_ERROR_INTERNAL : ret;
    }

    const int localMemoryRet = transport_ == nullptr ?
        TILEXR_SUCCESS : transport_->CleanupAllMemory();
    ret = AgreeStatus(localMemoryRet);
    if (ret != TILEXR_SUCCESS ||
        (transport_ != nullptr && transport_->HasMemoryCleanupPending())) {
        EnterCleanupPending("registered-memory cleanup did not complete on every rank");
        return ret == TILEXR_SUCCESS ? TILEXR_ERROR_INTERNAL : ret;
    }

    int localRegistryRet = CleanupAllRegistries();
    const int fullmeshViewRet = FreeDeviceFullmeshView(fullmeshViewDev_);
    if (localRegistryRet == TILEXR_SUCCESS && fullmeshViewRet != TILEXR_SUCCESS) {
        localRegistryRet = fullmeshViewRet;
    }
    const int retiredFullmeshRet = CleanupRetiredFullmeshViews();
    if (localRegistryRet == TILEXR_SUCCESS &&
        retiredFullmeshRet != TILEXR_SUCCESS) {
        localRegistryRet = retiredFullmeshRet;
    }
    ret = AgreeStatus(localRegistryRet);
    if (ret != TILEXR_SUCCESS || udmaRegistryDev_ != nullptr ||
        !retiredRegistryDevs_.empty() || fullmeshViewDev_ != nullptr ||
        !retiredFullmeshViewDevs_.empty()) {
        EnterCleanupPending("device registry cleanup did not complete on every rank");
        return ret == TILEXR_SUCCESS ? TILEXR_ERROR_INTERNAL : ret;
    }

    if (transport_ != nullptr) {
        ret = transport_->Shutdown();
        if (ret != TILEXR_SUCCESS) {
            EnterCleanupPending("transport cleanup did not complete on every rank");
            return ret;
        }
        transport_.reset();
    }

    retiredRegistryDevs_.clear();
    retiredFullmeshViewDevs_.clear();
    profiles_.clear();
    nextProfileHandle_ = 1;
    udmaRegistryDev_ = nullptr;
    fullmeshViewDev_ = nullptr;
    registry_ = TileXRUDMARegistry {};
    fullmeshView_ = TileXRUDMAFullmeshHostView {};
    registrationGeneration_ = 0U;
    registeredPtr_ = nullptr;
    registeredBytes_ = 0;
    udmaInfoDev_ = nullptr;
    lifecycle_ = Lifecycle::Unavailable;
    shutdownStarted_ = false;
    return TILEXR_SUCCESS;
}

bool TileXRUDMAContext::IsAvailable() const
{
    return lifecycle_ != Lifecycle::Unavailable && udmaInfoDev_ != nullptr &&
        transport_ != nullptr && transport_->IsAvailable();
}

TileXRUDMACommArgsState TileXRUDMAContext::GetCommArgsState() const
{
    TileXRUDMACommArgsState state {};
    state.available = IsAvailable();
    state.sharedQp = state.available && transport_->UsesSharedQps();
    state.infoDev = state.available ? udmaInfoDev_ : nullptr;
    state.registryDev = state.available && lifecycle_ == Lifecycle::MemoryReady ? udmaRegistryDev_ : nullptr;
    state.fullmeshAvailable = state.available &&
        lifecycle_ == Lifecycle::MemoryReady &&
        fullmeshViewDev_ != nullptr && fullmeshView_.registrationReady != 0U;
    state.fullmeshViewDev = state.fullmeshAvailable ? fullmeshViewDev_ : nullptr;
    state.registrationGeneration = state.available &&
            lifecycle_ == Lifecycle::MemoryReady ?
        registrationGeneration_ : 0U;
    return state;
}

int TileXRUDMAContext::RegisterMemory(GM_ADDR localPtr, size_t bytes, TileXRUDMAMemHandle* handle)
{
    if (localPtr == nullptr || bytes == 0 || handle == nullptr) {
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    if (!IsAvailable()) {
        TILEXR_LOG(ERROR) << "TileXRUDMARegister called while UDMA is unavailable";
        return TILEXR_ERROR_NOT_FOUND;
    }
    if (options_.threadMode) {
        TILEXR_LOG(ERROR) << "TileXRUDMARegister is not supported in InitThread mode";
        return TILEXR_ERROR_INTERNAL;
    }
    if (options_.exchange == nullptr) {
        TILEXR_LOG(ERROR) << "TileXRUDMARegister requires live socket exchange";
        return TILEXR_ERROR_INTERNAL;
    }
    if (lifecycle_ == Lifecycle::CleanupPending) {
        TILEXR_LOG(ERROR) << "TileXRUDMARegister requires pending UDMA memory cleanup first";
        return TILEXR_ERROR_INTERNAL;
    }

    const TileXRUDMACommArgsState previousState = GetCommArgsState();
    int ret = transport_->PrepareMemory(localPtr, bytes);
    if (ret != TILEXR_SUCCESS) {
        TILEXR_LOG(ERROR) << "TileXR UDMA memory registration failed: " << ret;
        const int localCleanupStatus = transport_->HasMemoryCleanupPending()
            ? TILEXR_ERROR_INTERNAL : TILEXR_SUCCESS;
        const int cleanupStatus = AgreeStatus(localCleanupStatus);
        if (cleanupStatus != TILEXR_SUCCESS) {
            EnterCleanupPending("candidate prepare rollback did not complete on every rank");
        }
        return ret;
    }

    TileXRUDMARegionDesc localRegion {};
    localRegion.base = localPtr;
    localRegion.bytes = bytes;
    std::vector<TileXRUDMARegionDesc> allRegions(options_.rankSize);
    ret = options_.exchange->AllGather(&localRegion, 1, allRegions.data());
    if (ret != TILEXR_SUCCESS) {
        TILEXR_LOG(ERROR) << "TileXRUDMARegister allgather failed: " << ret;
        const int cleanupRet = transport_->AbortPreparedMemory();
        if (cleanupRet != TILEXR_SUCCESS || transport_->HasMemoryCleanupPending()) {
            EnterCleanupPending("candidate rollback failed after region exchange");
        }
        return ret;
    }

    const uint64_t candidateGeneration =
        registrationGeneration_ == std::numeric_limits<uint64_t>::max() ?
        1U : registrationGeneration_ + 1U;
    std::vector<uint64_t> allGenerations(options_.rankSize);
    ret = options_.exchange->AllGather(
        &candidateGeneration, 1, allGenerations.data());
    if (ret != TILEXR_SUCCESS) {
        const int cleanupRet = transport_->AbortPreparedMemory();
        if (cleanupRet != TILEXR_SUCCESS ||
            transport_->HasMemoryCleanupPending()) {
            EnterCleanupPending(
                "candidate rollback failed after generation exchange");
        }
        return ret;
    }
    if (std::any_of(allGenerations.begin(), allGenerations.end(),
            [candidateGeneration](uint64_t generation) {
                return generation != candidateGeneration;
            })) {
        const int cleanupRet = transport_->AbortPreparedMemory();
        if (cleanupRet != TILEXR_SUCCESS ||
            transport_->HasMemoryCleanupPending()) {
            EnterCleanupPending(
                "candidate rollback failed after generation mismatch");
        }
        return TILEXR_ERROR_INTERNAL;
    }

    TileXRUDMARegistry nextRegistry {};
    GM_ADDR nextRegistryDev = nullptr;
    int candidateStatus = TILEXR_SUCCESS;
    if (options_.rankSize <= 0 || options_.rankSize > static_cast<int>(TILEXR_MAX_RANK_SIZE)) {
        candidateStatus = TILEXR_ERROR_PARA_CHECK_FAIL;
    } else {
        nextRegistry.rankSize = static_cast<uint32_t>(options_.rankSize);
        nextRegistry.regionCount = 1;
        for (int i = 0; i < options_.rankSize; ++i) {
            if (allRegions[i].base == nullptr || allRegions[i].bytes == 0) {
                TILEXR_LOG(ERROR) << "TileXRUDMARegister received invalid region from rank " << i;
                candidateStatus = TILEXR_ERROR_PARA_CHECK_FAIL;
                break;
            }
            nextRegistry.regions[i] = allRegions[i];
        }
    }

    if (candidateStatus == TILEXR_SUCCESS) {
        const aclError allocRet = aclrtMalloc(
            reinterpret_cast<void**>(&nextRegistryDev), sizeof(nextRegistry), ACL_MEM_MALLOC_HUGE_FIRST);
        if (allocRet != ACL_SUCCESS) {
            TILEXR_LOG(ERROR) << "aclrtMalloc UDMA registry failed: " << allocRet;
            candidateStatus = TILEXR_ERROR_INTERNAL;
        }
    }

    if (candidateStatus == TILEXR_SUCCESS) {
        const aclError copyRet = aclrtMemcpy(nextRegistryDev, sizeof(nextRegistry),
            &nextRegistry, sizeof(nextRegistry), ACL_MEMCPY_HOST_TO_DEVICE);
        if (copyRet != ACL_SUCCESS) {
            TILEXR_LOG(ERROR) << "aclrtMemcpy UDMA registry failed: " << copyRet;
            candidateStatus = TILEXR_ERROR_INTERNAL;
        }
    }

    const int agreedCandidateStatus = AgreeStatus(candidateStatus);
    if (agreedCandidateStatus != TILEXR_SUCCESS) {
        int cleanupRet = transport_->AbortPreparedMemory();
        const int registryRet = FreeDeviceRegistry(nextRegistryDev);
        if (registryRet != TILEXR_SUCCESS) {
            RetainRegistry(nextRegistryDev);
        }
        if (cleanupRet == TILEXR_SUCCESS && registryRet != TILEXR_SUCCESS) {
            cleanupRet = registryRet;
        }
        const int agreedCleanupStatus = AgreeStatus(cleanupRet);
        if (agreedCleanupStatus != TILEXR_SUCCESS || transport_->HasMemoryCleanupPending() ||
            !retiredRegistryDevs_.empty()) {
            EnterCleanupPending("candidate registry rollback did not complete on every rank");
        }
        return agreedCandidateStatus;
    }

    GM_ADDR nextFullmeshViewDev = nullptr;
    TileXRUDMAFullmeshHostView nextFullmeshView {};
    bool publishFullmesh = transport_->PreparedFullmeshReady() &&
        transport_->HasFullmeshDomain();
    int fullmeshCandidateStatus = TILEXR_SUCCESS;
    if (publishFullmesh) {
        const uint32_t localRank = static_cast<uint32_t>(
            options_.rank % options_.localRankSize);
        const uint32_t validPeerMask =
            transport_->GetFullmeshValidPeerMask();
        TileXRUDMAFullmeshDeviceView deviceView {};
        deviceView.connectedCount = static_cast<uint32_t>(
            __builtin_popcount(validPeerMask));
        deviceView.localRank = localRank;
        deviceView.validPeerMask = validPeerMask;
        deviceView.registrationReady = 1U;
        deviceView.registrationGeneration = candidateGeneration;
        deviceView.infoPtr = reinterpret_cast<uint64_t>(
            transport_->GetPreparedFullmeshInfoDev());
        if (deviceView.infoPtr == 0U || options_.localRankSize <= 1 ||
            validPeerMask != UDMAFullmeshExpectedPeerMask(localRank,
                static_cast<uint32_t>(options_.localRankSize)) ||
            deviceView.connectedCount + 1U !=
                static_cast<uint32_t>(options_.localRankSize)) {
            fullmeshCandidateStatus = TILEXR_ERROR_NOT_INITIALIZED;
        }
        if (fullmeshCandidateStatus == TILEXR_SUCCESS) {
            const aclError allocRet = aclrtMalloc(
                reinterpret_cast<void**>(&nextFullmeshViewDev),
                sizeof(deviceView), ACL_MEM_MALLOC_HUGE_FIRST);
            if (allocRet != ACL_SUCCESS) {
                fullmeshCandidateStatus = TILEXR_ERROR_INTERNAL;
            }
        }
        if (fullmeshCandidateStatus == TILEXR_SUCCESS) {
            const aclError copyRet = aclrtMemcpy(nextFullmeshViewDev,
                sizeof(deviceView), &deviceView, sizeof(deviceView),
                ACL_MEMCPY_HOST_TO_DEVICE);
            if (copyRet != ACL_SUCCESS) {
                fullmeshCandidateStatus = TILEXR_ERROR_INTERNAL;
            }
        }
        if (fullmeshCandidateStatus == TILEXR_SUCCESS) {
            nextFullmeshView.version = TILEXR_UDMA_FULLMESH_VERSION;
            nextFullmeshView.slotCount = TILEXR_UDMA_FULLMESH_SLOT_COUNT;
            nextFullmeshView.connectedCount = deviceView.connectedCount;
            nextFullmeshView.localRank = localRank;
            nextFullmeshView.validPeerMask = validPeerMask;
            nextFullmeshView.registrationReady = 1U;
            nextFullmeshView.registrationGeneration = candidateGeneration;
            nextFullmeshView.infoDev = reinterpret_cast<GM_ADDR>(
                deviceView.infoPtr);
            nextFullmeshView.viewDev = nextFullmeshViewDev;
        }
    }
    const int agreedFullmeshStatus = AgreeStatus(fullmeshCandidateStatus);
    if (agreedFullmeshStatus != TILEXR_SUCCESS) {
        publishFullmesh = false;
        nextFullmeshView = TileXRUDMAFullmeshHostView {};
        const int viewCleanupRet = FreeDeviceFullmeshView(
            nextFullmeshViewDev);
        if (viewCleanupRet != TILEXR_SUCCESS) {
            RetainFullmeshView(nextFullmeshViewDev);
            const int cleanupRet = transport_->AbortPreparedMemory();
            const int registryRet = FreeDeviceRegistry(nextRegistryDev);
            if (registryRet != TILEXR_SUCCESS) {
                RetainRegistry(nextRegistryDev);
            }
            (void)cleanupRet;
            EnterCleanupPending(
                "Fullmesh candidate view cleanup did not complete");
            return viewCleanupRet;
        }
        TILEXR_LOG(WARN) << "TileXR Fullmesh view unavailable for registration generation "
                         << candidateGeneration << ": "
                         << agreedFullmeshStatus;
    }

    TileXRUDMACommArgsState nextState {};
    nextState.available = true;
    nextState.sharedQp = transport_->UsesSharedQps();
    nextState.infoDev = transport_->GetPreparedUDMAInfoDev();
    nextState.registryDev = nextRegistryDev;
    nextState.fullmeshAvailable = publishFullmesh;
    nextState.fullmeshViewDev = publishFullmesh ?
        nextFullmeshViewDev : nullptr;
    nextState.registrationGeneration = candidateGeneration;
    const int localPublishStatus = nextState.infoDev == nullptr
        ? TILEXR_ERROR_NOT_INITIALIZED : ApplyCommArgsState(nextState);
    const int publishStatus = AgreeStatus(localPublishStatus);
    if (publishStatus != TILEXR_SUCCESS) {
        const int localRestoreStatus = ApplyCommArgsState(previousState);
        const int restoreStatus = AgreeStatus(localRestoreStatus);
        if (restoreStatus != TILEXR_SUCCESS) {
            RetainRegistry(nextRegistryDev);
            EnterCleanupPending("candidate publication could not restore the previous comm args");
            return publishStatus;
        }

        int cleanupRet = transport_->AbortPreparedMemory();
        const int registryRet = FreeDeviceRegistry(nextRegistryDev);
        if (registryRet != TILEXR_SUCCESS) {
            RetainRegistry(nextRegistryDev);
        }
        if (cleanupRet == TILEXR_SUCCESS && registryRet != TILEXR_SUCCESS) {
            cleanupRet = registryRet;
        }
        const int fullmeshViewRet = FreeDeviceFullmeshView(
            nextFullmeshViewDev);
        if (fullmeshViewRet != TILEXR_SUCCESS) {
            RetainFullmeshView(nextFullmeshViewDev);
        }
        if (cleanupRet == TILEXR_SUCCESS &&
            fullmeshViewRet != TILEXR_SUCCESS) {
            cleanupRet = fullmeshViewRet;
        }
        const int agreedCleanupStatus = AgreeStatus(cleanupRet);
        if (agreedCleanupStatus != TILEXR_SUCCESS || transport_->HasMemoryCleanupPending() ||
            !retiredRegistryDevs_.empty() ||
            !retiredFullmeshViewDevs_.empty()) {
            EnterCleanupPending("candidate rollback failed after comm args restoration");
        }
        return publishStatus;
    }

    const GM_ADDR nextInfoDev = nextState.infoDev;
    const int localCommitStatus = transport_->CommitPreparedMemory();
    const int commitStatus = AgreeStatus(localCommitStatus);
    if (commitStatus != TILEXR_SUCCESS) {
        RetainRegistry(nextRegistryDev);
        RetainFullmeshView(nextFullmeshViewDev);
        EnterCleanupPending("candidate publication could not commit transport ownership");
        return commitStatus;
    }

    GM_ADDR previousRegistryDev = udmaRegistryDev_;
    udmaRegistryDev_ = nextRegistryDev;
    nextRegistryDev = nullptr;
    RetainRegistry(previousRegistryDev);
    GM_ADDR previousFullmeshViewDev = fullmeshViewDev_;
    fullmeshViewDev_ = nextFullmeshViewDev;
    nextFullmeshViewDev = nullptr;
    RetainFullmeshView(previousFullmeshViewDev);
    udmaInfoDev_ = nextInfoDev;
    registry_ = nextRegistry;
    fullmeshView_ = publishFullmesh ? nextFullmeshView :
        TileXRUDMAFullmeshHostView {};
    fullmeshView_.viewDev = fullmeshViewDev_;
    registrationGeneration_ = candidateGeneration;
    registeredPtr_ = localPtr;
    registeredBytes_ = bytes;
    lifecycle_ = Lifecycle::MemoryReady;
    *handle = 0;

    int cleanupRet = transport_->CleanupRetiredMemory();
    const int registryCleanupRet = CleanupRetiredRegistries();
    if (cleanupRet == TILEXR_SUCCESS && registryCleanupRet != TILEXR_SUCCESS) {
        cleanupRet = registryCleanupRet;
    }
    const int fullmeshCleanupRet = CleanupRetiredFullmeshViews();
    if (cleanupRet == TILEXR_SUCCESS &&
        fullmeshCleanupRet != TILEXR_SUCCESS) {
        cleanupRet = fullmeshCleanupRet;
    }
    const int agreedCleanupStatus = AgreeStatus(cleanupRet);
    if (agreedCleanupStatus != TILEXR_SUCCESS || transport_->HasMemoryCleanupPending() ||
        !retiredRegistryDevs_.empty() ||
        !retiredFullmeshViewDevs_.empty()) {
        EnterCleanupPending("replaced registration cleanup did not complete on every rank");
        return agreedCleanupStatus == TILEXR_SUCCESS ? TILEXR_ERROR_INTERNAL : agreedCleanupStatus;
    }
    return TILEXR_SUCCESS;
}

int TileXRUDMAContext::UnregisterMemory(TileXRUDMAMemHandle handle)
{
    if (handle != 0) {
        return TILEXR_ERROR_NOT_FOUND;
    }

    if (!IsAvailable() || transport_ == nullptr) {
        return TILEXR_ERROR_NOT_FOUND;
    }

    TileXRUDMACommArgsState hiddenState {};
    hiddenState.available = true;
    hiddenState.sharedQp = transport_->UsesSharedQps();
    hiddenState.infoDev = transport_->GetBaseUDMAInfoDev();
    hiddenState.registryDev = nullptr;
    int ret = hiddenState.infoDev == nullptr
        ? TILEXR_ERROR_NOT_INITIALIZED : ApplyCommArgsState(hiddenState);
    ret = AgreeStatus(ret);
    if (ret != TILEXR_SUCCESS) {
        TILEXR_LOG(ERROR) << "TileXRUDMAUnregister failed to clear comm args: " << ret;
        return ret;
    }
    udmaInfoDev_ = hiddenState.infoDev;
    lifecycle_ = Lifecycle::CleanupPending;

    ret = transport_->CleanupAllMemory();
    const int registryRet = CleanupAllRegistries();
    if (ret == TILEXR_SUCCESS && registryRet != TILEXR_SUCCESS) {
        ret = registryRet;
    }
    const int fullmeshViewRet = FreeDeviceFullmeshView(fullmeshViewDev_);
    if (ret == TILEXR_SUCCESS && fullmeshViewRet != TILEXR_SUCCESS) {
        ret = fullmeshViewRet;
    }
    const int retiredFullmeshViewRet = CleanupRetiredFullmeshViews();
    if (ret == TILEXR_SUCCESS &&
        retiredFullmeshViewRet != TILEXR_SUCCESS) {
        ret = retiredFullmeshViewRet;
    }
    if (ret == TILEXR_SUCCESS && (transport_->HasMemoryCleanupPending() ||
        udmaRegistryDev_ != nullptr || !retiredRegistryDevs_.empty() ||
        fullmeshViewDev_ != nullptr || !retiredFullmeshViewDevs_.empty())) {
        ret = TILEXR_ERROR_INTERNAL;
    }
    ret = AgreeStatus(ret);
    if (ret != TILEXR_SUCCESS) {
        TILEXR_LOG(ERROR) << "TileXR UDMA memory unregistration failed: " << ret;
        return ret;
    }

    registeredPtr_ = nullptr;
    registeredBytes_ = 0;
    registry_ = TileXRUDMARegistry {};
    fullmeshView_ = TileXRUDMAFullmeshHostView {};
    lifecycle_ = Lifecycle::TransportReady;
    return TILEXR_SUCCESS;
}

int TileXRUDMAContext::RegisterProfile(
    const TileXRUDMAProfileDesc& desc, TileXRUDMAProfileHandle* handle)
{
    if (handle == nullptr) {
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    *handle = 0;
    if (!IsAvailable() || transport_ == nullptr) {
        TILEXR_LOG(ERROR) << "TileXRUDMAProfileRegister called while UDMA is unavailable";
        return TILEXR_ERROR_NOT_SUPPORT;
    }
    if (options_.threadMode) {
        TILEXR_LOG(ERROR) << "TileXRUDMAProfileRegister is not supported in InitThread mode";
        return TILEXR_ERROR_NOT_SUPPORT;
    }
    if (options_.exchange == nullptr) {
        TILEXR_LOG(ERROR) << "TileXRUDMAProfileRegister requires live socket exchange";
        return TILEXR_ERROR_INTERNAL;
    }
    if (transport_->HasProfileCleanupPending()) {
        TILEXR_LOG(ERROR) << "TileXRUDMAProfileRegister requires pending profile cleanup first";
        return TILEXR_ERROR_INTERNAL;
    }

    const uint32_t qpCount = transport_->GetQpCount();
    int localStatus = UDMAProfileDescValid(&desc, qpCount)
        ? TILEXR_SUCCESS : TILEXR_ERROR_PARA_CHECK_FAIL;
    int ret = AgreeStatus(localStatus);
    if (ret != TILEXR_SUCCESS) {
        return ret;
    }

    std::vector<TileXRUDMAProfileDesc> allDescs(options_.rankSize);
    ret = options_.exchange->AllGather(&desc, 1, allDescs.data());
    if (ret != TILEXR_SUCCESS) {
        return ret;
    }
    localStatus = TILEXR_SUCCESS;
    for (int rank = 0; rank < options_.rankSize; ++rank) {
        if (!UDMAProfileDescValid(&allDescs[rank], qpCount) ||
            !UDMAProfileContractsEqual(allDescs[0], allDescs[rank])) {
            TILEXR_LOG(ERROR) << "TileXR UDMA profile contract mismatch at rank " << rank;
            localStatus = TILEXR_ERROR_PARA_CHECK_FAIL;
            break;
        }
    }
    ret = AgreeStatus(localStatus);
    if (ret != TILEXR_SUCCESS) {
        return ret;
    }

    const TileXRUDMAProfileHandle candidateHandle = NextProfileHandle();
    localStatus = candidateHandle == 0 ? TILEXR_ERROR_INTERNAL : TILEXR_SUCCESS;
    ret = AgreeStatus(localStatus);
    if (ret != TILEXR_SUCCESS) {
        return ret;
    }
    std::vector<TileXRUDMAProfileHandle> allHandles(options_.rankSize);
    ret = options_.exchange->AllGather(&candidateHandle, 1, allHandles.data());
    if (ret != TILEXR_SUCCESS) {
        return ret;
    }
    for (int rank = 0; rank < options_.rankSize; ++rank) {
        if (allHandles[rank] != candidateHandle) {
            localStatus = TILEXR_ERROR_INTERNAL;
            break;
        }
    }
    ret = AgreeStatus(localStatus);
    if (ret != TILEXR_SUCCESS) {
        return ret;
    }

    ret = transport_->PrepareProfile(desc);
    if (ret != TILEXR_SUCCESS) {
        TILEXR_LOG(ERROR) << "TileXR UDMA profile memory registration failed: " << ret;
        return ret;
    }

    std::unique_ptr<ProfileRecord> candidate(new (std::nothrow) ProfileRecord());
    localStatus = candidate == nullptr ? TILEXR_ERROR_INTERNAL : TILEXR_SUCCESS;
    if (candidate != nullptr) {
        candidate->registry.rankSize = static_cast<uint32_t>(options_.rankSize);
        candidate->registry.regionCount = desc.regionCount;
        candidate->registry.qpCount = qpCount;
        candidate->registry.reserved = desc.reserved;
        for (uint32_t qp = 0; qp < qpCount; ++qp) {
            candidate->registry.qpBindings[qp] = desc.qpBindings[qp];
        }
        for (int rank = 0; rank < options_.rankSize; ++rank) {
            for (uint32_t region = 0; region < desc.regionCount; ++region) {
                const size_t index = static_cast<size_t>(rank) *
                    TILEXR_UDMA_PROFILE_MAX_REGIONS + region;
                candidate->registry.regions[index] = allDescs[rank].regions[region];
                candidate->registry.regions[index].registrationBase = nullptr;
                candidate->registry.regions[index].registrationBytes = 0;
            }
        }
        const aclError allocRet = aclrtMalloc(reinterpret_cast<void**>(&candidate->registryDev),
            sizeof(candidate->registry), ACL_MEM_MALLOC_HUGE_FIRST);
        if (allocRet != ACL_SUCCESS) {
            TILEXR_LOG(ERROR) << "aclrtMalloc UDMA profile registry failed: " << allocRet;
            localStatus = TILEXR_ERROR_INTERNAL;
        }
    }
    if (localStatus == TILEXR_SUCCESS) {
        const aclError copyRet = aclrtMemcpy(candidate->registryDev, sizeof(candidate->registry),
            &candidate->registry, sizeof(candidate->registry), ACL_MEMCPY_HOST_TO_DEVICE);
        if (copyRet != ACL_SUCCESS) {
            TILEXR_LOG(ERROR) << "aclrtMemcpy UDMA profile registry failed: " << copyRet;
            localStatus = TILEXR_ERROR_INTERNAL;
        }
    }

    int agreedStatus = AgreeStatus(localStatus);
    if (agreedStatus != TILEXR_SUCCESS) {
        int cleanupRet = transport_->AbortPreparedProfile();
        if (candidate != nullptr) {
            const int registryRet = FreeDeviceRegistry(candidate->registryDev);
            if (registryRet != TILEXR_SUCCESS) {
                RetainRegistry(candidate->registryDev);
            }
            if (cleanupRet == TILEXR_SUCCESS && registryRet != TILEXR_SUCCESS) {
                cleanupRet = registryRet;
            }
        }
        const int agreedCleanupStatus = AgreeStatus(cleanupRet);
        if (agreedCleanupStatus != TILEXR_SUCCESS || transport_->HasProfileCleanupPending()) {
            TILEXR_LOG(ERROR) << "TileXR UDMA profile candidate cleanup remains pending";
        }
        return agreedStatus;
    }

    localStatus = transport_->GetPreparedProfileInfoDev() == nullptr
        ? TILEXR_ERROR_NOT_INITIALIZED
        : transport_->CommitPreparedProfile(candidateHandle);
    agreedStatus = AgreeStatus(localStatus);
    if (agreedStatus != TILEXR_SUCCESS) {
        int cleanupRet = localStatus == TILEXR_SUCCESS
            ? transport_->CleanupProfile(candidateHandle)
            : transport_->AbortPreparedProfile();
        const int registryRet = FreeDeviceRegistry(candidate->registryDev);
        if (registryRet != TILEXR_SUCCESS) {
            RetainRegistry(candidate->registryDev);
        }
        if (cleanupRet == TILEXR_SUCCESS && registryRet != TILEXR_SUCCESS) {
            cleanupRet = registryRet;
        }
        (void)AgreeStatus(cleanupRet);
        return agreedStatus;
    }

    profiles_.emplace(candidateHandle, std::move(candidate));
    nextProfileHandle_ = candidateHandle == std::numeric_limits<TileXRUDMAProfileHandle>::max()
        ? 1U : candidateHandle + 1U;
    *handle = candidateHandle;
    return TILEXR_SUCCESS;
}

int TileXRUDMAContext::UnregisterProfile(TileXRUDMAProfileHandle handle)
{
    const auto it = profiles_.find(handle);
    if (handle == 0 || it == profiles_.end() || transport_ == nullptr) {
        return TILEXR_ERROR_NOT_FOUND;
    }
    ProfileRecord& record = *it->second;
    const bool retrying = record.cleanupPending;
    record.cleanupPending = true;
    int transportRet = transport_->CleanupProfile(handle);
    if (retrying && transportRet == TILEXR_ERROR_NOT_FOUND) {
        transportRet = TILEXR_SUCCESS;
    }
    const int registryRet = FreeDeviceRegistry(record.registryDev);
    int ret = transportRet;
    if (ret == TILEXR_SUCCESS && registryRet != TILEXR_SUCCESS) {
        ret = registryRet;
    }
    ret = AgreeStatus(ret);
    if (ret == TILEXR_SUCCESS) {
        profiles_.erase(it);
    }
    return ret;
}

int TileXRUDMAContext::QueryProfile(
    TileXRUDMAProfileHandle handle, TileXRUDMAProfileView* view) const
{
    if (view == nullptr) {
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    *view = TileXRUDMAProfileView {};
    const auto it = profiles_.find(handle);
    if (handle == 0 || it == profiles_.end() || it->second->cleanupPending ||
        transport_ == nullptr) {
        return TILEXR_ERROR_NOT_FOUND;
    }
    const ProfileRecord& record = *it->second;
    const GM_ADDR infoDev = transport_->GetProfileInfoDev(handle);
    if (infoDev == nullptr || record.registryDev == nullptr ||
        !UDMAProfileRegistryValid(&record.registry, options_.rankSize,
            record.registry.regionCount, transport_->GetQpCount())) {
        return TILEXR_ERROR_NOT_INITIALIZED;
    }
    view->rankSize = record.registry.rankSize;
    view->regionCount = record.registry.regionCount;
    view->qpCount = transport_->GetQpCount();
    view->infoDev = infoDev;
    view->registryDev = record.registryDev;
    view->registryHost = &record.registry;
    return TILEXR_SUCCESS;
}

GM_ADDR TileXRUDMAContext::GetRegistryDev() const
{
    return lifecycle_ == Lifecycle::MemoryReady ? udmaRegistryDev_ : nullptr;
}

const TileXRUDMARegistry* TileXRUDMAContext::GetRegistryHost() const
{
    return lifecycle_ == Lifecycle::MemoryReady && UDMARegistryValid(&registry_, options_.rankSize)
        ? &registry_ : nullptr;
}

uint32_t TileXRUDMAContext::GetQpCount() const
{
    return IsAvailable() ? transport_->GetQpCount() : 0U;
}

bool TileXRUDMAContext::HasGroupedQpProfile() const
{
    return IsAvailable() && transport_->UsesGroupedQpProfile();
}

int TileXRUDMAContext::QueryFullmesh(
    TileXRUDMAFullmeshHostView* view) const
{
    if (view == nullptr) {
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    *view = TileXRUDMAFullmeshHostView {};
    if (!IsAvailable() || lifecycle_ != Lifecycle::MemoryReady ||
        transport_ == nullptr || !transport_->HasFullmeshDomain() ||
        !UDMAFullmeshHostViewValid(fullmeshView_,
            static_cast<uint32_t>(options_.rank % options_.localRankSize),
            static_cast<uint32_t>(options_.localRankSize),
            registrationGeneration_)) {
        return TILEXR_ERROR_NOT_SUPPORT;
    }
    *view = fullmeshView_;
    return TILEXR_SUCCESS;
}

int TileXRUDMAContext::ApplyCommArgsState(const TileXRUDMACommArgsState& state) const
{
    if (options_.updateCommArgs == nullptr) {
        return TILEXR_SUCCESS;
    }
    return options_.updateCommArgs(state, options_.updateCommArgsUserData);
}

int TileXRUDMAContext::LoadAndAgreeQpConfig(UDMAQpConfig& config) const
{
    if (options_.exchange == nullptr || options_.rankSize <= 0) {
        return TILEXR_ERROR_INTERNAL;
    }

    std::string parseError;
    UDMAQpConfigParseStatus parseStatus = UDMAQpConfigParseStatus::SUCCESS;
    if (options_.sharedQpDomain) {
        parseStatus = LoadUDMASharedQpConfigFromEnv(config, &parseError);
    } else {
        parseStatus = LoadUDMAQpConfigFromEnv(config, &parseError);
    }
    if (parseStatus != UDMAQpConfigParseStatus::SUCCESS) {
        TILEXR_LOG(ERROR) << "TileXR UDMA QP route specification is invalid: " << parseError;
    }
    const UDMAQpConfigWireDescriptor localDescriptor =
        BuildUDMAQpConfigWireDescriptor(config, parseStatus);
    std::vector<UDMAQpConfigWireDescriptor> allDescriptors(options_.rankSize);
    const int exchangeRet = options_.exchange->AllGather(&localDescriptor, 1, allDescriptors.data());
    if (exchangeRet != TILEXR_SUCCESS) {
        return exchangeRet;
    }

    std::string descriptorError;
    for (int rank = 0; rank < options_.rankSize; ++rank) {
        if (!ValidateUDMAQpConfigWireDescriptor(allDescriptors[rank], &descriptorError)) {
            TILEXR_LOG(ERROR) << "TileXR UDMA QP descriptor from rank " << rank
                              << " is invalid: " << descriptorError;
            return TILEXR_ERROR_PARA_CHECK_FAIL;
        }
        if (allDescriptors[rank].parseStatus !=
            static_cast<uint32_t>(UDMAQpConfigParseStatus::SUCCESS)) {
            TILEXR_LOG(ERROR) << "TileXR UDMA QP configuration failed on rank " << rank;
            return TILEXR_ERROR_PARA_CHECK_FAIL;
        }
        if (!UDMAQpConfigWireDescriptorsEqual(allDescriptors[0], allDescriptors[rank])) {
            TILEXR_LOG(ERROR) << "TileXR UDMA QP configuration mismatch at rank " << rank;
            return TILEXR_ERROR_PARA_CHECK_FAIL;
        }
    }
    if (!UDMAQpConfigFromWireDescriptor(allDescriptors[0], config, &descriptorError)) {
        TILEXR_LOG(ERROR) << "TileXR UDMA agreed QP descriptor is unusable: " << descriptorError;
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    return TILEXR_SUCCESS;
}

int TileXRUDMAContext::AgreeStatus(int localStatus) const
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

int TileXRUDMAContext::FreeDeviceRegistry(GM_ADDR& registryDev) const
{
    if (registryDev == nullptr) {
        return TILEXR_SUCCESS;
    }
    const aclError ret = aclrtFree(registryDev);
    if (ret != ACL_SUCCESS) {
        TILEXR_LOG(ERROR) << "Free UDMA registry failed: " << ret
                          << ", ptr " << reinterpret_cast<uintptr_t>(registryDev);
        return TILEXR_ERROR_INTERNAL;
    }
    registryDev = nullptr;
    return TILEXR_SUCCESS;
}

int TileXRUDMAContext::FreeDeviceFullmeshView(GM_ADDR& viewDev) const
{
    if (viewDev == nullptr) {
        return TILEXR_SUCCESS;
    }
    const aclError ret = aclrtFree(viewDev);
    if (ret != ACL_SUCCESS) {
        TILEXR_LOG(ERROR) << "Free UDMA Fullmesh view failed: " << ret
                          << ", ptr "
                          << reinterpret_cast<uintptr_t>(viewDev);
        return TILEXR_ERROR_INTERNAL;
    }
    viewDev = nullptr;
    return TILEXR_SUCCESS;
}

int TileXRUDMAContext::CleanupRetiredRegistries()
{
    int firstError = TILEXR_SUCCESS;
    for (auto it = retiredRegistryDevs_.begin(); it != retiredRegistryDevs_.end();) {
        const int ret = FreeDeviceRegistry(*it);
        if (firstError == TILEXR_SUCCESS && ret != TILEXR_SUCCESS) {
            firstError = ret;
        }
        if (*it == nullptr) {
            it = retiredRegistryDevs_.erase(it);
        } else {
            ++it;
        }
    }
    return firstError;
}

int TileXRUDMAContext::CleanupRetiredFullmeshViews()
{
    int firstError = TILEXR_SUCCESS;
    for (auto it = retiredFullmeshViewDevs_.begin();
        it != retiredFullmeshViewDevs_.end();) {
        const int ret = FreeDeviceFullmeshView(*it);
        if (firstError == TILEXR_SUCCESS && ret != TILEXR_SUCCESS) {
            firstError = ret;
        }
        if (*it == nullptr) {
            it = retiredFullmeshViewDevs_.erase(it);
        } else {
            ++it;
        }
    }
    return firstError;
}

int TileXRUDMAContext::CleanupAllRegistries()
{
    int firstError = FreeDeviceRegistry(udmaRegistryDev_);
    const int retiredRet = CleanupRetiredRegistries();
    if (firstError == TILEXR_SUCCESS && retiredRet != TILEXR_SUCCESS) {
        firstError = retiredRet;
    }
    return firstError;
}

int TileXRUDMAContext::CleanupAllProfiles()
{
    int firstError = TILEXR_SUCCESS;
    std::vector<TileXRUDMAProfileHandle> handles;
    handles.reserve(profiles_.size());
    for (const auto& entry : profiles_) {
        handles.push_back(entry.first);
    }
    for (TileXRUDMAProfileHandle handle : handles) {
        const int ret = UnregisterProfile(handle);
        if (firstError == TILEXR_SUCCESS && ret != TILEXR_SUCCESS) {
            firstError = ret;
        }
    }
    return firstError;
}

TileXRUDMAProfileHandle TileXRUDMAContext::NextProfileHandle() const
{
    TileXRUDMAProfileHandle candidate = nextProfileHandle_ == 0 ? 1U : nextProfileHandle_;
    for (size_t checked = 0; checked <= profiles_.size(); ++checked) {
        if (profiles_.count(candidate) == 0) {
            return candidate;
        }
        candidate = candidate == std::numeric_limits<TileXRUDMAProfileHandle>::max()
            ? 1U : candidate + 1U;
    }
    return 0;
}

void TileXRUDMAContext::RetainRegistry(GM_ADDR& registryDev)
{
    if (registryDev == nullptr) {
        return;
    }
    if (std::find(retiredRegistryDevs_.begin(), retiredRegistryDevs_.end(), registryDev) ==
        retiredRegistryDevs_.end()) {
        retiredRegistryDevs_.push_back(registryDev);
    }
    registryDev = nullptr;
}

void TileXRUDMAContext::RetainFullmeshView(GM_ADDR& viewDev)
{
    if (viewDev == nullptr) {
        return;
    }
    if (std::find(retiredFullmeshViewDevs_.begin(),
            retiredFullmeshViewDevs_.end(), viewDev) ==
        retiredFullmeshViewDevs_.end()) {
        retiredFullmeshViewDevs_.push_back(viewDev);
    }
    viewDev = nullptr;
}

void TileXRUDMAContext::EnterCleanupPending(const char* reason)
{
    lifecycle_ = Lifecycle::CleanupPending;
    if (transport_ != nullptr) {
        const GM_ADDR ownedPtr = transport_->GetRegisteredMemoryPtr();
        if (ownedPtr != nullptr) {
            registeredPtr_ = ownedPtr;
            registeredBytes_ = transport_->GetRegisteredMemoryBytes();
        }
    }

    TILEXR_LOG(ERROR) << "TileXR UDMA entered cleanup-pending state: " << reason;
    TileXRUDMACommArgsState hiddenState {};
    hiddenState.available = !shutdownStarted_ && IsAvailable();
    hiddenState.sharedQp = hiddenState.available && transport_ != nullptr &&
        transport_->UsesSharedQps();
    hiddenState.infoDev = hiddenState.available ? udmaInfoDev_ : nullptr;
    hiddenState.registryDev = nullptr;
    const int ret = ApplyCommArgsState(hiddenState);
    if (ret != TILEXR_SUCCESS) {
        TILEXR_LOG(ERROR) << "TileXR UDMA failed to hide registry in cleanup-pending state: " << ret;
    }
}

} // namespace TileXR
