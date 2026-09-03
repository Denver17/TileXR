#include <cassert>
#include <cstdint>
#include <set>

#include "alltoall_a5_common.h"

namespace {

void TestCoverage(uint32_t rankSize)
{
    for (uint32_t rank = 0U; rank < rankSize; ++rank) {
        std::set<int32_t> peers;
        for (uint32_t group = 0U; group < TileXRGrouped::GroupCount(rankSize); ++group) {
            for (uint32_t lane = 0U; lane < TileXRGrouped::kGroupWidth; ++lane) {
                const int32_t peer = TileXRGrouped::Peer(rank, rankSize, group, lane);
                if (peer >= 0) {
                    assert(peer != static_cast<int32_t>(rank));
                    assert(peers.insert(peer).second);
                }
            }
        }
        assert(peers.size() == rankSize - 1U);
    }
}

void TestInverse(uint32_t rankSize)
{
    for (uint32_t rank = 0U; rank < rankSize; ++rank) {
        for (uint32_t group = 0U; group < TileXRGrouped::GroupCount(rankSize); ++group) {
            for (uint32_t lane = 0U; lane < TileXRGrouped::kGroupWidth; ++lane) {
                const int32_t peer = TileXRGrouped::Peer(rank, rankSize, group, lane);
                if (peer < 0) {
                    continue;
                }
                uint32_t inverseCount = 0U;
                for (uint32_t inverseLane = 0U;
                     inverseLane < TileXRGrouped::kGroupWidth; ++inverseLane) {
                    if (TileXRGrouped::Peer(static_cast<uint32_t>(peer), rankSize,
                            group, inverseLane) == static_cast<int32_t>(rank)) {
                        ++inverseCount;
                    }
                }
                assert(inverseCount == 1U);
            }
        }
    }
}

void TestCreditRing(uint32_t rankSize)
{
    const uint32_t groupCount = TileXRGrouped::GroupCount(rankSize);
    for (uint32_t rank = 0U; rank < rankSize; ++rank) {
        for (uint32_t lane = 0U; lane < TileXRGrouped::kGroupWidth; ++lane) {
            uint32_t validGroups[TileXRGrouped::kMaxRankSize] = {};
            uint32_t validCount = 0U;
            for (uint32_t group = 0U; group < groupCount; ++group) {
                if (TileXRGrouped::Peer(rank, rankSize, group, lane) >= 0) {
                    validGroups[validCount++] = group;
                }
            }
            for (uint32_t index = 0U; index < validCount; ++index) {
                const uint32_t group = validGroups[index];
                const uint32_t expectedGroup = validGroups[(index + 1U) % validCount];
                assert(TileXRGrouped::NextCreditGroup(
                    rank, rankSize, group, lane) == expectedGroup);
                assert(TileXRGrouped::NextCreditPeer(
                    rank, rankSize, group, lane) == TileXRGrouped::Peer(
                        rank, rankSize, expectedGroup, lane));
                const uint32_t expectedTokenGroup = expectedGroup > group ?
                    expectedGroup : groupCount;
                assert(TileXRGrouped::NextCreditTokenGroup(
                    rank, rankSize, group, lane) == expectedTokenGroup);
            }
        }
    }
}

void Test128PartitionBalancedSchedule()
{
    constexpr uint32_t rankSize = 128U;
    constexpr uint32_t groupCount = 8U;
    const int32_t expectedRank0[groupCount][TileXRGrouped::kGroupWidth] = {
        {1, 2, 3, 4, 63, 62, 61, 60,
            64, 65, 66, 67, 96, 127, 126, 125},
        {5, 6, 7, 8, 59, 58, 57, 56,
            68, 69, 70, 71, 124, 123, 122, 121},
        {9, 10, 11, 12, 55, 54, 53, 52,
            72, 73, 74, 75, 120, 119, 118, 117},
        {13, 14, 15, 16, 51, 50, 49, 48,
            76, 77, 78, 79, 116, 115, 114, 113},
        {17, 18, 19, 20, 47, 46, 45, 44,
            80, 81, 82, 83, 112, 111, 110, 109},
        {21, 22, 23, 24, 43, 42, 41, 40,
            84, 85, 86, 87, 108, 107, 106, 105},
        {25, 26, 27, 28, 39, 38, 37, 36,
            88, 89, 90, 91, 104, 103, 102, 101},
        {29, 30, 31, 32, 35, 34, 33, -1,
            92, 93, 94, 95, 100, 99, 98, 97},
    };

    assert(TileXRGrouped::GroupCount(rankSize) == groupCount);
    for (uint32_t group = 0U; group < groupCount; ++group) {
        for (uint32_t lane = 0U; lane < TileXRGrouped::kGroupWidth; ++lane) {
            assert(TileXRGrouped::Peer(0U, rankSize, group, lane) ==
                expectedRank0[group][lane]);
        }
    }

    for (uint32_t rank = 0U; rank < rankSize; ++rank) {
        const uint32_t partition = rank / 64U;
        for (uint32_t group = 0U; group < groupCount; ++group) {
            uint32_t localPeers = 0U;
            uint32_t remotePeers = 0U;
            for (uint32_t lane = 0U; lane < TileXRGrouped::kGroupWidth; ++lane) {
                const int32_t peer = TileXRGrouped::Peer(rank, rankSize, group, lane);
                if (peer < 0) {
                    assert(group == groupCount - 1U);
                    assert(lane == 7U);
                } else if (static_cast<uint32_t>(peer) / 64U == partition) {
                    assert(lane < TileXRGrouped::kHalfGroupWidth);
                    ++localPeers;
                } else {
                    assert(lane >= TileXRGrouped::kHalfGroupWidth);
                    ++remotePeers;
                }
            }
            assert(localPeers == (group == groupCount - 1U ? 7U : 8U));
            assert(remotePeers == 8U);
        }
    }
}

void TestLayout()
{
    const auto layout = TileXRGrouped::BuildLayout(64U, 1024U);
    assert(layout.valid);
    assert(layout.bytesPerPeer == 4096U);
    assert(layout.bytesPerRank == 256U * 1024U);
    assert(layout.groupCount == 4U);
    assert(layout.payloadOffset[0] == 0U);
    assert(layout.payloadOffset[1] >= layout.payloadOffset[0] + layout.bytesPerRank);
    assert(layout.signalPlaneBytes ==
        64U * TileXRGrouped::kSignalPeerStrideBytes);
    assert(layout.signalOffset[0] >= layout.payloadOffset[1] + layout.bytesPerRank);
    assert(layout.signalOffset[1] >=
        layout.signalOffset[0] + layout.signalPlaneBytes);
    assert(layout.signalSourceOffset >=
        layout.signalOffset[1] + layout.signalPlaneBytes);
    assert(layout.statusOffset >=
        layout.signalSourceOffset + TileXRGrouped::kSignalSourceBytes);
    assert(layout.workspaceBytes >= layout.statusOffset + TileXRGrouped::kStatusBytes);
    assert(layout.workspaceBytes % TileXRGrouped::kWorkspaceAlignment == 0U);
    assert(TileXRGrouped::SignalByteOffset(3U, 1U) ==
        3U * TileXRGrouped::kSignalPeerStrideBytes +
            TileXRGrouped::kSignalRouteStrideBytes);
    assert(TileXRGrouped::SignalSourceByteOffset(31U) ==
        31U * TileXRGrouped::kSignalSourceStrideBytes);

    const auto layout128 = TileXRGrouped::BuildLayout(128U, 1024U);
    assert(layout128.valid);
    assert(layout128.bytesPerRank == 512U * 1024U);
    assert(layout128.groupCount == 8U);
    assert(layout128.signalPlaneBytes ==
        128U * TileXRGrouped::kSignalPeerStrideBytes);

    assert(TileXRGrouped::BuildLayout(8U, 1U).valid);
    assert(!TileXRGrouped::BuildLayout(136U, 1U).valid);
    assert(!TileXRGrouped::BuildLayout(16U, 0U).valid);
    assert(!TileXRGrouped::BuildLayout(64U, UINT64_MAX).valid);
}

void TestSupportedRanksAndKernelDispatch()
{
    constexpr uint32_t supported[] = {8U, 16U, 32U, 64U, 128U};
    for (uint32_t rankSize : supported) {
        assert(TileXRGrouped::ValidRankSize(rankSize));
        assert(TileXRGrouped::BuildLayout(rankSize, 1U).valid);
        const auto expected = rankSize == 128U ?
            TileXRGrouped::KernelVariant::Rank128 :
            TileXRGrouped::KernelVariant::Rank64;
        assert(TileXRGrouped::SelectKernelVariant(rankSize) == expected);
    }
    constexpr uint32_t unsupported[] = {
        0U, 1U, 2U, 4U, 7U, 9U, 15U, 24U, 48U,
        72U, 80U, 96U, 120U, 127U, 129U, 136U,
    };
    for (uint32_t rankSize : unsupported) {
        assert(!TileXRGrouped::ValidRankSize(rankSize));
        assert(!TileXRGrouped::BuildLayout(rankSize, 1U).valid);
        assert(TileXRGrouped::SelectKernelVariant(rankSize) ==
            TileXRGrouped::KernelVariant::Invalid);
    }
}

void TestIpcOwnership()
{
    for (uint32_t rank = 0U; rank < 128U; ++rank) {
        const uint64_t credit = TileXRGrouped::IpcEntryOffset(
            rank, TileXRGrouped::kCreditOffsetBytes);
        assert(credit / TileXRGrouped::kCreditStrideBytes == rank);
    }
}

void TestSupportedQpCount()
{
    static_assert(TileXRGrouped::kRequiredQpCount == 32U,
        "grouped data plane requires two 16-lane QP routes");
    assert(!TileXRGrouped::SupportedQpCount(32U));
    assert(TileXRGrouped::SupportedQpCount(39U));
    assert(!TileXRGrouped::SupportedQpCount(48U));
}

void TestProfilingLaunchCapacity()
{
    static_assert(TileXRGrouped::kProfilingRecordsPerLaunch == 144U,
        "grouped profiling launch stride changed");
    assert(TileXRGrouped::ProfilingLaunchSlotValid(0U, 144U));
    assert(!TileXRGrouped::ProfilingLaunchSlotValid(1U, 144U));
    assert(TileXRGrouped::ProfilingLaunchSlotValid(55U, 8192U));
    assert(!TileXRGrouped::ProfilingLaunchSlotValid(56U, 8192U));
    assert(!TileXRGrouped::ProfilingLaunchSlotValid(0U, 143U));
}

} // namespace

int main()
{
    TestCoverage(8U);
    TestCoverage(16U);
    TestCoverage(32U);
    TestCoverage(64U);
    TestCoverage(128U);
    TestInverse(8U);
    TestInverse(16U);
    TestInverse(32U);
    TestInverse(64U);
    TestInverse(128U);
    TestCreditRing(8U);
    TestCreditRing(16U);
    TestCreditRing(32U);
    TestCreditRing(64U);
    TestCreditRing(128U);
    Test128PartitionBalancedSchedule();
    TestLayout();
    TestSupportedRanksAndKernelDispatch();
    TestIpcOwnership();
    TestSupportedQpCount();
    TestProfilingLaunchCapacity();
    return 0;
}
