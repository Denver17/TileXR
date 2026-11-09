/*
 * 实现单 owner 的 TryAllocateRing：按实际 bytes 处理对齐、环尾绕回和容量检查，
 * 成功后提交游标、地址及 Handle。tilexr_elastic_memory_device.h 用它完成 Device 分配，
 * pool.cpp 用同一算法校验提交结果；reclaim 水位由 Host 根据 lease 生命周期提供。
 */
#ifndef TILEXR_ELASTIC_MEMORY_ALLOCATOR_H
#define TILEXR_ELASTIC_MEMORY_ALLOCATOR_H
#include "tilexr_elastic_memory_abi.h"
#ifndef TILEXR_ELASTIC_INLINE
#define TILEXR_ELASTIC_INLINE inline
#define TILEXR_ELASTIC_UNDEF_INLINE
#endif
namespace TileXR { namespace Elastic {
TILEXR_ELASTIC_INLINE bool IsEmpty(const Handle& handle)
{
    return handle.segmentId == 0 && handle.offset == 0 && handle.bytes == 0 && handle.generation == 0;
}
TILEXR_ELASTIC_INLINE bool SameHandle(const Handle& a, const Handle& b)
{
    return a.segmentId == b.segmentId && a.offset == b.offset && a.bytes == b.bytes &&
           a.generation == b.generation;
}
TILEXR_ELASTIC_INLINE Error TryAllocateRing(RingState& state, uint64_t bytes, uint64_t alignment,
                                          uint64_t generation, Handle* handle, uint64_t* address)
{
    if (handle != nullptr) *handle = Handle{};
    if (address != nullptr) *address = 0;
    if (handle == nullptr || address == nullptr || alignment == 0 ||
        (alignment & (alignment - 1)) != 0) return Invalid;
    if (bytes == 0) return Success;
    if (generation == 0) return Invalid;
    const Segment& segment = state.segment;
    if (segment.id == 0 && segment.address == 0 && segment.bytes == 0 &&
        state.head == 0 && state.tail == 0 && state.reclaim == 0 &&
        state.tailOffset == 0 && state.alignment == 0) return Capacity;
    if (segment.id == 0 || segment.address == 0 || segment.bytes == 0 ||
        segment.address > UINT64_MAX - segment.bytes ||
        state.head > state.reclaim || state.reclaim > state.tail ||
        state.tail - state.head > segment.bytes || state.tailOffset >= segment.bytes) return Invalid;

    RingState candidate = state;
    candidate.head = candidate.reclaim;
    if (candidate.head == candidate.tail) {
        // Only an empty ring can rebase without changing any live reservation coordinates.
        candidate.head = candidate.tail = candidate.reclaim = candidate.tailOffset = 0;
    }
    const uint64_t available = segment.bytes - (candidate.tail - candidate.head);
    const uint64_t suffix = segment.bytes - candidate.tailOffset;
    const uint64_t mask = alignment - 1;
    // Unsigned negation computes alignment padding without overflowing start + mask.
    uint64_t padding = (uint64_t(0) - (segment.address + candidate.tailOffset)) & mask;
    uint64_t offset = candidate.tailOffset;
    if (padding > suffix || bytes > suffix - padding) {
        const uint64_t basePadding = (uint64_t(0) - segment.address) & mask;
        if (suffix > available || basePadding > available - suffix ||
            bytes > available - suffix - basePadding) return Capacity;
        padding = suffix + basePadding;
        offset = basePadding;
    } else {
        if (padding > available || bytes > available - padding) return Capacity;
        offset += padding;
    }
    const uint64_t reserved = padding + bytes; // bounded by available above
    if (candidate.tail > UINT64_MAX - reserved) return Invalid;
    candidate.tail += reserved;
    const uint64_t endOffset = offset + bytes; // bounded by segment.bytes above
    candidate.tailOffset = endOffset == segment.bytes ? 0 : endOffset;
    candidate.alignment = alignment;
    *handle = Handle{segment.id, offset, bytes, generation};
    *address = segment.address + offset;
    state = candidate;
    return Success;
}
} }
#ifdef TILEXR_ELASTIC_UNDEF_INLINE
#undef TILEXR_ELASTIC_INLINE
#undef TILEXR_ELASTIC_UNDEF_INLINE
#endif
#endif
