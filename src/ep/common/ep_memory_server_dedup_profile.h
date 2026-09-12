#ifndef TILEXR_EP_MEMORY_SERVER_DEDUP_PROFILE_H
#define TILEXR_EP_MEMORY_SERVER_DEDUP_PROFILE_H

#include <cstddef>
#include <cstdint>
#include "ep_memory_server_dedup_count_layout.h"

namespace TileXREp {
constexpr uint32_t kEpServerDedupProfileMarker = 0x53445046U;
constexpr uint16_t kEpServerDedupProfileVersion = 2U;
constexpr uint32_t kEpServerDedupProfilePoints = 7U;

// Count-only increment: an independent trace area immediately follows both
// count banks and the reserved relay counter. Payload layout must reserve or
// relocate this area when payload processing is implemented.
struct alignas(64) EpServerDedupProfileRecord {
    uint32_t marker;
    uint16_t version;
    uint16_t recordBytes;
    uint32_t rank;
    uint32_t core;
    uint32_t blockDim;
    uint32_t reserved;
    uint64_t generation;
    uint64_t time[kEpServerDedupProfilePoints];
    uint64_t padding[5];
};
constexpr uint64_t kEpServerDedupProfileBytes =
    kEpServerDedupCoreNum * sizeof(EpServerDedupProfileRecord);
static_assert(sizeof(EpServerDedupProfileRecord) == 128U, "count trace record ABI");
static_assert(offsetof(EpServerDedupProfileRecord, time) == 32U, "count trace timestamp ABI");
} // namespace TileXREp
#endif
