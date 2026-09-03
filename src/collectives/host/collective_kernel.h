/*
 * Copyright (c) 2024-2026 TileXR Project
 * This file is a part of the CANN Open Software.
 * Licensed under CANN Open Software License Agreement Version 1.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#ifndef TILEXR_COLLECTIVES_HOST_COLLECTIVE_KERNEL_H
#define TILEXR_COLLECTIVES_HOST_COLLECTIVE_KERNEL_H

#include <cstdint>

#include "acl/acl_base.h"
#include "collective_launcher.h"
#include "tilexr_types.h"

namespace TileXRCollectives {
namespace Host {

struct AscendCCLKernelArgs {
    const void *input = nullptr;
    const void *output = nullptr;
    const void *commArgsPtr = nullptr;
    int64_t count = 0;
    int64_t magic = 0;
    int op = 0;
    int root = 0;
    int cycleCount = 0;
    const void *scale = nullptr;
    int64_t scaleCount = 0;
    const void *offset = nullptr;
};

struct CollectiveLaunchAttrs {
    int op = 0;
    int root = 0;
};

int LaunchCollectiveKernel(TileXRCommPtr comm, TileXR::TileXRType type, const HostLaunchContext &context,
                           void *sendBuf, void *recvBuf, int64_t kernelCount,
                           TileXR::TileXRDataType dataType, uint32_t blockDim,
                           aclrtStream stream,
                           CollectiveLaunchAttrs attrs = CollectiveLaunchAttrs {});

} // namespace Host
} // namespace TileXRCollectives

#endif // TILEXR_COLLECTIVES_HOST_COLLECTIVE_KERNEL_H
