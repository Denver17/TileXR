#ifndef TILEXR_EP_COMBINE_MEMORY_BOUNDARY_TEST_H
#define TILEXR_EP_COMBINE_MEMORY_BOUNDARY_TEST_H

#include <cstdint>
#include "ep_memory_server_dedup_buffer_layout.h"
#if defined(__CCE__) && defined(__CCE_IS_AICORE__)
#define TILEXR_BOUNDARY_INLINE __attribute__((always_inline)) inline __aicore__
#else
#include "acl/acl.h"
#define TILEXR_BOUNDARY_INLINE inline
#endif

namespace TileXRCombineMemoryBoundaryTest {
constexpr uint64_t kPeerBytes = UINT64_C(8) * 1024 * 1024 * 1024;
constexpr uint64_t kWindowBytes = kPeerBytes / 2;
constexpr uint64_t kRuntimeReservedBytes = UINT64_C(4) * 1024 * 1024;
constexpr uint64_t kLegalWindowBytes = kWindowBytes - kRuntimeReservedBytes;
constexpr uint32_t kProbeBytes = 32;
constexpr uint32_t kResultBytes = 64;
constexpr uint32_t kWindowCount = 2;
constexpr uint32_t kProbesPerWindow = 4;

TILEXR_BOUNDARY_INLINE bool ValidProbe(uint32_t window, uint32_t index)
{
    return window < kWindowCount && index < kProbesPerWindow;
}
TILEXR_BOUNDARY_INLINE uint64_t ProbeOffset(uint32_t window, uint32_t index)
{
    if (!ValidProbe(window, index)) return UINT64_MAX;
    uint64_t relative = TILEXR_EP_SERVER_DEDUP_COMBINE_CONTROL_OFFSET;
    if (index == 1) relative += TILEXR_EP_SERVER_DEDUP_COMBINE_CONTROL_BYTES - kProbeBytes;
    if (index == 2) relative = TILEXR_EP_SERVER_DEDUP_SHARED_DATA_OFFSET;
    if (index == 3) relative = kLegalWindowBytes - kProbeBytes;
    return uint64_t(window) * kWindowBytes + relative;
}
TILEXR_BOUNDARY_INLINE uint32_t ProbePattern(uint32_t window, uint32_t index)
{
    return ValidProbe(window, index) ? (0xC7000101U + window * 0x100U + index * 0x10U) : 0U;
}
// Guards may read the layout's alignment gap, but neither half's trailing 4MiB.
TILEXR_BOUNDARY_INLINE bool LegalRange(uint64_t offset, uint64_t bytes)
{
    if (offset >= kPeerBytes || bytes == 0) return false;
    const uint64_t relative = offset % kWindowBytes;
    return relative < kLegalWindowBytes && bytes <= kLegalWindowBytes - relative;
}
TILEXR_BOUNDARY_INLINE uint64_t GuardBeforeOffset(uint32_t window, uint32_t index)
{
    return ValidProbe(window, index) ? ProbeOffset(window, index) - kProbeBytes : UINT64_MAX;
}

#if !defined(__CCE__) || !defined(__CCE_IS_AICORE__)
// One registered single-AIV probe. result[0:32] is initial GM data, result[32:64]
// is the read after writing ProbePattern repeatedly as eight uint32 words.
// This test entry neither allocates a communicator nor consumes its magic.
int Launch(void *commArgs, void *result, uint32_t window, uint32_t index, aclrtStream stream);
#endif
}
#undef TILEXR_BOUNDARY_INLINE
#endif
