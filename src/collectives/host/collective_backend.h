/*
 * Copyright (c) 2024-2026 TileXR Project
 * This file is a part of the CANN Open Software.
 * Licensed under CANN Open Software License Agreement Version 1.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#ifndef TILEXR_COLLECTIVES_HOST_COLLECTIVE_BACKEND_H
#define TILEXR_COLLECTIVES_HOST_COLLECTIVE_BACKEND_H

#include <cstdint>

#include "acl/acl_base.h"
#include "tilexr_collectives.h"
#include "tilexr_types.h"

namespace TileXRCollectives {
namespace Host {

struct CollectiveRequest {
    TileXR::TileXRType type = TileXR::TileXRType::ALL_GATHER;
    void *sendBuf = nullptr;
    void *recvBuf = nullptr;
    int64_t count = 0;
    TileXR::TileXRDataType dataType = TileXR::TILEXR_DATA_TYPE_RESERVED;
    TileXR::TileXRReduceOp reduceOp = TileXR::TILEXR_REDUCE_RESERVED;
    int root = 0;
    TileXRCommPtr comm = nullptr;
    aclrtStream stream = nullptr;
};

struct BackendTestState {
    bool enabled = false;
    bool udmaInitialized = false;
    bool udmaSupported = false;
    int udmaReturn = TileXR::TILEXR_ERROR_NOT_SUPPORT;
    bool ccuInitialized = false;
    bool ccuSupported = false;
    int ccuReturn = TileXR::TILEXR_ERROR_NOT_SUPPORT;
    int aivReturn = TileXR::TILEXR_SUCCESS;
};

int DispatchCollective(const CollectiveRequest &request, TileXRCollectiveBackend backend);
void SetBackendTestState(const BackendTestState &state);
void ResetBackendTestState();

} // namespace Host
} // namespace TileXRCollectives

#endif // TILEXR_COLLECTIVES_HOST_COLLECTIVE_BACKEND_H
