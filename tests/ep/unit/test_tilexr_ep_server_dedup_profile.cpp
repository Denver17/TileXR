#include "tilexr_ep_dispatch_memory_server_dedup_kernel.h"
#include "server_dedup_test_checks.h"

namespace AscendC {
uint32_t testCore = 0;
uint64_t testCopies = 0;
}

int main()
{
    using namespace TileXREp;
    using ServerDedupTest::Check;
    const auto layout = TileXREpServerDedupCountLayout(96);
    std::vector<uint8_t> storage(layout.stateBytes + kEpServerDedupProfileBytes + 63, 0xa5);
    auto *base = reinterpret_cast<uint8_t *>((reinterpret_cast<uintptr_t>(storage.data()) + 63) & ~uintptr_t(63));
    TileXR::CommArgs args {};
    args.rankSize = 16;
    args.peerMems[0] = base;
    args.peerMemBytes = layout.stateBytes + kEpServerDedupProfileBytes;
    for (uint64_t generation = 1; generation <= 3; ++generation) {
        AscendC::TPipe pipe;
        DispatchMemoryServerDedupKernel kernel;
        kernel.ProfileMark(0);
        // At BS96 core 0 has no count work; Process still emits an honest trace.
        kernel.Init(reinterpret_cast<uint8_t *>(&args), nullptr, nullptr, nullptr,
            96, 6, 96, 1, generation, &pipe);
        kernel.ProfileMark(1);
        kernel.Process();
        const auto &record = *reinterpret_cast<EpServerDedupProfileRecord *>(base + layout.stateBytes);
        Check(record.marker == kEpServerDedupProfileMarker && record.version == 2 &&
            record.recordBytes == 128 && record.generation == generation && record.core == 0 &&
            record.rank == 0 && record.blockDim == 64, "trace identity and final generation");
        Check(record.time[0] > 0 && record.time[6] > record.time[0] &&
            std::is_sorted(record.time, record.time + kEpServerDedupProfilePoints), "ordered trace points");
        Check(record.time[2] == record.time[3] && record.time[3] == record.time[4] &&
            record.time[4] == record.time[5], "idle receive phases have zero duration");
        Check(std::all_of(base, base + layout.stateBytes, [](uint8_t v) { return v == 0xa5; }),
            "trace does not overwrite either count bank or relay counter");
        Check(std::all_of(base + layout.stateBytes + sizeof(record), base + args.peerMemBytes,
            [](uint8_t v) { return v == 0xa5; }), "trace only writes its own core slot");
    }
    return ServerDedupTest::Finish("profile");
}
