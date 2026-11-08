/*
 * 功能：计算 benchmark 的 SGE 地址和 warmup slot；Host 侧另计算 source/target 布局、payload 标识及保护区预期值。
 * 调用关系：bench/multiqp 使用 Host 布局初始化、校验 buffer，AIV kernel 复用地址 helper；CPU 布局测试也调用这些函数。
 * 资源与生命周期：只计算大小、地址偏移和预期值，不分配或持有 payload buffer。
 *
 * Purpose: Compute benchmark SGE addresses/warmup slots and, on Host, source/target layouts, payload tags and expected guard contents.
 * Calls: bench/multiqp initialize and verify buffers using Host helpers; AIV kernels share address helpers and CPU layout tests exercise them.
 * Lifetime: Computes sizes, offsets and expected values without allocating or owning payload buffers.
 */
#pragma once
#include <stdint.h>
// Shared by the Host layout and the scalar kernel descriptor builder. Logical
// entry zero is physically last; each multi-SGE entry has a 64-byte guard gap.
#if defined(__CCE_AICORE__) || defined(__CCE__) || defined(__NPU_ARCH__)
#define RDMA_BENCH_INLINE __aicore__ inline
#else
#define RDMA_BENCH_INLINE inline
#endif
RDMA_BENCH_INLINE uint64_t SgeSourceOffset(uint64_t source, uint32_t payload,
    uint32_t numSge, uint32_t entry)
{
    return source + uint64_t(numSge - 1U - entry) * (uint64_t(payload) + (numSge > 1U ? 64U : 0U));
}
RDMA_BENCH_INLINE uint32_t MultiQpWarmupSlot(uint32_t call, uint32_t count, uint32_t cyclicSlots)
{
    return cyclicSlots != 0U ? 1U % cyclicSlots : call % count;
}
#undef RDMA_BENCH_INLINE

// Only the scalar address helper above is needed by the pure AIV translation
// unit. Keep Host construction/verification out of device compilation.
#if !defined(__CCE_AICORE__) && !defined(__CCE__) && !defined(__NPU_ARCH__)
inline uint32_t CyclicSlots(uint64_t bytes)
{
    return bytes <= 8192 ? 32U : bytes <= 16384 ? 16U : bytes <= 32768 ? 8U :
        bytes <= 65536 ? 4U : bytes <= 131072 ? 2U : 1U;
}

constexpr uint64_t kCaseGuardWord = 0xA5A5A5A5A5A5A5A5ULL;
constexpr uint64_t kCaseLayoutVersion = 2U;

inline uint64_t CasePattern(uint32_t rank, uint32_t entry, uint32_t slot, uint32_t numSge)
{
    const uint64_t old = 0x729B36D4F8150ACEULL ^ (uint64_t(rank + 1U) * 0x9E3779B97F4A7C15ULL);
    // Keep the historical SGE1 payload byte-for-byte, including streaming slots.
    return numSge == 1U ? old : old ^ (uint64_t(entry + 1U) * 0xD6E8FEB86659FD93ULL) ^
        (uint64_t(slot + 1U) * 0xA0761D6478BD642FULL);
}

struct CaseLayout {
    // Preserve the old fields: stride is the source stride, bytes the full MR.
    uint64_t source = 4096, target, stride, bytes;
    uint32_t slots, cycles;
    uint64_t targetStride;
    uint32_t sgeCount, sgeBytes;
    uint64_t totalWqeBytes;
    CaseLayout(uint32_t payload, uint32_t count, bool streaming, uint32_t numSge = 1U)
        : stride(numSge == 1U ? uint64_t(payload) + (streaming ? 64U : 0U) :
              uint64_t(numSge) * (uint64_t(payload) + 64U)),
          slots(streaming ? count : CyclicSlots(uint64_t(payload) * numSge)), cycles(streaming ? 0U : slots),
          targetStride(numSge == 1U ? stride : uint64_t(payload) * numSge + 64U),
          sgeCount(numSge), sgeBytes(payload), totalWqeBytes(uint64_t(payload) * numSge)
    {
        target = (source + uint64_t(slots) * stride + 8191U) & ~uint64_t(4095U);
        bytes = target + uint64_t(slots) * targetStride + 4096U;
    }
    uint64_t SourceOffset(uint32_t slot, uint32_t entry) const
    {
        return SgeSourceOffset(source + uint64_t(slot) * stride, sgeBytes, sgeCount, entry);
    }
};

// Expected contents of every aligned 64-bit word, including untouched cyclic
// slots, source holes, target gaps and the leading/inter-region/trailing guards.
inline uint64_t ExpectedCaseWord(const CaseLayout& layout, uint64_t off,
    uint32_t rank, uint32_t count, uint32_t direction)
{
    if (off >= layout.source && off < layout.source + uint64_t(layout.slots) * layout.stride) {
        const uint64_t relative = off - layout.source;
        const uint64_t within = relative % layout.stride;
        const uint64_t pitch = uint64_t(layout.sgeBytes) + (layout.sgeCount > 1U ? 64U : 0U);
        const uint32_t physical = uint32_t(within / pitch);
        if (physical < layout.sgeCount && within % pitch < layout.sgeBytes) {
            return CasePattern(rank, layout.sgeCount - 1U - physical,
                uint32_t(relative / layout.stride), layout.sgeCount);
        }
    }
    if ((direction == 2U || direction == 1U - rank) && off >= layout.target &&
        off < layout.target + uint64_t(layout.slots) * layout.targetStride) {
        const uint64_t relative = off - layout.target;
        const uint64_t within = relative % layout.targetStride;
        const uint32_t slot = uint32_t(relative / layout.targetStride);
        const bool written = layout.cycles == 0U || count >= layout.cycles || (slot > 0U && slot <= count);
        if (within < layout.totalWqeBytes && written) {
            return CasePattern(1U - rank, uint32_t(within / layout.sgeBytes), slot, layout.sgeCount);
        }
    }
    return kCaseGuardWord;
}
#endif
