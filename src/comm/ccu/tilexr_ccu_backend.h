/*
 * Copyright (c) 2026 TileXR Project
 */
#ifndef TILEXR_CCU_BACKEND_H
#define TILEXR_CCU_BACKEND_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "ccu/tilexr_ccu_direct_orchestrator.h"
#include "acl/acl_base.h"
#include "tilexr_types.h"

namespace TileXR {

class TileXRSockExchange;
class TileXRCcuRuntimeSession;
class TileXRCcuCollectivePlanner;
class TileXRCcuExecutor;

struct TileXRCcuBackendOptions {
    int rank = 0;
    int rankSize = 0;
    int devId = 0;
    std::string uid;
    TileXRSockExchange *exchange = nullptr;
};

struct TileXRCcuCollectiveRequest {
    TileXRType type = TileXRType::ALL_GATHER;
    void *sendBuf = nullptr;
    void *recvBuf = nullptr;
    int64_t count = 0;
    TileXRDataType dataType = TILEXR_DATA_TYPE_RESERVED;
    TileXRReduceOp reduceOp = TILEXR_REDUCE_RESERVED;
    int root = 0;
    aclrtStream stream = nullptr;
};

struct TileXRCcuCollectivePlan {
    bool ready = false;
};

enum class TileXRCcuSignalWaitRole {
    Signal = 0,
    Wait = 1,
    SignalAndWait = 2,
};

struct TileXRCcuSignalWaitRequest {
    int peerRank = -1;
    TileXRCcuSignalWaitRole role = TileXRCcuSignalWaitRole::Signal;
    uint32_t syncInstructionCount = 0;
    uint16_t missionStartId = 0;
    uint16_t instructionStartId = 0;
    uint16_t missionInstructionStartId = 0;
    uint16_t xnStartId = 0;
    uint16_t remoteXnStartId = 0;
    uint16_t remoteXnCount = 0;
    uint16_t ckeStartId = 0;
    uint16_t channelStartId = 0;
    uint16_t localWaitCkeStartId = 0;
    uint16_t localWaitCkeCount = 0;
    uint16_t remoteNotifyCkeStartId = 0;
    uint16_t remoteNotifyCkeCount = 0;
    uint16_t timeout = 0;
    std::string provider;
};

struct TileXRCcuSignalWaitPlan {
    bool ready = false;
    TileXRCcuDirectInstallAttempt attempt;
    std::vector<TileXRCcuTask> submitTasks;
};

class TileXRCcuBackend {
public:
    TileXRCcuBackend();
    ~TileXRCcuBackend();

    TileXRCcuBackend(const TileXRCcuBackend&) = delete;
    TileXRCcuBackend& operator=(const TileXRCcuBackend&) = delete;

    int Init(const TileXRCcuBackendOptions &options);
    void Shutdown();
    bool Available() const;
    bool Supports(const TileXRCcuCollectiveRequest &request) const;
    int PrepareCollective(const TileXRCcuCollectiveRequest &request, TileXRCcuCollectivePlan *plan);
    int SubmitCollective(const TileXRCcuCollectivePlan &plan, aclrtStream stream);
    int PrepareSignalWait(const TileXRCcuSignalWaitRequest &request, TileXRCcuSignalWaitPlan *plan);
    int SubmitSignalWait(
        const TileXRCcuSignalWaitPlan &plan,
        aclrtStream stream,
        TileXRCcuDirectSubmitReport *report);
#ifdef TILEXR_CCU_TESTING
    bool RuntimeInitializedForTest() const;
#endif

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace TileXR

#endif // TILEXR_CCU_BACKEND_H
