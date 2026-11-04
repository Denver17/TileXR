/*
 * Copyright (c) 2024 Huawei Technologies Co., Ltd.
 * This file is a part of the CANN Open Software.
 * Licensed under CANN Open Software License Agreement Version 1.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */

/*
 * 功能：声明 TileXR Host 公共接口，涵盖 communicator 生命周期、CommArgs 获取、
 * magic 轮次推进，以及 UDMA/SDMA 能力与资源访问。
 * 关系：应用和 collectives/EP Host 层通过本接口使用核心运行时；
 * src/comm/comm_wrap.cpp 将入口转发给 TileXRComm。comm_args.h 定义传给
 * Device kernel 的共享参数；tilexr_udma_fullmesh.h 与 tilexr_udma_reg.h
 * 提供 UDMA 接口类型。TileXRCommPtr 是运行时对象的不透明句柄。
 *
 * Purpose: Declare public TileXR Host interfaces for communicator lifecycle,
 * CommArgs access, magic-round advancement, and UDMA/SDMA capabilities/resources.
 * Relationships: Applications and collectives/EP Host code use this runtime API;
 * src/comm/comm_wrap.cpp forwards entry points to TileXRComm. comm_args.h defines
 * shared arguments passed to Device kernels; tilexr_udma_fullmesh.h and
 * tilexr_udma_reg.h supply UDMA API types. TileXRCommPtr is an opaque runtime handle.
 */
#ifndef TILEXR_API_H
#define TILEXR_API_H

#ifdef __cplusplus
#include <cstddef>
#include <cstdint>
#include "comm_args.h"
#include "tilexr_udma_fullmesh.h"
#include "tilexr_udma_reg.h"
#else
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
typedef uint8_t *GM_ADDR;
#endif

#ifdef __cplusplus
extern "C" {
#endif // __cplusplus

typedef void *TileXRCommPtr;
typedef uint32_t TileXRUDMAMemHandle;
typedef uint32_t TileXRUDMAProfileHandle;
#define TILEXRUNIQUE_ID_BYTES 128
typedef struct { char internal[TILEXRUNIQUE_ID_BYTES]; } TileXRUniqueId;

int TileXRGetUniqueId(TileXRUniqueId *uniqueId, int commDomain);

int TileXRCommInitRankLocal(int rankSize, int rank, TileXRCommPtr *comm);

int TileXRCommInitRank(TileXRUniqueId commId, int rankSize, int rank, TileXRCommPtr *comm);

int TileXRCommInitRankWithCustDomainSize(int commDomain, int bufferSize, int rankSize, int rank, TileXRCommPtr *comm);

int TileXRCommInitRankWithDomain(int commDomain, int rankSize, int rank, TileXRCommPtr *comm);

// Use a commDomain distinct from every concurrently active non-shared communicator.
int TileXRCommInitRankWithSharedQpDomain(int commDomain, int rankSize, int rank, TileXRCommPtr *comm);

// Creates an isolated peer-memory domain without Credit, UDMA, Fullmesh, or SDMA resources.
int TileXRCommInitRankMemoryDomain(int commDomain, int rankSize, int rank, TileXRCommPtr *comm);

// bufferSize is the data capacity in MiB (>=1), excluding the 4 MiB flag area.
// All ranks in this domain must use the same size. The default API above stays at 8 MiB.
int TileXRCommInitRankMemoryDomainWithSize(
    int commDomain, int bufferSize, int rankSize, int rank, TileXRCommPtr *comm);

#ifdef __cplusplus
int TileXRGetCommArgsDev(TileXRCommPtr comm, GM_ADDR &commArgsPtr);

int TileXRGetCommArgsHost(TileXRCommPtr comm, TileXR::CommArgs *&commArgsPtr);
#endif

int TileXRCommNextMagic(TileXRCommPtr comm, int64_t *magic);

// EP compatibility APIs for ep_profiling_compat.h writers only; independent
// of the standalone TileXR::Profiling SDK and its DeviceView lifecycle.
// Reserve disjoint per-core slots for an ordered profiling launch. The budget
// must be a nonzero multiple of 8. UINT32_MAX means disabled/exhausted; this is
// success, and the operator should still launch. Exhaustion marks export as
// incomplete. All launches sharing a capture must be ordered by the caller.
// Every direct writer in the default capture region must use this allocator;
// do not mix it with append ProfilingRecorder writers.
int TileXRProfilingReserveLaunch(TileXRCommPtr comm, uint32_t recordsPerLaunch,
    uint32_t *launchSlot);

// Reuse slot zero for every ordered launch, retaining each core's final launch
// only. Pair exclusively with DirectProfilingWriter::InitializeLatest. Keep the
// launch budget and logical core set fixed for that capture region; do not
// mix with ReserveLaunch or append ProfilingRecorder. The two
// reservation APIs reject mixing; legacy writers cannot be detected by Host.
// Budget is a nonzero multiple of 8. UINT32_MAX means disabled/insufficient GM
// capacity, with success so communication can proceed. Invalid/mixed requests
// return a parameter error and UINT32_MAX. Capacity/mode failures mark capture
// incomplete. Reserve at least this budget in TILEXR_PROFILING_RECORDS_PER_CORE;
// total allocation and record processing need not scale with launch count.
int TileXRProfilingReserveLatestLaunch(TileXRCommPtr comm, uint32_t recordsPerLaunch,
    uint32_t *launchSlot);

// Independent Combine region with the same latest-launch reservation contract.
// Enable it with TILEXR_PROFILING_COMBINE_RECORDS_PATH and pair exclusively with
// DirectProfilingWriter::InitializeLatestCombine. Its width, counters and failure
// state are independent of the default region. Both regions share capture UUID
// and per-core capacity configuration, but must have different output paths.
int TileXRProfilingReserveLatestCombineLaunch(TileXRCommPtr comm, uint32_t recordsPerLaunch,
    uint32_t *launchSlot);

int TileXRUDMARegister(TileXRCommPtr comm, GM_ADDR localPtr, size_t bytes, TileXRUDMAMemHandle *handle);

int TileXRUDMAUnregister(TileXRCommPtr comm, TileXRUDMAMemHandle handle);

int TileXRUDMAProfileRegister(TileXRCommPtr comm, const TileXR::TileXRUDMAProfileDesc *desc,
    TileXRUDMAProfileHandle *handle);

int TileXRUDMAProfileUnregister(TileXRCommPtr comm, TileXRUDMAProfileHandle handle);

int TileXRUDMAProfileQuery(TileXRCommPtr comm, TileXRUDMAProfileHandle handle,
    TileXR::TileXRUDMAProfileView *view);

int TileXRUDMAGetQpCount(TileXRCommPtr comm, uint32_t *qpCount);

int TileXRUDMAFullmeshQuery(TileXRCommPtr comm,
    TileXR::TileXRUDMAFullmeshHostView *view);

#ifdef __cplusplus
int TileXRGetUDMARegistryDev(TileXRCommPtr comm, GM_ADDR &registryPtr);

int TileXRGetUDMARegistryHost(TileXRCommPtr comm, const TileXR::TileXRUDMARegistry **registry);
#endif

int TileXRSDMAAvailable(TileXRCommPtr comm, bool *available);

int TileXRGetSDMAWorkspaceDev(TileXRCommPtr comm, GM_ADDR *workspace);

void TileXRPrintDFX2Log(TileXRCommPtr comm);

int TileXRCommInit(int rank, int rankSize, TileXRCommPtr *comms);

int TileXRCommInitAll(uint32_t ndev, int32_t* devices, TileXRCommPtr *comms);

int TileXRCommInitThread(int rank, int rankSize, const char *uid, TileXRCommPtr *comms);

int TileXRCommDestroy(TileXRCommPtr comm);

#ifdef __cplusplus
}
#endif // __cplusplus
#endif // TILEXR_API_H
