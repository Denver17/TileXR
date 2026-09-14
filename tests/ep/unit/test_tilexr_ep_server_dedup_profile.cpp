#include "server_dedup_profile_capture.h"

namespace AscendC { uint32_t testCore = 0; uint64_t testCopies = 0; }
int main()
{
    using namespace TileXREp;
    using ServerDedupTest::Check;
    TileXR::CommArgs args {};
    ServerDedupTest::ProfileCapture capture(args, 3U);
    const auto syncBefore = AscendC::TestSyncOperations();
    for (uint32_t launch = 0; launch < 3U; ++launch) {
        for (uint32_t core = 0; core < 64U; ++core) {
            AscendC::testCore = core;
            ServerDedupProfile profile;
            profile.Init(reinterpret_cast<GM_ADDR>(&args), launch);
            profile.Mark(0U);
            profile.Mark(EpDedupDetailPoint::RecvBegin);
            for (uint32_t i = 0; i < 3U; ++i) {
                profile.Begin(EpDedupDetailMetric::Metadata);
                profile.Begin(EpDedupDetailMetric::Atomic);
                profile.End(EpDedupDetailMetric::Atomic);
                profile.End(EpDedupDetailMetric::Metadata);
            }
            profile.Mark(8U);
            profile.Publish();
            const auto events = capture.Events(core, launch);
            Check(events.size() == 15U, "all nested/repeated observations survive without local aggregation");
            Check(events.front() == kEpServerDedupMainEventBase && events.back() == kEpServerDedupMainEventBase + 8U,
                "each launch retains its own total boundaries");
            for (uint32_t i = 0; i < 3U; ++i) {
                Check(events[2U + 4U*i] == kEpServerDedupOperationEventBase + 4U &&
                    events[3U + 4U*i] == kEpServerDedupOperationEventBase + 6U &&
                    events[4U + 4U*i] == kEpServerDedupOperationEventBase + 7U &&
                    events[5U + 4U*i] == kEpServerDedupOperationEventBase + 5U,
                    "atomic intervals are nested inside the same metadata invocation");
            }
            Check(capture.counts[core * 8U] == launch * capture.recordsPerLaunch + 16U,
                "publication aligns standard records without overwriting a previous launch");
        }
    }
    Check(AscendC::TestSyncOperations() == syncBefore && AscendC::testCopies == 0U,
        "marker and publication introduce no UB data movement, barriers or events");
    ServerDedupProfile disabled;
    disabled.Init(nullptr, 0U);
    disabled.Mark(0U);
    disabled.Publish();
    return ServerDedupTest::Finish("profile");
}
