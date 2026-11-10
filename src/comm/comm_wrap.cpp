/*
 * Copyright (c) 2024 Huawei Technologies Co., Ltd.
 * This file is a part of the CANN Open Software.
 * Licensed under CANN Open Software License Agreement Version 1.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */

/*
 * 实现 Host 公共 API：校验参数并向 TileXRComm 转发初始化、查询和注册请求。
 * 创建成功后返回 opaque handle；销毁按 rank 协议完成资源清理后才删除对象。
 * Implements public Host APIs through TileXRComm and socket bootstrap.
 * Returns owned opaque handles; destruction agrees cleanup status before deletion.
 */
#include "tilexr_api.h"
#include <vector>
#include <thread>
#include <atomic>
#include <memory>
#include <acl/acl_rt.h>

#include "tilexr_comm.h"
#include "tilexr_internal_resource_cleanup.h"
#include "tilexr_log.h"
#include "tools/socket/tilexr_sock_exchange.h"

using namespace std;
using namespace TileXR;

namespace {

int CommInitRankWithDomainConfig(int commDomain, int bufferSize,
    int rankSize, int rank, bool sharedQpDomain, bool memoryOnlyDomain,
    TileXRCommPtr *comm)
{
    TILEXR_LOG(INFO) << "using tilexr c++ api! rank" << rank
                     << ", shared QP domain: " << sharedQpDomain
                     << ", memory-only domain: " << memoryOnlyDomain;
    if (comm == nullptr) {
        TILEXR_LOG(ERROR) << "tilexr comm ptr is nullptr!";
        return TILEXR_ERROR_INTERNAL;
    }
    *comm = nullptr;

    const int minBufferSize = memoryOnlyDomain ? 1 : TILEXR_COMM_BUFFER_SIZE;
    if (bufferSize < minBufferSize) {
        TILEXR_LOG(ERROR) << "tilexr comm buffer size " << bufferSize
                          << " MBytes should not be less than "
                          << minBufferSize << " MBytes!";
        return TILEXR_ERROR_INTERNAL;
    }

    unique_ptr<TileXRComm> c(new (std::nothrow) TileXRComm(
        rank, rankSize, commDomain, bufferSize, sharedQpDomain,
        memoryOnlyDomain));
    if (c == nullptr) {
        TILEXR_LOG(ERROR) << "TileXRComm create failed. rank : " << rank
                          << ", rankSize : " << rankSize;
        return TILEXR_ERROR_INTERNAL;
    }
    const int ret = c->Init();
    if (ret != TILEXR_SUCCESS) {
        TILEXR_LOG(ERROR) << "tilexr init failed! ret: " << ret;
        return ret;
    }
    *comm = c.release();
    return TILEXR_SUCCESS;
}

} // namespace

int TileXRCommInitRankLocal(int rankSize, int rank, TileXRCommPtr *comm)
{
    TILEXR_LOG(INFO) << "using tilexr c++ api! rank" << rank;
    if (comm == nullptr) {
        TILEXR_LOG(ERROR) << "tilexr comm ptr is nullptr!";
        return TILEXR_ERROR_INTERNAL;
    }
    *comm = nullptr;
    unique_ptr<TileXRComm> c(new (std::nothrow) TileXRComm(rank, rankSize));
    if (c == nullptr) {
        TILEXR_LOG(ERROR) << "TileXRComm create failed. rank : " << rank << ", rankSize : " << rankSize;
        return TILEXR_ERROR_INTERNAL;
    }
    int ret = c->Init();
    if (ret != TILEXR_SUCCESS) {
        TILEXR_LOG(ERROR) << "tilexr init failed! ret: " << ret;
        return ret;
    }
    *comm = c.release();
    return TILEXR_SUCCESS;
}

int TileXRGetUniqueId(TileXRUniqueId *uniqueId, int commDomain)
{
    if (uniqueId == nullptr) {
        TILEXR_LOG(ERROR) << "uniqueId is nullptr!";
        return TILEXR_ERROR_INTERNAL;
    }
    int res = BootstrapGetUniqueId(reinterpret_cast<struct TileXRBootstrapHandle *>(uniqueId), commDomain);
    if (res != TILEXR_SUCCESS) {
        TILEXR_LOG(ERROR) << "tilexr BootstrapGetUniqueId failed!";
        return TILEXR_ERROR_INTERNAL;
    }
    return TILEXR_SUCCESS;
}

int TileXRCommInitRank(TileXRUniqueId commId, int rankSize, int rank, TileXRCommPtr *comm)
{
    TILEXR_LOG(INFO) << "using tilexr c++ api! rank" << rank;
    if (comm == nullptr) {
        TILEXR_LOG(ERROR) << "tilexr comm ptr is nullptr!";
        return TILEXR_ERROR_INTERNAL;
    }
    *comm = nullptr;
    unique_ptr<TileXRComm> c(new (std::nothrow) TileXRComm(rank, rankSize, commId));
    if (c == nullptr) {
        TILEXR_LOG(ERROR) << "TileXRComm create failed. rank : " << rank << ", rankSize : " << rankSize;
        return TILEXR_ERROR_INTERNAL;
    }
    int ret = c->Init();
    if (ret != TILEXR_SUCCESS) {
        TILEXR_LOG(ERROR) << "tilexr init failed! ret: " << ret;
        return ret;
    }
    *comm = c.release();
    return TILEXR_SUCCESS;
}

int TileXRCommInitRankWithCustDomainSize(int commDomain, int bufferSize, int rankSize, int rank, TileXRCommPtr *comm)
{
    return CommInitRankWithDomainConfig(
        commDomain, bufferSize, rankSize, rank, false, false, comm);
}

int TileXRCommInitRankWithDomain(int commDomain, int rankSize, int rank, TileXRCommPtr *comm)
{
    constexpr int minBufferSize = TILEXR_COMM_BUFFER_SIZE;
    return TileXRCommInitRankWithCustDomainSize(commDomain, minBufferSize, rankSize, rank, comm);
}

int TileXRCommInitRankWithSharedQpDomain(
    int commDomain, int rankSize, int rank, TileXRCommPtr *comm)
{
    constexpr int minBufferSize = TILEXR_COMM_BUFFER_SIZE;
    return CommInitRankWithDomainConfig(
        commDomain, minBufferSize, rankSize, rank, true, false, comm);
}

int TileXRCommInitRankMemoryDomain(
    int commDomain, int rankSize, int rank, TileXRCommPtr *comm)
{
    return TileXRCommInitRankMemoryDomainWithSize(
        commDomain, TILEXR_MEMORY_DOMAIN_BUFFER_SIZE, rankSize, rank, comm);
}

int TileXRCommInitRankMemoryDomainWithSize(
    int commDomain, int bufferSize, int rankSize, int rank, TileXRCommPtr *comm)
{
    return CommInitRankWithDomainConfig(commDomain,
        bufferSize, rankSize, rank, false, true, comm);
}

int TileXRGetCommArgsDev(TileXRCommPtr comm, GM_ADDR &commArgsPtr)
{
    if (comm == nullptr) {
        TILEXR_LOG(ERROR) << "tilexr comm is nullptr!";
        return TILEXR_ERROR_INTERNAL;
    }
    auto* tilexr = static_cast<TileXRComm *>(comm);
    commArgsPtr = tilexr->GetCommArgsPtr();
    return TILEXR_SUCCESS;
}

int TileXRGetCommArgsHost(TileXRCommPtr comm, TileXR::CommArgs *&commArgsPtr)
{
    if (comm == nullptr) {
        TILEXR_LOG(ERROR) << "tilexr comm is nullptr!";
        return TILEXR_ERROR_INTERNAL;
    }
    auto* c = static_cast<TileXRComm *>(comm);
    commArgsPtr = c->GetCommArgs();
    return TILEXR_SUCCESS;
}

int TileXRCommNextMagic(TileXRCommPtr comm, int64_t *magic)
{
    if (comm == nullptr || magic == nullptr) {
        TILEXR_LOG(ERROR) << "TileXRCommNextMagic invalid input";
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    auto* c = static_cast<TileXRComm *>(comm);
    *magic = c->NextMagic();
    return TILEXR_SUCCESS;
}

int TileXRCommHostAllGather(TileXRCommPtr comm, const void *sendBuf, size_t sendBytes, void *recvBuf)
{
    if (comm == nullptr || sendBuf == nullptr || recvBuf == nullptr || sendBytes == 0) {
        TILEXR_LOG(ERROR) << "TileXRCommHostAllGather invalid input";
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    return static_cast<TileXRComm *>(comm)->HostAllGather(sendBuf, sendBytes, recvBuf);
}

int TileXRCommHostAllGatherTimeout(TileXRCommPtr comm, const void *sendBuf, size_t sendBytes,
                                   void *recvBuf, uint32_t timeoutMs)
{
    if (comm == nullptr || sendBuf == nullptr || recvBuf == nullptr || sendBytes == 0 || timeoutMs == 0) {
        TILEXR_LOG(ERROR) << "TileXRCommHostAllGatherTimeout invalid input";
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    return static_cast<TileXRComm *>(comm)->HostAllGather(sendBuf, sendBytes, recvBuf, timeoutMs);
}

int TileXRUDMARegister(TileXRCommPtr comm, GM_ADDR localPtr, size_t bytes, TileXRUDMAMemHandle *handle)
{
    if (comm == nullptr || localPtr == nullptr || handle == nullptr || bytes == 0) {
        TILEXR_LOG(ERROR) << "TileXRUDMARegister invalid input";
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    auto* c = static_cast<TileXRComm *>(comm);
    return c->RegisterUDMAMemory(localPtr, bytes, handle);
}

int TileXRUDMAUnregister(TileXRCommPtr comm, TileXRUDMAMemHandle handle)
{
    if (comm == nullptr) {
        TILEXR_LOG(ERROR) << "TileXRUDMAUnregister invalid comm";
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    auto* c = static_cast<TileXRComm *>(comm);
    return c->UnregisterUDMAMemory(handle);
}

int TileXRUDMAProfileRegister(TileXRCommPtr comm,
    const TileXR::TileXRUDMAProfileDesc *desc, TileXRUDMAProfileHandle *handle)
{
    if (comm == nullptr || desc == nullptr || handle == nullptr) {
        TILEXR_LOG(ERROR) << "TileXRUDMAProfileRegister invalid input";
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    *handle = 0;
    auto* c = static_cast<TileXRComm *>(comm);
    return c->RegisterUDMAProfile(*desc, handle);
}

int TileXRUDMAProfileUnregister(TileXRCommPtr comm, TileXRUDMAProfileHandle handle)
{
    if (comm == nullptr || handle == 0) {
        TILEXR_LOG(ERROR) << "TileXRUDMAProfileUnregister invalid input";
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    auto* c = static_cast<TileXRComm *>(comm);
    return c->UnregisterUDMAProfile(handle);
}

int TileXRUDMAProfileQuery(TileXRCommPtr comm, TileXRUDMAProfileHandle handle,
    TileXR::TileXRUDMAProfileView *view)
{
    if (view != nullptr) {
        *view = TileXR::TileXRUDMAProfileView {};
    }
    if (comm == nullptr || handle == 0 || view == nullptr) {
        TILEXR_LOG(ERROR) << "TileXRUDMAProfileQuery invalid input";
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    auto* c = static_cast<TileXRComm *>(comm);
    return c->QueryUDMAProfile(handle, view);
}

int TileXRUDMAGetQpCount(TileXRCommPtr comm, uint32_t *qpCount)
{
    if (qpCount != nullptr) {
        *qpCount = 0;
    }
    if (comm == nullptr || qpCount == nullptr) {
        TILEXR_LOG(ERROR) << "TileXRUDMAGetQpCount invalid input";
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    auto* c = static_cast<TileXRComm *>(comm);
    return c->GetUDMAQpCount(qpCount);
}

int TileXRUDMAFullmeshQuery(TileXRCommPtr comm,
    TileXR::TileXRUDMAFullmeshHostView *view)
{
    if (view != nullptr) {
        *view = TileXR::TileXRUDMAFullmeshHostView {};
    }
    if (comm == nullptr || view == nullptr) {
        TILEXR_LOG(ERROR) << "TileXRUDMAFullmeshQuery invalid input";
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    auto* c = static_cast<TileXRComm *>(comm);
    return c->QueryUDMAFullmesh(view);
}

int TileXRGetUDMARegistryDev(TileXRCommPtr comm, GM_ADDR &registryPtr)
{
    if (comm == nullptr) {
        TILEXR_LOG(ERROR) << "TileXRGetUDMARegistryDev invalid comm";
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    auto* c = static_cast<TileXRComm *>(comm);
    registryPtr = c->GetUDMARegistryPtr();
    return TILEXR_SUCCESS;
}

int TileXRGetUDMARegistryHost(TileXRCommPtr comm, const TileXR::TileXRUDMARegistry **registry)
{
    if (comm == nullptr || registry == nullptr) {
        TILEXR_LOG(ERROR) << "TileXRGetUDMARegistryHost invalid input";
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    auto* c = static_cast<TileXRComm *>(comm);
    *registry = c->GetUDMARegistryHost();
    return *registry == nullptr ? TILEXR_ERROR_NOT_INITIALIZED : TILEXR_SUCCESS;
}

int TileXRSDMAAvailable(TileXRCommPtr comm, bool *available)
{
    if (comm == nullptr || available == nullptr) {
        TILEXR_LOG(ERROR) << "TileXRSDMAAvailable invalid input";
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    auto* c = static_cast<TileXRComm *>(comm);
    *available = c->IsSDMAAvailable();
    return TILEXR_SUCCESS;
}

int TileXRGetSDMAWorkspaceDev(TileXRCommPtr comm, GM_ADDR *workspace)
{
    if (comm == nullptr || workspace == nullptr) {
        TILEXR_LOG(ERROR) << "TileXRGetSDMAWorkspaceDev invalid input";
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    auto* c = static_cast<TileXRComm *>(comm);
    *workspace = c->GetSDMAWorkspacePtr();
    return TILEXR_SUCCESS;
}

void TileXRPrintDFX2Log(TileXRCommPtr comm)
{
    if (comm == nullptr) {
        TILEXR_LOG(ERROR) << "tilexr comm is nullptr!";
        return;
    }
    auto* tilexr = static_cast<TileXRComm *>(comm);
    TILEXR_LOG(INFO) << tilexr->PrintDFX();
}

int TileXRCommInit(int rank, int rankSize, TileXRCommPtr *comms)
{
    if (comms == nullptr) {
        TILEXR_LOG(ERROR) << "tilexr comms is nullptr!";
        return TILEXR_ERROR_INTERNAL;
    }
    *comms = nullptr;
    *comms = new (std::nothrow) TileXRComm(rank, rankSize);
    if (*comms == nullptr) {
        TILEXR_LOG(ERROR) << "TileXRComm create failed. rank : " << rank << ", rankSize : " << rankSize;
        return TILEXR_ERROR_INTERNAL;
    }
    return TILEXR_SUCCESS;
}

int TileXRCommInitAll(uint32_t ndev, int32_t *devices, TileXRCommPtr *comms)
{
    if (comms == nullptr) {
        TILEXR_LOG(ERROR) << "tilexr comms is nullptr!";
        return TILEXR_ERROR_INTERNAL;
    }
    if (devices == nullptr) {
        TILEXR_LOG(ERROR) << "tilexr devices is nullptr!";
        return TILEXR_ERROR_INTERNAL;
    }
    for (uint32_t i = 0; i < ndev; ++i) {
        comms[i] = nullptr;
    }
    static int commDomain = 0;
    commDomain++;
    vector<unique_ptr<TileXRComm>> commHolders;
    commHolders.reserve(ndev);
    for (uint32_t i = 0; i < ndev; ++i) {
        unique_ptr<TileXRComm> commHolder(new (std::nothrow) TileXRComm(i, ndev, commDomain, TILEXR_COMM_BUFFER_SIZE));
        if (commHolder == nullptr) {
            TILEXR_LOG(ERROR) << "TileXRComm create failed. dev : " << i << ", ndev : " << ndev;
            return TILEXR_ERROR_INTERNAL;
        }
        commHolders.emplace_back(std::move(commHolder));
    }
    static atomic<int> uid;
    uid++;
    vector<unique_ptr<thread>> threads;
    atomic<int> error(TILEXR_SUCCESS);
    for (uint32_t r = 0; r < ndev; r++) {
        threads.emplace_back(make_unique<thread>(
            [&](int rank) {
                aclrtSetDevice(devices[rank]);
                auto* c = commHolders[rank].get();
                int ret = c->InitThread("uid" + to_string(uid));
                if (ret != TILEXR_SUCCESS) {
                    error.store(ret);
                }
            },
            r));
    }
    for (auto &t : threads) {
        t->join();
    }
    threads.clear();
    int ret = error.load();
    if (ret != TILEXR_SUCCESS) {
        TILEXR_LOG(ERROR) << "TileXRCommInitAll init failed! ret: " << ret;
        return ret;
    }
    for (uint32_t i = 0; i < ndev; ++i) {
        comms[i] = commHolders[i].release();
    }
    return TILEXR_SUCCESS;
}

int TileXRCommInitThread(int rank, int rankSize, const char *uid, TileXRCommPtr *comms)
{
    if (uid == nullptr) {
        TILEXR_LOG(ERROR) << "tilexr uid is nullptr!";
        return TILEXR_ERROR_INTERNAL;
    }
    if (comms == nullptr) {
        TILEXR_LOG(ERROR) << "tilexr comms is nullptr!";
        return TILEXR_ERROR_INTERNAL;
    }
    *comms = nullptr;
    if (rank >= rankSize) {
        TILEXR_LOG(ERROR) << "tilexr rank : " << rank << " rankSize : " << rankSize;
        return TILEXR_ERROR_INTERNAL;
    }
    unique_ptr<TileXRComm> c(new (std::nothrow) TileXRComm(rank, rankSize));
    if (c == nullptr) {
        TILEXR_LOG(ERROR) << "TileXRComm create failed. rank : " << rank << ", rankSize : " << rankSize;
        return TILEXR_ERROR_INTERNAL;
    }
    int ret = c->InitThread(string(uid));
    if (ret != TILEXR_SUCCESS) {
        TILEXR_LOG(ERROR) << "tilexr init thread failed! ret: " << ret;
        return ret;
    }
    *comms = c.release();
    return TILEXR_SUCCESS;
}

int TileXRCommDestroy(TileXRCommPtr comm)
{
    if (comm == nullptr) {
        return TILEXR_INVALID_VALUE;
    }
    auto *c = static_cast<TileXRComm *>(comm);
    // Optional local resource ownership must not choose which ranks enter
    // cleanup agreement. Successful local resources have already been detached;
    // peers retaining failed items can retry destruction without replaying them.
    const int resourceRet = TileXR::FinalizeInternalResourceTogether(*c);
    if (resourceRet != TILEXR_SUCCESS) {
        return resourceRet;
    }
    int finalizeRet = c->FinalizeKernelTilings();
    // Reuse the existing rank agreement so a failed tiling release cannot leave
    // one rank retrying destruction after its peers have deleted their comms.
    finalizeRet = c->AgreeInternalStatus(finalizeRet);
    if (finalizeRet != TILEXR_SUCCESS) {
        return finalizeRet;
    }
    const int profilingRet = c->AgreeInternalStatus(c->FinalizeProfilingSlots());
    if (profilingRet != TILEXR_SUCCESS) return profilingRet;
    const int rdmaRet = c->FinalizeRDMA();
    if (rdmaRet != TILEXR_SUCCESS) {
        return rdmaRet;
    }
    const int udmaRet = c->FinalizeUDMA();
    if (udmaRet != TILEXR_SUCCESS) {
        return udmaRet;
    }
    delete c;
    return TILEXR_SUCCESS;
}

int TileXRCommGetKernelTiling(TileXRCommPtr comm, const void *data,
    uint32_t bytes, GM_ADDR *deviceData)
{
    if (deviceData != nullptr) *deviceData = nullptr;
    if (comm == nullptr) return TILEXR_ERROR_PARA_CHECK_FAIL;
    return static_cast<TileXRComm *>(comm)->GetKernelTiling(data, bytes, deviceData);
}

int TileXRCommGetInternalResource(TileXRCommPtr comm, void **resource)
{
    if (comm == nullptr || resource == nullptr) {
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    return static_cast<TileXRComm *>(comm)->GetInternalResource(resource);
}

int TileXRCommAttachInternalResource(TileXRCommPtr comm, void *resource,
    TileXRInternalResourceDestroyFn destroy)
{
    if (comm == nullptr || resource == nullptr || destroy == nullptr) {
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    return static_cast<TileXRComm *>(comm)->AttachInternalResource(resource, destroy);
}

int TileXRCommDetachInternalResource(TileXRCommPtr comm, void *resource)
{
    if (comm == nullptr || resource == nullptr) {
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    return static_cast<TileXRComm *>(comm)->DetachInternalResource(resource);
}

int TileXRCommAgreeInternalStatus(TileXRCommPtr comm, int localStatus)
{
    if (comm == nullptr) {
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    return static_cast<TileXRComm *>(comm)->AgreeInternalStatus(localStatus);
}

int TileXRCommHasA5GroupedQpProfile(TileXRCommPtr comm, bool *available)
{
    if (comm == nullptr || available == nullptr) {
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    *available = static_cast<TileXRComm *>(comm)->HasUDMAGroupedQpProfile();
    return TILEXR_SUCCESS;
}

int TileXRCommLockInternalResource(TileXRCommPtr comm)
{
    if (comm == nullptr) {
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    static_cast<TileXRComm *>(comm)->LockInternalResource();
    return TILEXR_SUCCESS;
}

int TileXRCommUnlockInternalResource(TileXRCommPtr comm)
{
    if (comm == nullptr) {
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    static_cast<TileXRComm *>(comm)->UnlockInternalResource();
    return TILEXR_SUCCESS;
}
