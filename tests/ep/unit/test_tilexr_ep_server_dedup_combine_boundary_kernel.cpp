// Sparse CPU GM executes the real probe kernel without allocating 8GiB.
// It checks address/range/UB ordering only, not device cache or timing behavior.
#include <array>
#include <cstring>
#include <iostream>
#include <map>
#include <stdexcept>
#define __CCE__ 1
#define __CCE_IS_AICORE__ 1
#include "kernel_operator.h"
#include "server_dedup_test_checks.h"
#include "../integration/combine_memory_boundary_kernel.cpp"

namespace {
using namespace TileXRCombineMemoryBoundaryTest;
using ServerDedupTest::Check;
constexpr uintptr_t peerBase = UINT64_C(0x10000000000);
std::map<uintptr_t, std::array<uint8_t, 96>> memory;
alignas(32) std::array<uint8_t, 128> output;
uint32_t reads = 0, writes = 0;
bool skipSecondRead = false;
uint8_t *Map(uintptr_t address, uint32_t bytes)
{
    if (address < peerBase || !LegalRange(address - peerBase, bytes))
        throw std::runtime_error("probe touched runtime or an out-of-range address");
    auto found = memory.upper_bound(address);
    if (found != memory.begin()) --found;
    if (found == memory.end() || address < found->first || address - found->first > found->second.size() ||
        bytes > found->second.size() - (address - found->first))
        throw std::runtime_error("probe touched an unplanned sparse GM address");
    return found->second.data() + address - found->first;
}
void Invoke(TileXR::CommArgs &args, uint32_t window, uint32_t index, uint64_t expected = kPeerBytes)
{
    tilexr_ep_combine_memory_boundary_test_kernel(reinterpret_cast<GM_ADDR>(&args), output.data() + 32,
        expected, window, index);
}
bool Words(const uint8_t *bytes, uint32_t expected)
{
    for (unsigned i = 0; i < 8; ++i) {
        uint32_t word = 0;
        std::memcpy(&word, bytes + i * 4, 4);
        if (word != expected) return false;
    }
    return true;
}
}
namespace AscendC {
uint32_t testCore = 0;
void ReadGm(uintptr_t address, void *data, uint32_t bytes)
{
    ++reads;
    if (skipSecondRead && reads % 2 == 0) return;
    if (bytes != 32) throw std::runtime_error("probe read width must be 32 bytes");
    std::memcpy(data, Map(address, bytes), bytes);
}
void WriteGm(uintptr_t address, const void *data, uint32_t bytes)
{
    ++writes;
    const uintptr_t result = reinterpret_cast<uintptr_t>(output.data() + 32);
    if (bytes != 32) throw std::runtime_error("probe write width must be 32 bytes");
    if (address == result || address == result + 32) {
        std::memcpy(reinterpret_cast<void *>(address), data, bytes);
        return;
    }
    std::memcpy(Map(address, bytes), data, bytes);
}
}
int main()
{
    try {
        TileXR::CommArgs args {};
        args.rank = 3; args.rankSize = 16; args.peerMemBytes = kPeerBytes;
        args.peerMems[3] = reinterpret_cast<GM_ADDR>(peerBase);
        // Neighboring windows have distinct sparse backing, so truncating offset
        // or dropping the bank index cannot accidentally land on the right data.
        for (uint32_t window = 0; window < 2; ++window) for (uint32_t index = 0; index < 4; ++index) {
            auto &bytes = memory[peerBase + ProbeOffset(window, index) - 32];
            bytes.fill(0xa5);
            std::fill(bytes.begin() + 32, bytes.begin() + 64, 0);
        }
        for (uint32_t window = 0; window < 2; ++window) for (uint32_t index = 0; index < 4; ++index) {
            output.fill(0x55);
            const uint32_t beforeReads = reads, beforeWrites = writes;
            Invoke(args, window, index);
            Check(reads == beforeReads + 2 && writes == beforeWrites + 3,
                "probe performs two GM reads, target write and two result writes");
            Check(Words(output.data() + 32, 0) && Words(output.data() + 64, ProbePattern(window, index)),
                "initial and fresh readback use separate result halves");
            Check(Words(output.data(), 0x55555555) && Words(output.data() + 96, 0x55555555),
                "result output writes preserve allocation guards");
            Check(AscendC::PendingWrites().empty(), "kernel completes all writes before returning");
        }
        for (uint32_t window = 0; window < 2; ++window) for (uint32_t index = 0; index < 4; ++index) {
            const auto &bytes = memory.at(peerBase + ProbeOffset(window, index) - 32);
            Check(Words(bytes.data(), 0xa5a5a5a5) && Words(bytes.data() + 64, 0xa5a5a5a5),
                "each sparse target preserves both neighboring guards");
            Check(Words(bytes.data() + 32, ProbePattern(window, index)),
                "both windows retain all independent patterns after every probe");
        }
        // A suppressed second read cannot pass by reusing the write pattern in UB.
        output.fill(0x55);
        skipSecondRead = true;
        Invoke(args, 1, 3);
        skipSecondRead = false;
        Check(Words(output.data() + 64, 0), "UB is cleared before second GM read");
        const uint32_t transfers = reads + writes;
        const auto baseline = args;
        for (unsigned fault = 0; fault < 10; ++fault) {
            args = baseline;
            switch (fault) {
                case 0: args.rank = -1; break;
                case 1: args.rank = 16; break;
                case 2: args.rankSize = 0; break;
                case 3: args.rankSize = TileXR::TILEXR_MAX_RANK_SIZE + 1; break;
                case 4: args.peerMemBytes -= 32; break;
                case 5: args.peerMems[3] = nullptr; break;
                case 6: args.peerMems[3] = reinterpret_cast<GM_ADDR>(peerBase + 1); break;
                case 7: args.peerMems[3] = reinterpret_cast<GM_ADDR>(UINT64_MAX - 511); break;
                case 8: AscendC::testCore = 1; break;
                case 9: args.peerMemBytes += 32; break;
            }
            Invoke(args, 0, 0);
            AscendC::testCore = 0;
            Check(reads + writes == transfers, "invalid device context cannot issue a GM transfer");
        }
        args = baseline;
        Invoke(args, 2, 0);
        Invoke(args, 0, 4);
        Invoke(args, 1, 3, kPeerBytes - 32);
        tilexr_ep_combine_memory_boundary_test_kernel(nullptr, output.data() + 32, kPeerBytes, 0, 0);
        tilexr_ep_combine_memory_boundary_test_kernel(reinterpret_cast<GM_ADDR>(&args), nullptr, kPeerBytes, 0, 0);
        tilexr_ep_combine_memory_boundary_test_kernel(reinterpret_cast<GM_ADDR>(&args), output.data() + 33, kPeerBytes, 0, 0);
        tilexr_ep_combine_memory_boundary_test_kernel(reinterpret_cast<GM_ADDR>(uintptr_t(1)), output.data() + 32, kPeerBytes, 0, 0);
        tilexr_ep_combine_memory_boundary_test_kernel(reinterpret_cast<GM_ADDR>(&args),
            reinterpret_cast<GM_ADDR>(UINTPTR_MAX - 31U), kPeerBytes, 0, 0);
        tilexr_ep_combine_memory_boundary_test_kernel(reinterpret_cast<GM_ADDR>(&args),
            reinterpret_cast<GM_ADDR>(peerBase + kPeerBytes - 64U), kPeerBytes, 0, 0);
        Check(reads + writes == transfers, "invalid indices/capacity/pointers are guarded before transfer");
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    if (ServerDedupTest::Failures()) return 1;
    std::cout << "EP_SERVER_DEDUP_COMBINE_BOUNDARY_KERNEL_PASS\n";
    return 0;
}
