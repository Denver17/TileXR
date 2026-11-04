/* Copyright (c) 2026 TileXR Project. Licensed under the Apache License, Version 2.0. */

/*
 * 功能：将单段或 SGE WRITE 参数转给 pipeline；多 QP 入口仅选择调用方指定的 QP。
 * 调用关系：业务 AIV kernel 调用本文件，tilexr_rdma_device.h 执行发送，write_options 提供批次和 scratch 参数。
 * 资源与生命周期：借用 view 和 UB scratch；QP 分配、数据分片及远端 consumer 同步由应用负责。
 *
 * Purpose: Adapt single-segment or SGE WRITEs to the pipeline; multi-QP entries select the caller-specified QP.
 * Calls: Application AIV kernels use tilexr_rdma_device.h through these adapters and write_options parameters.
 * Lifetime: Borrows views and UB scratch; applications assign QPs, partition payloads and synchronize remote consumers.
 */
#ifndef TILEXR_RDMA_WRITE_H
#define TILEXR_RDMA_WRITE_H

#include "tilexr_rdma_device.h"
#include "tilexr_rdma_write_options.h"

namespace TileXR {
namespace RdmaWriteDetail {

__aicore__ inline bool ValidQpArray(const __gm__ TileXRRDMADeviceView* views,
    uint32_t qpCount, uint32_t qpIndex)
{
    const uint64_t address = reinterpret_cast<uint64_t>(views);
    return views != nullptr && (address & 7U) == 0U &&
        qpCount >= 1U && qpCount <= TILEXR_RDMA_MAX_QPS && qpIndex < qpCount &&
        RdmaWire::Span(address, uint64_t(qpCount) * sizeof(TileXRRDMADeviceView));
}

} // namespace RdmaWriteDetail

/* Synchronous WRITE batch; all offsets/strides are bytes. Zero strides reuse
 * a span; cyclicSlots follows the existing pipeline's persistent WR ordinal.
 * postBatch is 1..64 and maxInFlight must hold min(count, 2*postBatch) WRs.
 * Scratch is UB >= RDMAWriteScratchBytes(), aligned32 and live for the call.
 * The pipelines retain full-span/no-wrap admission, fresh CQ polling, ordered
 * MTE3 publication and st_dev doorbells. Entering pipeline mode is permanent
 * for this QP; do not switch back to the legacy all-signaled APIs.
 * One AIV exclusively owns each QP across regions/streams. Remote consumers
 * need application synchronization. Posted errors retain/poison the QP;
 * quiesce and destroy it before freeing potentially referenced memory. */
__aicore__ inline uint32_t RDMAWrite(const __gm__ TileXRRDMADeviceView* view,
    uint64_t localOffset, uint64_t remoteOffset, uint32_t bytes,
    const RDMAWriteOptions& options, const AscendC::LocalTensor<uint8_t>& scratch)
{
    const RdmaWire::PipelineOptions pipeline{options.postBatch, options.maxInFlight, options.cyclicSlots};
    return RDMAPutPipeline(view, localOffset, remoteOffset, bytes, options.count,
        options.localStride, options.remoteStride, pipeline, scratch, options.maxPolls);
}

/* A kernel-local SGE array gathers into one contiguous remote span per WR.
 * localStride advances every SGE. Scratch needs RDMAWriteScratchBytes(numSge).
 * The list is stable for the call and each entry's reserved field is zero. */
__aicore__ inline uint32_t RDMAWriteSge(const __gm__ TileXRRDMADeviceView* view,
    const TileXRRDMADeviceSge* sges, uint32_t numSge, uint64_t remoteOffset,
    const RDMAWriteOptions& options, const AscendC::LocalTensor<uint8_t>& scratch)
{
    if (sges == nullptr || numSge == 0U || numSge > TILEXR_RDMA_DEVICE_MAX_SGE) {
        return TILEXR_RDMA_DEVICE_INVALID;
    }
    if (numSge == 1U) {
        if (sges[0].reserved != 0U) { return TILEXR_RDMA_DEVICE_INVALID; }
        return RDMAWrite(view, sges[0].localOffset, remoteOffset, sges[0].bytes, options, scratch);
    }
    const RdmaWire::PipelineOptions pipeline{options.postBatch, options.maxInFlight, options.cyclicSlots};
    return RDMAPutSgeListPipeline(view, sges, numSge, remoteOffset, options.count,
        options.localStride, options.remoteStride, pipeline, scratch, options.maxPolls);
}

/* views names qpCount contiguous V2 views in GM, qpCount=1..16. Each call
 * selects only qpIndex, exclusively owned by the calling AIV. Launch/partition
 * AIVs explicitly; there is no QP loop, implicit barrier or payload partition.
 * Validate the complete array address span before selected-pointer arithmetic;
 * the caller supplies the actual allocation and keeps every view alive. */
__aicore__ inline uint32_t RDMAWriteMultiQp(const __gm__ TileXRRDMADeviceView* views,
    uint32_t qpCount, uint32_t qpIndex, uint64_t localOffset, uint64_t remoteOffset, uint32_t bytes,
    const RDMAWriteOptions& options, const AscendC::LocalTensor<uint8_t>& scratch)
{
    if (!RdmaWriteDetail::ValidQpArray(views, qpCount, qpIndex)) { return TILEXR_RDMA_DEVICE_INVALID; }
    return RDMAWrite(views + qpIndex, localOffset, remoteOffset, bytes, options, scratch);
}

__aicore__ inline uint32_t RDMAWriteSgeMultiQp(const __gm__ TileXRRDMADeviceView* views,
    uint32_t qpCount, uint32_t qpIndex, const TileXRRDMADeviceSge* sges, uint32_t numSge,
    uint64_t remoteOffset, const RDMAWriteOptions& options, const AscendC::LocalTensor<uint8_t>& scratch)
{
    if (!RdmaWriteDetail::ValidQpArray(views, qpCount, qpIndex)) { return TILEXR_RDMA_DEVICE_INVALID; }
    return RDMAWriteSge(views + qpIndex, sges, numSge, remoteOffset, options, scratch);
}

} // namespace TileXR
#endif
