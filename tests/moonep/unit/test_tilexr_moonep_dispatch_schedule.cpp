#include <cstdint>
#include <iostream>
#include <vector>

#include "dispatch_common.h"
#include "dispatch_credit.h"
#include "dispatch_schedule.h"
#include "dispatch_wqe_batch.h"

namespace {

int g_failures = 0;

#define CHECK_TRUE(expr) do { if (!(expr)) { std::cerr << "CHECK_TRUE line " \
    << __LINE__ << ": " #expr << std::endl; ++g_failures; } } while (0)
#define CHECK_EQ(lhs, rhs) do { const auto lhsValue = (lhs); const auto rhsValue = (rhs); \
    if (lhsValue != rhsValue) { std::cerr << "CHECK_EQ line " << __LINE__ \
    << ": " #lhs " != " #rhs << std::endl; ++g_failures; } } while (0)

void CheckCoverage(int64_t rankSize, uint32_t coreCount)
{
    const int64_t phases = TileXRMoonEp::DispatchGroupCount(rankSize);
    const uint32_t workCount = TileXRMoonEp::DispatchPeerWorkCount(coreCount);
    for (int64_t rank = 0; rank < rankSize; ++rank) {
        std::vector<uint32_t> visits(static_cast<size_t>(rankSize), 0U);
        for (int64_t phase = 0; phase < phases; ++phase) {
            for (uint32_t work = 0U; work < workCount; ++work) {
                for (uint32_t core = 0U; core < coreCount; ++core) {
                    const int64_t peer = TileXRMoonEp::DispatchPeerForCore(
                        rank, rankSize, phase, core, coreCount, work);
                    if (peer >= 0) {
                        CHECK_TRUE(peer < rankSize);
                        ++visits[static_cast<size_t>(peer)];
                    }
                }
            }
        }
        for (uint32_t count : visits) {
            CHECK_EQ(count, 1U);
        }
    }
}

void CheckGroupedCoverage(
    int64_t rankSize, uint32_t groupWidth, uint32_t coreCount)
{
    const uint32_t workCount = TileXRMoonEp::DispatchGroupedPeerWorkCount(
        rankSize, groupWidth, coreCount);
    for (int64_t rank = 0; rank < rankSize; ++rank) {
        std::vector<uint32_t> visits(static_cast<size_t>(rankSize), 0U);
        for (uint32_t work = 0U; work < workCount; ++work) {
            for (uint32_t core = 0U; core < coreCount; ++core) {
                uint32_t group = UINT32_MAX;
                uint32_t lane = UINT32_MAX;
                const int64_t peer = TileXRMoonEp::DispatchGroupedPeerForCore(
                    rank, rankSize, groupWidth, core, coreCount, work,
                    group, lane);
                if (peer >= 0) {
                    CHECK_TRUE(peer < rankSize);
                    CHECK_TRUE(peer != rank);
                    CHECK_TRUE(group < TileXRMoonEp::DispatchGroupedGroupCount(
                        rankSize, groupWidth));
                    CHECK_TRUE(lane < groupWidth);
                    ++visits[static_cast<size_t>(peer)];
                }
            }
        }
        for (int64_t peer = 0; peer < rankSize; ++peer) {
            CHECK_EQ(visits[static_cast<size_t>(peer)], peer == rank ? 0U : 1U);
        }
    }
}

void CheckServerLocality()
{
    CHECK_TRUE(TileXRMoonEp::DispatchSameServer(0, 7, 8));
    CHECK_TRUE(TileXRMoonEp::DispatchSameServer(8, 15, 8));
    CHECK_TRUE(!TileXRMoonEp::DispatchSameServer(7, 8, 8));
    CHECK_TRUE(!TileXRMoonEp::DispatchSameServer(0, 1, 0));
    CHECK_EQ(TileXRMoonEp::DispatchFullmeshSlot(15, 8), 7U);
    CHECK_EQ(TileXRMoonEp::DispatchFullmeshSlot(-1, 8), UINT32_MAX);
    CHECK_EQ(TileXRMoonEp::DispatchCompletionLaneCount(8, 15, 8), 1U);
    CHECK_EQ(TileXRMoonEp::DispatchCompletionLaneCount(8, 16, 8), 2U);
}

void TestSchedule()
{
    CHECK_EQ(TileXRMoonEp::DispatchGroupCount(1), 1);
    CHECK_EQ(TileXRMoonEp::DispatchGroupCount(64), 1);
    CHECK_EQ(TileXRMoonEp::DispatchGroupCount(65), 2);
    CHECK_EQ(TileXRMoonEp::DispatchGroupCount(128), 2);
    CHECK_EQ(TileXRMoonEp::DispatchGroupCount(512), 8);
    for (uint32_t coreCount : {1U, 4U, 8U, 16U, 64U}) {
        for (int64_t ranks : {1, 8, 16, 63, 64, 65, 127, 137, 256, 511, 512}) {
            CheckCoverage(ranks, coreCount);
        }
    }

    CHECK_EQ(TileXRMoonEp::DispatchPeerWorkCount(1U), 64U);
    CHECK_EQ(TileXRMoonEp::DispatchPeerWorkCount(8U), 8U);
    CHECK_EQ(TileXRMoonEp::DispatchPeerWorkCount(16U), 4U);
    CHECK_EQ(TileXRMoonEp::DispatchPeerWorkCount(64U), 1U);
    CHECK_EQ(TileXRMoonEp::DispatchPeerWorkCount(0U), 0U);
    CHECK_EQ(TileXRMoonEp::DispatchPeerWorkCount(65U), 0U);

    CHECK_EQ(TileXRMoonEp::DispatchPeerForCore(137, 512, 0, 0U, 8U, 0U), 128);
    CHECK_EQ(TileXRMoonEp::DispatchPeerForCore(137, 512, 0, 7U, 8U, 0U), 135);
    CHECK_EQ(TileXRMoonEp::DispatchPeerForCore(137, 512, 0, 0U, 8U, 1U), 136);
    CHECK_EQ(TileXRMoonEp::DispatchPeerForCore(137, 512, 0, 7U, 8U, 7U), 191);
    CHECK_EQ(TileXRMoonEp::DispatchPeerForCore(137, 512, 0, 0U, 8U, 8U), -1);
    CHECK_EQ(TileXRMoonEp::DispatchPeerForCore(0, 16, 0, 0U, 0U, 0U), -1);
}

void TestGroupedSchedule()
{
    CHECK_TRUE(TileXRMoonEp::DispatchGroupWidthValid(8U));
    CHECK_TRUE(TileXRMoonEp::DispatchGroupWidthValid(16U));
    CHECK_TRUE(!TileXRMoonEp::DispatchGroupWidthValid(0U));
    CHECK_TRUE(!TileXRMoonEp::DispatchGroupWidthValid(4U));

    CHECK_EQ(TileXRMoonEp::DispatchGroupedGroupCount(1, 8U), 0U);
    CHECK_EQ(TileXRMoonEp::DispatchGroupedGroupCount(8, 8U), 1U);
    CHECK_EQ(TileXRMoonEp::DispatchGroupedGroupCount(16, 8U), 2U);
    CHECK_EQ(TileXRMoonEp::DispatchGroupedGroupCount(17, 8U), 2U);
    CHECK_EQ(TileXRMoonEp::DispatchGroupedGroupCount(32, 8U), 4U);
    CHECK_EQ(TileXRMoonEp::DispatchGroupedGroupCount(16, 16U), 1U);
    CHECK_EQ(TileXRMoonEp::DispatchGroupedGroupCount(17, 16U), 1U);
    CHECK_EQ(TileXRMoonEp::DispatchGroupedGroupCount(32, 16U), 2U);

    for (uint32_t groupWidth : {8U, 16U}) {
        for (uint32_t coreCount : {1U, 4U, 8U, 16U, 32U, 64U}) {
            for (int64_t ranks : {1, 2, 7, 8, 16, 17, 31, 32, 63,
                    64, 65, 127, 128, 256, 512}) {
                CheckGroupedCoverage(ranks, groupWidth, coreCount);
            }
        }
    }

    CHECK_EQ(TileXRMoonEp::DispatchGroupedPeer(0, 16, 0U, 0U, 8U), 1);
    CHECK_EQ(TileXRMoonEp::DispatchGroupedPeer(0, 16, 0U, 3U, 8U), 4);
    CHECK_EQ(TileXRMoonEp::DispatchGroupedPeer(0, 16, 0U, 4U, 8U), 15);
    CHECK_EQ(TileXRMoonEp::DispatchGroupedPeer(0, 16, 1U, 3U, 8U), 8);
    CHECK_EQ(TileXRMoonEp::DispatchGroupedPeer(0, 16, 1U, 7U, 8U), -1);
    CHECK_EQ(TileXRMoonEp::DispatchGroupedNextCreditPeer(
        0, 16, 0U, 0U, 8U), 5);
    CHECK_EQ(TileXRMoonEp::DispatchGroupedNextCreditPeer(
        0, 16, 1U, 0U, 8U), -1);

    uint32_t group = 0U;
    uint32_t lane = 0U;
    CHECK_EQ(TileXRMoonEp::DispatchGroupedPeerForCore(
        0, 16, 8U, 8U, 64U, 0U, group, lane), 5);
    CHECK_EQ(group, 1U);
    CHECK_EQ(lane, 0U);
    CHECK_EQ(TileXRMoonEp::DispatchGroupedPeerWorkCount(16, 8U, 64U), 1U);
    CHECK_EQ(TileXRMoonEp::DispatchGroupedPeerWorkCount(128, 8U, 64U), 2U);
    CHECK_EQ(TileXRMoonEp::DispatchGroupedPeerWorkCount(128, 16U, 64U), 2U);
}

void TestSplit128GroupedSchedule()
{
    constexpr int64_t expectedRank0[8][16] = {
        {1, 63, 2, 62, 3, 61, 4, 60,
            64, 96, 65, 127, 66, 126, 67, 125},
        {5, 59, 6, 58, 7, 57, 8, 56,
            68, 124, 69, 123, 70, 122, 71, 121},
        {9, 55, 10, 54, 11, 53, 12, 52,
            72, 120, 73, 119, 74, 118, 75, 117},
        {13, 51, 14, 50, 15, 49, 16, 48,
            76, 116, 77, 115, 78, 114, 79, 113},
        {17, 47, 18, 46, 19, 45, 20, 44,
            80, 112, 81, 111, 82, 110, 83, 109},
        {21, 43, 22, 42, 23, 41, 24, 40,
            84, 108, 85, 107, 86, 106, 87, 105},
        {25, 39, 26, 38, 27, 37, 28, 36,
            88, 104, 89, 103, 90, 102, 91, 101},
        {29, 35, 30, 34, 31, 33, 32, -1,
            92, 100, 93, 99, 94, 98, 95, 97},
    };
    for (uint32_t group = 0U; group < 8U; ++group) {
        for (uint32_t lane = 0U; lane < 16U; ++lane) {
            CHECK_EQ(TileXRMoonEp::DispatchGroupedPeer(
                0, 128, group, lane, 16U), expectedRank0[group][lane]);
        }
    }

    for (int64_t rank = 0; rank < 128; ++rank) {
        const int64_t localHalf = rank / 64;
        std::vector<int32_t> peerGroups(128U, -1);
        for (uint32_t group = 0U; group < 8U; ++group) {
            uint32_t localCount = 0U;
            uint32_t remoteCount = 0U;
            for (uint32_t lane = 0U; lane < 16U; ++lane) {
                const int64_t peer = TileXRMoonEp::DispatchGroupedPeer(
                    rank, 128, group, lane, 16U);
                CHECK_EQ(TileXRMoonEp::DispatchGroupedNextCreditPeer(
                    rank, 128, group, lane, 16U),
                    group < 7U ? TileXRMoonEp::DispatchGroupedPeer(
                        rank, 128, group + 1U, lane, 16U) : -1);
                if (peer < 0) {
                    CHECK_EQ(group, 7U);
                    CHECK_EQ(lane, 7U);
                    continue;
                }
                CHECK_EQ(peerGroups[static_cast<size_t>(peer)], -1);
                peerGroups[static_cast<size_t>(peer)] =
                    static_cast<int32_t>(group);
                if (peer / 64 == localHalf) {
                    ++localCount;
                } else {
                    ++remoteCount;
                }
            }
            CHECK_EQ(localCount, group < 7U ? 8U : 7U);
            CHECK_EQ(remoteCount, 8U);
        }
        for (int64_t peer = 0; peer < 128; ++peer) {
            if (peer == rank) {
                CHECK_EQ(peerGroups[static_cast<size_t>(peer)], -1);
                continue;
            }
            CHECK_TRUE(peerGroups[static_cast<size_t>(peer)] >= 0);
            int32_t reverseGroup = -1;
            for (uint32_t group = 0U; group < 8U; ++group) {
                for (uint32_t lane = 0U; lane < 16U; ++lane) {
                    if (TileXRMoonEp::DispatchGroupedPeer(
                            peer, 128, group, lane, 16U) == rank) {
                        reverseGroup = static_cast<int32_t>(group);
                    }
                }
            }
            CHECK_EQ(reverseGroup, peerGroups[static_cast<size_t>(peer)]);
        }
    }

    for (uint32_t coreCount : {16U, 32U, 64U}) {
        uint32_t sameCorePreviousWaitCount = 0U;
        const uint32_t workCount =
            TileXRMoonEp::DispatchGroupedPeerWorkCount(128, 16U, coreCount);
        for (uint32_t core = 0U; core < coreCount; ++core) {
            int64_t previousPeer = -1;
            uint32_t previousGroup = UINT32_MAX;
            for (uint32_t work = 0U; work < workCount; ++work) {
                uint32_t group = UINT32_MAX;
                uint32_t lane = UINT32_MAX;
                const int64_t peer = TileXRMoonEp::DispatchGroupedPeerForCore(
                    0, 128, 16U, core, coreCount, work, group, lane);
                if (peer < 0) {
                    continue;
                }
                if (group != 0U &&
                    previousPeer == TileXRMoonEp::DispatchGroupedPeer(
                        0, 128, group - 1U, lane, 16U) &&
                    previousGroup == group - 1U) {
                    ++sameCorePreviousWaitCount;
                }
                previousPeer = peer;
                previousGroup = group;
            }
        }
        CHECK_EQ(sameCorePreviousWaitCount, coreCount == 16U ? 111U : 0U);
    }
}

void TestCreditContract()
{
    CHECK_EQ(TileXRMoonEp::kDispatchCreditStrideBytes, UINT64_C(512));
    CHECK_EQ(TileXRMoonEp::kDispatchCreditPlaneBytes,
        static_cast<uint64_t>(TileXR::TILEXR_MAX_RANK_SIZE) * 512U);
    CHECK_EQ(TileXRMoonEp::kDispatchCreditBytes,
        2U * TileXRMoonEp::kDispatchCreditPlaneBytes);

    uint64_t token = UINT64_MAX;
    CHECK_TRUE(TileXRMoonEp::DispatchCreditToken(1, 0U, token));
    CHECK_EQ(token, (UINT64_C(1) << 16U) | UINT64_C(1));
    uint64_t nextToken = 0U;
    CHECK_TRUE(TileXRMoonEp::DispatchCreditToken(1, 1U, nextToken));
    CHECK_TRUE(nextToken > token);
    CHECK_TRUE(TileXRMoonEp::DispatchCreditToken(2, 0U, nextToken));
    CHECK_TRUE(nextToken > token);
    CHECK_TRUE(!TileXRMoonEp::DispatchCreditToken(0, 0U, token));
    CHECK_EQ(token, UINT64_C(0));
    CHECK_TRUE(!TileXRMoonEp::DispatchCreditToken(-1, 0U, token));
    CHECK_TRUE(!TileXRMoonEp::DispatchCreditToken(
        static_cast<int64_t>(TileXRMoonEp::kDispatchCreditMaxMagic + 1U),
        0U, token));
    CHECK_TRUE(!TileXRMoonEp::DispatchCreditToken(
        1, static_cast<uint32_t>(TileXRMoonEp::kDispatchCreditGroupMask),
        token));

    CHECK_EQ(TileXRMoonEp::DispatchCreditPlaneOffset(1),
        TileXRMoonEp::kDispatchCreditPlaneBytes);
    CHECK_EQ(TileXRMoonEp::DispatchCreditPlaneOffset(2), UINT64_C(0));
    CHECK_EQ(TileXRMoonEp::DispatchCreditPlaneOffset(0), UINT64_MAX);
    CHECK_EQ(TileXRMoonEp::DispatchCreditEntryOffset(7U), UINT64_C(3584));
    CHECK_EQ(TileXRMoonEp::DispatchCreditEntryOffset(
        static_cast<uint32_t>(TileXR::TILEXR_MAX_RANK_SIZE)), UINT64_MAX);
    CHECK_EQ(TileXRMoonEp::DispatchCreditSourceOffset(0U, 0U), UINT64_C(0));
    CHECK_EQ(TileXRMoonEp::DispatchCreditSourceOffset(1U, 0U), UINT64_C(1024));
    CHECK_EQ(TileXRMoonEp::DispatchCreditSourceOffset(63U, 15U),
        TileXRMoonEp::kDispatchCreditSourceBytes -
            TileXRMoonEp::kDispatchCreditSourceStrideBytes);
    CHECK_EQ(TileXRMoonEp::DispatchCreditSourceOffset(64U, 0U), UINT64_MAX);
    CHECK_EQ(TileXRMoonEp::DispatchCreditSourceOffset(0U, 16U), UINT64_MAX);
    CHECK_TRUE(!TileXRMoonEp::DispatchCreditRequired(0U));
    CHECK_TRUE(TileXRMoonEp::DispatchCreditRequired(1U));
    CHECK_TRUE(!TileXRMoonEp::DispatchCreditReady(0U, 0U));
    CHECK_TRUE(TileXRMoonEp::DispatchCreditReady(9U, 8U));
}

void TestDestinationDecode()
{
    int64_t rank = -1;
    int64_t slot = -1;
    CHECK_TRUE(TileXRMoonEp::DispatchDecodeDestination(5, 4, 2, &rank, &slot));
    CHECK_EQ(rank, 1);
    CHECK_EQ(slot, 1);
    CHECK_TRUE(!TileXRMoonEp::DispatchDecodeDestination(-6, 4, 2, &rank, &slot));
    CHECK_TRUE(!TileXRMoonEp::DispatchDecodeDestination(8, 4, 2, &rank, &slot));
    CHECK_TRUE(!TileXRMoonEp::DispatchDecodeDestination(INT32_MIN, 4, 2, &rank, &slot));

    CHECK_TRUE(TileXRMoonEp::DispatchDecodeDestination(
        2039, 2040, 2, &rank, &slot));
    CHECK_EQ(rank, 0);
    CHECK_EQ(slot, 2039);
    CHECK_TRUE(TileXRMoonEp::DispatchDecodeDestination(
        2040, 2040, 2, &rank, &slot));
    CHECK_EQ(rank, 1);
    CHECK_EQ(slot, 0);
    CHECK_TRUE(TileXRMoonEp::DispatchDecodeDestination(
        4079, 2040, 2, &rank, &slot));
    CHECK_EQ(rank, 1);
    CHECK_EQ(slot, 2039);
    CHECK_TRUE(!TileXRMoonEp::DispatchDecodeDestination(
        4080, 2040, 2, &rank, &slot));
}

void TestContiguousRanges()
{
    for (uint64_t itemCount : {UINT64_C(0), UINT64_C(1), UINT64_C(7),
            UINT64_C(64), UINT64_C(8192)}) {
        for (uint32_t partCount : {1U, 2U, 8U, 64U}) {
            uint64_t expectedStart = 0U;
            for (uint32_t part = 0U; part < partCount; ++part) {
                uint64_t start = UINT64_MAX;
                uint64_t end = UINT64_MAX;
                TileXRMoonEp::DispatchContiguousRange(
                    itemCount, partCount, part, start, end);
                CHECK_EQ(start, expectedStart);
                CHECK_TRUE(end >= start);
                CHECK_TRUE(end <= itemCount);
                expectedStart = end;
            }
            CHECK_EQ(expectedStart, itemCount);
        }
    }
    uint64_t start = UINT64_MAX;
    uint64_t end = UINT64_MAX;
    TileXRMoonEp::DispatchContiguousRange(8U, 0U, 0U, start, end);
    CHECK_EQ(start, UINT64_C(0));
    CHECK_EQ(end, UINT64_C(0));
    TileXRMoonEp::DispatchContiguousRange(8U, 4U, 4U, start, end);
    CHECK_EQ(start, UINT64_C(0));
    CHECK_EQ(end, UINT64_C(0));
}

void TestWqeBatchBoundaries()
{
    CHECK_EQ(TileXRMoonEp::kDispatchLogicalWqeBatchCapacity, 256U);
    constexpr uint32_t kSqEntries = 4096U;
    CHECK_EQ(TileXRMoonEp::DispatchWqeBatchCount(0U, 0U, kSqEntries), 0U);
    CHECK_EQ(TileXRMoonEp::DispatchWqeBatchCount(1U, 0U, kSqEntries), 1U);
    CHECK_EQ(TileXRMoonEp::DispatchWqeBatchCount(129U, 0U, kSqEntries),
        TileXRMoonEp::kDispatchWqeBatchCapacity);
    CHECK_EQ(TileXRMoonEp::DispatchWqeBatchCount(128U, kSqEntries - 3U,
        kSqEntries), 3U);
    CHECK_EQ(TileXRMoonEp::DispatchWqeBatchCount(128U, kSqEntries,
        kSqEntries), TileXRMoonEp::kDispatchWqeBatchCapacity);
    CHECK_EQ(TileXRMoonEp::DispatchWqeBatchCount(128U, 0U, 0U), 0U);

    uint32_t head = 0U;
    for (uint32_t call = 0U; call < 7U; ++call) {
        head += 1025U;
    }
    CHECK_EQ(head, 7175U);
    CHECK_EQ(TileXRMoonEp::DispatchWqeBatchCount(UINT64_MAX, head, 8192U), 128U);
    head += 7U * 128U;
    CHECK_EQ(TileXRMoonEp::DispatchWqeBatchCount(UINT64_MAX, head, 8192U), 121U);
    CHECK_EQ((1017U * sizeof(int16_t)) % 32U, 18U);
    CHECK_TRUE(TileXRMoonEp::DispatchPeerWqesStreamable(8192U, 16384U));
    CHECK_TRUE(TileXRMoonEp::DispatchPeerWqesStreamable(32768U, 16384U));
    CHECK_TRUE(!TileXRMoonEp::DispatchPeerWqesStreamable(UINT64_MAX, 16384U));
    CHECK_TRUE(!TileXRMoonEp::DispatchPeerWqesStreamable(8192U,
        TileXRMoonEp::kDispatchSqPollReserve +
            TileXRMoonEp::kDispatchWqeBatchCapacity - 1U));
    CHECK_TRUE(!TileXRMoonEp::DispatchGroupedBatchNeedsCompletion(0U));
    CHECK_TRUE(TileXRMoonEp::DispatchGroupedBatchNeedsCompletion(1U));
    CHECK_TRUE(TileXRMoonEp::DispatchGroupedBatchNeedsCompletion(
        TileXRMoonEp::kDispatchWqeBatchCapacity));

    CHECK_EQ(TileXRMoonEp::DispatchRouteTileCount(32768U, 0U, 1024U), 1024U);
    CHECK_EQ(TileXRMoonEp::DispatchRouteTileCount(32768U, 31744U, 1024U),
        1024U);
    CHECK_EQ(TileXRMoonEp::DispatchRouteTileCount(32768U, 32768U, 1024U), 0U);
    CHECK_EQ(TileXRMoonEp::DispatchRouteTileCount(1000U, 0U, 1024U), 1000U);
    CHECK_EQ(TileXRMoonEp::DispatchRouteTileCount(1000U, 0U, 0U), 0U);

    uint64_t dataWqes = UINT64_MAX;
    CHECK_TRUE(TileXRMoonEp::DispatchDataWqeCount(17U, false, dataWqes));
    CHECK_EQ(dataWqes, UINT64_C(17));
    CHECK_TRUE(TileXRMoonEp::DispatchDataWqeCount(17U, true, dataWqes));
    CHECK_EQ(dataWqes, UINT64_C(34));
    CHECK_TRUE(!TileXRMoonEp::DispatchDataWqeCount(
        UINT64_MAX, true, dataWqes));
    CHECK_EQ(dataWqes, UINT64_C(0));

    for (uint32_t route = 0U; route < 129U; ++route) {
        const uint32_t hiddenTask = route * 2U;
        const uint32_t weightTask = hiddenTask + 1U;
        CHECK_EQ(TileXRMoonEp::DispatchDataTaskRouteIndex(hiddenTask, true),
            route);
        CHECK_EQ(TileXRMoonEp::DispatchDataTaskRouteIndex(weightTask, true),
            route);
        CHECK_TRUE(!TileXRMoonEp::DispatchDataTaskIsWeight(hiddenTask, true));
        CHECK_TRUE(TileXRMoonEp::DispatchDataTaskIsWeight(weightTask, true));
    }
    CHECK_TRUE(!TileXRMoonEp::DispatchSignalFitsAfterData(64U, true, 128U));
    CHECK_TRUE(TileXRMoonEp::DispatchSignalFitsAfterData(63U, true, 128U));
    CHECK_TRUE(TileXRMoonEp::DispatchSignalFitsAfterData(127U, false, 128U));
    CHECK_TRUE(!TileXRMoonEp::DispatchSignalFitsAfterData(128U, false, 128U));

}

void TestSparseCqBatchAndOwnerGeneration()
{
    constexpr uint32_t cqEntries = 16384U;
    CHECK_EQ(TileXRMoonEp::DispatchCqePollBatchCount(
        0U, cqEntries, 64U), 64U);
    CHECK_EQ(TileXRMoonEp::DispatchCqePollBatchCount(
        cqEntries - 17U, cqEntries, 64U), 17U);
    CHECK_EQ(TileXRMoonEp::DispatchCqePollBatchCount(
        cqEntries, cqEntries, 64U), 64U);
    CHECK_EQ(TileXRMoonEp::DispatchCqePollBatchCount(
        0U, 0U, 64U), 0U);
    CHECK_EQ(TileXRMoonEp::DispatchCqePollBatchCount(
        0U, cqEntries, 0U), 0U);

    CHECK_TRUE(!TileXRMoonEp::DispatchCqeOwnerReady(
        0U, cqEntries, 0U));
    CHECK_TRUE(TileXRMoonEp::DispatchCqeOwnerReady(
        0U, cqEntries, 1U));
    CHECK_TRUE(!TileXRMoonEp::DispatchCqeOwnerReady(
        cqEntries, cqEntries, 1U));
    CHECK_TRUE(TileXRMoonEp::DispatchCqeOwnerReady(
        cqEntries, cqEntries, 0U));
    CHECK_TRUE(!TileXRMoonEp::DispatchCqeOwnerReady(
        0U, 0U, 1U));
}

void TestSparseCqRestoresAbsoluteSqTail()
{
    constexpr uint32_t sqEntries = 16384U;
    uint32_t completedSqTail = 0U;
    CHECK_TRUE(TileXRMoonEp::DispatchCompletedSqTail(
        0U, 128U, 127U, sqEntries, completedSqTail));
    CHECK_EQ(completedSqTail, 128U);

    CHECK_TRUE(TileXRMoonEp::DispatchCompletedSqTail(
        16300U, 200U, 35U, sqEntries, completedSqTail));
    CHECK_EQ(completedSqTail, 16420U);

    CHECK_TRUE(TileXRMoonEp::DispatchCompletedSqTail(
        14098U, 16277U, 16511U, sqEntries, completedSqTail));
    CHECK_EQ(completedSqTail, 16512U);

    CHECK_TRUE(!TileXRMoonEp::DispatchCompletedSqTail(
        0U, 0U, 0U, sqEntries, completedSqTail));
    CHECK_TRUE(!TileXRMoonEp::DispatchCompletedSqTail(
        0U, 64U, 127U, sqEntries, completedSqTail));
    CHECK_TRUE(!TileXRMoonEp::DispatchCompletedSqTail(
        128U, 128U, 127U, sqEntries, completedSqTail));
    CHECK_TRUE(TileXRMoonEp::DispatchCompletedSqTail(
        0U, 128U, sqEntries, sqEntries, completedSqTail));
    CHECK_EQ(completedSqTail, 1U);

    const uint32_t wrappingSqTail = UINT32_MAX - 63U;
    CHECK_TRUE(TileXRMoonEp::DispatchCompletedSqTail(
        wrappingSqTail, 128U, sqEntries - 1U, sqEntries,
        completedSqTail));
    CHECK_EQ(completedSqTail, 0U);
    const uint32_t firstWrappedCompletion = completedSqTail;
    CHECK_TRUE(TileXRMoonEp::DispatchCompletedSqTail(
        wrappingSqTail, 128U, 63U, sqEntries, completedSqTail));
    CHECK_EQ(completedSqTail, 64U);
    CHECK_TRUE(TileXRMoonEp::DispatchSqTailIsFurther(
        wrappingSqTail, completedSqTail, firstWrappedCompletion));
    CHECK_TRUE(!TileXRMoonEp::DispatchSqTailIsFurther(
        wrappingSqTail, firstWrappedCompletion, completedSqTail));
}

void TestDualQpSplit()
{
    CHECK_TRUE(!TileXRMoonEp::DispatchQpCountSupported(1U));
    CHECK_TRUE(TileXRMoonEp::DispatchQpCountSupported(2U));
    CHECK_TRUE(TileXRMoonEp::DispatchQpCountSupported(32U));
    CHECK_TRUE(!TileXRMoonEp::DispatchQpCountSupported(2U, true));
    CHECK_TRUE(!TileXRMoonEp::DispatchQpCountSupported(32U, true));
    CHECK_TRUE(TileXRMoonEp::DispatchQpCountSupported(48U, true));
    CHECK_TRUE(!TileXRMoonEp::DispatchQpCountSupported(49U, true));
    CHECK_EQ(TileXRMoonEp::DispatchPeerCoreCount(64U, false), 64U);
    CHECK_EQ(TileXRMoonEp::DispatchPeerCoreCount(64U, true), 16U);
    CHECK_EQ(TileXRMoonEp::DispatchPeerCoreCount(8U, true), 8U);
    CHECK_EQ(TileXRMoonEp::DispatchPhysicalQpIndex(0U, 7U, false), 0U);
    CHECK_EQ(TileXRMoonEp::DispatchPhysicalQpIndex(1U, 7U, false), 1U);
    CHECK_EQ(TileXRMoonEp::DispatchPhysicalQpIndex(0U, 7U, true), 7U);
    CHECK_EQ(TileXRMoonEp::DispatchPhysicalQpIndex(1U, 7U, true), 23U);
    CHECK_EQ(TileXRMoonEp::DispatchCreditPhysicalQpIndex(7U, true), 39U);
    CHECK_EQ(TileXRMoonEp::DispatchCreditPhysicalQpIndex(7U, false), 0U);
    CHECK_EQ(TileXRMoonEp::DispatchCreditPhysicalQpIndex(16U, true), UINT32_MAX);
    CHECK_EQ(TileXRMoonEp::DispatchPhysicalQpIndex(0U, 16U, true), UINT32_MAX);
    CHECK_EQ(TileXRMoonEp::DispatchPhysicalQpIndex(2U, 0U, true), UINT32_MAX);
    std::vector<uint32_t> sharedQpVisits(
        TileXRMoonEp::kDispatchSharedQpCount, 0U);
    for (uint32_t core = 0U;
        core < TileXRMoonEp::kDispatchSharedQpCoreCount; ++core) {
        for (uint32_t logicalQp = 0U;
            logicalQp < TileXRMoonEp::kDispatchQpCount; ++logicalQp) {
            ++sharedQpVisits[TileXRMoonEp::DispatchPhysicalQpIndex(
                logicalQp, core, true)];
        }
        ++sharedQpVisits[TileXRMoonEp::DispatchCreditPhysicalQpIndex(
            core, true)];
    }
    for (uint32_t visits : sharedQpVisits) {
        CHECK_EQ(visits, 1U);
    }
    for (uint32_t phase = 0U; phase < 4U; ++phase) {
        for (uint32_t count = 0U; count <= 257U; ++count) {
            const uint32_t qp0 = TileXRMoonEp::DispatchQpRouteCount(count, phase, 0U);
            const uint32_t qp1 = TileXRMoonEp::DispatchQpRouteCount(count, phase, 1U);
            CHECK_EQ(qp0 + qp1, count);
            std::vector<uint32_t> visits(count, 0U);
            for (uint32_t qp = 0U; qp < TileXRMoonEp::kDispatchQpCount; ++qp) {
                const uint32_t qpCount = TileXRMoonEp::DispatchQpRouteCount(
                    count, phase, qp);
                for (uint32_t index = 0U; index < qpCount; ++index) {
                    const uint32_t selected = TileXRMoonEp::DispatchQpSelectedIndex(
                        index, phase, qp);
                    CHECK_TRUE(selected < count);
                    if (selected < count) {
                        ++visits[selected];
                    }
                }
            }
            for (uint32_t visit : visits) {
                CHECK_EQ(visit, 1U);
            }
        }
    }
    CHECK_EQ(TileXRMoonEp::DispatchQpRouteCount(16U, 0U, 0U), 12U);
    CHECK_EQ(TileXRMoonEp::DispatchQpRouteCount(16U, 0U, 1U), 4U);
    CHECK_EQ(TileXRMoonEp::DispatchQpRouteCount(16384U, 0U, 0U), 12288U);
    CHECK_EQ(TileXRMoonEp::DispatchQpRouteCount(16384U, 0U, 1U), 4096U);
    CHECK_EQ(TileXRMoonEp::DispatchQpSelectedIndex(0U, 0U, 1U), 3U);
    CHECK_EQ(TileXRMoonEp::DispatchQpSelectedIndex(3U, 0U, 1U), 15U);
    CHECK_EQ(TileXRMoonEp::DispatchQpSelectedIndex(0U, 0U, 2U), UINT32_MAX);
}

} // namespace

int main()
{
    CheckServerLocality();
    TestSchedule();
    TestGroupedSchedule();
    TestSplit128GroupedSchedule();
    TestCreditContract();
    TestDestinationDecode();
    TestContiguousRanges();
    TestWqeBatchBoundaries();
    TestSparseCqBatchAndOwnerGeneration();
    TestSparseCqRestoresAbsoluteSqTail();
    TestDualQpSplit();
    if (g_failures != 0) {
        return 1;
    }
    std::cout << "MoonEP dispatch schedule checks passed" << std::endl;
    return 0;
}
