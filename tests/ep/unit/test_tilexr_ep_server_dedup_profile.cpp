#include "tilexr_ep_server_dedup_profile.h"
#include "server_dedup_test_checks.h"

namespace AscendC { uint32_t testCore = 0; uint64_t testCopies = 0; }
int main()
{
    using namespace TileXREp;
    using ServerDedupTest::Check;
    const uint64_t peerBytes = 4ULL * 1024U * 1024U * 1024U, flagBytes = 4ULL * 1024U * 1024U;
    const auto detail = TileXREpServerDedupSendProfileLayout(peerBytes, 96, 6);
    Check(detail.capacity == 12 && detail.coreStride == 256 && detail.bytes == 12288,
        "BS96/TopK6 reserves every possible physical packet across all 48 send cores");
    Check(detail.offset % 512U == 0 && detail.offset + detail.bytes == peerBytes - flagBytes,
        "detail trace ends before communicator flags, with 512B alignment");
    Check(TileXREpServerDedupSendProfileLayout(flagBytes + detail.bytes - 1U, 96, 6).offset == 0,
        "insufficient payload capacity cannot underflow");
    Check(TileXREpServerDedupSendProfileLayout(peerBytes - 1U, 49, 32).capacity == 64,
        "uneven ownership uses ceiling capacity including K32");
    const auto layout = TileXREpServerDedupCountLayout(96);
    const uint64_t bytes = layout.stateBytes + detail.bytes + kEpServerDedupDetailBytes + flagBytes;
    std::vector<uint8_t> storage(bytes + 63U, 0xa5);
    auto *base = reinterpret_cast<uint8_t *>((reinterpret_cast<uintptr_t>(storage.data()) + 63U) & ~uintptr_t(63U));
    TileXR::CommArgs args {};
    args.rankSize = 16; args.peerMems[0] = base; args.peerMemBytes = bytes;
    for (uint64_t generation = 1; generation <= 3; ++generation) {
        for (uint32_t core = 0; core < 64; ++core) {
            AscendC::testCore = core;
            ServerDedupProfile profile;
            profile.Detail().Init(&args, 96U, 6U);
            for (uint32_t point = 0; point < 12; ++point) profile.Mark(point);
            const uint64_t syncBefore = AscendC::TestSyncOperations();
            profile.Detail().Mark(EpDedupDetailPoint::RecvBegin);
            profile.Detail().Sample(EpDedupDetailMetric::Probe, profile.Detail().Now());
            profile.Detail().Sample(EpDedupDetailMetric::Probe, profile.Detail().Now());
            profile.Detail().Count(EpDedupDetailCounter::ProbeBatches, 2U);
            profile.Flush(&args, generation);
            Check(AscendC::TestSyncOperations() == syncBefore, "fine timestamps and flush add no synchronization");
            const auto *rows = reinterpret_cast<EpServerDedupProfileRecord *>(base) + core * 3U;
            uint64_t previous = 0;
            for (uint32_t segment = 0; segment < 3; ++segment) {
                const auto &row = rows[segment];
                Check(row.marker == kEpServerDedupProfileMarker && row.version == 6 && row.recordBytes == 64 &&
                    row.rank == 0 && row.core == core && row.blockDim == 64 && row.segment == segment && row.generation == generation,
                    "64B profile rows retain complete identity and generation");
                for (auto tick : row.time) { Check(tick > previous, "ordered timestamp segments"); previous = tick; }
            }
            const auto offset = TileXREpServerDedupDetailOffset(TileXREpServerDedupSendProfileLayout(bytes, 96U, 6U).offset);
            const auto *diagnostic = reinterpret_cast<EpServerDedupDetailRecord *>(base + offset) + core;
            const auto &metric = diagnostic->metrics[0];
            Check(diagnostic->version == 6U && diagnostic->recordBytes == 512U && diagnostic->generation == generation &&
                diagnostic->core == core && diagnostic->rank == 0U, "fixed detail records preserve identity across rounds");
            Check(metric.count == 2U && metric.totalTicks >= metric.maxEnd - metric.maxBegin &&
                metric.maxBegin > 0U && diagnostic->counters[1] == 2U, "sum, count and actual maximum interval survive serialization");
            Check(diagnostic->reserved == 0U && diagnostic->points[15] == 0U && diagnostic->counters[15] == 0U,
                "unused detail state is initialized, not stale from prior rounds");
        }
        Check(std::all_of(base + kEpServerDedupProfileBytes, base + layout.stateBytes,
            [](uint8_t v) { return v == 0xa5; }), "profile does not touch reserved tail, cnt/cumsum or relay counter");
    }
    return ServerDedupTest::Finish("profile");
}
