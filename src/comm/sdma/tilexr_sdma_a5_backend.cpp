/*
 * Copyright (c) 2024-2026 TileXR Project
 * Licensed under the Apache License, Version 2.0
 */

#include "sdma/tilexr_sdma_a5_backend.h"

#include <array>
#include <cstring>
#include <dlfcn.h>
#include <limits>
#include <new>
#include <utility>
#include <vector>

#include "acl/acl.h"
#include "aclnn/aclnn_base.h"
#include "driver/ascend_hal.h"
#include "sdma/tilexr_sdma_a5_cleanup.h"
#include "tilexr_log.h"

#ifndef ACL_STREAM_DEVICE_USE_ONLY
#define ACL_STREAM_DEVICE_USE_ONLY 0x00000020U
#endif

namespace TileXR {
namespace {

constexpr size_t kBuiltinWorkspaceBytes = 16U * 1024U;
constexpr aclError kExpectedAicpuQueryFailure =
    static_cast<aclError>(detail::TILEXR_SDMA_A5_EXPECTED_QUERY_STATUS);
constexpr int32_t kDeviceInfoModuleType = 0;
constexpr int32_t kPhysicalDieInfoType = 19;

using RtGetDevicePhyIdByIndexFn = int32_t (*)(uint32_t, uint32_t*);
using RtStreamGetSqidFn = int32_t (*)(const void*, uint32_t*);
using RtStreamGetCqidFn = int32_t (*)(const void*, uint32_t*, uint32_t*);
using RtGetDeviceInfoFn = int32_t (*)(uint32_t, int32_t, int32_t, int64_t*);
using HalResAddrMapFn = drvError_t (*)(unsigned int, res_addr_info*,
                                       unsigned long*, unsigned int*);
using HalResAddrUnmapFn = drvError_t (*)(unsigned int, res_addr_info*);
using AclCreateTensorFn = aclTensor* (*)(const int64_t*, uint64_t, aclDataType,
                                         const int64_t*, int64_t, aclFormat,
                                         const int64_t*, uint64_t, void*);
using AclDestroyTensorFn = int32_t (*)(const aclTensor*);
using AclnnQueryWorkspaceFn = aclnnStatus (*)(const aclTensor*, aclTensor*,
                                               uint64_t*, aclOpExecutor**);
using AclnnQueryFn = aclnnStatus (*)(void*, uint64_t, aclOpExecutor*, aclrtStream);

template <typename T>
bool LoadSymbol(void* handle, const char* name, T& symbol)
{
    symbol = reinterpret_cast<T>(dlsym(handle, name));
    if (symbol != nullptr) {
        return true;
    }
    TILEXR_LOG(WARN) << "TileXR A5 SDMA missing runtime symbol " << name;
    return false;
}

class A5RuntimeApi {
public:
    ~A5RuntimeApi()
    {
        Close();
    }

    bool Load()
    {
        Close();
        runtimeHandle_ = dlopen("libruntime.so", RTLD_NOW | RTLD_LOCAL);
        if (runtimeHandle_ == nullptr) {
            TILEXR_LOG(WARN) << "TileXR A5 SDMA could not load libruntime.so: " << dlerror();
            return false;
        }
        if (!LoadSymbol(runtimeHandle_, "rtGetDevicePhyIdByIndex", getPhysicalDevice) ||
            !LoadSymbol(runtimeHandle_, "rtStreamGetSqid", getSqId) ||
            !LoadSymbol(runtimeHandle_, "rtStreamGetCqid", getCqId) ||
            !LoadSymbol(runtimeHandle_, "rtGetDeviceInfo", getDeviceInfo)) {
            Close();
            return false;
        }

        opapiHandle_ = dlopen("libopapi.so", RTLD_NOW | RTLD_LOCAL);
        if (opapiHandle_ == nullptr) {
            TILEXR_LOG(WARN) << "TileXR A5 SDMA could not load libopapi.so: " << dlerror();
            Close();
            return false;
        }
        if (!LoadSymbol(opapiHandle_, "aclCreateTensor", createTensor) ||
            !LoadSymbol(opapiHandle_, "aclDestroyTensor", destroyTensor) ||
            !LoadSymbol(opapiHandle_, "aclnnShmemSdmaStarsQueryGetWorkspaceSize", prepareQuery) ||
            !LoadSymbol(opapiHandle_, "aclnnShmemSdmaStarsQuery", executeQuery)) {
            Close();
            return false;
        }

        if (!LoadSymbol(RTLD_DEFAULT, "halResAddrMap", mapResource) ||
            !LoadSymbol(RTLD_DEFAULT, "halResAddrUnmap", unmapResource)) {
            Close();
            return false;
        }
        return true;
    }

    void Close()
    {
        getPhysicalDevice = nullptr;
        getSqId = nullptr;
        getCqId = nullptr;
        getDeviceInfo = nullptr;
        createTensor = nullptr;
        destroyTensor = nullptr;
        prepareQuery = nullptr;
        executeQuery = nullptr;
        mapResource = nullptr;
        unmapResource = nullptr;
        if (opapiHandle_ != nullptr) {
            (void)dlclose(opapiHandle_);
            opapiHandle_ = nullptr;
        }
        if (runtimeHandle_ != nullptr) {
            (void)dlclose(runtimeHandle_);
            runtimeHandle_ = nullptr;
        }
    }

    RtGetDevicePhyIdByIndexFn getPhysicalDevice = nullptr;
    RtStreamGetSqidFn getSqId = nullptr;
    RtStreamGetCqidFn getCqId = nullptr;
    RtGetDeviceInfoFn getDeviceInfo = nullptr;
    AclCreateTensorFn createTensor = nullptr;
    AclDestroyTensorFn destroyTensor = nullptr;
    AclnnQueryWorkspaceFn prepareQuery = nullptr;
    AclnnQueryFn executeQuery = nullptr;
    HalResAddrMapFn mapResource = nullptr;
    HalResAddrUnmapFn unmapResource = nullptr;

private:
    void* runtimeHandle_ = nullptr;
    void* opapiHandle_ = nullptr;
};

struct QuerySnapshot {
    uint32_t flag = 0U;
    uint32_t totalQueueCount = 0U;
    aclError syncStatus = ACL_SUCCESS;
    std::vector<detail::A5BuiltinChannelInfo> channels;
};

int CleanupSetCurrentContext(void*, void* context)
{
    return static_cast<int>(
        aclrtSetCurrentContext(static_cast<aclrtContext>(context)));
}

int CleanupDestroyStream(void*, void* stream)
{
    return static_cast<int>(aclrtDestroyStream(static_cast<aclrtStream>(stream)));
}

int CleanupDestroyContext(void*, void* context)
{
    return static_cast<int>(aclrtDestroyContext(static_cast<aclrtContext>(context)));
}

int CleanupFreeDevice(void*, void* address)
{
    return static_cast<int>(aclrtFree(address));
}

int CleanupDestroyTensor(void* opaque, const void* tensor)
{
    A5RuntimeApi* api = static_cast<A5RuntimeApi*>(opaque);
    return api == nullptr || api->destroyTensor == nullptr
        ? -1
        : api->destroyTensor(static_cast<const aclTensor*>(tensor));
}

detail::A5QueryCleanupOps MakeQueryCleanupOps(A5RuntimeApi& api)
{
    detail::A5QueryCleanupOps ops;
    ops.opaque = &api;
    ops.setCurrentContext = CleanupSetCurrentContext;
    ops.destroyStream = CleanupDestroyStream;
    ops.destroyContext = CleanupDestroyContext;
    ops.freeDevice = CleanupFreeDevice;
    ops.destroyTensor = CleanupDestroyTensor;
    return ops;
}

bool AllocateTrackedBuffer(std::vector<void*>& buffers, size_t bytes,
                           bool zero, void*& address)
{
    address = nullptr;
    if (aclrtMalloc(&address, bytes, ACL_MEM_MALLOC_HUGE_FIRST) != ACL_SUCCESS) {
        return false;
    }
    buffers.push_back(address);
    return !zero || aclrtMemset(address, bytes, 0, bytes) == ACL_SUCCESS;
}

bool CreateTrackedUint64Tensor(A5RuntimeApi& api,
                               detail::A5PendingQueryCleanup& cleanup,
                               void* address, int64_t elements,
                               aclTensor*& tensor)
{
    const int64_t shape[] = {elements};
    const int64_t strides[] = {1};
    tensor = api.createTensor(shape, 1U, ACL_UINT64, strides, 0,
                              ACL_FORMAT_ND, shape, 1U, address);
    if (tensor == nullptr) {
        return false;
    }
    cleanup.tensors.push_back(tensor);
    return true;
}

bool CheckRuntimeHealth(detail::A5PendingQueryCleanup& cleanup, void* scratch)
{
    aclrtStream stream = nullptr;
    const aclError createStatus = aclrtCreateStream(&stream);
    cleanup.healthStream = stream;
    if (createStatus != ACL_SUCCESS || stream == nullptr) {
        return false;
    }
    const aclError memsetStatus = aclrtMemsetAsync(
        scratch, sizeof(uint64_t), 0xA5, sizeof(uint64_t), stream);
    const aclError syncStatus = memsetStatus == ACL_SUCCESS
        ? aclrtSynchronizeStream(stream)
        : memsetStatus;
    uint64_t value = 0U;
    const aclError copyStatus = syncStatus == ACL_SUCCESS
        ? aclrtMemcpy(&value, sizeof(value), scratch, sizeof(value),
                      ACL_MEMCPY_DEVICE_TO_HOST)
        : syncStatus;
    return memsetStatus == ACL_SUCCESS && syncStatus == ACL_SUCCESS &&
        copyStatus == ACL_SUCCESS && value == 0xA5A5A5A5A5A5A5A5ULL;
}

bool FinishQuery(A5RuntimeApi& api,
                 detail::A5PendingQueryCleanup& cleanup,
                 std::vector<detail::A5PendingQueryCleanup>& pending,
                 bool result)
{
    const bool released = detail::CleanupA5QueryResources(
        cleanup, MakeQueryCleanupOps(api), cleanup.ownerContext);
    if (!cleanup.Empty()) {
        pending.push_back(std::move(cleanup));
    }
    return result && released;
}

void LogQueryStepFailure(const char* step, int64_t status)
{
    const char* recent = aclGetRecentErrMsg();
    TILEXR_LOG(WARN) << "TileXR A5 SDMA built-in query failed at " << step
                     << ", status " << status
                     << ", recent ACL error: "
                     << (recent == nullptr ? "<none>" : recent);
}

bool RunBuiltinQuery(A5RuntimeApi& api,
                     int32_t logicalDevice,
                     const std::vector<detail::A5BuiltinStreamInfo>& streams,
                     std::vector<detail::A5PendingQueryCleanup>& pending,
                     QuerySnapshot& snapshot)
{
    if (streams.empty() || streams.size() > detail::TILEXR_SDMA_A5_CHANNEL_COUNT) {
        return false;
    }

    detail::A5PendingQueryCleanup cleanup;
    aclrtContext ownerContext = nullptr;
    if (aclrtGetCurrentContext(&ownerContext) != ACL_SUCCESS ||
        ownerContext == nullptr) {
        TILEXR_LOG(WARN) << "TileXR A5 SDMA query requires an active owner context";
        return false;
    }
    cleanup.ownerContext = ownerContext;

    const size_t streamsBytes = streams.size() * sizeof(detail::A5BuiltinStreamInfo);
    void* streamsDev = nullptr;
    void* resourceDev = nullptr;
    void* builtinWorkspaceDev = nullptr;
    void* inputDev = nullptr;
    void* outputDev = nullptr;
    void* opWorkspaceDev = nullptr;
    if (!AllocateTrackedBuffer(cleanup.ownerBuffers, streamsBytes, false, streamsDev) ||
        !AllocateTrackedBuffer(cleanup.ownerBuffers,
                               sizeof(detail::A5BuiltinOpResource), true, resourceDev) ||
        !AllocateTrackedBuffer(cleanup.ownerBuffers,
                               kBuiltinWorkspaceBytes, true, builtinWorkspaceDev) ||
        !AllocateTrackedBuffer(cleanup.ownerBuffers,
                               2U * sizeof(uint64_t), false, inputDev) ||
        !AllocateTrackedBuffer(cleanup.ownerBuffers,
                               sizeof(uint64_t), true, outputDev) ||
        aclrtMemcpy(streamsDev, streamsBytes, streams.data(), streamsBytes,
                    ACL_MEMCPY_HOST_TO_DEVICE) != ACL_SUCCESS) {
        TILEXR_LOG(WARN) << "TileXR A5 SDMA query buffer setup failed";
        return FinishQuery(api, cleanup, pending, false);
    }

    detail::A5BuiltinOpResource resource {};
    resource.size = streams.size();
    resource.streamsAddress = reinterpret_cast<uint64_t>(streamsDev);
    resource.workspaceAddress = reinterpret_cast<uint64_t>(builtinWorkspaceDev);
    const uint64_t inputs[] = {
        reinterpret_cast<uint64_t>(resourceDev),
        reinterpret_cast<uint64_t>(builtinWorkspaceDev),
    };
    if (aclrtMemcpy(resourceDev, sizeof(resource), &resource, sizeof(resource),
                    ACL_MEMCPY_HOST_TO_DEVICE) != ACL_SUCCESS ||
        aclrtMemcpy(inputDev, sizeof(inputs), inputs, sizeof(inputs),
                    ACL_MEMCPY_HOST_TO_DEVICE) != ACL_SUCCESS) {
        TILEXR_LOG(WARN) << "TileXR A5 SDMA query resource upload failed";
        return FinishQuery(api, cleanup, pending, false);
    }

    aclrtContext isolated = nullptr;
    const aclError createContextStatus = aclrtCreateContext(&isolated, logicalDevice);
    cleanup.isolatedContext = isolated;
    if (createContextStatus != ACL_SUCCESS || isolated == nullptr) {
        TILEXR_LOG(WARN) << "TileXR A5 SDMA isolated query context creation failed";
        return FinishQuery(api, cleanup, pending, false);
    }

    aclTensor* inputTensor = nullptr;
    aclTensor* outputTensor = nullptr;
    aclrtStream queryStream = nullptr;
    bool queryLaunched = false;
    aclError syncStatus = ACL_SUCCESS;
    uint64_t opWorkspaceBytes = 0U;
    aclOpExecutor* executor = nullptr;
    bool ok = CreateTrackedUint64Tensor(api, cleanup, inputDev, 2, inputTensor);
    if (!ok) {
        LogQueryStepFailure("input tensor creation", -1);
    }
    if (ok) {
        ok = CreateTrackedUint64Tensor(api, cleanup, outputDev, 1, outputTensor);
        if (!ok) {
            LogQueryStepFailure("output tensor creation", -1);
        }
    }
    if (ok) {
        const aclnnStatus prepareStatus = api.prepareQuery(
            inputTensor, outputTensor, &opWorkspaceBytes, &executor);
        ok = prepareStatus == ACL_SUCCESS;
        if (!ok) {
            LogQueryStepFailure("workspace query", static_cast<int64_t>(prepareStatus));
        }
    }
    if (ok && opWorkspaceBytes != 0U) {
        ok = AllocateTrackedBuffer(cleanup.isolatedBuffers,
                                   static_cast<size_t>(opWorkspaceBytes), false,
                                   opWorkspaceDev);
        if (!ok) {
            LogQueryStepFailure("operator workspace allocation", -1);
        }
    }
    if (ok) {
        const aclError createStreamStatus = aclrtCreateStreamWithConfig(
            &queryStream, 0, ACL_STREAM_FAST_LAUNCH | ACL_STREAM_FAST_SYNC);
        cleanup.queryStream = queryStream;
        ok = createStreamStatus == ACL_SUCCESS && queryStream != nullptr;
        if (!ok) {
            LogQueryStepFailure("stream creation",
                                static_cast<int64_t>(createStreamStatus));
        }
    }
    if (ok) {
        aclrtStreamAttrValue failureMode {};
        failureMode.failureMode = 0;
        const aclError attributeStatus = aclrtSetStreamAttribute(
            queryStream, ACL_STREAM_ATTR_FAILURE_MODE, &failureMode);
        ok = attributeStatus == ACL_SUCCESS;
        if (!ok) {
            LogQueryStepFailure("stream failure-mode attribute",
                                static_cast<int64_t>(attributeStatus));
        }
    }
    if (ok) {
        const aclnnStatus launchStatus = api.executeQuery(
            opWorkspaceDev, opWorkspaceBytes, executor, queryStream);
        queryLaunched = launchStatus == ACL_SUCCESS;
        ok = queryLaunched;
        if (!ok) {
            LogQueryStepFailure("operator launch", static_cast<int64_t>(launchStatus));
        }
        if (queryLaunched) {
            syncStatus = aclrtSynchronizeStream(queryStream);
        }
    }

    if (aclrtSetCurrentContext(ownerContext) != ACL_SUCCESS) {
        TILEXR_LOG(WARN) << "TileXR A5 SDMA could not restore query owner context";
        return FinishQuery(api, cleanup, pending, false);
    }
    if (!ok || !queryLaunched) {
        TILEXR_LOG(WARN) << "TileXR A5 SDMA built-in query launch failed";
        return FinishQuery(api, cleanup, pending, false);
    }

    const size_t snapshotBytes = sizeof(detail::A5BuiltinWorkspaceHeader) +
        streams.size() * sizeof(detail::A5BuiltinChannelInfo);
    std::vector<uint8_t> bytes(snapshotBytes, 0U);
    if (aclrtMemcpy(bytes.data(), bytes.size(), builtinWorkspaceDev, bytes.size(),
                    ACL_MEMCPY_DEVICE_TO_HOST) != ACL_SUCCESS) {
        TILEXR_LOG(WARN) << "TileXR A5 SDMA query workspace download failed";
        return FinishQuery(api, cleanup, pending, false);
    }
    detail::A5BuiltinWorkspaceHeader header {};
    std::memcpy(&header, bytes.data(), sizeof(header));
    snapshot.flag = header.flag;
    snapshot.totalQueueCount = header.totalQueueCount;
    snapshot.syncStatus = syncStatus;
    snapshot.channels.resize(streams.size());
    std::memcpy(snapshot.channels.data(), bytes.data() + sizeof(header),
                snapshot.channels.size() * sizeof(snapshot.channels[0]));

    if (syncStatus == kExpectedAicpuQueryFailure &&
        !CheckRuntimeHealth(cleanup, outputDev)) {
        TILEXR_LOG(WARN) << "TileXR A5 SDMA context health check failed after expected AICPU error";
        return FinishQuery(api, cleanup, pending, false);
    }
    return FinishQuery(api, cleanup, pending, true);
}

int32_t QueryHostSq(uint32_t physicalDevice, uint32_t sqId,
                    drvSqCqPropType_t property, uint32_t (&values)[3])
{
    halSqCqQueryInfo query {};
    query.type = DRV_NORMAL_TYPE;
    query.tsId = 0U;
    query.sqId = sqId;
    query.cqId = 0U;
    query.prop = property;
    const drvError_t status = halSqCqQuery(physicalDevice, &query);
    values[0] = query.value[0];
    values[1] = query.value[1];
    values[2] = query.value[2];
    return static_cast<int32_t>(status);
}

} // namespace

struct TileXRA5SDMABackend::Impl {
    struct OwnedChannel {
        aclrtStream stream = nullptr;
        res_addr_info mapInfo {};
        bool mapped = false;
        uint64_t rtsqAddress = 0U;
        uint32_t rtsqLength = 0U;
        detail::A5HostChannelIdentity identity {};
        detail::A5BuiltinChannelInfo query {};
    };

    int32_t logicalDevice = -1;
    uint32_t physicalDevice = 0U;
    uint32_t physicalDieId = 0U;
    A5RuntimeApi api;
    aclrtContext ownerContext = nullptr;
    aclrtContext restoreContext = nullptr;
    bool restorePending = false;
    void* workspaceDev = nullptr;
    std::vector<detail::A5PendingQueryCleanup> pendingQueries;
    std::array<OwnedChannel, detail::TILEXR_SDMA_A5_CHANNEL_COUNT> channels {};

    bool HasOwnedResources() const
    {
        if (workspaceDev != nullptr || !pendingQueries.empty()) {
            return true;
        }
        for (const OwnedChannel& channel : channels) {
            if (channel.mapped || channel.stream != nullptr) {
                return true;
            }
        }
        return false;
    }

    void EraseCompletedQueries()
    {
        auto query = pendingQueries.begin();
        while (query != pendingQueries.end()) {
            if (query->Empty()) {
                query = pendingQueries.erase(query);
            } else {
                ++query;
            }
        }
    }
};

TileXRA5SDMABackend::TileXRA5SDMABackend() = default;

TileXRA5SDMABackend::~TileXRA5SDMABackend()
{
    (void)Shutdown();
}

bool TileXRA5SDMABackend::Init(int32_t deviceId)
{
    if (impl_ != nullptr) {
        TILEXR_LOG(ERROR) << "TileXR A5 SDMA backend contains state before initialization";
        return false;
    }
    std::unique_ptr<Impl> state(new (std::nothrow) Impl());
    if (state == nullptr) {
        return false;
    }
    state->logicalDevice = deviceId;
    if (aclrtGetCurrentContext(&state->ownerContext) != ACL_SUCCESS ||
        state->ownerContext == nullptr) {
        TILEXR_LOG(WARN) << "TileXR A5 SDMA requires an active owner context";
        return false;
    }

    if (!state->api.Load() ||
        state->api.getPhysicalDevice(
            static_cast<uint32_t>(deviceId), &state->physicalDevice) != 0) {
        TILEXR_LOG(WARN) << "TileXR A5 SDMA runtime discovery failed";
        return false;
    }
    int64_t physicalDie = -1;
    if (state->api.getDeviceInfo(
            static_cast<uint32_t>(deviceId), kDeviceInfoModuleType,
            kPhysicalDieInfoType, &physicalDie) != 0 ||
        physicalDie < 0 || static_cast<uint64_t>(physicalDie) >
            static_cast<uint64_t>(std::numeric_limits<uint32_t>::max())) {
        TILEXR_LOG(WARN) << "TileXR A5 SDMA physical die discovery failed";
        return false;
    }
    state->physicalDieId = static_cast<uint32_t>(physicalDie);
    impl_ = std::move(state);
    A5RuntimeApi& api = impl_->api;

    std::vector<detail::A5BuiltinStreamInfo> streamInfos;
    streamInfos.reserve(detail::TILEXR_SDMA_A5_CHANNEL_COUNT);
    for (uint32_t index = 0U; index < detail::TILEXR_SDMA_A5_CHANNEL_COUNT; ++index) {
        Impl::OwnedChannel& owned = impl_->channels[index];
        if (aclrtCreateStreamWithConfig(
                &owned.stream, 0, ACL_STREAM_DEVICE_USE_ONLY) != ACL_SUCCESS) {
            TILEXR_LOG(WARN) << "TileXR A5 SDMA STARS stream creation failed at channel " << index;
            Shutdown();
            return false;
        }
        int32_t streamId = -1;
        uint32_t sqId = 0U;
        uint32_t cqId = 0U;
        uint32_t logicalCqId = 0U;
        if (aclrtStreamGetId(owned.stream, &streamId) != ACL_SUCCESS || streamId < 0 ||
            api.getSqId(owned.stream, &sqId) != 0 ||
            api.getCqId(owned.stream, &cqId, &logicalCqId) != 0) {
            TILEXR_LOG(WARN) << "TileXR A5 SDMA stream identifier query failed at channel " << index;
            Shutdown();
            return false;
        }
        owned.identity = {
            static_cast<uint32_t>(streamId), sqId, cqId, logicalCqId, impl_->physicalDieId,
        };
        owned.mapInfo.id = 0U;
        owned.mapInfo.target_proc_type = PROCESS_CP1;
        owned.mapInfo.res_type = RES_ADDR_TYPE_STARS_RTSQ;
        owned.mapInfo.res_id = sqId;
        unsigned long mappedAddress = 0UL;
        unsigned int mappedLength = 0U;
        const drvError_t mapStatus = api.mapResource(
            impl_->physicalDevice, &owned.mapInfo, &mappedAddress, &mappedLength);
        owned.mapped = mapStatus == DRV_ERROR_NONE;
        if (mapStatus != DRV_ERROR_NONE || mappedAddress == 0UL ||
            mappedLength < sizeof(uint32_t)) {
            TILEXR_LOG(WARN) << "TileXR A5 SDMA RTSQ map failed at channel " << index
                             << ", status " << mapStatus;
            Shutdown();
            return false;
        }
        owned.rtsqAddress = static_cast<uint64_t>(mappedAddress);
        owned.rtsqLength = mappedLength;

        detail::A5BuiltinStreamInfo info {};
        info.stream = reinterpret_cast<uint64_t>(owned.stream);
        info.context = reinterpret_cast<uint64_t>(impl_->ownerContext);
        info.streamId = streamId;
        info.sqId = sqId;
        info.cqId = cqId;
        info.logicalCqId = logicalCqId;
        info.deviceId = static_cast<int32_t>(impl_->physicalDieId);
        streamInfos.push_back(info);
    }

    QuerySnapshot batch;
    if (!RunBuiltinQuery(
            api, deviceId, streamInfos, impl_->pendingQueries, batch)) {
        Shutdown();
        return false;
    }
    const detail::A5QueryResultKind batchKind = batch.channels.empty()
        ? detail::A5QueryResultKind::INVALID
        : detail::ClassifyA5QueryResult(
            static_cast<int32_t>(batch.syncStatus), batch.flag, batch.totalQueueCount,
            batch.channels[0], impl_->channels[0].identity);
    if (batchKind == detail::A5QueryResultKind::COMPLETE) {
        if (batch.channels.size() != detail::TILEXR_SDMA_A5_CHANNEL_COUNT) {
            Shutdown();
            return false;
        }
        for (uint32_t index = 0U; index < detail::TILEXR_SDMA_A5_CHANNEL_COUNT; ++index) {
            if (!detail::ValidateA5BuiltinChannel(
                    batch.channels[index], impl_->channels[index].identity, true)) {
                TILEXR_LOG(WARN) << "TileXR A5 SDMA complete query validation failed at channel " << index;
                Shutdown();
                return false;
            }
            impl_->channels[index].query = batch.channels[index];
        }
    } else if (batchKind == detail::A5QueryResultKind::EXPECTED_PARTIAL) {
        impl_->channels[0].query = batch.channels[0];
        for (uint32_t index = 1U; index < detail::TILEXR_SDMA_A5_CHANNEL_COUNT; ++index) {
            std::vector<detail::A5BuiltinStreamInfo> oneStream(1U, streamInfos[index]);
            QuerySnapshot isolated;
            if (!RunBuiltinQuery(
                    api, deviceId, oneStream, impl_->pendingQueries, isolated) ||
                isolated.channels.size() != 1U ||
                detail::ClassifyA5QueryResult(
                    static_cast<int32_t>(isolated.syncStatus), isolated.flag,
                    isolated.totalQueueCount, isolated.channels[0],
                    impl_->channels[index].identity) !=
                    detail::A5QueryResultKind::EXPECTED_PARTIAL) {
                TILEXR_LOG(WARN) << "TileXR A5 SDMA isolated query validation failed at channel " << index;
                Shutdown();
                return false;
            }
            impl_->channels[index].query = isolated.channels[0];
        }
    } else {
        TILEXR_LOG(WARN) << "TileXR A5 SDMA query returned unsupported status " << batch.syncStatus;
        Shutdown();
        return false;
    }

    detail::A5SdmaWorkspace hostWorkspace {};
    hostWorkspace.header.magic = detail::TILEXR_SDMA_A5_WORKSPACE_MAGIC;
    hostWorkspace.header.abiVersion = detail::TILEXR_SDMA_A5_ABI_VERSION;
    hostWorkspace.header.backendKind = detail::TILEXR_SDMA_A5_BACKEND_KIND;
    hostWorkspace.header.channelCount = detail::TILEXR_SDMA_A5_CHANNEL_COUNT;
    hostWorkspace.header.sqeSize = detail::TILEXR_SDMA_A5_SQE_BYTES;
    hostWorkspace.header.channelStride = sizeof(detail::A5SdmaChannel);
    hostWorkspace.header.workspaceSize = sizeof(hostWorkspace);
    hostWorkspace.header.maxTransferBytes =
        static_cast<uint32_t>(detail::TILEXR_SDMA_A5_MAX_TRANSFER_BYTES);

    if (aclrtMalloc(&impl_->workspaceDev, sizeof(hostWorkspace),
                    ACL_MEM_MALLOC_HUGE_FIRST) != ACL_SUCCESS) {
        TILEXR_LOG(WARN) << "TileXR A5 SDMA workspace allocation failed";
        Shutdown();
        return false;
    }
    const uint64_t workspaceBase = reinterpret_cast<uint64_t>(impl_->workspaceDev);
    for (uint32_t index = 0U; index < detail::TILEXR_SDMA_A5_CHANNEL_COUNT; ++index) {
        const Impl::OwnedChannel& owned = impl_->channels[index];
        detail::A5SdmaChannel& channel = hostWorkspace.channels[index];
        channel.sqBase = owned.query.sqBase;
        channel.rtsqAddress = owned.rtsqAddress;
        channel.completionPayloadAddress = workspaceBase +
            offsetof(detail::A5SdmaWorkspace, completionPayloads) +
            index * sizeof(detail::A5SdmaCompletionLine);
        channel.completionRecordAddress = workspaceBase +
            offsetof(detail::A5SdmaWorkspace, completionRecords) +
            index * sizeof(detail::A5SdmaCompletionLine);
        channel.depth = owned.query.sqDepth;
        channel.head = owned.query.sqHead;
        channel.tail = owned.query.sqTail;
        channel.taskId = detail::A5SdmaQueueDistance(
            owned.query.sqHead, owned.query.sqTail, owned.query.sqDepth);
        channel.rtsqLength = owned.rtsqLength;
        channel.streamId = owned.identity.streamId;
        channel.sqId = owned.identity.sqId;
        channel.cqId = owned.identity.cqId;
        channel.logicalCqId = owned.identity.logicalCqId;
        channel.physicalDieId = owned.identity.physicalDieId;

        uint32_t tailValues[3] = {0U, 0U, 0U};
        uint32_t sqeSizeValues[3] = {0U, 0U, 0U};
        if (QueryHostSq(impl_->physicalDevice, channel.sqId,
                        DRV_SQCQ_PROP_SQ_TAIL, tailValues) != 0 ||
            QueryHostSq(impl_->physicalDevice, channel.sqId,
                        DRV_SQCQ_PROP_SQE_SIZE, sqeSizeValues) != 0 ||
            tailValues[0] != channel.tail ||
            sqeSizeValues[0] != detail::TILEXR_SDMA_A5_SQE_BYTES) {
            TILEXR_LOG(WARN) << "TileXR A5 SDMA Host SQ cross-check failed at channel " << index;
            Shutdown();
            return false;
        }
    }
    if (aclrtMemcpy(impl_->workspaceDev, sizeof(hostWorkspace), &hostWorkspace,
                    sizeof(hostWorkspace), ACL_MEMCPY_HOST_TO_DEVICE) != ACL_SUCCESS) {
        TILEXR_LOG(WARN) << "TileXR A5 SDMA workspace upload failed";
        Shutdown();
        return false;
    }

    TILEXR_LOG(INFO) << "TileXR A5 direct SDMA initialized on device " << deviceId
                     << " with " << detail::TILEXR_SDMA_A5_CHANNEL_COUNT << " channels";
    return true;
}

bool TileXRA5SDMABackend::Shutdown()
{
    if (impl_ == nullptr) {
        return true;
    }
    if (impl_->restorePending) {
        if (aclrtSetCurrentContext(impl_->restoreContext) != ACL_SUCCESS) {
            TILEXR_LOG(WARN) << "TileXR A5 SDMA could not complete pending context restore";
            return false;
        }
        impl_->restoreContext = nullptr;
        impl_->restorePending = false;
        if (!impl_->HasOwnedResources()) {
            impl_.reset();
            return true;
        }
    }

    const detail::A5QueryCleanupOps queryCleanupOps =
        MakeQueryCleanupOps(impl_->api);
    for (size_t reverse = impl_->pendingQueries.size(); reverse > 0U; --reverse) {
        detail::A5PendingQueryCleanup& query = impl_->pendingQueries[reverse - 1U];
        if (query.restorePending &&
            !detail::CleanupA5QueryResources(
                query, queryCleanupOps, query.restoreContext)) {
            TILEXR_LOG(WARN) << "TileXR A5 SDMA query context restore remains pending";
            return false;
        }
    }
    impl_->EraseCompletedQueries();

    aclrtContext previous = nullptr;
    if (aclrtGetCurrentContext(&previous) != ACL_SUCCESS) {
        TILEXR_LOG(WARN) << "TileXR A5 SDMA could not capture current context for cleanup";
        return false;
    }
    for (size_t reverse = impl_->pendingQueries.size(); reverse > 0U; --reverse) {
        detail::A5PendingQueryCleanup& query = impl_->pendingQueries[reverse - 1U];
        if (!detail::CleanupA5QueryResources(query, queryCleanupOps, previous)) {
            impl_->EraseCompletedQueries();
            TILEXR_LOG(WARN) << "TileXR A5 SDMA query cleanup incomplete; retained for retry";
            return false;
        }
    }
    impl_->EraseCompletedQueries();

    if (impl_->ownerContext == nullptr ||
        aclrtSetCurrentContext(impl_->ownerContext) != ACL_SUCCESS) {
        TILEXR_LOG(WARN) << "TileXR A5 SDMA could not switch to owner context for cleanup";
        return false;
    }

    if (impl_->workspaceDev != nullptr) {
        if (aclrtFree(impl_->workspaceDev) == ACL_SUCCESS) {
            impl_->workspaceDev = nullptr;
        }
    }
    if (impl_->workspaceDev == nullptr) {
        for (size_t reverse = detail::TILEXR_SDMA_A5_CHANNEL_COUNT; reverse > 0U; --reverse) {
            Impl::OwnedChannel& owned = impl_->channels[reverse - 1U];
            bool mappingReleased = true;
            if (owned.mapped) {
                if (impl_->api.unmapResource(
                        impl_->physicalDevice, &owned.mapInfo) == DRV_ERROR_NONE) {
                    owned.mapped = false;
                } else {
                    mappingReleased = false;
                }
            }
            if (mappingReleased && owned.stream != nullptr &&
                aclrtDestroyStream(owned.stream) == ACL_SUCCESS) {
                owned.stream = nullptr;
            }
        }
    }
    if (previous != impl_->ownerContext &&
        aclrtSetCurrentContext(previous) != ACL_SUCCESS) {
        TILEXR_LOG(WARN) << "TileXR A5 SDMA could not restore previous context after cleanup";
        impl_->restoreContext = previous;
        impl_->restorePending = true;
        return false;
    }
    if (impl_->HasOwnedResources()) {
        TILEXR_LOG(WARN) << "TileXR A5 SDMA cleanup incomplete; retained resources for retry";
        return false;
    }
    impl_.reset();
    return true;
}

GM_ADDR TileXRA5SDMABackend::GetWorkspaceDev() const
{
    return impl_ == nullptr ? nullptr : static_cast<GM_ADDR>(impl_->workspaceDev);
}

} // namespace TileXR
