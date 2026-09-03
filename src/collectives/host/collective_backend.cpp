/*
 * Copyright (c) 2024-2026 TileXR Project
 * This file is a part of the CANN Open Software.
 * Licensed under CANN Open Software License Agreement Version 1.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#include "collective_backend.h"

#include "../../comm/ccu/tilexr_ccu_backend.h"
#include "../../comm/tilexr_comm.h"

namespace TileXRCollectives {
namespace Host {
namespace {

BackendTestState g_testState {};

int DispatchAiv(const CollectiveRequest &request)
{
    (void)request;
    return g_testState.enabled ? g_testState.aivReturn : TileXR::TILEXR_SUCCESS;
}

int DispatchUdma(const CollectiveRequest &request)
{
    if (g_testState.enabled) {
        if (!g_testState.udmaInitialized) {
            return TileXR::TILEXR_ERROR_NOT_INITIALIZED;
        }
        return g_testState.udmaSupported ? g_testState.udmaReturn : TileXR::TILEXR_ERROR_NOT_SUPPORT;
    }

    auto *comm = static_cast<TileXR::TileXRComm *>(request.comm);
    if (comm == nullptr || !comm->IsUdmaAvailableForCollectives()) {
        return TileXR::TILEXR_ERROR_NOT_INITIALIZED;
    }
    return TileXR::TILEXR_ERROR_NOT_SUPPORT;
}

int DispatchCcu(const CollectiveRequest &request)
{
    if (g_testState.enabled) {
        if (!g_testState.ccuInitialized) {
            return TileXR::TILEXR_ERROR_NOT_INITIALIZED;
        }
        return g_testState.ccuSupported ? g_testState.ccuReturn : TileXR::TILEXR_ERROR_NOT_SUPPORT;
    }

    auto *comm = static_cast<TileXR::TileXRComm *>(request.comm);
    TileXR::TileXRCcuBackend *backend = comm->GetCcuBackendForCollectives();
    if (backend == nullptr || !backend->Available()) {
        return TileXR::TILEXR_ERROR_NOT_INITIALIZED;
    }
    TileXR::TileXRCcuCollectiveRequest ccuRequest {};
    ccuRequest.type = request.type;
    ccuRequest.sendBuf = request.sendBuf;
    ccuRequest.recvBuf = request.recvBuf;
    ccuRequest.count = request.count;
    ccuRequest.dataType = request.dataType;
    ccuRequest.reduceOp = request.reduceOp;
    ccuRequest.root = request.root;
    ccuRequest.stream = request.stream;
    TileXR::TileXRCcuCollectivePlan plan {};
    const int ret = backend->PrepareCollective(ccuRequest, &plan);
    if (ret != TileXR::TILEXR_SUCCESS) {
        return ret;
    }
    return backend->SubmitCollective(plan, request.stream);
}

} // namespace

int DispatchCollective(const CollectiveRequest &request, TileXRCollectiveBackend backend)
{
    if (request.comm == nullptr || request.sendBuf == nullptr || request.recvBuf == nullptr || request.count <= 0) {
        return TileXR::TILEXR_ERROR_PARA_CHECK_FAIL;
    }

    switch (backend) {
        case TILEXR_COLLECTIVE_BACKEND_AIV:
            return DispatchAiv(request);
        case TILEXR_COLLECTIVE_BACKEND_UDMA:
            return DispatchUdma(request);
        case TILEXR_COLLECTIVE_BACKEND_CCU:
            return DispatchCcu(request);
        case TILEXR_COLLECTIVE_BACKEND_AUTO:
        default:
            if (g_testState.enabled && g_testState.ccuInitialized && g_testState.ccuSupported) {
                return DispatchCcu(request);
            }
            if (g_testState.enabled && g_testState.udmaInitialized && g_testState.udmaSupported) {
                return DispatchUdma(request);
            }
            return DispatchAiv(request);
    }
}

void SetBackendTestState(const BackendTestState &state)
{
    g_testState = state;
    g_testState.enabled = true;
}

void ResetBackendTestState()
{
    g_testState = BackendTestState {};
}

} // namespace Host
} // namespace TileXRCollectives
