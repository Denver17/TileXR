#ifndef TILEXR_EP_MEMORY_SERVER_DEDUP_PROFILE_H
#define TILEXR_EP_MEMORY_SERVER_DEDUP_PROFILE_H

#include <cstddef>
#include <cstdint>
#include "ep_memory_server_dedup_count_layout.h"

namespace TileXREp {
constexpr uint32_t kEpServerDedupProfileMarker = 0x53445046U;
constexpr uint16_t kEpServerDedupProfileVersion = 4U;
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

// Detailed records live at the end of the payload window, BEFORE the
// communicator's 4 MiB flag region. Existing count/data offsets are unchanged.
struct EpServerDedupSendProfileHeader {
    uint32_t marker;
    uint16_t version;
    uint16_t recordBytes;
    uint32_t rank;
    uint32_t core;
    uint64_t generation;
    uint32_t packetCount;
    uint32_t firstHopCount;
};
struct EpServerDedupSendProfilePacket {
    uint64_t issue;
    uint32_t token;
    uint32_t k;
};
struct EpServerDedupSendProfileLayout {
    uint64_t offset;
    uint64_t bytes;
    uint32_t coreStride;
    uint32_t capacity;
};
static_assert(sizeof(EpServerDedupSendProfileHeader) == 32U, "send profile header ABI");
static_assert(sizeof(EpServerDedupSendProfilePacket) == 16U, "send issue record ABI");
static_assert(offsetof(EpServerDedupSendProfilePacket, token) == 8U, "send issue metadata ABI");

#if TILEXR_ASCENDC_AICORE_COMPILE
FORCE_INLINE_AICORE
#else
inline
#endif
EpServerDedupSendProfileLayout TileXREpServerDedupSendProfileLayout(uint64_t peerBytes, uint32_t bs, uint32_t k)
{
    EpServerDedupSendProfileLayout layout {};
    layout.capacity = uint32_t((uint64_t(bs) + kEpServerDedupSendCoreNum - 1U) / kEpServerDedupSendCoreNum * k);
    layout.coreStride = uint32_t((sizeof(EpServerDedupSendProfileHeader) +
        uint64_t(layout.capacity) * sizeof(EpServerDedupSendProfilePacket) + 63U) / 64U * 64U);
    layout.bytes = (uint64_t(layout.coreStride) * kEpServerDedupSendCoreNum + 511U) / 512U * 512U;
    constexpr uint64_t flagBytes = 4ULL * 1024U * 1024U;
    const uint64_t payloadEnd = peerBytes >= flagBytes ? (peerBytes - flagBytes) / 512U * 512U : 0U;
    // Zero offset denotes insufficient capacity; Host rejects overlap with data.
    layout.offset = payloadEnd >= layout.bytes ? payloadEnd - layout.bytes : 0U;
    return layout;
}
} // namespace TileXREp
#endif
