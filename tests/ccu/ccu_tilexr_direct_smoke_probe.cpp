/*
 * Copyright (c) 2026 TileXR Project
 * Licensed under the Apache License, Version 2.0
 *
 * Private C++ integration probe for the TileXR-owned direct CCU prepare path.
 * The default run is intentionally hardware-safe. Set TILEXR_CCU_DIRECT_SMOKE_ENABLE=1
 * in a real multi-rank TileXRComm launch to prepare the no-hcomm direct CCU install attempt.
 */

#include "acl/acl.h"
#include "acl/acl_rt.h"
#include "tilexr_api.h"
#include "tilexr_types.h"
#include "ccu/tilexr_ccu_collective_planner.h"
#include "ccu/tilexr_ccu_executor.h"
#include "ccu/tilexr_ccu_runtime_session.h"
#include "tools/socket/tilexr_sock_exchange.h"
#include "runtime/dev.h"

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <new>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

struct DirectCcuSmokeContext {
    std::unique_ptr<TileXR::TileXRSockExchange> exchange;
    TileXR::TileXRCcuRuntimeSession session;
    TileXR::TileXRCcuCollectivePlanner planner;
    TileXR::TileXRCcuExecutor executor;
};

using TileXRDirectCcuPrepareOptions = TileXR::TileXRCcuDirectInstallOptions;
using TileXRDirectCcuPrepareReport = TileXR::TileXRCcuDirectInstallReport;
using TileXRDirectCcuSubmitReport = TileXR::TileXRCcuDirectSubmitReport;
using TileXRDirectCcuPreparedTasksPtr = TileXR::TileXRCcuDirectInstallAttempt*;
using TileXRDirectCcuTaskInfo = TileXR::TileXRCcuTask;
using TileXRDirectCcuInstructionReadbackReport = TileXR::TileXRCcuDriverAdapterReport;

constexpr uint32_t TILEXR_DIRECT_CCU_SQE_ARGS_LEN = TileXR::TILEXR_CCU_SQE_ARGS_LEN;

struct TileXRDirectCcuInstructionWords {
    uint32_t words[4] = {};
};

constexpr const char* kEnableEnv = "TILEXR_CCU_DIRECT_SMOKE_ENABLE";
constexpr const char* kThreadModeEnv = "TILEXR_CCU_DIRECT_SMOKE_THREAD_MODE";
constexpr const char* kDirectCcuOnlyInitEnv = "TILEXR_CCU_DIRECT_SMOKE_DIRECT_CCU_ONLY_INIT";
constexpr const char* kFastExitOnPrepareFailureEnv = "TILEXR_CCU_DIRECT_SMOKE_FAST_EXIT_ON_PREPARE_FAILURE";
constexpr const char* kFastExitAfterRunEnv = "TILEXR_CCU_DIRECT_SMOKE_FAST_EXIT_AFTER_RUN";
constexpr const char* kTraceLifecycleEnv = "TILEXR_CCU_DIRECT_SMOKE_TRACE_LIFECYCLE";
constexpr const char* kReadbackInstructionsEnv = "TILEXR_CCU_DIRECT_SMOKE_READBACK_INSTRUCTIONS";
constexpr const char* kSubmitEnv = "TILEXR_CCU_DIRECT_SMOKE_SUBMIT";
constexpr const char* kReadyDirEnv = "TILEXR_CCU_DIRECT_SMOKE_READY_DIR";
constexpr const char* kDoneDirEnv = "TILEXR_CCU_DIRECT_SMOKE_DONE_DIR";
constexpr const char* kReadyTimeoutMsEnv = "TILEXR_CCU_DIRECT_SMOKE_READY_TIMEOUT_MS";
constexpr const char* kSubmitTaskSelectorEnv = "TILEXR_CCU_DIRECT_SMOKE_SUBMIT_TASK_SELECTOR";
constexpr const char* kDelayRankEnv = "TILEXR_CCU_DIRECT_SMOKE_DELAY_RANK";
constexpr const char* kPreSubmitDelayMsEnv = "TILEXR_CCU_DIRECT_SMOKE_PRE_SUBMIT_DELAY_MS";
constexpr const char* kP2pCcuCopyEnv = "TILEXR_CCU_DIRECT_SMOKE_P2P_CCU_COPY";
constexpr const char* kExpectP2pCcuCopyEnv = "TILEXR_CCU_DIRECT_SMOKE_EXPECT_P2P_CCU_COPY";
constexpr const char* kP2pCcuCopyBytesEnv = "TILEXR_CCU_DIRECT_SMOKE_P2P_CCU_COPY_BYTES";
constexpr const char* kLocalWaitCkeStartEnv = "TILEXR_CCU_PROBE_LOCAL_WAIT_CKE_START";
constexpr const char* kLocalWaitCkeCountEnv = "TILEXR_CCU_PROBE_LOCAL_WAIT_CKE_COUNT";
constexpr const char* kRemoteNotifyCkeStartEnv = "TILEXR_CCU_PROBE_REMOTE_NOTIFY_CKE_START";
constexpr const char* kRemoteNotifyCkeCountEnv = "TILEXR_CCU_PROBE_REMOTE_NOTIFY_CKE_COUNT";
constexpr const char* kBarrierModeEnv = "TILEXR_CCU_DIRECT_BARRIER_MODE";
constexpr const char* kRepositoryInstallWindowEnv = "TILEXR_CCU_DIRECT_REPOSITORY_INSTALL_WINDOW";
constexpr const char* kRepositoryInstallDataLenModeEnv = "TILEXR_CCU_DIRECT_REPOSITORY_DATA_LEN_MODE";
constexpr const char* kRepositoryMemoryAllocModeEnv = "TILEXR_CCU_DIRECT_REPOSITORY_MEMORY_ALLOC_MODE";
constexpr const char* kInstallOrderEnv = "TILEXR_CCU_DIRECT_INSTALL_ORDER";
constexpr const char* kResourceWindowTokenIdEnv = "TILEXR_CCU_DIRECT_RESOURCE_WINDOW_TOKEN_ID";
constexpr const char* kResourceWindowRawTokenIdEnv = "TILEXR_CCU_DIRECT_RESOURCE_WINDOW_RAW_TOKEN_ID";
constexpr const char* kResourceWindowTokenValueEnv = "TILEXR_CCU_DIRECT_RESOURCE_WINDOW_TOKEN_VALUE";
constexpr uint32_t kHcommStyleTask1PreludeInstructionCount = 5U;

struct DeviceBuffer {
    void* ptr = nullptr;
    size_t bytes = 0;

    ~DeviceBuffer()
    {
        if (ptr != nullptr) {
            aclrtFree(ptr);
        }
    }

    int Allocate(size_t size)
    {
        if (size == 0) {
            return TileXR::TILEXR_ERROR_PARA_CHECK_FAIL;
        }
        bytes = size;
        return aclrtMalloc(&ptr, bytes, ACL_MEM_MALLOC_HUGE_FIRST);
    }
};

struct P2pCcuCopyState {
    DeviceBuffer source;
    DeviceBuffer destination;
    std::vector<uint8_t> expected;
    std::vector<uint8_t> observed;
    size_t bytes = 0;
    int initRet = ACL_SUCCESS;
    int readRet = ACL_SUCCESS;
    uint32_t mismatchCount = 0;
    bool passed = false;
};

bool EnvFlag(const char* name)
{
    const char* value = std::getenv(name);
    if (value == nullptr) {
        return false;
    }
    const std::string text(value);
    return text == "1" || text == "true" || text == "TRUE" || text == "yes" || text == "on";
}

bool ShouldFastExitAfterPrepareFailure(int ret)
{
    return ret != 0 && EnvFlag(kFastExitOnPrepareFailureEnv);
}

const char* FastExitReasonForReturnCode(int ret)
{
    switch (ret) {
        case 6:
            return "prepare failed; skipping cleanup to preserve diagnostic status";
        case 8:
            return "direct CCU stream synchronize failed; skipping cleanup to preserve diagnostic status";
        case 9:
            return "direct CCU submit failed; skipping cleanup to preserve diagnostic status";
        case 13:
            return "direct CCU collective completion timed out; skipping cleanup to preserve diagnostic status";
        case 14:
            return "direct CCU P2P CCU-copy check failed; skipping cleanup to preserve diagnostic status";
        default:
            return "direct CCU smoke failed; skipping cleanup to preserve diagnostic status";
    }
}

bool ShouldFastExitAfterRun()
{
    return EnvFlag(kFastExitAfterRunEnv);
}

void TraceLifecycle(const char* stage)
{
    if (!EnvFlag(kTraceLifecycleEnv)) {
        return;
    }
    std::cout << "tilexr_ccu_direct_smoke lifecycle " << stage << std::endl;
}

int ParseInt(const char* value, int fallback)
{
    if (value == nullptr || value[0] == '\0') {
        return fallback;
    }
    char* end = nullptr;
    const long parsed = std::strtol(value, &end, 0);
    if (end == value || *end != '\0') {
        return fallback;
    }
    return static_cast<int>(parsed);
}

int EnvInt(const char* name, int fallback)
{
    return ParseInt(std::getenv(name), fallback);
}

uint64_t ParseU64(const char* value, uint64_t fallback)
{
    if (value == nullptr || value[0] == '\0') {
        return fallback;
    }
    char* end = nullptr;
    const unsigned long long parsed = std::strtoull(value, &end, 0);
    if (end == value || *end != '\0') {
        return fallback;
    }
    return static_cast<uint64_t>(parsed);
}

uint64_t EnvU64(const char* name, uint64_t fallback)
{
    return ParseU64(std::getenv(name), fallback);
}

std::string RankEnvName(const char* prefix, int rank, const char* suffix)
{
    return std::string(prefix) + std::to_string(rank) + suffix;
}

int RankEnvInt(const char* prefix, int rank, const char* suffix, const char* commonName, int fallback)
{
    const std::string rankName = RankEnvName(prefix, rank, suffix);
    const char* rankValue = std::getenv(rankName.c_str());
    if (rankValue != nullptr && rankValue[0] != '\0') {
        return ParseInt(rankValue, fallback);
    }
    return EnvInt(commonName, fallback);
}

bool SyncCkeBarrierMode()
{
    const char* value = std::getenv(kBarrierModeEnv);
    return value != nullptr && std::string(value) == "sync_cke";
}

bool SyncCkeSetWaitBarrierMode()
{
    const char* value = std::getenv(kBarrierModeEnv);
    return value != nullptr && std::string(value) == "sync_cke_set_wait";
}

bool SyncCkePostOnlyBarrierMode()
{
    const char* value = std::getenv(kBarrierModeEnv);
    return value != nullptr && std::string(value) == "sync_cke_post_only";
}

bool LocalCkeBarrierMode()
{
    const char* value = std::getenv(kBarrierModeEnv);
    return value != nullptr && std::string(value) == "local_cke";
}

bool LocalCkePostOnlyBarrierMode()
{
    const char* value = std::getenv(kBarrierModeEnv);
    return value != nullptr && std::string(value) == "local_cke_post_only";
}

bool SyncXnPostOnlyBarrierMode()
{
    const char* value = std::getenv(kBarrierModeEnv);
    return value != nullptr && std::string(value) == "sync_xn_post_only";
}

bool SyncXnLoadPostOnlyBarrierMode()
{
    const char* value = std::getenv(kBarrierModeEnv);
    return value != nullptr && std::string(value) == "sync_xn_load_post_only";
}

TileXR::TileXRCcuRepositoryInstallWindow RepositoryInstallWindowFromEnv()
{
    const char* value = std::getenv(kRepositoryInstallWindowEnv);
    if (value == nullptr || value[0] == '\0') {
        return TileXR::TileXRCcuRepositoryInstallWindow::Mission;
    }
    const std::string text(value);
    if (text == "full_repository" || text == "full" || text == "1") {
        return TileXR::TileXRCcuRepositoryInstallWindow::FullRepository;
    }
    return TileXR::TileXRCcuRepositoryInstallWindow::Mission;
}

TileXR::TileXRCcuRepositoryInstallDataLenMode RepositoryInstallDataLenModeFromEnv()
{
    const char* value = std::getenv(kRepositoryInstallDataLenModeEnv);
    if (value == nullptr || value[0] == '\0') {
        return TileXR::TileXRCcuRepositoryInstallDataLenMode::InstructionBytes;
    }
    const std::string text(value);
    if (text == "descriptor_bytes" || text == "descriptor" || text == "1") {
        return TileXR::TileXRCcuRepositoryInstallDataLenMode::DescriptorBytes;
    }
    return TileXR::TileXRCcuRepositoryInstallDataLenMode::InstructionBytes;
}

TileXR::TileXRCcuRepositoryMemoryAllocMode RepositoryMemoryAllocModeFromEnv()
{
    const char* value = std::getenv(kRepositoryMemoryAllocModeEnv);
    if (value == nullptr || value[0] == '\0') {
        return TileXR::TileXRCcuRepositoryMemoryAllocMode::Acl;
    }
    const std::string text(value);
    if (text == "acl_module3" || text == "acl_hccl_module" || text == "module3" || text == "1") {
        return TileXR::TileXRCcuRepositoryMemoryAllocMode::AclModule3;
    }
    if (text == "rt_hbm" || text == "rt" || text == "runtime_hbm" || text == "2") {
        return TileXR::TileXRCcuRepositoryMemoryAllocMode::RtHbm;
    }
    return TileXR::TileXRCcuRepositoryMemoryAllocMode::Acl;
}

TileXR::TileXRCcuInstallOrder InstallOrderFromEnv()
{
    const char* value = std::getenv(kInstallOrderEnv);
    if (value == nullptr || value[0] == '\0') {
        return TileXR::TileXRCcuInstallOrder::InstallLowerLayerFirst;
    }
    const std::string text(value);
    if (text == "lower_layer_first" || text == "install_lower_layer_first" || text == "1") {
        return TileXR::TileXRCcuInstallOrder::InstallLowerLayerFirst;
    }
    if (text == "repository_first" || text == "repo_first" || text == "0") {
        return TileXR::TileXRCcuInstallOrder::RepositoryFirst;
    }
    return TileXR::TileXRCcuInstallOrder::RepositoryFirst;
}

uint32_t DefaultSyncInstructionCount(uint32_t syncResourceCount)
{
    if (SyncCkeBarrierMode() || SyncCkeSetWaitBarrierMode()) {
        return syncResourceCount * 2U + 1U;
    }
    if (SyncCkePostOnlyBarrierMode()) {
        return syncResourceCount + 1U;
    }
    if (LocalCkePostOnlyBarrierMode()) {
        return syncResourceCount;
    }
    if (SyncXnPostOnlyBarrierMode()) {
        return kHcommStyleTask1PreludeInstructionCount + syncResourceCount;
    }
    if (SyncXnLoadPostOnlyBarrierMode()) {
        return kHcommStyleTask1PreludeInstructionCount + syncResourceCount * 2U;
    }
    return kHcommStyleTask1PreludeInstructionCount + syncResourceCount * 2U;
}

const char* FirstEnv(const char* a, const char* b, const char* c, const char* d)
{
    const char* value = std::getenv(a);
    if (value != nullptr && value[0] != '\0') {
        return value;
    }
    value = std::getenv(b);
    if (value != nullptr && value[0] != '\0') {
        return value;
    }
    value = std::getenv(c);
    if (value != nullptr && value[0] != '\0') {
        return value;
    }
    value = std::getenv(d);
    return value != nullptr && value[0] != '\0' ? value : nullptr;
}

int RankFromEnv()
{
    const char* rank = std::getenv("TILEXR_CCU_PROBE_RANK");
    if (rank != nullptr && rank[0] != '\0') {
        return ParseInt(rank, 0);
    }
    return ParseInt(FirstEnv("PMI_RANK", "OMPI_COMM_WORLD_RANK", "MV2_COMM_WORLD_RANK", "RANK"), 0);
}

int RankSizeFromEnv()
{
    const char* rankSize = std::getenv("TILEXR_CCU_PROBE_RANK_SIZE");
    if (rankSize != nullptr && rankSize[0] != '\0') {
        return ParseInt(rankSize, 1);
    }
    return ParseInt(FirstEnv("PMI_SIZE", "OMPI_COMM_WORLD_SIZE", "MV2_COMM_WORLD_SIZE", "RANK_SIZE"), 1);
}

std::vector<uint8_t> BuildP2pCcuCopyPattern(int rank, size_t bytes)
{
    std::vector<uint8_t> pattern(bytes);
    for (size_t i = 0; i < bytes; ++i) {
        pattern[i] = static_cast<uint8_t>((static_cast<uint32_t>(rank + 1) * 17U + i * 13U) & 0xffU);
    }
    return pattern;
}

int InitP2pCcuCopyState(int rank, int peer, P2pCcuCopyState* state)
{
    if (state == nullptr) {
        return TileXR::TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    state->bytes = static_cast<size_t>(EnvInt(kP2pCcuCopyBytesEnv, 4096));
    if (state->bytes == 0) {
        state->initRet = TileXR::TILEXR_ERROR_PARA_CHECK_FAIL;
        return state->initRet;
    }
    state->expected = BuildP2pCcuCopyPattern(peer, state->bytes);
    state->observed.assign(state->bytes, 0);
    const std::vector<uint8_t> source = BuildP2pCcuCopyPattern(rank, state->bytes);
    std::vector<uint8_t> destination(state->bytes, 0xa5U);

    int ret = state->source.Allocate(state->bytes);
    if (ret != ACL_SUCCESS) {
        state->initRet = ret;
        return ret;
    }
    ret = state->destination.Allocate(state->bytes);
    if (ret != ACL_SUCCESS) {
        state->initRet = ret;
        return ret;
    }
    ret = aclrtMemcpy(
        state->source.ptr,
        state->bytes,
        source.data(),
        state->bytes,
        ACL_MEMCPY_HOST_TO_DEVICE);
    if (ret != ACL_SUCCESS) {
        state->initRet = ret;
        return ret;
    }
    ret = aclrtMemcpy(
        state->destination.ptr,
        state->bytes,
        destination.data(),
        state->bytes,
        ACL_MEMCPY_HOST_TO_DEVICE);
    state->initRet = ret;
    return ret;
}

int CheckP2pCcuCopyState(P2pCcuCopyState* state)
{
    if (state == nullptr || state->destination.ptr == nullptr || state->bytes == 0) {
        return TileXR::TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    state->readRet = aclrtMemcpy(
        state->observed.data(),
        state->observed.size(),
        state->destination.ptr,
        state->bytes,
        ACL_MEMCPY_DEVICE_TO_HOST);
    if (state->readRet != ACL_SUCCESS) {
        return state->readRet;
    }
    state->mismatchCount = 0;
    for (size_t i = 0; i < state->bytes; ++i) {
        if (state->observed[i] != state->expected[i]) {
            ++state->mismatchCount;
        }
    }
    state->passed = state->mismatchCount == 0;
    return state->passed ? ACL_SUCCESS : TileXR::TILEXR_ERROR_INTERNAL;
}

void PrintP2pCcuCopy(
    int rank,
    int peer,
    const P2pCcuCopyState& state,
    int prepareRet,
    int submitRet,
    int syncRet)
{
    const uint32_t firstObserved = state.observed.empty() ? 0U : state.observed.front();
    const uint32_t firstExpected = state.expected.empty() ? 0U : state.expected.front();
    std::cout << "tilexr_ccu_direct_smoke p2pCcuCopy"
              << " rank=" << rank
              << " peer=" << peer
              << " bytes=" << state.bytes
              << " initRet=" << state.initRet
              << " prepareRet=" << prepareRet
              << " submitRet=" << submitRet
              << " syncRet=" << syncRet
              << " readRet=" << state.readRet
              << " mismatches=" << state.mismatchCount
              << " firstObserved=0x" << std::hex << firstObserved
              << " firstExpected=0x" << firstExpected
              << std::dec
              << " passed=" << (state.passed ? 1 : 0)
              << std::endl;
}

int RunP2pCcuCopy(
    int rank,
    int peer,
    P2pCcuCopyState* state,
    int prepareRet,
    int submitRet,
    int syncRet)
{
    if (state == nullptr) {
        return TileXR::TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    if (syncRet == ACL_SUCCESS && submitRet == TileXR::TILEXR_SUCCESS) {
        (void)CheckP2pCcuCopyState(state);
    }
    PrintP2pCcuCopy(rank, peer, *state, prepareRet, submitRet, syncRet);
    return state->passed ? TileXR::TILEXR_SUCCESS : TileXR::TILEXR_ERROR_INTERNAL;
}

int DeviceFromList(const char* devices, int rank, int fallback)
{
    if (devices == nullptr || devices[0] == '\0') {
        return fallback;
    }
    std::string list(devices);
    size_t start = 0;
    int index = 0;
    while (start <= list.size()) {
        const size_t comma = list.find(',', start);
        const size_t end = comma == std::string::npos ? list.size() : comma;
        if (index == rank && end > start) {
            return ParseInt(list.substr(start, end - start).c_str(), fallback);
        }
        if (comma == std::string::npos) {
            break;
        }
        start = comma + 1;
        ++index;
    }
    return fallback;
}

int DeviceFromEnv(int rank)
{
    const char* explicitDevice = std::getenv("TILEXR_CCU_PROBE_DEVICE");
    if (explicitDevice != nullptr && explicitDevice[0] != '\0') {
        return ParseInt(explicitDevice, 0);
    }
    const int firstDevice = EnvInt("TILEXR_TEST_FIRST_NPU", 0);
    return DeviceFromList(std::getenv("TILEXR_TEST_DEVICES"), rank, firstDevice + rank);
}

int InitCommForDirectCcuSmoke(int commDomain, int rankSize, int rank, int device, DirectCcuSmokeContext* context)
{
    if (context == nullptr) {
        return TileXR::TILEXR_ERROR_PARA_CHECK_FAIL;
    }

    context->exchange.reset(new (std::nothrow) TileXR::TileXRSockExchange(rank, rankSize, commDomain));
    if (context->exchange == nullptr) {
        return TileXR::TILEXR_ERROR_INTERNAL;
    }

    TileXR::TileXRCcuBackendOptions options {};
    options.rank = rank;
    options.rankSize = rankSize;
    options.devId = device;
    options.uid = "tilexr-direct-smoke-probe";
    options.exchange = context->exchange.get();
    const int ret = context->session.Init(options);
    if (ret == TileXR::TILEXR_SUCCESS) {
        std::cout << "tilexr_ccu_direct_smoke internalDirectCcuInit"
                  << " rank=" << rank
                  << " rankSize=" << rankSize
                  << " device=" << device
                  << " directOnly=" << (EnvFlag(kDirectCcuOnlyInitEnv) ? 1 : 0)
                  << std::endl;
    }
    return ret;
}

TileXRDirectCcuPrepareOptions MakePrepareOptions(int rank, int rankSize, int device)
{
    TileXRDirectCcuPrepareOptions options {};
    options.syncResourceCount = static_cast<uint32_t>(EnvInt("TILEXR_CCU_PROBE_SYNC_RESOURCE_COUNT", 1));
    options.sqeArgCount =
        static_cast<uint32_t>(EnvInt("TILEXR_CCU_PROBE_SQE_ARG_COUNT", TILEXR_DIRECT_CCU_SQE_ARGS_LEN));
    const uint32_t defaultSyncInstructionCount = DefaultSyncInstructionCount(options.syncResourceCount);
    options.syncInstructionCount =
        static_cast<uint32_t>(EnvInt("TILEXR_CCU_PROBE_SYNC_INSTRUCTION_COUNT", defaultSyncInstructionCount));
    options.bindingsPerSyncResource = static_cast<uint32_t>(EnvInt("TILEXR_CCU_PROBE_BINDINGS_PER_RESOURCE", 1));
    options.missionStartId = static_cast<uint16_t>(EnvInt("TILEXR_CCU_PROBE_MISSION_START", 1));
    options.instructionStartId = static_cast<uint16_t>(EnvInt("TILEXR_CCU_PROBE_INSTRUCTION_START", 1));
    options.missionInstructionStartId =
        static_cast<uint16_t>(EnvInt("TILEXR_CCU_PROBE_MISSION_INSTRUCTION_START", 0));
    options.xnStartId = static_cast<uint16_t>(
        RankEnvInt("TILEXR_CCU_PROBE_RANK", rank, "_XN_START", "TILEXR_CCU_PROBE_XN_START", 1));
    options.gsaStartId = static_cast<uint16_t>(EnvInt("TILEXR_CCU_PROBE_GSA_START", 0));
    options.remoteXnStartId = static_cast<uint16_t>(
        RankEnvInt("TILEXR_CCU_PROBE_RANK", rank, "_REMOTE_XN_START", "TILEXR_CCU_PROBE_REMOTE_XN_START", 0));
    options.remoteXnCount = static_cast<uint16_t>(
        RankEnvInt("TILEXR_CCU_PROBE_RANK", rank, "_REMOTE_XN_COUNT", "TILEXR_CCU_PROBE_REMOTE_XN_COUNT", 0));
    options.ckeStartId = static_cast<uint16_t>(EnvInt("TILEXR_CCU_PROBE_CKE_START", 1));
    options.channelStartId = static_cast<uint16_t>(EnvInt("TILEXR_CCU_PROBE_CHANNEL_START", 1));
    options.localWaitCkeStartId = static_cast<uint16_t>(
        RankEnvInt("TILEXR_CCU_PROBE_RANK", rank, "_LOCAL_WAIT_CKE_START", kLocalWaitCkeStartEnv, 0));
    options.localWaitCkeCount = static_cast<uint16_t>(
        RankEnvInt("TILEXR_CCU_PROBE_RANK", rank, "_LOCAL_WAIT_CKE_COUNT", kLocalWaitCkeCountEnv, 0));
    options.remoteNotifyCkeStartId = static_cast<uint16_t>(
        RankEnvInt("TILEXR_CCU_PROBE_RANK", rank, "_REMOTE_NOTIFY_CKE_START", kRemoteNotifyCkeStartEnv, 0));
    options.remoteNotifyCkeCount = static_cast<uint16_t>(
        RankEnvInt("TILEXR_CCU_PROBE_RANK", rank, "_REMOTE_NOTIFY_CKE_COUNT", kRemoteNotifyCkeCountEnv, 0));
    options.repositoryInstallOptions.window = RepositoryInstallWindowFromEnv();
    options.repositoryInstallOptions.dataLenMode = RepositoryInstallDataLenModeFromEnv();
    options.repositoryMemoryAllocMode = RepositoryMemoryAllocModeFromEnv();
    options.installOrder = InstallOrderFromEnv();
    options.deviceId = static_cast<uint32_t>(device);
    options.rank = static_cast<uint32_t>(rank);
    options.provider = rankSize > 0 ? "tilexr-direct-smoke-probe" : "";
    return options;
}

void PrintInstallReport(
    const char* prefix,
    int ret,
    const TileXRDirectCcuPrepareReport& report)
{
    std::cout << prefix
              << " ret=" << ret
              << " pipelineBuilt=" << (report.pipelineBuilt ? 1 : 0)
              << " installAttempted=" << (report.installAttempted ? 1 : 0)
              << " installSucceeded=" << (report.installSucceeded ? 1 : 0)
              << " submitReady=" << (report.submitReady ? 1 : 0)
              << " requiredInstallSurfaceCount=" << report.requiredInstallSurfaceCount
              << " publicVerifiedInstallSurfaceCount=" << report.publicVerifiedInstallSurfaceCount
              << " missingInstallSurfaceCount=" << report.missingInstallSurfaceCount
              << " taskCount=" << report.taskCount
              << " submitTaskCount=" << report.submitTaskCount
              << " message=\"" << report.message << "\""
              << std::endl;
}

void PrintSubmitReport(
    const char* prefix,
    int ret,
    const TileXRDirectCcuSubmitReport& report)
{
    std::cout << prefix
              << " ret=" << ret
              << " submitted=" << (report.submitted ? 1 : 0)
              << " taskCount=" << report.taskCount
              << " submittedTaskCount=" << report.submittedTaskCount
              << " message=\"" << report.message << "\""
              << std::endl;
}

int TileXRDirectCcuGetPreparedTask(
    TileXRDirectCcuPreparedTasksPtr prepared,
    uint32_t taskIndex,
    TileXRDirectCcuTaskInfo* task)
{
    if (prepared == nullptr || task == nullptr || taskIndex >= prepared->submitTasks.size()) {
        return TileXR::TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    *task = prepared->submitTasks[taskIndex];
    return TileXR::TILEXR_SUCCESS;
}

int TileXRDirectCcuSubmitPrepared(
    TileXRDirectCcuPreparedTasksPtr prepared,
    void* stream,
    TileXRDirectCcuSubmitReport* report)
{
    if (prepared == nullptr) {
        return TileXR::TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    return TileXR::TileXRCcuSubmitPreparedTasks(prepared->submitTasks, stream, nullptr, nullptr, report);
}

int TileXRDirectCcuSubmitPreparedTask(
    TileXRDirectCcuPreparedTasksPtr prepared,
    uint32_t taskIndex,
    void* stream,
    TileXRDirectCcuSubmitReport* report)
{
    if (prepared == nullptr || taskIndex >= prepared->submitTasks.size()) {
        if (report != nullptr) {
            *report = TileXRDirectCcuSubmitReport {};
            report->taskCount = prepared == nullptr ? 0U : static_cast<uint32_t>(prepared->submitTasks.size());
            report->message = "selected direct CCU submit task is missing";
        }
        return TileXR::TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    std::vector<TileXR::TileXRCcuTask> selected {prepared->submitTasks[taskIndex]};
    return TileXR::TileXRCcuSubmitPreparedTasks(selected, stream, nullptr, nullptr, report);
}

int TileXRDirectCcuDestroyPrepared(TileXRDirectCcuPreparedTasksPtr prepared)
{
    if (prepared == nullptr) {
        return TileXR::TILEXR_SUCCESS;
    }
    return TileXR::TileXRCcuReleaseDirectInstallAttemptResources(*prepared);
}

int TileXRCommReadDirectCcuInstructions(
    DirectCcuSmokeContext* context,
    uint8_t dieId,
    uint16_t instructionStartId,
    uint32_t instructionCount,
    TileXRDirectCcuInstructionWords* instructions,
    TileXRDirectCcuInstructionReadbackReport* report)
{
    if (context == nullptr || instructions == nullptr) {
        return TileXR::TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    return context->executor.ReadDirectCcuInstructionsForDebug(
        context->session,
        dieId,
        instructionStartId,
        instructions,
        instructionCount,
        sizeof(TileXRDirectCcuInstructionWords),
        report);
}

int SubmitPreparedWithSelector(
    TileXRDirectCcuPreparedTasksPtr prepared,
    uint32_t taskCount,
    void* stream,
    TileXRDirectCcuSubmitReport* report)
{
    const char* value = std::getenv(kSubmitTaskSelectorEnv);
    const std::string selector = value == nullptr ? "all" : std::string(value);
    if (selector.empty() || selector == "all") {
        return TileXRDirectCcuSubmitPrepared(prepared, stream, report);
    }
    const bool selectFirst = selector == "first";
    const bool selectSecond = selector == "second";
    if (!selectFirst && !selectSecond) {
        if (report != nullptr) {
            *report = TileXRDirectCcuSubmitReport {};
            report->message = "invalid TILEXR_CCU_DIRECT_SMOKE_SUBMIT_TASK_SELECTOR";
        }
        return TileXR::TILEXR_ERROR_PARA_CHECK_FAIL;
    }

    const uint32_t selectedIndex = selectFirst ? 0U : 1U;
    std::cout << "tilexr_ccu_direct_smoke submitTaskSelector="
              << selector
              << " selectedIndex=" << selectedIndex
              << " preparedTaskCount=" << taskCount
              << std::endl;
    if (selectedIndex >= taskCount) {
        if (report != nullptr) {
            *report = TileXRDirectCcuSubmitReport {};
            report->taskCount = taskCount;
            report->message = "selected direct CCU submit task is missing";
        }
        return TileXR::TILEXR_ERROR_PARA_CHECK_FAIL;
    }

    return TileXRDirectCcuSubmitPreparedTask(prepared, selectedIndex, stream, report);
}

void PrintSubmitTiming(
    int rank,
    int delayMs,
    int submitRet,
    int syncRet,
    long long submitMs,
    long long syncMs)
{
    std::cout << "tilexr_ccu_direct_smoke submitTiming"
              << " rank=" << rank
              << " preSubmitDelayMs=" << delayMs
              << " submitRet=" << submitRet
              << " syncRet=" << syncRet
              << " submitMs=" << submitMs
              << " syncMs=" << syncMs
              << std::endl;
}

void PrintConfig(
    const TileXRDirectCcuPrepareOptions& options,
    int rankSize)
{
    std::cout << "tilexr_ccu_direct_smoke config"
              << " rank=" << options.rank
              << " rankSize=" << rankSize
              << " device=" << options.deviceId
              << " syncResourceCount=" << options.syncResourceCount
              << " sqeArgCount=" << options.sqeArgCount
              << " syncInstructionCount=" << options.syncInstructionCount
              << " bindingsPerSyncResource=" << options.bindingsPerSyncResource
              << " missionStartId=" << options.missionStartId
              << " instructionStartId=" << options.instructionStartId
              << " missionInstructionStartId=" << options.missionInstructionStartId
              << " xnStartId=" << options.xnStartId
              << " gsaStartId=" << options.gsaStartId
              << " remoteXnStartId=" << options.remoteXnStartId
              << " remoteXnCount=" << options.remoteXnCount
              << " ckeStartId=" << options.ckeStartId
              << " channelStartId=" << options.channelStartId
              << " localWaitCkeStartId=" << options.localWaitCkeStartId
              << " localWaitCkeCount=" << options.localWaitCkeCount
              << " remoteNotifyCkeStartId=" << options.remoteNotifyCkeStartId
              << " remoteNotifyCkeCount=" << options.remoteNotifyCkeCount
              << " repositoryInstallWindow=" << static_cast<uint32_t>(options.repositoryInstallOptions.window)
              << " repositoryInstallDataLenMode=" << static_cast<uint32_t>(options.repositoryInstallOptions.dataLenMode)
              << " repositoryMemoryAllocMode=" << static_cast<uint32_t>(options.repositoryMemoryAllocMode)
              << " installOrder=" << static_cast<uint32_t>(options.installOrder)
              << " barrierMode=\"" << (std::getenv(kBarrierModeEnv) == nullptr ? "" : std::getenv(kBarrierModeEnv))
              << "\""
              << " resourceWindowTokenId=\""
              << (std::getenv(kResourceWindowTokenIdEnv) == nullptr ? "" : std::getenv(kResourceWindowTokenIdEnv))
              << "\""
              << " resourceWindowRawTokenId=\""
              << (std::getenv(kResourceWindowRawTokenIdEnv) == nullptr ? "" : std::getenv(kResourceWindowRawTokenIdEnv))
              << "\""
              << " resourceWindowTokenValue=\""
              << (std::getenv(kResourceWindowTokenValueEnv) == nullptr ? "" : std::getenv(kResourceWindowTokenValueEnv))
              << "\""
              << " provider=\"" << options.provider << "\""
              << std::endl;
}

void PrintPreparedTasks(TileXRDirectCcuPreparedTasksPtr prepared, uint32_t taskCount)
{
    std::cout << "tilexr_ccu_direct_smoke preparedTasks"
              << " count=" << taskCount;
    const uint32_t previewCount = taskCount < 2U ? taskCount : 2U;
    for (size_t i = 0; i < previewCount; ++i) {
        TileXRDirectCcuTaskInfo task;
        const int ret = TileXRDirectCcuGetPreparedTask(prepared, static_cast<uint32_t>(i), &task);
        if (ret != TileXR::TILEXR_SUCCESS) {
            std::cout << " task" << i << ".ret=" << ret;
            continue;
        }
        std::cout << " task" << i
                  << ".dieId=" << static_cast<uint32_t>(task.dieId)
                  << " task" << i << ".missionId=" << static_cast<uint32_t>(task.missionId)
                  << " task" << i << ".timeout=" << task.timeout
                  << " task" << i << ".instStartId=" << task.instStartId
                  << " task" << i << ".instCnt=" << task.instCnt
                  << " task" << i << ".key=0x" << std::hex << task.key << std::dec
                  << " task" << i << ".argSize=" << task.argSize;
        for (uint32_t arg = 0; arg < TILEXR_DIRECT_CCU_SQE_ARGS_LEN; ++arg) {
            std::cout << " task" << i
                      << ".arg" << arg << "=0x"
                      << std::hex << task.args[arg] << std::dec;
        }
    }
    std::cout << std::endl;
}

void PrintInstructionReadback(DirectCcuSmokeContext* context, TileXRDirectCcuPreparedTasksPtr prepared, uint32_t taskCount)
{
    if (!EnvFlag(kReadbackInstructionsEnv)) {
        return;
    }
    if (context == nullptr) {
        std::cout << "tilexr_ccu_direct_smoke instructionReadback ret="
                  << TileXR::TILEXR_ERROR_PARA_CHECK_FAIL
                  << " message=\"missing direct CCU smoke context\""
                  << std::endl;
        return;
    }
    for (uint32_t taskIndex = 0; taskIndex < taskCount; ++taskIndex) {
        TileXRDirectCcuTaskInfo task;
        const int taskRet = TileXRDirectCcuGetPreparedTask(prepared, taskIndex, &task);
        if (taskRet != TileXR::TILEXR_SUCCESS) {
            std::cout << "tilexr_ccu_direct_smoke instructionReadback"
                      << " task=" << taskIndex
                      << " ret=" << taskRet
                      << " message=\"failed to read prepared task\""
                      << std::endl;
            continue;
        }

        constexpr uint32_t kMaxReadbackInstructionCount = 8U;
        const uint32_t readCount = task.instCnt < kMaxReadbackInstructionCount ?
            task.instCnt : kMaxReadbackInstructionCount;
        std::vector<TileXRDirectCcuInstructionWords> readback(readCount);
        TileXRDirectCcuInstructionReadbackReport report;
        const int readRet = TileXRCommReadDirectCcuInstructions(
            context,
            static_cast<uint8_t>(task.dieId),
            static_cast<uint16_t>(task.instStartId),
            readCount,
            readback.data(),
            &report);
        std::cout << "tilexr_ccu_direct_smoke instructionReadback"
                  << " task=" << taskIndex
                  << " ret=" << readRet
                  << " dieId=" << static_cast<uint32_t>(task.dieId)
                  << " instStartId=" << task.instStartId
                  << " requestedCount=" << task.instCnt
                  << " readCount=" << readCount
                  << " opcode=" << report.opcode
                  << " driverRet=" << report.driverRet
                  << " opRet=" << report.opRet
                  << " message=\"" << report.message << "\"";
        if (readRet == TileXR::TILEXR_SUCCESS) {
            for (uint32_t i = 0; i < readCount; ++i) {
                std::cout << " instr" << i << "=";
                for (uint32_t word = 0; word < 4U; ++word) {
                    if (word != 0) {
                        std::cout << ",";
                    }
                    std::cout << "0x" << std::hex << std::nouppercase << readback[i].words[word] << std::dec;
                }
            }
        }
        std::cout << std::endl;
    }
}

bool CollectiveSubmitReadyGateConfigured()
{
    const char* readyDir = std::getenv(kReadyDirEnv);
    return readyDir != nullptr && readyDir[0] != '\0';
}

std::string SubmitReadinessPath(int rank)
{
    const char* readyDir = std::getenv(kReadyDirEnv);
    if (readyDir == nullptr || readyDir[0] == '\0') {
        return {};
    }
    return std::string(readyDir) + "/rank" + std::to_string(rank) + ".ready";
}

bool WriteSubmitReadiness(int rank, bool ready)
{
    const std::string path = SubmitReadinessPath(rank);
    if (path.empty()) {
        return true;
    }
    std::ofstream out(path.c_str(), std::ios::out | std::ios::trunc);
    if (!out) {
        return false;
    }
    out << (ready ? 1 : 0) << "\n";
    return static_cast<bool>(out);
}

bool ReadSubmitReadiness(int rank, bool* ready)
{
    if (ready == nullptr) {
        return false;
    }
    const std::string path = SubmitReadinessPath(rank);
    if (path.empty()) {
        return false;
    }
    std::ifstream in(path.c_str());
    if (!in) {
        return false;
    }
    int value = 0;
    in >> value;
    if (!in) {
        return false;
    }
    *ready = value != 0;
    return true;
}

bool WaitForCollectiveSubmitReadiness(int rank, int rankSize, bool localReady)
{
    if (!CollectiveSubmitReadyGateConfigured()) {
        return localReady;
    }
    const bool wrote = WriteSubmitReadiness(rank, localReady);
    const int timeoutMs = EnvInt(kReadyTimeoutMsEnv, 5000);
    const auto start = std::chrono::steady_clock::now();
    for (;;) {
        bool allSeen = wrote;
        bool allReady = wrote && localReady;
        for (int peer = 0; peer < rankSize; ++peer) {
            bool peerReady = false;
            if (!ReadSubmitReadiness(peer, &peerReady)) {
                allSeen = false;
                allReady = false;
                break;
            }
            allReady = allReady && peerReady;
        }
        if (allSeen) {
            std::cout << "tilexr_ccu_direct_smoke collectiveSubmitReady"
                      << " rank=" << rank
                      << " localReady=" << (localReady ? 1 : 0)
                      << " allRanksReady=" << (allReady ? 1 : 0)
                      << std::endl;
            return allReady;
        }
        const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start).count();
        if (elapsedMs >= timeoutMs) {
            std::cout << "tilexr_ccu_direct_smoke collectiveSubmitReady"
                      << " rank=" << rank
                      << " localReady=" << (localReady ? 1 : 0)
                      << " allRanksReady=0"
                      << " timeoutMs=" << timeoutMs
                      << std::endl;
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
}

bool CollectiveSubmitDoneGateConfigured()
{
    const char* doneDir = std::getenv(kDoneDirEnv);
    return doneDir != nullptr && doneDir[0] != '\0';
}

std::string SubmitDonePath(int rank)
{
    const char* doneDir = std::getenv(kDoneDirEnv);
    if (doneDir == nullptr || doneDir[0] == '\0') {
        return {};
    }
    return std::string(doneDir) + "/rank" + std::to_string(rank) + ".done";
}

bool WriteSubmitDone(int rank, int result)
{
    const std::string path = SubmitDonePath(rank);
    if (path.empty()) {
        return true;
    }
    std::ofstream out(path.c_str(), std::ios::out | std::ios::trunc);
    if (!out) {
        return false;
    }
    out << result << "\n";
    return static_cast<bool>(out);
}

bool ReadSubmitDone(int rank, int* result)
{
    if (result == nullptr) {
        return false;
    }
    const std::string path = SubmitDonePath(rank);
    if (path.empty()) {
        return false;
    }
    std::ifstream in(path.c_str());
    if (!in) {
        return false;
    }
    in >> *result;
    return static_cast<bool>(in);
}

bool WaitForCollectiveSubmitDone(int rank, int rankSize, int localResult)
{
    if (!CollectiveSubmitDoneGateConfigured()) {
        return true;
    }
    const bool wrote = WriteSubmitDone(rank, localResult);
    const int timeoutMs = EnvInt(kReadyTimeoutMsEnv, 5000);
    const auto start = std::chrono::steady_clock::now();
    for (;;) {
        bool allSeen = wrote;
        bool allSucceeded = wrote && localResult == 0;
        for (int peer = 0; peer < rankSize; ++peer) {
            int peerResult = 0;
            if (!ReadSubmitDone(peer, &peerResult)) {
                allSeen = false;
                allSucceeded = false;
                break;
            }
            allSucceeded = allSucceeded && peerResult == 0;
        }
        if (allSeen) {
            std::cout << "tilexr_ccu_direct_smoke collectiveSubmitDone"
                      << " rank=" << rank
                      << " localResult=" << localResult
                      << " allRanksDone=1"
                      << " allRanksSucceeded=" << (allSucceeded ? 1 : 0)
                      << std::endl;
            return true;
        }
        const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start).count();
        if (elapsedMs >= timeoutMs) {
            std::cout << "tilexr_ccu_direct_smoke collectiveSubmitDone"
                      << " rank=" << rank
                      << " localResult=" << localResult
                      << " allRanksDone=0"
                      << " timeoutMs=" << timeoutMs
                      << std::endl;
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
}

int RunPreparedSmokeForRank(DirectCcuSmokeContext* context, int rank, int rankSize, int device)
{
    TileXRDirectCcuPrepareOptions options = MakePrepareOptions(rank, rankSize, device);
    const int peer = rankSize == 2 ? 1 - rank : (rank + 1) % rankSize;
    const bool p2pCcuCopyEnabled = EnvFlag(kP2pCcuCopyEnv);
    P2pCcuCopyState p2pCcuCopy;
    if (p2pCcuCopyEnabled) {
        options.syncResourceCount = 1;
        options.sqeArgCount = 0;
        options.syncInstructionCount = 7;
        p2pCcuCopy.initRet = InitP2pCcuCopyState(rank, peer, &p2pCcuCopy);
    }
    PrintConfig(options, rankSize);

    TileXR::TileXRCcuDirectInstallAttempt attempt;
    TileXRDirectCcuPreparedTasksPtr prepared = &attempt;
    TileXRDirectCcuPrepareReport installReport;
    int ret = p2pCcuCopyEnabled && p2pCcuCopy.initRet != ACL_SUCCESS ?
        p2pCcuCopy.initRet :
        p2pCcuCopyEnabled ?
        context->planner.PrepareDirectCcuMemoryCopyInstallAttempt(
            context->session,
            options,
            reinterpret_cast<uint64_t>(p2pCcuCopy.source.ptr),
            reinterpret_cast<uint64_t>(p2pCcuCopy.destination.ptr),
            p2pCcuCopy.bytes,
            static_cast<uint32_t>(peer),
            TileXR::TileXRCcuMemoryCopyDirection::RemoteToLocal,
            prepared,
            &installReport) :
        context->planner.PrepareDirectCcuInstallAttempt(context->session, options, prepared, &installReport);
    PrintInstallReport("tilexr_ccu_direct_smoke prepare", ret, installReport);
    PrintPreparedTasks(prepared, installReport.submitTaskCount);
    PrintInstructionReadback(context, prepared, installReport.submitTaskCount);

    int finalRet = 0;
    const bool submitRequested = EnvFlag(kSubmitEnv);
    const bool collectiveSubmitReady = submitRequested ?
        WaitForCollectiveSubmitReadiness(
            rank,
            rankSize,
            ret == TileXR::TILEXR_SUCCESS && installReport.submitReady) :
        false;
    if (ret != TileXR::TILEXR_SUCCESS) {
        finalRet = 6;
    } else if (submitRequested && !collectiveSubmitReady && CollectiveSubmitReadyGateConfigured()) {
        std::cout << "tilexr_ccu_direct_smoke submit skipped reason=\"collective submitReady gate did not pass\""
                  << " localSubmitReady=" << (installReport.submitReady ? 1 : 0)
                  << std::endl;
    } else if (submitRequested && !installReport.submitReady) {
        std::cout << "tilexr_ccu_direct_smoke submit skipped reason=\"prepare did not reach submitReady\""
                  << std::endl;
    } else if (submitRequested) {
        aclrtStream stream = nullptr;
        int streamRet = aclrtCreateStream(&stream);
        if (streamRet != ACL_SUCCESS) {
            std::cerr << "tilexr_ccu_direct_smoke aclrtCreateStream ret=" << streamRet << std::endl;
            finalRet = 7;
        } else {
            const int delayRank = EnvInt(kDelayRankEnv, -1);
            const int preSubmitDelayMs = EnvInt(kPreSubmitDelayMsEnv, 0);
            const int effectiveDelayMs = rank == delayRank && preSubmitDelayMs > 0 ? preSubmitDelayMs : 0;
            if (effectiveDelayMs > 0) {
                std::cout << "tilexr_ccu_direct_smoke preSubmitDelay"
                          << " rank=" << rank
                          << " delayMs=" << effectiveDelayMs
                          << std::endl;
                std::this_thread::sleep_for(std::chrono::milliseconds(effectiveDelayMs));
            }

            TileXRDirectCcuSubmitReport submitReport;
            const auto submitBegin = std::chrono::steady_clock::now();
            const char* submitTaskSelector = std::getenv(kSubmitTaskSelectorEnv);
            const bool submitAllTasks = submitTaskSelector == nullptr || submitTaskSelector[0] == '\0' ||
                std::string(submitTaskSelector) == "all";
            const int submitRet = submitAllTasks ?
                TileXRDirectCcuSubmitPrepared(prepared, stream, &submitReport) :
                SubmitPreparedWithSelector(prepared, installReport.submitTaskCount, stream, &submitReport);
            const auto submitEnd = std::chrono::steady_clock::now();
            PrintSubmitReport("tilexr_ccu_direct_smoke submit", submitRet, submitReport);
            const auto syncBegin = std::chrono::steady_clock::now();
            TraceLifecycle("before aclrtSynchronizeStream");
            const int syncRet = aclrtSynchronizeStream(stream);
            TraceLifecycle("after aclrtSynchronizeStream");
            const auto syncEnd = std::chrono::steady_clock::now();
            PrintSubmitTiming(
                rank,
                effectiveDelayMs,
                submitRet,
                syncRet,
                std::chrono::duration_cast<std::chrono::milliseconds>(submitEnd - submitBegin).count(),
                std::chrono::duration_cast<std::chrono::milliseconds>(syncEnd - syncBegin).count());
            if (syncRet != ACL_SUCCESS) {
                std::cerr << "tilexr_ccu_direct_smoke aclrtSynchronizeStream ret=" << syncRet << std::endl;
                finalRet = 8;
            } else if (submitRet != TileXR::TILEXR_SUCCESS) {
                finalRet = 9;
            }
            if (p2pCcuCopyEnabled) {
                const int p2pCcuCopyRet = RunP2pCcuCopy(rank, peer, &p2pCcuCopy, ret, submitRet, syncRet);
                if (p2pCcuCopyRet != TileXR::TILEXR_SUCCESS && finalRet == 0) {
                    finalRet = 14;
                }
            }
            if (!WaitForCollectiveSubmitDone(rank, rankSize, finalRet) && finalRet == 0) {
                finalRet = 13;
            }
            TraceLifecycle("before aclrtDestroyStream");
            aclrtDestroyStream(stream);
            TraceLifecycle("after aclrtDestroyStream");
        }
    }

    if (prepared != nullptr && ShouldFastExitAfterRun()) {
        std::cout << "tilexr_ccu_direct_smoke fastExitAfterRun=1"
                  << " ret=" << finalRet
                  << " reason=\"skipping prepared-task cleanup to isolate cleanup hangs\""
                  << std::endl;
        std::fflush(stdout);
        std::fflush(stderr);
        std::_Exit(finalRet);
    }
    if (prepared != nullptr) {
        TraceLifecycle("before TileXRDirectCcuDestroyPrepared");
        const int preparedDestroyRet = TileXRDirectCcuDestroyPrepared(prepared);
        TraceLifecycle("after TileXRDirectCcuDestroyPrepared");
        if (preparedDestroyRet != TileXR::TILEXR_SUCCESS) {
            std::cerr << "tilexr_ccu_direct_smoke prepared destroy ret=" << preparedDestroyRet << std::endl;
            finalRet = finalRet == 0 ? 11 : finalRet;
        }
    }
    return finalRet;
}

int RunThreadModeSmoke(int rankSize)
{
    std::cout << "tilexr_ccu_direct_smoke threadMode begin"
              << " rankSize=" << rankSize
              << std::endl;
    std::vector<int32_t> devices(static_cast<size_t>(rankSize));
    for (int rank = 0; rank < rankSize; ++rank) {
        devices[rank] = DeviceFromEnv(rank);
    }
    std::vector<TileXRCommPtr> comms(static_cast<size_t>(rankSize), nullptr);
    int ret = TileXRCommInitAll(static_cast<uint32_t>(rankSize), devices.data(), comms.data());
    if (ret != TileXR::TILEXR_SUCCESS) {
        std::cerr << "tilexr_ccu_direct_smoke threadMode comm init ret=" << ret << std::endl;
        return 5;
    }

    std::vector<int> rankResults(static_cast<size_t>(rankSize), 0);
    std::vector<std::thread> rankThreads;
    rankThreads.reserve(static_cast<size_t>(rankSize));
    for (int rank = 0; rank < rankSize; ++rank) {
        rankThreads.emplace_back([&, rank]() {
            const int setDeviceRet = aclrtSetDevice(devices[rank]);
            if (setDeviceRet != ACL_SUCCESS) {
                std::cerr << "tilexr_ccu_direct_smoke threadMode aclrtSetDevice ret="
                          << setDeviceRet
                          << " rank=" << rank
                          << " device=" << devices[rank]
                          << std::endl;
                rankResults[rank] = 14;
                return;
            }
            DirectCcuSmokeContext context;
            const int initRet = InitCommForDirectCcuSmoke(0, rankSize, rank, devices[rank], &context);
            if (initRet != TileXR::TILEXR_SUCCESS) {
                std::cerr << "tilexr_ccu_direct_smoke threadMode direct CCU init ret="
                          << initRet
                          << " rank=" << rank
                          << " device=" << devices[rank]
                          << std::endl;
                rankResults[rank] = 5;
                return;
            }
            rankResults[rank] = RunPreparedSmokeForRank(&context, rank, rankSize, devices[rank]);
        });
    }
    for (auto& rankThread : rankThreads) {
        rankThread.join();
    }

    int finalRet = 0;
    for (int rank = 0; rank < rankSize; ++rank) {
        std::cout << "tilexr_ccu_direct_smoke threadMode rank=" << rank
                  << " ret=" << rankResults[rank]
                  << std::endl;
        if (rankResults[rank] != 0 && finalRet == 0) {
            finalRet = rankResults[rank];
        }
    }
    for (auto comm : comms) {
        if (comm == nullptr) {
            continue;
        }
        const int destroyRet = TileXRCommDestroy(comm);
        if (destroyRet != TileXR::TILEXR_SUCCESS) {
            std::cerr << "tilexr_ccu_direct_smoke threadMode destroy ret=" << destroyRet << std::endl;
            finalRet = finalRet == 0 ? 10 : finalRet;
        }
    }
    return finalRet;
}

} // namespace

int main()
{
    if (!EnvFlag(kEnableEnv)) {
        std::cout << "tilexr_ccu_direct_smoke skipped set "
                  << kEnableEnv << "=1 to run private C++ integration probe"
                  << std::endl;
        return 0;
    }

    const int rank = RankFromEnv();
    const int rankSize = RankSizeFromEnv();
    const int device = DeviceFromEnv(rank);
    const int commDomain = EnvInt("TILEXR_CCU_PROBE_COMM_DOMAIN", 0);

    if (rankSize <= 1) {
        std::cout << "tilexr_ccu_direct_smoke skipped rankSize=" << rankSize
                  << " reason=\"direct CCU prepare requires a multi-rank communicator\""
                  << std::endl;
        return 0;
    }
    if (rank < 0 || rank >= rankSize) {
        std::cerr << "tilexr_ccu_direct_smoke invalid rank=" << rank
                  << " rankSize=" << rankSize << std::endl;
        return 2;
    }
    if (EnvFlag(kThreadModeEnv)) {
        const int aclRet = aclInit(nullptr);
        if (aclRet != ACL_SUCCESS) {
            std::cerr << "tilexr_ccu_direct_smoke aclInit ret=" << aclRet << std::endl;
            return 3;
        }
        const int threadRet = RunThreadModeSmoke(rankSize);
        aclFinalize();
        return threadRet;
    }

    std::cout << "tilexr_ccu_direct_smoke begin"
              << " rank=" << rank
              << " rankSize=" << rankSize
              << " device=" << device
              << " commDomain=" << commDomain
              << std::endl;

    int ret = aclInit(nullptr);
    if (ret != ACL_SUCCESS) {
        std::cerr << "tilexr_ccu_direct_smoke aclInit ret=" << ret << std::endl;
        return 3;
    }

    ret = aclrtSetDevice(device);
    if (ret != ACL_SUCCESS) {
        std::cerr << "tilexr_ccu_direct_smoke aclrtSetDevice ret=" << ret
                  << " device=" << device << std::endl;
        aclFinalize();
        return 4;
    }

    DirectCcuSmokeContext context;
    ret = InitCommForDirectCcuSmoke(commDomain, rankSize, rank, device, &context);
    if (ret != TileXR::TILEXR_SUCCESS) {
        std::cerr << "tilexr_ccu_direct_smoke direct CCU context init ret=" << ret << std::endl;
        aclrtResetDevice(device);
        aclFinalize();
        return 5;
    }

    int finalRet = RunPreparedSmokeForRank(&context, rank, rankSize, device);
    if (ShouldFastExitAfterPrepareFailure(finalRet)) {
        std::cout << "tilexr_ccu_direct_smoke fastExitOnPrepareFailure=1"
                  << " ret=" << finalRet
                  << " reason=\"" << FastExitReasonForReturnCode(finalRet) << "\""
                  << std::endl;
        std::fflush(stdout);
        std::fflush(stderr);
        std::_Exit(finalRet);
    }
    if (ShouldFastExitAfterRun()) {
        std::cout << "tilexr_ccu_direct_smoke fastExitAfterRun=1"
                  << " ret=" << finalRet
                  << " reason=\"skipping communicator cleanup to isolate cleanup hangs\""
                  << std::endl;
        std::fflush(stdout);
        std::fflush(stderr);
        std::_Exit(finalRet);
    }
    TraceLifecycle("before DirectCcuSmokeContext shutdown");
    context.session.Shutdown();
    TraceLifecycle("after DirectCcuSmokeContext shutdown");
    TraceLifecycle("before aclrtResetDevice");
    aclrtResetDevice(device);
    TraceLifecycle("after aclrtResetDevice");
    TraceLifecycle("before aclFinalize");
    aclFinalize();
    TraceLifecycle("after aclFinalize");
    return finalRet;
}
