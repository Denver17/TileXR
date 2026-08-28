#include <array>
#include <cstdint>
#include <iostream>
#include <vector>

#include "combine_v2_schedule.h"
#include "combine_v2_weight.h"
#include "combine_v2_wqe_batch.h"
#include "tilexr_udma_fullmesh.h"

namespace {

int failures = 0;

void Check(bool condition, const char *message)
{
    if (!condition) {
        std::cerr << message << '\n';
        ++failures;
    }
}

std::vector<uint32_t> SelectPeerIndicesReference(
    const std::vector<int32_t> &dst, uint32_t peer, uint32_t slots)
{
    std::vector<uint32_t> selected;
    const uint64_t peerBase = static_cast<uint64_t>(peer) * slots;
    const uint64_t peerEnd = peerBase + slots;
    for (uint32_t index = 0U; index < dst.size(); ++index) {
        const int32_t encoded = dst[index];
        if (encoded >= 0 &&
            static_cast<uint64_t>(encoded) >= peerBase &&
            static_cast<uint64_t>(encoded) < peerEnd) {
            selected.push_back(index);
        }
    }
    return selected;
}

void TestSchedule()
{
    using namespace TileXRMoonEp;
    const uint32_t supported[] = {2U, 3U, 4U, 5U, 6U, 7U, 8U,
        16U, 32U, 64U, 128U};
    for (uint32_t rankSize = 1U;
        rankSize <= kMoonEpCombineV2RankCount; ++rankSize) {
        bool expected = false;
        for (uint32_t value : supported) {
            expected = expected || rankSize == value;
        }
        Check(MoonEpCombineV2RankSizeSupported(rankSize) == expected,
            "supported-rank predicate mismatch");
    }
    Check(!MoonEpCombineV2RankSizeSupported(0U),
        "zero ranks accepted");
    Check(!MoonEpCombineV2RankSizeSupported(1U),
        "one rank accepted");

    for (uint32_t rankSize : supported) {
        const uint32_t activeCores =
            MoonEpCombineV2ActiveCoreCount(rankSize);
        const uint32_t stepCount = MoonEpCombineV2StepCount(rankSize);
        Check(activeCores == (rankSize <= 8U ? rankSize : 16U),
            "active-core contract mismatch");
        Check(stepCount == (rankSize <= 8U ? 1U : rankSize / 16U),
            "step-count contract mismatch");
        Check(MoonEpCombineV2LocalRankSize(rankSize) ==
                (rankSize <= 8U ? rankSize : 8U),
            "local-rank contract mismatch");
        for (uint32_t source = 0U; source < rankSize; ++source) {
            std::array<bool, kMoonEpCombineV2RankCount> seen {};
            uint32_t selfCount = 0U;
            for (uint32_t step = 0U; step < stepCount; ++step) {
                for (uint32_t core = 0U; core < activeCores; ++core) {
                    const uint32_t peer = MoonEpCombineV2Peer(
                        source, step, core, rankSize);
                    Check(peer < rankSize, "peer out of range");
                    Check(!seen[peer], "peer repeated");
                    seen[peer] = true;
                    if (rankSize > 8U) {
                        Check((core < 8U && peer < rankSize / 2U) ||
                                (core >= 8U && peer >= rankSize / 2U),
                            "core crossed target half");
                    }
                    Check(MoonEpCombineV2ReceiveStep(
                            peer, source, rankSize) == step,
                        "receive step does not invert peer");
                    if (peer == source) {
                        ++selfCount;
                        Check(step + 1U == stepCount,
                            "self peer must be in the final step");
                    }
                    if (step + 1U < stepCount) {
                        const uint32_t successor = MoonEpCombineV2Successor(
                            source, core, rankSize);
                        Check(successor / (rankSize / 2U) ==
                                source / (rankSize / 2U),
                            "successor crossed source half");
                        Check(peer == MoonEpCombineV2Peer(successor,
                                step + 1U, core, rankSize),
                            "successor schedule mismatch");
                    }
                }
            }
            for (uint32_t peer = 0U; peer < rankSize; ++peer) {
                Check(seen[peer], "schedule does not cover every peer");
            }
            Check(selfCount == 1U, "schedule must visit self once");
        }
    }
}

void TestQpAndBatchContract()
{
    using namespace TileXRMoonEp;
    std::array<bool, kMoonEpCombineV2QpCount> seen {};
    for (uint32_t core = 0; core < kMoonEpCombineV2CoreCount; ++core) {
        for (uint32_t lane = 0; lane < kMoonEpCombineV2LaneCount; ++lane) {
            const uint32_t qp = MoonEpCombineV2Qp(core, lane);
            Check(qp < kMoonEpCombineV2QpCount, "QP out of range");
            Check(!seen[qp], "QP ownership is not exclusive");
            seen[qp] = true;
        }
    }

    uint32_t sixPortRows = 0;
    uint32_t twoPortRows = 0;
    for (uint32_t row = 0; row < kMoonEpCombineV2LogicalBatchRows; ++row) {
        if (MoonEpCombineV2LaneForPosition(row) ==
            MOONEP_COMBINE_V2_SIX_PORT) {
            ++sixPortRows;
        } else {
            ++twoPortRows;
        }
    }
    Check(sixPortRows == kMoonEpCombineV2SixPortRows,
        "six-port row count mismatch");
    Check(twoPortRows == kMoonEpCombineV2TwoPortRows,
        "two-port row count mismatch");

    const MoonEpCombineV2LaneCounts payload =
        MoonEpCombineV2BatchLaneCounts(
            128U, 0U, 0U, kMoonEpCombineV2StepCount, false);
    Check(payload.sixPort == 96U && payload.twoPort == 32U,
        "payload split mismatch");
    const MoonEpCombineV2LaneCounts finalBatch =
        MoonEpCombineV2BatchLaneCounts(
            128U, 0U, 0U, kMoonEpCombineV2StepCount, true);
    Check(finalBatch.sixPort == 98U && finalBatch.twoPort == 34U,
        "control WQE split mismatch");

    for (uint32_t phase = 0; phase < 4U; ++phase) {
        for (uint32_t rows = 0; rows <= 128U; ++rows) {
            const MoonEpCombineV2LaneCounts counts =
                MoonEpCombineV2BatchLaneCounts(
                    rows, phase, 0U, kMoonEpCombineV2StepCount, false);
            Check(counts.sixPort + counts.twoPort == rows,
                "batch split lost rows");
        }
    }

    const MoonEpCombineV2RingSegments wrapped =
        MoonEpCombineV2SplitRingCopy(16380U, 34U, 16384U);
    Check(wrapped.first == 4U && wrapped.second == 30U,
        "SQ wrap split mismatch");
    Check(MoonEpCombineV2NextCqTarget(UINT32_MAX, true) == 0U,
        "CQ target must wrap as uint32");
    Check(MoonEpCombineV2CompletionCount(false) == 0U,
        "payload-only batch must not publish a CQ completion");
    Check(MoonEpCombineV2CompletionCount(true) == 1U,
        "final batch must publish exactly one CQ completion per lane");
    Check(MoonEpCombineV2CqTargetReached(0U, 0U),
        "wrapped CQ target must be reachable");
}

void TestGroupAllToAllSchedule()
{
    using namespace TileXRMoonEp;
    const MoonEpCombineV2ScheduleMode mode = MOONEP_COMBINE_V2_GROUP;
    Check(sizeof(MoonEpCombineV2CreditSignal) == 64U,
        "Credit signal is not one cache line");
    Check(MoonEpCombineV2ScheduleModeValid(mode) &&
            MoonEpCombineV2ScheduleRankSizeSupported(
                mode, kMoonEpCombineV2RankCount),
        "128P Group schedule mode is not supported");
    Check(!MoonEpCombineV2ScheduleRankSizeSupported(mode, 64U) &&
            !MoonEpCombineV2BuildStepRoute(
                mode, 0U, 0U, 0U, 64U).valid,
        "Group schedule accepted a non-128P rank size");
    const uint32_t nonGroupRankSizes[] = {
        2U, 3U, 4U, 5U, 6U, 7U, 8U, 16U, 32U, 64U};
    for (uint32_t rankSize : nonGroupRankSizes) {
        Check(MoonEpCombineV2ScheduleModeForRankSize(rankSize) ==
                MOONEP_COMBINE_V2_SINGLE_RING,
            "non-128P rank size did not select Ring mapping");
    }
    Check(MoonEpCombineV2ScheduleModeForRankSize(
            kMoonEpCombineV2RankCount) == MOONEP_COMBINE_V2_GROUP,
        "128P rank size did not select Group mapping");

    std::array<uint32_t,
        kMoonEpCombineV2RankCount *
        kMoonEpCombineV2CreditTransitionCount *
        kMoonEpCombineV2CoreCount> creditWriters {};
    for (uint32_t rank = 0U; rank < kMoonEpCombineV2RankCount; ++rank) {
        std::array<bool, kMoonEpCombineV2RankCount> sendSeen {};
        std::array<bool, kMoonEpCombineV2RankCount> receiveSeen {};
        uint32_t selfStep = kMoonEpCombineV2InvalidPeer;
        const uint32_t rankInner = MoonEpCombineV2GroupInnerIndex(rank);
        Check(rankInner < kMoonEpCombineV2CoreCount,
            "Group inner index out of range");
        Check(MoonEpCombineV2GroupId(rank) < kMoonEpCombineV2StepCount,
            "Group id out of range");
        for (uint32_t step = 0U;
            step < kMoonEpCombineV2StepCount; ++step) {
            for (uint32_t core = 0U;
                core < kMoonEpCombineV2CoreCount; ++core) {
                const uint32_t destination =
                    MoonEpCombineV2GroupSendDstRank(rank, step, core);
                const uint32_t source =
                    MoonEpCombineV2GroupRecvSrcRank(rank, step, core);
                const MoonEpCombineV2StepRoute route =
                    MoonEpCombineV2BuildStepRoute(mode, rank, core, step,
                        kMoonEpCombineV2RankCount);
                Check(destination < kMoonEpCombineV2RankCount,
                    "Group send destination out of range");
                Check(source < kMoonEpCombineV2RankCount,
                    "Group receive source out of range");
                Check(route.valid && route.sendDstRank == destination &&
                        route.recvSrcRank == source,
                    "Group StepRoute endpoints changed");
                Check(MoonEpCombineV2MapSendDst(mode, rank, core, step,
                            kMoonEpCombineV2RankCount) == destination &&
                        MoonEpCombineV2MapRecvSrc(mode, rank, core, step,
                            kMoonEpCombineV2RankCount) == source,
                    "Group mapping is not routed through the common framework");
                Check(MoonEpCombineV2GroupInnerIndex(destination) == core,
                    "Group send did not preserve destination inner index");
                Check(MoonEpCombineV2GroupInnerIndex(source) == core,
                    "Group receive did not preserve source inner index");
                Check(MoonEpCombineV2GroupRecvSrcRank(
                        destination, step, rankInner) == rank,
                    "Group receive mapping did not invert send mapping");
                Check(MoonEpCombineV2GroupSendDstRank(
                        source, step, rankInner) == rank,
                    "Group send mapping did not invert receive mapping");
                Check(MoonEpCombineV2FindSendCore(mode, rank, destination,
                            step, kMoonEpCombineV2RankCount) == core &&
                        MoonEpCombineV2FindRecvCore(mode, destination, rank,
                            step, kMoonEpCombineV2RankCount) == rankInner,
                    "Group route core inverse changed");
                const uint32_t nextStep = MoonEpCombineV2NextStep(
                    step, kMoonEpCombineV2StepCount);
                const uint32_t expectedSuccessor =
                    MoonEpCombineV2GroupRecvSrcRank(destination, nextStep,
                        rankInner);
                Check(route.sendSuccessorRank == expectedSuccessor &&
                        MoonEpCombineV2GroupSendDstRank(
                            expectedSuccessor, nextStep, core) == destination,
                    "Group route successor changed");
                if (route.waitCredit) {
                    Check(route.creditWaitSrcRank == destination &&
                            route.creditWaitSrcCore == rankInner,
                        "Group Credit wait route changed");
                }
                if (route.publishCredit) {
                    Check(route.creditPublishDstRank ==
                                MoonEpCombineV2GroupRecvSrcRank(
                                    rank, nextStep, core) &&
                            route.creditPublishDstCore == rankInner,
                        "Group Credit publish route changed");
                }
                Check(!sendSeen[destination],
                    "Group send mapping repeated a destination");
                Check(!receiveSeen[source],
                    "Group receive mapping repeated a source");
                sendSeen[destination] = true;
                receiveSeen[source] = true;
                if (destination == rank) {
                    Check(core == rankInner,
                        "Group Self route used the wrong core");
                    selfStep = step;
                }

                if (step + 1U < kMoonEpCombineV2StepCount) {
                    const uint32_t transitionStep = step + 1U;
                    const uint32_t nextSender =
                        MoonEpCombineV2GroupRecvSrcRank(
                            rank, transitionStep, core);
                    const uint32_t targetCore = rankInner;
                    Check(MoonEpCombineV2GroupInnerIndex(nextSender) == core,
                        "Credit writer does not select the sender inner index");
                    Check(MoonEpCombineV2GroupSendDstRank(
                            nextSender, transitionStep, targetCore) == rank,
                        "Credit target does not send back to the writer rank");
                    const uint64_t flat =
                        (static_cast<uint64_t>(nextSender) *
                            kMoonEpCombineV2CreditTransitionCount +
                            transitionStep - 1U) *
                            kMoonEpCombineV2CoreCount + targetCore;
                    ++creditWriters[flat];
                }
            }
        }
        for (uint32_t peer = 0U;
            peer < kMoonEpCombineV2RankCount; ++peer) {
            Check(sendSeen[peer],
                "Group send schedule does not cover every rank");
            Check(receiveSeen[peer],
                "Group receive schedule does not cover every rank");
        }
        Check(selfStep == (((rank >> 2U) & 1U) == 0U ? 2U : 7U),
            "Group Self step does not match the card-half contract");
    }

    for (uint32_t writers : creditWriters) {
        Check(writers == 1U,
            "Group Credit target does not have exactly one writer");
    }
    for (uint32_t step = 0U;
        step < kMoonEpCombineV2StepCount; ++step) {
        std::array<bool, kMoonEpCombineV2StepCount> destinationGroups {};
        for (uint32_t sourceGroup = 0U;
            sourceGroup < kMoonEpCombineV2StepCount; ++sourceGroup) {
            const uint32_t source = MoonEpCombineV2GroupMakeRank(
                sourceGroup >> 2U, sourceGroup & 1U,
                (sourceGroup >> 1U) & 1U, 0U);
            const uint32_t destination =
                MoonEpCombineV2GroupSendDstRank(source, step, 0U);
            const uint32_t destinationGroup =
                MoonEpCombineV2GroupId(destination);
            Check(!destinationGroups[destinationGroup],
                "Group mapping is not bijective within a step");
            destinationGroups[destinationGroup] = true;
        }
    }

    Check(MoonEpCombineV2GroupRecvSrcRank(0U, 0U, 0U) == 8U,
        "rank0/core0 step0 source changed");
    Check(MoonEpCombineV2GroupRecvSrcRank(0U, 1U, 0U) == 76U,
        "rank0/core0 step1 source changed");
    Check(MoonEpCombineV2CreditPublishedAfterStep(6U, 128U),
        "step6 must publish the step7 Credit");
    Check(!MoonEpCombineV2CreditPublishedAfterStep(7U, 128U),
        "step7 must not publish Credit");
    Check(!MoonEpCombineV2CreditRequiredBeforeStep(0U, 128U) &&
            MoonEpCombineV2CreditRequiredBeforeStep(7U, 128U),
        "Group Credit admission boundary mismatch");
    Check(MoonEpCombineV2CreditReceiveOffset(0U, 0U, 0U) == UINT64_MAX &&
            MoonEpCombineV2CreditReceiveOffset(0U, 8U, 0U) == UINT64_MAX,
        "invalid Group Credit transition accessed a slot");
    const uint64_t finalReceiveOffset = MoonEpCombineV2CreditReceiveOffset(
        1U, 7U, 15U);
    Check(finalReceiveOffset + kMoonEpCombineV2CreditSignalBytes ==
            static_cast<uint64_t>(TileXR::CREDIT_IPC_BYTES),
        "Group Credit receive index exceeded IPC memory");

    MoonEpCombineV2CreditSignal signal {};
    signal.magic = 19U;
    signal.marker = kMoonEpCombineV2CreditMarker;
    signal.transitionStep = 1U;
    signal.sourceRank = 0U;
    signal.sourceCore = 0U;
    signal.targetRank = 76U;
    signal.targetCore = 0U;
    signal.guard = MoonEpCombineV2CreditGuard(19U, 1U,
        signal.sourceRank, signal.sourceCore,
        signal.targetRank, signal.targetCore);
    Check(MoonEpCombineV2CreditMatches(signal, 19U, 1U,
            0U, 0U, 76U, 0U),
        "valid Group Credit signal rejected");
    Check(!MoonEpCombineV2CreditMatches(signal, 20U, 1U,
            0U, 0U, 76U, 0U),
        "stale Group Credit magic accepted");
}

void TestTokensAndShapes()
{
    using namespace TileXRMoonEp;
    Check(MoonEpCombineV2ShapeValid(kMoonEpCombineV2TargetBs,
        kMoonEpCombineV2TargetH, kMoonEpCombineV2TargetTopK,
        kMoonEpCombineV2TargetSlots), "target shape rejected");
    Check(MoonEpCombineV2ShapeValid(kMoonEpCombineV2SmallBs,
        kMoonEpCombineV2TargetH, kMoonEpCombineV2TargetTopK,
        kMoonEpCombineV2SmallSlots), "small shape rejected");
    Check(MoonEpCombineV2ShapeValid(256, 1024, 4, 2040),
        "PR113 shape rejected");
    Check(MoonEpCombineV2ReduceInputStrideElements(8U) == 16U,
        "short BF16 reduction rows must use a 32-byte UB stride");
    Check(MoonEpCombineV2ReduceInputStrideElements(4096U) == 4096U,
        "aligned BF16 reduction rows changed stride");
    Check(!MoonEpCombineV2ShapeValid(256, 1024, 4, 1023),
        "undersized NvS accepted");

    const uint64_t magic = 17U;
    const uint64_t token = MoonEpCombineV2Token(magic, 6U);
    Check(MoonEpCombineV2TokenMatches(token, magic, 6U, 128U),
        "token did not match");
    Check(!MoonEpCombineV2TokenMatches(token, magic + 1U, 6U, 128U),
        "stale magic accepted");
    Check(!MoonEpCombineV2TokenMatches(token, magic, 6U, 64U),
        "step outside runtime schedule accepted");
    Check(MoonEpCombineV2DestinationValid(0,
        kMoonEpCombineV2SmallSlots, 128U), "valid destination rejected");
    Check(MoonEpCombineV2DestinationValid(-1,
        2040U, 8U), "padding sentinel rejected");
    Check(!MoonEpCombineV2DestinationValid(-2,
        2040U, 8U), "invalid negative destination accepted");
}

void TestWqeBatchHelpers()
{
    using namespace TileXRMoonEp;
    Check(kMoonEpCombineV2PayloadBatchRows == 128U,
        "payload batch tuning constant mismatch");
    Check(kMoonEpCombineV2BuilderThreads == 128U,
        "SIMT builder thread count mismatch");
    Check(kMoonEpCombineV2MaxSelectedPayloadWqes == 128U,
        "payload submission bound mismatch");
    Check(kMoonEpCombineV2WqeBatchCapacity == 128U,
        "WQE batch capacity mismatch");
    Check(kMoonEpCombineV2BatchQpCount == 2U,
        "WQE batch QP count mismatch");
    Check(kMoonEpCombineV2SelfRelayHalfBytes == 64U * 1024U,
        "Self relay half size mismatch");
    Check(MoonEpCombineV2SelfRowsPerBatch(7168U) == 8U,
        "H=3584 BF16 must use eight Self rows per batch");
    Check(MoonEpCombineV2SelfRowsPerBatch(8192U) == 8U,
        "8 KiB rows must use eight Self rows per batch");
    Check(MoonEpCombineV2SelfRowsPerBatch(14336U) == 4U,
        "14 KiB rows must use four Self rows per batch");
    Check(MoonEpCombineV2SelfRowsPerBatch(32768U) == 2U,
        "32 KiB rows must use two Self rows per batch");
    Check(MoonEpCombineV2SelfRowsPerBatch(65536U) == 1U,
        "64 KiB rows must use one Self row per batch");
    Check(MoonEpCombineV2SelfRowsPerBatch(65537U) == 0U,
        "oversized Self rows must select tiled copy mode");
    Check(MoonEpCombineV2SelfTileCount(65537U) == 2U,
        "oversized Self row tile count mismatch");
    Check(MoonEpCombineV2SelfTileBytes(65537U, 0U) == 65536U,
        "oversized Self row first tile mismatch");
    Check(MoonEpCombineV2SelfTileBytes(65537U, 1U) == 1U,
        "oversized Self row final partial tile mismatch");
    Check(MoonEpCombineV2SelfTileBytes(65537U, 2U) == 0U,
        "out-of-range Self row tile was not rejected");
    Check(MoonEpCombineV2SelfAlignedRowBytes(7169U) == 7200U,
        "Self relay row alignment mismatch");
    Check(MoonEpCombineV2SelfAlignedRowBytes(UINT64_MAX) == 0U,
        "Self relay alignment overflow was not rejected");
    Check(MoonEpCombineV2WqeBatchCount(0U, 0U, 16384U) == 0U,
        "empty WQE batch mismatch");
    Check(MoonEpCombineV2WqeBatchCount(129U, 0U, 16384U) == 128U,
        "WQE batch capacity not enforced");
    Check(MoonEpCombineV2WqeBatchCount(128U, 16380U, 16384U) == 4U,
        "WQE ring wrap not split");
    Check(MoonEpCombineV2CqePollBatchCount(16380U, 16384U, 128U) == 4U,
        "CQ poll ring wrap not split");
    Check(MoonEpCombineV2CqeOwnerReady(0U, 16384U, 1U),
        "initial CQ owner not recognized");
    Check(!MoonEpCombineV2CqeOwnerReady(0U, 16384U, 0U),
        "stale CQ owner accepted");

    uint32_t completed = 0;
    uint32_t detail = 0;
    Check(MoonEpCombineV2AdvanceSingleCqe(0U, 16384U, 1U, 0U, 0U,
        0U, 2U, 1U, 16384U, completed, detail) ==
        MOONEP_COMBINE_V2_SINGLE_CQE_TARGET_REACHED && completed == 2U,
        "CQ completion did not reach the submitted SQ tail");
    Check(MoonEpCombineV2AdvanceSingleCqe(0U, 16384U, 1U, 3U, 4U,
        0U, 2U, 1U, 16384U, completed, detail) ==
        MOONEP_COMBINE_V2_SINGLE_CQE_ERROR && detail == 0x304U,
        "CQ error detail mismatch");

    for (uint32_t phase = 0; phase < 4U; ++phase) {
        for (uint32_t tokenCount = 0;
            tokenCount <= kMoonEpCombineV2MaxSelectedPayloadWqes;
            ++tokenCount) {
            std::array<bool, kMoonEpCombineV2MaxSelectedPayloadWqes> seen {};
            uint32_t selectedCount = 0;
            for (uint32_t qp = 0; qp < kMoonEpCombineV2BatchQpCount; ++qp) {
                const uint32_t qpTokens = MoonEpCombineV2QpTokenCount(
                    tokenCount, phase, qp);
                selectedCount += qpTokens;
                for (uint32_t index = 0; index < qpTokens; ++index) {
                    const uint32_t selected = MoonEpCombineV2QpSelectedIndex(
                        index, phase, qp);
                    Check(selected < tokenCount, "QP selection out of range");
                    Check(!seen[selected], "QP selection duplicated a token");
                    seen[selected] = true;
                }
            }
            Check(selectedCount == tokenCount,
                "QP split did not cover every token");
        }
    }

    for (uint32_t phase = 0; phase < 4U; ++phase) {
        const MoonEpCombineV2LaneCounts payload =
            MoonEpCombineV2BatchLaneCounts(
                kMoonEpCombineV2MaxSelectedPayloadWqes, phase,
                0U, kMoonEpCombineV2StepCount, false);
        Check(payload.sixPort == 96U && payload.twoPort == 32U,
            "maximum payload does not fit the 96/32 QP split");
        const MoonEpCombineV2LaneCounts finalBatch =
            MoonEpCombineV2BatchLaneCounts(
                kMoonEpCombineV2MaxSelectedPayloadWqes, phase,
                0U, kMoonEpCombineV2StepCount, true);
        Check(finalBatch.sixPort == 98U && finalBatch.twoPort == 34U,
            "maximum final batch does not fit the 98/34 issue split");
    }
}

void TestServerPairSchedule()
{
    using namespace TileXRMoonEp;
    const MoonEpCombineV2ScheduleMode mode =
        MOONEP_COMBINE_V2_SERVER_PAIR_SAME_CROSS;
    const uint32_t rankSizes[] = {32U, 64U, 128U};

    Check(!MoonEpCombineV2ServerPairRankSize(16U) &&
            MoonEpCombineV2ServerPairRankSize(32U) &&
            MoonEpCombineV2ServerPairRankSize(64U) &&
            MoonEpCombineV2ServerPairRankSize(128U),
        "Server-Pair rank-size contract mismatch");
    Check(MoonEpCombineV2ServerPairPhaseStepCount(16U) == 0U &&
            MoonEpCombineV2ServerPairRoundCount(16U) == 0U &&
            MoonEpCombineV2ServerPairPhase(0U, 16U) ==
                kMoonEpCombineV2InvalidPeer &&
            MoonEpCombineV2ServerPairPhaseStep(0U, 16U) ==
                kMoonEpCombineV2InvalidPeer,
        "Server-Pair helpers accepted a legacy rank size");
    const MoonEpCombineV2ScheduleCoordinate invalid =
        MoonEpCombineV2ServerPairReceive(0U, 0U, 16U);
    Check(invalid.phase == kMoonEpCombineV2InvalidPeer &&
            invalid.phaseStep == kMoonEpCombineV2InvalidPeer &&
            invalid.round == kMoonEpCombineV2InvalidPeer &&
            invalid.core == kMoonEpCombineV2InvalidPeer,
        "Server-Pair receive accepted a legacy rank size");

    const uint32_t legacyRankSizes[] = {2U, 3U, 4U, 5U, 6U, 7U, 8U, 16U};
    for (uint32_t rankSize : legacyRankSizes) {
        for (uint32_t source = 0U; source < rankSize; ++source) {
            for (uint32_t step = 0U;
                step < MoonEpCombineV2StepCount(rankSize); ++step) {
                for (uint32_t core = 0U;
                    core < MoonEpCombineV2ActiveCoreCount(rankSize); ++core) {
                    Check(MoonEpCombineV2Peer(
                            source, step, core, rankSize, mode) ==
                            MoonEpCombineV2Peer(source, step, core, rankSize,
                                MOONEP_COMBINE_V2_BIDIRECTIONAL_RING),
                        "Server-Pair mode changed the 2P-16P Ring schedule");
                }
            }
        }
    }

    for (uint32_t rankSize : rankSizes) {
        const uint32_t phaseStepCount = rankSize / 32U;
        const uint32_t roundCount = rankSize / 16U;
        Check(MoonEpCombineV2ServerPairPhaseStepCount(rankSize) ==
                phaseStepCount &&
                MoonEpCombineV2ServerPairRoundCount(rankSize) == roundCount,
            "Server-Pair phase dimensions mismatch");
        for (uint32_t source = 0U; source < rankSize; ++source) {
            std::array<bool, kMoonEpCombineV2RankCount> seen {};
            for (uint32_t round = 0U; round < roundCount; ++round) {
                const uint32_t phase = round / phaseStepCount;
                const uint32_t phaseStep = round % phaseStepCount;
                const uint32_t expectedHalf = phase == 0U ?
                    source / (rankSize / 2U) :
                    1U - source / (rankSize / 2U);
                std::array<uint32_t, kMoonEpCombineV2GroupCount>
                    targetServerCounts {};
                for (uint32_t core = 0U;
                    core < kMoonEpCombineV2CoreCount; ++core) {
                    const uint32_t peer = MoonEpCombineV2ServerPairPeer(
                        source, round, core, rankSize);
                    Check(peer < rankSize, "Server-Pair peer out of range");
                    Check(!seen[peer], "Server-Pair peer repeated");
                    seen[peer] = true;
                    Check(peer / (rankSize / 2U) == expectedHalf,
                        "Server-Pair peer selected the wrong target half");
                    Check(peer % kMoonEpCombineV2GroupSize ==
                            core % kMoonEpCombineV2GroupSize,
                        "Server-Pair core changed the target local rank");
                    ++targetServerCounts[
                        peer / kMoonEpCombineV2GroupSize];
                    Check(MoonEpCombineV2Peer(
                            source, round, core, rankSize, mode) == peer,
                        "active Server-Pair mode does not use its peer helper");
                    const MoonEpCombineV2ScheduleCoordinate coordinate =
                        MoonEpCombineV2ServerPairReceive(
                            peer, source, rankSize);
                    Check(coordinate.phase == phase &&
                            coordinate.phaseStep == phaseStep &&
                            coordinate.round == round &&
                            coordinate.core == core,
                        "Server-Pair receive does not invert peer mapping");
                    Check(MoonEpCombineV2ReceiveStep(
                            peer, source, rankSize, mode) == round,
                        "active Server-Pair receive round mismatch");
                    Check(peer == source || MoonEpCombineV2SenderCore(
                            source, peer, rankSize, mode) == core,
                        "active Server-Pair sender core mismatch");
                }
                uint32_t targetServerCount = 0U;
                for (uint32_t count : targetServerCounts) {
                    if (count != 0U) {
                        ++targetServerCount;
                        Check(count == kMoonEpCombineV2GroupSize,
                            "Server-Pair round does not cover a full server");
                    }
                }
                Check(targetServerCount == 2U,
                    "Server-Pair round does not select exactly two servers");
            }
            for (uint32_t destination = 0U;
                destination < rankSize; ++destination) {
                Check(seen[destination],
                    "Server-Pair schedule does not cover every destination");
                const MoonEpCombineV2ScheduleCoordinate coordinate =
                    MoonEpCombineV2ServerPairReceive(
                        destination, source, rankSize);
                Check(coordinate.round < roundCount &&
                        coordinate.core < kMoonEpCombineV2CoreCount &&
                        MoonEpCombineV2ServerPairPeer(source,
                            coordinate.round, coordinate.core, rankSize) ==
                            destination,
                    "Server-Pair coordinate failed exhaustive inversion");
            }
        }

        for (uint32_t destination = 0U;
            destination < rankSize; ++destination) {
            for (uint32_t round = 0U; round < roundCount; ++round) {
                std::array<uint32_t, kMoonEpCombineV2GroupCount>
                    sourceServerCounts {};
                uint32_t sourceCount = 0U;
                for (uint32_t source = 0U; source < rankSize; ++source) {
                    const MoonEpCombineV2ScheduleCoordinate coordinate =
                        MoonEpCombineV2ServerPairReceive(
                            destination, source, rankSize);
                    if (coordinate.round == round) {
                        ++sourceCount;
                        ++sourceServerCounts[
                            source / kMoonEpCombineV2GroupSize];
                        const uint32_t expectedSourceHalf =
                            round / phaseStepCount == 0U ?
                                destination / (rankSize / 2U) :
                                1U - destination / (rankSize / 2U);
                        Check(source / (rankSize / 2U) == expectedSourceHalf,
                            "Server-Pair fan-in came from the wrong half");
                    }
                }
                uint32_t sourceServerCount = 0U;
                for (uint32_t count : sourceServerCounts) {
                    if (count != 0U) {
                        ++sourceServerCount;
                        Check(count == kMoonEpCombineV2GroupSize,
                            "Server-Pair fan-in omitted a source card");
                    }
                }
                Check(sourceCount == 2U * kMoonEpCombineV2GroupSize &&
                        sourceServerCount == 2U,
                    "Server-Pair fan-in is not two complete source servers");
            }
        }
    }

    const uint32_t expected128Servers[8U][2U] = {
        {1U, 2U}, {3U, 4U}, {5U, 6U}, {7U, 0U},
        {9U, 10U}, {11U, 12U}, {13U, 14U}, {15U, 8U}};
    for (uint32_t round = 0U; round < 8U; ++round) {
        for (uint32_t core = 0U; core < 16U; ++core) {
            const uint32_t expectedServer =
                expected128Servers[round][core / 8U];
            Check(MoonEpCombineV2ServerPairPeer(
                    0U, round, core, 128U) ==
                    expectedServer * 8U + core % 8U,
                "128P Server-Pair anchor mismatch");
        }
    }
}

void TestCreditSchedule()
{
    using namespace TileXRMoonEp;
    const uint32_t rankSizes[] = {2U, 3U, 4U, 5U, 6U, 7U, 8U,
        16U, 32U, 64U, 128U};
    const MoonEpCombineV2ScheduleMode modes[] = {
        MOONEP_COMBINE_V2_SINGLE_RING,
        MOONEP_COMBINE_V2_BIDIRECTIONAL_RING,
        MOONEP_COMBINE_V2_SERVER_PAIR_SAME_CROSS,
        MOONEP_COMBINE_V2_SERVER_PAIR_PARITY};

    Check(TileXR::DISPATCH_CREDIT_IPC_BYTES == 131072,
        "Dispatch Credit IPC layout changed");
    Check(kMoonEpCombineV2CreditBaseBytes ==
            static_cast<uint64_t>(TileXR::DISPATCH_CREDIT_IPC_BYTES),
        "Combine Credit overlaps Dispatch Credit");
    Check(kMoonEpCombineV2CreditBytes == 14336U &&
            TileXR::CREDIT_IPC_BYTES == 145408,
        "Combine Credit IPC size mismatch");

    for (uint32_t rankSize : rankSizes) {
        const uint32_t stepCount = MoonEpCombineV2StepCount(rankSize);
        const uint32_t transitionCount =
            MoonEpCombineV2CreditTransitionCount(rankSize);
        Check(transitionCount == stepCount - 1U,
            "Credit transition count mismatch");
        Check(transitionCount == (rankSize == 32U ? 1U :
                rankSize == 64U ? 3U : rankSize == 128U ? 7U : 0U),
            "rank-size Credit transition contract mismatch");
        Check(!MoonEpCombineV2CreditRequiredBeforeStep(0U, rankSize),
            "step0 unexpectedly waits for Credit");

        for (MoonEpCombineV2ScheduleMode mode : modes) {
            const uint32_t activeCores =
                MoonEpCombineV2ActiveCoreCount(rankSize);
            std::vector<uint32_t> writers(
                static_cast<size_t>(rankSize) * transitionCount *
                    activeCores,
                0U);
            for (uint32_t source = 0U; source < rankSize; ++source) {
                for (uint32_t step = 0U; step < stepCount; ++step) {
                    for (uint32_t core = 0U; core < activeCores; ++core) {
                        const uint32_t destination =
                            MoonEpCombineV2EffectivePeer(
                                MoonEpCombineV2Peer(source, step, core,
                                    rankSize, mode),
                                source);
                        Check(destination < rankSize,
                            "effective peer out of range");
                        Check(MoonEpCombineV2TransferCore(source,
                                destination, step, rankSize, mode) == core,
                            "Credit transfer core does not invert peer");
                        const uint32_t receiverCore =
                            MoonEpCombineV2ReceiveCore(destination, source,
                                step, rankSize, mode);
                        Check(receiverCore < activeCores,
                            "Credit receive core is invalid");
                        Check(MoonEpCombineV2ReceiveSource(destination, step,
                                receiverCore, rankSize, mode) == source,
                            "Credit receive source does not invert peer");
                    }
                }
            }

            for (uint32_t destination = 0U;
                destination < rankSize; ++destination) {
                for (uint32_t transition = 1U;
                    transition <= transitionCount; ++transition) {
                    for (uint32_t receiverCore = 0U;
                        receiverCore < activeCores; ++receiverCore) {
                        const uint32_t nextSource =
                            MoonEpCombineV2ReceiveSource(destination,
                                transition, receiverCore, rankSize, mode);
                        Check(MoonEpCombineV2ReceiveCore(destination,
                                nextSource, transition, rankSize, mode) ==
                                receiverCore,
                            "Credit receiver core/source mapping is not inverse");
                        const uint32_t targetCore =
                            MoonEpCombineV2TransferCore(nextSource,
                                destination, transition, rankSize, mode);
                        Check(nextSource < rankSize &&
                                targetCore < activeCores,
                            "Credit publisher route is invalid");
                        if (nextSource >= rankSize ||
                            targetCore >= activeCores) {
                            continue;
                        }
                        const size_t index =
                            (static_cast<size_t>(nextSource) *
                                transitionCount + transition - 1U) *
                                activeCores + targetCore;
                        ++writers[index];

                        MoonEpCombineV2CreditSignal signal {};
                        signal.magic = 19U;
                        signal.marker = kMoonEpCombineV2CreditMarker;
                        signal.transitionStep = transition;
                        signal.sourceRank = destination;
                        signal.sourceCore = receiverCore;
                        signal.targetRank = nextSource;
                        signal.targetCore = targetCore;
                        signal.guard = MoonEpCombineV2CreditGuard(19U,
                            transition, destination, receiverCore,
                            nextSource, targetCore);
                        Check(MoonEpCombineV2CreditMatches(signal, 19U,
                                transition, destination, receiverCore,
                                nextSource, targetCore),
                            "Credit publisher/waiter route mismatch");
                    }
                }
            }
            for (uint32_t count : writers) {
                Check(count == 1U,
                    "Credit receive slot does not have one writer");
            }
        }
    }

    Check(MoonEpCombineV2CreditReceiveOffset(0U, 0U, 0U) == UINT64_MAX &&
            MoonEpCombineV2CreditReceiveOffset(2U, 1U, 0U) == UINT64_MAX &&
            MoonEpCombineV2CreditReceiveOffset(0U, 8U, 0U) == UINT64_MAX,
        "invalid Credit slot accepted");
    const uint64_t lastOffset =
        MoonEpCombineV2CreditReceiveOffset(1U, 7U, 15U);
    Check(lastOffset + kMoonEpCombineV2CreditSignalBytes ==
            static_cast<uint64_t>(TileXR::CREDIT_IPC_BYTES),
        "Combine Credit final slot exceeds IPC allocation");
}

void TestStepRouteMapping()
{
    using namespace TileXRMoonEp;
    const uint32_t rankSizes[] = {2U, 3U, 4U, 5U, 6U, 7U, 8U,
        16U, 32U, 64U, 128U};
    const MoonEpCombineV2ScheduleMode modes[] = {
        MOONEP_COMBINE_V2_SINGLE_RING,
        MOONEP_COMBINE_V2_BIDIRECTIONAL_RING,
        MOONEP_COMBINE_V2_SERVER_PAIR_SAME_CROSS,
        MOONEP_COMBINE_V2_SERVER_PAIR_PARITY};

    for (MoonEpCombineV2ScheduleMode mode : modes) {
        Check(MoonEpCombineV2ScheduleModeValid(mode),
            "valid schedule mode rejected");
        for (uint32_t rankSize : rankSizes) {
            const uint32_t activeCores =
                MoonEpCombineV2ActiveCoreCount(rankSize);
            const uint32_t stepCount = MoonEpCombineV2StepCount(rankSize);
            const uint32_t transitionCount =
                MoonEpCombineV2CreditTransitionCount(rankSize);
            std::vector<uint32_t> creditWriters(
                static_cast<size_t>(rankSize) * transitionCount *
                    activeCores,
                0U);

            for (uint32_t rank = 0U; rank < rankSize; ++rank) {
                for (uint32_t step = 0U; step < stepCount; ++step) {
                    for (uint32_t core = 0U; core < activeCores; ++core) {
                        const uint32_t oldSendDst =
                            MoonEpCombineV2EffectivePeer(
                                MoonEpCombineV2Peer(rank, step, core,
                                    rankSize, mode),
                                rank);
                        const uint32_t oldRecvSrc =
                            MoonEpCombineV2ReceiveSource(
                                rank, step, core, rankSize, mode);
                        Check(MoonEpCombineV2MapSendDst(
                                mode, rank, core, step, rankSize) ==
                                oldSendDst,
                            "step route send mapping changed");
                        Check(MoonEpCombineV2MapRecvSrc(
                                mode, rank, core, step, rankSize) ==
                                oldRecvSrc,
                            "step route receive mapping changed");
                        Check(MoonEpCombineV2FindSendCore(
                                mode, rank, oldSendDst, step, rankSize) ==
                                core,
                            "step route send core is not invertible");

                        const uint32_t oldRecvCore =
                            MoonEpCombineV2ReceiveCore(
                                oldSendDst, rank, step, rankSize, mode);
                        Check(MoonEpCombineV2FindRecvCore(
                                mode, oldSendDst, rank, step, rankSize) ==
                                oldRecvCore,
                            "step route receive core changed");

                        const MoonEpCombineV2StepRoute route =
                            MoonEpCombineV2BuildStepRoute(
                                mode, rank, core, step, rankSize);
                        Check(route.valid,
                            "valid step route was rejected");
                        Check(route.sendDstRank == oldSendDst &&
                                route.recvSrcRank == oldRecvSrc,
                            "step route endpoints changed");
                        const uint32_t oldSuccessor =
                            MoonEpCombineV2Successor(
                                rank, step, core, rankSize, mode);
                        Check(route.sendSuccessorRank == oldSuccessor,
                            "step route send successor changed");

                        const bool waitCredit =
                            MoonEpCombineV2CreditRequiredBeforeStep(
                                step, rankSize);
                        Check(route.waitCredit == waitCredit,
                            "step route Credit wait boundary changed");
                        if (waitCredit) {
                            Check(route.creditWaitSrcRank == oldSendDst &&
                                    route.creditWaitSrcCore == oldRecvCore,
                                "step route Credit wait source changed");
                        } else {
                            Check(route.creditWaitSrcRank ==
                                    kMoonEpCombineV2InvalidPeer &&
                                    route.creditWaitSrcCore ==
                                        kMoonEpCombineV2InvalidPeer,
                                "step0 route contains a Credit wait source");
                        }

                        const bool publishCredit =
                            MoonEpCombineV2CreditPublishedAfterStep(
                                step, rankSize);
                        Check(route.publishCredit == publishCredit,
                            "step route Credit publish boundary changed");
                        if (publishCredit) {
                            const uint32_t transitionStep = step + 1U;
                            const uint32_t oldTargetRank =
                                MoonEpCombineV2ReceiveSource(rank,
                                    transitionStep, core, rankSize, mode);
                            const uint32_t oldTargetCore =
                                MoonEpCombineV2TransferCore(oldTargetRank,
                                    rank, transitionStep, rankSize, mode);
                            Check(route.creditPublishDstRank ==
                                    oldTargetRank &&
                                    route.creditPublishDstCore ==
                                        oldTargetCore,
                                "step route Credit publish target changed");
                            const size_t index =
                                (static_cast<size_t>(oldTargetRank) *
                                    transitionCount + step) *
                                    activeCores + oldTargetCore;
                            ++creditWriters[index];
                        } else {
                            Check(route.creditPublishDstRank ==
                                    kMoonEpCombineV2InvalidPeer &&
                                    route.creditPublishDstCore ==
                                        kMoonEpCombineV2InvalidPeer,
                                "final route contains a Credit publish target");
                        }
                    }
                }
            }

            for (uint32_t count : creditWriters) {
                Check(count == 1U,
                    "step route Credit slot does not have one writer");
            }
        }
    }

    const MoonEpCombineV2ScheduleMode invalidMode =
        static_cast<MoonEpCombineV2ScheduleMode>(UINT32_MAX);
    Check(!MoonEpCombineV2ScheduleModeValid(invalidMode),
        "invalid schedule mode accepted");
    Check(MoonEpCombineV2MapSendDst(
            invalidMode, 0U, 0U, 0U, 32U) ==
            kMoonEpCombineV2InvalidPeer,
        "invalid mode produced a send destination");
    Check(MoonEpCombineV2MapRecvSrc(
            invalidMode, 0U, 0U, 0U, 32U) ==
            kMoonEpCombineV2InvalidPeer,
        "invalid mode produced a receive source");
    Check(!MoonEpCombineV2BuildStepRoute(
            invalidMode, 0U, 0U, 0U, 32U).valid,
        "invalid mode produced a valid step route");
    Check(!MoonEpCombineV2BuildStepRoute(
            MOONEP_COMBINE_V2_SERVER_PAIR_PARITY,
            0U, 16U, 0U, 32U).valid,
        "invalid core produced a valid step route");
    Check(!MoonEpCombineV2BuildStepRoute(
            MOONEP_COMBINE_V2_SERVER_PAIR_PARITY,
            0U, 0U, 2U, 32U).valid,
        "invalid step produced a valid step route");
    Check(!MoonEpCombineV2BuildStepRoute(
            MOONEP_COMBINE_V2_SERVER_PAIR_PARITY,
            32U, 0U, 0U, 32U).valid,
        "invalid rank produced a valid step route");
    Check(!MoonEpCombineV2BuildStepRoute(
            MOONEP_COMBINE_V2_SERVER_PAIR_PARITY,
            0U, 0U, 0U, 12U).valid,
        "unsupported rank size produced a valid step route");
}

void TestServerPairParitySchedule()
{
    using namespace TileXRMoonEp;
    const MoonEpCombineV2ScheduleMode mode =
        MOONEP_COMBINE_V2_SERVER_PAIR_PARITY;
    const uint32_t rankSizes[] = {32U, 64U, 128U};

    const uint32_t legacyRankSizes[] = {2U, 3U, 4U, 5U, 6U, 7U, 8U, 16U};
    for (uint32_t rankSize : legacyRankSizes) {
        for (uint32_t source = 0U; source < rankSize; ++source) {
            for (uint32_t step = 0U;
                step < MoonEpCombineV2StepCount(rankSize); ++step) {
                for (uint32_t core = 0U;
                    core < MoonEpCombineV2ActiveCoreCount(rankSize); ++core) {
                    Check(MoonEpCombineV2Peer(
                            source, step, core, rankSize, mode) ==
                            MoonEpCombineV2Peer(source, step, core, rankSize,
                                MOONEP_COMBINE_V2_BIDIRECTIONAL_RING),
                        "parity Server-Pair changed the 2P-16P Ring schedule");
                }
            }
        }
    }

    for (uint32_t rankSize : rankSizes) {
        const uint32_t phaseStepCount = rankSize / 32U;
        const uint32_t roundCount = rankSize / 16U;
        const uint32_t halfRankCount = rankSize / 2U;
        for (uint32_t source = 0U; source < rankSize; ++source) {
            std::array<bool, kMoonEpCombineV2RankCount> seen {};
            for (uint32_t round = 0U; round < roundCount; ++round) {
                const uint32_t phase = round / phaseStepCount;
                const uint32_t phaseStep = round % phaseStepCount;
                const uint32_t expectedHalf = phase == 0U ?
                    1U - (source & 1U) : source & 1U;
                std::array<uint32_t, kMoonEpCombineV2GroupCount>
                    targetServerCounts {};
                for (uint32_t core = 0U;
                    core < kMoonEpCombineV2CoreCount; ++core) {
                    const uint32_t peer = MoonEpCombineV2ServerPairPeer(
                        source, round, core, rankSize, mode);
                    Check(peer < rankSize,
                        "parity Server-Pair peer out of range");
                    Check(!seen[peer],
                        "parity Server-Pair peer repeated");
                    seen[peer] = true;
                    Check(peer / halfRankCount == expectedHalf,
                        "parity Server-Pair selected the wrong target half");
                    Check(peer % kMoonEpCombineV2GroupSize ==
                            core % kMoonEpCombineV2GroupSize,
                        "parity Server-Pair changed target local rank");
                    ++targetServerCounts[
                        peer / kMoonEpCombineV2GroupSize];
                    Check(MoonEpCombineV2Peer(
                            source, round, core, rankSize, mode) == peer,
                        "active parity mode bypasses its peer helper");
                    const MoonEpCombineV2ScheduleCoordinate coordinate =
                        MoonEpCombineV2ServerPairReceive(
                            peer, source, rankSize, mode);
                    Check(coordinate.phase == phase &&
                            coordinate.phaseStep == phaseStep &&
                            coordinate.round == round &&
                            coordinate.core == core,
                        "parity receive does not invert peer mapping");
                    Check(MoonEpCombineV2ReceiveStep(
                            peer, source, rankSize, mode) == round,
                        "active parity receive round mismatch");
                    Check(peer == source || MoonEpCombineV2SenderCore(
                            source, peer, rankSize, mode) == core,
                        "active parity sender core mismatch");
                }
                uint32_t targetServerCount = 0U;
                for (uint32_t count : targetServerCounts) {
                    if (count != 0U) {
                        ++targetServerCount;
                        Check(count == kMoonEpCombineV2GroupSize,
                            "parity round does not cover a full server");
                    }
                }
                Check(targetServerCount == 2U,
                    "parity round does not select exactly two servers");
            }
            for (uint32_t destination = 0U;
                destination < rankSize; ++destination) {
                Check(seen[destination],
                    "parity schedule does not cover every destination");
                const MoonEpCombineV2ScheduleCoordinate coordinate =
                    MoonEpCombineV2ServerPairReceive(
                        destination, source, rankSize, mode);
                Check(coordinate.round < roundCount &&
                        coordinate.core < kMoonEpCombineV2CoreCount &&
                        MoonEpCombineV2ServerPairPeer(source,
                            coordinate.round, coordinate.core, rankSize,
                            mode) == destination,
                    "parity coordinate failed exhaustive inversion");
            }
        }

        for (uint32_t destination = 0U;
            destination < rankSize; ++destination) {
            const uint32_t destinationHalf = destination / halfRankCount;
            for (uint32_t round = 0U; round < roundCount; ++round) {
                const uint32_t phase = round / phaseStepCount;
                const uint32_t expectedParity = phase == 0U ?
                    1U - destinationHalf : destinationHalf;
                std::array<uint32_t, kMoonEpCombineV2GroupCount>
                    sourceServerCounts {};
                uint32_t sourceCount = 0U;
                for (uint32_t source = 0U; source < rankSize; ++source) {
                    const MoonEpCombineV2ScheduleCoordinate coordinate =
                        MoonEpCombineV2ServerPairReceive(
                            destination, source, rankSize, mode);
                    if (coordinate.round != round) {
                        continue;
                    }
                    ++sourceCount;
                    ++sourceServerCounts[
                        source / kMoonEpCombineV2GroupSize];
                    Check((source & 1U) == expectedParity,
                        "parity fan-in came from the wrong card parity");
                }
                uint32_t sourceServerCount = 0U;
                uint32_t sameHalfServerCount = 0U;
                for (uint32_t server = 0U;
                    server < rankSize / kMoonEpCombineV2GroupSize; ++server) {
                    const uint32_t count = sourceServerCounts[server];
                    if (count == 0U) {
                        continue;
                    }
                    ++sourceServerCount;
                    sameHalfServerCount +=
                        server / (rankSize / 16U) == destinationHalf ? 1U : 0U;
                    Check(count == 4U,
                        "parity fan-in is not four same-parity cards");
                }
                Check(sourceCount == kMoonEpCombineV2CoreCount &&
                        sourceServerCount == 4U,
                    "parity fan-in is not four servers by four cards");
                Check(sameHalfServerCount == 2U,
                    "parity fan-in does not contain two same-half servers");
            }
        }
    }

    const uint32_t expected128Servers[8U][2U] = {
        {9U, 10U}, {11U, 12U}, {13U, 14U}, {15U, 8U},
        {1U, 2U}, {3U, 4U}, {5U, 6U}, {7U, 0U}};
    for (uint32_t round = 0U; round < 8U; ++round) {
        for (uint32_t core = 0U; core < 16U; ++core) {
            Check(MoonEpCombineV2ServerPairPeer(
                    0U, round, core, 128U, mode) ==
                    expected128Servers[round][core / 8U] * 8U + core % 8U,
                "128P parity Server-Pair anchor mismatch");
        }
    }
}
void TestBidirectionalScheduleAnchors()
{
    using namespace TileXRMoonEp;
    const MoonEpCombineV2ScheduleMode mode =
        MOONEP_COMBINE_V2_BIDIRECTIONAL_RING;

    const std::array<uint32_t, 8U> expected8 = {
        1U, 7U, 2U, 6U, 3U, 5U, 4U,
        kMoonEpCombineV2InvalidPeer};
    for (uint32_t core = 0U; core < expected8.size(); ++core) {
        Check(MoonEpCombineV2Peer(0U, 0U, core, 8U, mode) ==
                expected8[core],
            "8P bidirectional anchor mismatch");
    }

    const std::array<uint32_t, 16U> expected32Step0 = {
        1U, 15U, 2U, 14U, 3U, 13U, 4U, 12U,
        16U, 24U, 17U, 31U, 18U, 30U, 19U, 29U};
    const std::array<uint32_t, 16U> expected32Step1 = {
        5U, 11U, 6U, 10U, 7U, 9U, 8U,
        kMoonEpCombineV2InvalidPeer,
        20U, 28U, 21U, 27U, 22U, 26U, 23U, 25U};
    const std::array<uint32_t, 16U> expected64Step0 = {
        1U, 31U, 2U, 30U, 3U, 29U, 4U, 28U,
        32U, 48U, 33U, 63U, 34U, 62U, 35U, 61U};
    const std::array<uint32_t, 16U> expected64Step3 = {
        13U, 19U, 14U, 18U, 15U, 17U, 16U,
        kMoonEpCombineV2InvalidPeer,
        44U, 52U, 45U, 51U, 46U, 50U, 47U, 49U};
    const std::array<uint32_t, 16U> expected128Step0 = {
        1U, 63U, 2U, 62U, 3U, 61U, 4U, 60U,
        64U, 96U, 65U, 127U, 66U, 126U, 67U, 125U};
    const std::array<uint32_t, 16U> expected128Step7 = {
        29U, 35U, 30U, 34U, 31U, 33U, 32U,
        kMoonEpCombineV2InvalidPeer,
        92U, 100U, 93U, 99U, 94U, 98U, 95U, 97U};

    for (uint32_t core = 0U; core < 16U; ++core) {
        Check(MoonEpCombineV2Peer(0U, 0U, core, 32U, mode) ==
                expected32Step0[core],
            "32P bidirectional first-step anchor mismatch");
        Check(MoonEpCombineV2Peer(0U, 1U, core, 32U, mode) ==
                expected32Step1[core],
            "32P bidirectional final-step anchor mismatch");
        Check(MoonEpCombineV2Peer(0U, 0U, core, 64U, mode) ==
                expected64Step0[core],
            "64P bidirectional first-step anchor mismatch");
        Check(MoonEpCombineV2Peer(0U, 3U, core, 64U, mode) ==
                expected64Step3[core],
            "64P bidirectional final-step anchor mismatch");
        Check(MoonEpCombineV2Peer(0U, 0U, core, 128U, mode) ==
                expected128Step0[core],
            "128P bidirectional first-step anchor mismatch");
        Check(MoonEpCombineV2Peer(0U, 7U, core, 128U, mode) ==
                expected128Step7[core],
            "128P bidirectional final-step anchor mismatch");
    }

    const std::array<uint32_t, 16U> expectedRank32Step0 = {
        33U, 31U, 34U, 30U, 35U, 29U, 36U, 28U,
        96U, 64U, 97U, 95U, 98U, 94U, 99U, 93U};
    for (uint32_t core = 0U; core < 16U; ++core) {
        Check(MoonEpCombineV2Peer(32U, 0U, core, 128U, mode) ==
                expectedRank32Step0[core],
            "128P nonzero-center bidirectional anchor mismatch");
    }
}

void TestBidirectionalSchedule()
{
    using namespace TileXRMoonEp;
    const MoonEpCombineV2ScheduleMode single =
        MOONEP_COMBINE_V2_SINGLE_RING;
    const MoonEpCombineV2ScheduleMode bidirectional =
        MOONEP_COMBINE_V2_BIDIRECTIONAL_RING;
    const uint32_t supported[] = {2U, 3U, 4U, 5U, 6U, 7U, 8U,
        16U, 32U, 64U, 128U};

    for (uint32_t rankSize : supported) {
        const uint32_t activeCores = MoonEpCombineV2ActiveCoreCount(rankSize);
        const uint32_t stepCount = MoonEpCombineV2StepCount(rankSize);
        for (uint32_t source = 0U; source < rankSize; ++source) {
            std::array<bool, kMoonEpCombineV2RankCount> seen {};
            uint32_t invalidCount = 0U;
            uint32_t selfCount = 0U;
            for (uint32_t step = 0U; step < stepCount; ++step) {
                for (uint32_t core = 0U; core < activeCores; ++core) {
                    Check(MoonEpCombineV2Peer(
                            source, step, core, rankSize, single) ==
                            MoonEpCombineV2Peer(
                                source, step, core, rankSize),
                        "explicit single-ring mode changed the old peer order");
                    const uint32_t rawPeer = MoonEpCombineV2Peer(
                        source, step, core, rankSize, bidirectional);
                    if (rawPeer == kMoonEpCombineV2InvalidPeer) {
                        ++invalidCount;
                    } else {
                        Check(rawPeer < rankSize,
                            "bidirectional peer out of range");
                    }
                    const uint32_t peer = MoonEpCombineV2EffectivePeer(
                        rawPeer, source);
                    Check(!seen[peer], "bidirectional peer repeated");
                    seen[peer] = true;
                    if (peer == source) {
                        ++selfCount;
                    }
                    Check(MoonEpCombineV2ReceiveStep(
                            peer, source, rankSize, bidirectional) == step,
                        "bidirectional receive step does not invert peer");
                    if (rankSize >= 16U) {
                        const uint32_t halfRankCount = rankSize / 2U;
                        Check(peer / halfRankCount == core / 8U,
                            "bidirectional core crossed target half");
                    }
                    if (step + 1U < stepCount) {
                        const uint32_t successor =
                            MoonEpCombineV2Successor(source, step, core,
                                rankSize, bidirectional);
                        Check(successor / (rankSize / 2U) ==
                                source / (rankSize / 2U),
                            "bidirectional successor crossed source half");
                        const uint32_t nextRawPeer = MoonEpCombineV2Peer(
                            successor, step + 1U, core, rankSize,
                            bidirectional);
                        Check(peer == MoonEpCombineV2EffectivePeer(
                                nextRawPeer, successor),
                            "bidirectional successor schedule mismatch");
                    }
                }
            }
            for (uint32_t peer = 0U; peer < rankSize; ++peer) {
                Check(seen[peer],
                    "bidirectional schedule does not cover every peer");
            }
            const bool optimized = rankSize == 8U || rankSize >= 16U;
            Check(invalidCount == (optimized ? 1U : 0U),
                "bidirectional invalid-peer count mismatch");
            Check(selfCount == 1U,
                "bidirectional schedule must visit Self once");
        }
    }

    TestBidirectionalScheduleAnchors();
}

void TestVectorSelectionReferenceModel()
{
    using namespace TileXRMoonEp;
    const uint32_t slots = 2040U;

    std::vector<int32_t> noMatches(23U, -1);
    Check(SelectPeerIndicesReference(noMatches, 3U, slots).empty(),
        "vector selection produced an index for an empty tail chunk");

    std::vector<int32_t> mixed(kMoonEpCombineV2SelectionChunkRows, -1);
    std::vector<uint32_t> expected;
    for (uint32_t index = 7U; index < mixed.size(); index += 37U) {
        mixed[index] = static_cast<int32_t>(5U * slots + index % slots);
        expected.push_back(index);
    }
    mixed[0] = static_cast<int32_t>(5U * slots);
    expected.insert(expected.begin(), 0U);
    mixed[1] = static_cast<int32_t>(6U * slots);
    const std::vector<uint32_t> selected = SelectPeerIndicesReference(
        mixed, 5U, slots);
    Check(selected == expected,
        "vector selection did not preserve compacted chunk indices");

    std::vector<int32_t> allMatches(1024U);
    for (uint32_t index = 0U; index < allMatches.size(); ++index) {
        allMatches[index] = static_cast<int32_t>(2U * slots + index);
    }
    const std::vector<uint32_t> allSelected = SelectPeerIndicesReference(
        allMatches, 2U, slots);
    Check(allSelected.size() == allMatches.size(),
        "vector selection did not retain every matching index");
    std::array<bool, 1024U> seen {};
    for (uint32_t relative : allSelected) {
        Check(relative < seen.size() && !seen[relative],
            "vector selection duplicated an all-match index");
        if (relative < seen.size()) {
            seen[relative] = true;
        }
    }
}

struct WeightPlanStats {
    uint32_t records = 0U;
    uint32_t batches = 0U;
    uint32_t commands = 0U;
    uint32_t mergedRuns = 0U;
    uint32_t stageReuseWaits = 0U;
};

WeightPlanStats BuildWeightPlanReference(
    const std::vector<uint32_t> &destinations)
{
    using namespace TileXRMoonEp;
    WeightPlanStats stats;
    stats.records = static_cast<uint32_t>(destinations.size());
    uint32_t batchSequence = 0U;
    for (uint32_t batchStart = 0U; batchStart < destinations.size();
        batchStart += kMoonEpCombineV2PayloadBatchRows) {
        const uint32_t batchRows = MoonEpCombineV2WeightBatchRows(
            stats.records, batchStart);
        Check(batchRows > 0U &&
                batchRows <= kMoonEpCombineV2PayloadBatchRows,
            "weight batch row count out of range");
        Check(MoonEpCombineV2WeightStageIndex(batchSequence) ==
                (batchSequence & 1U),
            "weight stage selection does not alternate");
        if (MoonEpCombineV2WeightStageReuseWaitRequired(batchSequence)) {
            ++stats.stageReuseWaits;
        }
        ++stats.batches;

        uint32_t commandStart = 0U;
        while (commandStart < batchRows) {
            const uint32_t firstTarget = destinations[batchStart + commandStart];
            uint32_t runLength = 1U;
            while (commandStart + runLength < batchRows &&
                MoonEpCombineV2WeightRunContinues(firstTarget, runLength,
                    destinations[batchStart + commandStart + runLength])) {
                ++runLength;
            }
            ++stats.commands;
            if (runLength > 1U) {
                ++stats.mergedRuns;
            }
            commandStart += runLength;
        }
        ++batchSequence;
    }
    return stats;
}

void TestWeightBatchAndRunPlan()
{
    using namespace TileXRMoonEp;
    const uint32_t boundaries[] = {0U, 1U, 127U, 128U, 129U, 8191U, 8192U};
    for (uint32_t count : boundaries) {
        std::vector<uint32_t> contiguous(count);
        std::vector<uint32_t> discrete(count);
        for (uint32_t index = 0U; index < count; ++index) {
            contiguous[index] = index;
            discrete[index] = index * 2U;
        }

        const WeightPlanStats contiguousStats =
            BuildWeightPlanReference(contiguous);
        const WeightPlanStats discreteStats = BuildWeightPlanReference(discrete);
        const uint32_t expectedBatches = MoonEpCombineV2WeightBatchCount(count);
        Check(contiguousStats.records == count &&
                contiguousStats.batches == expectedBatches,
            "contiguous weight boundary split mismatch");
        Check(discreteStats.records == count &&
                discreteStats.batches == expectedBatches,
            "discrete weight boundary split mismatch");
        Check(contiguousStats.commands == expectedBatches,
            "contiguous weight plan did not merge each batch");
        Check(discreteStats.commands == count,
            "discrete weight plan merged unrelated destinations");
        const uint32_t expectedReuseWaits =
            expectedBatches > 2U ? expectedBatches - 2U : 0U;
        Check(contiguousStats.stageReuseWaits == expectedReuseWaits &&
                discreteStats.stageReuseWaits == expectedReuseWaits,
            "weight ping-pong stage reuse count mismatch");
        if (count != 0U) {
            const uint32_t lastStart =
                (expectedBatches - 1U) * kMoonEpCombineV2PayloadBatchRows;
            const uint32_t expectedTail = count - lastStart;
            Check(MoonEpCombineV2WeightBatchRows(count, lastStart) ==
                    expectedTail,
                "weight final batch tail mismatch");
        }
    }

    const std::vector<uint32_t> mixed {0U, 1U, 4U, 5U, 6U, 9U};
    const WeightPlanStats mixedStats = BuildWeightPlanReference(mixed);
    Check(mixedStats.commands == 3U && mixedStats.mergedRuns == 2U,
        "mixed weight run plan mismatch");
    Check(!MoonEpCombineV2WeightRunContinues(UINT32_MAX, 1U, 0U),
        "weight run continuation accepted uint32 wraparound");
}

void TestWeightCompactionReferenceModel()
{
    const uint32_t slots = 8192U;
    const uint32_t peer = 3U;
    std::vector<int32_t> destinations(slots, -1);
    std::vector<float> weights(slots, 0.0F);
    std::vector<int32_t> packedDestinations;
    std::vector<float> packedWeights;
    for (uint32_t index = 0U; index < slots; ++index) {
        weights[index] = static_cast<float>(index) + 0.25F;
        if ((index % 3U) != 1U) {
            destinations[index] = static_cast<int32_t>(peer * slots + index);
            packedDestinations.push_back(static_cast<int32_t>(index));
            packedWeights.push_back(weights[index]);
        }
    }

    const std::vector<uint32_t> selected = SelectPeerIndicesReference(
        destinations, peer, slots);
    Check(selected.size() == packedDestinations.size() &&
            selected.size() == packedWeights.size(),
        "weight destination/value compaction counts differ");
    for (uint32_t packed = 0U; packed < selected.size(); ++packed) {
        const uint32_t source = selected[packed];
        Check(packedDestinations[packed] ==
                destinations[source] - static_cast<int32_t>(peer * slots),
            "packed weight destination lost source order");
        Check(packedWeights[packed] == weights[source],
            "packed weight value lost source order");
    }
}

void TestFullmeshRouting()
{
    using namespace TileXRMoonEp;
    const uint32_t rankSizes[] = {2U, 8U, 16U, 32U, 64U, 128U};
    for (uint32_t rankSize : rankSizes) {
        const uint32_t localRankSize = MoonEpCombineV2LocalRankSize(rankSize);
        for (uint32_t rank = 0U; rank < rankSize; ++rank) {
            const uint32_t localRank = rank % localRankSize;
            uint32_t peerMask = 0U;
            uint32_t localRemoteCount = 0U;
            for (uint32_t peer = 0U; peer < rankSize; ++peer) {
                const bool sameServer = rank / localRankSize ==
                    peer / localRankSize;
                Check(MoonEpCombineV2SameServer(
                        rank, peer, localRankSize) == sameServer,
                    "same-server classification mismatch");
                Check(MoonEpCombineV2LocalSlot(peer, localRankSize) ==
                        peer % localRankSize,
                    "Fullmesh local slot mismatch");
                Check(MoonEpCombineV2FullmeshLogicalQp(
                        peer, localRankSize) ==
                        kMoonEpCombineV2FullmeshLogicalQpBase +
                            peer % localRankSize,
                    "Fullmesh logical QP mismatch");
                const uint32_t expectedDone = peer == rank ? 0U :
                    (sameServer ? 1U : 2U);
                Check(MoonEpCombineV2ExpectedDoneCount(
                        peer, rank, localRankSize) == expectedDone,
                    "locality-aware done count mismatch");
                for (uint32_t lane = 0U;
                    lane < kMoonEpCombineV2LaneCount; ++lane) {
                    Check(MoonEpCombineV2DoneLaneRequired(
                            peer, rank, lane, localRankSize) ==
                            (lane < expectedDone),
                        "locality-aware done lane mismatch");
                }
                if (sameServer && peer != rank) {
                    peerMask |= 1U << (peer % localRankSize);
                    ++localRemoteCount;
                }
            }
            Check(localRemoteCount == localRankSize - 1U,
                "Fullmesh peer count mismatch");
            Check(peerMask == TileXR::UDMAFullmeshExpectedPeerMask(
                    localRank, localRankSize),
                "Fullmesh peer mask mismatch");
        }
    }
    Check(!MoonEpCombineV2SameServer(0U, 1U, 0U),
        "zero local-rank size accepted");
    Check(MoonEpCombineV2LocalSlot(1U, 9U) ==
            kMoonEpCombineV2InvalidPeer,
        "oversized Fullmesh domain accepted");
}

} // namespace

int main()
{
    TestSchedule();
    TestGroupAllToAllSchedule();
    TestBidirectionalSchedule();
    TestServerPairSchedule();
    TestServerPairParitySchedule();
    TestCreditSchedule();
    TestStepRouteMapping();
    TestQpAndBatchContract();
    TestTokensAndShapes();
    TestWqeBatchHelpers();
    TestVectorSelectionReferenceModel();
    TestWeightBatchAndRunPlan();
    TestWeightCompactionReferenceModel();
    TestFullmeshRouting();
    return failures == 0 ? 0 : 1;
}
