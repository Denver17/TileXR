/*
 * Copyright (c) 2024 Huawei Technologies Co., Ltd.
 * This file is a part of the CANN Open Software.
 * Licensed under CANN Open Software License Agreement Version 1.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#ifndef TILEXR_TYPES_H
#define TILEXR_TYPES_H

#include <cstdint>

#ifndef __CCE__
#include <map>
#include <string>
#endif

namespace TileXR {
constexpr int TILEXR_SUCCESS = 0;
constexpr int TILEXR_ERROR_NOT_INITIALIZED = -1;
constexpr int TILEXR_ERROR_MKIRT = -2;
constexpr int TILEXR_ERROR_PARA_CHECK_FAIL = -3;
constexpr int TILEXR_ERROR_INTERNAL = -4;
constexpr int TILEXR_ERROR_TIMEOUT = -5;
constexpr int TILEXR_ERROR_NOT_SUPPORT = -6;
constexpr int TILEXR_ERROR_NOT_FOUND = -7;
constexpr int64_t TILEXR_INVALID_VALUE = -1;

// shared buffer size，这里要和collectives.cce文件中的常量联动修改！！！
constexpr int64_t TILEXR_BUFF_BYTES = 2048LL * 1024 * 1024;
constexpr int TILEXR_FLAG_BUFF_BYTES = 4 * 1024 * 1024;
constexpr int TILEXR_COMM_BUFFER_SIZE = 2044; // 单位MB
constexpr int TILEXR_MEMORY_DOMAIN_BUFFER_SIZE = 8; // 单位MB

enum class ChipName {
    CHIP_310P3 = 0,
    CHIP_910B1,
    CHIP_910B2,
    CHIP_910B3,
    CHIP_910B4,
    CHIP_910B41,
    CHIP_910B2C,
    CHIP_910_9391,
    CHIP_910_9381,
    CHIP_910_9392,
    CHIP_910_9382,
    CHIP_910_9372,
    CHIP_910_9361,
    CHIP_910_9362,
    CHIP_910A5,
    CHIP_950PR,
    CHIP_950,
    RESERVED,
};

enum class PhysicalLink {
    HCCS = 0,
    PCIE = 1,
    RESERVED,
};

enum TileXRDataType {
    TILEXR_DATA_TYPE_INT8 = 0,
    TILEXR_DATA_TYPE_INT16 = 1,
    TILEXR_DATA_TYPE_INT32 = 2,
    TILEXR_DATA_TYPE_FP16 = 3,
    TILEXR_DATA_TYPE_FP32 = 4,
    TILEXR_DATA_TYPE_INT64 = 5,
    TILEXR_DATA_TYPE_UINT64 = 6,
    TILEXR_DATA_TYPE_UINT8 = 7,
    TILEXR_DATA_TYPE_UINT16 = 8,
    TILEXR_DATA_TYPE_UINT32 = 9,
    TILEXR_DATA_TYPE_FP64 = 10,
    TILEXR_DATA_TYPE_BFP16 = 11,
    TILEXR_DATA_TYPE_INT128 = 12,
    TILEXR_DATA_TYPE_HIF8 = 14,
    TILEXR_DATA_TYPE_FP8E4M3 = 15,
    TILEXR_DATA_TYPE_FP8E5M2 = 16,
    TILEXR_DATA_TYPE_FP8E8M0 = 17,
    TILEXR_DATA_TYPE_RESERVED = 255
};

enum TileXRReduceOp {
    TILEXR_REDUCE_SUM = 0,
    TILEXR_REDUCE_PROD = 1,
    TILEXR_REDUCE_MAX = 2,
    TILEXR_REDUCE_MIN = 3,
    TILEXR_REDUCE_RESERVED = 255
};

// 包含 物理链路、芯片名称 信息。
struct PhysicalInfo {
    ChipName chipName = ChipName::RESERVED;
    PhysicalLink physicalLink = PhysicalLink::RESERVED;
    uint32_t coreNum = 0;
};

enum class TileXRType {
    ALL_REDUCE = 1,
    REDUCE_SCATTER = 2,
    ALL_GATHER = 3,
    BROADCAST = 4,
    ALL2ALL = 5,
    ALL2ALL_V_C = 6,
    GATHER = 7,
    LOCAL_REDUCE = 8,
    SEND = 9,
    RECV = 10,
    PROFILE_PROBE = 11,
    PURE_MATMUL = 101,
    MATMUL_ALL_REDUCE = 102,
    MATMUL_REDUCE_SCATTER = 103,
    ALL_GATHER_MATMUL = 104,
    ALL_GATHER_MATMUL_V2 = 105,
    ALL2ALL_MATMUL = 106,
    MATMUL_ALL2ALL = 107,
    MTE2_TEST = 108,
    ALL_GATHER_MATMUL_REDUCE_SCATTER = 111,
    TILEXR_TYPE_MAX = 310,
    BANDWIDTH = 201,

    ALLTOALLV_ALLGATHER_MATMUL = 305,
    MATMUL_REDUCESCATTER_ALLTOALLV = 306,
    ALLTOALLVC_ALLGATHER_MATMUL = 307,
    MATMUL_REDUCESCATTER_ALLTOALLVC = 308,
    ALLTOALLVC_ALLGATHER_MATMUL_HIDDEN = 309,
    MATMUL_REDUCESCATTER_ALLTOALLVC_HIDDEN = 310,
    LCAL_TYPE_MAX = 311
};

#ifndef __CCE__
const std::map<TileXRType, std::string> TILEXR_TYPE2NAME = {
    { TileXRType::ALL_REDUCE, "TileXRAllReduce" },
    { TileXRType::REDUCE_SCATTER, "TileXRReduceScatter" },
    { TileXRType::ALL_GATHER, "TileXRAllGather" },
    { TileXRType::BROADCAST, "TileXRBroadcast" },
    { TileXRType::PURE_MATMUL, "TileXRPureMatmul" },
    { TileXRType::MATMUL_ALL_REDUCE, "TileXRMatmulAllReduce" },
    { TileXRType::MATMUL_REDUCE_SCATTER, "TileXRMatmulReduceScatter" },
    { TileXRType::ALL_GATHER_MATMUL, "TileXRAllGatherMatmul" },
    { TileXRType::ALL_GATHER_MATMUL_V2, "TileXRAllGatherMatmulV2" },
    { TileXRType::ALL2ALL_MATMUL, "TileXRAll2AllMatmul" },
    { TileXRType::MATMUL_ALL2ALL, "TileXRMatmulAll2All" },
    { TileXRType::MTE2_TEST, "TileXRMTE2Test" },
    { TileXRType::ALL2ALL, "TileXRAll2All" },
    { TileXRType::ALL2ALL_V_C, "TileXRAll2AllVC" },
    { TileXRType::ALL_GATHER_MATMUL_REDUCE_SCATTER, "TileXRAllGatherMatmulReduceScatter" },
    { TileXRType::BANDWIDTH, "TileXRBandwidthTest" },
    { TileXRType::LOCAL_REDUCE, "TileXRLocalReduce" },
    { TileXRType::GATHER, "TileXRGather" },
    { TileXRType::SEND, "TileXRSend" },
    { TileXRType::RECV, "TileXRRecv" },
    { TileXRType::PROFILE_PROBE, "TileXRProfileProbe" },
    { TileXRType::ALLTOALLV_ALLGATHER_MATMUL, "TileXRAllToAllVAllGatherMatmul" },
    { TileXRType::MATMUL_REDUCESCATTER_ALLTOALLV, "TileXRMatmulReduceScatterAllToAllV" },

    { TileXRType::ALLTOALLVC_ALLGATHER_MATMUL, "TileXRAllToAllVAllGatherMatmul" },
    { TileXRType::MATMUL_REDUCESCATTER_ALLTOALLVC, "TileXRMatmulReduceScatterAllToAllV" }
};
#endif

} // namespace TileXR
#endif // TILEXR_TYPES_H
