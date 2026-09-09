#include <algorithm>
#include <cassert>
#include <cstring>
#include <iostream>
#include "comm_args.h"
#include "ep_memory_server_dedup_profile.h"
// Exercise the real entry body. These fakes do not validate CANN ABI or device cache visibility.
#include "tilexr_ep_dispatch_memory_server_dedup_kernel.cpp"
namespace AscendC {
uint32_t testCore = 0;
uint32_t testCores = 200;
uint64_t testTicks = 100;
uint32_t testCleans = 0;
}
alignas(64) static uint8_t data[200 * 64 + 128];
static TileXR::CommArgs args;
static uint8_t outputs[8];
void Invoke(GM_ADDR comm, int64_t generation, int64_t bs = 1)
{
    tilexr_ep_dispatch_memory_server_dedup_kernel(comm, outputs, outputs, nullptr,
        outputs, outputs, outputs, nullptr, outputs, outputs, outputs,
        bs, 1, 1, 32, 16, 0, TileXR::TILEXR_DATA_TYPE_FP16,
        TileXR::TILEXR_DATA_TYPE_FP16, generation);
}
int main()
{
    using namespace TileXREp;
    std::memset(data, 0xA5, sizeof(data));
    std::memset(outputs, 0x5A, sizeof(outputs));
    args.rankSize = 128;
    args.rank = 127;
    args.extraFlag = TileXR::ExtraFlag::MEMORY_ONLY;
    args.peerMems[127] = data + 64;
    args.peerMemBytes = 200 * 64;
#if defined(TILEXR_EP_SERVER_DEDUP_ENABLE_PROFILING) && TILEXR_EP_SERVER_DEDUP_ENABLE_PROFILING
    for (int64_t generation = 1; generation <= 7; ++generation) {
        for (uint32_t core = 0; core < 200; ++core) {
            AscendC::testCore = core;
            Invoke(reinterpret_cast<GM_ADDR>(&args), generation);
        }
    }
    for (uint32_t core = 0; core < 200; ++core) {
        EpServerDedupProfileRecord record {};
        std::memcpy(&record, data + 64 + core * 64, sizeof(record));
        assert(record.marker == kEpServerDedupProfileMarker && record.version == 1 && record.recordBytes == 64);
        assert(record.rank == 127 && record.core == core && record.blockDim == 200 && record.generation == 7);
        assert(record.start > 0 && record.start < record.checked && record.checked < record.end);
        assert(record.reserved == 0 && record.reserved2 == 0);
    }
    assert(AscendC::testCleans == 1400);
    assert(std::all_of(data, data + 64, [](uint8_t b) { return b == 0xA5; }));
    assert(std::all_of(data + 64 + 200 * 64, data + sizeof(data), [](uint8_t b) { return b == 0xA5; }));
    uint8_t snapshot[sizeof(data)];
    std::memcpy(snapshot, data, sizeof(data));
    const auto validArgs = args;
    for (int test = 0; test < 9; ++test) {
        args = validArgs;
        AscendC::testCore = 0;
        AscendC::testCores = 200;
        switch(test) {
            case 0: args.rank = -1; break;
            case 1: args.rank = 128; break;
            case 2: args.rankSize = 129; break;
            case 3: args.extraFlag = 0; break;
            case 4: args.peerMemBytes = 12799; break;
            case 5: args.peerMems[127] += 1; break;
            case 6: AscendC::testCores = 201; break;
            case 7: AscendC::testCore = 200; break;
            case 8: args.peerMems[127] = nullptr; break;
        }
        Invoke(reinterpret_cast<GM_ADDR>(&args), 8);
        assert(std::memcmp(snapshot, data, sizeof(data)) == 0);
        assert(AscendC::testCleans == 1400);
    }
    Invoke(reinterpret_cast<GM_ADDR>(1), 8, 0);
    assert(std::memcmp(snapshot, data, sizeof(data)) == 0);
#else
    Invoke(reinterpret_cast<GM_ADDR>(1), 1);
    assert(AscendC::testTicks == 100 && AscendC::testCleans == 0);
    assert(std::all_of(data, data + sizeof(data), [](uint8_t b) { return b == 0xA5; }));
#endif
    assert(std::all_of(outputs, outputs + sizeof(outputs), [](uint8_t b) { return b == 0x5A; }));
    std::cout << "PROFILE_BEHAVIOR_PASS\n";
}
