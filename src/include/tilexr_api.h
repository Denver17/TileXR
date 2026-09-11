/*
 * Copyright (c) 2024 Huawei Technologies Co., Ltd.
 * This file is a part of the CANN Open Software.
 * Licensed under CANN Open Software License Agreement Version 1.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
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
