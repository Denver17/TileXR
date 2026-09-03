/*
 * Copyright (c) 2024-2026 TileXR Project
 * This file is a part of the CANN Open Software.
 * Licensed under CANN Open Software License Agreement Version 1.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#include <limits>

#include "acl/acl_rt.h"
#include "collective_backend.h"
#include "collective_kernel.h"
#include "collective_launcher.h"
#include "collective_utils.h"
#include "tilexr_collectives.h"

namespace {

int ValidateCommon(void *sendBuf, void *recvBuf, int64_t sendCount,
                   TileXR::TileXRDataType dataType, TileXRCommPtr comm)
{
    if (comm == nullptr || sendBuf == nullptr || recvBuf == nullptr || sendCount <= 0 ||
        !TileXRCollectives::Host::IsSupportedDataType(dataType)) {
        return TileXR::TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    if (TileXRCollectives::Host::CountToBytes(sendCount, dataType) < 0) {
        return TileXR::TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    return TileXR::TILEXR_SUCCESS;
}

int ValidateReduce(void *sendBuf, void *recvBuf, int64_t count,
                   TileXR::TileXRDataType dataType, TileXR::TileXRReduceOp op,
                   TileXRCommPtr comm)
{
    const int ret = ValidateCommon(sendBuf, recvBuf, count, dataType, comm);
    if (ret != TileXR::TILEXR_SUCCESS) {
        return ret;
    }
    return TileXRCollectives::Host::IsSupportedReductionDataType(dataType) &&
            TileXRCollectives::Host::IsSupportedReduceOp(op) ?
        TileXR::TILEXR_SUCCESS : TileXR::TILEXR_ERROR_PARA_CHECK_FAIL;
}

int ValidateBroadcastLocal(void *buf, int64_t count, TileXR::TileXRDataType dataType,
                           int root, TileXRCommPtr comm)
{
    if (comm == nullptr || buf == nullptr || count <= 0 ||
        !TileXRCollectives::Host::IsSupportedDataType(dataType) || root < 0) {
        return TileXR::TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    if (TileXRCollectives::Host::CountToBytes(count, dataType) < 0) {
        return TileXR::TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    return TileXR::TILEXR_SUCCESS;
}

int LoopbackCopy(void *sendBuf, void *recvBuf, int64_t bytes, aclrtStream stream)
{
    if (sendBuf == recvBuf) {
        return TileXR::TILEXR_SUCCESS;
    }
    const aclError ret = aclrtMemcpyAsync(recvBuf, static_cast<size_t>(bytes), sendBuf, static_cast<size_t>(bytes),
        ACL_MEMCPY_DEVICE_TO_DEVICE, stream);
    return ret == ACL_SUCCESS ? TileXR::TILEXR_SUCCESS : TileXR::TILEXR_ERROR_INTERNAL;
}

TileXRCollectiveBackend SelectedBackend(const TileXRCollectiveOptions *options)
{
    return options == nullptr ? TILEXR_COLLECTIVE_BACKEND_AUTO : options->backend;
}

bool UsesForcedNonAivBackend(TileXRCollectiveBackend backend)
{
    return backend == TILEXR_COLLECTIVE_BACKEND_UDMA || backend == TILEXR_COLLECTIVE_BACKEND_CCU;
}

} // namespace

int TileXRAllGatherEx(void *sendBuf, void *recvBuf, int64_t sendCount,
                      TileXR::TileXRDataType dataType, TileXRCommPtr comm,
                      aclrtStream stream, const TileXRCollectiveOptions *options)
{
    int ret = ValidateCommon(sendBuf, recvBuf, sendCount, dataType, comm);
    if (ret != TileXR::TILEXR_SUCCESS) {
        return ret;
    }
    const TileXRCollectiveBackend backend = SelectedBackend(options);
    if (UsesForcedNonAivBackend(backend)) {
        TileXRCollectives::Host::CollectiveRequest request {};
        request.type = TileXR::TileXRType::ALL_GATHER;
        request.sendBuf = sendBuf;
        request.recvBuf = recvBuf;
        request.count = sendCount;
        request.dataType = dataType;
        request.comm = comm;
        request.stream = stream;
        return TileXRCollectives::Host::DispatchCollective(request, backend);
    }

    TileXRCollectives::Host::HostLaunchContext context;
    ret = TileXRCollectives::Host::PrepareHostLaunchContext(comm, context);
    if (ret != TileXR::TILEXR_SUCCESS) {
        return ret;
    }

    const int64_t bytes = TileXRCollectives::Host::CountToBytes(sendCount, dataType);
    if (context.hostArgs->rankSize <= 1) {
        return LoopbackCopy(sendBuf, recvBuf, bytes, stream);
    }

    const uint32_t blockDim = TileXRCollectives::Host::GetAllGatherBlockNum(*context.hostArgs, bytes);
    return TileXRCollectives::Host::LaunchCollectiveKernel(comm, TileXR::TileXRType::ALL_GATHER, context,
        sendBuf, recvBuf, sendCount, dataType, blockDim, stream);
}

int TileXRAllGather(void *sendBuf, void *recvBuf, int64_t sendCount,
                    TileXR::TileXRDataType dataType, TileXRCommPtr comm,
                    aclrtStream stream)
{
    return TileXRAllGatherEx(sendBuf, recvBuf, sendCount, dataType, comm, stream, nullptr);
}

int TileXRAllToAllEx(void *sendBuf, void *recvBuf, int64_t sendCount,
                     TileXR::TileXRDataType dataType, TileXRCommPtr comm,
                     aclrtStream stream, const TileXRCollectiveOptions *options)
{
    int ret = ValidateCommon(sendBuf, recvBuf, sendCount, dataType, comm);
    if (ret != TileXR::TILEXR_SUCCESS) {
        return ret;
    }
    const TileXRCollectiveBackend backend = SelectedBackend(options);
    if (UsesForcedNonAivBackend(backend)) {
        TileXRCollectives::Host::CollectiveRequest request {};
        request.type = TileXR::TileXRType::ALL2ALL;
        request.sendBuf = sendBuf;
        request.recvBuf = recvBuf;
        request.count = sendCount;
        request.dataType = dataType;
        request.comm = comm;
        request.stream = stream;
        return TileXRCollectives::Host::DispatchCollective(request, backend);
    }

    TileXRCollectives::Host::HostLaunchContext context;
    ret = TileXRCollectives::Host::PrepareHostLaunchContext(comm, context);
    if (ret != TileXR::TILEXR_SUCCESS) {
        return ret;
    }

    const int64_t loopbackBytes = TileXRCollectives::Host::CountToBytes(sendCount, dataType);
    const int rankSize = context.hostArgs->rankSize;
    if (rankSize <= 1) {
        return LoopbackCopy(sendBuf, recvBuf, loopbackBytes, stream);
    }
    if (rankSize % TileXR::RANK_SIZE_TWO != 0) {
        return TileXR::TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    if (sendCount > std::numeric_limits<int64_t>::max() / static_cast<int64_t>(rankSize)) {
        return TileXR::TILEXR_ERROR_PARA_CHECK_FAIL;
    }

    const int64_t kernelCount = sendCount * static_cast<int64_t>(rankSize);
    const int64_t kernelBytes = TileXRCollectives::Host::CountToBytes(kernelCount, dataType);
    if (kernelBytes < 0) {
        return TileXR::TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    const uint32_t blockDim = TileXRCollectives::Host::GetAllToAllBlockNum(*context.hostArgs, kernelBytes);
    if (blockDim == 0) {
        return TileXR::TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    return TileXRCollectives::Host::LaunchCollectiveKernel(comm, TileXR::TileXRType::ALL2ALL, context,
        sendBuf, recvBuf, kernelCount, dataType, blockDim, stream);
}

int TileXRAllToAll(void *sendBuf, void *recvBuf, int64_t sendCount,
                   TileXR::TileXRDataType dataType, TileXRCommPtr comm,
                   aclrtStream stream)
{
    return TileXRAllToAllEx(sendBuf, recvBuf, sendCount, dataType, comm, stream, nullptr);
}

int TileXRAllReduceEx(void *sendBuf, void *recvBuf, int64_t count,
                      TileXR::TileXRDataType dataType, TileXR::TileXRReduceOp op,
                      TileXRCommPtr comm, aclrtStream stream, const TileXRCollectiveOptions *options)
{
    int ret = ValidateReduce(sendBuf, recvBuf, count, dataType, op, comm);
    if (ret != TileXR::TILEXR_SUCCESS) {
        return ret;
    }
    const TileXRCollectiveBackend backend = SelectedBackend(options);
    if (UsesForcedNonAivBackend(backend)) {
        TileXRCollectives::Host::CollectiveRequest request {};
        request.type = TileXR::TileXRType::ALL_REDUCE;
        request.sendBuf = sendBuf;
        request.recvBuf = recvBuf;
        request.count = count;
        request.dataType = dataType;
        request.reduceOp = op;
        request.comm = comm;
        request.stream = stream;
        return TileXRCollectives::Host::DispatchCollective(request, backend);
    }

    TileXRCollectives::Host::HostLaunchContext context;
    ret = TileXRCollectives::Host::PrepareHostLaunchContext(comm, context);
    if (ret != TileXR::TILEXR_SUCCESS) {
        return ret;
    }

    const int64_t bytes = TileXRCollectives::Host::CountToBytes(count, dataType);
    if (context.hostArgs->rankSize <= 1) {
        return LoopbackCopy(sendBuf, recvBuf, bytes, stream);
    }

    const uint32_t blockDim = TileXRCollectives::Host::GetAllReduceBlockNum(*context.hostArgs, bytes);
    return TileXRCollectives::Host::LaunchCollectiveKernel(comm, TileXR::TileXRType::ALL_REDUCE, context,
        sendBuf, recvBuf, count, dataType, blockDim, stream,
        TileXRCollectives::Host::CollectiveLaunchAttrs { static_cast<int>(op), 0 });
}

int TileXRAllReduce(void *sendBuf, void *recvBuf, int64_t count,
                    TileXR::TileXRDataType dataType, TileXR::TileXRReduceOp op,
                    TileXRCommPtr comm, aclrtStream stream)
{
    return TileXRAllReduceEx(sendBuf, recvBuf, count, dataType, op, comm, stream, nullptr);
}

int TileXRReduceScatterEx(void *sendBuf, void *recvBuf, int64_t recvCount,
                          TileXR::TileXRDataType dataType, TileXR::TileXRReduceOp op,
                          TileXRCommPtr comm, aclrtStream stream,
                          const TileXRCollectiveOptions *options)
{
    int ret = ValidateReduce(sendBuf, recvBuf, recvCount, dataType, op, comm);
    if (ret != TileXR::TILEXR_SUCCESS) {
        return ret;
    }
    const TileXRCollectiveBackend backend = SelectedBackend(options);
    if (UsesForcedNonAivBackend(backend)) {
        TileXRCollectives::Host::CollectiveRequest request {};
        request.type = TileXR::TileXRType::REDUCE_SCATTER;
        request.sendBuf = sendBuf;
        request.recvBuf = recvBuf;
        request.count = recvCount;
        request.dataType = dataType;
        request.reduceOp = op;
        request.comm = comm;
        request.stream = stream;
        return TileXRCollectives::Host::DispatchCollective(request, backend);
    }

    TileXRCollectives::Host::HostLaunchContext context;
    ret = TileXRCollectives::Host::PrepareHostLaunchContext(comm, context);
    if (ret != TileXR::TILEXR_SUCCESS) {
        return ret;
    }

    const int64_t bytes = TileXRCollectives::Host::CountToBytes(recvCount, dataType);
    const int rankSize = context.hostArgs->rankSize;
    if (rankSize <= 1) {
        return LoopbackCopy(sendBuf, recvBuf, bytes, stream);
    }
    if (recvCount > std::numeric_limits<int64_t>::max() / static_cast<int64_t>(rankSize)) {
        return TileXR::TILEXR_ERROR_PARA_CHECK_FAIL;
    }

    const int64_t inputCount = recvCount * static_cast<int64_t>(rankSize);
    const int64_t inputBytes = TileXRCollectives::Host::CountToBytes(inputCount, dataType);
    if (inputBytes < 0) {
        return TileXR::TILEXR_ERROR_PARA_CHECK_FAIL;
    }

    const uint32_t blockDim = TileXRCollectives::Host::GetReduceScatterBlockNum(*context.hostArgs, bytes);
    return TileXRCollectives::Host::LaunchCollectiveKernel(comm, TileXR::TileXRType::REDUCE_SCATTER, context,
        sendBuf, recvBuf, recvCount, dataType, blockDim, stream,
        TileXRCollectives::Host::CollectiveLaunchAttrs { static_cast<int>(op), 0 });
}

int TileXRReduceScatter(void *sendBuf, void *recvBuf, int64_t recvCount,
                        TileXR::TileXRDataType dataType, TileXR::TileXRReduceOp op,
                        TileXRCommPtr comm, aclrtStream stream)
{
    return TileXRReduceScatterEx(sendBuf, recvBuf, recvCount, dataType, op, comm, stream, nullptr);
}

int TileXRBroadcastEx(void *buf, int64_t count,
                      TileXR::TileXRDataType dataType, int root,
                      TileXRCommPtr comm, aclrtStream stream, const TileXRCollectiveOptions *options)
{
    int ret = ValidateBroadcastLocal(buf, count, dataType, root, comm);
    if (ret != TileXR::TILEXR_SUCCESS) {
        return ret;
    }
    const TileXRCollectiveBackend backend = SelectedBackend(options);
    if (UsesForcedNonAivBackend(backend)) {
        TileXRCollectives::Host::CollectiveRequest request {};
        request.type = TileXR::TileXRType::BROADCAST;
        request.sendBuf = buf;
        request.recvBuf = buf;
        request.count = count;
        request.dataType = dataType;
        request.root = root;
        request.comm = comm;
        request.stream = stream;
        return TileXRCollectives::Host::DispatchCollective(request, backend);
    }

    TileXRCollectives::Host::HostLaunchContext context;
    ret = TileXRCollectives::Host::PrepareHostLaunchContext(comm, context);
    if (ret != TileXR::TILEXR_SUCCESS) {
        return ret;
    }
    if (root >= context.hostArgs->rankSize) {
        return TileXR::TILEXR_ERROR_PARA_CHECK_FAIL;
    }

    const int64_t bytes = TileXRCollectives::Host::CountToBytes(count, dataType);
    if (context.hostArgs->rankSize <= 1) {
        return TileXR::TILEXR_SUCCESS;
    }

    const uint32_t blockDim = TileXRCollectives::Host::GetBroadcastBlockNum(*context.hostArgs, bytes);
    if (blockDim == 0) {
        return TileXR::TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    return TileXRCollectives::Host::LaunchCollectiveKernel(comm, TileXR::TileXRType::BROADCAST, context,
        buf, buf, bytes, dataType, blockDim, stream,
        TileXRCollectives::Host::CollectiveLaunchAttrs { 0, root });
}

int TileXRBroadcast(void *buf, int64_t count,
                    TileXR::TileXRDataType dataType, int root,
                    TileXRCommPtr comm, aclrtStream stream)
{
    return TileXRBroadcastEx(buf, count, dataType, root, comm, stream, nullptr);
}
