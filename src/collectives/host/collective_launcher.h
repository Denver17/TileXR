/*
 * Copyright (c) 2024-2026 TileXR Project
 * This file is a part of the CANN Open Software.
 * Licensed under CANN Open Software License Agreement Version 1.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
/* 声明 collective launcher 和可选 profiling 采集入口，供 collective_kernel.cpp 传递 communicator、stream 与执行参数。
 * Declare collective launch and optional profiling entry points consumed by collective_kernel.cpp with communicator, stream and execution arguments.
 */

#ifndef TILEXR_COLLECTIVES_HOST_COLLECTIVE_LAUNCHER_H
#define TILEXR_COLLECTIVES_HOST_COLLECTIVE_LAUNCHER_H

#include <cstdint>

#include "acl/acl_base.h"
#include "tilexr_api.h"
#include "tilexr_types.h"

namespace TileXRCollectives {
namespace Host {

struct HostLaunchContext {
    TileXR::CommArgs *hostArgs = nullptr;
    GM_ADDR devArgs = nullptr;
    int64_t magic = 0;
};

// Caller holds the communicator resource lock through kernel enqueue.
void *AcquireCollectiveProfiling(TileXRCommPtr comm, const HostLaunchContext &context,
    aclrtStream stream, uint32_t lanes, TileXR::TileXRType type,
    int64_t kernelCount, TileXR::TileXRDataType dataType, int reduction, int root);

int PrepareHostLaunchContext(TileXRCommPtr comm, HostLaunchContext &context);

bool IsA5AllToAllCandidate(const HostLaunchContext &context,
    TileXR::TileXRDataType dataType);

int LaunchA5AllToAll(TileXRCommPtr comm, const HostLaunchContext &context,
    void *sendBuf, void *recvBuf, int64_t sendCount, aclrtStream stream);

} // namespace Host
} // namespace TileXRCollectives

#endif // TILEXR_COLLECTIVES_HOST_COLLECTIVE_LAUNCHER_H
