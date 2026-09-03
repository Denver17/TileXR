/*
 * Copyright (c) 2024 Huawei Technologies Co., Ltd.
 * This file is a part of the CANN Open Software.
 * Licensed under CANN Open Software License Agreement Version 1.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#ifndef TILEXR_COMM_H
#define TILEXR_COMM_H

#include <atomic>
#include <memory>
#include <vector>
#include <string>
#include "../include/tilexr_sdma_types.h"
#include "../include/tilexr_udma_reg.h"
#include "../include/tilexr_udma_fullmesh.h"
#include "../include/tilexr_types.h"
#include "../include/tilexr_api.h"
#include "../include/comm_args.h"
#include "tilexr_profiling_runtime.h"

namespace TileXR {
constexpr int IPC_NAME_SIZE = 65;
constexpr int64_t TILEXR_MIB_BYTES = 1024LL * 1024;
constexpr uint64_t TILEXR_MEMORY_DOMAIN_EXTRA_ALLOC_BYTES = 1024ULL * 1024ULL;
constexpr uint64_t TILEXR_MEMORY_DOMAIN_RANK_OFFSET_BYTES = 1024ULL;
static_assert(static_cast<uint64_t>(TILEXR_MAX_RANK_SIZE - 1) *
        TILEXR_MEMORY_DOMAIN_RANK_OFFSET_BYTES <
        TILEXR_MEMORY_DOMAIN_EXTRA_ALLOC_BYTES,
    "memory-domain rank offsets must fit in the extra allocation");
static_assert(TILEXR_BUFF_BYTES ==
        static_cast<int64_t>(TILEXR_COMM_BUFFER_SIZE) * TILEXR_MIB_BYTES + TILEXR_FLAG_BUFF_BYTES,
    "communicator allocation constants are inconsistent");
static_assert(TILEXR_BUFF_BYTES ==
        PING_PONG_SIZE * (IPC_DATA_OFFSET + IPC_BUFF_MAX_SIZE),
    "communicator allocation must cover both ping-pong windows");

class TileXRSockExchange;
class TileXRUDMAContext;
struct TileXRUDMACommArgsState;
class TileXRSDMATransport;
class TileXRComm {
public:
    TileXRComm(int rank, int rankSize);
    TileXRComm(int rank, int rankSize, int commDomain, int bufferSize,
        bool sharedQpDomain = false, bool memoryOnlyDomain = false);
    TileXRComm(int rank, int rankSize, TileXRUniqueId commId);
    ~TileXRComm();
    TileXRComm(const TileXRComm &) = delete;
    TileXRComm &operator=(const TileXRComm &) = delete;
    int Init();
    int InitThread(const std::string &uid = "default");
    int GetRank() const;
    int GetRankSize() const;
    int GetCommSize() const;
    const PhysicalInfo &GetPhysicalInfo() const;
    GM_ADDR GetCommArgsPtr();
    CommArgs* GetCommArgs();
    int64_t NextMagic();
    int RegisterUDMAMemory(GM_ADDR localPtr, size_t bytes, TileXRUDMAMemHandle *handle);
    int UnregisterUDMAMemory(TileXRUDMAMemHandle handle);
    int RegisterUDMAProfile(const TileXRUDMAProfileDesc &desc, TileXRUDMAProfileHandle *handle);
    int UnregisterUDMAProfile(TileXRUDMAProfileHandle handle);
    int QueryUDMAProfile(TileXRUDMAProfileHandle handle, TileXRUDMAProfileView *view) const;
    int GetUDMAQpCount(uint32_t *qpCount) const;
    int QueryUDMAFullmesh(TileXRUDMAFullmeshHostView *view) const;
    GM_ADDR GetUDMARegistryPtr() const;
    const TileXRUDMARegistry* GetUDMARegistryHost() const;
    bool IsSDMAAvailable() const;
    GM_ADDR GetSDMAWorkspacePtr() const;
    SDMAInitStatus GetSDMAInitStatus() const;
    std::string PrintDFX();
    int FinalizeProfiling();
    friend class Lccl;
    friend class Lcoc;
    friend class LcclTest;

private:
    int SetMemoryName(std::string &name);
    int SetIpcPidSdid(std::string &name, const uint32_t *pids, const int64_t *sdids) const;
    int OpenIpcMem(const char names[TILEXR_MAX_RANK_SIZE][IPC_NAME_SIZE]);
    int OpenCreditIpcMem(const char names[TILEXR_MAX_RANK_SIZE][IPC_NAME_SIZE]);
    int GetDev();
    int GetDevThread(const std::string &uid = "");
    int EnablePeerAccess();
    int InitCommMem();
    int InitCreditCommMem();
    int InitCreditIpcMem(const uint32_t *pids, const int64_t *sdids);
    int InitCommon();
    void CloseIpcMem();
    void CloseCreditIpcMem();
    void FreePeerMem(GM_ADDR &mem) const;
    int InitMem();
    int GetSidId(int64_t sdids[TILEXR_MAX_RANK_SIZE], int rankSize);
    int GetPid(uint32_t *pids);
    int GetName(std::string &name, char names[TILEXR_MAX_RANK_SIZE][IPC_NAME_SIZE]) const;
    int SyncCommArgs();
    int InitDumpAddr();
    int InitProfiling();
    int InitUDMA();
    int InitUDMAPeerMemory();
    int ApplyUDMACommArgsState(const TileXRUDMACommArgsState &state);
    static int ApplyUDMACommArgsStateCallback(const TileXRUDMACommArgsState &state, void *userData);
    int InitSDMA();
    int UpdateCommArgsDev();
    void ResetSDMAState();
    void ResetProfilingState();

private:
    int rank_ = 0;  // global rank id
    int rankSize_ = 0;  // global rank size
    int commSize_ = 0;  // local TileXRComm size
    int localRank_ = -1;
    uint32_t localRankSize_ = 0;
    int devId_ = 0;
    std::atomic<int64_t> magic_ {1};
    bool inited_ = false;
    bool ipcMemInited_ = false;
    bool creditIpcMemInited_ = false;
    std::string uid_ = {};
    std::vector<int> devList_ = {};
    int commDomain_ = {};
    int bufferSize_ = TILEXR_COMM_BUFFER_SIZE;
    bool sharedQpDomain_ = false;
    bool memoryOnlyDomain_ = false;
    bool udmaPeerMemMode_ = false;

    // Allocation/mapping bases own the runtime lifecycle; peerMem_ exposes rank-offset logical windows.
    GM_ADDR peerMemAllocation_[TILEXR_MAX_RANK_SIZE] = {};
    GM_ADDR peerMem_[TILEXR_MAX_RANK_SIZE] = {};
    GM_ADDR creditIpcMem_[TILEXR_MAX_RANK_SIZE] = {};
    PhysicalInfo physicalInfo_ = {};
    CommArgs commArgs_ = {};    // host侧
    GM_ADDR commArgsPtr_ = nullptr; // device侧
    TileXRUniqueId commId_ = {};
    TileXRSockExchange *socketExchange_ = nullptr;
    bool isEnableMsprofOp_ = false;
    std::unique_ptr<TileXRUDMAContext> udmaContext_;
    GM_ADDR sdmaWorkspaceDev_ = nullptr;
    SDMAInitStatus sdmaInitStatus_ = SDMAInitStatus::DISABLED_BY_ENV;
    std::unique_ptr<TileXRSDMATransport> sdmaTransport_;
    TileXRProfilingRuntimeConfig profilingConfig_ {};
    GM_ADDR profilingTimestampsDev_ = nullptr;
    GM_ADDR profilingEventIdsDev_ = nullptr;
    GM_ADDR profilingRecordCountsDev_ = nullptr;
    GM_ADDR profilingDroppedCountsDev_ = nullptr;
    bool profilingFinalized_ = false;
};
} // TileXR

#endif // TILEXR_COMM_H
