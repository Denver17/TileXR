/*
 * 功能：定义诊断 session 的校准状态、buffer、待完成 stream 记录和可替换的 runtime hooks。
 * 调用关系：straggler_session.cpp 实现会话操作，clock_kernel.cpp 读取 launch 所需字段；unit test 替换 hooks。
 * 资源与生命周期：session 持有 device allocation 和 RDMA registration，借用 communicator 与调用方的 stream。
 * Purpose: Define calibration state, buffers, pending-stream records and replaceable runtime hooks.
 * Calls: straggler_session.cpp implements operations, clock_kernel.cpp reads launch state and unit tests replace hooks.
 * Lifetime: Sessions own device allocations/RDMA registration and borrow the communicator and caller streams.
 */
#ifndef TILEXR_DIAGNOSTICS_HOST_STRAGGLER_SESSION_H
#define TILEXR_DIAGNOSTICS_HOST_STRAGGLER_SESSION_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <mutex>
#include <thread>
#include <vector>

#include "acl/acl_base.h"
#include "comm_args.h"
#include "tilexr_straggler.h"
#include "tilexr_straggler_layout.h"
#include "tilexr_types.h"
#include "clock_kernel.h"
#include "clock_drift.h"
#include "tilexr_rdma.h"

int TileXRCommHostAllGather(TileXRCommPtr comm, const void *sendBuf, size_t sendBytes, void *recvBuf);
int TileXRCommHostAllGatherTimeout(TileXRCommPtr comm, const void *sendBuf, size_t sendBytes,
                                   void *recvBuf, uint32_t timeoutMs);

namespace TileXRDiagnostics {
namespace Host {

struct StragglerSession {
    TileXRCommPtr comm = nullptr;
    TileXRStragglerConfig config {};
    std::string outputDir;
    TileXR::CommArgs *hostArgs = nullptr;
    GM_ADDR devArgs = nullptr;
    void *deviceSamples = nullptr;
    size_t deviceSamplesBytes = 0;
    void *deviceRecords = nullptr;
    bool clockUsesRdma = false;
    bool clockRdmaDeferred = false;
    bool clockRdmaCompact = false;
    bool clockRdmaReady = false;
    bool clockRdmaFailed = false;
    bool clockRdmaParticipating = false;
    int clockRdmaSetupStatus = TileXR::TILEXR_SUCCESS;
    void *clockRdmaMemory = nullptr;
    TileXRRDMAMemHandle clockRdmaHandle = 0;
    std::vector<TileXRRDMADeviceView> clockRdmaViews;
    size_t deviceRecordsBytes = 0;
    std::vector<TileXR::TileXRStragglerDeviceControl> recordControls;
    std::vector<int> recordStatuses;
    std::vector<aclrtStream> recordStreams;
    std::vector<aclrtStream> outstandingStreams;
    aclrtStream clockStream = nullptr;
    uint64_t localLaunchId = 0;
    uint64_t nextCollectId = 1;
    uint64_t droppedRecords = 0;
    uint32_t generation = 0;
    uint64_t cyclesPerSecond = 0;
    TileXRClockSyncResult clock {};
    ClockDriftTracker clockDrift;
    std::vector<TileXRClockDriftModel> clockDriftModels;
    uint64_t clockDriftFrozenHostNs = 0;
    uint64_t calibrationHostNs = 0;
    int observationStatus = TileXR::TILEXR_SUCCESS;
    int testDelayRank = -1;
    uint64_t testDelayCycles = 0;
    uint64_t controlSequence = 0;
    std::thread::id ownerThread;
    std::mutex stateMutex;
};

struct StragglerRuntimeHooks {
    aclError (*mallocDevice)(void **ptr, size_t bytes);
    aclError (*freeDevice)(void *ptr);
    aclError (*memsetDeviceAsync)(void *ptr, size_t bytes, int value, size_t count, aclrtStream stream);
    aclError (*copyHostToDeviceAsync)(void *dst, size_t dstBytes, const void *src, size_t bytes,
                                     aclrtStream stream);
    aclError (*copyDeviceToHost)(void *dst, size_t dstBytes, const void *src, size_t bytes);
    aclError (*synchronizeStream)(aclrtStream stream);
    int (*getCommArgsHost)(TileXRCommPtr comm, TileXR::CommArgs *&args);
    int (*getCommArgsDev)(TileXRCommPtr comm, GM_ADDR &args);
    int (*nextMagic)(TileXRCommPtr comm, int64_t *magic);
    int (*hostAllGather)(TileXRCommPtr comm, const void *sendBuf, size_t sendBytes, void *recvBuf,
                         uint32_t timeoutMs);
    int (*launchClockSync)(const StragglerSession &session, uint32_t warmupSamples,
                           uint32_t samples, int64_t magic, uint64_t timeoutCycles,
                           aclrtStream stream);
    uint64_t (*steadyNowNs)();
    const char *(*getRecentError)();
    int (*rdmaQuery)(void *comm, TileXRRDMAInfo *info) = nullptr;
    int (*rdmaRegister)(void *comm, void *ptr, size_t bytes, TileXRRDMAMemHandle *handle) = nullptr;
    int (*rdmaUnregister)(void *comm, TileXRRDMAMemHandle handle) = nullptr;
    int (*rdmaGetView)(void *comm, TileXRRDMAMemHandle handle, int peer,
                        TileXRRDMADeviceView *view) = nullptr;
    int (*rdmaShutdown)(void *comm) = nullptr;
};

void SetStragglerRuntimeHooksForTest(const StragglerRuntimeHooks *hooks);
void SetStragglerTestDelayForHost(StragglerSession *session, int rank, uint64_t cycles);

StragglerSession *FindStragglerSession(TileXRCommPtr comm);
int BeginObservation(StragglerSession *session, const TileXRStragglerOperation *operation,
                     aclrtStream stream, TileXRStragglerObservation *observation);
int EndObservation(StragglerSession *session, const TileXRStragglerObservation *observation,
                   aclrtStream stream, int launchStatus);

} // namespace Host
} // namespace TileXRDiagnostics

#endif // TILEXR_DIAGNOSTICS_HOST_STRAGGLER_SESSION_H
