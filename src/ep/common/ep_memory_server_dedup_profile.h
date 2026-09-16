#ifndef TILEXR_EP_MEMORY_SERVER_DEDUP_PROFILE_H
#define TILEXR_EP_MEMORY_SERVER_DEDUP_PROFILE_H

#include <cstdint>

namespace TileXREp {
// Numeric catalog only. Storage and publication belong to tilexr_profiling.h.
constexpr uint32_t kEpServerDedupProfilingRecordsPerLaunch = 65536U;
constexpr uint32_t kEpServerDedupMainEventBase = 0x53000001U;
constexpr uint32_t kEpServerDedupDetailEventBase = 0x53000100U;
constexpr uint32_t kEpServerDedupOperationEventBase = 0x53000200U;
constexpr uint32_t kEpServerDedupPacketIssueEvent = 0x53000300U;

enum class EpDedupDetailPoint : uint32_t {
    RecvBegin, PrefixEnd, TasksEnd, PrepareEnd, LoopEnd, DrainEnd, ClearIssueEnd, ClearEnd,
    FinishBegin, LocalEnd, SyncEnd, CounterEnd, PublishEnd, WaitEnd // CounterEnd retired; keep ID.
};
enum class EpDedupDetailMetric : uint32_t {
    Probe, AcquireLoad, Metadata, Atomic, OutputIssue, RelayIssue, RoundPeerWait, // Atomic retired; keep ID.
    // Append only: older captures retain their existing event identities.
    LoadIssue, LoadWait, ProbeLoadFlag, ProbeCalSum
};
} // namespace TileXREp
#endif
