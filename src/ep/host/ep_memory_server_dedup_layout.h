#ifndef TILEXR_EP_MEMORY_SERVER_DEDUP_LAYOUT_H
#define TILEXR_EP_MEMORY_SERVER_DEDUP_LAYOUT_H

#include <cstddef>
#include <cstdint>

namespace TileXREp {

struct EpMemoryServerDedupLayout {
    uint64_t globalBs = 0;
    uint64_t localExpertNum = 0;
    uint64_t capacityRows = 0;
    uint64_t xBytes = 0;
    uint64_t expertIdsBytes = 0;
    uint64_t weightBytes = 0;
    uint64_t expandXBytes = 0;
    uint64_t expertTokenNumsBytes = 0;
    uint64_t sendCountsBytes = 0;
    uint64_t countStateBytes = 0;
    uint64_t assistExBytes = 0;
    uint64_t sourceMaskBytes = 0;
    uint64_t groupRowStrideBytes = 0;
    uint64_t groupBytes = 0;
};

// Caller-side allocation records only. The public bare-pointer API cannot infer these sizes.
struct EpServerDedupAllocation {
    const void *data = nullptr;
    uint64_t bytes = 0;
    uint64_t requiredBytes = 0;
    uint32_t alignment = 1;
    int32_t deviceId = -1;
};

int TileXREpBuildMemoryServerDedupLayout(int64_t rankSize, int64_t bs, int64_t h,
    int64_t topK, int64_t moeExpertNum, EpMemoryServerDedupLayout *out);
int TileXREpValidateServerDedupAllocations(const EpServerDedupAllocation *allocations,
    std::size_t count, int32_t deviceId);

} // namespace TileXREp

#endif // TILEXR_EP_MEMORY_SERVER_DEDUP_LAYOUT_H
