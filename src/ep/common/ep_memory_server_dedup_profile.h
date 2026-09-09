#ifndef TILEXR_EP_MEMORY_SERVER_DEDUP_PROFILE_H
#define TILEXR_EP_MEMORY_SERVER_DEDUP_PROFILE_H

#include <cstddef>
#include <cstdint>

namespace TileXREp {

constexpr uint32_t kEpServerDedupProfileMarker = 0x53445046U;
constexpr uint16_t kEpServerDedupProfileVersion = 1U;
constexpr uint32_t kEpServerDedupProfileMaxCores = 200U;
constexpr uint64_t kEpServerDedupProfileOffset = 0U;

// Internal breakdown record in the owning rank's otherwise unused MemoryDomain data region.
// One cache line per AIV; consecutive launches overwrite it, leaving the final generation.
// Timestamps are raw GetSystemCycle ticks. Conversion requires a target-verified frequency.
struct alignas(64) EpServerDedupProfileRecord {
    uint32_t marker;
    uint16_t version;
    uint16_t recordBytes;
    uint32_t rank;
    uint32_t core;
    uint32_t blockDim;
    uint32_t reserved;
    uint64_t generation;
    uint64_t start;
    uint64_t checked;
    uint64_t end;
    uint64_t reserved2;
};

static_assert(sizeof(EpServerDedupProfileRecord) == 64U, "Profile record occupies one cache line");
static_assert(alignof(EpServerDedupProfileRecord) == 64U, "Profile record cache-line alignment");
static_assert(offsetof(EpServerDedupProfileRecord, rank) == 8U, "Profile rank offset");
static_assert(offsetof(EpServerDedupProfileRecord, generation) == 24U, "Profile generation offset");
static_assert(offsetof(EpServerDedupProfileRecord, start) == 32U, "Profile start offset");
static_assert(offsetof(EpServerDedupProfileRecord, checked) == 40U, "Profile checked offset");
static_assert(offsetof(EpServerDedupProfileRecord, end) == 48U, "Profile end offset");
static_assert(offsetof(EpServerDedupProfileRecord, reserved2) == 56U, "Profile reserved tail offset");

} // namespace TileXREp

#endif // TILEXR_EP_MEMORY_SERVER_DEDUP_PROFILE_H
