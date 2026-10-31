/*
 * Copyright (c) 2024-2026 TileXR Project
 * This file is a part of the CANN Open Software.
 * Licensed under CANN Open Software License Agreement Version 1.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#include "collective_utils.h"

#include <cstdlib>
#include <limits>
#include <string>

namespace TileXRCollectives {
namespace Host {
namespace {

constexpr uint32_t AX_RANK_SIZE = 16;
constexpr uint32_t AX_BLOCK_NUM = 10;
constexpr uint32_t TWO_BLOCK_NUM = 2;
constexpr uint32_t ALL_GATHER_HDB_RING_BLOCK_NUM = 32;
constexpr int64_t ALL_GATHER_SMALL_DATA_SIZE = 2 * 1024 * 1024;
constexpr int64_t SMALL_DATA_SIZE_910_93 = 32LL * 1024 * 1024;
constexpr uint32_t SMALL_RANK_SIZE = 8;
constexpr uint32_t ALLTOALL_TWO_STEP_BLOCK_NUM = 16;
constexpr uint32_t THREE_STEP_BLOCK_NUM = 3;
constexpr uint32_t ALL_REDUCE_DB_RING_BLOCK_NUM = 34;
constexpr uint32_t REDUCE_SCATTER_FOUR_STEP_BLOCK_NUM = 34;
constexpr uint32_t REDUCE_SCATTER_DB_RING_BLOCK_NUM = 36;
constexpr uint32_t BROADCAST_MAX_RANK_SIZE = 8;
constexpr int64_t ONE_MIB = 1LL * 1024 * 1024;
constexpr int64_t TWO_MIB = 2LL * 1024 * 1024;
constexpr int64_t THIRTY_TWO_MIB = 32LL * 1024 * 1024;

int64_t DataTypeSize(TileXR::TileXRDataType dataType)
{
    switch (dataType) {
        case TileXR::TILEXR_DATA_TYPE_INT8:
            return 1;
        case TileXR::TILEXR_DATA_TYPE_INT16:
        case TileXR::TILEXR_DATA_TYPE_FP16:
        case TileXR::TILEXR_DATA_TYPE_BFP16:
            return 2;
        case TileXR::TILEXR_DATA_TYPE_INT32:
        case TileXR::TILEXR_DATA_TYPE_FP32:
            return 4;
        case TileXR::TILEXR_DATA_TYPE_INT64:
            return 8;
        default:
            return TileXR::TILEXR_INVALID_VALUE;
    }
}

bool GetParallel()
{
    static int parallel = -1;
    if (parallel == -1) {
        const char *env = std::getenv("LCCL_PARALLEL");
        parallel = (env != nullptr && (std::string(env) == "1" || std::string(env) == "true")) ? 1 : 0;
    }
    return parallel == 1;
}

uint32_t RankSizeOrZero(const TileXR::CommArgs &commArgs)
{
    return commArgs.rankSize > 0 ? static_cast<uint32_t>(commArgs.rankSize) : 0;
}

} // namespace

bool IsSupportedDataType(TileXR::TileXRDataType dataType)
{
    return DataTypeSize(dataType) > 0;
}

bool IsSupportedReductionDataType(TileXR::TileXRDataType dataType)
{
    if (!IsSupportedDataType(dataType)) {
        return false;
    }
#if defined(TILEXR_COLLECTIVES_C310_ATOMIC_LIMITS)
    switch (dataType) {
        case TileXR::TILEXR_DATA_TYPE_INT8:
        case TileXR::TILEXR_DATA_TYPE_INT16:
        case TileXR::TILEXR_DATA_TYPE_INT32:
        case TileXR::TILEXR_DATA_TYPE_FP16:
        case TileXR::TILEXR_DATA_TYPE_FP32:
        case TileXR::TILEXR_DATA_TYPE_BFP16:
            return true;
        default:
            return false;
    }
#else
    return true;
#endif
}

bool IsSupportedReduceOp(TileXR::TileXRReduceOp reduceOp)
{
    // The first standalone reduction API surface is SUM-only until other ops pass hardware validation.
    return reduceOp == TileXR::TILEXR_REDUCE_SUM;
}

int64_t CountToBytes(int64_t count, TileXR::TileXRDataType dataType)
{
    if (count < 0) {
        return TileXR::TILEXR_INVALID_VALUE;
    }
    const int64_t typeSize = DataTypeSize(dataType);
    if (typeSize <= 0) {
        return TileXR::TILEXR_INVALID_VALUE;
    }
    if (count > std::numeric_limits<int64_t>::max() / typeSize) {
        return TileXR::TILEXR_INVALID_VALUE;
    }
    return count * typeSize;
}

int64_t ComputeCollectiveMessageBytes(TileXR::TileXRType type,
                                      TileXR::TileXRDataType dataType,
                                      int64_t count,
                                      int rankSize)
{
    if (type == TileXR::TileXRType::BROADCAST) {
        return count;
    }

    int64_t elementCount = count;
    if (type == TileXR::TileXRType::REDUCE_SCATTER) {
        if (rankSize <= 0 || count < 0 ||
            count > std::numeric_limits<int64_t>::max() / static_cast<int64_t>(rankSize)) {
            return TileXR::TILEXR_INVALID_VALUE;
        }
        elementCount = count * static_cast<int64_t>(rankSize);
    }
    return CountToBytes(elementCount, dataType);
}

uint32_t GetAllGatherBlockNum(const TileXR::CommArgs &commArgs, int64_t dataSize)
{
    const uint32_t rankSize = RankSizeOrZero(commArgs);
    if (rankSize == 0) {
        return 0;
    }
    const uint32_t extraFlag = commArgs.extraFlag;
    if ((extraFlag & TileXR::ExtraFlag::TOPO_910B2C) != 0 && rankSize == AX_RANK_SIZE) {
        return AX_BLOCK_NUM;
    }
    if ((extraFlag & TileXR::ExtraFlag::TOPO_PCIE) != 0) {
        return rankSize * TWO_BLOCK_NUM;
    }
    if (GetParallel()) {
        return rankSize;
    }
    if ((extraFlag & TileXR::ExtraFlag::TOPO_910_93) != 0 &&
        (dataSize > SMALL_DATA_SIZE_910_93 || rankSize > SMALL_RANK_SIZE) &&
        rankSize > TileXR::RANK_SIZE_TWO && rankSize % TileXR::RANK_SIZE_TWO == 0) {
        return ALL_GATHER_HDB_RING_BLOCK_NUM;
    }
    return (rankSize == TileXR::RANK_SIZE_TWO || dataSize >= ALL_GATHER_SMALL_DATA_SIZE) ?
        rankSize * TWO_BLOCK_NUM : rankSize;
}

uint32_t GetAllToAllBlockNum(const TileXR::CommArgs &commArgs, int64_t dataSize)
{
    const uint32_t rankSize = RankSizeOrZero(commArgs);
    if (rankSize == 0) {
        return 0;
    }
    const uint32_t extraFlag = commArgs.extraFlag;
    if ((extraFlag & TileXR::ExtraFlag::TOPO_910_93) == 0) {
        return 0;
    }
    if (rankSize <= SMALL_RANK_SIZE && dataSize > TileXR::SMALL_DATA_SIZE &&
        dataSize % static_cast<int64_t>(SMALL_RANK_SIZE * SMALL_RANK_SIZE * rankSize) == 0) {
        return ALLTOALL_TWO_STEP_BLOCK_NUM * TWO_BLOCK_NUM;
    }
    return rankSize <= ALLTOALL_TWO_STEP_BLOCK_NUM ?
        rankSize * TWO_BLOCK_NUM : ALLTOALL_TWO_STEP_BLOCK_NUM * TWO_BLOCK_NUM;
}

uint32_t GetAllReduceBlockNum(const TileXR::CommArgs &commArgs, int64_t dataSize)
{
    const uint32_t rankSize = RankSizeOrZero(commArgs);
    if (rankSize == 0 || dataSize < 0) {
        return 0;
    }

    const uint32_t extraFlag = commArgs.extraFlag;
    if ((extraFlag & TileXR::ExtraFlag::TOPO_PCIE) != 0 && rankSize == TileXR::RANK_SIZE_TWO) {
        return rankSize * TWO_BLOCK_NUM;
    }
    if ((extraFlag & TileXR::ExtraFlag::TOPO_910B2C) != 0 && rankSize > SMALL_RANK_SIZE) {
        return dataSize < TWO_MIB ? rankSize :
            (rankSize / TWO_BLOCK_NUM * THREE_STEP_BLOCK_NUM + TWO_BLOCK_NUM);
    }
    if (GetParallel()) {
        return rankSize;
    }
    if ((extraFlag & TileXR::ExtraFlag::TOPO_910_93) != 0 &&
        dataSize > THIRTY_TWO_MIB && rankSize != TileXR::RANK_SIZE_TWO) {
        return rankSize % TileXR::RANK_SIZE_TWO == 0 ?
            ALL_REDUCE_DB_RING_BLOCK_NUM : rankSize * THREE_STEP_BLOCK_NUM;
    }
    return (rankSize == TileXR::RANK_SIZE_TWO || dataSize >= TWO_MIB) ?
        rankSize * TWO_BLOCK_NUM : rankSize;
}

uint32_t GetReduceScatterBlockNum(const TileXR::CommArgs &commArgs, int64_t dataSize)
{
    const uint32_t rankSize = RankSizeOrZero(commArgs);
    if (rankSize == 0 || dataSize < 0) {
        return 0;
    }

    const uint32_t extraFlag = commArgs.extraFlag;
    if ((extraFlag & TileXR::ExtraFlag::TOPO_PCIE) != 0) {
        return rankSize * TWO_BLOCK_NUM;
    }

    const bool isDbRingDataSize = dataSize > TWO_MIB / SMALL_RANK_SIZE &&
        dataSize <= THIRTY_TWO_MIB / SMALL_RANK_SIZE;
    const bool isDbRing = (rankSize == 4 || rankSize == SMALL_RANK_SIZE) && isDbRingDataSize;
    if ((extraFlag & TileXR::ExtraFlag::TOPO_910_93) != 0 &&
        ((rankSize > SMALL_RANK_SIZE && rankSize % TWO_BLOCK_NUM == 0) || isDbRing)) {
        if (isDbRing) {
            return REDUCE_SCATTER_DB_RING_BLOCK_NUM;
        }
        return dataSize <= ONE_MIB ? rankSize : REDUCE_SCATTER_FOUR_STEP_BLOCK_NUM;
    }
    return (rankSize == TileXR::RANK_SIZE_TWO || dataSize >= TWO_MIB) ?
        rankSize * TWO_BLOCK_NUM : rankSize;
}

uint32_t GetBroadcastBlockNum(const TileXR::CommArgs &commArgs, int64_t dataSize)
{
    const uint32_t rankSize = RankSizeOrZero(commArgs);
    if (rankSize == 0 || dataSize < 0 || rankSize > BROADCAST_MAX_RANK_SIZE) {
        return 0;
    }
    return rankSize;
}

} // namespace Host
} // namespace TileXRCollectives
