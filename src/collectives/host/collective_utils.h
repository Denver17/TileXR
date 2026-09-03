/*
 * Copyright (c) 2024-2026 TileXR Project
 * This file is a part of the CANN Open Software.
 * Licensed under CANN Open Software License Agreement Version 1.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#ifndef TILEXR_COLLECTIVES_HOST_COLLECTIVE_UTILS_H
#define TILEXR_COLLECTIVES_HOST_COLLECTIVE_UTILS_H

#include <cstdint>

#include "comm_args.h"
#include "tilexr_types.h"

namespace TileXRCollectives {
namespace Host {

bool IsSupportedDataType(TileXR::TileXRDataType dataType);

bool IsSupportedReductionDataType(TileXR::TileXRDataType dataType);

bool IsSupportedReduceOp(TileXR::TileXRReduceOp reduceOp);

int64_t CountToBytes(int64_t count, TileXR::TileXRDataType dataType);

uint32_t GetAllGatherBlockNum(const TileXR::CommArgs &commArgs, int64_t dataSize);

uint32_t GetAllToAllBlockNum(const TileXR::CommArgs &commArgs, int64_t dataSize);

uint32_t GetAllReduceBlockNum(const TileXR::CommArgs &commArgs, int64_t dataSize);

uint32_t GetReduceScatterBlockNum(const TileXR::CommArgs &commArgs, int64_t dataSize);

uint32_t GetBroadcastBlockNum(const TileXR::CommArgs &commArgs, int64_t dataSize);

} // namespace Host
} // namespace TileXRCollectives

#endif // TILEXR_COLLECTIVES_HOST_COLLECTIVE_UTILS_H
