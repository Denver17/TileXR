#ifndef TILEXR_MOONEP_COMBINE_V2_SCHEDULE_H
#define TILEXR_MOONEP_COMBINE_V2_SCHEDULE_H

#include <cstdint>
#include <limits>

#include "comm_args.h"

#if defined(__CCE__) && defined(__CCE_IS_AICORE__)
#define TILEXR_MOONEP_COMBINE_V2_INLINE \
    __attribute__((always_inline)) inline __aicore__
#else
#define TILEXR_MOONEP_COMBINE_V2_INLINE inline constexpr
#endif

namespace TileXRMoonEp {

constexpr uint32_t kMoonEpCombineV2RankCount = 128U;
constexpr uint32_t kMoonEpCombineV2GroupSize = 8U;
constexpr uint32_t kMoonEpCombineV2GroupCount = 16U;
constexpr uint32_t kMoonEpCombineV2GroupsPerHalf = 8U;
constexpr uint32_t kMoonEpCombineV2StepCount = 8U;
constexpr uint32_t kMoonEpCombineV2CoreCount = 16U;
constexpr uint32_t kMoonEpCombineV2LaneCount = 2U;
constexpr uint32_t kMoonEpCombineV2QpCount = 32U;
constexpr uint32_t kMoonEpCombineV2FullmeshSlotCount = 8U;
constexpr uint32_t kMoonEpCombineV2FullmeshLogicalQpBase =
    kMoonEpCombineV2QpCount;
constexpr uint32_t kMoonEpCombineV2MaxSourcesPerCore =
    kMoonEpCombineV2RankCount / kMoonEpCombineV2CoreCount;
constexpr uint32_t kMoonEpCombineV2EpochCount = 2U;
constexpr uint32_t kMoonEpCombineV2LogicalBatchRows = 128U;
constexpr uint32_t kMoonEpCombineV2SixPortRows = 96U;
constexpr uint32_t kMoonEpCombineV2TwoPortRows = 32U;
constexpr uint32_t kMoonEpCombineV2SelectionChunkRows = 8192U;
constexpr uint32_t kMoonEpCombineV2MaxOutstanding = 16384U;
constexpr uint32_t kMoonEpCombineV2MteBlockBytes = 32U;
constexpr uint32_t kMoonEpCombineV2CreditTransitionCount =
    kMoonEpCombineV2StepCount - 1U;
constexpr uint64_t kMoonEpCombineV2CreditSignalBytes =
    static_cast<uint64_t>(TileXR::COMBINE_CREDIT_IPC_SIGNAL_BYTES);
constexpr uint64_t kMoonEpCombineV2CreditPlaneBytes =
    static_cast<uint64_t>(TileXR::COMBINE_CREDIT_IPC_PLANE_BYTES);
constexpr uint64_t kMoonEpCombineV2CreditBytes =
    static_cast<uint64_t>(TileXR::COMBINE_CREDIT_IPC_BYTES);
constexpr uint64_t kMoonEpCombineV2CreditBaseBytes =
    static_cast<uint64_t>(TileXR::COMBINE_CREDIT_IPC_BASE);
constexpr uint32_t kMoonEpCombineV2CollectiveStatusSlotCount = 16U;
constexpr uint32_t kMoonEpCombineV2CollectiveStatusSlotBytes = 64U;
constexpr int64_t kMoonEpCombineV2SmallBs = 8;
constexpr int64_t kMoonEpCombineV2SmallSlots = 128;
constexpr int64_t kMoonEpCombineV2TargetBs = 8192;
constexpr int64_t kMoonEpCombineV2TargetH = 3584;
constexpr int64_t kMoonEpCombineV2TargetTopK = 16;
constexpr int64_t kMoonEpCombineV2TargetSlots = 131072;
constexpr uint64_t kMoonEpCombineV2TokenStrideBytes = 64U;
constexpr uint32_t kMoonEpCombineV2FailureMarker = 0x47505632U;
constexpr uint32_t kMoonEpCombineV2CreditMarker = 0x43524454U; // CRDT
constexpr uint32_t kMoonEpCombineV2CollectiveStatusMarker =
    0x43535453U; // CSTS
constexpr uint64_t kMoonEpCombineV2MaxMagic =
    std::numeric_limits<uint64_t>::max() >> 3U;

enum MoonEpCombineV2Lane : uint32_t {
    MOONEP_COMBINE_V2_SIX_PORT = 0U,
    MOONEP_COMBINE_V2_TWO_PORT = 1U,
    MOONEP_COMBINE_V2_FULLMESH = 2U,
};

enum MoonEpCombineV2ScheduleMode : uint32_t {
    MOONEP_COMBINE_V2_SINGLE_RING = 0U,
    MOONEP_COMBINE_V2_BIDIRECTIONAL_RING = 1U,
    MOONEP_COMBINE_V2_SERVER_PAIR_SAME_CROSS = 2U,
    MOONEP_COMBINE_V2_SERVER_PAIR_PARITY = 3U,
    MOONEP_COMBINE_V2_GROUP = 4U,
};

constexpr uint32_t kMoonEpCombineV2InvalidPeer = UINT32_MAX;

enum MoonEpCombineV2FailureStatus : uint32_t {
    MOONEP_COMBINE_V2_SUCCESS = 0U,
    MOONEP_COMBINE_V2_INVALID_CONFIG = 1U,
    MOONEP_COMBINE_V2_POISONED = 2U,
    MOONEP_COMBINE_V2_OUTSTANDING_LIMIT = 3U,
    MOONEP_COMBINE_V2_CQ_TIMEOUT = 4U,
    MOONEP_COMBINE_V2_CQ_ERROR = 5U,
    MOONEP_COMBINE_V2_DONE_TIMEOUT = 7U,
    MOONEP_COMBINE_V2_BAD_DESTINATION = 8U,
    MOONEP_COMBINE_V2_COLLECTIVE_STATUS_ERROR = 12U,
    MOONEP_COMBINE_V2_CREDIT_TIMEOUT = 14U,
    MOONEP_COMBINE_V2_WEIGHT_MEMORY_INVALID_CONFIG = 15U,
    MOONEP_COMBINE_V2_WEIGHT_DONE_TIMEOUT = 16U,
};

struct alignas(64) MoonEpCombineV2CollectiveStatus {
    uint64_t magic;
    uint64_t guard;
    uint32_t marker;
    uint32_t stageId;
    uint32_t status;
    uint32_t firstFailureCore;
    uint64_t reserved[4];
};

struct alignas(64) MoonEpCombineV2CreditSignal {
    uint64_t magic;
    uint64_t guard;
    uint32_t marker;
    uint32_t transitionStep;
    uint32_t sourceRank;
    uint32_t sourceCore;
    uint32_t targetRank;
    uint32_t targetCore;
    uint64_t reserved[3];
};

struct alignas(64) MoonEpCombineV2FailureRecord {
    uint64_t magic;
    uint32_t status;
    uint32_t rank;
    uint32_t core;
    uint32_t step;
    uint32_t peer;
    uint32_t lane;
    uint32_t qp;
    uint32_t cqStatus;
    uint64_t expected;
    uint64_t observed;
    uint32_t poison;
    uint32_t marker;
};

struct MoonEpCombineV2LaneCounts {
    uint32_t sixPort;
    uint32_t twoPort;
};

struct MoonEpCombineV2RingSegments {
    uint32_t first;
    uint32_t second;
};

struct MoonEpCombineV2ScheduleCoordinate {
    uint32_t phase;
    uint32_t phaseStep;
    uint32_t round;
    uint32_t core;
};

struct MoonEpCombineV2StepRoute {
    uint32_t sendDstRank;
    uint32_t recvSrcRank;
    uint32_t sendSuccessorRank;
    uint32_t creditWaitSrcRank;
    uint32_t creditWaitSrcCore;
    uint32_t creditPublishDstRank;
    uint32_t creditPublishDstCore;
    bool waitCredit;
    bool publishCredit;
    bool valid;
};

static_assert(sizeof(MoonEpCombineV2FailureRecord) == 64U,
    "MoonEP Combine V2 failure record ABI changed");
static_assert(sizeof(MoonEpCombineV2CollectiveStatus) ==
        kMoonEpCombineV2CollectiveStatusSlotBytes,
    "MoonEP Combine V2 collective status must occupy one cache line");
static_assert(sizeof(MoonEpCombineV2CreditSignal) ==
        kMoonEpCombineV2CreditSignalBytes,
    "MoonEP Combine V2 Credit signal must occupy one cache line");
static_assert(kMoonEpCombineV2CreditPlaneBytes ==
        kMoonEpCombineV2CreditTransitionCount *
            kMoonEpCombineV2CoreCount * kMoonEpCombineV2CreditSignalBytes,
    "MoonEP Combine V2 Credit plane layout changed");
static_assert(kMoonEpCombineV2CreditBytes ==
        kMoonEpCombineV2EpochCount * kMoonEpCombineV2CreditPlaneBytes,
    "MoonEP Combine V2 Credit epoch layout changed");

TILEXR_MOONEP_COMBINE_V2_INLINE bool
MoonEpCombineV2RankSizeSupported(uint32_t rankSize)
{
    return (rankSize >= 2U && rankSize <= kMoonEpCombineV2GroupSize) ||
        rankSize == 16U || rankSize == 32U || rankSize == 64U ||
        rankSize == kMoonEpCombineV2RankCount;
}

TILEXR_MOONEP_COMBINE_V2_INLINE bool
MoonEpCombineV2ScheduleModeValid(MoonEpCombineV2ScheduleMode mode)
{
    return mode >= MOONEP_COMBINE_V2_SINGLE_RING &&
        mode <= MOONEP_COMBINE_V2_GROUP;
}

TILEXR_MOONEP_COMBINE_V2_INLINE bool
MoonEpCombineV2ScheduleRankSizeSupported(
    MoonEpCombineV2ScheduleMode mode, uint32_t rankSize)
{
    return MoonEpCombineV2ScheduleModeValid(mode) &&
        MoonEpCombineV2RankSizeSupported(rankSize) &&
        (mode != MOONEP_COMBINE_V2_GROUP ||
            rankSize == kMoonEpCombineV2RankCount);
}

TILEXR_MOONEP_COMBINE_V2_INLINE MoonEpCombineV2ScheduleMode
MoonEpCombineV2ScheduleModeForRankSize(uint32_t rankSize)
{
    return rankSize == kMoonEpCombineV2RankCount ?
        MOONEP_COMBINE_V2_GROUP : MOONEP_COMBINE_V2_SINGLE_RING;
}

TILEXR_MOONEP_COMBINE_V2_INLINE bool
MoonEpCombineV2RankValid(uint32_t rank, uint32_t rankSize)
{
    return MoonEpCombineV2RankSizeSupported(rankSize) && rank < rankSize;
}

TILEXR_MOONEP_COMBINE_V2_INLINE uint32_t
MoonEpCombineV2ActiveCoreCount(uint32_t rankSize)
{
    return rankSize <= kMoonEpCombineV2GroupSize ? rankSize :
        kMoonEpCombineV2CoreCount;
}

TILEXR_MOONEP_COMBINE_V2_INLINE uint32_t
MoonEpCombineV2StepCount(uint32_t rankSize)
{
    return rankSize <= kMoonEpCombineV2GroupSize ? 1U :
        rankSize / kMoonEpCombineV2CoreCount;
}

TILEXR_MOONEP_COMBINE_V2_INLINE uint32_t
MoonEpCombineV2CreditTransitionCount(uint32_t rankSize)
{
    const uint32_t stepCount = MoonEpCombineV2StepCount(rankSize);
    return MoonEpCombineV2RankSizeSupported(rankSize) && stepCount != 0U ?
        stepCount - 1U : 0U;
}

TILEXR_MOONEP_COMBINE_V2_INLINE bool
MoonEpCombineV2CreditRequiredBeforeStep(uint32_t step, uint32_t rankSize)
{
    return MoonEpCombineV2RankSizeSupported(rankSize) && step != 0U &&
        step < MoonEpCombineV2StepCount(rankSize);
}

TILEXR_MOONEP_COMBINE_V2_INLINE bool
MoonEpCombineV2CreditPublishedAfterStep(uint32_t step, uint32_t rankSize)
{
    return MoonEpCombineV2RankSizeSupported(rankSize) &&
        step + 1U < MoonEpCombineV2StepCount(rankSize);
}

TILEXR_MOONEP_COMBINE_V2_INLINE uint32_t
MoonEpCombineV2EffectivePeer(uint32_t peer, uint32_t sourceRank)
{
    return peer == kMoonEpCombineV2InvalidPeer ? sourceRank : peer;
}

TILEXR_MOONEP_COMBINE_V2_INLINE uint32_t
MoonEpCombineV2GroupsPerHalf(uint32_t rankSize)
{
    return rankSize / kMoonEpCombineV2CoreCount;
}

TILEXR_MOONEP_COMBINE_V2_INLINE uint32_t
MoonEpCombineV2LocalRankSize(uint32_t rankSize)
{
    return rankSize <= kMoonEpCombineV2GroupSize ? rankSize :
        kMoonEpCombineV2GroupSize;
}

TILEXR_MOONEP_COMBINE_V2_INLINE bool
MoonEpCombineV2CoreValid(uint32_t core, uint32_t rankSize)
{
    return MoonEpCombineV2RankSizeSupported(rankSize) &&
        core < MoonEpCombineV2ActiveCoreCount(rankSize);
}

TILEXR_MOONEP_COMBINE_V2_INLINE uint32_t
MoonEpCombineV2GroupMakeRank(uint32_t cabinet, uint32_t serverParity,
    uint32_t cardHalf, uint32_t groupInnerIndex)
{
    if (cabinet >= 2U || serverParity >= 2U || cardHalf >= 2U ||
        groupInnerIndex >= kMoonEpCombineV2CoreCount) {
        return kMoonEpCombineV2InvalidPeer;
    }
    const uint32_t server = ((groupInnerIndex >> 2U) << 1U) |
        serverParity;
    const uint32_t card = (cardHalf << 2U) | (groupInnerIndex & 3U);
    return (cabinet << 6U) | (server << 3U) | card;
}

TILEXR_MOONEP_COMBINE_V2_INLINE uint32_t
MoonEpCombineV2GroupId(uint32_t rank)
{
    return rank < kMoonEpCombineV2RankCount ?
        ((rank >> 6U) << 2U) | (((rank >> 2U) & 1U) << 1U) |
            ((rank >> 3U) & 1U) :
        kMoonEpCombineV2InvalidPeer;
}

TILEXR_MOONEP_COMBINE_V2_INLINE uint32_t
MoonEpCombineV2GroupInnerIndex(uint32_t rank)
{
    if (rank >= kMoonEpCombineV2RankCount) {
        return kMoonEpCombineV2InvalidPeer;
    }
    const uint32_t server = (rank >> 3U) & 7U;
    return ((server >> 1U) << 2U) | (rank & 3U);
}

TILEXR_MOONEP_COMBINE_V2_INLINE uint32_t
MoonEpCombineV2GroupSendDstRank(
    uint32_t rank, uint32_t step, uint32_t destinationInnerIndex)
{
    if (rank >= kMoonEpCombineV2RankCount ||
        step >= kMoonEpCombineV2StepCount ||
        destinationInnerIndex >= kMoonEpCombineV2CoreCount) {
        return kMoonEpCombineV2InvalidPeer;
    }
    const uint32_t sourceCabinet = rank >> 6U;
    const uint32_t sourceParity = (rank >> 3U) & 1U;
    const uint32_t sourceHalf = (rank >> 2U) & 1U;
    const uint32_t t0 = step & 1U;
    const uint32_t t1 = (step >> 1U) & 1U;
    const uint32_t t2 = (step >> 2U) & 1U;
    const uint32_t interCabinet = sourceHalf ^ t2;
    return MoonEpCombineV2GroupMakeRank(
        sourceCabinet ^ interCabinet, sourceParity ^ t1 ^ 1U,
        interCabinet ^ t0, destinationInnerIndex);
}

TILEXR_MOONEP_COMBINE_V2_INLINE uint32_t
MoonEpCombineV2GroupRecvSrcRank(
    uint32_t rank, uint32_t step, uint32_t sourceInnerIndex)
{
    if (rank >= kMoonEpCombineV2RankCount ||
        step >= kMoonEpCombineV2StepCount ||
        sourceInnerIndex >= kMoonEpCombineV2CoreCount) {
        return kMoonEpCombineV2InvalidPeer;
    }
    const uint32_t destinationCabinet = rank >> 6U;
    const uint32_t destinationParity = (rank >> 3U) & 1U;
    const uint32_t destinationHalf = (rank >> 2U) & 1U;
    const uint32_t t0 = step & 1U;
    const uint32_t t1 = (step >> 1U) & 1U;
    const uint32_t t2 = (step >> 2U) & 1U;
    const uint32_t interCabinet = destinationHalf ^ t0;
    return MoonEpCombineV2GroupMakeRank(
        destinationCabinet ^ interCabinet,
        destinationParity ^ t1 ^ 1U, interCabinet ^ t2,
        sourceInnerIndex);
}
TILEXR_MOONEP_COMBINE_V2_INLINE bool
MoonEpCombineV2StepValid(uint32_t step, uint32_t rankSize)
{
    return MoonEpCombineV2RankSizeSupported(rankSize) &&
        step < MoonEpCombineV2StepCount(rankSize);
}

TILEXR_MOONEP_COMBINE_V2_INLINE bool
MoonEpCombineV2ServerPairRankSize(uint32_t rankSize)
{
    return rankSize == 32U || rankSize == 64U ||
        rankSize == kMoonEpCombineV2RankCount;
}

TILEXR_MOONEP_COMBINE_V2_INLINE uint32_t
MoonEpCombineV2ServerPairPhaseStepCount(uint32_t rankSize)
{
    return MoonEpCombineV2ServerPairRankSize(rankSize) ?
        rankSize / 32U : 0U;
}

TILEXR_MOONEP_COMBINE_V2_INLINE uint32_t
MoonEpCombineV2ServerPairRoundCount(uint32_t rankSize)
{
    return MoonEpCombineV2ServerPairRankSize(rankSize) ?
        rankSize / kMoonEpCombineV2CoreCount : 0U;
}

TILEXR_MOONEP_COMBINE_V2_INLINE uint32_t
MoonEpCombineV2ServerPairPhase(uint32_t round, uint32_t rankSize)
{
    const uint32_t phaseStepCount =
        MoonEpCombineV2ServerPairPhaseStepCount(rankSize);
    return phaseStepCount != 0U &&
            round < MoonEpCombineV2ServerPairRoundCount(rankSize) ?
        round / phaseStepCount : kMoonEpCombineV2InvalidPeer;
}

TILEXR_MOONEP_COMBINE_V2_INLINE uint32_t
MoonEpCombineV2ServerPairPhaseStep(uint32_t round, uint32_t rankSize)
{
    const uint32_t phaseStepCount =
        MoonEpCombineV2ServerPairPhaseStepCount(rankSize);
    return phaseStepCount != 0U &&
            round < MoonEpCombineV2ServerPairRoundCount(rankSize) ?
        round % phaseStepCount : kMoonEpCombineV2InvalidPeer;
}

TILEXR_MOONEP_COMBINE_V2_INLINE uint32_t
MoonEpCombineV2ServerPairTargetHalf(
    uint32_t sourceRank, uint32_t phase, uint32_t rankSize,
    MoonEpCombineV2ScheduleMode mode =
        MOONEP_COMBINE_V2_SERVER_PAIR_SAME_CROSS)
{
    if (!MoonEpCombineV2ServerPairRankSize(rankSize) ||
        !MoonEpCombineV2RankValid(sourceRank, rankSize) || phase >= 2U) {
        return kMoonEpCombineV2InvalidPeer;
    }
    if (mode == MOONEP_COMBINE_V2_SERVER_PAIR_PARITY) {
        const uint32_t sourceParity = sourceRank & 1U;
        return phase == 0U ? 1U - sourceParity : sourceParity;
    }
    if (mode != MOONEP_COMBINE_V2_SERVER_PAIR_SAME_CROSS) {
        return kMoonEpCombineV2InvalidPeer;
    }
    const uint32_t sourceHalf = sourceRank / (rankSize / 2U);
    return phase == 0U ? sourceHalf : 1U - sourceHalf;
}

TILEXR_MOONEP_COMBINE_V2_INLINE uint32_t
MoonEpCombineV2ServerPairIndex(
    uint32_t sourceRank, uint32_t phaseStep, uint32_t rankSize)
{
    const uint32_t phaseStepCount =
        MoonEpCombineV2ServerPairPhaseStepCount(rankSize);
    if (!MoonEpCombineV2RankValid(sourceRank, rankSize) ||
        phaseStepCount == 0U || phaseStep >= phaseStepCount) {
        return kMoonEpCombineV2InvalidPeer;
    }
    const uint32_t sourceServer =
        sourceRank / kMoonEpCombineV2GroupSize;
    return (sourceServer % phaseStepCount + phaseStep) % phaseStepCount;
}

TILEXR_MOONEP_COMBINE_V2_INLINE uint32_t
MoonEpCombineV2ServerPairPeer(uint32_t sourceRank, uint32_t round,
    uint32_t core, uint32_t rankSize,
    MoonEpCombineV2ScheduleMode mode =
        MOONEP_COMBINE_V2_SERVER_PAIR_SAME_CROSS)
{
    if (!MoonEpCombineV2RankValid(sourceRank, rankSize) ||
        !MoonEpCombineV2ServerPairRankSize(rankSize) ||
        core >= kMoonEpCombineV2CoreCount) {
        return kMoonEpCombineV2InvalidPeer;
    }
    const uint32_t phase = MoonEpCombineV2ServerPairPhase(round, rankSize);
    const uint32_t phaseStep =
        MoonEpCombineV2ServerPairPhaseStep(round, rankSize);
    const uint32_t targetHalf = MoonEpCombineV2ServerPairTargetHalf(
        sourceRank, phase, rankSize, mode);
    const uint32_t pairIndex = MoonEpCombineV2ServerPairIndex(
        sourceRank, phaseStep, rankSize);
    if (targetHalf == kMoonEpCombineV2InvalidPeer ||
        pairIndex == kMoonEpCombineV2InvalidPeer) {
        return kMoonEpCombineV2InvalidPeer;
    }
    const uint32_t phaseStepCount =
        MoonEpCombineV2ServerPairPhaseStepCount(rankSize);
    const uint32_t serversPerHalf = 2U * phaseStepCount;
    const uint32_t halfBase = targetHalf * serversPerHalf;
    const uint32_t firstServer = halfBase +
        (2U * pairIndex + 1U) % serversPerHalf;
    const uint32_t secondServer = halfBase +
        (2U * pairIndex + 2U) % serversPerHalf;
    const uint32_t targetServer = core < kMoonEpCombineV2GroupSize ?
        firstServer : secondServer;
    return targetServer * kMoonEpCombineV2GroupSize +
        core % kMoonEpCombineV2GroupSize;
}

TILEXR_MOONEP_COMBINE_V2_INLINE MoonEpCombineV2ScheduleCoordinate
MoonEpCombineV2ServerPairReceive(
    uint32_t destinationRank, uint32_t sourceRank, uint32_t rankSize,
    MoonEpCombineV2ScheduleMode mode =
        MOONEP_COMBINE_V2_SERVER_PAIR_SAME_CROSS)
{
    const MoonEpCombineV2ScheduleCoordinate invalid = {
        kMoonEpCombineV2InvalidPeer, kMoonEpCombineV2InvalidPeer,
        kMoonEpCombineV2InvalidPeer, kMoonEpCombineV2InvalidPeer};
    if (!MoonEpCombineV2ServerPairRankSize(rankSize) ||
        !MoonEpCombineV2RankValid(destinationRank, rankSize) ||
        !MoonEpCombineV2RankValid(sourceRank, rankSize)) {
        return invalid;
    }
    const uint32_t phaseStepCount =
        MoonEpCombineV2ServerPairPhaseStepCount(rankSize);
    const uint32_t serversPerHalf = 2U * phaseStepCount;
    const uint32_t sourceServer =
        sourceRank / kMoonEpCombineV2GroupSize;
    const uint32_t destinationServer =
        destinationRank / kMoonEpCombineV2GroupSize;
    const uint32_t destinationLocal =
        destinationRank % kMoonEpCombineV2GroupSize;
    const uint32_t targetHalf = destinationServer / serversPerHalf;
    const uint32_t relativeServer =
        destinationServer - targetHalf * serversPerHalf;
    const uint32_t pairIndex =
        ((relativeServer + serversPerHalf - 1U) % serversPerHalf) / 2U;
    const uint32_t phaseStep =
        (pairIndex + phaseStepCount - sourceServer % phaseStepCount) %
        phaseStepCount;
    uint32_t phase = kMoonEpCombineV2InvalidPeer;
    if (mode == MOONEP_COMBINE_V2_SERVER_PAIR_PARITY) {
        phase = targetHalf == (sourceRank & 1U) ? 1U : 0U;
    } else if (mode == MOONEP_COMBINE_V2_SERVER_PAIR_SAME_CROSS) {
        const uint32_t sourceHalf = sourceRank / (rankSize / 2U);
        phase = targetHalf == sourceHalf ? 0U : 1U;
    } else {
        return invalid;
    }
    const uint32_t firstServer = targetHalf * serversPerHalf +
        (2U * pairIndex + 1U) % serversPerHalf;
    const uint32_t core = destinationServer == firstServer ?
        destinationLocal : kMoonEpCombineV2GroupSize + destinationLocal;
    return MoonEpCombineV2ScheduleCoordinate {
        phase, phaseStep, phase * phaseStepCount + phaseStep, core};
}

TILEXR_MOONEP_COMBINE_V2_INLINE uint32_t
MoonEpCombineV2ServerPairSenderCore(
    uint32_t sourceRank, uint32_t destinationRank, uint32_t rankSize,
    MoonEpCombineV2ScheduleMode mode =
        MOONEP_COMBINE_V2_SERVER_PAIR_SAME_CROSS)
{
    return MoonEpCombineV2ServerPairReceive(
        destinationRank, sourceRank, rankSize, mode).core;
}
TILEXR_MOONEP_COMBINE_V2_INLINE bool
MoonEpCombineV2MagicValid(uint64_t magic)
{
    return magic != 0U && magic <= kMoonEpCombineV2MaxMagic;
}

TILEXR_MOONEP_COMBINE_V2_INLINE uint32_t
MoonEpCombineV2Epoch(uint64_t magic)
{
    return static_cast<uint32_t>(magic & 1U);
}

TILEXR_MOONEP_COMBINE_V2_INLINE uint32_t
MoonEpCombineV2Qp(uint32_t core, uint32_t lane)
{
    return lane == MOONEP_COMBINE_V2_SIX_PORT ? core :
        kMoonEpCombineV2CoreCount + core;
}

TILEXR_MOONEP_COMBINE_V2_INLINE bool
MoonEpCombineV2SameServer(
    uint32_t lhs, uint32_t rhs, uint32_t localRankSize)
{
    return localRankSize > 0U &&
        localRankSize <= kMoonEpCombineV2FullmeshSlotCount &&
        lhs / localRankSize == rhs / localRankSize;
}

TILEXR_MOONEP_COMBINE_V2_INLINE uint32_t
MoonEpCombineV2LocalSlot(uint32_t rank, uint32_t localRankSize)
{
    return localRankSize == 0U ||
            localRankSize > kMoonEpCombineV2FullmeshSlotCount ?
        kMoonEpCombineV2InvalidPeer : rank % localRankSize;
}

TILEXR_MOONEP_COMBINE_V2_INLINE uint32_t
MoonEpCombineV2FullmeshLogicalQp(uint32_t peer, uint32_t localRankSize)
{
    const uint32_t slot = MoonEpCombineV2LocalSlot(peer, localRankSize);
    return slot == kMoonEpCombineV2InvalidPeer ? slot :
        kMoonEpCombineV2FullmeshLogicalQpBase + slot;
}

TILEXR_MOONEP_COMBINE_V2_INLINE uint32_t
MoonEpCombineV2ExpectedDoneCount(
    uint32_t source, uint32_t destination, uint32_t localRankSize)
{
    return source == destination ? 0U :
        (MoonEpCombineV2SameServer(source, destination, localRankSize) ?
            1U : kMoonEpCombineV2LaneCount);
}

TILEXR_MOONEP_COMBINE_V2_INLINE bool
MoonEpCombineV2DoneLaneRequired(uint32_t source, uint32_t destination,
    uint32_t lane, uint32_t localRankSize)
{
    const uint32_t doneCount = MoonEpCombineV2ExpectedDoneCount(
        source, destination, localRankSize);
    return lane < doneCount;
}

TILEXR_MOONEP_COMBINE_V2_INLINE uint32_t
MoonEpCombineV2LaneForPosition(uint32_t position)
{
    return (position & 3U) == 3U ? MOONEP_COMBINE_V2_TWO_PORT :
        MOONEP_COMBINE_V2_SIX_PORT;
}

TILEXR_MOONEP_COMBINE_V2_INLINE uint32_t
MoonEpCombineV2ControlWqesPerLane(
    uint32_t step, uint32_t stepCount, bool finalBatch)
{
    return finalBatch ? 2U : 0U;
}

TILEXR_MOONEP_COMBINE_V2_INLINE uint32_t
MoonEpCombineV2NextStep(uint32_t step, uint32_t stepCount)
{
    return step + 1U == stepCount ? 0U : step + 1U;
}

TILEXR_MOONEP_COMBINE_V2_INLINE MoonEpCombineV2LaneCounts
MoonEpCombineV2BatchLaneCounts(uint32_t logicalRows,
    uint32_t sequencePhase, uint32_t step, uint32_t stepCount,
    bool finalBatch)
{
    const uint32_t firstTwoPort =
        (3U - (sequencePhase & 3U)) & 3U;
    const uint32_t twoPort = logicalRows <= firstTwoPort ? 0U :
        1U + (logicalRows - 1U - firstTwoPort) / 4U;
    const uint32_t control =
        MoonEpCombineV2ControlWqesPerLane(step, stepCount, finalBatch);
    return MoonEpCombineV2LaneCounts {
        logicalRows - twoPort + control, twoPort + control};
}

TILEXR_MOONEP_COMBINE_V2_INLINE MoonEpCombineV2RingSegments
MoonEpCombineV2SplitRingCopy(uint32_t absoluteHead,
    uint32_t count, uint32_t ringEntries)
{
    const uint32_t ringIndex = ringEntries == 0U ? 0U :
        absoluteHead % ringEntries;
    const uint32_t untilEnd = ringEntries - ringIndex;
    const uint32_t first = count < untilEnd ? count : untilEnd;
    return ringEntries == 0U ? MoonEpCombineV2RingSegments {0U, 0U} :
        MoonEpCombineV2RingSegments {first, count - first};
}

TILEXR_MOONEP_COMBINE_V2_INLINE uint32_t
MoonEpCombineV2NextCqTarget(
    uint32_t cqTail, bool finalBatch)
{
    return finalBatch ? cqTail + 1U : cqTail;
}

TILEXR_MOONEP_COMBINE_V2_INLINE uint32_t
MoonEpCombineV2CompletionCount(bool finalBatch)
{
    return finalBatch ? 1U : 0U;
}

TILEXR_MOONEP_COMBINE_V2_INLINE bool
MoonEpCombineV2CqTargetReached(
    uint32_t cqTail, uint32_t cqTarget)
{
    return cqTail == cqTarget;
}

TILEXR_MOONEP_COMBINE_V2_INLINE bool
MoonEpCombineV2ShapeValid(
    int64_t bs, int64_t h, int64_t topK, int64_t nvS)
{
    return bs > 0 && h > 0 && topK > 0 && topK <= 32 && nvS > 0 &&
        bs <= nvS / topK && nvS <= std::numeric_limits<int32_t>::max();
}

TILEXR_MOONEP_COMBINE_V2_INLINE uint32_t
MoonEpCombineV2ReduceInputStrideElements(uint32_t tileElements)
{
    const uint32_t bytes = tileElements * sizeof(uint16_t);
    return ((bytes + kMoonEpCombineV2MteBlockBytes - 1U) /
        kMoonEpCombineV2MteBlockBytes * kMoonEpCombineV2MteBlockBytes) /
        sizeof(uint16_t);
}

TILEXR_MOONEP_COMBINE_V2_INLINE bool
MoonEpCombineV2DestinationValid(
    int32_t encodedDestination, uint64_t slots, uint32_t rankSize)
{
    return MoonEpCombineV2RankSizeSupported(rankSize) &&
        slots != 0U &&
        (encodedDestination == -1 ||
            (encodedDestination >= 0 &&
            static_cast<uint64_t>(encodedDestination) <
                static_cast<uint64_t>(rankSize) * slots));
}

TILEXR_MOONEP_COMBINE_V2_INLINE uint32_t
MoonEpCombineV2SingleRingPeer(
    uint32_t sourceRank, uint32_t step, uint32_t core, uint32_t rankSize)
{
    if (rankSize <= kMoonEpCombineV2GroupSize) {
        return core;
    }
    const uint32_t groupsPerHalf = MoonEpCombineV2GroupsPerHalf(rankSize);
    const uint32_t sourceGroup =
        (sourceRank / kMoonEpCombineV2GroupSize) %
        groupsPerHalf;
    const uint32_t targetHalf = core / kMoonEpCombineV2GroupSize;
    const uint32_t targetOffset = core % kMoonEpCombineV2GroupSize;
    const uint32_t distance = (step + 1U) % groupsPerHalf;
    const uint32_t targetIndex = core < kMoonEpCombineV2GroupSize ?
        (sourceGroup + distance) % groupsPerHalf :
        (sourceGroup + groupsPerHalf - distance) % groupsPerHalf;
    return (targetIndex + targetHalf * groupsPerHalf) *
        kMoonEpCombineV2GroupSize + targetOffset;
}

TILEXR_MOONEP_COMBINE_V2_INLINE uint32_t
MoonEpCombineV2RingOffset(
    uint32_t position, int32_t offset, uint32_t ringSize)
{
    if (offset >= 0) {
        return (position + static_cast<uint32_t>(offset)) % ringSize;
    }
    return (position + ringSize - static_cast<uint32_t>(-offset)) % ringSize;
}

TILEXR_MOONEP_COMBINE_V2_INLINE int32_t
MoonEpCombineV2BidirectionalOffset(
    uint32_t step, uint32_t lane, uint32_t ringSize, bool sameHalf)
{
    const uint32_t ordinal = step * kMoonEpCombineV2GroupSize + lane;
    if (sameHalf) {
        if (ordinal + 1U == ringSize) {
            return 0;
        }
        const int32_t distance = static_cast<int32_t>(ordinal / 2U + 1U);
        return (ordinal & 1U) == 0U ? distance : -distance;
    }
    if (ordinal == 0U) {
        return 0;
    }
    if (ordinal == 1U) {
        return static_cast<int32_t>(ringSize / 2U);
    }
    const uint32_t pairOrdinal = ordinal - 2U;
    const int32_t distance = static_cast<int32_t>(pairOrdinal / 2U + 1U);
    return (pairOrdinal & 1U) == 0U ? distance : -distance;
}

TILEXR_MOONEP_COMBINE_V2_INLINE bool
MoonEpCombineV2BidirectionalScheduleEnabled(
    uint32_t rankSize, MoonEpCombineV2ScheduleMode mode)
{
    const bool bidirectionalMode =
        mode == MOONEP_COMBINE_V2_BIDIRECTIONAL_RING ||
        mode == MOONEP_COMBINE_V2_SERVER_PAIR_SAME_CROSS ||
        mode == MOONEP_COMBINE_V2_SERVER_PAIR_PARITY;
    return bidirectionalMode &&
        (rankSize == kMoonEpCombineV2GroupSize || rankSize >= 16U);
}

TILEXR_MOONEP_COMBINE_V2_INLINE bool
MoonEpCombineV2ServerPairScheduleEnabled(
    uint32_t rankSize, MoonEpCombineV2ScheduleMode mode)
{
    return (mode == MOONEP_COMBINE_V2_SERVER_PAIR_SAME_CROSS ||
            mode == MOONEP_COMBINE_V2_SERVER_PAIR_PARITY) &&
        MoonEpCombineV2ServerPairRankSize(rankSize);
}
TILEXR_MOONEP_COMBINE_V2_INLINE uint32_t
MoonEpCombineV2BidirectionalPeer(
    uint32_t sourceRank, uint32_t step, uint32_t core, uint32_t rankSize)
{
    if (rankSize == kMoonEpCombineV2GroupSize) {
        if (core + 1U == rankSize) {
            return kMoonEpCombineV2InvalidPeer;
        }
        return MoonEpCombineV2RingOffset(sourceRank,
            MoonEpCombineV2BidirectionalOffset(
                step, core, rankSize, true), rankSize);
    }

    const uint32_t halfRankCount = rankSize / 2U;
    const uint32_t sourceHalf = sourceRank / halfRankCount;
    const uint32_t targetHalf = core / kMoonEpCombineV2GroupSize;
    const uint32_t lane = core % kMoonEpCombineV2GroupSize;
    const uint32_t ordinal = step * kMoonEpCombineV2GroupSize + lane;
    const bool sameHalf = sourceHalf == targetHalf;
    if (sameHalf && ordinal + 1U == halfRankCount) {
        return kMoonEpCombineV2InvalidPeer;
    }
    const uint32_t targetLocal = MoonEpCombineV2RingOffset(
        sourceRank % halfRankCount,
        MoonEpCombineV2BidirectionalOffset(
            step, lane, halfRankCount, sameHalf),
        halfRankCount);
    return targetHalf * halfRankCount + targetLocal;
}

TILEXR_MOONEP_COMBINE_V2_INLINE uint32_t
MoonEpCombineV2Peer(
    uint32_t sourceRank, uint32_t step, uint32_t core, uint32_t rankSize,
    MoonEpCombineV2ScheduleMode mode)
{
    if (MoonEpCombineV2ServerPairScheduleEnabled(rankSize, mode)) {
        return MoonEpCombineV2ServerPairPeer(
            sourceRank, step, core, rankSize, mode);
    }
    return MoonEpCombineV2BidirectionalScheduleEnabled(rankSize, mode) ?
        MoonEpCombineV2BidirectionalPeer(
            sourceRank, step, core, rankSize) :
        MoonEpCombineV2SingleRingPeer(sourceRank, step, core, rankSize);
}

TILEXR_MOONEP_COMBINE_V2_INLINE uint32_t
MoonEpCombineV2Peer(
    uint32_t sourceRank, uint32_t step, uint32_t core, uint32_t rankSize)
{
    return MoonEpCombineV2Peer(sourceRank, step, core, rankSize,
        MOONEP_COMBINE_V2_SINGLE_RING);
}

TILEXR_MOONEP_COMBINE_V2_INLINE uint32_t
MoonEpCombineV2SingleRingSuccessor(
    uint32_t sourceRank, uint32_t core, uint32_t rankSize)
{
    if (rankSize <= kMoonEpCombineV2GroupSize) {
        return sourceRank;
    }
    const uint32_t halfRankCount = rankSize / 2U;
    const uint32_t groupsPerHalf = MoonEpCombineV2GroupsPerHalf(rankSize);
    const uint32_t halfBase = sourceRank / halfRankCount * halfRankCount;
    const uint32_t groupInHalf = (sourceRank % halfRankCount) /
        kMoonEpCombineV2GroupSize;
    const uint32_t successorGroup = core < kMoonEpCombineV2GroupSize ?
        (groupInHalf + groupsPerHalf - 1U) % groupsPerHalf :
        (groupInHalf + 1U) % groupsPerHalf;
    return halfBase + successorGroup * kMoonEpCombineV2GroupSize +
        sourceRank % kMoonEpCombineV2GroupSize;
}

TILEXR_MOONEP_COMBINE_V2_INLINE uint32_t
MoonEpCombineV2Successor(
    uint32_t sourceRank, uint32_t step, uint32_t core, uint32_t rankSize,
    MoonEpCombineV2ScheduleMode mode)
{
    if (!MoonEpCombineV2BidirectionalScheduleEnabled(rankSize, mode) ||
        rankSize <= kMoonEpCombineV2GroupSize) {
        return MoonEpCombineV2SingleRingSuccessor(
            sourceRank, core, rankSize);
    }
    const uint32_t halfRankCount = rankSize / 2U;
    const uint32_t sourceHalf = sourceRank / halfRankCount;
    const uint32_t targetHalf = core / kMoonEpCombineV2GroupSize;
    const uint32_t lane = core % kMoonEpCombineV2GroupSize;
    const bool sameHalf = sourceHalf == targetHalf;
    const uint32_t nextStep = MoonEpCombineV2NextStep(
        step, MoonEpCombineV2StepCount(rankSize));
    const int32_t currentOffset = MoonEpCombineV2BidirectionalOffset(
        step, lane, halfRankCount, sameHalf);
    const int32_t nextOffset = MoonEpCombineV2BidirectionalOffset(
        nextStep, lane, halfRankCount, sameHalf);
    const uint32_t targetLocal = MoonEpCombineV2RingOffset(
        sourceRank % halfRankCount, currentOffset, halfRankCount);
    const uint32_t successorLocal = MoonEpCombineV2RingOffset(
        targetLocal, -nextOffset, halfRankCount);
    return sourceHalf * halfRankCount + successorLocal;
}

TILEXR_MOONEP_COMBINE_V2_INLINE uint32_t
MoonEpCombineV2Successor(
    uint32_t sourceRank, uint32_t core, uint32_t rankSize)
{
    return MoonEpCombineV2SingleRingSuccessor(
        sourceRank, core, rankSize);
}

TILEXR_MOONEP_COMBINE_V2_INLINE uint32_t
MoonEpCombineV2SingleRingReceiveStep(
    uint32_t destinationRank, uint32_t sourceRank, uint32_t rankSize)
{
    if (rankSize <= kMoonEpCombineV2GroupSize) {
        return 0U;
    }
    const uint32_t groupsPerHalf = MoonEpCombineV2GroupsPerHalf(rankSize);
    const uint32_t halfRankCount = rankSize / 2U;
    const uint32_t destinationIndex =
        (destinationRank / kMoonEpCombineV2GroupSize) %
        groupsPerHalf;
    const uint32_t sourceIndex =
        (sourceRank / kMoonEpCombineV2GroupSize) %
        groupsPerHalf;
    const uint32_t delta = destinationRank < halfRankCount ?
        (destinationIndex + groupsPerHalf - sourceIndex) % groupsPerHalf :
        (sourceIndex + groupsPerHalf - destinationIndex) % groupsPerHalf;
    const uint32_t distance = delta == 0U ? groupsPerHalf : delta;
    return distance - 1U;
}

TILEXR_MOONEP_COMBINE_V2_INLINE uint32_t
MoonEpCombineV2ReceiveStep(
    uint32_t destinationRank, uint32_t sourceRank, uint32_t rankSize,
    MoonEpCombineV2ScheduleMode mode)
{
    if (MoonEpCombineV2ServerPairScheduleEnabled(rankSize, mode)) {
        return MoonEpCombineV2ServerPairReceive(
            destinationRank, sourceRank, rankSize, mode).round;
    }
    if (!MoonEpCombineV2BidirectionalScheduleEnabled(rankSize, mode) ||
        rankSize <= kMoonEpCombineV2GroupSize) {
        return MoonEpCombineV2SingleRingReceiveStep(
            destinationRank, sourceRank, rankSize);
    }
    const uint32_t halfRankCount = rankSize / 2U;
    const uint32_t sourceLocal = sourceRank % halfRankCount;
    const uint32_t destinationLocal = destinationRank % halfRankCount;
    const uint32_t clockwise =
        (destinationLocal + halfRankCount - sourceLocal) % halfRankCount;
    const uint32_t counterClockwise =
        clockwise == 0U ? 0U : halfRankCount - clockwise;
    const uint32_t distance = clockwise < counterClockwise ?
        clockwise : counterClockwise;
    const bool sameHalf = destinationRank / halfRankCount ==
        sourceRank / halfRankCount;
    if (sameHalf) {
        return distance == 0U ? MoonEpCombineV2StepCount(rankSize) - 1U :
            (distance - 1U) / 4U;
    }
    return distance == halfRankCount / 2U ? 0U : distance / 4U;
}

TILEXR_MOONEP_COMBINE_V2_INLINE uint32_t
MoonEpCombineV2ReceiveStep(
    uint32_t destinationRank, uint32_t sourceRank, uint32_t rankSize)
{
    return MoonEpCombineV2ReceiveStep(destinationRank, sourceRank, rankSize,
        MOONEP_COMBINE_V2_SINGLE_RING);
}

TILEXR_MOONEP_COMBINE_V2_INLINE uint32_t
MoonEpCombineV2SourceForCore(
    uint32_t core, uint32_t sourceIndex, uint32_t rankSize)
{
    return core + sourceIndex * MoonEpCombineV2ActiveCoreCount(rankSize);
}

TILEXR_MOONEP_COMBINE_V2_INLINE uint32_t
MoonEpCombineV2SenderCore(
    uint32_t sourceRank, uint32_t destinationRank, uint32_t rankSize,
    MoonEpCombineV2ScheduleMode mode)
{
    if (!MoonEpCombineV2RankValid(sourceRank, rankSize) ||
        !MoonEpCombineV2RankValid(destinationRank, rankSize) ||
        sourceRank == destinationRank) {
        return kMoonEpCombineV2InvalidPeer;
    }
    if (MoonEpCombineV2ServerPairScheduleEnabled(rankSize, mode)) {
        return MoonEpCombineV2ServerPairSenderCore(
            sourceRank, destinationRank, rankSize, mode);
    }
    const uint32_t activeCoreCount = MoonEpCombineV2ActiveCoreCount(rankSize);
    const uint32_t stepCount = MoonEpCombineV2StepCount(rankSize);
    for (uint32_t core = 0U; core < activeCoreCount; ++core) {
        for (uint32_t step = 0U; step < stepCount; ++step) {
            if (MoonEpCombineV2Peer(
                    sourceRank, step, core, rankSize, mode) ==
                destinationRank) {
                return core;
            }
        }
    }
    return kMoonEpCombineV2InvalidPeer;
}

TILEXR_MOONEP_COMBINE_V2_INLINE uint32_t
MoonEpCombineV2ReceiveSource(uint32_t destinationRank, uint32_t step,
    uint32_t receiverCore, uint32_t rankSize,
    MoonEpCombineV2ScheduleMode mode)
{
    if (!MoonEpCombineV2RankValid(destinationRank, rankSize) ||
        !MoonEpCombineV2StepValid(step, rankSize) ||
        !MoonEpCombineV2CoreValid(receiverCore, rankSize)) {
        return kMoonEpCombineV2InvalidPeer;
    }
    uint32_t matchedCount = 0U;
    for (uint32_t sourceRank = 0U; sourceRank < rankSize; ++sourceRank) {
        if (MoonEpCombineV2ReceiveStep(
                destinationRank, sourceRank, rankSize, mode) != step) {
            continue;
        }
        if (matchedCount == receiverCore) {
            return sourceRank;
        }
        ++matchedCount;
    }
    return kMoonEpCombineV2InvalidPeer;
}

TILEXR_MOONEP_COMBINE_V2_INLINE uint32_t
MoonEpCombineV2ReceiveCore(uint32_t destinationRank, uint32_t sourceRank,
    uint32_t step, uint32_t rankSize, MoonEpCombineV2ScheduleMode mode)
{
    if (!MoonEpCombineV2RankValid(destinationRank, rankSize) ||
        !MoonEpCombineV2RankValid(sourceRank, rankSize) ||
        !MoonEpCombineV2StepValid(step, rankSize) ||
        MoonEpCombineV2ReceiveStep(
            destinationRank, sourceRank, rankSize, mode) != step) {
        return kMoonEpCombineV2InvalidPeer;
    }
    uint32_t receiverCore = 0U;
    for (uint32_t candidate = 0U; candidate < rankSize; ++candidate) {
        if (MoonEpCombineV2ReceiveStep(
                destinationRank, candidate, rankSize, mode) != step) {
            continue;
        }
        if (candidate == sourceRank) {
            return receiverCore;
        }
        ++receiverCore;
    }
    return kMoonEpCombineV2InvalidPeer;
}

TILEXR_MOONEP_COMBINE_V2_INLINE uint32_t
MoonEpCombineV2TransferCore(uint32_t sourceRank, uint32_t destinationRank,
    uint32_t step, uint32_t rankSize, MoonEpCombineV2ScheduleMode mode)
{
    if (!MoonEpCombineV2RankValid(sourceRank, rankSize) ||
        !MoonEpCombineV2RankValid(destinationRank, rankSize) ||
        !MoonEpCombineV2StepValid(step, rankSize)) {
        return kMoonEpCombineV2InvalidPeer;
    }
    const uint32_t activeCoreCount = MoonEpCombineV2ActiveCoreCount(rankSize);
    uint32_t matchedCore = kMoonEpCombineV2InvalidPeer;
    for (uint32_t core = 0U; core < activeCoreCount; ++core) {
        const uint32_t peer = MoonEpCombineV2EffectivePeer(
            MoonEpCombineV2Peer(sourceRank, step, core, rankSize, mode),
            sourceRank);
        if (peer != destinationRank) {
            continue;
        }
        if (matchedCore != kMoonEpCombineV2InvalidPeer) {
            return kMoonEpCombineV2InvalidPeer;
        }
        matchedCore = core;
    }
    return matchedCore;
}

TILEXR_MOONEP_COMBINE_V2_INLINE uint32_t
MoonEpCombineV2MapSendDst(MoonEpCombineV2ScheduleMode mode,
    uint32_t sourceRank, uint32_t senderCore, uint32_t step,
    uint32_t rankSize)
{
    if (!MoonEpCombineV2ScheduleRankSizeSupported(mode, rankSize) ||
        !MoonEpCombineV2RankValid(sourceRank, rankSize) ||
        !MoonEpCombineV2CoreValid(senderCore, rankSize) ||
        !MoonEpCombineV2StepValid(step, rankSize)) {
        return kMoonEpCombineV2InvalidPeer;
    }
    if (mode == MOONEP_COMBINE_V2_GROUP) {
        return MoonEpCombineV2GroupSendDstRank(
            sourceRank, step, senderCore);
    }
    return MoonEpCombineV2EffectivePeer(
        MoonEpCombineV2Peer(
            sourceRank, step, senderCore, rankSize, mode),
        sourceRank);
}

TILEXR_MOONEP_COMBINE_V2_INLINE uint32_t
MoonEpCombineV2MapRecvSrc(MoonEpCombineV2ScheduleMode mode,
    uint32_t destinationRank, uint32_t receiverCore, uint32_t step,
    uint32_t rankSize)
{
    if (!MoonEpCombineV2ScheduleRankSizeSupported(mode, rankSize) ||
        !MoonEpCombineV2RankValid(destinationRank, rankSize) ||
        !MoonEpCombineV2CoreValid(receiverCore, rankSize) ||
        !MoonEpCombineV2StepValid(step, rankSize)) {
        return kMoonEpCombineV2InvalidPeer;
    }
    if (mode == MOONEP_COMBINE_V2_GROUP) {
        return MoonEpCombineV2GroupRecvSrcRank(
            destinationRank, step, receiverCore);
    }
    return MoonEpCombineV2ReceiveSource(
        destinationRank, step, receiverCore, rankSize, mode);
}

TILEXR_MOONEP_COMBINE_V2_INLINE uint32_t
MoonEpCombineV2FindRecvCore(MoonEpCombineV2ScheduleMode mode,
    uint32_t destinationRank, uint32_t sourceRank, uint32_t step,
    uint32_t rankSize)
{
    if (!MoonEpCombineV2ScheduleRankSizeSupported(mode, rankSize) ||
        !MoonEpCombineV2RankValid(destinationRank, rankSize) ||
        !MoonEpCombineV2RankValid(sourceRank, rankSize) ||
        !MoonEpCombineV2StepValid(step, rankSize)) {
        return kMoonEpCombineV2InvalidPeer;
    }
    if (mode == MOONEP_COMBINE_V2_GROUP) {
        return MoonEpCombineV2GroupInnerIndex(sourceRank);
    }
    // Current modes retain their cheaper inverse; later modes use the
    // mapping-only fallback below until they provide an equivalent fast path.
    if (mode <= MOONEP_COMBINE_V2_SERVER_PAIR_PARITY) {
        return MoonEpCombineV2ReceiveCore(
            destinationRank, sourceRank, step, rankSize, mode);
    }
    const uint32_t activeCoreCount = MoonEpCombineV2ActiveCoreCount(rankSize);
    uint32_t matchedCore = kMoonEpCombineV2InvalidPeer;
    for (uint32_t core = 0U; core < activeCoreCount; ++core) {
        if (MoonEpCombineV2MapRecvSrc(
                mode, destinationRank, core, step, rankSize) != sourceRank) {
            continue;
        }
        if (matchedCore != kMoonEpCombineV2InvalidPeer) {
            return kMoonEpCombineV2InvalidPeer;
        }
        matchedCore = core;
    }
    return matchedCore;
}

TILEXR_MOONEP_COMBINE_V2_INLINE uint32_t
MoonEpCombineV2FindSendCore(MoonEpCombineV2ScheduleMode mode,
    uint32_t sourceRank, uint32_t destinationRank, uint32_t step,
    uint32_t rankSize)
{
    if (!MoonEpCombineV2ScheduleRankSizeSupported(mode, rankSize) ||
        !MoonEpCombineV2RankValid(sourceRank, rankSize) ||
        !MoonEpCombineV2RankValid(destinationRank, rankSize) ||
        !MoonEpCombineV2StepValid(step, rankSize)) {
        return kMoonEpCombineV2InvalidPeer;
    }
    if (mode == MOONEP_COMBINE_V2_GROUP) {
        return MoonEpCombineV2GroupInnerIndex(destinationRank);
    }
    if (mode <= MOONEP_COMBINE_V2_SERVER_PAIR_PARITY) {
        return MoonEpCombineV2TransferCore(
            sourceRank, destinationRank, step, rankSize, mode);
    }
    const uint32_t activeCoreCount = MoonEpCombineV2ActiveCoreCount(rankSize);
    uint32_t matchedCore = kMoonEpCombineV2InvalidPeer;
    for (uint32_t core = 0U; core < activeCoreCount; ++core) {
        if (MoonEpCombineV2MapSendDst(
                mode, sourceRank, core, step, rankSize) != destinationRank) {
            continue;
        }
        if (matchedCore != kMoonEpCombineV2InvalidPeer) {
            return kMoonEpCombineV2InvalidPeer;
        }
        matchedCore = core;
    }
    return matchedCore;
}

TILEXR_MOONEP_COMBINE_V2_INLINE uint32_t
MoonEpCombineV2FindSendSuccessor(MoonEpCombineV2ScheduleMode mode,
    uint32_t sourceRank, uint32_t senderCore, uint32_t step,
    uint32_t rankSize)
{
    if (!MoonEpCombineV2ScheduleRankSizeSupported(mode, rankSize) ||
        !MoonEpCombineV2RankValid(sourceRank, rankSize) ||
        !MoonEpCombineV2CoreValid(senderCore, rankSize) ||
        !MoonEpCombineV2StepValid(step, rankSize)) {
        return kMoonEpCombineV2InvalidPeer;
    }
    if (mode == MOONEP_COMBINE_V2_GROUP) {
        const uint32_t destinationRank = MoonEpCombineV2GroupSendDstRank(
            sourceRank, step, senderCore);
        const uint32_t nextStep = MoonEpCombineV2NextStep(
            step, MoonEpCombineV2StepCount(rankSize));
        return MoonEpCombineV2GroupRecvSrcRank(destinationRank, nextStep,
            MoonEpCombineV2GroupInnerIndex(sourceRank));
    }
    if (mode <= MOONEP_COMBINE_V2_SERVER_PAIR_PARITY) {
        return MoonEpCombineV2Successor(
            sourceRank, step, senderCore, rankSize, mode);
    }

    const uint32_t destinationRank = MoonEpCombineV2MapSendDst(
        mode, sourceRank, senderCore, step, rankSize);
    const uint32_t nextStep = MoonEpCombineV2NextStep(
        step, MoonEpCombineV2StepCount(rankSize));
    uint32_t matchedSource = kMoonEpCombineV2InvalidPeer;
    for (uint32_t candidate = 0U; candidate < rankSize; ++candidate) {
        if (MoonEpCombineV2MapSendDst(
                mode, candidate, senderCore, nextStep, rankSize) !=
            destinationRank) {
            continue;
        }
        if (matchedSource != kMoonEpCombineV2InvalidPeer) {
            return kMoonEpCombineV2InvalidPeer;
        }
        matchedSource = candidate;
    }
    return matchedSource;
}

TILEXR_MOONEP_COMBINE_V2_INLINE MoonEpCombineV2StepRoute
MoonEpCombineV2BuildStepRoute(MoonEpCombineV2ScheduleMode mode,
    uint32_t rank, uint32_t core, uint32_t step, uint32_t rankSize)
{
    MoonEpCombineV2StepRoute route = {
        kMoonEpCombineV2InvalidPeer,
        kMoonEpCombineV2InvalidPeer,
        kMoonEpCombineV2InvalidPeer,
        kMoonEpCombineV2InvalidPeer,
        kMoonEpCombineV2InvalidPeer,
        kMoonEpCombineV2InvalidPeer,
        kMoonEpCombineV2InvalidPeer,
        false,
        false,
        false};
    if (!MoonEpCombineV2ScheduleRankSizeSupported(mode, rankSize) ||
        !MoonEpCombineV2RankValid(rank, rankSize) ||
        !MoonEpCombineV2CoreValid(core, rankSize) ||
        !MoonEpCombineV2StepValid(step, rankSize)) {
        return route;
    }

    route.sendDstRank = MoonEpCombineV2MapSendDst(
        mode, rank, core, step, rankSize);
    route.recvSrcRank = MoonEpCombineV2MapRecvSrc(
        mode, rank, core, step, rankSize);
    route.sendSuccessorRank = MoonEpCombineV2FindSendSuccessor(
        mode, rank, core, step, rankSize);
    if (!MoonEpCombineV2RankValid(route.sendDstRank, rankSize) ||
        !MoonEpCombineV2RankValid(route.recvSrcRank, rankSize) ||
        !MoonEpCombineV2RankValid(route.sendSuccessorRank, rankSize)) {
        return route;
    }

    route.waitCredit = MoonEpCombineV2CreditRequiredBeforeStep(
        step, rankSize);
    if (route.waitCredit) {
        route.creditWaitSrcRank = route.sendDstRank;
        route.creditWaitSrcCore = MoonEpCombineV2FindRecvCore(
            mode, route.sendDstRank, rank, step, rankSize);
        if (!MoonEpCombineV2CoreValid(
                route.creditWaitSrcCore, rankSize)) {
            return route;
        }
    }

    route.publishCredit = MoonEpCombineV2CreditPublishedAfterStep(
        step, rankSize);
    if (route.publishCredit) {
        const uint32_t transitionStep = step + 1U;
        route.creditPublishDstRank = MoonEpCombineV2MapRecvSrc(
            mode, rank, core, transitionStep, rankSize);
        route.creditPublishDstCore = MoonEpCombineV2FindSendCore(
            mode, route.creditPublishDstRank, rank, transitionStep,
            rankSize);
        if (!MoonEpCombineV2RankValid(
                route.creditPublishDstRank, rankSize) ||
            !MoonEpCombineV2CoreValid(
                route.creditPublishDstCore, rankSize)) {
            return route;
        }
    }

    route.valid = true;
    return route;
}

TILEXR_MOONEP_COMBINE_V2_INLINE uint64_t
MoonEpCombineV2CollectiveStatusGuard(uint64_t magic, uint32_t stageId)
{
    return magic ^
        (static_cast<uint64_t>(kMoonEpCombineV2CollectiveStatusMarker) <<
            32U) ^
        static_cast<uint64_t>(stageId);
}
TILEXR_MOONEP_COMBINE_V2_INLINE uint64_t
MoonEpCombineV2CreditGuard(uint64_t magic, uint32_t transitionStep,
    uint32_t sourceRank, uint32_t sourceCore, uint32_t targetRank,
    uint32_t targetCore)
{
    const uint64_t route = static_cast<uint64_t>(sourceRank) |
        (static_cast<uint64_t>(sourceCore) << 7U) |
        (static_cast<uint64_t>(targetRank) << 11U) |
        (static_cast<uint64_t>(targetCore) << 18U) |
        (static_cast<uint64_t>(transitionStep) << 22U);
    return magic ^
        (static_cast<uint64_t>(kMoonEpCombineV2CreditMarker) << 32U) ^
        route;
}

TILEXR_MOONEP_COMBINE_V2_INLINE bool
MoonEpCombineV2CreditMatches(
    const MoonEpCombineV2CreditSignal &signal, uint64_t magic,
    uint32_t transitionStep, uint32_t sourceRank, uint32_t sourceCore,
    uint32_t targetRank, uint32_t targetCore)
{
    return MoonEpCombineV2MagicValid(magic) &&
        transitionStep > 0U && transitionStep < kMoonEpCombineV2StepCount &&
        sourceRank < kMoonEpCombineV2RankCount &&
        sourceCore < kMoonEpCombineV2CoreCount &&
        targetRank < kMoonEpCombineV2RankCount &&
        targetCore < kMoonEpCombineV2CoreCount &&
        signal.magic == magic &&
        signal.marker == kMoonEpCombineV2CreditMarker &&
        signal.transitionStep == transitionStep &&
        signal.sourceRank == sourceRank && signal.sourceCore == sourceCore &&
        signal.targetRank == targetRank && signal.targetCore == targetCore &&
        signal.guard == MoonEpCombineV2CreditGuard(magic,
            transitionStep, sourceRank, sourceCore, targetRank, targetCore);
}

TILEXR_MOONEP_COMBINE_V2_INLINE uint64_t
MoonEpCombineV2CollectiveStatusIndex(uint32_t epoch, uint32_t stageId)
{
    return static_cast<uint64_t>(epoch) *
        kMoonEpCombineV2CollectiveStatusSlotCount +
        stageId % kMoonEpCombineV2CollectiveStatusSlotCount;
}

TILEXR_MOONEP_COMBINE_V2_INLINE uint64_t
MoonEpCombineV2Token(
    uint64_t magic, uint32_t step)
{
    return (magic << 3U) | static_cast<uint64_t>(step);
}

TILEXR_MOONEP_COMBINE_V2_INLINE bool
MoonEpCombineV2TokenMatches(
    uint64_t token, uint64_t magic, uint32_t step, uint32_t rankSize)
{
    return MoonEpCombineV2MagicValid(magic) &&
        MoonEpCombineV2StepValid(step, rankSize) &&
        token == MoonEpCombineV2Token(magic, step);
}

TILEXR_MOONEP_COMBINE_V2_INLINE uint64_t
MoonEpCombineV2DoneIndex(
    uint32_t epoch, uint32_t sourceRank, uint32_t lane)
{
    return (static_cast<uint64_t>(epoch) * kMoonEpCombineV2RankCount +
        sourceRank) * kMoonEpCombineV2LaneCount + lane;
}

TILEXR_MOONEP_COMBINE_V2_INLINE uint64_t
MoonEpCombineV2CreditSignalIndex(
    uint32_t epoch, uint32_t transitionStep, uint32_t targetCore)
{
    if (epoch >= kMoonEpCombineV2EpochCount || transitionStep == 0U ||
        transitionStep > kMoonEpCombineV2CreditTransitionCount ||
        targetCore >= kMoonEpCombineV2CoreCount) {
        return UINT64_MAX;
    }
    return (static_cast<uint64_t>(epoch) *
            kMoonEpCombineV2CreditTransitionCount + transitionStep - 1U) *
        kMoonEpCombineV2CoreCount + targetCore;
}

TILEXR_MOONEP_COMBINE_V2_INLINE uint64_t
MoonEpCombineV2CreditReceiveOffset(
    uint32_t epoch, uint32_t transitionStep, uint32_t targetCore)
{
    const uint64_t index = MoonEpCombineV2CreditSignalIndex(
        epoch, transitionStep, targetCore);
    return index == UINT64_MAX ? UINT64_MAX :
        kMoonEpCombineV2CreditBaseBytes +
            index * kMoonEpCombineV2CreditSignalBytes;
}

TILEXR_MOONEP_COMBINE_V2_INLINE uint64_t
MoonEpCombineV2FailureIndex(
    uint32_t epoch, uint32_t core)
{
    return static_cast<uint64_t>(epoch) * kMoonEpCombineV2CoreCount + core;
}

} // namespace TileXRMoonEp

#undef TILEXR_MOONEP_COMBINE_V2_INLINE

#endif // TILEXR_MOONEP_COMBINE_V2_SCHEDULE_H
