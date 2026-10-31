#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <thread>
#include <vector>

#include "straggler_session.h"
#include "tilexr_straggler.h"
#include "tilexr_types.h"

namespace {

int g_failures = 0;
int g_mallocCall = 0;
int g_failMallocCall = 0;
int g_freeCalls = 0;
aclError g_memsetStatus = ACL_SUCCESS;
aclError g_copyStatus = ACL_SUCCESS;
aclError g_syncStatus = ACL_SUCCESS;
int g_nextMagicStatus = TileXR::TILEXR_SUCCESS;
int g_nextMagicCalls = 0;
int g_allGatherStatus = TileXR::TILEXR_SUCCESS;
int g_clockLaunchStatus = TileXR::TILEXR_SUCCESS;
uint32_t g_launchedWarmupSamples = 0;
uint32_t g_launchedMeasuredSamples = 0;
int g_remoteControlStatus = TileXR::TILEXR_SUCCESS;
bool g_remoteSequenceMismatch = false;
uint64_t g_nowNs = 1000000000ull;
uint64_t g_cycle = 1000;
TileXR::CommArgs g_commArgs {};
std::vector<aclrtStream> g_synchronizedStreams;

template <typename T>
void CheckEq(const char *label, T actual, T expected)
{
    if (actual != expected) {
        std::cerr << label << " actual=" << actual << " expected=" << expected << std::endl;
        ++g_failures;
    }
}

void CheckTrue(const char *label, bool condition)
{
    if (!condition) {
        std::cerr << label << " failed" << std::endl;
        ++g_failures;
    }
}

aclError MallocDevice(void **ptr, size_t bytes)
{
    ++g_mallocCall;
    if (g_mallocCall == g_failMallocCall) {
        *ptr = nullptr;
        return 1;
    }
    *ptr = std::calloc(1, bytes);
    return *ptr == nullptr ? 1 : ACL_SUCCESS;
}

aclError FreeDevice(void *ptr)
{
    ++g_freeCalls;
    std::free(ptr);
    return ACL_SUCCESS;
}

aclError MemsetDeviceAsync(void *ptr, size_t bytes, int value, size_t count, aclrtStream)
{
    if (g_memsetStatus == ACL_SUCCESS) {
        std::memset(ptr, value, count <= bytes ? count : bytes);
    }
    return g_memsetStatus;
}

aclError CopyHostToDeviceAsync(void *dst, size_t dstBytes, const void *src, size_t bytes, aclrtStream)
{
    if (g_copyStatus == ACL_SUCCESS && bytes <= dstBytes) {
        std::memcpy(dst, src, bytes);
    }
    return g_copyStatus;
}

aclError CopyDeviceToHost(void *dst, size_t dstBytes, const void *src, size_t bytes)
{
    if (g_copyStatus == ACL_SUCCESS && bytes <= dstBytes) {
        std::memcpy(dst, src, bytes);
    }
    return g_copyStatus;
}

aclError SynchronizeStream(aclrtStream stream)
{
    g_synchronizedStreams.push_back(stream);
    return g_syncStatus;
}

int GetCommArgsHost(TileXRCommPtr, TileXR::CommArgs *&args)
{
    args = &g_commArgs;
    return TileXR::TILEXR_SUCCESS;
}

int GetCommArgsDev(TileXRCommPtr, GM_ADDR &args)
{
    args = reinterpret_cast<GM_ADDR>(0x2000);
    return TileXR::TILEXR_SUCCESS;
}

int NextMagic(TileXRCommPtr, int64_t *magic)
{
    ++g_nextMagicCalls;
    if (g_nextMagicStatus == TileXR::TILEXR_SUCCESS) {
        *magic = 7;
    }
    return g_nextMagicStatus;
}

int HostAllGather(TileXRCommPtr, const void *sendBuf, size_t sendBytes, void *recvBuf, uint32_t)
{
    if (g_allGatherStatus == TileXR::TILEXR_SUCCESS) {
        uint64_t firstWord = 0;
        std::memcpy(&firstWord, sendBuf, sizeof(firstWord));
        for (int rank = 0; rank < g_commArgs.rankSize; ++rank) {
            std::memcpy(static_cast<uint8_t *>(recvBuf) + rank * sendBytes, sendBuf, sendBytes);
            if (sendBytes == 64u && firstWord == 0x5458525354524147ull) {
                std::memcpy(static_cast<uint8_t *>(recvBuf) + rank * sendBytes + 40u,
                            &rank, sizeof(rank));
                if (rank == 1) {
                    std::memcpy(static_cast<uint8_t *>(recvBuf) + rank * sendBytes + 32u,
                                &g_remoteControlStatus, sizeof(g_remoteControlStatus));
                    if (g_remoteSequenceMismatch) {
                        const uint64_t badSequence = 999;
                        std::memcpy(static_cast<uint8_t *>(recvBuf) + rank * sendBytes + 8u,
                                    &badSequence, sizeof(badSequence));
                    }
                }
            } else if (sendBytes == sizeof(TileXR::TileXRStragglerTimingRecord)) {
                std::memcpy(static_cast<uint8_t *>(recvBuf) + rank * sendBytes +
                                offsetof(TileXR::TileXRStragglerTimingRecord, rank),
                            &rank, sizeof(rank));
            }
        }
        TileXRClockRankResult *clock = static_cast<TileXRClockRankResult *>(recvBuf);
        if (sendBytes == sizeof(TileXRClockRankResult) && firstWord == 0) {
            clock->errorBoundCycles = 7;
            clock->errorBoundNs = 7;
        }
    }
    return g_allGatherStatus;
}

int LaunchClock(const TileXRDiagnostics::Host::StragglerSession &session, uint32_t warmupSamples,
                uint32_t samples, int64_t, uint64_t, aclrtStream)
{
    g_launchedWarmupSamples = warmupSamples;
    g_launchedMeasuredSamples = samples;
    if (g_clockLaunchStatus == TileXR::TILEXR_SUCCESS) {
        auto *output = static_cast<TileXR::TileXRClockSyncDeviceSample *>(session.deviceSamples);
        for (uint32_t index = 0; index < samples; ++index) {
            output[index].t0 = 1000 + index * 100;
            output[index].t1 = output[index].t0;
            output[index].t2 = output[index].t0;
            output[index].t3 = output[index].t0;
            output[index].sequence = index;
            output[index].status = 0;
        }
    }
    return g_clockLaunchStatus;
}

uint64_t SteadyNowNs()
{
    return g_nowNs;
}

const char *GetRecentError()
{
    return "injected runtime failure";
}

const TileXRDiagnostics::Host::StragglerRuntimeHooks kHooks {
    MallocDevice,
    FreeDevice,
    MemsetDeviceAsync,
    CopyHostToDeviceAsync,
    CopyDeviceToHost,
    SynchronizeStream,
    GetCommArgsHost,
    GetCommArgsDev,
    NextMagic,
    HostAllGather,
    LaunchClock,
    SteadyNowNs,
    GetRecentError,
};

void ResetHooks()
{
    TileXRDiagnostics::Host::SetStragglerRuntimeHooksForTest(&kHooks);
    g_mallocCall = 0;
    g_failMallocCall = 0;
    g_freeCalls = 0;
    g_memsetStatus = ACL_SUCCESS;
    g_copyStatus = ACL_SUCCESS;
    g_syncStatus = ACL_SUCCESS;
    g_nextMagicStatus = TileXR::TILEXR_SUCCESS;
    g_nextMagicCalls = 0;
    g_allGatherStatus = TileXR::TILEXR_SUCCESS;
    g_clockLaunchStatus = TileXR::TILEXR_SUCCESS;
    g_launchedWarmupSamples = 0;
    g_launchedMeasuredSamples = 0;
    g_remoteControlStatus = TileXR::TILEXR_SUCCESS;
    g_remoteSequenceMismatch = false;
    g_synchronizedStreams.clear();
    g_nowNs = 1000000000ull;
    g_cycle = 1000;
    g_commArgs = TileXR::CommArgs {};
    g_commArgs.rank = 0;
    g_commArgs.rankSize = 1;
    g_commArgs.extraFlag = TileXR::ExtraFlag::PERF_CYCLE_1GHZ;
    g_commArgs.peerMems[0] = reinterpret_cast<GM_ADDR>(0x1000);
}

TileXRStragglerConfig Config(uint32_t capacity = 4)
{
    TileXRStragglerConfig config {};
    CheckEq("default config", TileXRStragglerConfigDefault(&config),
            TileXR::TILEXR_SUCCESS);
    config.recordCapacity = capacity;
    config.maxCalibrationAgeMs = 1;
    CheckEq("default warmup samples", config.clockSyncWarmupSamples, 32u);
    CheckEq("default measured samples", config.clockSyncSamples, 128u);
    return config;
}

TileXRStraggler CreateOn(TileXRCommPtr comm,
                                   const TileXRStragglerConfig &config)
{
    TileXRStraggler detector = nullptr;
    CheckEq("create detector", TileXRStragglerCreate(
        comm, &config, &detector), TileXR::TILEXR_SUCCESS);
    CheckTrue("detector allocated", detector != nullptr);
    return detector;
}

TileXRStraggler Create(const TileXRStragglerConfig &config)
{
    return CreateOn(reinterpret_cast<TileXRCommPtr>(0x1), config);
}

void Calibrate(TileXRStraggler detector)
{
    TileXRClockSyncResult result {};
    CheckEq("clock synchronize", TileXRClockSynchronize(
        detector, reinterpret_cast<aclrtStream>(0x1), &result), TileXR::TILEXR_SUCCESS);
    CheckEq("clock rank size", result.rankSize, 1u);
    CheckEq("clock frequency", result.cyclesPerSecond, static_cast<uint64_t>(1000000000ull));
    CheckEq("public clock precision", result.precisionNs, static_cast<uint64_t>(7));
    CheckEq("public warmup sample count", result.warmupSampleCount, 32u);
    CheckEq("public measured sample count", result.sampleCount, 128u);
    CheckEq("launched warmup samples", g_launchedWarmupSamples, 32u);
    CheckEq("launched measured samples", g_launchedMeasuredSamples, 128u);
}

void ObserveOnStream(TileXRStraggler detector, aclrtStream stream,
                     void **record, uint64_t *launchId)
{
    auto *session = static_cast<TileXRDiagnostics::Host::StragglerSession *>(detector);
    *record = nullptr;
    *launchId = 0;
    TileXRStragglerOperation operation {};
    operation.componentKind = TILEXR_STRAGGLER_COMPONENT_COLLECTIVES;
    operation.operationKind = static_cast<uint32_t>(TileXR::TileXRType::ALL_REDUCE);
    operation.operationId = g_cycle;
    operation.messageBytes = 4096;
    TileXRStragglerObservation observation {};
    const int beginStatus = TileXRStragglerObservationBegin(
        detector, &operation, stream, &observation);
    if (beginStatus == TileXR::TILEXR_SUCCESS) {
        *record = observation.deviceRecord;
        *launchId = observation.launchId;
    }
    if (*record != nullptr) {
        auto *timing = static_cast<TileXR::TileXRStragglerTimingRecord *>(*record);
        timing->launchId = *launchId;
        timing->operationId = observation.operationId;
        timing->messageBytes = observation.messageBytes;
        timing->startCycle = g_cycle;
        timing->readyCycle = g_cycle;
        timing->endCycle = g_cycle + 100;
        timing->generation = session->generation;
        timing->componentKind = observation.componentKind;
        timing->operationKind = observation.operationKind;
        timing->stageId = observation.stageId;
        timing->rank = static_cast<uint32_t>(session->hostArgs->rank);
        timing->status = TileXR::TILEXR_TIMING_RECORD_STATUS_COMPLETE;
        timing->reserved = 0;
        timing->blockCount = 1;
        g_cycle += 1000;
        TileXRStragglerObservationEnd(
            detector, &observation, stream, TileXR::TILEXR_SUCCESS);
    }
}

void Observe(TileXRStraggler detector, void **record, uint64_t *launchId)
{
    ObserveOnStream(detector, reinterpret_cast<aclrtStream>(0x1), record, launchId);
}

void TestValidationAndAllocationFailures()
{
    ResetHooks();
    CheckEq("default rejects null", TileXRStragglerConfigDefault(nullptr),
            TileXR::TILEXR_ERROR_PARA_CHECK_FAIL);
    TileXRStraggler detector = reinterpret_cast<TileXRStraggler>(0x1);
    TileXRStragglerConfig invalid = Config();
    invalid.recordCapacity = 0;
    CheckEq("create rejects zero capacity", TileXRStragglerCreate(
        reinterpret_cast<TileXRCommPtr>(0x1), &invalid, &detector),
        TileXR::TILEXR_ERROR_PARA_CHECK_FAIL);
    CheckTrue("failed create clears detector", detector == nullptr);

    invalid = Config();
    invalid.clockSyncWarmupSamples = TileXR::TILEXR_CLOCK_SYNC_MAX_WARMUP_SAMPLES + 1u;
    CheckEq("create rejects excessive warmup", TileXRStragglerCreate(
        reinterpret_cast<TileXRCommPtr>(0x1), &invalid, &detector),
        TileXR::TILEXR_ERROR_PARA_CHECK_FAIL);

    const TileXRStragglerConfig config = Config();
    g_failMallocCall = 1;
    CheckEq("first allocation failure", TileXRStragglerCreate(
        reinterpret_cast<TileXRCommPtr>(0x1), &config, &detector), TileXR::TILEXR_ERROR_INTERNAL);
    g_mallocCall = 0;
    g_failMallocCall = 2;
    CheckEq("second allocation failure", TileXRStragglerCreate(
        reinterpret_cast<TileXRCommPtr>(0x1), &config, &detector), TileXR::TILEXR_ERROR_INTERNAL);
    CheckEq("partial allocation freed", g_freeCalls, 1);
}

void TestLifecycleAndCrossThreadOwnership()
{
    ResetHooks();
    TileXRStraggler detector = Create(Config());
    CheckTrue("communicator lookup finds detector",
        TileXRDiagnostics::Host::FindStragglerSession(
            reinterpret_cast<TileXRCommPtr>(0x1)) == detector);
    int otherThreadStatus = TileXR::TILEXR_SUCCESS;
    std::thread other([&]() {
        TileXRStragglerOperation operation {};
        operation.componentKind = TILEXR_STRAGGLER_COMPONENT_USER;
        TileXRStragglerObservation observation {};
        otherThreadStatus = TileXRStragglerObservationBegin(
            detector, &operation, reinterpret_cast<aclrtStream>(0x1), &observation);
    });
    other.join();
    CheckEq("cross-thread observation rejected", otherThreadStatus,
            TileXR::TILEXR_ERROR_PARA_CHECK_FAIL);
    CheckEq("destroy detector", TileXRStragglerDestroy(detector),
            TileXR::TILEXR_SUCCESS);
    CheckTrue("destroy clears communicator lookup",
        TileXRDiagnostics::Host::FindStragglerSession(
            reinterpret_cast<TileXRCommPtr>(0x1)) == nullptr);
    CheckEq("both buffers freed", g_freeCalls, 2);
}

void TestCommunicatorOwnershipAndMultiStreamDestroy()
{
    ResetHooks();
    const TileXRStragglerConfig config = Config();
    TileXRStraggler detector = Create(config);
    TileXRStraggler duplicate = reinterpret_cast<TileXRStraggler>(0x1);
    CheckEq("duplicate detector for communicator rejected", TileXRStragglerCreate(
        reinterpret_cast<TileXRCommPtr>(0x1), &config, &duplicate),
        TileXR::TILEXR_ERROR_PARA_CHECK_FAIL);
    CheckTrue("duplicate detector output cleared", duplicate == nullptr);

    Calibrate(detector);
    g_synchronizedStreams.clear();
    void *record = nullptr;
    uint64_t launchId = 0;
    const aclrtStream streamA = reinterpret_cast<aclrtStream>(0x11);
    const aclrtStream streamB = reinterpret_cast<aclrtStream>(0x22);
    ObserveOnStream(detector, streamA, &record, &launchId);
    ObserveOnStream(detector, streamB, &record, &launchId);
    g_syncStatus = 1;
    const int freeCallsBefore = g_freeCalls;
    CheckEq("destroy sync failure keeps detector", TileXRStragglerDestroy(detector),
            TileXR::TILEXR_ERROR_INTERNAL);
    CheckEq("destroy sync failure does not free", g_freeCalls, freeCallsBefore);
    g_syncStatus = ACL_SUCCESS;
    CheckEq("destroy multi-stream detector", TileXRStragglerDestroy(detector),
            TileXR::TILEXR_SUCCESS);
    CheckTrue("destroy synchronizes first producer stream",
        std::find(g_synchronizedStreams.begin(), g_synchronizedStreams.end(), streamA) !=
            g_synchronizedStreams.end());
    CheckTrue("destroy synchronizes second producer stream",
        std::find(g_synchronizedStreams.begin(), g_synchronizedStreams.end(), streamB) !=
            g_synchronizedStreams.end());

    TileXRStraggler replacement = Create(config);
    CheckEq("communicator ownership released after destroy",
            TileXRStragglerDestroy(replacement), TileXR::TILEXR_SUCCESS);
}

void TestDomainPreflightFailures()
{
    ResetHooks();
    g_commArgs.rankSize = 2;
    g_commArgs.peerMems[1] = reinterpret_cast<GM_ADDR>(0x2000);
    TileXRStraggler detector = Create(Config());
    TileXRClockSyncResult clock {};
    g_remoteControlStatus = TileXR::TILEXR_ERROR_NOT_SUPPORT;
    CheckEq("remote preflight status converges", TileXRClockSynchronize(
        detector, reinterpret_cast<aclrtStream>(0x1), &clock), TileXR::TILEXR_ERROR_NOT_SUPPORT);
    CheckEq("failed preflight does not consume magic", g_nextMagicCalls, 0);
    g_remoteControlStatus = TileXR::TILEXR_SUCCESS;
    g_remoteSequenceMismatch = true;
    CheckEq("control sequence mismatch rejected", TileXRClockSynchronize(
        detector, reinterpret_cast<aclrtStream>(0x1), &clock), TileXR::TILEXR_ERROR_INTERNAL);
    CheckEq("mismatched preflight does not consume magic", g_nextMagicCalls, 0);
    g_remoteSequenceMismatch = false;
    CheckEq("destroy preflight detector", TileXRStragglerDestroy(detector),
            TileXR::TILEXR_SUCCESS);
}

void TestStaleCalibrationAndRingCapacity()
{
    ResetHooks();
    TileXRStraggler detector = Create(Config(1));
    Calibrate(detector);
    void *firstRecord = nullptr;
    uint64_t firstId = 0;
    Observe(detector, &firstRecord, &firstId);
    CheckTrue("first observation accepted", firstRecord != nullptr && firstId == 1);

    void *droppedRecord = nullptr;
    uint64_t droppedId = 0;
    Observe(detector, &droppedRecord, &droppedId);
    CheckTrue("full ring drops second observation", droppedRecord == nullptr && droppedId == 0);

    TileXRStragglerReport report {};
    const int oldestStatus = TileXRStragglerCollect(
        detector, reinterpret_cast<aclrtStream>(0x1), &report);
    if (oldestStatus != TileXR::TILEXR_SUCCESS) {
        std::cerr << "oldest report diagnostic complete=" << report.complete
                  << " launch=" << report.launchId
                  << " component=" << report.componentKind
                  << " operation=" << report.operationKind
                  << " generation=" << report.calibrationGeneration
                  << " rank_status=" << report.ranks[0].status
                  << " reasons=" << report.ranks[0].reasonFlags
                  << " start=" << report.ranks[0].localStartCycle
                  << " ready=" << report.ranks[0].localReadyCycle
                  << " end=" << report.ranks[0].localEndCycle << std::endl;
    }
    CheckEq("oldest report survives later capacity error", oldestStatus, TileXR::TILEXR_SUCCESS);
    CheckEq("capacity drop reported", report.droppedRecords, static_cast<uint64_t>(1));
    CheckEq("capacity error returned after queued report", TileXRStragglerCollect(
        detector, reinterpret_cast<aclrtStream>(0x1), &report), TileXR::TILEXR_ERROR_INTERNAL);
    CheckEq("empty ring after capacity error", TileXRStragglerCollect(
        detector, reinterpret_cast<aclrtStream>(0x1), &report), TileXR::TILEXR_ERROR_NOT_FOUND);

    Observe(detector, &firstRecord, &firstId);
    g_nowNs += 2000000ull;
    CheckEq("stale calibration blocks collection", TileXRStragglerCollect(
        detector, reinterpret_cast<aclrtStream>(0x1), &report), TileXR::TILEXR_ERROR_TIMEOUT);
    CheckEq("destroy ring detector", TileXRStragglerDestroy(detector),
            TileXR::TILEXR_SUCCESS);
}

void TestRuntimeFailures()
{
    ResetHooks();
    TileXRStraggler detector = Create(Config());
    TileXRClockSyncResult clock {};
    g_memsetStatus = 1;
    CheckEq("clock memset failure", TileXRClockSynchronize(
        detector, reinterpret_cast<aclrtStream>(0x1), &clock), TileXR::TILEXR_ERROR_INTERNAL);
    g_memsetStatus = ACL_SUCCESS;
    g_clockLaunchStatus = TileXR::TILEXR_ERROR_MKIRT;
    CheckEq("clock launch failure", TileXRClockSynchronize(
        detector, reinterpret_cast<aclrtStream>(0x1), &clock), TileXR::TILEXR_ERROR_MKIRT);
    g_clockLaunchStatus = TileXR::TILEXR_SUCCESS;
    g_syncStatus = 1;
    CheckEq("clock synchronize failure", TileXRClockSynchronize(
        detector, reinterpret_cast<aclrtStream>(0x1), &clock), TileXR::TILEXR_ERROR_INTERNAL);
    auto *session = static_cast<TileXRDiagnostics::Host::StragglerSession *>(detector);
    CheckTrue("failed clock stream retained for retry",
              session->clockStream == reinterpret_cast<aclrtStream>(0x1));
    g_syncStatus = ACL_SUCCESS;
    g_copyStatus = 1;
    CheckEq("clock copy failure", TileXRClockSynchronize(
        detector, reinterpret_cast<aclrtStream>(0x1), &clock), TileXR::TILEXR_ERROR_INTERNAL);
    CheckTrue("recovered clock stream released", session->clockStream == nullptr);
    g_copyStatus = ACL_SUCCESS;
    Calibrate(detector);

    void *record = nullptr;
    uint64_t launchId = 0;
    Observe(detector, &record, &launchId);
    session->recordStatuses[0] = TileXR::TILEXR_ERROR_MKIRT;
    TileXRStragglerReport report {};
    CheckEq("observation failure surfaces at collect", TileXRStragglerCollect(
        detector, reinterpret_cast<aclrtStream>(0x1), &report), TileXR::TILEXR_ERROR_INTERNAL);
    CheckTrue("observation failure report incomplete", report.complete == 0);
    CheckEq("destroy runtime detector", TileXRStragglerDestroy(detector),
            TileXR::TILEXR_SUCCESS);
}

void TestCollectionSynchronizationRetryKeepsRingSlot()
{
    ResetHooks();
    TileXRStraggler detector = Create(Config(1));
    Calibrate(detector);
    void *record = nullptr;
    uint64_t launchId = 0;
    Observe(detector, &record, &launchId);
    auto *session = static_cast<TileXRDiagnostics::Host::StragglerSession *>(detector);
    TileXRStragglerReport report {};

    g_syncStatus = 1;
    CheckEq("collection synchronize failure", TileXRStragglerCollect(
        detector, reinterpret_cast<aclrtStream>(0x1), &report), TileXR::TILEXR_ERROR_INTERNAL);
    CheckEq("failed collection retains launch", session->nextCollectId, static_cast<uint64_t>(1));
    CheckTrue("failed collection retains producer stream", session->recordStreams[0] != nullptr);

    void *droppedRecord = nullptr;
    uint64_t droppedLaunchId = 0;
    Observe(detector, &droppedRecord, &droppedLaunchId);
    CheckTrue("failed collection prevents slot reuse",
        droppedRecord == nullptr && droppedLaunchId == 0);

    g_syncStatus = ACL_SUCCESS;
    CheckEq("collection retry succeeds", TileXRStragglerCollect(
        detector, reinterpret_cast<aclrtStream>(0x1), &report), TileXR::TILEXR_SUCCESS);
    CheckEq("successful retry advances launch", session->nextCollectId, static_cast<uint64_t>(2));
    CheckTrue("successful retry releases producer stream", session->recordStreams[0] == nullptr);
    CheckEq("destroy retry detector", TileXRStragglerDestroy(detector),
        TileXR::TILEXR_SUCCESS);
}

} // namespace

int main()
{
    TestValidationAndAllocationFailures();
    TestLifecycleAndCrossThreadOwnership();
    TestCommunicatorOwnershipAndMultiStreamDestroy();
    TestDomainPreflightFailures();
    TestStaleCalibrationAndRingCapacity();
    TestRuntimeFailures();
    TestCollectionSynchronizationRetryKeepsRingSlot();
    TileXRDiagnostics::Host::SetStragglerRuntimeHooksForTest(nullptr);
    return g_failures == 0 ? 0 : 1;
}
