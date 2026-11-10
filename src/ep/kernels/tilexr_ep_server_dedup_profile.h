/*
 * 将 ServerDedup Dispatch 的业务阶段转换为 event ID，直接调用 SDK DirectWriter；Host 持有 capture，kernel 末尾提交本 lane。
 */
#ifndef TILEXR_EP_SERVER_DEDUP_DEVICE_PROFILE_H
#define TILEXR_EP_SERVER_DEDUP_DEVICE_PROFILE_H

#include "kernel_operator.h"
#include "ep_memory_server_dedup_profile.h"
#include "device/tilexr_ep_profiling.h"

namespace TileXREp {
// Only event-ID translation lives here; the standard writer owns all state.
// Markers are passive Scalar observations, with no additional pipe barriers.
class ServerDedupProfile {
public:
    __aicore__ inline void Init(GM_ADDR commArgs, int64_t magic)
    {
        GM_ADDR descriptor = ProfileDescriptor(commArgs, magic, AscendC::GetBlockNum());
        enabled_ = descriptor != nullptr;
        writer_.Initialize(descriptor, AscendC::GetBlockIdx());
    }
    __aicore__ inline void Mark(uint32_t point)
    {
        Record(kEpServerDedupMainEventBase + point);
    }
    __aicore__ inline void Mark(EpDedupDetailPoint point)
    {
        Record(kEpServerDedupDetailEventBase + uint32_t(point));
    }
    __aicore__ inline void Begin(EpDedupDetailMetric operation)
    {
        Record(kEpServerDedupOperationEventBase + 2U * uint32_t(operation));
    }
    __aicore__ inline void End(EpDedupDetailMetric operation)
    {
        Record(kEpServerDedupOperationEventBase + 2U * uint32_t(operation) + 1U);
    }
    __aicore__ inline void PacketIssue()
    {
        Record(kEpServerDedupPacketIssueEvent);
    }
    __aicore__ inline void Publish()
    {
        writer_.Publish();
    }

private:
    __aicore__ inline void Record(uint32_t event)
    {
#if defined(TILEXR_ENABLE_PROFILING) && TILEXR_ENABLE_PROFILING == 1
        if (enabled_) {
            writer_.RecordAt(event, static_cast<uint64_t>(AscendC::GetSystemCycle()));
        }
#else
        (void)event;
#endif
    }
    bool enabled_ = false;
    TileXR::Profiling::DirectWriter writer_;
};
} // namespace TileXREp
#endif
