/*
 * Copyright (c) 2026 TileXR Project
 */
#ifndef TILEXR_CCU_BACKEND_H
#define TILEXR_CCU_BACKEND_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

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
#ifdef TILEXR_CCU_TESTING
    bool RuntimeInitializedForTest() const;
#endif

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace TileXR

#endif // TILEXR_CCU_BACKEND_H
