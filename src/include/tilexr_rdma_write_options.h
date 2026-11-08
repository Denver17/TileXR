/* Copyright (c) 2026 TileXR Project. Licensed under the Apache License, Version 2.0. */

/*
 * 功能：定义 WRITE 参数，并按 SGE 数和 pending 容量计算 work scratch、context 与兼容入口所需的 UB 大小。
 * 调用关系：Host 和 AIV 使用方共用参数及大小计算；常量来自 pipeline，本文件不依赖 CANN kernel 头文件。
 * 资源与生命周期：这里只计算大小；调用方分配 UB，显式 context 的 storage 须跨 Write/Wait 调用保留。
 *
 * Purpose: Define WRITE options and size UB work scratch, contexts and compatibility scratch from SGE count and pending capacity.
 * Calls: Shared by Host/AIV consumers; sizing uses pipeline constants without CANN kernel headers.
 * Lifetime: Calculates sizes only. Callers allocate UB and retain explicit context storage across Write/Wait calls.
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
    uint32_t maxInFlight = UINT32_MAX; // no additional application cap on the QP-derived window.
    uint32_t cyclicSlots = 0U;
    uint32_t maxPolls = 1000000U;
};

// Caller-owned UB, aligned to TILEXR_RDMA_DEVICE_SCRATCH_ALIGNMENT (32 bytes).
// A single SGE uses the single-segment pipeline and its smaller allocation.
#if defined(__CCE_AICORE__) || defined(__CCE__) || defined(__NPU_ARCH__)
__aicore__
#endif
constexpr uint32_t RDMAWriteWorkScratchBytes(uint32_t numSge = 1U)
{
    return numSge == 1U ? RdmaWire::kPipelineScratchBytes :
        (numSge >= 2U && numSge <= TILEXR_RDMA_DEVICE_MAX_SGE ? RdmaWire::kPipelineSgeScratchBytes : 0U);
}

// The context owns one 64B header and the actual QP pending FIFO in UB.
#if defined(__CCE_AICORE__) || defined(__CCE__) || defined(__NPU_ARCH__)
__aicore__
#endif
constexpr uint32_t RDMAWriteContextBytes(uint32_t pendingCapacity)
{
    return pendingCapacity > 0U && pendingCapacity <= (UINT32_MAX - 95U) / 8U ?
        (64U + pendingCapacity * 8U + 31U) & ~31U : 0U;
}

// Compatibility entries borrow the suffix for a temporary context. Pass the
// actual pendingCapacity when it exceeds the default; explicit context users
// allocate RDMAWriteWorkScratchBytes() and RDMAWriteContextBytes() separately.
#if defined(__CCE_AICORE__) || defined(__CCE__) || defined(__NPU_ARCH__)
__aicore__
#endif
constexpr uint32_t RDMAWriteScratchBytes(uint32_t numSge = 1U,
    uint32_t pendingCapacity = TILEXR_RDMA_DEVICE_DEFAULT_MAX_IN_FLIGHT)
{
    return RDMAWriteWorkScratchBytes(numSge) != 0U && RDMAWriteContextBytes(pendingCapacity) != 0U &&
        RDMAWriteContextBytes(pendingCapacity) <= UINT32_MAX - RDMAWriteWorkScratchBytes(numSge) ?
        RDMAWriteWorkScratchBytes(numSge) + RDMAWriteContextBytes(pendingCapacity) : 0U;
}

} // namespace TileXR
#endif
