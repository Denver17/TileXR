/*
 * Copyright (c) 2024 Huawei Technologies Co., Ltd.
 * This file is a part of the CANN Open Software.
 * Licensed under CANN Open Software License Agreement Version 1.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#include "tilexr_comm.h"
#include "tilexr_internal.h"
#include "tilexr_profiling.h"
#include "ccu/tilexr_ccu_backend.h"
#include "sdma/tilexr_sdma_transport.h"
#include "udma/tilexr_udma_context.h"

#include <acl/acl_rt.h>
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <vector>
#include <mutex>
#include <map>
#include <set>
#include <thread>
#include <sstream>
#include <iomanip>
#include <limits>
#include <cstring>

#include "tilexr_log.h"
#include "tools/socket/tilexr_sock_exchange.h"

#include "runtime/mem.h"
#include "runtime/dev.h"
#include "runtime/rts/rts_device.h"
#include "runtime/rt_ffts.h"

enum TopologyType : int {
    TOPOLOGY_HCCS = 0,
    TOPOLOGY_PIX,
    TOPOLOGY_PIB,
    TOPOLOGY_PHB,
    TOPOLOGY_SYS,
    TOPOLOGY_SIO,
    TOPOLOGY_HCCS_SW
};

using namespace std;
using namespace chrono;

namespace TileXR {
constexpr int HCCL_IPC_PID_ARRAY_SIZE = 1; // 固定每次只传一个PID数据
constexpr int TILEXR_INIT_TIMEOUT = 600;
static map<string, GM_ADDR [TILEXR_MAX_RANK_SIZE]> g_localPeerMemAllocationMap;
static map<string, int[TILEXR_MAX_RANK_SIZE]> g_devList;
static std::mutex g_mtx;
static std::mutex g_sdmaMtx;
static bool g_sdmaUnavailable = false;

namespace {

uint64_t GetPeerMemBytes(int bufferSize)
{
    return static_cast<uint64_t>(bufferSize) *
        static_cast<uint64_t>(TILEXR_MIB_BYTES) + TILEXR_FLAG_BUFF_BYTES;
}

uint64_t GetPeerMemAllocationBytes(int bufferSize, bool memoryOnlyDomain)
{
    return GetPeerMemBytes(bufferSize) +
        (memoryOnlyDomain ? TILEXR_MEMORY_DOMAIN_EXTRA_ALLOC_BYTES : 0U);
}

uint64_t GetPeerMemRankOffset(int rank, bool memoryOnlyDomain)
{
    return memoryOnlyDomain ? static_cast<uint64_t>(rank) *
        TILEXR_MEMORY_DOMAIN_RANK_OFFSET_BYTES : 0U;
}

GM_ADDR GetPeerMemLogicalAddress(
    GM_ADDR allocation, int rank, bool memoryOnlyDomain)
{
    return allocation == nullptr ? nullptr :
        allocation + GetPeerMemRankOffset(rank, memoryOnlyDomain);
}

bool IsEnvEnabled(const char* name, bool defaultValue)
{
    const char* value = std::getenv(name);
    if (value == nullptr || value[0] == '\0') {
        return defaultValue;
    }
    const std::string text(value);
    if (text == "0" || text == "false" || text == "FALSE" || text == "off" || text == "OFF") {
        return false;
    }
    if (text == "1" || text == "true" || text == "TRUE" || text == "on" || text == "ON") {
        return true;
    }
    TILEXR_LOG(WARN) << name << " has invalid value '" << text << "', using default " << defaultValue;
    return defaultValue;
}

constexpr const char* TILEXR_ENABLE_CCU_BACKEND_ENV = "TILEXR_ENABLE_CCU_BACKEND";

bool ShouldEnableCcuBackend()
{
    const char *value = std::getenv(TILEXR_ENABLE_CCU_BACKEND_ENV);
    if (value == nullptr || value[0] == '\0') {
        return false;
    }
    const string text(value);
    return text == "1" || text == "true" || text == "TRUE" || text == "yes" || text == "on";
}

} // namespace

// 如果是互联的链路，返回false； 对910B2C那些不互联的链路，返回true
bool SkipUnusedChannel910B2C(int curRank, int peerRank, ChipName chipName)
{
    if (chipName == ChipName::CHIP_910B2C) {
        constexpr int rankSizePerNode = 8;
        // 双节点16P中不用的链路: 不在同一个节点 且rank在节点内序号不同； 在调用时将跳过
        if ((curRank / rankSizePerNode != peerRank / rankSizePerNode)
            && (std::abs(curRank - peerRank) != rankSizePerNode)) {
            return true;
        }
    }
    return false;
}

int TileXRComm::InitDumpAddr()
{
    constexpr uint32_t dumpCoreCnt = 75;
    constexpr uint32_t dumpSizePerCore = 1 * 1024 * 1024;
    constexpr uint32_t dumpWorkspaceSize = dumpCoreCnt * dumpSizePerCore;
    GM_ADDR dumpAddr = nullptr;
    int ret = 0;
    ret = aclrtMalloc(reinterpret_cast<void **>(&dumpAddr), dumpWorkspaceSize, ACL_MEM_MALLOC_HUGE_FIRST);
    if (ret != ACL_SUCCESS) {
        TILEXR_LOG(ERROR) << "aclrtMalloc err " << __LINE__ << " " << ret;
        return TILEXR_ERROR_INTERNAL;
    }
    ret = aclrtMemset(dumpAddr, dumpWorkspaceSize, 0, dumpWorkspaceSize);
    if (ret != ACL_SUCCESS) {
        TILEXR_LOG(ERROR) << "aclrtMemset err " << __LINE__ << " " << ret;
        aclrtFree(dumpAddr);
        return TILEXR_ERROR_INTERNAL;
    }
 
    GM_ADDR memory = static_cast<GM_ADDR>(std::malloc(dumpWorkspaceSize));
    if (!memory) {
        TILEXR_LOG(ERROR) << "std::malloc err " << __LINE__;
        aclrtFree(dumpAddr);
        return TILEXR_ERROR_INTERNAL;
    }
    // Zero out allocated memory
    for (size_t i = 0; i < dumpWorkspaceSize; ++i) {
        ((uint8_t*)memory)[i] = 0;
    }
    // 遍历每个block进行初始化
        for (uint32_t i = 0; i < dumpCoreCnt; ++i) {
        // 计算当前block的起始地址
        GM_ADDR blockStart = memory + i * dumpSizePerCore;
        GM_ADDR deviceBlockStart = dumpAddr + i * dumpSizePerCore;
        
        // 初始化BlockInfo
        LcclDumpBlockInfo* blockInfo = reinterpret_cast<LcclDumpBlockInfo*>(blockStart);
        blockInfo->len = dumpSizePerCore;
        blockInfo->core = i;
        blockInfo->blockNum = 0;
        blockInfo->dumpOffset = dumpSizePerCore - sizeof(LcclDumpBlockInfo);
        blockInfo->magic = 0; // 示例魔法值
        blockInfo->dumpAddr = reinterpret_cast<uint64_t>(deviceBlockStart + sizeof(LcclDumpBlockInfo));
    }
 
    ret = aclrtMemcpy(dumpAddr, dumpWorkspaceSize, memory, dumpWorkspaceSize, ACL_MEMCPY_HOST_TO_DEVICE);
    if (ret != ACL_SUCCESS) {
        TILEXR_LOG(ERROR) << "aclrtMemcpy err " << __LINE__ << " " << ret;
        std::free(memory);
        aclrtFree(dumpAddr);
        return TILEXR_ERROR_INTERNAL;
    }
    std::free(memory);
 
    commArgs_.dumpAddr = dumpAddr;
    return TILEXR_SUCCESS;
}

int TileXRComm::InitProfiling()
{
    std::string error;
    if (!LoadTileXRProfilingRuntimeConfig(
            rank_, devId_, commDomain_, &profilingConfig_, &error)) {
        TILEXR_LOG(ERROR) << "Invalid TileXR profiling configuration: " << error;
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    if (!profilingConfig_.enabled) {
        return TILEXR_SUCCESS;
    }

    const uint64_t slotCount =
        static_cast<uint64_t>(profilingConfig_.coreCapacity) *
        profilingConfig_.recordsPerCore;
    if (slotCount > std::numeric_limits<size_t>::max() / sizeof(uint64_t)) {
        TILEXR_LOG(ERROR) << "TileXR profiling allocation size overflow";
        return TILEXR_ERROR_INTERNAL;
    }
    const size_t timestampBytes = static_cast<size_t>(slotCount) * sizeof(uint64_t);
    const size_t eventIdBytes = static_cast<size_t>(slotCount) * sizeof(uint32_t);
    const size_t counterBytes =
        static_cast<size_t>(profilingConfig_.coreCapacity) *
        TILEXR_PROFILING_COUNT_STRIDE * sizeof(uint32_t);

    const auto allocateZeroed = [](GM_ADDR *destination, size_t bytes) -> aclError {
        aclError ret = aclrtMalloc(
            reinterpret_cast<void **>(destination), bytes, ACL_MEM_MALLOC_HUGE_FIRST);
        if (ret != ACL_SUCCESS) {
            return ret;
        }
        ret = aclrtMemset(*destination, bytes, 0, bytes);
        if (ret != ACL_SUCCESS) {
            (void)aclrtFree(*destination);
            *destination = nullptr;
        }
        return ret;
    };

    aclError ret = allocateZeroed(&profilingTimestampsDev_, timestampBytes);
    if (ret == ACL_SUCCESS) {
        ret = allocateZeroed(&profilingEventIdsDev_, eventIdBytes);
    }
    if (ret == ACL_SUCCESS) {
        ret = allocateZeroed(&profilingRecordCountsDev_, counterBytes);
    }
    if (ret == ACL_SUCCESS) {
        ret = allocateZeroed(&profilingDroppedCountsDev_, counterBytes);
    }
    if (ret != ACL_SUCCESS) {
        TILEXR_LOG(ERROR) << "TileXR profiling device allocation failed: " << ret;
        ResetProfilingState();
        return TILEXR_ERROR_INTERNAL;
    }

    commArgs_.tilexrProfilingTimestampsGM = profilingTimestampsDev_;
    commArgs_.tilexrProfilingEventIdsGM = profilingEventIdsDev_;
    commArgs_.tilexrProfilingRecordCountsGM = profilingRecordCountsDev_;
    commArgs_.tilexrProfilingDroppedCountsGM = profilingDroppedCountsDev_;
    commArgs_.tilexrProfilingRecordsPerCore = profilingConfig_.recordsPerCore;
    commArgs_.tilexrProfilingCoreCapacity = profilingConfig_.coreCapacity;
    commArgs_.extraFlag |= ExtraFlag::PROFILING;
    TILEXR_LOG(INFO) << "TileXR profiling enabled: records="
                     << profilingConfig_.recordsPath
                     << ", recordsPerCore=" << profilingConfig_.recordsPerCore;
    return TILEXR_SUCCESS;
}

int TileXRComm::FinalizeProfiling()
{
    if (!profilingConfig_.enabled || profilingFinalized_) {
        return TILEXR_SUCCESS;
    }
    aclError ret = aclrtSynchronizeDevice();
    if (ret != ACL_SUCCESS) {
        TILEXR_LOG(ERROR) << "TileXR profiling device synchronization failed: " << ret;
        return TILEXR_ERROR_INTERNAL;
    }

    const uint64_t slotCount =
        static_cast<uint64_t>(profilingConfig_.coreCapacity) *
        profilingConfig_.recordsPerCore;
    std::vector<uint32_t> occupiedCounts(profilingConfig_.coreCapacity, 0U);
    std::vector<uint32_t> droppedCounts(profilingConfig_.coreCapacity, 0U);
    std::vector<uint32_t> rawCounters(
        static_cast<size_t>(profilingConfig_.coreCapacity) *
        TILEXR_PROFILING_COUNT_STRIDE, 0U);
    std::vector<uint64_t> timestamps(static_cast<size_t>(slotCount), 0U);
    std::vector<uint32_t> eventIds(static_cast<size_t>(slotCount), 0U);

    const size_t counterBytes = rawCounters.size() * sizeof(uint32_t);
    ret = aclrtMemcpy(rawCounters.data(), counterBytes,
        profilingRecordCountsDev_, counterBytes, ACL_MEMCPY_DEVICE_TO_HOST);
    if (ret == ACL_SUCCESS) {
        for (uint32_t core = 0U; core < profilingConfig_.coreCapacity; ++core) {
            occupiedCounts[core] = rawCounters[
                static_cast<size_t>(core) * TILEXR_PROFILING_COUNT_STRIDE];
        }
        ret = aclrtMemcpy(rawCounters.data(), counterBytes,
            profilingDroppedCountsDev_, counterBytes, ACL_MEMCPY_DEVICE_TO_HOST);
    }
    if (ret == ACL_SUCCESS) {
        for (uint32_t core = 0U; core < profilingConfig_.coreCapacity; ++core) {
            droppedCounts[core] = rawCounters[
                static_cast<size_t>(core) * TILEXR_PROFILING_COUNT_STRIDE];
        }
        ret = aclrtMemcpy(timestamps.data(), timestamps.size() * sizeof(uint64_t),
            profilingTimestampsDev_, timestamps.size() * sizeof(uint64_t),
            ACL_MEMCPY_DEVICE_TO_HOST);
    }
    if (ret == ACL_SUCCESS) {
        ret = aclrtMemcpy(eventIds.data(), eventIds.size() * sizeof(uint32_t),
            profilingEventIdsDev_, eventIds.size() * sizeof(uint32_t),
            ACL_MEMCPY_DEVICE_TO_HOST);
    }
    if (ret != ACL_SUCCESS) {
        TILEXR_LOG(ERROR) << "TileXR profiling device capture copy failed: " << ret;
        return TILEXR_ERROR_INTERNAL;
    }

    std::string error;
    if (!WriteTileXRProfilingRecords(profilingConfig_, rank_, occupiedCounts,
            droppedCounts, timestamps, eventIds, &error)) {
        TILEXR_LOG(ERROR) << "TileXR profiling records export failed: " << error;
        return TILEXR_ERROR_INTERNAL;
    }
    TILEXR_LOG(INFO) << "TileXR profiling records written to "
                     << profilingConfig_.recordsPath;
    profilingFinalized_ = true;
    return TILEXR_SUCCESS;
}

void TileXRComm::ResetProfilingState()
{
    FreePeerMem(profilingTimestampsDev_);
    FreePeerMem(profilingEventIdsDev_);
    FreePeerMem(profilingRecordCountsDev_);
    FreePeerMem(profilingDroppedCountsDev_);
    commArgs_.tilexrProfilingTimestampsGM = nullptr;
    commArgs_.tilexrProfilingEventIdsGM = nullptr;
    commArgs_.tilexrProfilingRecordCountsGM = nullptr;
    commArgs_.tilexrProfilingDroppedCountsGM = nullptr;
    commArgs_.tilexrProfilingRecordsPerCore = 0U;
    commArgs_.tilexrProfilingCoreCapacity = 0U;
    commArgs_.extraFlag &= ~ExtraFlag::PROFILING;
    profilingConfig_ = TileXRProfilingRuntimeConfig {};
}

int TileXRComm::InitUDMA()
{
    udmaContext_.reset(new (nothrow) TileXRUDMAContext());
    if (udmaContext_ == nullptr) {
        TILEXR_LOG(WARN) << "TileXRUDMAContext allocation failed, UDMA disabled";
        return TILEXR_SUCCESS;
    }

    TileXRUDMAContextOptions options {};
    options.rank = rank_;
    options.rankSize = rankSize_;
    options.localRankSize = static_cast<int>(localRankSize_);
    options.devId = devId_;
    options.sharedQpDomain = sharedQpDomain_;
    options.nonPinRegistration =
        physicalInfo_.chipName == ChipName::CHIP_950 ||
        physicalInfo_.chipName == ChipName::CHIP_950PR;
    options.exchange = socketExchange_;
    options.threadMode = !uid_.empty();
    options.updateCommArgs = &TileXRComm::ApplyUDMACommArgsStateCallback;
    options.updateCommArgsUserData = this;

    const int ret = udmaContext_->Init(options);
    if (ret != TILEXR_SUCCESS) {
        udmaContext_.reset();
    }
    return ret;
}

int TileXRComm::InitUDMAPeerMemory()
{
    if (!udmaPeerMemMode_ || udmaContext_ == nullptr ||
        !udmaContext_->IsAvailable()) {
        return TILEXR_SUCCESS;
    }
    if (peerMem_[rank_] == nullptr) {
        TILEXR_LOG(WARN) << "TileXR UDMA peer-memory registration skipped: local IPC memory is null";
        udmaContext_->Shutdown();
        udmaContext_.reset();
        return TILEXR_SUCCESS;
    }

    const size_t peerMemBytes =
        static_cast<size_t>(bufferSize_) * 1024U * 1024U +
        static_cast<size_t>(TILEXR_FLAG_BUFF_BYTES);
    TileXRUDMAMemHandle handle = 0;
    const int ret = udmaContext_->RegisterMemory(
        peerMem_[rank_], peerMemBytes, &handle);
    if (ret != TILEXR_SUCCESS) {
        TILEXR_LOG(WARN) << "TileXR UDMA peer-memory registration failed: "
                         << ret << ", UDMA disabled";
        udmaContext_->Shutdown();
        udmaContext_.reset();
        return TILEXR_SUCCESS;
    }

    TILEXR_LOG(INFO) << "TileXR UDMA registered local peer memory, base "
                     << static_cast<void *>(peerMem_[rank_]) << ", bytes "
                     << peerMemBytes;
    return TILEXR_SUCCESS;
}

int TileXRComm::ApplyUDMACommArgsState(const TileXRUDMACommArgsState &state)
{
    const uint32_t oldExtraFlag = commArgs_.extraFlag;
    const GM_ADDR oldInfoPtr = commArgs_.udmaInfoPtr;
    const GM_ADDR oldRegistryPtr = commArgs_.udmaRegistryPtr;
    const GM_ADDR oldFullmeshPtr = commArgs_.udmaFullmeshPtr;
    const uint64_t oldRegistrationGeneration =
        commArgs_.udmaRegistrationGeneration;

    if (state.available && state.infoDev != nullptr) {
        commArgs_.extraFlag |= ExtraFlag::UDMA;
        if (state.sharedQp) {
            commArgs_.extraFlag |= ExtraFlag::UDMA_SHARED_QP;
        } else {
            commArgs_.extraFlag &= ~ExtraFlag::UDMA_SHARED_QP;
        }
        commArgs_.udmaInfoPtr = state.infoDev;
        commArgs_.udmaRegistryPtr = state.registryDev;
        commArgs_.udmaRegistrationGeneration =
            state.registrationGeneration;
        if (state.fullmeshAvailable && state.fullmeshViewDev != nullptr) {
            commArgs_.extraFlag |= ExtraFlag::UDMA_FULLMESH;
            commArgs_.udmaFullmeshPtr = state.fullmeshViewDev;
        } else {
            commArgs_.extraFlag &= ~ExtraFlag::UDMA_FULLMESH;
            commArgs_.udmaFullmeshPtr = nullptr;
        }
    } else {
        commArgs_.extraFlag &= ~(ExtraFlag::UDMA |
            ExtraFlag::UDMA_SHARED_QP | ExtraFlag::UDMA_FULLMESH);
        commArgs_.udmaInfoPtr = nullptr;
        commArgs_.udmaRegistryPtr = nullptr;
        commArgs_.udmaFullmeshPtr = nullptr;
        commArgs_.udmaRegistrationGeneration = 0U;
    }

    if (commArgsPtr_ == nullptr) {
        return TILEXR_SUCCESS;
    }

    const int ret = UpdateCommArgsDev();
    if (ret != TILEXR_SUCCESS) {
        commArgs_.extraFlag = oldExtraFlag;
        commArgs_.udmaInfoPtr = oldInfoPtr;
        commArgs_.udmaRegistryPtr = oldRegistryPtr;
        commArgs_.udmaFullmeshPtr = oldFullmeshPtr;
        commArgs_.udmaRegistrationGeneration = oldRegistrationGeneration;
        TILEXR_LOG(ERROR) << "TileXR UDMA update comm args failed: " << ret;
        return ret;
    }
    return TILEXR_SUCCESS;
}

int TileXRComm::ApplyUDMACommArgsStateCallback(const TileXRUDMACommArgsState &state, void *userData)
{
    if (userData == nullptr) {
        TILEXR_LOG(ERROR) << "ApplyUDMACommArgsStateCallback missing user data";
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    return static_cast<TileXRComm *>(userData)->ApplyUDMACommArgsState(state);
}

int TileXRComm::InitCcuBackend()
{
    if (ccuBackend_ == nullptr) {
        ccuBackend_.reset(new (nothrow) TileXRCcuBackend());
        if (ccuBackend_ == nullptr) {
            return TILEXR_ERROR_INTERNAL;
        }
    }

    TileXRCcuBackendOptions options {};
    options.rank = rank_;
    options.rankSize = rankSize_;
    options.devId = devId_;
    options.uid = uid_;
    options.exchange = socketExchange_;
    return ccuBackend_->Init(options);
}

int TileXRComm::InitCcuBackendIfEnabled()
{
    if (!ShouldEnableCcuBackend()) {
        return TILEXR_SUCCESS;
    }

    const int ccuRet = InitCcuBackend();
    if (ccuRet != TILEXR_SUCCESS || ccuBackend_ == nullptr || !ccuBackend_->Available()) {
        TILEXR_LOG(WARN) << "TileXR CCU backend init failed, direct CCU disabled, ret: " << ccuRet;
        if (ccuBackend_ != nullptr) {
            ccuBackend_->Shutdown();
            ccuBackend_.reset();
        }
        return TILEXR_SUCCESS;
    }

    TILEXR_LOG(INFO) << "TileXR CCU backend initialized";
    return TILEXR_SUCCESS;
}

TileXRCcuBackend *TileXRComm::GetCcuBackendForCollectives()
{
    return ccuBackend_.get();
}

const TileXRCcuBackend *TileXRComm::GetCcuBackendForCollectives() const
{
    return ccuBackend_.get();
}

int TileXRComm::EnableCcuBackendForTest()
{
    return InitCcuBackend();
}

int TileXRComm::InitSDMA()
{
    {
        lock_guard<mutex> lock(g_sdmaMtx);
        if (g_sdmaUnavailable) {
            TILEXR_LOG(INFO) << "InitSDMA skipped after previous SDMA init failure";
            sdmaInitStatus_ = SDMAInitStatus::PTO_UNAVAILABLE;
            return TILEXR_SUCCESS;
        }
    }

    sdmaTransport_.reset(new (nothrow) TileXRSDMATransport());
    if (sdmaTransport_ == nullptr) {
        TILEXR_LOG(WARN) << "TileXRSDMATransport allocation failed, SDMA disabled";
        sdmaInitStatus_ = SDMAInitStatus::INIT_FAILED;
        return TILEXR_SUCCESS;
    }

    TileXRSDMATransportOptions options {};
    options.devId = devId_;
    int ret = sdmaTransport_->Init(options);
    sdmaInitStatus_ = sdmaTransport_->GetLastStatus();
    if (ret != TILEXR_SUCCESS || !sdmaTransport_->IsAvailable()) {
        if (sdmaInitStatus_ != SDMAInitStatus::DISABLED_BY_ENV) {
            TILEXR_LOG(WARN) << "TileXR SDMA init unavailable, status " << static_cast<int>(sdmaInitStatus_);
            lock_guard<mutex> lock(g_sdmaMtx);
            g_sdmaUnavailable = true;
        }
        sdmaTransport_.reset();
        sdmaWorkspaceDev_ = nullptr;
        commArgs_.sdmaWorkspacePtr = nullptr;
        return TILEXR_SUCCESS;
    }

    sdmaWorkspaceDev_ = sdmaTransport_->GetWorkspaceDev();
    if (sdmaWorkspaceDev_ == nullptr) {
        TILEXR_LOG(WARN) << "TileXR SDMA workspace is null, SDMA disabled";
        sdmaInitStatus_ = SDMAInitStatus::NULL_WORKSPACE;
        commArgs_.extraFlag &= ~ExtraFlag::SDMA;
        commArgs_.sdmaWorkspacePtr = nullptr;
        sdmaWorkspaceDev_ = nullptr;
        sdmaTransport_.reset();
        return TILEXR_SUCCESS;
    }

    commArgs_.sdmaWorkspacePtr = sdmaWorkspaceDev_;
    commArgs_.extraFlag |= ExtraFlag::SDMA;
    sdmaInitStatus_ = SDMAInitStatus::INITIALIZED;
    TILEXR_LOG(INFO) << "InitSDMA success, workspace " << static_cast<void*>(sdmaWorkspaceDev_);
    return TILEXR_SUCCESS;
}

void TileXRComm::ResetSDMAState()
{
    commArgs_.extraFlag &= ~ExtraFlag::SDMA;
    commArgs_.sdmaWorkspacePtr = nullptr;
    sdmaWorkspaceDev_ = nullptr;
    sdmaInitStatus_ = SDMAInitStatus::DISABLED_BY_ENV;
    if (sdmaTransport_ != nullptr) {
        sdmaTransport_->Shutdown();
        sdmaTransport_.reset();
    }
}

bool TileXRComm::IsSDMAAvailable() const
{
    return (commArgs_.extraFlag & ExtraFlag::SDMA) != 0 && commArgs_.sdmaWorkspacePtr != nullptr;
}

GM_ADDR TileXRComm::GetSDMAWorkspacePtr() const
{
    return sdmaWorkspaceDev_;
}

SDMAInitStatus TileXRComm::GetSDMAInitStatus() const
{
    return sdmaInitStatus_;
}

int TileXRComm::SyncCommArgs()
{
    commArgs_.rank = rank_;
    commArgs_.localRank = localRank_;
    commArgs_.rankSize = rankSize_;
    commArgs_.localRankSize = localRankSize_;
    commArgs_.commDomain = commDomain_;
    commArgs_.peerMemBytes = GetPeerMemBytes(bufferSize_);
    for (int i = 0; i < rankSize_; ++i) {
        commArgs_.peerMems[i] = peerMem_[i];    // 这里不会越界，之前有逻辑校验过越界了
    }
    for (int i = 0; i < rankSize_; ++i) {
        commArgs_.creditMems[i] = creditIpcMem_[i];
    }

    if (isEnableMsprofOp_) {
        if (InitDumpAddr() != TILEXR_SUCCESS) {
            return TILEXR_ERROR_INTERNAL;
        }

        uint64_t fftsVal = 0;
        uint32_t fftsLen = 0;
        int error = rtGetC2cCtrlAddr(&fftsVal, &fftsLen);
        if (error != RT_ERROR_NONE) {
            TILEXR_LOG(ERROR) << "rtGetC2cCtrlAddr err:" << error;
            return TILEXR_ERROR_MKIRT;
        }
        commArgs_.fftsVal = fftsVal;
    }

    int ret = 0;
    ret = aclrtMalloc(reinterpret_cast<void **>(&commArgsPtr_), sizeof(commArgs_), ACL_MEM_MALLOC_HUGE_FIRST);
    if (ret != ACL_SUCCESS) {
        TILEXR_LOG(ERROR) << "aclrtMalloc err " << __LINE__ << " " << ret;
        return TILEXR_ERROR_INTERNAL;
    }
    ret = aclrtMemcpy(commArgsPtr_, sizeof(commArgs_), &commArgs_, sizeof(commArgs_), ACL_MEMCPY_HOST_TO_DEVICE);
    if (ret != ACL_SUCCESS) {
        TILEXR_LOG(ERROR) << "aclrtMemcpy err " << __LINE__ << " " << ret;
        FreePeerMem(commArgsPtr_);
        return TILEXR_ERROR_INTERNAL;
    }
    return TILEXR_SUCCESS;
}

int TileXRComm::UpdateCommArgsDev()
{
    if (commArgsPtr_ == nullptr) {
        return TILEXR_SUCCESS;
    }
    int ret = aclrtMemcpy(commArgsPtr_, sizeof(commArgs_), &commArgs_, sizeof(commArgs_), ACL_MEMCPY_HOST_TO_DEVICE);
    if (ret != ACL_SUCCESS) {
        TILEXR_LOG(ERROR) << "aclrtMemcpy update comm args err " << ret;
        return TILEXR_ERROR_INTERNAL;
    }
    return TILEXR_SUCCESS;
}

int TileXRComm::RegisterUDMAMemory(GM_ADDR localPtr, size_t bytes, TileXRUDMAMemHandle *handle)
{
    if (!inited_) {
        TILEXR_LOG(ERROR) << "TileXRUDMARegister requires initialized communicator";
        return TILEXR_ERROR_NOT_INITIALIZED;
    }
    if (localPtr == nullptr || bytes == 0 || handle == nullptr) {
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    if (udmaPeerMemMode_) {
        TILEXR_LOG(ERROR) << "TileXRUDMARegister cannot replace communicator-owned peer memory";
        return TILEXR_ERROR_NOT_SUPPORT;
    }
    if (udmaContext_ == nullptr) {
        TILEXR_LOG(ERROR) << "TileXRUDMARegister called while UDMA is unavailable";
        return TILEXR_ERROR_NOT_FOUND;
    }
    return udmaContext_->RegisterMemory(localPtr, bytes, handle);
}

int TileXRComm::UnregisterUDMAMemory(TileXRUDMAMemHandle handle)
{
    if (udmaPeerMemMode_) {
        TILEXR_LOG(ERROR) << "TileXRUDMAUnregister cannot release communicator-owned peer memory";
        return TILEXR_ERROR_NOT_SUPPORT;
    }
    if (handle != 0) {
        return TILEXR_ERROR_NOT_FOUND;
    }
    if (udmaContext_ == nullptr) {
        commArgs_.udmaRegistryPtr = nullptr;
        return UpdateCommArgsDev();
    }
    return udmaContext_->UnregisterMemory(handle);
}

int TileXRComm::RegisterUDMAProfile(
    const TileXRUDMAProfileDesc &desc, TileXRUDMAProfileHandle *handle)
{
    if (!inited_) {
        TILEXR_LOG(ERROR) << "TileXRUDMAProfileRegister requires initialized communicator";
        return TILEXR_ERROR_NOT_INITIALIZED;
    }
    if (handle == nullptr) {
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    if (udmaContext_ == nullptr || !udmaContext_->IsAvailable()) {
        TILEXR_LOG(ERROR) << "TileXRUDMAProfileRegister called while UDMA is unavailable";
        return TILEXR_ERROR_NOT_SUPPORT;
    }
    return udmaContext_->RegisterProfile(desc, handle);
}

int TileXRComm::UnregisterUDMAProfile(TileXRUDMAProfileHandle handle)
{
    if (handle == 0 || udmaContext_ == nullptr) {
        return TILEXR_ERROR_NOT_FOUND;
    }
    return udmaContext_->UnregisterProfile(handle);
}

int TileXRComm::QueryUDMAProfile(
    TileXRUDMAProfileHandle handle, TileXRUDMAProfileView *view) const
{
    if (view == nullptr || handle == 0) {
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    if (udmaContext_ == nullptr || !udmaContext_->IsAvailable()) {
        return TILEXR_ERROR_NOT_SUPPORT;
    }
    return udmaContext_->QueryProfile(handle, view);
}

int TileXRComm::GetUDMAQpCount(uint32_t *qpCount) const
{
    if (qpCount == nullptr) {
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    *qpCount = 0;
    if (udmaContext_ == nullptr || !udmaContext_->IsAvailable()) {
        return TILEXR_ERROR_NOT_SUPPORT;
    }
    *qpCount = udmaContext_->GetQpCount();
    return *qpCount == 0 ? TILEXR_ERROR_NOT_SUPPORT : TILEXR_SUCCESS;
}

bool TileXRComm::HasUDMAGroupedQpProfile() const
{
    return udmaContext_ != nullptr && udmaContext_->HasGroupedQpProfile();
}

int TileXRComm::QueryUDMAFullmesh(
    TileXRUDMAFullmeshHostView *view) const
{
    if (view == nullptr) {
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    *view = TileXRUDMAFullmeshHostView {};
    if (udmaContext_ == nullptr || !udmaContext_->IsAvailable()) {
        return TILEXR_ERROR_NOT_SUPPORT;
    }
    return udmaContext_->QueryFullmesh(view);
}

GM_ADDR TileXRComm::GetUDMARegistryPtr() const
{
    return udmaContext_ == nullptr ? nullptr : udmaContext_->GetRegistryDev();
}

const TileXRUDMARegistry* TileXRComm::GetUDMARegistryHost() const
{
    return udmaContext_ == nullptr ? nullptr : udmaContext_->GetRegistryHost();
}

bool TileXRComm::IsUdmaAvailableForCollectives() const
{
    return udmaContext_ != nullptr && udmaContext_->IsAvailable();
}

int TileXRComm::InitCommon()
{
    // enable peer device
    if (EnablePeerAccess() != TILEXR_SUCCESS) {
        TILEXR_LOG(ERROR) << "EnablePeerAccess failed!";
        return TILEXR_ERROR_INTERNAL;
    }
    const char *lcclDeterministic = std::getenv("LCCL_DETERMINISTIC");
    if (lcclDeterministic && (string(lcclDeterministic) == "1" || string(lcclDeterministic) == "true")) {
        commArgs_.extraFlag |= ExtraFlag::DETERMINISTIC;
    }
    if (GetChipName() == ChipName::CHIP_910B2C) {
        commArgs_.extraFlag |= ExtraFlag::TOPO_910B2C;
    }
    if (GetChipName() >= ChipName::CHIP_910_9391) {
        commArgs_.extraFlag |= ExtraFlag::TOPO_910_93;
    }
    if (GetChipName() > ChipName::CHIP_910_9362) {
        commArgs_.extraFlag |= ExtraFlag::TOPO_910A5;
    }
    constexpr uint32_t AI_CORE_NUM_20 = 20;
    if (GetCoreNum(GetChipName()) > AI_CORE_NUM_20) {
        commArgs_.extraFlag |= ExtraFlag::IS_GREATER_THAN_40_AIV;
    }

    localRank_ = rank_ % localRankSize_;
    return TILEXR_SUCCESS;
}

void TileXRComm::CloseIpcMem()
{
    for (int i = 0; i < rankSize_; ++i) {
        if (i == rank_ || peerMemAllocation_[i] == nullptr) {
            continue;
        }
        int ret = rtIpcCloseMemory(static_cast<void *>(peerMemAllocation_[i]));
        if (ret != RT_ERROR_NONE) {
            TILEXR_LOG(WARN) << "Close ipc[" << i << "] memory failed! ret: " << ret;
        }
        peerMemAllocation_[i] = nullptr;
        peerMem_[i] = nullptr;
    }
}

void TileXRComm::CloseCreditIpcMem()
{
    for (int i = 0; i < rankSize_; ++i) {
        if (i == rank_ || creditIpcMem_[i] == nullptr) {
            continue;
        }
        const int ret = rtIpcCloseMemory(static_cast<void *>(creditIpcMem_[i]));
        if (ret != RT_ERROR_NONE) {
            TILEXR_LOG(WARN) << "Close credit ipc[" << i
                << "] memory failed! ret: " << ret;
        }
        creditIpcMem_[i] = nullptr;
    }
}

void TileXRComm::FreePeerMem(GM_ADDR &mem) const
{
    if (mem != nullptr) {
        aclError aclRet = aclrtFree(mem);
        if (aclRet != ACL_SUCCESS) {
            TILEXR_LOG(ERROR) << "Free share memory failed! ret: " << aclRet;
        }
    }
    mem = nullptr;
}

int TileXRComm::Init()
{
    if (inited_) {
        return TILEXR_SUCCESS;
    }
    if (rank_ < 0 || rank_ >= rankSize_ || rankSize_ <= 0 || rankSize_ > TILEXR_MAX_RANK_SIZE) {
        TILEXR_LOG(ERROR) << "The rank is invalid! rank:" << rank_ << " rankSize:" << rankSize_;
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    if (TileXRSockExchange::CheckValid(commId_)) {
        socketExchange_ = new (nothrow) TileXRSockExchange(rank_, rankSize_, commId_);
    } else {
        socketExchange_ = new (nothrow) TileXRSockExchange(rank_, rankSize_, commDomain_);
    }
    if (socketExchange_ == nullptr) {
        TILEXR_LOG(ERROR) << "TileXRSockExchange create failed. rank : " << rank_ << " rankSize:" << rankSize_;
        return TILEXR_ERROR_INTERNAL;
    }
    int ret = GetDev();
    if (ret != TILEXR_SUCCESS) {
        TILEXR_LOG(ERROR) << "init context failed! ret: " << ret;
        return ret;
    }

    TILEXR_LOG(INFO) << "rank " << rank_ << "/" << rankSize_ << " running devId:" << devId_;

    if (InitCommon() != TILEXR_SUCCESS) {
        TILEXR_LOG(ERROR) << "init common failed!";
        return TILEXR_ERROR_INTERNAL;
    }

    ret = InitProfiling();
    if (ret != TILEXR_SUCCESS) {
        return ret;
    }

    const int32_t localIpcEnabled = memoryOnlyDomain_ ? 1 :
        (IsEnvEnabled("TILEXR_ENABLE_IPC", true) ? 1 : 0);
    std::vector<int32_t> allIpcEnabled(rankSize_, -1);
    ret = socketExchange_->AllGather(&localIpcEnabled, 1, allIpcEnabled.data());
    if (ret != TILEXR_SUCCESS) {
        TILEXR_LOG(ERROR) << "Failed to agree TILEXR_ENABLE_IPC across ranks: " << ret;
        return ret;
    }
    if (std::any_of(allIpcEnabled.begin(), allIpcEnabled.end(),
            [localIpcEnabled](int32_t enabled) { return enabled != localIpcEnabled; })) {
        TILEXR_LOG(ERROR) << "TILEXR_ENABLE_IPC must match on every rank";
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }

    if (localIpcEnabled != 0) {
        TILEXR_LOG(DEBUG) << "Prepare to InitCommMem localRankSize_ -> " << localRankSize_
                          << ", localRank_ -> " << localRank_;
        if (InitCommMem() != TILEXR_SUCCESS) {
            TILEXR_LOG(ERROR) << "InitCommMem failed!";
            return TILEXR_ERROR_INTERNAL;
        }
        TILEXR_LOG(DEBUG) << "InitCommMem " << rank_ << "/" << rankSize_ << ", localRank_ : " << localRank_
                          << ", localRankSize_ : " << localRankSize_ << " success";
    } else {
        TILEXR_LOG(INFO) << "TileXR IPC memory disabled by TILEXR_ENABLE_IPC";
    }

    const int32_t localUdmaPeerMemRequested = memoryOnlyDomain_ ? 0 :
        (IsEnvEnabled("TILEXR_UDMA_USE_PEER_MEM", false) ? 1 : 0);
    std::vector<int32_t> allUdmaPeerMemRequested(rankSize_, -1);
    ret = socketExchange_->AllGather(&localUdmaPeerMemRequested, 1,
        allUdmaPeerMemRequested.data());
    if (ret != TILEXR_SUCCESS) {
        TILEXR_LOG(ERROR) << "Failed to agree TILEXR_UDMA_USE_PEER_MEM across ranks: " << ret;
        return ret;
    }
    if (std::any_of(allUdmaPeerMemRequested.begin(), allUdmaPeerMemRequested.end(),
            [localUdmaPeerMemRequested](int32_t enabled) {
                return enabled != localUdmaPeerMemRequested;
            })) {
        TILEXR_LOG(ERROR) << "TILEXR_UDMA_USE_PEER_MEM must match on every rank";
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    udmaPeerMemMode_ = localUdmaPeerMemRequested != 0 && localIpcEnabled != 0;
    if (localUdmaPeerMemRequested != 0 && !udmaPeerMemMode_) {
        TILEXR_LOG(WARN) << "TILEXR_UDMA_USE_PEER_MEM requires IPC; peer-memory UDMA disabled";
    }

    const int32_t localCreditIpcEnabled = memoryOnlyDomain_ ? 0 :
        IsEnvEnabled("TILEXR_ENABLE_CREDIT_IPC", false) ? 1 : 0;
    std::vector<int32_t> allCreditIpcEnabled(rankSize_, -1);
    ret = socketExchange_->AllGather(
        &localCreditIpcEnabled, 1, allCreditIpcEnabled.data());
    if (ret != TILEXR_SUCCESS) {
        TILEXR_LOG(ERROR) << "Failed to agree TILEXR_ENABLE_CREDIT_IPC across ranks: " << ret;
        return ret;
    }
    if (std::any_of(allCreditIpcEnabled.begin(), allCreditIpcEnabled.end(),
            [localCreditIpcEnabled](int32_t enabled) {
                return enabled != localCreditIpcEnabled;
            })) {
        TILEXR_LOG(ERROR) << "TILEXR_ENABLE_CREDIT_IPC must match on every rank";
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    if (localCreditIpcEnabled != 0) {
        ret = InitCreditCommMem();
        if (ret != TILEXR_SUCCESS) {
            TILEXR_LOG(ERROR) << "InitCreditCommMem failed! ret: " << ret;
            return ret;
        }
        TILEXR_LOG(INFO) << "Dedicated credit IPC memory initialized";
    }

    const int32_t localUdmaEnabled = memoryOnlyDomain_ ? 0 :
        (IsEnvEnabled("TILEXR_ENABLE_UDMA", true) ? 1 : 0);
    std::vector<int32_t> allUdmaEnabled(rankSize_, -1);
    ret = socketExchange_->AllGather(&localUdmaEnabled, 1, allUdmaEnabled.data());
    if (ret != TILEXR_SUCCESS) {
        TILEXR_LOG(ERROR) << "Failed to agree TILEXR_ENABLE_UDMA across ranks: " << ret;
        return ret;
    }
    if (std::any_of(allUdmaEnabled.begin(), allUdmaEnabled.end(),
            [localUdmaEnabled](int32_t enabled) { return enabled != localUdmaEnabled; })) {
        TILEXR_LOG(ERROR) << "TILEXR_ENABLE_UDMA must match on every rank";
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    if (localUdmaEnabled != 0) {
        ret = InitUDMA();
        if (ret != TILEXR_SUCCESS) {
            return ret;
        }
        ret = InitUDMAPeerMemory();
        if (ret != TILEXR_SUCCESS) {
            return ret;
        }
    } else {
        TILEXR_LOG(INFO) << "TileXR UDMA disabled by TILEXR_ENABLE_UDMA";
    }
    if (!memoryOnlyDomain_) {
        ret = InitSDMA();
        if (ret != TILEXR_SUCCESS) {
            return ret;
        }
    } else {
        commArgs_.extraFlag |= ExtraFlag::MEMORY_ONLY;
    }

    ret = InitCcuBackendIfEnabled();
    if (ret != TILEXR_SUCCESS) {
        return ret;
    }

    // set comm args in device.
    ret = SyncCommArgs();
    if (ret != TILEXR_SUCCESS) {
        TILEXR_LOG(ERROR) << "SyncCommArgs failed! ret: " << ret;
        return ret;
    }
    TILEXR_LOG(INFO) << "TileXRCommInit " << rank_ << "/" << rankSize_ << " success. extraFlag:" << commArgs_.extraFlag <<
        " commArgs_.localRank : " << commArgs_.localRank << " commArgs_.localRankSize : " << commArgs_.localRankSize;
    inited_ = true;
    return TILEXR_SUCCESS;
}

int TileXRComm::InitThread(const std::string &uid)
{
    if (inited_) {
        return TILEXR_SUCCESS;
    }
    if (rank_ < 0 || rank_ >= rankSize_ || rankSize_ <= 0 || rankSize_ > TILEXR_MAX_RANK_SIZE) {
        TILEXR_LOG(ERROR) << "The rank is invalid! rank:" << rank_ << "rankSize:" << rankSize_;
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    if (GetDevThread(uid) != TILEXR_SUCCESS) {
        TILEXR_LOG(ERROR) << "get devs failed.";
        return TILEXR_ERROR_INTERNAL;
    }
    TILEXR_LOG(INFO) << "rank " << rank_ << "/" << rankSize_ << " running devId:" << devId_ << "uid: " << uid;

    if (InitCommon() != TILEXR_SUCCESS) {
        TILEXR_LOG(ERROR) << "init common failed!";
        return TILEXR_ERROR_INTERNAL;
    }
    int ret = InitProfiling();
    if (ret != TILEXR_SUCCESS) {
        return ret;
    }
    {
        lock_guard<mutex> lock(g_mtx);
        if (g_localPeerMemAllocationMap.find(uid) == g_localPeerMemAllocationMap.end()) {
            for (int i = 0; i < rankSize_; ++i) {
                g_localPeerMemAllocationMap[uid][i] = nullptr;
            }
        }
        uid_ = uid;
    }
    ret = InitMem();
    if (ret != TILEXR_SUCCESS) {
        TILEXR_LOG(ERROR) << "InitMem failed! ret: " << ret;
        return ret;
    }
    g_localPeerMemAllocationMap[uid][rank_] = peerMemAllocation_[rank_];

    auto start = high_resolution_clock::now();
    for (int i = 0; i < rankSize_; ++i) {
        while (g_localPeerMemAllocationMap[uid][i] == nullptr) { // check other threads
            this_thread::sleep_for(1ms);
            auto elapsed = duration_cast<seconds>(high_resolution_clock::now() - start);
            if (elapsed.count() > TILEXR_INIT_TIMEOUT) {
                TILEXR_LOG(ERROR) << "Lccl Init timeout!";
                FreePeerMem(peerMemAllocation_[rank_]);
                g_localPeerMemAllocationMap[uid][rank_] = nullptr;
                peerMem_[rank_] = nullptr;
                return TILEXR_ERROR_TIMEOUT;
            }
        }
        peerMemAllocation_[i] = g_localPeerMemAllocationMap[uid][i];
        peerMem_[i] = GetPeerMemLogicalAddress(
            peerMemAllocation_[i], i, memoryOnlyDomain_);
    }
    localRank_ = rank_;
    localRankSize_ = rankSize_;

    // 注意：InitThread 为单进程多线程模式，不支持 UDMA（需要 socketExchange_ 进行跨进程协调）
    // UDMA 主要用于跨进程/跨节点通信，线程模式使用进程内共享内存即可
    TILEXR_LOG(DEBUG) << "Thread mode: UDMA initialization skipped (single-process multi-thread scenario)";

    ret = InitSDMA();
    if (ret != TILEXR_SUCCESS) {
        return ret;
    }
    ret = InitCcuBackendIfEnabled();
    if (ret != TILEXR_SUCCESS) {
        return ret;
    }
    ret = SyncCommArgs();
    if (ret != TILEXR_SUCCESS) {
        TILEXR_LOG(ERROR) << "SyncCommArgs failed! ret: " << ret;
        return ret;
    }
    TILEXR_LOG(INFO) << "Lccl init multi thread " << rank_ << "/" << rankSize_ << " success, uid:" << uid;
    inited_ = true;
    return TILEXR_SUCCESS;
}

/**
 * @brief 函数内部会有检测，是否需要进行 aclrtDeviceEnablePeerAccess，如果芯片为310P且是HCCS链路，则不调用此函数。
 *
 *
 */
int TileXRComm::EnablePeerAccess()
{
    physicalInfo_.chipName = GetChipName();
    for (auto &dev : devList_) {
        if (devId_ == dev) {
            continue;
        }
        // 处理910B2C 16卡通信的特例
        if (SkipUnusedChannel910B2C(dev, devId_, GetChipName())) {
            continue;
        }

        int64_t value = 0;
        if (rtGetPairDevicesInfo(devId_, dev, 0, &value) != RT_ERROR_NONE) {
            TILEXR_LOG(WARN) << devId_ << " & " << dev << " pair devices info failed to get";
        } else {
            TILEXR_LOG(DEBUG) << devId_ << " <-----> " << dev << ", halGetPairDevicesInfo: *value = " << value;
        }

        // 如果310P未来通信域要支持两卡四芯的话，这里需要做更改。并且现在默认服务器上机器只有一个链路种类。
        if (value == TOPOLOGY_HCCS || value == TOPOLOGY_SIO || value == TOPOLOGY_HCCS_SW ||
            GetChipName() == ChipName::CHIP_910B2C) {
            physicalInfo_.physicalLink = PhysicalLink::HCCS;
            commArgs_.extraFlag &= ~(ExtraFlag::TOPO_PCIE);
        } else if (physicalInfo_.physicalLink == PhysicalLink::RESERVED) {
            physicalInfo_.physicalLink = PhysicalLink::PCIE;
            commArgs_.extraFlag |= ExtraFlag::TOPO_PCIE;
        }

        physicalInfo_.coreNum = GetCoreNum(physicalInfo_.chipName);

        // value里的0实际上对应驱动枚举类的 TOPOLOGY_HCCS
        if (physicalInfo_.chipName == ChipName::CHIP_310P3 && value == 0) {
            TILEXR_LOG(WARN) << "warn aclrtDeviceEnablePeerAccess is skipped! peerDeviceId = " << dev;
            continue;
        }

        aclError ret = aclrtDeviceEnablePeerAccess(dev, 0);
        if (ret != ACL_SUCCESS) {
            TILEXR_LOG(ERROR) << "err aclrtDeviceEnablePeerAccess failed peerDeviceId = " << dev << " ,rank = " << rank_
                           << ", value = " << value << ", flags = " << 0 << "," << __LINE__ << ": " << ret;
            return TILEXR_ERROR_INTERNAL;
        }
    }
    TILEXR_LOG(DEBUG) << "EnablePeerAccess succeed" << rank_;
    return TILEXR_SUCCESS;
}

int TileXRComm::GetDev()
{
    // 这里这个nodeNum可以理解为Y轴长度，手动控制的话将这个拦截修改即可。
    int nodeNum = socketExchange_->GetNodeNum();
    if (nodeNum <= 0 || nodeNum > rankSize_) {
        TILEXR_LOG(ERROR) << "error! node num : " << nodeNum << " rank size: " << rankSize_;
        return TILEXR_ERROR_INTERNAL;
    }
    localRankSize_ = rankSize_ < 0 ? 0 : rankSize_ / nodeNum;
    localRank_ = rank_ % localRankSize_;
    TILEXR_LOG(DEBUG) << "GetDev : localRankSize_ : " << localRankSize_ << " localRank_: " << localRank_
                    << "  rank :" << rank_ << "   rankSize :" << rankSize_;
    devList_.resize(rankSize_);
    // get current id and broadcast
    aclError aclRet = aclrtGetDevice(&devId_);
    if (aclRet != ACL_SUCCESS) {
        TILEXR_LOG(ERROR) << "aclrtGetDevice error! ret: " << aclRet;
        return TILEXR_ERROR_INTERNAL;
    }
    // get other rank dev id, put into devList_
    int ret = socketExchange_->AllGather(&devId_, 1, devList_.data());
    if (ret != TILEXR_SUCCESS) {
        TILEXR_LOG(ERROR) << "TileXRSockExchange AllGather error! ret: " << ret;
        return TILEXR_ERROR_INTERNAL;
    }
    std::string devIdStr = "";
    for (int i = 0; i < rankSize_; ++i) {
        devIdStr += (i == 0 ? "" : ", ");
        devIdStr += to_string(devList_[i]);
    }
    TILEXR_LOG(DEBUG) << "rank " << rank_ << " devId: " << devId_ << ", otherDevList : " << devIdStr;
    TILEXR_LOG(INFO) << "AllGather: Get other rank dev id success";
    return TILEXR_SUCCESS;
}

int TileXRComm::GetDevThread(const std::string &uid)
{
    devList_.resize(rankSize_);
    // get current id and broadcast
    aclError aclRet = aclrtGetDevice(&devId_);
    if (aclRet != ACL_SUCCESS) {
        TILEXR_LOG(ERROR) << "aclrtGetDevice error! ret: " << aclRet;
        return TILEXR_ERROR_INTERNAL;
    }
    {
        std::lock_guard<std::mutex> lock(g_mtx);
        if (g_devList.find(uid) == g_devList.end()) {
            for (int i = 0; i < rankSize_; ++i) {
                g_devList[uid][i] = 0;
            }
        }
    }
    g_devList[uid][rank_] = devId_ + 1; // 0 is invalid
    auto start = high_resolution_clock::now();
    for (int i = 0; i < rankSize_; ++i) {
        while (g_devList[uid][i] == 0) { // check other threads
            this_thread::sleep_for(1ms);
            auto elapsed = duration_cast<seconds>(high_resolution_clock::now() - start);
            if (elapsed.count() > TILEXR_INIT_TIMEOUT) {
                TILEXR_LOG(ERROR) << "Lccl Init timeout!";
                return TILEXR_ERROR_TIMEOUT;
            }
        }
        devList_.at(i) = g_devList[uid][i] - 1;
    }
    return TILEXR_SUCCESS;
}

int TileXRComm::InitMem()
{
    const uint64_t peerMemBytes = GetPeerMemBytes(bufferSize_);
    const uint64_t allocationBytes =
        GetPeerMemAllocationBytes(bufferSize_, memoryOnlyDomain_);
    const uint64_t rankOffset = GetPeerMemRankOffset(rank_, memoryOnlyDomain_);

    TILEXR_LOG(DEBUG) << "tilexr peer memory bytes " << peerMemBytes
                      << ", allocation bytes " << allocationBytes
                      << ", rank offset " << rankOffset;
    aclError ret = aclrtMalloc(
        reinterpret_cast<void **>(&peerMemAllocation_[rank_]), allocationBytes,
        (GetChipName() == ChipName::CHIP_310P3) ? ACL_MEM_MALLOC_HUGE_FIRST_P2P : ACL_MEM_MALLOC_HUGE_FIRST);
    if (ret != ACL_SUCCESS) {
        TILEXR_LOG(ERROR) << "allocate device mem error " << __FILE__ << ":" << __LINE__ << " " << ret;
        return TILEXR_ERROR_INTERNAL;
    }
    ret = aclrtMemset(
        peerMemAllocation_[rank_], allocationBytes, 0, allocationBytes);
    if (ret != ACL_SUCCESS) {
        TILEXR_LOG(ERROR) << "initialize device mem error " << __FILE__ << ":"
                          << __LINE__ << " " << ret;
        FreePeerMem(peerMemAllocation_[rank_]);
        return TILEXR_ERROR_INTERNAL;
    }
    peerMem_[rank_] = GetPeerMemLogicalAddress(
        peerMemAllocation_[rank_], rank_, memoryOnlyDomain_);
    TILEXR_LOG(DEBUG) << "peerMem[rank" << rank_ << "], allocate finished.";
    return TILEXR_SUCCESS;
}

int TileXRComm::GetPid(uint32_t *pids)
{
    if (rtDeviceGetBareTgid(&pids[rank_]) != RT_ERROR_NONE) {  // 获取docker外的进程id，bare指docker�?        TILEXR_LOG(ERROR) << "DeviceGetBareTgid err " << __LINE__;
        return TILEXR_ERROR_INTERNAL;
    }
    int ret = socketExchange_->AllGather(&pids[rank_], 1, pids);
    if (ret != TILEXR_SUCCESS) {
        TILEXR_LOG(ERROR) << "TileXRSockExchange AllGather error! ret: " << ret;
        return ret;
    }
    for (int i = 0; i < rankSize_; ++i) {
        TILEXR_LOG(DEBUG) << "rank : " << rank_ << ", otherRank : " << i << " pid[" << i << "]: " << pids[i];
    }
    TILEXR_LOG(DEBUG) << "AllGather: Get other rank pid";
    return TILEXR_SUCCESS;
}

int TileXRComm::GetSidId(int64_t sdids[TILEXR_MAX_RANK_SIZE], int rankSize)
{
    if (rank_ >= rankSize) {
        TILEXR_LOG(ERROR) << "TileXRComm::GetSidId err rank_ >= rankSize " << rank_ << ">=" << rankSize;
        return TILEXR_ERROR_INTERNAL;
    }
    if ((physicalInfo_.chipName >= ChipName::CHIP_910_9391) && (physicalInfo_.chipName < ChipName::RESERVED)) {
        const int rtModuleTypeSystem = 0;
        const int infoTypeSdid = 26;
        if (rtGetDeviceInfo(devList_[rank_], rtModuleTypeSystem, infoTypeSdid, &sdids[rank_]) != RT_ERROR_NONE) {
            TILEXR_LOG(ERROR) << "DeviceGetDeviceInfo err " << __LINE__;
            return TILEXR_ERROR_INTERNAL;
        }
        TILEXR_LOG(DEBUG) << "rank " << rank_ << " dev id: " << devList_[rank_]
                       << " rtGetDeviceInfo sdid: " << sdids[rank_];

        int ret = socketExchange_->AllGather(&sdids[rank_], 1, sdids);
        if (ret != TILEXR_SUCCESS) {
            TILEXR_LOG(ERROR) << "TileXRSockExchange AllGather error! ret: " << ret;
            return ret;
        }
        for (int i = 0; i < rankSize_; ++i) {
            TILEXR_LOG(DEBUG) << "rank " << i << " sdid: " << sdids[i];
        }
        TILEXR_LOG(DEBUG) << "AllGather: Get other rank sdid";
    }
    return TILEXR_SUCCESS;
}

int TileXRComm::GetName(string &name, char names[TILEXR_MAX_RANK_SIZE][IPC_NAME_SIZE]) const
{
    int ret = socketExchange_->AllGather<char>(name.c_str(), IPC_NAME_SIZE, names[0]);
    if (ret != TILEXR_SUCCESS) {
        TILEXR_LOG(ERROR) << "TileXRSockExchange AllGather error! ret: " << ret;
        return TILEXR_ERROR_INTERNAL;
    }
    for (int i = 0; i < rankSize_; ++i) {
        TILEXR_LOG(DEBUG) << "rank " << i << " mem name: " << names[i];
    }
    TILEXR_LOG(DEBUG) << "AllGather: Get other rank mem name";
    return TILEXR_SUCCESS;
}

int TileXRComm::InitCommMem()
{
    int ret = InitMem();
    if (ret != TILEXR_SUCCESS) {
        TILEXR_LOG(ERROR) << "InitMem error! ret: " << ret;
        return ret;
    }

    // 获取所有进程的pid
    uint32_t pids[TILEXR_MAX_RANK_SIZE] = {0};
    ret = GetPid(pids);
    if (ret != TILEXR_SUCCESS) {
        TILEXR_LOG(ERROR) << "GetPid error! ret: " << ret;
        return ret;
    }

    // 获取所有进程的sdid
    int64_t sdids[TILEXR_MAX_RANK_SIZE] = {0};
    ret = GetSidId(sdids, rankSize_);
    if (ret != TILEXR_SUCCESS) {
        TILEXR_LOG(ERROR) << "GetSidId error! ret: " << ret;
        return ret;
    }

    // 获取所有进程的mem name
    string name;
    if (SetMemoryName(name) != TILEXR_SUCCESS) {
        TILEXR_LOG(ERROR) << "SetMemoryName err ";
        return TILEXR_ERROR_INTERNAL;
    }

    if (SetIpcPidSdid(name, pids, sdids) != TILEXR_SUCCESS) {
        TILEXR_LOG(ERROR) << "SetIpcPidSdid failed!";
        return TILEXR_ERROR_INTERNAL;
    }

    TILEXR_LOG(DEBUG) << "rank " << rank_ << " mem name: " << name << " name len: " << name.size();
    char names[TILEXR_MAX_RANK_SIZE][IPC_NAME_SIZE];
    name.resize(IPC_NAME_SIZE);
    ret = GetName(name, names);
    if (ret != TILEXR_SUCCESS) {
        TILEXR_LOG(ERROR) << "GetName error! ret: " << ret;
        return ret;
    }

    if (OpenIpcMem(names) != TILEXR_SUCCESS) {
        TILEXR_LOG(ERROR) << "rank: " << rank_ << " OpenIpcMem failed!";
        return TILEXR_ERROR_INTERNAL;
    }
    return TILEXR_SUCCESS;
}

int TileXRComm::OpenIpcMem(const char names[TILEXR_MAX_RANK_SIZE][IPC_NAME_SIZE])
{
    static mutex mut;
    lock_guard<mutex> lock(mut);
    for (int i = 0; i < rankSize_; ++i) {
        if (i == rank_) {
            continue;
        }
        // 处理910B2C 16卡通信的特例
        if (SkipUnusedChannel910B2C(rank_, i, GetChipName())) {
            continue;
        }
        int ret = rtIpcOpenMemory(
            reinterpret_cast<void **>(&peerMemAllocation_[i]), names[i]);
        if (ret != RT_ERROR_NONE) {
            CloseIpcMem();
            TILEXR_LOG(ERROR) << "rank : " << rank_ << " localRank : " << localRank_ << " peerMem: " << i <<
                " devId : " << devId_ << " peerDevId : " << (i < static_cast<int>(devList_.size()) ? devList_[i] : -1) <<
                " localRankSize : " << localRankSize_ << " ipcNameLen : " << std::strlen(names[i]) <<
                " ipcNamePrefix : " << std::string(names[i], std::min<size_t>(std::strlen(names[i]), 16U)) <<
                " IpcOpenMemory err " << ret;
            return TILEXR_ERROR_INTERNAL;
        }
        peerMem_[i] = GetPeerMemLogicalAddress(
            peerMemAllocation_[i], i, memoryOnlyDomain_);
    }
    ipcMemInited_ = true;
    return TILEXR_SUCCESS;
}

int TileXRComm::InitCreditCommMem()
{
    uint32_t pids[TILEXR_MAX_RANK_SIZE] = {0};
    int ret = GetPid(pids);
    if (ret != TILEXR_SUCCESS) {
        TILEXR_LOG(ERROR) << "GetPid for credit IPC failed! ret: " << ret;
        return ret;
    }
    int64_t sdids[TILEXR_MAX_RANK_SIZE] = {0};
    ret = GetSidId(sdids, rankSize_);
    if (ret != TILEXR_SUCCESS) {
        TILEXR_LOG(ERROR) << "GetSidId for credit IPC failed! ret: " << ret;
        return ret;
    }
    return InitCreditIpcMem(pids, sdids);
}

int TileXRComm::InitCreditIpcMem(const uint32_t *pids, const int64_t *sdids)
{
    aclError aclRet = aclrtMalloc(
        reinterpret_cast<void **>(&creditIpcMem_[rank_]), CREDIT_IPC_BYTES,
        GetChipName() == ChipName::CHIP_310P3 ?
            ACL_MEM_MALLOC_HUGE_FIRST_P2P : ACL_MEM_MALLOC_HUGE_FIRST);
    if (aclRet != ACL_SUCCESS) {
        TILEXR_LOG(ERROR) << "allocate credit IPC memory failed: " << aclRet;
        return TILEXR_ERROR_INTERNAL;
    }
    aclRet = aclrtMemset(
        creditIpcMem_[rank_], CREDIT_IPC_BYTES, 0, CREDIT_IPC_BYTES);
    if (aclRet != ACL_SUCCESS) {
        TILEXR_LOG(ERROR) << "initialize credit IPC memory failed: " << aclRet;
        FreePeerMem(creditIpcMem_[rank_]);
        return TILEXR_ERROR_INTERNAL;
    }

    char nameBuffer[IPC_NAME_SIZE] = {};
    if (rtIpcSetMemoryName(
            creditIpcMem_[rank_], CREDIT_IPC_BYTES,
            nameBuffer, IPC_NAME_SIZE) != RT_ERROR_NONE) {
        TILEXR_LOG(ERROR) << "set credit IPC memory name failed";
        FreePeerMem(creditIpcMem_[rank_]);
        return TILEXR_ERROR_INTERNAL;
    }
    string name(nameBuffer);
    if (SetIpcPidSdid(name, pids, sdids) != TILEXR_SUCCESS) {
        TILEXR_LOG(ERROR) << "set credit IPC pid/sdid failed";
        FreePeerMem(creditIpcMem_[rank_]);
        return TILEXR_ERROR_INTERNAL;
    }
    char names[TILEXR_MAX_RANK_SIZE][IPC_NAME_SIZE] = {};
    name.resize(IPC_NAME_SIZE);
    if (GetName(name, names) != TILEXR_SUCCESS) {
        TILEXR_LOG(ERROR) << "gather credit IPC memory names failed";
        FreePeerMem(creditIpcMem_[rank_]);
        return TILEXR_ERROR_INTERNAL;
    }
    const int ret = OpenCreditIpcMem(names);
    if (ret != TILEXR_SUCCESS) {
        FreePeerMem(creditIpcMem_[rank_]);
    }
    return ret;
}

int TileXRComm::OpenCreditIpcMem(
    const char names[TILEXR_MAX_RANK_SIZE][IPC_NAME_SIZE])
{
    static mutex mut;
    lock_guard<mutex> lock(mut);
    for (int i = 0; i < rankSize_; ++i) {
        if (i == rank_) {
            continue;
        }
        if (SkipUnusedChannel910B2C(rank_, i, GetChipName())) {
            continue;
        }
        const int ret = rtIpcOpenMemory(
            reinterpret_cast<void **>(&creditIpcMem_[i]), names[i]);
        if (ret != RT_ERROR_NONE) {
            CloseCreditIpcMem();
            TILEXR_LOG(ERROR) << "rank: " << rank_ << " creditIpcMem: " << i
                << " IpcOpenMemory err " << ret;
            return TILEXR_ERROR_INTERNAL;
        }
    }
    creditIpcMemInited_ = true;
    return TILEXR_SUCCESS;
}

int TileXRComm::SetMemoryName(string &name)
{
    char nameModified[IPC_NAME_SIZE] = {};
    int memRank = rank_;
    const uint64_t allocationBytes =
        GetPeerMemAllocationBytes(bufferSize_, memoryOnlyDomain_);
    if (rtIpcSetMemoryName(peerMemAllocation_[memRank], allocationBytes,
            nameModified, IPC_NAME_SIZE) != RT_ERROR_NONE) {
        return TILEXR_ERROR_INTERNAL;
    }
    name = nameModified;
    return TILEXR_SUCCESS;
}

int TileXRComm::SetIpcPidSdid(string &name, const uint32_t *pids, const int64_t *sdids) const
{
    const char *modeValue = std::getenv("TILEXR_IPC_PID_MODE");
    const IpcPidMode mode = ResolveIpcPidMode(modeValue);
    if (mode == IpcPidMode::INVALID) {
        TILEXR_LOG(ERROR) << "TILEXR_IPC_PID_MODE must be auto, pid, or sdid";
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    for (int i = 0; i < rankSize_; ++i) {
        if (i == rank_) {
            continue;
        }

        if (UsePidOnlyIpcForPeer(physicalInfo_.chipName, rank_, i, localRankSize_, mode)) {
            int32_t pidInt32 = pids[i];
            int rtRet = rtSetIpcMemPid(name.c_str(), &pidInt32, HCCL_IPC_PID_ARRAY_SIZE);
            if (rtRet != RT_ERROR_NONE) {
                TILEXR_LOG(ERROR) << "err " << rtRet;
                return TILEXR_ERROR_INTERNAL;
            }
        } else {
            int32_t pidInt32 = pids[i];
            int rtRet = rtSetIpcMemorySuperPodPid(name.c_str(), sdids[i], &pidInt32, HCCL_IPC_PID_ARRAY_SIZE);
            if (rtRet != RT_ERROR_NONE) {
                TILEXR_LOG(ERROR) << "err " << rtRet;
                return TILEXR_ERROR_INTERNAL;
            }
        }
    }
    return TILEXR_SUCCESS;
}

TileXRComm::~TileXRComm()
{
    const int resourceRet = FinalizeInternalResource();
    if (resourceRet != TILEXR_SUCCESS) {
        TILEXR_LOG(ERROR) << "TileXR internal resource cleanup failed: " << resourceRet;
    }
    {
        lock_guard<mutex> lock(g_mtx);
        if (g_localPeerMemAllocationMap.find(uid_) != g_localPeerMemAllocationMap.end()) {
            g_localPeerMemAllocationMap.erase(uid_);
        }
    }
    if (udmaContext_ != nullptr) {
        const int udmaRet = udmaContext_->Shutdown();
        if (udmaRet != TILEXR_SUCCESS) {
            TILEXR_LOG(ERROR) << "TileXR UDMA fallback cleanup failed: " << udmaRet;
        }
        udmaContext_.reset();
    }
    if (ipcMemInited_) {
        CloseIpcMem();
        ipcMemInited_ = false;
    }
    if (creditIpcMemInited_) {
        CloseCreditIpcMem();
        creditIpcMemInited_ = false;
    }
    if (socketExchange_) {
        delete socketExchange_;
        socketExchange_ = nullptr;
    }
    FreePeerMem(commArgs_.dumpAddr);
    ResetProfilingState();
    FreePeerMem(peerMemAllocation_[rank_]);
    peerMem_[rank_] = nullptr;
    FreePeerMem(creditIpcMem_[rank_]);
    FreePeerMem(commArgsPtr_);
    if (ccuBackend_ != nullptr) {
        ccuBackend_->Shutdown();
        ccuBackend_.reset();
    }
    ResetSDMAState();
}

TileXRComm::TileXRComm(int rank, int rankSize) : rank_(rank), rankSize_(rankSize)
{
}

TileXRComm::TileXRComm(int rank, int rankSize, int commDomain, int bufferSize,
    bool sharedQpDomain, bool memoryOnlyDomain)
    : rank_(rank), rankSize_(rankSize), commDomain_(commDomain),
      bufferSize_(bufferSize), sharedQpDomain_(sharedQpDomain),
      memoryOnlyDomain_(memoryOnlyDomain)
{
}

TileXRComm::TileXRComm(int rank, int rankSize, TileXRUniqueId commId)
    : rank_(rank), rankSize_(rankSize), commId_(commId)
{
}

int TileXRComm::GetRank() const
{
    return rank_;
}

int TileXRComm::GetRankSize() const
{
    return rankSize_;
}

int TileXRComm::GetCommSize() const
{
    return commSize_;
}

const PhysicalInfo &TileXRComm::GetPhysicalInfo() const
{
    return physicalInfo_;
}

GM_ADDR TileXRComm::GetCommArgsPtr()
{
    return commArgsPtr_;
}

CommArgs* TileXRComm::GetCommArgs()
{
    return &commArgs_;
}

int64_t TileXRComm::NextMagic()
{
    return magic_.fetch_add(1);
}

int TileXRComm::GetInternalResource(void **resource) const
{
    if (resource == nullptr) {
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    *resource = internalResource_;
    return TILEXR_SUCCESS;
}

int TileXRComm::AttachInternalResource(void *resource,
    TileXRInternalResourceDestroyFn destroy)
{
    if (resource == nullptr || destroy == nullptr || internalResource_ != nullptr) {
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    internalResource_ = resource;
    internalResourceDestroy_ = destroy;
    return TILEXR_SUCCESS;
}

int TileXRComm::DetachInternalResource(void *resource)
{
    if (resource == nullptr || resource != internalResource_) {
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    internalResource_ = nullptr;
    internalResourceDestroy_ = nullptr;
    return TILEXR_SUCCESS;
}

int TileXRComm::FinalizeInternalResource()
{
    std::lock_guard<std::mutex> lock(internalResourceMutex_);
    if (internalResource_ == nullptr) {
        return TILEXR_SUCCESS;
    }
    if (internalResourceDestroy_ == nullptr) {
        return TILEXR_ERROR_INTERNAL;
    }
    const int ret = internalResourceDestroy_(this, internalResource_);
    if (ret == TILEXR_SUCCESS) {
        internalResource_ = nullptr;
        internalResourceDestroy_ = nullptr;
    }
    return ret;
}

int TileXRComm::FinalizeUDMA()
{
    if (udmaContext_ == nullptr) {
        return TILEXR_SUCCESS;
    }
    const int ret = udmaContext_->Shutdown();
    if (ret == TILEXR_SUCCESS) {
        udmaContext_.reset();
    }
    return ret;
}

int TileXRComm::AgreeInternalStatus(int localStatus) const
{
    if (rankSize_ <= 0 || rankSize_ > TILEXR_MAX_RANK_SIZE) {
        return TILEXR_ERROR_NOT_INITIALIZED;
    }
    if (socketExchange_ == nullptr) {
        return localStatus;
    }
    int32_t local = static_cast<int32_t>(localStatus);
    std::vector<int32_t> allStatus(static_cast<size_t>(rankSize_),
        TILEXR_ERROR_INTERNAL);
    const int exchangeRet = socketExchange_->AllGather(
        &local, 1, allStatus.data());
    if (exchangeRet != TILEXR_SUCCESS) {
        return exchangeRet;
    }
    for (int rank = 0; rank < rankSize_; ++rank) {
        if (allStatus[rank] != TILEXR_SUCCESS) {
            return static_cast<int>(allStatus[rank]);
        }
    }
    return TILEXR_SUCCESS;
}

void TileXRComm::LockInternalResource()
{
    internalResourceMutex_.lock();
}

void TileXRComm::UnlockInternalResource()
{
    internalResourceMutex_.unlock();
}

std::string TileXRComm::PrintDFX()
{
    if (commArgsPtr_ == nullptr) {
        return "no comm args";
    }

    int ret = aclrtMemcpy(&commArgs_, sizeof(commArgs_), commArgsPtr_, sizeof(commArgs_),
                          ACL_MEMCPY_DEVICE_TO_HOST);
    if (ret != ACL_SUCCESS) {
        TILEXR_LOG(ERROR) << "aclrtMemcpy err " << __LINE__ << " " << ret;
        return "acl mem copy error";
    }
    stringstream ss;
    // 输出CommArgs基本属性
    ss << "CommArgs {"
       << "\n  rank: " << commArgs_.rank
       << "\n  localRank: " << commArgs_.localRank
       << "\n  rankSize: " << commArgs_.rankSize
       << "\n  localRankSize: " << commArgs_.localRankSize
       << "\n  extraFlag:  0x" << std::hex << std::setfill('0') << commArgs_.extraFlag;

    // 输出peerMems数组内容
    ss << "\n  peerMems: [";
    for (int i = 0; i < TILEXR_MAX_RANK_SIZE; ++i) {
        if (commArgs_.peerMems[i] == nullptr) {
            continue;
        }
        if (i > 0) {
            ss << ", ";
        }
        ss << "{id: " << static_cast<void *>(commArgs_.peerMems[i]) << "}";
    }
    ss << "]";

    // magic数组内容
    ss << "\n  magics: [";
    for (int i = 0; i < rankSize_; ++i) {
        ss << std::dec << commArgs_.magics[i] << ",";
    }
    ss << "] \n";

    // 输出dfx数组内容
    ss << "\n  dfx: [";
    const int dfxGroupCount = 5;
    for (int i = 0; i < DFX_COUNT; ++i) {
        if (i % dfxGroupCount == 0) {
            ss << "\n    " << std::dec << setw(dfxGroupCount) << i << ": ";
        }
        ss << "0x"<< std::hex << commArgs_.dfx[i] << ", ";
    }
    ss << "\n    ]";

    ss << "\n  sdma: {"
       << " enabled: " << (((commArgs_.extraFlag & ExtraFlag::SDMA) != 0) ? "true" : "false")
       << ", status: " << static_cast<int>(sdmaInitStatus_)
       << ", workspace: " << static_cast<void*>(commArgs_.sdmaWorkspacePtr)
       << " }";

    ss << "\n}";
    return ss.str();
}

}  // TileXR
