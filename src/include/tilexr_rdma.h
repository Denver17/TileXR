/* Copyright (c) 2026 TileXR Project. Licensed under the Apache License, Version 2.0. */

/*
 * 功能：声明 RDMA 的 QP 配置、内存注册、Host 传输和 AIV view 查询接口。
 * 调用关系：由 tilexr_api.h 引入；tilexr_rdma_api.cpp 将请求转给 communicator 的 transport。
 * 资源与生命周期：注册不转移 payload buffer 所有权；调用方须停止本地和远端使用，再注销或关闭 transport。
 *
 * Purpose: Declare RDMA QP configuration, memory registration, Host transfers and AIV view queries.
 * Calls: Included by tilexr_api.h; tilexr_rdma_api.cpp forwards requests to the communicator's transport.
 * Lifetime: Registration retains caller ownership of payload buffers; quiesce local and remote users before deregistration or shutdown.
 */
#ifndef TILEXR_RDMA_H
#define TILEXR_RDMA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "tilexr_rdma_device_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Independent of the device CommArgs ABI. Include alongside tilexr_api.h. */
typedef uint64_t TileXRRDMAMemHandle;
typedef struct {
    uint32_t version;
    uint32_t connected;
    uint32_t hostDriven;
    uint32_t deviceDirect; /* Explicit, supported AIV NDA mode; excludes Host submission. */
    uint32_t peerCount;
    uint32_t registrationCount;
    uint32_t failed;       /* WR or registration rollback failed: destroy before freeing memory. */
    uint32_t reserved;
} TileXRRDMAInfo;

int TileXRRDMAQuery(void *comm, TileXRRDMAInfo *info);
/* Collective AIV queue setup, before registering memory. All ranks request the
 * same count in [1, TILEXR_RDMA_MAX_QPS]. Default is 1; only growth is supported.
 * No active registrations may exist. Partial setup failure requires collective
 * Shutdown/communicator destruction; queues are never silently shared or reset. */
int TileXRRDMAConfigureQps(void *comm, uint32_t qpCount);
/* Collective. Each rank registers its own original allocation, never an IPC alias.
 * Host RAM requires TILEXR_RDMA_DMA_MODE=default. Device HBM requires a provider
 * that supports that allocation; UBNIC device segments use indep_ub.
 * Host calls are serialized. InitThread/InitAll do not support RDMA.
 * On failure, handle is 0. Query failed: if set, rollback retained resources;
 * collective TileXRCommDestroy must succeed before freeing the allocation.
 * A broken bootstrap poisons the transport and retains MRs until process exit;
 * Destroy then closes local queues but cannot confirm collective MR release.
 * Synchronize producing NPU streams before Put, and consuming streams after Get. */
int TileXRRDMARegister(void *comm, void *localPtr, size_t bytes, TileXRRDMAMemHandle *handle);
/* Collective. Caller quiesces all local and remote users before unregister/destroy.
 * Failed cleanup retains the handle for retry. */
int TileXRRDMAUnregister(void *comm, TileXRRDMAMemHandle handle);
/* Collective transport teardown, including poisoned device queues. All AIV and
 * remote users must be quiescent. On success every RDMA handle/view on this
 * communicator is invalidated and retained payload allocations may be freed.
 * On failure retain allocations and retry; a broken bootstrap requires process
 * exit. Other communicator transports remain available. */
int TileXRRDMAShutdown(void *comm);
/* Synchronous, Host-driven RDMA using the same region handle at both ends.
 * Offsets are bytes; peer must differ from the local rank. No CPU payload staging.
 * The destination must not be consumed until the application communicates completion.
 * A timeout does not imply the WR stopped using memory: destroy before freeing it. */
int TileXRRDMAPut(void *comm, TileXRRDMAMemHandle handle, int peer,
    size_t localOffset, size_t remoteOffset, size_t bytes);
int TileXRRDMAGet(void *comm, TileXRRDMAMemHandle handle, int peer,
    size_t localOffset, size_t remoteOffset, size_t bytes);

/* Host-side descriptor for kernel RDMA. Copy the returned POD to device memory.
 * Requires TILEXR_RDMA_MODE=aiv and a supported NDA provider. No Host fallback.
 * A QP has one AIV owner across ALL views/regions/streams. Complete each operation
 * before the next; synchronize streams and remote consumers before unregister or
 * destroy. Views expire at unregister/destroy and must never be used afterwards.
 * Queue counters are transport-owned and shared by every region's view. */
/* V2 grows the output POD. Version the C symbol as well so loading an old
 * Host binary cannot write a 152-byte view into a V1 144-byte allocation.
 * The source-level spelling remains available; rebuild Host and kernel together. */
#define TileXRRDMAGetDeviceView TileXRRDMAGetDeviceViewV2
int TileXRRDMAGetDeviceViewV2(void *comm, TileXRRDMAMemHandle handle, int peer,
    TileXRRDMADeviceView *view);
/* Host output array, exactly qpCount elements matching ConfigureQps. Every view
 * shares this registration's memory but owns an independent QP/state. Copy the
 * array to device memory and assign one AIV owner per index. Partition remote
 * offsets explicitly; there is no ordering across QPs. Failure clears the output
 * when qpCount is in the supported range. The V2 single-view API selects QP0. */
int TileXRRDMAGetDeviceViews(void *comm, TileXRRDMAMemHandle handle, int peer,
    TileXRRDMADeviceView *views, uint32_t qpCount);

#ifdef __cplusplus
}
#endif
#endif
