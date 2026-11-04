#ifndef TILEXR_EP_MEMORY_SERVER_DEDUP_LAYOUT_H
#define TILEXR_EP_MEMORY_SERVER_DEDUP_LAYOUT_H

#include <cstddef>
#include <cstdint>
#include "ep_memory_server_dedup_data_layout.h"

namespace TileXREp {

struct EpMemoryServerDedupLayout {
    uint64_t globalBs = 0;
    uint64_t localExpertNum = 0;
    uint64_t capacityRows = 0;
    uint64_t xBytes = 0;
    uint64_t expertIdsBytes = 0;
    uint64_t weightBytes = 0;
    uint64_t expandXBytes = 0;
    uint64_t dynamicScalesBytes = 0;
    uint64_t expertTokenNumsBytes = 0;
    uint64_t sendCountsBytes = 0;
    uint64_t countStateBytes = 0;
    uint64_t tokenTypeBytes = 0;
    uint64_t destinationIndexBytes = 0;
    uint64_t relayReadIndexRowStrideBytes = 0;
    uint64_t relayReadIndexBytes = 0;
    uint64_t sourceMaskBytes = 0;
    EpServerDedupDataLayout data {};
};

// Caller-side allocation records only. The public bare-pointer API cannot infer these sizes.
struct EpServerDedupAllocation {
    const void *data = nullptr;
    uint64_t bytes = 0;
    uint64_t requiredBytes = 0;
    uint32_t alignment = 1;
    int32_t deviceId = -1;
};

// Host-only layout validation; the device entry uses the compile-time core split.
int TileXREpBuildMemoryServerDedupLayout(int64_t rankSize, int64_t bs, int64_t h, int64_t topK, int64_t moeExpertNum,
    EpMemoryServerDedupLayout *out, bool quantized = false);
int TileXREpValidateServerDedupAllocations(
    const EpServerDedupAllocation *allocations, std::size_t count, int32_t deviceId);

} // namespace TileXREp

#endif // TILEXR_EP_MEMORY_SERVER_DEDUP_LAYOUT_H
