/*
 * 功能：检查 session 创建/销毁、线程所有权、collective 失败、模型发布、持久化和 cleanup 重试。
 * 调用关系：通过 StragglerRuntimeHooks 为 straggler_session.cpp 注入 Host 内存、采样和故障。
 * 资源与生命周期：测试替身记录 allocation 和释放尝试；不提交真实 device kernel 或 RDMA 请求。
 * Purpose: Check session lifecycle/ownership, collective failures, model publication, persistence and cleanup retries.
 * Calls: Inject Host memory, samples and faults into straggler_session.cpp through StragglerRuntimeHooks.
 * Lifetime: Fakes track allocations and release attempts; no real device kernel or RDMA request is submitted.
 */
#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
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
int g_clockLaunchCalls = 0;
bool g_invalidTrackingSample = false;
bool g_launchedRdma = false;
uint32_t g_launchedWarmupSamples = 0;
uint32_t g_launchedMeasuredSamples = 0;
int g_remoteControlStatus = TileXR::TILEXR_SUCCESS;
uint32_t g_remoteControlStatusOperation = 0;
bool g_remoteDestroyAbort = false;
bool g_remoteSequenceMismatch = false;
bool g_remoteTransportMismatch = false;
bool g_remoteDriftMismatch = false;
uint32_t g_lastControlOperation = 0;
int g_rdmaQueryStatus = TileXR::TILEXR_SUCCESS;
int g_rdmaRegisterStatus = TileXR::TILEXR_SUCCESS;
int g_rdmaUnregisterStatus = TileXR::TILEXR_SUCCESS;
int g_rdmaGetViewStatus = TileXR::TILEXR_SUCCESS;
int g_rdmaShutdownStatus = TileXR::TILEXR_SUCCESS;
int g_rdmaQueryCalls = 0;
int g_rdmaRegisterCalls = 0;
int g_rdmaUnregisterCalls = 0;
int g_rdmaShutdownCalls = 0;
TileXRRDMAInfo g_rdmaInfo {};
TileXRRDMAMemHandle g_rdmaHandle = 0;
void *g_rdmaMemory = nullptr;
size_t g_rdmaBytes = 0;
alignas(64) TileXRRDMADeviceState g_rdmaStates[TILEXR_STRAGGLER_MAX_RANKS] {};
alignas(64) uint8_t g_rdmaQueueMemory[128] {};
std::vector<int> g_rdmaViewPeers;
std::vector<void *> g_allocations;
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

class ScopedClockTransport {
public:
    explicit ScopedClockTransport(const char *value, const char *name = "TILEXR_CLOCK_TRANSPORT")
        : name_(name)
    {
        const char *previous = std::getenv(name_.c_str());
        hadPrevious_ = previous != nullptr;
        if (hadPrevious_) {
            previous_ = previous;
        }
        Set(value);
    }

    ~ScopedClockTransport()
    {
        Set(hadPrevious_ ? previous_.c_str() : nullptr);
    }

private:
    void Set(const char *value)
    {
#ifdef _WIN32
        CheckEq("set clock environment", _putenv_s(name_.c_str(),
            value == nullptr ? "" : value), 0);
#else
        CheckEq("set clock environment", value == nullptr ?
            unsetenv(name_.c_str()) : setenv(name_.c_str(), value, 1), 0);
#endif
    }

    bool hadPrevious_ = false;
    std::string name_;
    std::string previous_;
};

aclError MallocDevice(void **ptr, size_t bytes)
{
    ++g_mallocCall;
    if (g_mallocCall == g_failMallocCall) {
        *ptr = nullptr;
        return 1;
    }
    *ptr = std::calloc(1, bytes);
    if (*ptr != nullptr) {
        g_allocations.push_back(*ptr);
    }
    return *ptr == nullptr ? 1 : ACL_SUCCESS;
}

aclError FreeDevice(void *ptr)
{
    ++g_freeCalls;
    CheckTrue("registered memory must be unregistered before free",
        g_rdmaHandle == 0 || ptr != g_rdmaMemory);
    const auto allocation = std::find(g_allocations.begin(), g_allocations.end(), ptr);
    CheckTrue("free owns allocation", allocation != g_allocations.end());
    if (allocation != g_allocations.end()) {
        g_allocations.erase(allocation);
    }
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
                std::memcpy(&g_lastControlOperation, static_cast<const uint8_t *>(sendBuf) + 36u,
                    sizeof(g_lastControlOperation));
                std::memcpy(static_cast<uint8_t *>(recvBuf) + rank * sendBytes + 40u,
                            &rank, sizeof(rank));
                if (rank != g_commArgs.rank) {
                    const int remoteStatus = g_remoteControlStatusOperation == 0 ||
                        g_remoteControlStatusOperation == g_lastControlOperation ?
                        g_remoteControlStatus : TileXR::TILEXR_SUCCESS;
                    std::memcpy(static_cast<uint8_t *>(recvBuf) + rank * sendBytes + 32u,
                                &remoteStatus, sizeof(remoteStatus));
                    if (g_lastControlOperation == 7u && g_remoteDestroyAbort) {
                        const uint64_t abortRdma = 1;
                        std::memcpy(static_cast<uint8_t *>(recvBuf) + rank * sendBytes + 56u,
                            &abortRdma, sizeof(abortRdma));
                    }
                    if (g_remoteSequenceMismatch) {
                        const uint64_t badSequence = 999;
                        std::memcpy(static_cast<uint8_t *>(recvBuf) + rank * sendBytes + 8u,
                                    &badSequence, sizeof(badSequence));
                    }
                    if (g_remoteTransportMismatch) {
                        uint64_t transport = 0;
                        std::memcpy(&transport, static_cast<const uint8_t *>(sendBuf) + 48u,
                            sizeof(transport));
                        transport ^= 1u; // ControlEnvelope.reserved0 selects MTE/RDMA.
                        std::memcpy(static_cast<uint8_t *>(recvBuf) + rank * sendBytes + 48u,
                            &transport, sizeof(transport));
                    }
                }
            } else if (sendBytes == sizeof(TileXRClockRankResult) && g_lastControlOperation == 3u) {
                auto *clock = static_cast<TileXRClockRankResult *>(recvBuf) + rank;
                clock->rank = rank;
                clock->errorBoundCycles = 7;
                clock->errorBoundNs = 7;
            } else if (sendBytes == sizeof(TileXRClockDriftModel)) {
                auto *model = static_cast<TileXRClockDriftModel *>(recvBuf)+rank;
                model->rank = rank;
                if (g_remoteDriftMismatch) { ++model->requestedWindowUs; }
            } else if (sendBytes == sizeof(TileXR::TileXRStragglerTimingRecord)) {
                std::memcpy(static_cast<uint8_t *>(recvBuf) + rank * sendBytes +
                                offsetof(TileXR::TileXRStragglerTimingRecord, rank),
                            &rank, sizeof(rank));
            }
        }
    }
    return g_allGatherStatus;
}

int LaunchClock(const TileXRDiagnostics::Host::StragglerSession &session, uint32_t warmupSamples,
                uint32_t samples, int64_t, uint64_t, aclrtStream)
{
    ++g_clockLaunchCalls;
    g_launchedRdma = session.clockUsesRdma;
    g_launchedWarmupSamples = warmupSamples;
    g_launchedMeasuredSamples = samples;
    if (g_clockLaunchStatus == TileXR::TILEXR_SUCCESS) {
        if (session.clockUsesRdma) {
            CheckTrue("RDMA prepared before launch", session.clockRdmaReady);
            CheckTrue("RDMA launch owns registered allocation",
                session.clockRdmaMemory == g_rdmaMemory && session.clockRdmaHandle == g_rdmaHandle &&
                g_rdmaHandle != 0);
            for (int peer = 0; peer < session.hostArgs->rankSize; ++peer) {
                if (peer == session.hostArgs->rank || (session.hostArgs->rank != 0 && peer != 0)) {
                    continue;
                }
                TileXRRDMADeviceView staged {};
                std::memcpy(&staged, static_cast<const uint8_t *>(session.clockRdmaMemory) +
                    TileXR::TILEXR_CLOCK_RDMA_VIEWS_OFFSET + peer * sizeof(staged), sizeof(staged));
                CheckEq("staged view references CPU queue state", staged.queue.state,
                    static_cast<uint64_t>(reinterpret_cast<uintptr_t>(&g_rdmaStates[peer])));
                // Model only queue consumption; this fake never executes an RDMA WRITE.
                auto &state = g_rdmaStates[peer];
                const uint64_t sends = static_cast<uint64_t>(warmupSamples + samples) *
                    (session.hostArgs->rank == 0 ? (session.clockRdmaCompact ? 2u : 3u) :
                        (session.clockRdmaCompact ? 1u : 2u));
                state.producer += sends;
                state.completed = state.producer;
                state.cqConsumer += sends;
            }
        }
        auto *output = static_cast<TileXR::TileXRClockSyncDeviceSample *>(session.deviceSamples);
        for (uint32_t index = 0; index < samples; ++index) {
            output[index].t0 = 1000 + index * 100;
            output[index].t1 = output[index].t0;
            output[index].t2 = output[index].t0;
            output[index].t3 = output[index].t0;
            output[index].sequence = index;
            output[index].status = 0;
        }
        if (g_invalidTrackingSample && samples > 17) { output[17].sequence = 999; }
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

int RdmaQuery(void *, TileXRRDMAInfo *info)
{
    ++g_rdmaQueryCalls;
    *info = g_rdmaInfo;
    return g_rdmaQueryStatus;
}

int RdmaRegister(void *, void *ptr, size_t bytes, TileXRRDMAMemHandle *handle)
{
    ++g_rdmaRegisterCalls;
    CheckEq("only one live RDMA registration", g_rdmaHandle, TileXRRDMAMemHandle {0});
    CheckTrue("RDMA registers a session allocation",
        std::find(g_allocations.begin(), g_allocations.end(), ptr) != g_allocations.end());
    CheckEq("RDMA registers full mailbox and staging allocation", bytes,
        static_cast<size_t>(TileXR::TILEXR_CLOCK_RDMA_BYTES));
    *handle = 0;
    if (g_rdmaRegisterStatus == TileXR::TILEXR_SUCCESS) {
        g_rdmaMemory = ptr;
        g_rdmaBytes = bytes;
        g_rdmaHandle = 17;
        *handle = g_rdmaHandle;
    }
    return g_rdmaRegisterStatus;
}

int RdmaUnregister(void *, TileXRRDMAMemHandle handle)
{
    ++g_rdmaUnregisterCalls;
    CheckTrue("unregister uses live handle", handle != 0 && handle == g_rdmaHandle);
    CheckTrue("allocation survives until unregister",
        std::find(g_allocations.begin(), g_allocations.end(), g_rdmaMemory) != g_allocations.end());
    if (g_rdmaUnregisterStatus == TileXR::TILEXR_SUCCESS) {
        g_rdmaHandle = 0;
        g_rdmaMemory = nullptr;
        g_rdmaBytes = 0;
    }
    return g_rdmaUnregisterStatus;
}

int RdmaGetView(void *, TileXRRDMAMemHandle handle, int peer, TileXRRDMADeviceView *view)
{
    g_rdmaViewPeers.push_back(peer);
    CheckTrue("view uses live handle", handle != 0 && handle == g_rdmaHandle);
    CheckTrue("view requested for remote rank", peer >= 0 && peer < g_commArgs.rankSize &&
        peer != g_commArgs.rank);
    if (g_rdmaGetViewStatus != TileXR::TILEXR_SUCCESS) {
        return g_rdmaGetViewStatus;
    }
    *view = TileXRRDMADeviceView {};
    view->version = TILEXR_RDMA_DEVICE_VERSION;
    view->structBytes = sizeof(*view);
    view->localAddress = reinterpret_cast<uintptr_t>(g_rdmaMemory);
    view->remoteAddress = view->localAddress;
    view->localBytes = g_rdmaBytes;
    view->remoteBytes = g_rdmaBytes;
    view->localKey = 1;
    view->remoteKey = 2;
    view->queue.sq = reinterpret_cast<uintptr_t>(g_rdmaQueueMemory);
    view->queue.cq = reinterpret_cast<uintptr_t>(g_rdmaQueueMemory + 64);
    view->queue.state = reinterpret_cast<uintptr_t>(&g_rdmaStates[peer]);
    view->queue.sqDbr = reinterpret_cast<uintptr_t>(g_rdmaQueueMemory);
    view->queue.cqDbr = reinterpret_cast<uintptr_t>(g_rdmaQueueMemory + 64);
    view->queue.doorbell = reinterpret_cast<uintptr_t>(g_rdmaQueueMemory);
    view->queue.doorbellBytes = sizeof(g_rdmaQueueMemory);
    view->queue.sqDepth = 4096;
    view->queue.cqDepth = 4096;
    view->queue.sqEntryBytes = 64;
    view->queue.cqEntryBytes = 64;
    view->queue.dbMode = TILEXR_RDMA_DB_SOFT;
    view->queue.queueAbi = TILEXR_RDMA_QUEUE_ABI_UBNIC_V2;
    view->queue.maxSendSge = 1;
    view->queue.maxSendWr = 4095;
    return TileXR::TILEXR_SUCCESS;
}

int MockShutdown(void *)
{
    ++g_rdmaShutdownCalls;
    CheckEq("RDMA shutdown follows destroy agreement", g_lastControlOperation, 7u);
    CheckTrue("shutdown retains registered allocation until completion",
        g_rdmaHandle == 0 ||
        std::find(g_allocations.begin(), g_allocations.end(), g_rdmaMemory) != g_allocations.end());
    if (g_rdmaShutdownStatus == TileXR::TILEXR_SUCCESS) {
        g_rdmaHandle = 0;
        g_rdmaMemory = nullptr;
        g_rdmaBytes = 0;
    }
    return g_rdmaShutdownStatus;
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
    RdmaQuery,
    RdmaRegister,
    RdmaUnregister,
    RdmaGetView,
    MockShutdown,
};

void ResetHooks()
{
    CheckTrue("previous test freed all device allocations", g_allocations.empty());
    CheckEq("previous test unregistered RDMA", g_rdmaHandle, TileXRRDMAMemHandle {0});
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
    g_clockLaunchCalls = 0;
    g_invalidTrackingSample = false;
    g_launchedRdma = false;
    g_launchedWarmupSamples = 0;
    g_launchedMeasuredSamples = 0;
    g_remoteControlStatus = TileXR::TILEXR_SUCCESS;
    g_remoteControlStatusOperation = 0;
    g_remoteDestroyAbort = false;
    g_remoteSequenceMismatch = false;
    g_remoteTransportMismatch = false;
    g_remoteDriftMismatch = false;
    g_lastControlOperation = 0;
    g_rdmaQueryStatus = TileXR::TILEXR_SUCCESS;
    g_rdmaRegisterStatus = TileXR::TILEXR_SUCCESS;
    g_rdmaUnregisterStatus = TileXR::TILEXR_SUCCESS;
    g_rdmaGetViewStatus = TileXR::TILEXR_SUCCESS;
    g_rdmaShutdownStatus = TileXR::TILEXR_SUCCESS;
    g_rdmaQueryCalls = 0;
    g_rdmaRegisterCalls = 0;
    g_rdmaUnregisterCalls = 0;
    g_rdmaShutdownCalls = 0;
    g_rdmaViewPeers.clear();
    g_rdmaInfo = TileXRRDMAInfo {};
    g_rdmaInfo.version = TILEXR_RDMA_DEVICE_VERSION;
    g_rdmaInfo.connected = 1;
    g_rdmaInfo.deviceDirect = 1;
    g_rdmaInfo.peerCount = 1;
    std::fill(g_rdmaStates, g_rdmaStates + TILEXR_STRAGGLER_MAX_RANKS, TileXRRDMADeviceState {});
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

TileXRClockSyncResult Calibrate(TileXRStraggler detector)
{
    TileXRClockSyncResult result {};
    CheckEq("clock synchronize", TileXRClockSynchronize(
        detector, reinterpret_cast<aclrtStream>(0x1), &result), TileXR::TILEXR_SUCCESS);
    CheckEq("clock rank size", result.rankSize, static_cast<uint32_t>(g_commArgs.rankSize));
    for (int rank = 0; rank < g_commArgs.rankSize; ++rank) {
        CheckEq("gathered clock rank", result.ranks[rank].rank, rank);
        CheckEq("gathered clock status", result.ranks[rank].status, TileXR::TILEXR_SUCCESS);
        CheckEq("gathered valid clock samples", result.ranks[rank].validSamples, 128u);
    }
    CheckEq("clock frequency", result.cyclesPerSecond, static_cast<uint64_t>(1000000000ull));
    CheckEq("public clock precision", result.precisionNs, static_cast<uint64_t>(7));
    CheckEq("public warmup sample count", result.warmupSampleCount, 32u);
    CheckEq("public measured sample count", result.sampleCount, 128u);
    CheckEq("launched warmup samples", g_launchedWarmupSamples, 32u);
    CheckEq("launched measured samples", g_launchedMeasuredSamples, 128u);
    return result;
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

void ConfigureTwoRanks(int rank = 0)
{
    g_commArgs.rank = rank;
    g_commArgs.rankSize = 2;
    g_commArgs.peerMems[0] = nullptr;
    g_commArgs.peerMems[1] = nullptr;
}

void TestDefaultClockTransport()
{
    for (const char *transport : {static_cast<const char *>(nullptr), "mte"}) {
        ScopedClockTransport environment(transport);
        ResetHooks();
        ConfigureTwoRanks();
        g_commArgs.peerMems[0] = reinterpret_cast<GM_ADDR>(0x1000);
        g_commArgs.peerMems[1] = reinterpret_cast<GM_ADDR>(0x2000);
        // Unsupported RDMA must have no effect on the default or explicit MTE path.
        g_rdmaInfo.deviceDirect = 0;
        TileXRStraggler detector = Create(Config());
        Calibrate(detector);
        CheckEq("MTE launches once", g_clockLaunchCalls, 1);
        CheckTrue("MTE launch selected", !g_launchedRdma);
        CheckEq("MTE never queries RDMA", g_rdmaQueryCalls, 0);
        CheckEq("MTE never registers RDMA", g_rdmaRegisterCalls, 0);
        CheckEq("MTE allocates only existing buffers", g_mallocCall, 2);

        g_commArgs.peerMems[1] = nullptr;
        TileXRClockSyncResult clock {};
        CheckEq("MTE still requires peer mappings", TileXRClockSynchronize(
            detector, reinterpret_cast<aclrtStream>(0x1), &clock), TileXR::TILEXR_ERROR_NOT_SUPPORT);
        CheckEq("missing MTE mapping prevents another launch", g_clockLaunchCalls, 1);
        CheckEq("destroy MTE transport detector", TileXRStragglerDestroy(detector),
            TileXR::TILEXR_SUCCESS);
        CheckEq("MTE never unregisters RDMA", g_rdmaUnregisterCalls, 0);
        CheckEq("MTE frees existing buffers", g_freeCalls, 2);
    }
}

void TestRdmaWithoutPeerMemoryAndRegistrationReuse()
{
    ScopedClockTransport environment("rdma");
    for (int rank : {0, 1}) {
        ResetHooks();
        ConfigureTwoRanks(rank);
        TileXRStraggler detector = Create(Config());
        auto *session = static_cast<TileXRDiagnostics::Host::StragglerSession *>(detector);
        CheckTrue("RDMA selected at create", session->clockUsesRdma);
        CheckEq("RDMA registration is lazy", g_rdmaRegisterCalls, 0);
        const TileXRClockSyncResult first = Calibrate(detector);
        void *memory = session->clockRdmaMemory;
        const TileXRRDMAMemHandle handle = session->clockRdmaHandle;
        const TileXRClockSyncResult second = Calibrate(detector);
        CheckEq("RDMA generation advances across rounds", second.generation, first.generation + 1u);
        CheckTrue("RDMA works without either peer mapping",
            g_commArgs.peerMems[0] == nullptr && g_commArgs.peerMems[1] == nullptr && g_launchedRdma);
        CheckEq("RDMA launches both rounds", g_clockLaunchCalls, 2);
        CheckEq("RDMA allocates mailbox only once", g_mallocCall, 3);
        CheckEq("RDMA registers once across rounds", g_rdmaRegisterCalls, 1);
        CheckEq("RDMA obtains one remote view", g_rdmaViewPeers.size(), size_t {1});
        CheckTrue("RDMA obtains correct peer view", g_rdmaViewPeers == std::vector<int> {1 - rank});
        CheckTrue("RDMA preserves allocation and handle across rounds",
            memory != nullptr && memory == session->clockRdmaMemory &&
            handle != 0 && handle == session->clockRdmaHandle);
        const uint64_t sendsPerRound = 160u * (rank == 0 ? 3u : 2u);
        CheckEq("RDMA retains transport-owned queue counters", g_rdmaStates[1 - rank].producer,
            2u * sendsPerRound);
        CheckEq("destroy RDMA transport detector", TileXRStragglerDestroy(detector),
            TileXR::TILEXR_SUCCESS);
        CheckEq("RDMA unregisters once", g_rdmaUnregisterCalls, 1);
        CheckEq("RDMA frees mailbox and existing buffers", g_freeCalls, 3);
        CheckTrue("RDMA destroy releases all allocations", g_allocations.empty());
    }
}

void TestRdmaUnsupportedAndTransportMismatch()
{
    ScopedClockTransport environment("rdma");
    ResetHooks();
    ConfigureTwoRanks();
    TileXRStraggler detector = Create(Config());
    TileXRClockSyncResult clock {};
    g_rdmaInfo.hostDriven = 1;
    g_rdmaInfo.deviceDirect = 0;
    CheckEq("RDMA rejects host-driven-only capability", TileXRClockSynchronize(
        detector, reinterpret_cast<aclrtStream>(0x1), &clock), TileXR::TILEXR_ERROR_NOT_SUPPORT);
    CheckEq("unsupported RDMA does not launch", g_clockLaunchCalls, 0);
    CheckEq("unsupported RDMA does not allocate mailbox", g_mallocCall, 2);
    CheckEq("unsupported RDMA does not register", g_rdmaRegisterCalls, 0);
    CheckEq("unsupported RDMA does not consume magic", g_nextMagicCalls, 0);
    g_rdmaInfo.deviceDirect = 1;
    g_remoteTransportMismatch = true;
    CheckEq("mixed MTE and RDMA control envelopes rejected", TileXRClockSynchronize(
        detector, reinterpret_cast<aclrtStream>(0x1), &clock), TileXR::TILEXR_ERROR_INTERNAL);
    CheckEq("transport mismatch does not launch", g_clockLaunchCalls, 0);
    CheckEq("transport mismatch does not register", g_rdmaRegisterCalls, 0);
    CheckEq("transport mismatch does not consume magic", g_nextMagicCalls, 0);
    g_remoteTransportMismatch = false;
    CheckEq("destroy unsupported RDMA detector", TileXRStragglerDestroy(detector),
        TileXR::TILEXR_SUCCESS);
}

void TestRdmaReadyQueuePreflight()
{
    ScopedClockTransport environment("rdma");
    // Exercise an already prepared session: cached views must not bypass the
    // per-round queue check. Root sends three slots/sample; a peer sends two.
    for (int rank : {0, 1}) {
        for (bool exhaustSq : {true, false}) {
            ResetHooks();
            ConfigureTwoRanks(rank);
            TileXRStraggler detector = Create(Config());
            Calibrate(detector);
            auto *session = static_cast<TileXRDiagnostics::Host::StragglerSession *>(detector);
            CheckTrue("queue preflight starts from prepared RDMA", session->clockRdmaReady);
            const auto &view = session->clockRdmaViews[1 - rank];
            auto &state = g_rdmaStates[1 - rank];
            const uint64_t sends = 160u * (rank == 0 ? 3u : 2u);
            if (exhaustSq) {
                // Exactly enough SQ entries for WRs still lacks the stop entry.
                state.producer = view.queue.sqDepth - sends;
                state.completed = state.producer;
            } else {
                state.cqConsumer = view.queue.cqDepth - sends;
            }
            const TileXRRDMADeviceState before = state;
            const int magicCalls = g_nextMagicCalls;
            TileXRClockSyncResult clock {};
            CheckEq(exhaustSq ? "ready SQ rejects no-wrap boundary" : "ready CQ rejects no-wrap boundary",
                TileXRClockSynchronize(detector, reinterpret_cast<aclrtStream>(0x1), &clock),
                TileXR::TILEXR_ERROR_NOT_SUPPORT);
            CheckEq("exhausted queue does not launch", g_clockLaunchCalls, 1);
            CheckEq("exhausted queue does not consume magic", g_nextMagicCalls, magicCalls);
            CheckEq("exhausted queue does not register again", g_rdmaRegisterCalls, 1);
            CheckTrue("preflight preserves queue counters", std::memcmp(&state, &before, sizeof(state)) == 0);
            CheckEq("failed round leaves generation unchanged", session->generation, 1u);
            CheckEq("failed round leaves result uninitialized", clock.version, 0u);
            CheckEq("destroy queue preflight detector", TileXRStragglerDestroy(detector),
                TileXR::TILEXR_SUCCESS);
        }
    }
}

void TestRdmaDeferredWholeRoundAdmission()
{
    ScopedClockTransport transport("rdma");
    ScopedClockTransport deferred("1", "TILEXR_CLOCK_RDMA_DEFER_CQE");
    for (bool compact : {false, true}) {
        ScopedClockTransport compactOption(compact ? "1" : "0", "TILEXR_CLOCK_RDMA_COMPACT");
        for (int rank : {0, 1}) {
            for (bool exactCredit : {false, true}) {
                ResetHooks();
                ConfigureTwoRanks(rank);
                TileXRStraggler detector = Create(Config());
                Calibrate(detector);
                auto *session = static_cast<TileXRDiagnostics::Host::StragglerSession *>(detector);
                CheckTrue("deferred option selected", session->clockRdmaDeferred);
                CheckEq("compact option selected", session->clockRdmaCompact, compact);
                auto &queue = session->clockRdmaViews[1 - rank].queue;
                const uint64_t sends = 160u * (rank == 0 ? (compact ? 2u : 3u) : (compact ? 1u : 2u));
                queue.maxSendWr = static_cast<uint32_t>(sends - (exactCredit ? 0u : 1u));
                const TileXRRDMADeviceState before = g_rdmaStates[1 - rank];
                const int magicCalls = g_nextMagicCalls;
                if (exactCredit) {
                    Calibrate(detector); // Equality is valid: each round drains before source reuse.
                    CheckEq("exact WR credits permit launch", g_clockLaunchCalls, 2);
                    CheckEq("deferred round consumes expected WRs", g_rdmaStates[1 - rank].producer,
                        before.producer + sends);
                } else {
                    TileXRClockSyncResult result {};
                    CheckEq("whole deferred round exceeds WR credit", TileXRClockSynchronize(
                        detector, reinterpret_cast<aclrtStream>(0x1), &result), TileXR::TILEXR_ERROR_NOT_SUPPORT);
                    CheckEq("WR admission failure does not launch", g_clockLaunchCalls, 1);
                    CheckEq("WR admission failure does not consume magic", g_nextMagicCalls, magicCalls);
                    CheckTrue("WR admission preserves counters", std::memcmp(
                        &before, &g_rdmaStates[1 - rank], sizeof(before)) == 0);
                    CheckEq("WR admission preserves generation", session->generation, 1u);
                    CheckEq("WR admission leaves result uninitialized", result.version, 0u);
                }
                CheckEq("destroy deferred admission detector", TileXRStragglerDestroy(detector),
                    TileXR::TILEXR_SUCCESS);
            }
        }
    }
}

void TestRdmaCompactRequiresDeferred()
{
    ScopedClockTransport transport("rdma");
    ScopedClockTransport deferred("0", "TILEXR_CLOCK_RDMA_DEFER_CQE");
    ScopedClockTransport compact("1", "TILEXR_CLOCK_RDMA_COMPACT");
    ResetHooks();
    const auto config = Config();
    TileXRStraggler detector = nullptr;
    CheckEq("compact mode requires deferred CQ", TileXRStragglerCreate(
        reinterpret_cast<TileXRCommPtr>(0x1), &config, &detector), TileXR::TILEXR_ERROR_PARA_CHECK_FAIL);
    CheckTrue("invalid compact mode creates no detector", detector == nullptr);
    CheckEq("invalid compact mode allocates nothing", g_mallocCall, 0);
}

void TestRdmaUnregisterRetryRetainsOwnership()
{
    ScopedClockTransport environment("rdma");
    ResetHooks();
    ConfigureTwoRanks();
    TileXRStraggler detector = Create(Config());
    Calibrate(detector);
    auto *session = static_cast<TileXRDiagnostics::Host::StragglerSession *>(detector);
    void *memory = session->clockRdmaMemory;
    const TileXRRDMAMemHandle handle = session->clockRdmaHandle;
    const size_t liveAllocations = g_allocations.size();
    g_rdmaUnregisterStatus = TileXR::TILEXR_ERROR_INTERNAL;
    CheckEq("RDMA unregister failure returned", TileXRStragglerDestroy(detector),
        TileXR::TILEXR_ERROR_INTERNAL);
    CheckEq("unregister failure frees nothing", g_freeCalls, 0);
    CheckEq("unregister failure retains all allocations", g_allocations.size(), liveAllocations);
    CheckTrue("unregister failure retains mailbox and handle",
        session->clockRdmaMemory == memory && session->clockRdmaHandle == handle &&
        g_rdmaHandle == handle && handle != 0);
    CheckTrue("unregister failure retains communicator ownership",
        TileXRDiagnostics::Host::FindStragglerSession(reinterpret_cast<TileXRCommPtr>(0x1)) == session);
    g_rdmaUnregisterStatus = TileXR::TILEXR_SUCCESS;
    CheckEq("RDMA unregister retry succeeds", TileXRStragglerDestroy(detector), TileXR::TILEXR_SUCCESS);
    CheckEq("RDMA unregister retried once", g_rdmaUnregisterCalls, 2);
    CheckEq("successful retry frees all allocations", g_freeCalls, 3);
    CheckTrue("successful retry releases communicator ownership",
        TileXRDiagnostics::Host::FindStragglerSession(reinterpret_cast<TileXRCommPtr>(0x1)) == nullptr);
    CheckTrue("successful retry leaves no allocations", g_allocations.empty());
}

void CheckRdmaOwnershipRetained(const TileXRDiagnostics::Host::StragglerSession *session,
                                void *memory, TileXRRDMAMemHandle handle,
                                const std::vector<void *> &allocations)
{
    CheckEq("failed destroy frees nothing", g_freeCalls, 0);
    CheckTrue("failed destroy retains every allocation", g_allocations == allocations);
    CheckTrue("failed destroy retains mailbox and live registration",
        handle != 0 && session->clockRdmaMemory == memory && session->clockRdmaHandle == handle &&
        g_rdmaHandle == handle && g_rdmaMemory == memory);
    CheckTrue("failed destroy retains communicator ownership",
        TileXRDiagnostics::Host::FindStragglerSession(session->comm) == session);
}

void TestRdmaDestroyQuiescenceRetry()
{
    ScopedClockTransport environment("rdma");
    for (int rank : {0, 1}) {
        for (bool remoteFailure : {false, true}) {
            ResetHooks();
            ConfigureTwoRanks(rank);
            TileXRStraggler detector = Create(Config());
            Calibrate(detector);
            auto *session = static_cast<TileXRDiagnostics::Host::StragglerSession *>(detector);
            CheckTrue("prepared RDMA participates in collective destroy", session->clockRdmaParticipating);
            void *record = nullptr;
            uint64_t launchId = 0;
            const aclrtStream producerStream = reinterpret_cast<aclrtStream>(0x22);
            ObserveOnStream(detector, producerStream, &record, &launchId);
            CheckTrue("destroy test has a producer to quiesce", record != nullptr && launchId == 1);
            void *memory = session->clockRdmaMemory;
            const TileXRRDMAMemHandle handle = session->clockRdmaHandle;
            const auto allocations = g_allocations;
            g_synchronizedStreams.clear();
            if (remoteFailure) {
                g_remoteControlStatusOperation = 7u; // CLOCK_DESTROY only.
                g_remoteControlStatus = TileXR::TILEXR_ERROR_INTERNAL;
            } else {
                g_syncStatus = 1;
            }
            CheckEq(remoteFailure ? "remote destroy quiescence failure converges" :
                    "local destroy quiescence failure converges",
                TileXRStragglerDestroy(detector), TileXR::TILEXR_ERROR_INTERNAL);
            CheckEq("quiescence failure still gathers destroy status", g_lastControlOperation, 7u);
            CheckTrue("destroy attempts producer synchronization",
                std::find(g_synchronizedStreams.begin(), g_synchronizedStreams.end(), producerStream) !=
                    g_synchronizedStreams.end());
            CheckEq("quiescence failure does not unregister", g_rdmaUnregisterCalls, 0);
            CheckEq("quiescence failure does not shutdown", g_rdmaShutdownCalls, 0);
            CheckRdmaOwnershipRetained(session, memory, handle, allocations);
            CheckTrue("quiescence failure preserves producer tracking",
                session->recordStreams[0] == producerStream &&
                std::find(session->outstandingStreams.begin(), session->outstandingStreams.end(),
                    producerStream) != session->outstandingStreams.end());
            g_syncStatus = ACL_SUCCESS;
            g_remoteControlStatus = TileXR::TILEXR_SUCCESS;
            CheckEq("quiescent RDMA destroy retry succeeds", TileXRStragglerDestroy(detector),
                TileXR::TILEXR_SUCCESS);
            CheckEq("quiescent retry unregisters exactly once", g_rdmaUnregisterCalls, 1);
            CheckEq("quiescent retry does not shutdown healthy RDMA", g_rdmaShutdownCalls, 0);
            CheckEq("quiescent retry frees every buffer", g_freeCalls, 3);
            CheckTrue("quiescent retry releases all allocations", g_allocations.empty());
            CheckTrue("quiescent retry releases communicator ownership",
                TileXRDiagnostics::Host::FindStragglerSession(reinterpret_cast<TileXRCommPtr>(0x1)) == nullptr);
        }
    }
}

void TestRdmaClockResultFailureShutdownRetry()
{
    ScopedClockTransport environment("rdma");
    for (int rank : {0, 1}) {
        for (bool remoteFailure : {false, true}) {
            ResetHooks();
            ConfigureTwoRanks(rank);
            TileXRStraggler detector = Create(Config());
            Calibrate(detector);
            auto *session = static_cast<TileXRDiagnostics::Host::StragglerSession *>(detector);
            void *memory = session->clockRdmaMemory;
            const TileXRRDMAMemHandle handle = session->clockRdmaHandle;
            const auto allocations = g_allocations;
            if (remoteFailure) {
                g_remoteControlStatusOperation = 3u; // CLOCK_RESULT only; setup and launch succeed.
                g_remoteControlStatus = TileXR::TILEXR_ERROR_MKIRT;
            } else {
                g_clockLaunchStatus = TileXR::TILEXR_ERROR_MKIRT;
            }
            TileXRClockSyncResult clock {};
            CheckEq(remoteFailure ? "remote clock result failure converges" : "local clock launch failure converges",
                TileXRClockSynchronize(detector, reinterpret_cast<aclrtStream>(0x1), &clock),
                TileXR::TILEXR_ERROR_MKIRT);
            CheckEq("failure reached CLOCK_RESULT", g_lastControlOperation, 3u);
            CheckEq("clock failure occurs after second launch", g_clockLaunchCalls, 2);
            CheckTrue("clock result failure marks RDMA failed on this rank", session->clockRdmaFailed);
            CheckEq("clock result failure does not publish a new generation", session->generation, 1u);
            CheckEq("clock result failure leaves output uninitialized", clock.version, 0u);
            g_clockLaunchStatus = TileXR::TILEXR_SUCCESS;
            g_remoteControlStatus = TileXR::TILEXR_SUCCESS;
            g_rdmaShutdownStatus = TileXR::TILEXR_ERROR_INTERNAL;
            CheckEq("faulted clock shutdown failure returned", TileXRStragglerDestroy(detector),
                TileXR::TILEXR_ERROR_INTERNAL);
            CheckEq("faulted clock attempts shutdown", g_rdmaShutdownCalls, 1);
            CheckEq("faulted clock bypasses unregister", g_rdmaUnregisterCalls, 0);
            CheckTrue("failed shutdown preserves RDMA failure", session->clockRdmaFailed);
            CheckRdmaOwnershipRetained(session, memory, handle, allocations);
            g_rdmaShutdownStatus = TileXR::TILEXR_SUCCESS;
            CheckEq("faulted clock shutdown retry succeeds", TileXRStragglerDestroy(detector),
                TileXR::TILEXR_SUCCESS);
            CheckEq("faulted clock retries shutdown once", g_rdmaShutdownCalls, 2);
            CheckEq("shutdown invalidates handle without unregister", g_rdmaUnregisterCalls, 0);
            CheckEq("successful shutdown retry frees every buffer", g_freeCalls, 3);
            CheckEq("successful shutdown clears registration", g_rdmaHandle, TileXRRDMAMemHandle {0});
            CheckTrue("successful shutdown releases all allocations", g_allocations.empty());
            CheckTrue("successful shutdown releases communicator ownership",
                TileXRDiagnostics::Host::FindStragglerSession(reinterpret_cast<TileXRCommPtr>(0x1)) == nullptr);
        }
    }
}

void TestRdmaDestroyRemoteAbortUsesShutdown()
{
    ScopedClockTransport environment("rdma");
    for (int rank : {0, 1}) {
        ResetHooks();
        ConfigureTwoRanks(rank);
        TileXRStraggler detector = Create(Config());
        Calibrate(detector);
        auto *session = static_cast<TileXRDiagnostics::Host::StragglerSession *>(detector);
        CheckTrue("local clock is healthy before remote destroy abort", !session->clockRdmaFailed);
        void *memory = session->clockRdmaMemory;
        const TileXRRDMAMemHandle handle = session->clockRdmaHandle;
        const auto allocations = g_allocations;
        g_remoteDestroyAbort = true; // ControlEnvelope.reserved1 on the other rank.
        g_rdmaShutdownStatus = TileXR::TILEXR_ERROR_INTERNAL;
        CheckEq("remote abort selects collective shutdown", TileXRStragglerDestroy(detector),
            TileXR::TILEXR_ERROR_INTERNAL);
        CheckTrue("remote destroy abort marks local RDMA failed", session->clockRdmaFailed);
        CheckEq("remote abort attempts shutdown", g_rdmaShutdownCalls, 1);
        CheckEq("remote abort prevents ordinary unregister", g_rdmaUnregisterCalls, 0);
        CheckRdmaOwnershipRetained(session, memory, handle, allocations);
        g_remoteDestroyAbort = false;
        g_rdmaShutdownStatus = TileXR::TILEXR_SUCCESS;
        CheckEq("remote abort shutdown retry succeeds", TileXRStragglerDestroy(detector),
            TileXR::TILEXR_SUCCESS);
        CheckEq("remote abort retries shutdown once", g_rdmaShutdownCalls, 2);
        CheckEq("remote abort retry never unregisters", g_rdmaUnregisterCalls, 0);
        CheckEq("remote abort retry frees every buffer", g_freeCalls, 3);
        CheckTrue("remote abort retry releases all allocations", g_allocations.empty());
    }
}

void TestRdmaPreparationFailureCleanup()
{
    ScopedClockTransport environment("rdma");
    enum class Failure { ALLOCATION, INITIALIZE, REGISTER, VIEW, COPY_VIEWS };
    for (Failure failure : {Failure::ALLOCATION, Failure::INITIALIZE, Failure::REGISTER,
                           Failure::VIEW, Failure::COPY_VIEWS}) {
        ResetHooks();
        ConfigureTwoRanks();
        TileXRStraggler detector = Create(Config());
        int expectedRegisters = 0;
        int expectedViews = 0;
        int expectedUnregisters = 0;
        const char *label = "";
        switch (failure) {
            case Failure::ALLOCATION:
                label = "RDMA mailbox allocation failure";
                g_failMallocCall = 3;
                break;
            case Failure::INITIALIZE:
                label = "RDMA mailbox initialization failure";
                g_memsetStatus = 1;
                break;
            case Failure::REGISTER:
                label = "RDMA registration failure";
                g_rdmaRegisterStatus = TileXR::TILEXR_ERROR_INTERNAL;
                expectedRegisters = 1;
                break;
            case Failure::VIEW:
                label = "RDMA device view failure";
                g_rdmaGetViewStatus = TileXR::TILEXR_ERROR_INTERNAL;
                expectedRegisters = expectedViews = expectedUnregisters = 1;
                break;
            case Failure::COPY_VIEWS:
                label = "RDMA device view staging failure";
                g_copyStatus = 1;
                expectedRegisters = expectedViews = expectedUnregisters = 1;
                break;
        }
        TileXRClockSyncResult clock {};
        CheckEq(label, TileXRClockSynchronize(detector, reinterpret_cast<aclrtStream>(0x1), &clock),
            TileXR::TILEXR_ERROR_INTERNAL);
        auto *session = static_cast<TileXRDiagnostics::Host::StragglerSession *>(detector);
        CheckTrue("failed preparation is not ready", !session->clockRdmaReady);
        CheckTrue("recoverable preparation failure does not poison RDMA", !session->clockRdmaFailed);
        CheckEq("failed preparation does not launch", g_clockLaunchCalls, 0);
        CheckEq("failed preparation does not consume magic", g_nextMagicCalls, 0);
        CheckEq("failed preparation registration count", g_rdmaRegisterCalls, expectedRegisters);
        CheckEq("failed preparation view count", g_rdmaViewPeers.size(), static_cast<size_t>(expectedViews));
        g_failMallocCall = 0;
        g_memsetStatus = ACL_SUCCESS;
        g_rdmaRegisterStatus = TileXR::TILEXR_SUCCESS;
        g_rdmaGetViewStatus = TileXR::TILEXR_SUCCESS;
        g_copyStatus = ACL_SUCCESS;
        CheckEq("failed preparation requires destroy before another round", TileXRClockSynchronize(
            detector, reinterpret_cast<aclrtStream>(0x1), &clock), TileXR::TILEXR_ERROR_NOT_SUPPORT);
        CheckEq("rejected setup retry does not register again", g_rdmaRegisterCalls, expectedRegisters);
        CheckEq("rejected setup retry does not allocate again", g_mallocCall, 3);
        CheckEq("rejected setup retry does not launch", g_clockLaunchCalls, 0);
        CheckEq("destroy after RDMA preparation failure", TileXRStragglerDestroy(detector),
            TileXR::TILEXR_SUCCESS);
        CheckEq("failed preparation unregisters only a live registration",
            g_rdmaUnregisterCalls, expectedUnregisters);
        CheckEq("failed preparation frees all successful allocations", g_freeCalls,
            failure == Failure::ALLOCATION ? 2 : 3);
        CheckTrue("failed preparation leaves no allocations", g_allocations.empty());
        CheckEq("failed preparation leaves no registration", g_rdmaHandle, TileXRRDMAMemHandle {0});
        CheckTrue("failed preparation releases communicator ownership",
            TileXRDiagnostics::Host::FindStragglerSession(reinterpret_cast<TileXRCommPtr>(0x1)) == nullptr);
    }
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

void TestDriftConfigurationAndCollection()
{
    ResetHooks();
    {
        ScopedClockTransport invalid("20us","TILEXR_CLOCK_DRIFT_WINDOW_US");
        auto config = Config();
        TileXRStraggler detector = nullptr;
        CheckEq("invalid drift window rejected",TileXRStragglerCreate(
            reinterpret_cast<TileXRCommPtr>(0x41),&config,&detector),TileXR::TILEXR_ERROR_PARA_CHECK_FAIL);
    }
    ScopedClockTransport enabled("1","TILEXR_CLOCK_DRIFT_WINDOW_US");
    g_commArgs.rankSize = 2;
    g_commArgs.peerMems[1] = reinterpret_cast<GM_ADDR>(0x2000);
    auto detector = Create(Config());
    TileXRClockDriftModel model {};
    CheckEq("get collecting model",TileXRClockGetDriftModel(detector,1,&model),TileXR::TILEXR_SUCCESS);
    CheckEq("model initially collecting",model.state,uint32_t {TILEXR_CLOCK_DRIFT_COLLECTING});
    Calibrate(detector);
    CheckEq("get gathered model",TileXRClockGetDriftModel(detector,1,&model),TileXR::TILEXR_SUCCESS);
    CheckEq("gathered rank identity",model.rank,1);
    CheckEq("mock zero drift collected",model.state,uint32_t {TILEXR_CLOCK_DRIFT_READY});
    CheckEq("window retained",model.requestedWindowUs,uint64_t {1});
    const auto generation = model.trainedAtGeneration;
    const auto *session = static_cast<TileXRDiagnostics::Host::StragglerSession *>(detector);
    const auto frozenHostNs = session->calibrationHostNs;
    g_nowNs += 10000;
    Calibrate(detector);
    CheckEq("get frozen model",TileXRClockGetDriftModel(detector,1,&model),TileXR::TILEXR_SUCCESS);
    CheckEq("successive calls never refit",model.trainedAtGeneration,generation);
    CheckEq("new sampler rounds do not refresh frozen model age",session->calibrationHostNs,frozenHostNs);
    CheckEq("out of range rank rejected",TileXRClockGetDriftModel(detector,2,&model),
        TileXR::TILEXR_ERROR_PARA_CHECK_FAIL);
    CheckEq("destroy drift detector",TileXRStragglerDestroy(detector),TileXR::TILEXR_SUCCESS);
}

void TestFailedDriftGatherDoesNotPublishPartialModel()
{
    ResetHooks();
    ScopedClockTransport enabled("1","TILEXR_CLOCK_DRIFT_WINDOW_US");
    g_commArgs.rankSize = 2;
    g_commArgs.peerMems[1] = reinterpret_cast<GM_ADDR>(0x2000);
    auto detector = Create(Config());
    g_remoteDriftMismatch = true;
    TileXRClockSyncResult result {};
    CheckEq("bad gathered model rejected",TileXRClockSynchronize(
        detector,reinterpret_cast<aclrtStream>(0x1),&result),TileXR::TILEXR_ERROR_INTERNAL);
    TileXRClockDriftModel model {};
    CheckEq("old model still accessible",TileXRClockGetDriftModel(detector,1,&model),TileXR::TILEXR_SUCCESS);
    CheckEq("failure cannot publish uncommitted model",model.state,uint32_t {TILEXR_CLOCK_DRIFT_COLLECTING});
    CheckEq("failed synchronization leaves result uncommitted",result.version,uint32_t {0});
    g_remoteDriftMismatch = false;
    Calibrate(detector);
    CheckEq("retry model accessible",TileXRClockGetDriftModel(detector,1,&model),TileXR::TILEXR_SUCCESS);
    CheckEq("successful retry publishes full model",model.state,uint32_t {TILEXR_CLOCK_DRIFT_READY});
    CheckEq("destroy drift failure detector",TileXRStragglerDestroy(detector),TileXR::TILEXR_SUCCESS);
}

void TestTrackingConfigurationAndPersistence()
{
    ResetHooks();
    ScopedClockTransport tracking("1","TILEXR_CLOCK_DRIFT_TRACK");
    auto config = Config();
    TileXRStraggler detector = nullptr;
    CheckEq("tracking requires output path",TileXRStragglerCreate(
        reinterpret_cast<TileXRCommPtr>(0x41),&config,&detector),TileXR::TILEXR_ERROR_PARA_CHECK_FAIL);
    config.outputDir = "clock-tracking-cpu-output";
    {
        ScopedClockTransport invalid("20000000","TILEXR_CLOCK_DRIFT_WINDOW_US");
        CheckEq("tracking requires approved ten-second window",TileXRStragglerCreate(
            reinterpret_cast<TileXRCommPtr>(0x41),&config,&detector),TileXR::TILEXR_ERROR_PARA_CHECK_FAIL);
    }
    g_commArgs.rankSize = 2;
    g_commArgs.peerMems[1] = reinterpret_cast<GM_ADDR>(0x2000);
    detector = Create(config);
    auto *session = static_cast<TileXRDiagnostics::Host::StragglerSession *>(detector);
    CheckTrue("tracking enabled",session->clockDrift.tracking);
    CheckEq("tracking initial window",session->clockDrift.windowUs,uint64_t {10000000});
    Calibrate(detector);
    CheckEq("persistence converges collectively",g_lastControlOperation,uint32_t {9});
    g_remoteControlStatusOperation = 9;
    g_remoteControlStatus = TileXR::TILEXR_ERROR_INTERNAL;
    TileXRClockSyncResult result {};
    CheckEq("remote persistence failure visible on all ranks",TileXRClockSynchronize(
        detector,reinterpret_cast<aclrtStream>(0x1),&result),TileXR::TILEXR_ERROR_INTERNAL);
    g_remoteControlStatus = TileXR::TILEXR_SUCCESS;
    CheckEq("destroy tracking detector",TileXRStragglerDestroy(detector),TileXR::TILEXR_SUCCESS);

    ResetHooks();
    g_commArgs.rank = 1;
    g_commArgs.rankSize = 2;
    g_commArgs.peerMems[0] = reinterpret_cast<GM_ADDR>(0x2000);
    g_commArgs.peerMems[1] = reinterpret_cast<GM_ADDR>(0x3000);
    detector = Create(config);
    session = static_cast<TileXRDiagnostics::Host::StragglerSession *>(detector);
    Calibrate(detector);
    session->clockDrift.closed = true;
    session->clockDrift.model.state = TILEXR_CLOCK_DRIFT_READY;
    session->clockDrift.model.anchorCycle = 1;
    session->clockDrift.model.trainedAtGeneration = 1;
    const uint64_t previousAgeAnchor = session->calibrationHostNs;
    g_nowNs += 60000000000ull;
    g_invalidTrackingSample = true;
    CheckEq("invalid feedback probe is a collective failure",TileXRClockSynchronize(
        detector,reinterpret_cast<aclrtStream>(0x1),&result),TileXR::TILEXR_ERROR_INTERNAL);
    CheckEq("invalid probe cannot renew validation age",session->calibrationHostNs,previousAgeAnchor);
    g_invalidTrackingSample = false;
    CheckEq("destroy invalid-probe detector",TileXRStragglerDestroy(detector),TileXR::TILEXR_SUCCESS);
}

} // namespace

int main()
{
    ScopedClockTransport environment(nullptr);
    ScopedClockTransport deferred(nullptr, "TILEXR_CLOCK_RDMA_DEFER_CQE");
    ScopedClockTransport compact(nullptr, "TILEXR_CLOCK_RDMA_COMPACT");
    ScopedClockTransport drift(nullptr, "TILEXR_CLOCK_DRIFT_WINDOW_US");
    ScopedClockTransport tracking(nullptr, "TILEXR_CLOCK_DRIFT_TRACK");
    TestValidationAndAllocationFailures();
    TestLifecycleAndCrossThreadOwnership();
    TestCommunicatorOwnershipAndMultiStreamDestroy();
    TestDomainPreflightFailures();
    TestDefaultClockTransport();
    TestRdmaWithoutPeerMemoryAndRegistrationReuse();
    TestRdmaUnsupportedAndTransportMismatch();
    TestRdmaReadyQueuePreflight();
    TestRdmaDeferredWholeRoundAdmission();
    TestRdmaCompactRequiresDeferred();
    TestRdmaUnregisterRetryRetainsOwnership();
    TestRdmaDestroyQuiescenceRetry();
    TestRdmaClockResultFailureShutdownRetry();
    TestRdmaDestroyRemoteAbortUsesShutdown();
    TestRdmaPreparationFailureCleanup();
    TestStaleCalibrationAndRingCapacity();
    TestRuntimeFailures();
    TestCollectionSynchronizationRetryKeepsRingSlot();
    TestDriftConfigurationAndCollection();
    TestFailedDriftGatherDoesNotPublishPartialModel();
    TestTrackingConfigurationAndPersistence();
    CheckTrue("all tests released device allocations", g_allocations.empty());
    CheckEq("all tests released RDMA registrations", g_rdmaHandle, TileXRRDMAMemHandle {0});
    TileXRDiagnostics::Host::SetStragglerRuntimeHooksForTest(nullptr);
    return g_failures == 0 ? 0 : 1;
}
