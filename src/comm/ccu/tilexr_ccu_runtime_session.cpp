/*
 * Copyright (c) 2026 TileXR Project
 */
#include "ccu/tilexr_ccu_runtime_session.h"

#include "tilexr_log.h"
#include "tools/socket/tilexr_sock_exchange.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <map>
#include <mutex>
#include <new>
#include <thread>

using namespace std;
using namespace chrono;

namespace TileXR {

constexpr int TILEXR_INIT_TIMEOUT = 600;

struct TileXRThreadAllGatherState {
    std::vector<uint8_t> data[TILEXR_MAX_RANK_SIZE];
    uint64_t arrivals = 0;
    uint64_t departures = 0;
    size_t bytes = 0;
};
static map<string, TileXRThreadAllGatherState> g_directCcuAllGatherStates;
static std::mutex g_mtx;
static std::mutex g_ccuDirectRuntimeMtx;
static bool g_ccuDirectRuntimeUnavailable = false;
static std::string g_ccuDirectRuntimeUnavailableMessage;

std::string TileXRCcuRuntimeSession::ProcessDirectCcuRuntimeUnavailableMessage()
{
    lock_guard<mutex> lock(g_ccuDirectRuntimeMtx);
    if (!g_ccuDirectRuntimeUnavailable) {
        return {};
    }
    return g_ccuDirectRuntimeUnavailableMessage.empty() ?
        "direct CCU runtime unavailable after process-level init failure" :
        "direct CCU runtime unavailable after process-level init failure: " +
            g_ccuDirectRuntimeUnavailableMessage;
}

void TileXRCcuRuntimeSession::Shutdown()
{
    initialized_ = false;
    ResetDirectCcuBasicInfo();
    directCcuThreadAllGatherRound_ = 0;
    if (ccuDirectRuntime_ != nullptr) {
        ccuDirectRuntime_->Shutdown();
        ccuDirectRuntime_.reset();
    }
    options_ = TileXRCcuBackendOptions {};
    socketExchange_ = nullptr;
}

bool TileXRCcuRuntimeSession::Available() const
{
    return initialized_ && ccuDirectRuntime_ != nullptr && ccuDirectRuntime_->IsAvailable();
}

int TileXRCcuRuntimeSession::Rank() const
{
    return rank_;
}

int TileXRCcuRuntimeSession::RankSize() const
{
    return rankSize_;
}

int TileXRCcuRuntimeSession::Init(const TileXRCcuBackendOptions &options)
{
    Shutdown();
    options_ = options;
    rank_ = options.rank;
    rankSize_ = options.rankSize;
    devId_ = options.devId;
    uid_ = options.uid;
    socketExchange_ = options.exchange;
    if (rankSize_ <= 1) {
        TILEXR_LOG(INFO) << "direct CCU runtime skipped for single-rank communicator";
        return TILEXR_SUCCESS;
    }

    lock_guard<mutex> lock(g_ccuDirectRuntimeMtx);
    if (g_ccuDirectRuntimeUnavailable) {
        TILEXR_LOG(INFO) << "direct CCU runtime skipped after previous init failure";
        return TILEXR_SUCCESS;
    }

    ccuDirectRuntime_.reset(new (nothrow) TileXRCcuDirectRuntime());
    if (ccuDirectRuntime_ == nullptr) {
        TILEXR_LOG(WARN) << "TileXRCcuDirectRuntime allocation failed, direct CCU disabled";
        return TILEXR_SUCCESS;
    }

    TileXRCcuDirectRuntimeOptions runtimeOptions {};
    runtimeOptions.rank = rank_;
    runtimeOptions.rankSize = rankSize_;
    runtimeOptions.devId = devId_;
    runtimeOptions.allGather = &TileXRCcuRuntimeSession::DirectCcuAllGatherCallback;
    runtimeOptions.allGatherUserData = this;
    TileXRCcuDirectRuntimeReport runtimeReport;
    const int ret = ccuDirectRuntime_->Init(runtimeOptions, &runtimeReport);
    if (ret != TILEXR_SUCCESS || !ccuDirectRuntime_->IsAvailable()) {
        TILEXR_LOG(WARN) << "TileXR direct CCU runtime init failed: " << ret
                         << ", logicDevId " << runtimeReport.logicDevId
                         << ", devicePhyId " << runtimeReport.devicePhyId
                         << ", hdcType " << runtimeReport.hdcType
                         << ", raInitialized " << (runtimeReport.raInitialized ? 1 : 0)
                         << ", ccuTlvInitialized " << (runtimeReport.ccuTlvInitialized ? 1 : 0)
                         << ", " << runtimeReport.message << ", direct CCU disabled";
        g_ccuDirectRuntimeUnavailable = true;
        g_ccuDirectRuntimeUnavailableMessage = runtimeReport.message;
        ResetDirectCcuBasicInfo();
        ccuDirectRuntime_.reset();
        return TILEXR_SUCCESS;
    }

    const int ccuInfoRet = RefreshDirectCcuBasicInfo(0);
    if (ccuInfoRet != TILEXR_SUCCESS && ccuInfoRet != TILEXR_ERROR_NOT_FOUND) {
        TILEXR_LOG(WARN) << "direct CCU basic info refresh failed after runtime init: " << ccuInfoRet
                         << ", " << directCcuBasicInfoReport_.message;
    }

    TILEXR_LOG(INFO) << "InitDirectCcuRuntime success, rank " << rank_ << "/" << rankSize_
                     << " logicDevId " << runtimeReport.logicDevId
                     << " devicePhyId " << runtimeReport.devicePhyId
                     << " hdcType " << runtimeReport.hdcType
                     << " raInitialized " << (runtimeReport.raInitialized ? 1 : 0)
                     << " ccuTlvInitialized " << (runtimeReport.ccuTlvInitialized ? 1 : 0);
    initialized_ = true;
    return TILEXR_SUCCESS;
}

void TileXRCcuRuntimeSession::ResetDirectCcuBasicInfo()
{
    directCcuBasicInfoValid_ = false;
    directCcuBasicInfoStatus_ = TILEXR_ERROR_NOT_FOUND;
    directCcuBasicInfo_ = TileXRCcuBasicInfo {};
    directCcuBasicInfoReport_ = TileXRCcuDriverAdapterReport {};
}

int TileXRCcuRuntimeSession::RefreshDirectCcuBasicInfo(uint8_t dieId)
{
    ResetDirectCcuBasicInfo();
    if (ccuDirectRuntime_ == nullptr || !ccuDirectRuntime_->IsAvailable()) {
        directCcuBasicInfoReport_.message = "direct CCU runtime is unavailable for basic info";
        directCcuBasicInfoStatus_ = TILEXR_ERROR_NOT_FOUND;
        return directCcuBasicInfoStatus_;
    }

    TileXRCcuBasicInfo basicInfo;
    TileXRCcuDriverAdapterReport report;
    const int ret = ccuDirectRuntime_->QueryBasicInfo(dieId, &basicInfo, &report);
    directCcuBasicInfoReport_ = report;
    if (ret != TILEXR_SUCCESS) {
        directCcuBasicInfoStatus_ = ret;
        return directCcuBasicInfoStatus_;
    }

    directCcuBasicInfo_ = basicInfo;
    directCcuBasicInfoReport_.message = "direct CCU basic info cached";
    directCcuBasicInfoValid_ = true;
    directCcuBasicInfoStatus_ = TILEXR_SUCCESS;
    return TILEXR_SUCCESS;
}

bool TileXRCcuRuntimeSession::HasDirectCcuBasicInfo() const
{
    return directCcuBasicInfoValid_;
}

int TileXRCcuRuntimeSession::GetDirectCcuBasicInfoStatus() const
{
    return directCcuBasicInfoStatus_;
}

const TileXRCcuBasicInfo *TileXRCcuRuntimeSession::GetDirectCcuBasicInfo() const
{
    return directCcuBasicInfoValid_ ? &directCcuBasicInfo_ : nullptr;
}

const TileXRCcuDriverAdapterReport &TileXRCcuRuntimeSession::GetDirectCcuBasicInfoReport() const
{
    return directCcuBasicInfoReport_;
}

int TileXRCcuRuntimeSession::RegisterCcuResourceRmaBuffer(uint64_t resourceAddr)
{
    if (ccuDirectRuntime_ == nullptr || !ccuDirectRuntime_->IsAvailable()) {
        return TILEXR_ERROR_NOT_FOUND;
    }
    return ccuDirectRuntime_->RegisterCcuResourceRmaBuffer(resourceAddr);
}

int TileXRCcuRuntimeSession::ExportLocalCcuRmaBuffer(TileXRCcuLocalResourceWindowInfo *info)
{
    if (ccuDirectRuntime_ == nullptr || !ccuDirectRuntime_->IsAvailable()) {
        return TILEXR_ERROR_NOT_FOUND;
    }
    return ccuDirectRuntime_->ExportLocalCcuRmaBuffer(info);
}

int TileXRCcuRuntimeSession::ExportRemoteCcuRmaBuffers(std::vector<TileXRCcuRemoteCcuBufferInfo> *buffers)
{
    if (ccuDirectRuntime_ == nullptr || !ccuDirectRuntime_->IsAvailable()) {
        return TILEXR_ERROR_NOT_FOUND;
    }
    return ccuDirectRuntime_->ExportRemoteCcuRmaBuffers(buffers);
}

int TileXRCcuRuntimeSession::ExportLowerLayerTransportSnapshot(
    const TileXRCcuLowerLayerTransportSnapshot &templateSnapshot,
    TileXRCcuLowerLayerTransportSnapshot *snapshot)
{
    if (ccuDirectRuntime_ == nullptr || !ccuDirectRuntime_->IsAvailable()) {
        return TILEXR_ERROR_NOT_FOUND;
    }
    return ccuDirectRuntime_->ExportLowerLayerTransportSnapshot(templateSnapshot, snapshot);
}

int TileXRCcuRuntimeSession::ConfigureLocalVerifiedEndpointRoute(const TileXRCcuLowerLayerTransportRoute &route)
{
    if (ccuDirectRuntime_ == nullptr || !ccuDirectRuntime_->IsAvailable()) {
        return TILEXR_ERROR_NOT_FOUND;
    }
    return ccuDirectRuntime_->ConfigureLocalVerifiedEndpointRoute(route);
}

int TileXRCcuRuntimeSession::RefreshLocalVerifiedEndpointRoute(TileXRCcuDirectRuntimeReport *report)
{
    if (ccuDirectRuntime_ == nullptr || !ccuDirectRuntime_->IsAvailable()) {
        return TILEXR_ERROR_NOT_FOUND;
    }
    return ccuDirectRuntime_->RefreshLocalVerifiedEndpointRoute(report);
}

int TileXRCcuRuntimeSession::CreateDriverAdapter(
    TileXRCcuDriverAdapter *adapter,
    TileXRCcuDriverAdapterReport *report)
{
    if (ccuDirectRuntime_ == nullptr || !ccuDirectRuntime_->IsAvailable()) {
        if (report != nullptr) {
            report->message = "direct CCU runtime is unavailable";
        }
        return TILEXR_ERROR_NOT_FOUND;
    }
    return ccuDirectRuntime_->CreateDriverAdapter(adapter, report);
}

int TileXRCcuRuntimeSession::AllGather(const void *sendBuf, size_t sendBytes, void *recvBuf)
{
    return DirectCcuAllGatherCallback(sendBuf, sendBytes, recvBuf, this);
}

int TileXRCcuRuntimeSession::DirectCcuAllGatherCallback(
    const void *sendBuf,
    size_t sendBytes,
    void *recvBuf,
    void *userData)
{
    auto *session = static_cast<TileXRCcuRuntimeSession *>(userData);
    if (session == nullptr || sendBuf == nullptr || recvBuf == nullptr || sendBytes == 0) {
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    if (session->socketExchange_ == nullptr) {
        return session->DirectCcuThreadAllGather(sendBuf, sendBytes, recvBuf);
    }
    return session->socketExchange_->AllGather(
        static_cast<const uint8_t *>(sendBuf),
        sendBytes,
        static_cast<uint8_t *>(recvBuf));
}

int TileXRCcuRuntimeSession::DirectCcuThreadAllGather(const void *sendBuf, size_t sendBytes, void *recvBuf)
{
    if (sendBuf == nullptr || recvBuf == nullptr || sendBytes == 0 || rank_ < 0 ||
        rank_ >= rankSize_ || rankSize_ <= 0 || uid_.empty()) {
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }

    const uint64_t round = directCcuThreadAllGatherRound_++;
    const std::string key = uid_ + ":ccu:" + std::to_string(round);
    auto start = high_resolution_clock::now();
    for (;;) {
        {
            lock_guard<mutex> lock(g_mtx);
            auto &state = g_directCcuAllGatherStates[key];
            if (state.bytes == 0) {
                state.bytes = sendBytes;
            } else if (state.bytes != sendBytes) {
                g_directCcuAllGatherStates.erase(key);
                return TILEXR_ERROR_PARA_CHECK_FAIL;
            }
            if (state.data[rank_].empty()) {
                state.data[rank_].resize(sendBytes);
                std::memcpy(state.data[rank_].data(), sendBuf, sendBytes);
                ++state.arrivals;
            }
            if (state.arrivals == static_cast<uint64_t>(rankSize_)) {
                auto *output = static_cast<uint8_t *>(recvBuf);
                for (int i = 0; i < rankSize_; ++i) {
                    std::memcpy(output + static_cast<size_t>(i) * sendBytes, state.data[i].data(), sendBytes);
                }
                ++state.departures;
                if (state.departures == static_cast<uint64_t>(rankSize_)) {
                    g_directCcuAllGatherStates.erase(key);
                }
                return TILEXR_SUCCESS;
            }
        }
        const std::string processUnavailableMessage = ProcessDirectCcuRuntimeUnavailableMessage();
        if (!processUnavailableMessage.empty()) {
            lock_guard<mutex> lock(g_mtx);
            g_directCcuAllGatherStates.erase(key);
            TILEXR_LOG(ERROR) << "direct CCU thread allgather abort rank " << rank_ << "/" << rankSize_
                              << " uid " << uid_ << " round " << round << ", "
                              << processUnavailableMessage;
            return TILEXR_ERROR_NOT_FOUND;
        }
        this_thread::sleep_for(1ms);
        auto elapsed = duration_cast<seconds>(high_resolution_clock::now() - start);
        if (elapsed.count() > TILEXR_INIT_TIMEOUT) {
            lock_guard<mutex> lock(g_mtx);
            g_directCcuAllGatherStates.erase(key);
            TILEXR_LOG(ERROR) << "direct CCU thread allgather timeout rank " << rank_ << "/" << rankSize_
                              << " uid " << uid_ << " round " << round;
            return TILEXR_ERROR_TIMEOUT;
        }
    }
}

} // namespace TileXR
