#ifndef TILEXR_EP_MEMORY_SERVER_DEDUP_PROFILE_H
#define TILEXR_EP_MEMORY_SERVER_DEDUP_PROFILE_H

#include <cstddef>
#include <cstdint>
#include "ep_memory_server_dedup_count_layout.h"

namespace TileXREp {
constexpr uint32_t kEpServerDedupProfileMarker = 0x53445046U;
constexpr uint16_t kEpServerDedupProfileVersion = 3U;
constexpr uint32_t kEpServerDedupProfilePoints = 8U;
constexpr uint32_t kEpServerDedupProfileSegments = 2U;

// R07-01 reserves 200 rows of 64B at offset zero. Each of 64 cores uses two rows.
struct alignas(64) EpServerDedupProfileRecord {
    uint32_t marker;
    uint16_t version;
    uint16_t recordBytes;
    uint32_t rank;
    uint32_t core;
    uint32_t blockDim;
    uint32_t segment;
    uint64_t generation;
    uint64_t time[kEpServerDedupProfilePoints / kEpServerDedupProfileSegments];
};
constexpr uint64_t kEpServerDedupProfileBytes =
    kEpServerDedupCoreNum * kEpServerDedupProfileSegments * sizeof(EpServerDedupProfileRecord);
static_assert(sizeof(EpServerDedupProfileRecord) == 64U, "trace row ABI");
static_assert(kEpServerDedupProfileBytes <= kEpServerDedupProfileReservedBytes, "trace stays within reserved profile region");
static_assert(offsetof(EpServerDedupProfileRecord, time) == 32U, "count trace timestamp ABI");
} // namespace TileXREp
#endif
