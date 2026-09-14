#ifndef TILEXR_EP_SERVER_DEDUP_DEVICE_PROFILE_H
#define TILEXR_EP_SERVER_DEDUP_DEVICE_PROFILE_H

#include "kernel_operator.h"
#include "ep_memory_server_dedup_profile.h"
#include "tilexr_profiling.h"

namespace TileXREp {
// Only event-ID translation lives here; the standard writer owns all state.
// Markers are passive Scalar observations, with no additional pipe barriers.
class ServerDedupProfile {
public:
    __aicore__ inline void Init(GM_ADDR commArgs, uint32_t launchSlot)
    {
        writer_.InitializeLatest(commArgs, AscendC::GetBlockIdx(), launchSlot,
            kEpServerDedupProfilingRecordsPerLaunch);
    }
    __aicore__ inline void Mark(uint32_t point)
    {
        writer_.Record(kEpServerDedupMainEventBase + point);
    }
    __aicore__ inline void Mark(EpDedupDetailPoint point)
    {
        writer_.Record(kEpServerDedupDetailEventBase + uint32_t(point));
    }
    __aicore__ inline void Begin(EpDedupDetailMetric operation)
    {
        writer_.Record(kEpServerDedupOperationEventBase + 2U * uint32_t(operation));
    }
    __aicore__ inline void End(EpDedupDetailMetric operation)
    {
        writer_.Record(kEpServerDedupOperationEventBase + 2U * uint32_t(operation) + 1U);
    }
    __aicore__ inline void PacketIssue() { writer_.Record(kEpServerDedupPacketIssueEvent); }
    __aicore__ inline void Publish() { writer_.Publish(); }
private:
    TileXR::DirectProfilingWriter writer_;
};
} // namespace TileXREp
#endif
