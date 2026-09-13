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
    std::vector<uint8_t> storage(layout.stateBytes + 63U, 0xa5);
    auto *base = reinterpret_cast<uint8_t *>((reinterpret_cast<uintptr_t>(storage.data()) + 63U) & ~uintptr_t(63U));
    TileXR::CommArgs args {};
    args.rankSize = 16; args.peerMems[0] = base; args.peerMemBytes = layout.stateBytes;
    for (uint64_t generation = 1; generation <= 3; ++generation) {
        for (uint32_t core = 0; core < 64; ++core) {
            AscendC::testCore = core;
            ServerDedupProfile profile;
            for (uint32_t point = 0; point < 12; ++point) profile.Mark(point);
            profile.Flush(&args, generation);
            const auto *rows = reinterpret_cast<EpServerDedupProfileRecord *>(base) + core * 3U;
            uint64_t previous = 0;
            for (uint32_t segment = 0; segment < 3; ++segment) {
                const auto &row = rows[segment];
                Check(row.marker == kEpServerDedupProfileMarker && row.version == 5 && row.recordBytes == 64 &&
                    row.rank == 0 && row.core == core && row.blockDim == 64 && row.segment == segment && row.generation == generation,
                    "64B profile rows retain complete identity and generation");
                for (auto tick : row.time) { Check(tick > previous, "ordered timestamp segments"); previous = tick; }
            }
        }
        Check(std::all_of(base + kEpServerDedupProfileBytes, base + layout.stateBytes,
            [](uint8_t v) { return v == 0xa5; }), "profile does not touch reserved tail, cnt/cumsum or relay counter");
    }
    return ServerDedupTest::Finish("profile");
}
