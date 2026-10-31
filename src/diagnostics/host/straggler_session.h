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
