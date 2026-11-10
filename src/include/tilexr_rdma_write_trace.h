/* Copyright (c) 2026 TileXR Project. Licensed under the Apache License, Version 2.0. */

/*
 * 功能：定义 WRITE 阶段的 event ID，并在启用 RDMA profiling 时记录 scope 的起止事件。
 * 调用关系：WRITE adapter、device context 和 pipeline 调用 trace；事件通过现有 Profiling::DirectWriter 写入 GM lane。
 * 资源与生命周期：trace 借用 writer 指针；调用方初始化 writer、保持目标 buffer 有效，并在记录结束后调用 Publish。
 *
 * Purpose: Define WRITE-stage event IDs and record scope boundaries when RDMA profiling is enabled.
 * Calls: WRITE adapters, device contexts and pipelines use the existing Profiling::DirectWriter to append events to a GM lane.
 * Lifetime: Borrows the writer pointer; callers initialize it, retain its target buffers and call Publish after recording.
 */
#ifndef TILEXR_RDMA_WRITE_TRACE_H
#define TILEXR_RDMA_WRITE_TRACE_H
#include <cstdint>
#include "tilexr_rdma_debug.h"
#if TILEXR_RDMA_PROFILE_ACTIVE
#include <tilexr/profiling.h>
#endif
#if defined(__CCE_AICORE__) || defined(__CCE__) || defined(__NPU_ARCH__)
#define TILEXR_RDMA_TRACE_INLINE __aicore__ inline
#else
#define TILEXR_RDMA_TRACE_INLINE inline
#endif
namespace TileXR {
enum class RDMAWriteEvent : uint32_t {
    TotalBegin = 1000, TotalEnd,
    WriteBegin = 1010, WriteEnd, SgeBegin, SgeEnd, MultiBegin, MultiEnd, SgeMultiBegin, SgeMultiEnd,
    ViewBegin = 1030, ViewEnd, ValidateBegin, ValidateEnd, StateBegin, StateEnd,
    EncodeBegin = 1040, EncodeEnd, StopBegin, StopEnd, TailBegin, TailEnd, HeadBegin, HeadEnd,
    DoorbellBegin = 1050, DoorbellEnd, BackpressureBegin, BackpressureEnd, WaitBegin, WaitEnd,
    RestoreBegin = 1060, RestoreEnd, SaveBegin, SaveEnd,
    ApiBegin = 1070, ApiEnd,
    WarmupBegin = 1080, WarmupEnd, IssueBegin, IssueEnd
};
class RDMAWriteTrace {
public:
#if TILEXR_RDMA_PROFILE_ACTIVE
    TILEXR_RDMA_TRACE_INLINE void Attach(Profiling::DirectWriter& writer) { writer_ = &writer; }
#endif
    TILEXR_RDMA_TRACE_INLINE void Mark(RDMAWriteEvent event)
    {
#if TILEXR_RDMA_PROFILE_ACTIVE
        if (writer_ != nullptr) {
            writer_->RecordAt(static_cast<uint32_t>(event), static_cast<uint64_t>(AscendC::GetSystemCycle()));
        }
#else
        (void)event;
#endif
    }
private:
#if TILEXR_RDMA_PROFILE_ACTIVE
    Profiling::DirectWriter* writer_ = nullptr;
#endif
};
class RDMAWriteTraceScope {
public:
    TILEXR_RDMA_TRACE_INLINE RDMAWriteTraceScope(RDMAWriteTrace* trace, RDMAWriteEvent begin, RDMAWriteEvent end)
#if TILEXR_RDMA_PROFILE_ACTIVE
        : trace_(trace), end_(end)
#endif
    {
#if TILEXR_RDMA_PROFILE_ACTIVE
        if (trace_ != nullptr) { trace_->Mark(begin); }
#else
        (void)trace; (void)begin; (void)end;
#endif
    }
    TILEXR_RDMA_TRACE_INLINE ~RDMAWriteTraceScope()
    {
#if TILEXR_RDMA_PROFILE_ACTIVE
        if (trace_ != nullptr) { trace_->Mark(end_); }
#endif
    }
private:
#if TILEXR_RDMA_PROFILE_ACTIVE
    RDMAWriteTrace* trace_;
    RDMAWriteEvent end_;
#endif
};
} // namespace TileXR
#undef TILEXR_RDMA_TRACE_INLINE
#endif
