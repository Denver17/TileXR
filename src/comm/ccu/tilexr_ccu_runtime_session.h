/*
 * Copyright (c) 2026 TileXR Project
 */
#ifndef TILEXR_CCU_RUNTIME_SESSION_H
#define TILEXR_CCU_RUNTIME_SESSION_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "ccu/tilexr_ccu_backend.h"
#include "ccu/tilexr_ccu_direct_runtime.h"

namespace TileXR {

class TileXRCcuRuntimeSession {
public:
    int Init(const TileXRCcuBackendOptions &options);
    void Shutdown();
    bool Available() const;

    int Rank() const;
    int RankSize() const;
    int AllGather(const void *sendBuf, size_t sendBytes, void *recvBuf);

    int RefreshDirectCcuBasicInfo(uint8_t dieId = 0);
    bool HasDirectCcuBasicInfo() const;
    int GetDirectCcuBasicInfoStatus() const;
    const TileXRCcuBasicInfo *GetDirectCcuBasicInfo() const;
    const TileXRCcuDriverAdapterReport &GetDirectCcuBasicInfoReport() const;

    int RegisterCcuResourceRmaBuffer(uint64_t resourceAddr);
    int ExportLocalCcuRmaBuffer(TileXRCcuLocalResourceWindowInfo *info);
    int ExportRemoteCcuRmaBuffers(std::vector<TileXRCcuRemoteCcuBufferInfo> *buffers);
    int ExportLowerLayerTransportSnapshot(
        const TileXRCcuLowerLayerTransportSnapshot &templateSnapshot,
        TileXRCcuLowerLayerTransportSnapshot *snapshot);
    int ConfigureLocalVerifiedEndpointRoute(const TileXRCcuLowerLayerTransportRoute &route);
    int RefreshLocalVerifiedEndpointRoute(TileXRCcuDirectRuntimeReport *report);
    int CreateDriverAdapter(TileXRCcuDriverAdapter *adapter, TileXRCcuDriverAdapterReport *report);

    static std::string ProcessDirectCcuRuntimeUnavailableMessage();

private:
    void ResetDirectCcuBasicInfo();
    static int DirectCcuAllGatherCallback(const void *sendBuf, size_t sendBytes, void *recvBuf, void *userData);
    int DirectCcuThreadAllGather(const void *sendBuf, size_t sendBytes, void *recvBuf);

    TileXRCcuBackendOptions options_ = {};
    int rank_ = 0;
    int rankSize_ = 0;
    int devId_ = 0;
    std::string uid_ = {};
    TileXRSockExchange *socketExchange_ = nullptr;
    bool initialized_ = false;
    std::unique_ptr<TileXRCcuDirectRuntime> ccuDirectRuntime_;
    bool directCcuBasicInfoValid_ = false;
    int directCcuBasicInfoStatus_ = TILEXR_ERROR_NOT_FOUND;
    TileXRCcuBasicInfo directCcuBasicInfo_ = {};
    TileXRCcuDriverAdapterReport directCcuBasicInfoReport_ = {};
    uint64_t directCcuThreadAllGatherRound_ = 0;
};

} // namespace TileXR

#endif // TILEXR_CCU_RUNTIME_SESSION_H
