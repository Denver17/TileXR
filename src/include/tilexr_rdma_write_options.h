/* Copyright (c) 2026 TileXR Project. Licensed under the Apache License, Version 2.0. */

/*
 * 功能：定义 WRITE 的批次、stride、在途窗口和轮询参数，按 SGE 数计算 UB scratch 大小。
 * 调用关系：Host 代码和 tilexr_rdma_write.h 共用这些参数；大小计算引用 pipeline 常量，不依赖 CANN 头文件。
 *
 * Purpose: Define WRITE batch, stride, in-flight and polling options, and size UB scratch by SGE count.
 * Calls: Shared by Host code and tilexr_rdma_write.h; sizing uses pipeline constants without CANN headers.
 */
#ifndef TILEXR_RDMA_WRITE_OPTIONS_H
#define TILEXR_RDMA_WRITE_OPTIONS_H

// Host-safe: shares the existing pipeline constants without CANN headers.
#include "tilexr_rdma_pipeline.h"

namespace TileXR {

struct RDMAWriteOptions {
    uint32_t count = 1U;
    uint64_t localStride = 0U;
    uint64_t remoteStride = 0U;
    uint32_t postBatch = 64U;
    uint32_t maxInFlight = 128U;
    uint32_t cyclicSlots = 0U;
    uint32_t maxPolls = 1000000U;
};

// Caller-owned UB, aligned to TILEXR_RDMA_DEVICE_SCRATCH_ALIGNMENT (32 bytes).
// A single SGE uses the single-segment pipeline and its smaller allocation.
#if defined(__CCE_AICORE__) || defined(__CCE__) || defined(__NPU_ARCH__)
__aicore__
#endif
constexpr uint32_t RDMAWriteScratchBytes(uint32_t numSge = 1U)
{
    return numSge == 1U ? RdmaWire::kPipelineScratchBytes :
        (numSge >= 2U && numSge <= TILEXR_RDMA_DEVICE_MAX_SGE ? RdmaWire::kPipelineSgeScratchBytes : 0U);
}

} // namespace TileXR
#endif
