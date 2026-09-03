/*
 * Copyright (c) 2024 Huawei Technologies Co., Ltd.
 * This file is a part of the CANN Open Software.
 * Licensed under CANN Open Software License Agreement Version 1.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#ifndef TILEXR_INTERNAL_H
#define TILEXR_INTERNAL_H

#include <cstdint>
#include "../include/tilexr_api.h"
#include "../include/tilexr_types.h"

using TileXRInternalResourceDestroyFn = int (*)(TileXRCommPtr, void*);

int TileXRCommGetInternalResource(TileXRCommPtr comm, void **resource);
int TileXRCommAttachInternalResource(TileXRCommPtr comm, void *resource,
    TileXRInternalResourceDestroyFn destroy);
int TileXRCommDetachInternalResource(TileXRCommPtr comm, void *resource);
int TileXRCommAgreeInternalStatus(TileXRCommPtr comm, int localStatus);
int TileXRCommHasA5GroupedQpProfile(TileXRCommPtr comm, bool *available);
int TileXRCommLockInternalResource(TileXRCommPtr comm);
int TileXRCommUnlockInternalResource(TileXRCommPtr comm);

namespace TileXR {
enum class IpcPidMode {
    AUTO,
    PID_ONLY,
    SUPERPOD_SDID,
    INVALID
};

// Common functions
ChipName GetChipName();

bool UseLegacyIpcPid(ChipName chipName);

IpcPidMode ResolveIpcPidMode(const char *value);

bool UsePidOnlyIpcForPeer(ChipName chipName, int rank, int peer, int localRankSize,
    IpcPidMode mode);

uint32_t GetCoreNum(ChipName chipName);
} // namespace TileXR
#endif // TILEXR_INTERNAL_H
