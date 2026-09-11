#ifndef TILEXR_EP_MEMORY_BOUNDARY_TEST_H
#define TILEXR_EP_MEMORY_BOUNDARY_TEST_H

#include <cstdint>
#include "acl/acl.h"

namespace TileXRMemoryBoundaryTest {
constexpr uint64_t kPeerBytes = UINT64_C(4) * 1024 * 1024 * 1024;
constexpr uint64_t kProbeBytes = 32;
constexpr uint64_t kOffset = kPeerBytes - kProbeBytes;

// Test-only registered kernel. result[0:32] is the device's initial read;
// result[32:64] is its read after writing 0xFF to its own peer-window tail.
int Launch(void *commArgs, void *result, aclrtStream stream);
}
#endif
