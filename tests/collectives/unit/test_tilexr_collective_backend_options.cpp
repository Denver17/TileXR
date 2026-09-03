/*
 * Copyright (c) 2024-2026 TileXR Project
 * This file is a part of the CANN Open Software.
 * Licensed under CANN Open Software License Agreement Version 1.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#include "tilexr_collectives.h"
#include "collective_backend.h"

#include <cstdint>

namespace {

static_assert(TILEXR_COLLECTIVE_BACKEND_AUTO == 0, "AUTO must be zero for zero-initialized options");
static_assert(TILEXR_COLLECTIVE_BACKEND_AIV == 1, "AIV enum value changed");
static_assert(TILEXR_COLLECTIVE_BACKEND_UDMA == 2, "UDMA enum value changed");
static_assert(TILEXR_COLLECTIVE_BACKEND_CCU == 3, "CCU enum value changed");

int CheckFunctionPointers()
{
    TileXRCollectiveOptions options {};
    if (options.backend != TILEXR_COLLECTIVE_BACKEND_AUTO) {
        return 1;
    }

    auto allGather = &TileXRAllGatherEx;
    auto allToAll = &TileXRAllToAllEx;
    auto allReduce = &TileXRAllReduceEx;
    auto reduceScatter = &TileXRReduceScatterEx;
    auto broadcast = &TileXRBroadcastEx;
    auto profileProbe = &TileXRProfileProbeEx;

    (void)allGather;
    (void)allToAll;
    (void)allReduce;
    (void)reduceScatter;
    (void)broadcast;
    (void)profileProbe;
    return 0;
}

int CheckBackendDispatch()
{
    using TileXRCollectives::Host::BackendTestState;
    using TileXRCollectives::Host::CollectiveRequest;
    using TileXRCollectives::Host::DispatchCollective;
    using TileXRCollectives::Host::ResetBackendTestState;
    using TileXRCollectives::Host::SetBackendTestState;

    CollectiveRequest request {};
    request.type = TileXR::TileXRType::ALL_GATHER;
    request.sendBuf = reinterpret_cast<void*>(0x1000);
    request.recvBuf = reinterpret_cast<void*>(0x2000);
    request.count = 1;
    request.dataType = TileXR::TILEXR_DATA_TYPE_INT32;
    request.comm = reinterpret_cast<TileXRCommPtr>(0x3000);
    request.stream = nullptr;
    int sendValue = 1;
    int recvValue = 0;
    TileXRCollectiveOptions options {};
    options.backend = TILEXR_COLLECTIVE_BACKEND_AUTO;

    BackendTestState state {};
    state.aivReturn = TileXR::TILEXR_SUCCESS;
    state.udmaInitialized = false;
    state.ccuInitialized = false;
    SetBackendTestState(state);
    if (DispatchCollective(request, TILEXR_COLLECTIVE_BACKEND_AUTO) != TileXR::TILEXR_SUCCESS) {
        return 2;
    }

    state.udmaInitialized = true;
    state.udmaSupported = true;
    state.udmaReturn = TileXR::TILEXR_SUCCESS;
    SetBackendTestState(state);
    if (DispatchCollective(request, TILEXR_COLLECTIVE_BACKEND_UDMA) != TileXR::TILEXR_SUCCESS) {
        return 3;
    }

    state.udmaInitialized = false;
    SetBackendTestState(state);
    if (DispatchCollective(request, TILEXR_COLLECTIVE_BACKEND_UDMA) != TileXR::TILEXR_ERROR_NOT_INITIALIZED) {
        return 4;
    }

    state.udmaInitialized = true;
    state.udmaSupported = false;
    SetBackendTestState(state);
    if (DispatchCollective(request, TILEXR_COLLECTIVE_BACKEND_UDMA) != TileXR::TILEXR_ERROR_NOT_SUPPORT) {
        return 5;
    }

    state.ccuInitialized = false;
    SetBackendTestState(state);
    if (DispatchCollective(request, TILEXR_COLLECTIVE_BACKEND_CCU) != TileXR::TILEXR_ERROR_NOT_INITIALIZED) {
        return 6;
    }

    state.ccuInitialized = true;
    state.ccuSupported = false;
    SetBackendTestState(state);
    if (DispatchCollective(request, TILEXR_COLLECTIVE_BACKEND_CCU) != TileXR::TILEXR_ERROR_NOT_SUPPORT) {
        return 7;
    }

    state.ccuSupported = true;
    state.ccuReturn = TileXR::TILEXR_SUCCESS;
    SetBackendTestState(state);
    if (DispatchCollective(request, TILEXR_COLLECTIVE_BACKEND_CCU) != TileXR::TILEXR_SUCCESS) {
        return 8;
    }

    options.backend = TILEXR_COLLECTIVE_BACKEND_UDMA;
    state.udmaInitialized = false;
    state.udmaSupported = false;
    SetBackendTestState(state);
    if (TileXRAllGatherEx(&sendValue, &recvValue, 1, TileXR::TILEXR_DATA_TYPE_INT32, request.comm, nullptr, &options) !=
        TileXR::TILEXR_ERROR_NOT_INITIALIZED) {
        return 9;
    }

    options.backend = TILEXR_COLLECTIVE_BACKEND_CCU;
    state.ccuInitialized = true;
    state.ccuSupported = false;
    SetBackendTestState(state);
    if (TileXRAllGatherEx(&sendValue, &recvValue, 1, TileXR::TILEXR_DATA_TYPE_INT32, request.comm, nullptr, &options) !=
        TileXR::TILEXR_ERROR_NOT_SUPPORT) {
        return 10;
    }

    ResetBackendTestState();
    return 0;
}

} // namespace

int main()
{
    const int pointerRet = CheckFunctionPointers();
    if (pointerRet != 0) {
        return pointerRet;
    }
    return CheckBackendDispatch();
}
