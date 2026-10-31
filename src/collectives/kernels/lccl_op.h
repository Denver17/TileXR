/*
 * Copyright (c) 2024 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#ifndef TILEXR_LCCL_OP_H
#define TILEXR_LCCL_OP_H

#if defined(__DAV_C220_VEC__) || defined(__DAV_C220_CUBE__) || defined(__DAV_C310_VEC__)

#include "op_def.h"
#include "tilexr/profiling.h"
#include "allgather.h"
#include "91093/allgather_hierarchy_double_ring.h"
#include "allreduce_quant.h"
#include "allreduce_one_shot.h"
#include "allreduce_two_shot.h"
#include "allreduce_big_data.h"
#include "91093/allreduce_big_data_sio.h"
#include "91093/allreduce_hierarchy_double_ring.h"
#include "reduce_scatter.h"
#include "91093/reduce_scatter_big_data_91093_4step.h"
#include "91093/reduce_scatter_hierarchy_double_ring.h"
#include "91093/all2all_hierarchy.h"
#include "91093/all2all_hierarchy_small.h"

#include "kernels/lcal_allreduce_2npu_read.cce"
#include "kernels/lcal_allreduce_2npu_write.cce"
#include "kernels/lcal_allreduce_2npu_big_write.cce"
#include "kernels/lcal_allreduce_two_shot.cce"
#include "kernels/lcal_allreduce_big_data.cce"
#include "kernels/lcal_allreduce_two_shot_910B2C.cce"
#include "kernels/lcal_allreduce_big_data_910B2C.cce"
#include "kernels/lcal_allreduce_deterministic.cce"
#include "kernels/lcal_allreduce_deterministic_big_data.cce"
#include "kernels/lcal_reduce_scatter_big_data_write.cce"
#include "kernels/lcal_reduce_scatter_write.cce"
#include "kernels/lcal_reduce_scatter.cce"
#include "kernels/lcal_reduce_scatter_big_data.cce"
#include "kernels/lcal_allgather_910B2C.cce"
#include "kernels/lcal_allgather_big_data_910B2C.cce"
#include "kernels/lcal_allgather_2npu.cce"
#include "kernels/lcal_allgather_2npu_big_data_write.cce"
#include "kernels/lcal_allgather.cce"
#include "kernels/lcal_allgather_big_data.cce"
#include "kernels/lcal_broadcast_write.cce"
#include "kernels/lcal_broadcast_big_data.cce"
#include "kernels/lcal_all2all_transpose.cce"

extern "C" __global__ __aicore__ __attribute__((section("Attr_Section_TileXR"))) void TileXRDescriptor() {}

#define TILEXR_COARSE_PERF_BEGIN(tokenName) \
    TileXR::TileXRStragglerTimingToken tokenName##Straggler = TileXR::TileXRStragglerTimingBegin( \
        stragglerRecord); \
    TileXR::Profiling::Recorder<2U> tokenName; \
    const bool tokenName##Enabled = profilingGM != nullptr; \
    if (tokenName##Enabled) { \
        tokenName.RecordMarker<0U, 1U>(); \
    }

#define TILEXR_COARSE_PERF_END(tokenName) \
    TileXR::TileXRStragglerTimingEnd(tokenName##Straggler, stragglerRecord); \
    if (tokenName##Enabled) { \
        tokenName.RecordMarker<1U, 2U>(); \
        tokenName.Flush(profilingGM, static_cast<uint32_t>(AscendC::GetBlockIdx())); \
    }

#define CLASS_OP_QUANT_LAUNCH(name, outputType, inputType) \
do { \
name<outputType, inputType> opKernel(localRank, localRankSize, extraFlag); \
opKernel.Init(KERNELS_ARGS_CALL()); \
opKernel.Process(); \
} while (0)

#define LCCL_BROADCAST_FUNC_AUTO_DEF(suffix) \
extern "C" __global__ __aicore__ void TileXRBroadcast##suffix(KERNELS_ARGS_FUN()) \
{ \
    if ASCEND_IS_AIV { \
    GET_COMM_ARGS; \
    TILEXR_COARSE_PERF_BEGIN(coarsePerf); \
    __gm__ char * shareAddrs[TILEXR_MAX_RANK_SIZE]; \
    GET_IPC_MEM_ARGS(char); \
    if ((extraFlag & ExtraFlag::TOPO_PCIE) != 0) { \
        TileXRBroadcast2npuBigDataWrite(ALLREDUCE_ARGS_CALL(char)); \
    } else { \
        TileXRBroadcastBigData(ALLREDUCE_ARGS_CALL(char)); \
    } \
    TILEXR_COARSE_PERF_END(coarsePerf); \
    } \
}

#define LCCL_ALLGATHER_FUNC_AUTO_DEF(type, suffix) \
extern "C" __global__ __aicore__ void TileXRAllGather_##type##suffix(KERNELS_ARGS_FUN()) { \
    if ASCEND_IS_AIV { \
    GET_COMM_ARGS; \
    TILEXR_COARSE_PERF_BEGIN(coarsePerf); \
    constexpr int32_t quickOneshotRankSize = 2; \
    constexpr int32_t cceSmallDataSize = 2 * 1024 * 1024; \
    constexpr int32_t smallRankSize = 8; \
    constexpr int32_t smallDataSize910a3 = 32 * 1024 * 1024; \
    __gm__ type * shareAddrs[TILEXR_MAX_RANK_SIZE]; \
    GET_IPC_MEM_ARGS(type); \
    if ((extraFlag & ExtraFlag::TOPO_910B2C) != 0 && rankSize > smallRankSize) { \
        if (len * sizeof(type) < cceSmallDataSize) { \
            TileXRAllGather910B2C<type>(ALLREDUCE_ARGS_CALL_16P(type)); \
        } else { \
            TileXRAllGatherBigData910B2C<type>(ALLREDUCE_ARGS_CALL_16P(type)); \
        } \
    } else if ((extraFlag & ExtraFlag::TOPO_PCIE) != 0) { \
        TileXRAllGather2npuBigDataWrite<type>(ALLREDUCE_ARGS_CALL_16P(type)); \
    } else if ((extraFlag & ExtraFlag::TOPO_910_93) != 0 && lcalBlockNum != rankSize && \
        (len > smallDataSize910a3 / sizeof(type) || rankSize > smallRankSize) && \
        rankSize > quickOneshotRankSize && rankSize % quickOneshotRankSize == 0) { \
        CLASS_OP_LAUNCH(AllGatherHierarchyDoubleRing, type); \
    } else { \
        if (rankSize == quickOneshotRankSize && len * sizeof(type) < SIZE_OF_8M && lcalBlockNum != rankSize) { \
            TileXRAllGather2npu<type>(ALLREDUCE_ARGS_CALL_16P(type)); \
        } else if (rankSize == quickOneshotRankSize && lcalBlockNum != rankSize) { \
            TileXRAllGather2npuBigDataWrite<type>(ALLREDUCE_ARGS_CALL_16P(type)); \
        } else if ((rankSize > quickOneshotRankSize && len * sizeof(type) < cceSmallDataSize) || \
            lcalBlockNum == rankSize) { \
            TileXRAllGather<type>(ALLREDUCE_ARGS_CALL_16P(type)); \
        } else { \
            TileXRAllGatherBigData<type>(ALLREDUCE_ARGS_CALL_16P(type)); \
        } \
    } \
    TILEXR_COARSE_PERF_END(coarsePerf); \
    } \
}

#define LCCL_ALL_REDUCE_FUNC_AUTO_DEF(type, suffix) \
extern "C" __global__ __aicore__ void TileXRAllReduce_##type##suffix(KERNELS_ARGS_FUN()) { \
    if ASCEND_IS_AIV { \
    GET_COMM_ARGS; \
    TILEXR_COARSE_PERF_BEGIN(coarsePerf); \
    constexpr int32_t quickOneshotRankSize = 2; \
    constexpr int32_t threeStepNum = 3; \
    constexpr int32_t smallRankSize = 8; \
    constexpr int32_t oneshotDataSize = 16 * 1024; \
    constexpr int64_t quantSmallDataSize = 512 * 1024; \
    constexpr int32_t cceSmallDataSize = 2 * 1024 * 1024; \
    constexpr int32_t smallDataSize910a3 = 32 * 1024 * 1024; \
    constexpr int32_t rankSize910a3 = 16; \
    constexpr int32_t scaleCountMax = 12 * 1024 * 1024; \
    __gm__ type * shareAddrs[TILEXR_MAX_RANK_SIZE]; \
    GET_IPC_MEM_ARGS(type); \
    if ((extraFlag & ExtraFlag::TOPO_PCIE) != 0 && rankSize == quickOneshotRankSize) { \
        if (len * sizeof(type) < SIZE_OF_8M) { \
            TileXRAllReduce2npuWrite<type>(ALLREDUCE_ARGS_CALL_16P(type)); \
        } else { \
            TileXRAllReduce2npuBigDataWrite<type>(ALLREDUCE_ARGS_CALL_16P(type)); \
        } \
    } else if ((extraFlag & ExtraFlag::QUANT_FP16) != 0 && std::is_same_v<type, int8_t>) { \
        if (len * sizeof(type) <= oneshotDataSize) { \
            CLASS_OP_QUANT_LAUNCH(AllReduceOneShot, half, int8_t); \
        } else if (len * sizeof(type) <= quantSmallDataSize) { \
            CLASS_OP_QUANT_LAUNCH(AllReduceTwoShot, half, int8_t); \
        } else if (scaleCount * rankSize <= scaleCountMax) { \
            CLASS_OP_QUANT_LAUNCH(AllReduceBigData, half, int8_t); \
        } else { \
            AllReduceBigData<half, int8_t> opTmp(localRank, localRankSize, extraFlag); \
            opTmp.Init(KERNELS_ARGS_CALL()); \
            opTmp.SupportBigScale(); \
            input = output; \
            CLASS_OP_LAUNCH(AllReduceBigData, half); \
        } \
    } else if ((extraFlag & ExtraFlag::TOPO_910B2C) != 0 && rankSize > smallRankSize) { \
        if (len * sizeof(type) < cceSmallDataSize) { \
            TileXRAllReduceTwoShot910B2C<type>(ALLREDUCE_ARGS_CALL_16P(type)); \
        } else { \
            TileXRAllReduceBigData910B2C<type>(ALLREDUCE_ARGS_CALL_16P(type)); \
        } \
    } else if ((extraFlag & ExtraFlag::DETERMINISTIC) != 0) { \
        constexpr uint32_t maxAivNum = 40; \
        const bool isAivNumSupport = ((extraFlag & ExtraFlag::IS_GREATER_THAN_40_AIV) != 0 || \
            rankSize * threeStepNum <= maxAivNum); \
        if ((extraFlag & ExtraFlag::TOPO_910_93) != 0) { \
            if (rankSize % quickOneshotRankSize == 1 || rankSize == quickOneshotRankSize || \
                (rankSize <= rankSize910a3 && len * sizeof(type) <= smallDataSize910a3 && isAivNumSupport)) { \
                TileXRAllReduceDeterministicBigData<type>(ALLREDUCE_ARGS_CALL_16P(type)); \
            } else { \
                CLASS_OP_LAUNCH(AllReduceHierarchyDoubleRing, type); \
            } \
        } else if (len * sizeof(type) < SMALL_DATA_SIZE) { \
            TileXRAllReduceDeterministic<type>(ALLREDUCE_ARGS_CALL_16P(type)); \
        } else { \
            TileXRAllReduceDeterministicBigData<type>(ALLREDUCE_ARGS_CALL_16P(type)); \
        } \
    } else if ((extraFlag & ExtraFlag::TOPO_910_93) != 0 && lcalBlockNum != rankSize && \
        (rankSize == quickOneshotRankSize || len * sizeof(type) > smallDataSize910a3)) { \
        if (rankSize == quickOneshotRankSize) { \
            TileXRAllReduce2npuBigDataWrite<type>(ALLREDUCE_ARGS_CALL_16P(type)); \
        } else if (rankSize % quickOneshotRankSize == 0) { \
            CLASS_OP_LAUNCH(AllReduceHierarchyDoubleRing, type); \
        } else { \
            CLASS_OP_LAUNCH(AllReduceBigDataSio, type); \
        } \
    } else { \
        if (len * sizeof(type) < cceSmallDataSize || lcalBlockNum == rankSize) { \
            if (rankSize == quickOneshotRankSize && lcalBlockNum != rankSize) { \
                TileXRAllReduce2npuRead<type>(ALLREDUCE_ARGS_CALL(type)); \
            } else { \
                TileXRAllReduceTwoShot<type>(ALLREDUCE_ARGS_CALL_16P(type)); \
            } \
        } else { \
            TileXRAllReduceBigData<type>(ALLREDUCE_ARGS_CALL_16P(type)); \
        } \
    } \
    TILEXR_COARSE_PERF_END(coarsePerf); \
    } \
}

#define LCCL_ALL2ALL_FUNC_AUTO_DEF(type, suffix) \
extern "C" __global__ __aicore__ void TileXRAll2All_##type##suffix(KERNELS_ARGS_FUN()) { \
    if ASCEND_IS_AIV { \
    GET_COMM_ARGS; \
    TILEXR_COARSE_PERF_BEGIN(coarsePerf); \
    __gm__ type * shareAddrs[TILEXR_MAX_RANK_SIZE]; \
    GET_IPC_MEM_ARGS(type); \
    constexpr int32_t smallRankSize = 8; \
    if (op != 0 && root != 0) { \
        TileXRAll2AllTranspose<type>(ALLREDUCE_ARGS_CALL_16P(type)); \
    } else if ((extraFlag & ExtraFlag::TOPO_910_93) != 0) { \
        if (rankSize <= smallRankSize && len * sizeof(type) > SMALL_DATA_SIZE && \
            (len * sizeof(type)) % (smallRankSize * smallRankSize * rankSize) == 0) { \
            CLASS_OP_LAUNCH(All2AllHierarchySmall, type); \
        } else { \
            CLASS_OP_LAUNCH(All2AllHierarchy, type); \
        } \
    } \
    TILEXR_COARSE_PERF_END(coarsePerf); \
    } \
}

#define LCCL_REDUCE_SCATTER_FUNC_AUTO_DEF(type, suffix) \
extern "C" __global__ __aicore__ void TileXRReduceScatter_##type##suffix(KERNELS_ARGS_FUN()) { \
    if ASCEND_IS_AIV { \
    GET_COMM_ARGS; \
    TILEXR_COARSE_PERF_BEGIN(coarsePerf); \
    constexpr int32_t quickOneshotRankSize = 2; \
    constexpr int32_t cceSmallDataSize = 2 * 1024 * 1024; \
    constexpr int32_t a3BigDataSize = 32 * 1024 * 1024; \
    constexpr int32_t a3SupportRankSize = 4; \
    constexpr int32_t smallRankSize = 8; \
    const bool isDbRing = (rankSize == a3SupportRankSize || rankSize == smallRankSize) && \
        (len * sizeof(type) * smallRankSize > cceSmallDataSize && \
        len * sizeof(type) * smallRankSize <= a3BigDataSize); \
    __gm__ type * shareAddrs[TILEXR_MAX_RANK_SIZE]; \
    GET_IPC_MEM_ARGS(type); \
    if ((extraFlag & ExtraFlag::TOPO_PCIE) != 0) { \
        TileXRReduceScatterBigDataWrite<type>(ALLREDUCE_ARGS_CALL(type)); \
    } else if ((extraFlag & ExtraFlag::TOPO_910_93) != 0 && \
        ((rankSize > smallRankSize && rankSize % quickOneshotRankSize == 0) || isDbRing)) { \
        if (isDbRing) { \
            CLASS_OP_LAUNCH(ReduceScatterHierarchyDoubleRing, type); \
        } else if (len * sizeof(type) <= SMALL_DATA_SIZE) { \
            CLASS_OP_LAUNCH(ReduceScatter, type); \
        } else { \
            CLASS_OP_LAUNCH(ReduceScatterBigData91093, type); \
        } \
    } else { \
        if (rankSize == quickOneshotRankSize && len * sizeof(type) < SIZE_OF_8M) { \
            TileXRReduceScatterWrite<type>(ALLREDUCE_ARGS_CALL(type)); \
        } else if (rankSize > quickOneshotRankSize && len * sizeof(type) < cceSmallDataSize) { \
            TileXRReduceScatter<type>(ALLREDUCE_ARGS_CALL_16P(type)); \
        } else { \
            TileXRReduceScatterBigData<type>(ALLREDUCE_ARGS_CALL_16P(type)); \
        } \
    } \
    TILEXR_COARSE_PERF_END(coarsePerf); \
    } \
}

#endif
#endif
