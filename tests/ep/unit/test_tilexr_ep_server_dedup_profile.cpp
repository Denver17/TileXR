#include "server_dedup_profile_capture.h"

namespace AscendC { uint32_t testCore = 0; uint64_t testCopies = 0; }
int main()
{
    using namespace TileXREp;
    using ServerDedupTest::Check;
    TileXR::CommArgs args {};
    ServerDedupTest::ProfileCapture capture(args);
    const auto syncBefore = AscendC::TestSyncOperations();
    for (uint32_t launch = 0; launch < 3U; ++launch) {
        for (uint32_t core = 0; core < 64U; ++core) {
            AscendC::testCore = core;
            const uint64_t previousHead = capture.timestamps[uint64_t(core) * capture.recordsPerCore + 1U];
            ServerDedupProfile profile;
            profile.Init(reinterpret_cast<GM_ADDR>(&args), 0U);
            profile.Mark(0U);
            profile.Mark(EpDedupDetailPoint::RecvBegin);
            for (uint32_t i = 0; i < 3U - launch; ++i) {
                profile.Begin(EpDedupDetailMetric::Metadata);
                profile.Begin(EpDedupDetailMetric::Atomic);
                profile.End(EpDedupDetailMetric::Atomic);
                profile.End(EpDedupDetailMetric::Metadata);
            }
            profile.Mark(8U);
            profile.Publish();
            const auto events = capture.Events(core);
            Check(capture.timestamps[uint64_t(core) * capture.recordsPerCore + 1U] > previousHead,
                "each latest launch overwrites the previous timestamp at the same GM address");
            Check(events.size() == 3U + 4U * (3U - launch), "all nested/repeated observations survive without local aggregation");
            Check(events.front() == kEpServerDedupMainEventBase && events.back() == kEpServerDedupMainEventBase + 8U,
                "latest launch retains its total boundaries");
            for (uint32_t i = 0; i < 3U - launch; ++i) {
                Check(events[2U + 4U*i] == kEpServerDedupOperationEventBase + 4U &&
                    events[3U + 4U*i] == kEpServerDedupOperationEventBase + 6U &&
                    events[4U + 4U*i] == kEpServerDedupOperationEventBase + 7U &&
                    events[5U + 4U*i] == kEpServerDedupOperationEventBase + 5U,
                    "atomic intervals are nested inside the same metadata invocation");
            }
            Check(capture.counts[core * 8U] == (launch == 2U ? 8U : 16U),
                "latest publication replaces previous valid count, including shorter captures");
        }
    }
    AscendC::testCore = 0U;
    ServerDedupProfile overflow;
    overflow.Init(reinterpret_cast<GM_ADDR>(&args), 0U);
    overflow.Mark(0U);
    for (uint32_t i = 0; i < capture.recordsPerLaunch; ++i) overflow.PacketIssue();
    overflow.Mark(8U);
    overflow.Publish();
    Check(capture.Events(0U).size() == capture.recordsPerLaunch && capture.dropped[0U] == 2U,
        "full latest capture is bounded and reports loss");
    ServerDedupProfile shorter;
    shorter.Init(reinterpret_cast<GM_ADDR>(&args), 0U);
    Check(capture.counts[0U] == 0U && capture.dropped[0U] == 0U,
        "latest launch resets only its published valid count and loss state");
    shorter.Mark(0U);
    shorter.Mark(8U);
    shorter.Publish();
    Check(capture.Events(0U).size() == 2U && capture.counts[0U] == 8U && capture.dropped[0U] == 0U,
        "shorter launch after overflow exposes no stale tail or previous loss");
    Check(capture.Events(1U).size() == 7U && capture.counts[8U] == 8U,
        "latest reset never changes another core capture");
    for (uint32_t slot : {1U, UINT32_MAX}) {
        ServerDedupProfile unavailable;
        unavailable.Init(reinterpret_cast<GM_ADDR>(&args), slot);
        unavailable.Mark(0U);
        unavailable.Publish();
        Check(capture.Events(0U).size() == 2U && capture.counts[0U] == 8U,
            "invalid latest slot does not reset or overwrite a published capture");
    }
    ServerDedupProfile empty;
    empty.Init(reinterpret_cast<GM_ADDR>(&args), 0U);
    empty.Publish();
    Check(capture.Events(0U).empty() && capture.counts[0U] == 0U && capture.dropped[0U] == 0U,
        "an empty latest capture cannot expose an earlier record prefix");
    Check(AscendC::TestSyncOperations() == syncBefore && AscendC::testCopies == 0U,
        "marker and publication introduce no UB data movement, barriers or events");
    ServerDedupProfile disabled;
    disabled.Init(nullptr, 0U);
    disabled.Mark(0U);
    disabled.Publish();
    return ServerDedupTest::Finish("profile");
}
