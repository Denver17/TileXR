/*
 * Copyright (c) 2024-2026 TileXR Project
 * This file is a part of the CANN Open Software.
 * Licensed under CANN Open Software License Agreement Version 1.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
/* 实现 collective Host launch，将执行计划交给 profiling producer，并在原有清理边界处理 capture 队列；依赖注册的外置 kernel 与 TileXR communicator。
 * Launch collectives with registered external kernels and the TileXR communicator; pass execution plans to the profiling producer and drain captures at existing cleanup boundaries.
 */

#include "collective_launcher.h"

#include <limits>
#include <new>
#include <utility>
#include <vector>
#if defined(__linux__)
#include <unistd.h>
#endif

#include "acl/acl_rt.h"
#include "alltoall_a5_common.h"
#include "collective_kernel.h"
#include "profiling_capture_queue.h"
#include "profiling_producer.h"
#include "collective_utils.h"
#include "tilexr_internal.h"
#include "tilexr_types.h"

namespace TileXRCollectives {
namespace Host {
namespace {

constexpr uint64_t kA5WaitIterations = UINT64_C(100000000);

TileXR::Profiling::Status RuntimeProducer(const CollectiveProfilingWorkload &workload,
    TileXR::Profiling::ManifestProducer *producer)
{
    using namespace TileXR::Profiling;
    const char *hash=std::getenv(workload.groupedA5 ? "TILEXR_PROFILING_A5_MAPPING_SHA256" :
        "TILEXR_PROFILING_COARSE_MAPPING_SHA256");
    if(!hash || !*hash) hash=std::getenv("TILEXR_PROFILING_MAPPING_SHA256");
    if(!hash || !*hash) return {StatusCode::InvalidArgument,"manifest requires the canonical fingerprint of the actual clock-adjusted mapping"};
    producer->mappingSha256=hash;
    // The queue replaces this placeholder with the selected capture's shared
    // UUID attempt before kernel enqueue, so preallocated shapes cannot mix.
    producer->attemptId="planned-collective";
    int32_t device=-1;
    const aclError result=aclrtGetDevice(&device);
    if(result!=ACL_SUCCESS) return {StatusCode::Acl,"aclrtGetDevice for profiling identity",int(result)};
    if(device<0) return {StatusCode::InvalidState,"ACL returned an invalid device ordinal"};
    producer->deviceId=std::to_string(device);
#if defined(__linux__)
    char host[256]={};
    if(gethostname(host,sizeof(host)-1)!=0) return {StatusCode::Io,"gethostname for profiling identity"};
    producer->host=host;
#else
    return {StatusCode::InvalidState,"collective profiling host identity requires the supported Linux runtime"};
#endif
    const char *soc=aclrtGetSocName();
    if(soc && *soc) producer->environment["soc"]=soc;
    const char *keys[]={"cann_version","driver_version","clock_basis"};
    const char *variables[]={"TILEXR_PROFILING_CANN_VERSION","TILEXR_PROFILING_DRIVER_VERSION","TILEXR_PROFILING_CLOCK_BASIS"};
    for(size_t i=0;i<3;++i) {
        const char *value=std::getenv(variables[i]);
        if(value && *value) producer->environment[keys[i]]=value;
    }
    producer->environment["identity_source"]="gethostname, aclrtGetDevice, aclrtGetSocName; versions/time basis explicitly supplied";
    producer->versions["sdk_version"]="1.0.0";
    return PopulateCollectiveProducer(workload,producer);
}

struct RetiredA5Resource {
    TileXRUDMAProfileHandle profile = 0U;
    void *workspace = nullptr;
};

struct CommonCollectiveRuntime {
    void *workspace = nullptr;
    uint64_t workspaceCapacity = 0U;
    TileXRUDMAProfileHandle profile = 0U;
    TileXR::TileXRUDMAProfileView profileView {};
    int64_t previousMagic = 0;
    bool launched = false;
    bool a5Resources = false;
    std::shared_ptr<ProfilingCaptureQueue> captures;
    bool cleanupPending = false;
    bool destroying = false;
    aclrtStream stream = nullptr;
    std::vector<RetiredA5Resource> retiredResources;
};

void *AcquireCapture(CommonCollectiveRuntime &runtime, aclrtStream stream,
    const CollectiveProfilingWorkload &workload) noexcept
{
#if defined(TILEXR_ENABLE_PROFILING) && TILEXR_ENABLE_PROFILING == 1
    const char *directory = std::getenv("TILEXR_PROFILING_OUTPUT_DIR");
    if (!directory || !*directory) return nullptr;
    try {
        if (!runtime.captures) runtime.captures = std::make_shared<ProfilingCaptureQueue>();
        TileXR::Profiling::ManifestProducer producer;
        const auto status=RuntimeProducer(workload,&producer);
        if(!status) {
            std::cerr << "TileXR profiling producer metadata unavailable: " << status.message << "\n";
            // Retain legacy provided-input capture when the caller has not
            // supplied a trustworthy mapping fingerprint/environment identity.
            return runtime.captures->Acquire(stream,workload.lanes,workload.rank,workload.rankSize);
        }
        return runtime.captures->Acquire(stream,workload.lanes,workload.rank,workload.rankSize,&producer);
    } catch (const std::exception &error) {
        std::cerr << "TileXR profiling skipped: " << error.what() << "\n";
        return nullptr;
    }
#else
    (void)runtime; (void)stream; (void)workload;
    return nullptr;
#endif
}

class InternalResourceGuard {
public:
    explicit InternalResourceGuard(TileXRCommPtr comm) : comm_(comm),
        locked_(TileXRCommLockInternalResource(comm) == TileXR::TILEXR_SUCCESS)
    {
    }

    ~InternalResourceGuard()
    {
        if (locked_) {
            (void)TileXRCommUnlockInternalResource(comm_);
        }
    }

    bool Locked() const
    {
        return locked_;
    }

private:
    TileXRCommPtr comm_ = nullptr;
    bool locked_ = false;
};

bool ProfileMatches(const CommonCollectiveRuntime &runtime,
    const HostLaunchContext &context)
{
    if (runtime.workspace == nullptr || runtime.profile == 0U ||
        context.hostArgs == nullptr) {
        return false;
    }
    const auto *localRegion = TileXR::UDMAProfileRegion(
        runtime.profileView.registryHost, context.hostArgs->rank,
        TileXRGrouped::kWorkspaceRegion);
    return localRegion != nullptr && localRegion->base == runtime.workspace &&
        localRegion->bytes == runtime.workspaceCapacity &&
        runtime.profileView.registryHost != nullptr &&
        runtime.profileView.infoDev != nullptr &&
        runtime.profileView.registryDev != nullptr &&
        TileXR::UDMAGroupedAllToAllProfileValid(
            runtime.profileView.registryHost, context.hostArgs->rankSize);
}

int CleanupRetiredProfiles(TileXRCommPtr comm, CommonCollectiveRuntime &runtime)
{
    int firstError = TileXR::TILEXR_SUCCESS;
    for (auto &resource : runtime.retiredResources) {
        if (resource.profile == 0U) {
            continue;
        }
        const int ret = TileXRUDMAProfileUnregister(comm, resource.profile);
        if (ret == TileXR::TILEXR_SUCCESS) {
            resource.profile = 0U;
        } else if (firstError == TileXR::TILEXR_SUCCESS) {
            firstError = ret;
        }
    }
    return firstError;
}

int CleanupAllProfiles(TileXRCommPtr comm, CommonCollectiveRuntime &runtime)
{
    int firstError = TileXR::TILEXR_SUCCESS;
    if (runtime.profile != 0U) {
        const int ret = TileXRUDMAProfileUnregister(comm, runtime.profile);
        if (ret == TileXR::TILEXR_SUCCESS) {
            runtime.profile = 0U;
            runtime.profileView = TileXR::TileXRUDMAProfileView {};
        } else {
            firstError = ret;
        }
    }
    const int retiredRet = CleanupRetiredProfiles(comm, runtime);
    return firstError == TileXR::TILEXR_SUCCESS ? retiredRet : firstError;
}

int CleanupRetiredWorkspaces(CommonCollectiveRuntime &runtime)
{
    int firstError = TileXR::TILEXR_SUCCESS;
    for (auto it = runtime.retiredResources.begin();
         it != runtime.retiredResources.end();) {
        if (it->profile != 0U) {
            ++it;
            continue;
        }
        if (it->workspace == nullptr || aclrtFree(it->workspace) == ACL_SUCCESS) {
            it = runtime.retiredResources.erase(it);
        } else {
            firstError = TileXR::TILEXR_ERROR_INTERNAL;
            ++it;
        }
    }
    return firstError;
}

int DestroyCollectiveRuntime(TileXRCommPtr comm, void *opaque)
{
    auto *runtime = static_cast<CommonCollectiveRuntime *>(opaque);
    if (runtime == nullptr) {
        return TileXR::TILEXR_SUCCESS;
    }
    runtime->destroying = true;
    if (!runtime->a5Resources) {
        // Generic captures are optional and may be absent on another rank.
        // The communicator destroy entry point performs the common agreement.
        if (runtime->captures && !runtime->captures->Drain()) return TileXR::TILEXR_ERROR_INTERNAL;
        delete runtime;
        return TileXR::TILEXR_SUCCESS;
    }
    const aclError syncRet = runtime->stream == nullptr ?
        ACL_SUCCESS : aclrtSynchronizeStream(runtime->stream);
    int ret = TileXRCommAgreeInternalStatus(comm,
        syncRet == ACL_SUCCESS ? TileXR::TILEXR_SUCCESS :
        TileXR::TILEXR_ERROR_MKIRT);
    if (ret != TileXR::TILEXR_SUCCESS) {
        return ret;
    }

    const int localProfileRet = CleanupAllProfiles(comm, *runtime);
    ret = TileXRCommAgreeInternalStatus(comm, localProfileRet);
    if (ret != TileXR::TILEXR_SUCCESS) {
        runtime->cleanupPending = true;
        return ret;
    }

    int localWorkspaceRet = TileXR::TILEXR_SUCCESS;
    if (runtime->workspace != nullptr) {
        const aclError freeRet = aclrtFree(runtime->workspace);
        if (freeRet != ACL_SUCCESS) {
            localWorkspaceRet = TileXR::TILEXR_ERROR_INTERNAL;
        } else {
            runtime->workspace = nullptr;
            runtime->workspaceCapacity = 0U;
        }
    }
    const int retiredRet = CleanupRetiredWorkspaces(*runtime);
    if (localWorkspaceRet == TileXR::TILEXR_SUCCESS &&
        retiredRet != TileXR::TILEXR_SUCCESS) {
        localWorkspaceRet = retiredRet;
    }
    ret = TileXRCommAgreeInternalStatus(comm, localWorkspaceRet);
    if (ret != TileXR::TILEXR_SUCCESS) {
        runtime->cleanupPending = true;
        return ret;
    }
    // A5 owners exist on every rank after coordinated resource creation. Keep
    // them until the same fixed drain agreement succeeds, including retries.
    ret = TileXRCommAgreeInternalStatus(comm, !runtime->captures || runtime->captures->Drain() ?
        TileXR::TILEXR_SUCCESS : TileXR::TILEXR_ERROR_INTERNAL);
    if (ret != TileXR::TILEXR_SUCCESS) return ret;
    delete runtime;
    return TileXR::TILEXR_SUCCESS;
}

int CreateA5Resources(TileXRCommPtr comm, const HostLaunchContext &context,
    uint64_t bytes, CommonCollectiveRuntime &candidate)
{
    uint32_t qpCount = 0U;
    int ret = TileXRUDMAGetQpCount(comm, &qpCount);
    if (ret != TileXR::TILEXR_SUCCESS ||
        qpCount != TileXRGrouped::kDirectSharedQpCount) {
        return ret == TileXR::TILEXR_SUCCESS ?
            TileXR::TILEXR_ERROR_NOT_SUPPORT : ret;
    }
    const aclError allocRet = aclrtMalloc(&candidate.workspace, bytes,
        ACL_MEM_MALLOC_HUGE_FIRST);
    const int localAllocStatus = allocRet == ACL_SUCCESS ?
        TileXR::TILEXR_SUCCESS : TileXR::TILEXR_ERROR_INTERNAL;
    ret = TileXRCommAgreeInternalStatus(comm, localAllocStatus);
    if (ret != TileXR::TILEXR_SUCCESS) {
        if (candidate.workspace != nullptr) {
            (void)aclrtFree(candidate.workspace);
            candidate.workspace = nullptr;
        }
        return ret;
    }
    candidate.workspaceCapacity = bytes;
    TileXR::TileXRUDMAProfileDesc desc {};
    desc.reserved = static_cast<uint32_t>(
        TileXR::TileXRUDMAProfileKind::A5_GROUPED_ALLTOALL);
    desc.regionCount = TileXRGrouped::kProfileRegionCount;
    desc.qpBindingCount = qpCount;
    desc.regions[TileXRGrouped::kWorkspaceRegion].base =
        static_cast<GM_ADDR>(candidate.workspace);
    desc.regions[TileXRGrouped::kWorkspaceRegion].bytes = bytes;
    for (uint32_t qp = 0U; qp < qpCount; ++qp) {
        desc.qpBindings[qp].localRegion = TileXRGrouped::kWorkspaceRegion;
        desc.qpBindings[qp].remoteRegion = TileXRGrouped::kWorkspaceRegion;
    }
    ret = TileXRUDMAProfileRegister(comm, &desc, &candidate.profile);
    if (ret == TileXR::TILEXR_SUCCESS) {
        ret = TileXRUDMAProfileQuery(
            comm, candidate.profile, &candidate.profileView);
    }
    if (ret == TileXR::TILEXR_SUCCESS && !ProfileMatches(candidate, context)) {
        ret = TileXR::TILEXR_ERROR_NOT_INITIALIZED;
    }
    if (ret == TileXR::TILEXR_ERROR_NOT_SUPPORT) {
        ret = TileXR::TILEXR_ERROR_NOT_INITIALIZED;
    }
    ret = TileXRCommAgreeInternalStatus(comm, ret);
    if (ret != TileXR::TILEXR_SUCCESS) {
        if (candidate.profile != 0U) {
            (void)TileXRUDMAProfileUnregister(comm, candidate.profile);
            candidate.profile = 0U;
        }
        (void)aclrtFree(candidate.workspace);
        candidate.workspace = nullptr;
        candidate.workspaceCapacity = 0U;
    }
    return ret;
}

int GetOrCreateA5Runtime(TileXRCommPtr comm, const HostLaunchContext &context,
    uint64_t requiredBytes, aclrtStream stream,
    CommonCollectiveRuntime **runtimeOut)
{
    void *opaque = nullptr;
    int ret = TileXRCommGetInternalResource(comm, &opaque);
    if (ret != TileXR::TILEXR_SUCCESS) {
        return ret;
    }
    auto *runtime = static_cast<CommonCollectiveRuntime *>(opaque);
    if (runtime != nullptr && runtime->destroying) {
        return TileXR::TILEXR_ERROR_NOT_INITIALIZED;
    }
    if (runtime != nullptr && runtime->workspace != nullptr && runtime->stream != stream) {
        return TileXR::TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    if (runtime != nullptr && requiredBytes <= runtime->workspaceCapacity) {
        if (runtime->cleanupPending) {
            const int localProfileRet = CleanupRetiredProfiles(comm, *runtime);
            int cleanupRet = TileXRCommAgreeInternalStatus(
                comm, localProfileRet);
            if (cleanupRet != TileXR::TILEXR_SUCCESS) {
                return cleanupRet;
            }
            const int localWorkspaceRet = CleanupRetiredWorkspaces(*runtime);
            cleanupRet = TileXRCommAgreeInternalStatus(
                comm, localWorkspaceRet);
            if (cleanupRet != TileXR::TILEXR_SUCCESS) {
                return cleanupRet;
            }
            runtime->cleanupPending = false;
        }
        *runtimeOut = runtime;
        return TileXR::TILEXR_SUCCESS;
    }

    CommonCollectiveRuntime candidate {};
    ret = CreateA5Resources(comm, context, requiredBytes, candidate);
    if (ret != TileXR::TILEXR_SUCCESS) {
        return ret;
    }
    candidate.a5Resources = true;
    if (runtime == nullptr || !runtime->a5Resources) {
        candidate.stream = stream;
        const bool needsOwner = runtime == nullptr;
        auto *newRuntime = needsOwner ? new (std::nothrow) CommonCollectiveRuntime(candidate) : runtime;
        // Every rank takes these same phases, including ranks with a generic
        // optional profiling owner and ranks whose optional allocation failed.
        ret = TileXRCommAgreeInternalStatus(comm, newRuntime ?
            TileXR::TILEXR_SUCCESS : TileXR::TILEXR_ERROR_INTERNAL);
        if (ret != TileXR::TILEXR_SUCCESS) {
            if (needsOwner) delete newRuntime;
            (void)TileXRUDMAProfileUnregister(comm, candidate.profile);
            (void)aclrtFree(candidate.workspace);
            return ret;
        }
        const int localAttachStatus = needsOwner ?
            TileXRCommAttachInternalResource(comm, newRuntime, DestroyCollectiveRuntime) : TileXR::TILEXR_SUCCESS;
        ret = TileXRCommAgreeInternalStatus(comm, localAttachStatus);
        if (ret != TileXR::TILEXR_SUCCESS) {
            if (needsOwner) {
                if (localAttachStatus == TileXR::TILEXR_SUCCESS) (void)TileXRCommDetachInternalResource(comm, newRuntime);
                delete newRuntime;
            }
            (void)TileXRUDMAProfileUnregister(comm, candidate.profile);
            (void)aclrtFree(candidate.workspace);
            return ret;
        }
        if (!needsOwner) {
            candidate.captures = runtime->captures;
            *runtime = std::move(candidate);
        } else runtime = newRuntime;
    } else {
        const aclError syncRet = aclrtSynchronizeStream(stream);
        const int localSyncStatus = syncRet == ACL_SUCCESS ?
            TileXR::TILEXR_SUCCESS : TileXR::TILEXR_ERROR_MKIRT;
        ret = TileXRCommAgreeInternalStatus(comm, localSyncStatus);
        if (ret != TileXR::TILEXR_SUCCESS) {
            (void)TileXRUDMAProfileUnregister(comm, candidate.profile);
            (void)aclrtFree(candidate.workspace);
            return ret;
        }
        candidate.stream = stream;
        const TileXRUDMAProfileHandle oldProfile = runtime->profile;
        void *oldWorkspace = runtime->workspace;
        const int64_t previousMagic = runtime->previousMagic;
        const bool launched = runtime->launched;
        auto captures = runtime->captures;
        const bool cleanupPending = runtime->cleanupPending;
        std::vector<RetiredA5Resource> retiredResources =
            std::move(runtime->retiredResources);
        *runtime = candidate;
        runtime->previousMagic = previousMagic;
        runtime->launched = launched;
        runtime->captures = std::move(captures);
        runtime->cleanupPending = cleanupPending;
        runtime->retiredResources = std::move(retiredResources);
        RetiredA5Resource retired {oldProfile, oldWorkspace};
        const int localUnregisterStatus =
            TileXRUDMAProfileUnregister(comm, oldProfile);
        if (localUnregisterStatus == TileXR::TILEXR_SUCCESS) {
            retired.profile = 0U;
        }
        runtime->retiredResources.push_back(retired);
        ret = TileXRCommAgreeInternalStatus(comm, localUnregisterStatus);
        if (ret != TileXR::TILEXR_SUCCESS) {
            runtime->cleanupPending = true;
            return ret;
        }
        const int localFreeStatus = CleanupRetiredWorkspaces(*runtime);
        ret = TileXRCommAgreeInternalStatus(comm, localFreeStatus);
        if (ret != TileXR::TILEXR_SUCCESS) {
            runtime->cleanupPending = true;
            return ret;
        }
    }
    *runtimeOut = runtime;
    return TileXR::TILEXR_SUCCESS;
}

} // namespace

void *AcquireCollectiveProfiling(TileXRCommPtr comm, const HostLaunchContext &context,
    aclrtStream stream, uint32_t lanes, TileXR::TileXRType type,
    int64_t kernelCount, TileXR::TileXRDataType dataType, int reduction, int root)
{
#if !defined(TILEXR_ENABLE_PROFILING) || TILEXR_ENABLE_PROFILING == 0
    (void)comm; (void)context; (void)stream; (void)lanes; (void)type;
    (void)kernelCount; (void)dataType; (void)reduction; (void)root;
    return nullptr;
#else
    if(!context.hostArgs || context.hostArgs->rankSize<=0 || context.hostArgs->rank<0 ||
        context.hostArgs->rank>=context.hostArgs->rankSize) return nullptr;
    void *opaque = nullptr;
    if (TileXRCommGetInternalResource(comm, &opaque) != TileXR::TILEXR_SUCCESS) return nullptr;
    auto *runtime = static_cast<CommonCollectiveRuntime *>(opaque);
    if (!runtime) {
        std::unique_ptr<CommonCollectiveRuntime> candidate(new (std::nothrow) CommonCollectiveRuntime());
        if (!candidate) return nullptr;
        if (TileXRCommAttachInternalResource(comm, candidate.get(), DestroyCollectiveRuntime) != TileXR::TILEXR_SUCCESS) return nullptr;
        runtime = candidate.release();
    }
    if (runtime->destroying) return nullptr;
    const int64_t bytes=CountToBytes(kernelCount,dataType);
    if(bytes<=0) return nullptr;
    try {
        CollectiveProfilingWorkload workload;
        workload.rank=context.hostArgs->rank; workload.rankSize=context.hostArgs->rankSize; workload.lanes=lanes;
        workload.inputBytes=uint64_t(bytes);
        if(type==TileXR::TileXRType::REDUCE_SCATTER) {
            if(workload.inputBytes>UINT64_MAX/workload.rankSize) return nullptr;
            workload.inputBytes*=workload.rankSize;
        }
        workload.operation="TileXR collective type "+std::to_string(static_cast<int>(type));
        workload.shape="type="+std::to_string(static_cast<int>(type))+",count="+std::to_string(kernelCount)+
            ",dtype="+std::to_string(static_cast<int>(dataType))+",ranks="+std::to_string(workload.rankSize)+
            ",reduction="+std::to_string(reduction)+",root="+std::to_string(root);
        return AcquireCapture(*runtime,stream,workload);
    } catch (...) {
        return nullptr; // Optional metadata allocation must not fail the collective.
    }
#endif
}

int PrepareHostLaunchContext(TileXRCommPtr comm, HostLaunchContext &context)
{
    context = HostLaunchContext {};
    int ret = TileXRGetCommArgsHost(comm, context.hostArgs);
    if (ret != TileXR::TILEXR_SUCCESS) {
        return ret;
    }
    if (context.hostArgs == nullptr) {
        context = HostLaunchContext {};
        return TileXR::TILEXR_ERROR_NOT_INITIALIZED;
    }
    ret = TileXRGetCommArgsDev(comm, context.devArgs);
    if (ret != TileXR::TILEXR_SUCCESS) {
        context = HostLaunchContext {};
        return ret;
    }
    if (context.devArgs == nullptr) {
        context = HostLaunchContext {};
        return TileXR::TILEXR_ERROR_NOT_INITIALIZED;
    }
    return TileXR::TILEXR_SUCCESS;
}

bool IsA5AllToAllCandidate(const HostLaunchContext &context,
    TileXR::TileXRDataType dataType)
{
    if (context.hostArgs == nullptr || dataType != TileXR::TILEXR_DATA_TYPE_INT32) {
        return false;
    }
    const TileXR::CommArgs &args = *context.hostArgs;
    const uint32_t required = TileXR::ExtraFlag::TOPO_910A5 |
        TileXR::ExtraFlag::UDMA | TileXR::ExtraFlag::UDMA_SHARED_QP |
        TileXR::ExtraFlag::SDMA;
    return (args.extraFlag & required) == required && args.udmaInfoPtr != nullptr &&
        args.sdmaWorkspacePtr != nullptr && args.rank >= 0 &&
        args.rankSize > 0 && args.rank < args.rankSize &&
        args.localRankSize == static_cast<int32_t>(TileXRGrouped::kRanksPerNode) &&
        TileXRGrouped::ValidRankSize(static_cast<uint32_t>(args.rankSize)) &&
        args.creditMems[args.rank] != nullptr;
}

int LaunchA5AllToAll(TileXRCommPtr comm, const HostLaunchContext &context,
    void *sendBuf, void *recvBuf, int64_t sendCount, aclrtStream stream)
{
    if (comm == nullptr || context.hostArgs == nullptr || context.devArgs == nullptr ||
        sendBuf == nullptr || recvBuf == nullptr || sendCount <= 0 || stream == nullptr) {
        return TileXR::TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    bool groupedQpProfile = false;
    int ret = TileXRCommHasA5GroupedQpProfile(comm, &groupedQpProfile);
    if (ret != TileXR::TILEXR_SUCCESS || !groupedQpProfile) {
        return ret == TileXR::TILEXR_SUCCESS ?
            TileXR::TILEXR_ERROR_NOT_SUPPORT : ret;
    }
    InternalResourceGuard resourceGuard(comm);
    if (!resourceGuard.Locked()) {
        return TileXR::TILEXR_ERROR_INTERNAL;
    }
    const TileXRGrouped::Layout layout = TileXRGrouped::BuildLayout(
        static_cast<uint32_t>(context.hostArgs->rankSize),
        static_cast<uint64_t>(sendCount));
    if (!layout.valid) {
        return TileXR::TILEXR_ERROR_NOT_SUPPORT;
    }
    CommonCollectiveRuntime *runtime = nullptr;
    ret = GetOrCreateA5Runtime(
        comm, context, layout.workspaceBytes, stream, &runtime);
    if (ret != TileXR::TILEXR_SUCCESS) {
        return ret;
    }
    const aclError memsetRet = aclrtMemsetAsync(
        static_cast<uint8_t *>(runtime->workspace) + layout.statusOffset,
        TileXRGrouped::kStatusBytes, 0, TileXRGrouped::kStatusBytes, stream);
    if (memsetRet != ACL_SUCCESS) {
        return TileXR::TILEXR_ERROR_MKIRT;
    }
    int64_t magic = 0;
    ret = TileXRCommNextMagic(comm, &magic);
    if (ret != TileXR::TILEXR_SUCCESS || magic <= 0) {
        return ret == TileXR::TILEXR_SUCCESS ?
            TileXR::TILEXR_ERROR_INTERNAL : ret;
    }
    TileXRGrouped::KernelArgs args {};
    args.commArgs = context.devArgs;
    args.profileInfo = runtime->profileView.infoDev;
    args.profileRegistry = runtime->profileView.registryDev;
    args.input = static_cast<uint8_t *>(sendBuf);
    args.output = static_cast<uint8_t *>(recvBuf);
    args.workspace = static_cast<uint8_t *>(runtime->workspace);
    args.status = static_cast<uint8_t *>(runtime->workspace) + layout.statusOffset;
    args.elementsPerPeer = layout.elementsPerPeer;
    args.bytesPerPeer = layout.bytesPerPeer;
    args.bytesPerRank = layout.bytesPerRank;
    args.waitIterations = kA5WaitIterations;
    args.magic = magic;
    args.previousMagic = runtime->previousMagic;
    args.rank = static_cast<uint32_t>(context.hostArgs->rank);
    args.rankSize = layout.rankSize;
    args.groupCount = layout.groupCount;
    args.primaryQp = 0U;
    args.secondaryQp = 16U;
    args.primaryWeight = TileXRGrouped::kPrimaryRouteParts;
    args.secondaryWeight = TileXRGrouped::kRouteParts -
        TileXRGrouped::kPrimaryRouteParts;
    args.firstInvocation = runtime->launched ? 0U : 1U;
#if defined(TILEXR_ENABLE_PROFILING) && TILEXR_ENABLE_PROFILING == 1
    try {
        CollectiveProfilingWorkload workload;
        workload.rank=uint32_t(context.hostArgs->rank); workload.rankSize=uint32_t(context.hostArgs->rankSize);
        workload.lanes=TileXRGrouped::kSendCoreCount+TileXRGrouped::kGroupWidth;
        workload.inputBytes=layout.bytesPerRank; workload.bytesPerPeer=layout.bytesPerPeer;
        workload.groupedA5=true; workload.firstInvocation=args.firstInvocation!=0;
        workload.operation="AIV URMA Grouped AllToAll";
        workload.shape="elements_per_peer="+std::to_string(sendCount)+",dtype=int32,ranks="+std::to_string(workload.rankSize);
        args.profilingGM = static_cast<uint8_t *>(AcquireCapture(*runtime,stream,workload));
    } catch (...) {
        args.profilingGM=nullptr; // Profiling is best effort; preserve the kernel launch.
    }
#endif
    for (uint32_t plane = 0U; plane < TileXRGrouped::kPlaneCount; ++plane) {
        args.payloadOffset[plane] = layout.payloadOffset[plane];
        args.signalOffset[plane] = layout.signalOffset[plane];
    }
    args.signalSourceOffset = layout.signalSourceOffset;
    ret = LaunchA5AllToAllKernel(layout.rankSize, args, stream);
    if (ret == TileXR::TILEXR_SUCCESS) {
        runtime->previousMagic = magic;
        runtime->launched = true;
    }
    return ret;
}

} // namespace Host
} // namespace TileXRCollectives
