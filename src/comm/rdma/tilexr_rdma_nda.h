/* Copyright (c) 2026 TileXR Project. Licensed under the Apache License, Version 2.0. */

/*
 * 功能：声明 NDA queue 创建、device queue 导出及显式 cleanup 的接口。
 * 调用关系：VerbsProvider 使用此接口，tilexr_rdma_nda.cpp 实现扩展调用和内存回调。
 * 资源与生命周期：RdmaNdaResources 持有扩展资源；返回的 plain QP/CQ 是借用引用，释放由该对象执行。
 *
 * Purpose: Declare NDA queue creation, device queue export and explicit cleanup interfaces.
 * Calls: Used by VerbsProvider and implemented in tilexr_rdma_nda.cpp, including extension memory callbacks.
 * Lifetime: RdmaNdaResources owns extension resources; returned plain QP/CQ handles are borrowed and released by that owner.
 */
#ifndef TILEXR_RDMA_NDA_H
#define TILEXR_RDMA_NDA_H

#include <cstdint>
#include <memory>
#include "tilexr_rdma_device_types.h"

struct ibv_context;
struct ibv_pd;
struct ibv_qp;
struct ibv_cq;

namespace TileXR {
struct RdmaConfig;

// Owns extension handles, callback allocations and mappings. The plain verbs
// QP/CQ returned by Create are borrowed aliases; only this object destroys them.
// Calls are serialized by the transport. The calling thread must have the
// configured ACL device selected, including during retryable teardown.
class RdmaNdaResources {
public:
    RdmaNdaResources();
    ~RdmaNdaResources();
    RdmaNdaResources(const RdmaNdaResources&) = delete;
    RdmaNdaResources& operator=(const RdmaNdaResources&) = delete;
    int Open(ibv_context* context, const RdmaConfig& config);
    bool Active() const;
    int Create(int peer, ibv_pd* pd, ibv_qp*& qp, ibv_cq*& cq);
    int DeviceQueue(int peer, uint32_t gidIndex, uint32_t mtuEnum,
        TileXRRDMADeviceQueue& queue) const;
    int DestroyQueues();
    int Close();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace TileXR
#endif
