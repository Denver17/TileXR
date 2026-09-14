#ifndef TILEXR_EP_MEMORY_SERVER_DEDUP_PROFILE_H
#define TILEXR_EP_MEMORY_SERVER_DEDUP_PROFILE_H

#include <cstddef>
#include <cstdint>
#include "ep_memory_server_dedup_count_layout.h"

namespace TileXREp {
constexpr uint32_t kEpServerDedupProfileMarker = 0x53445046U;
constexpr uint16_t kEpServerDedupProfileVersion = 6U;
constexpr uint32_t kEpServerDedupProfilePoints = 12U;
constexpr uint32_t kEpServerDedupProfileSegments = 3U;

// R07-01 reserves 200 rows of 64B. Each core uses three rows: nine meaningful
// timestamps followed by three copies of end, retaining the 64B record ABI.
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

enum class EpDedupDetailPoint : uint32_t {
    RecvBegin, PrefixEnd, TasksEnd, PrepareEnd, LoopEnd, DrainEnd, ClearIssueEnd, ClearEnd,
    FinishBegin, LocalEnd, SyncEnd, CounterEnd, PublishEnd, WaitEnd
};
enum class EpDedupDetailMetric : uint32_t {
    Probe, AcquireLoad, Metadata, Atomic, OutputIssue, RelayIssue, RoundPeerWait
};
enum class EpDedupDetailCounter : uint32_t {
    Rows, ProbeBatches, ProbeSlots, EmptyProbeBatches, FirstHopRows, RelayPackets,
    Lanes, Blocks, RoundPolls, RoundSlowestPeer
};
struct EpServerDedupDetailMetric {
    uint64_t totalTicks, maxBegin, maxEnd, count;
};
// Fixed diagnostic state: sums plus the slowest real interval for each operation.
// Atomic is a subset of Metadata; totals must not be laid out as a fake timeline.
struct alignas(64) EpServerDedupDetailRecord {
    uint32_t marker;
    uint16_t version, recordBytes;
    uint32_t rank, core;
    uint64_t generation, reserved;
    uint64_t points[16];
    EpServerDedupDetailMetric metrics[7];
    uint64_t counters[16];
};
constexpr uint64_t kEpServerDedupDetailBytes = kEpServerDedupCoreNum * sizeof(EpServerDedupDetailRecord);
static_assert(sizeof(EpServerDedupDetailRecord) == 512U, "detail record ABI and cache-line isolation");

#if TILEXR_ASCENDC_AICORE_COMPILE
FORCE_INLINE_AICORE
#else
inline
#endif
uint64_t TileXREpServerDedupDetailOffset(uint64_t sendOffset)
{
    return sendOffset >= kEpServerDedupDetailBytes ? sendOffset - kEpServerDedupDetailBytes : 0U;
}

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
