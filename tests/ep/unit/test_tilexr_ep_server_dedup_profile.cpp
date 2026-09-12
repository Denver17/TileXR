#include "tilexr_ep_server_dedup_profile.h"
#include "server_dedup_test_checks.h"

namespace AscendC { uint32_t testCore = 0; uint64_t testCopies = 0; }
int main()
{
    using namespace TileXREp;
    using ServerDedupTest::Check;
    const auto layout = TileXREpServerDedupCountLayout(96);
    std::vector<uint8_t> storage(layout.stateBytes + 63U, 0xa5);
    auto *base = reinterpret_cast<uint8_t *>((reinterpret_cast<uintptr_t>(storage.data()) + 63U) & ~uintptr_t(63U));
    TileXR::CommArgs args {};
    args.rankSize = 16; args.peerMems[0] = base; args.peerMemBytes = layout.stateBytes;
    for (uint64_t generation = 1; generation <= 3; ++generation) {
        for (uint32_t core = 0; core < 64; ++core) {
            AscendC::testCore = core;
            ServerDedupProfile profile;
            for (uint32_t point = 0; point < 8; ++point) profile.Mark(point);
            profile.Flush(&args, generation);
            const auto *rows = reinterpret_cast<EpServerDedupProfileRecord *>(base) + core * 2U;
            uint64_t previous = 0;
            for (uint32_t segment = 0; segment < 2; ++segment) {
                const auto &row = rows[segment];
                Check(row.marker == kEpServerDedupProfileMarker && row.version == 3 && row.recordBytes == 64 &&
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
