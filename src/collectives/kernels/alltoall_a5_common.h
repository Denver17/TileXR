/*
 * Copyright (c) 2024-2026 TileXR Project
 * Licensed under the Apache License, Version 2.0
 */

#ifndef TILEXR_ALLTOALL_A5_COMMON_H
#define TILEXR_ALLTOALL_A5_COMMON_H

#include <cstddef>
#include <cstdint>
#include <limits>

#include "comm_args.h"

namespace TileXRGrouped {

#if defined(__CCE__) && defined(__CCE_IS_AICORE__)
#define TILEXR_GROUPED_HOST_DEVICE_INLINE __aicore__ inline
#else
#define TILEXR_GROUPED_HOST_DEVICE_INLINE inline
#endif

constexpr uint32_t kMinRankSize = 8U;
#if defined(TILEXR_GROUPED_KERNEL_VARIANT_64)
constexpr uint32_t kMaxRankSize = 64U;
#else
constexpr uint32_t kMaxRankSize = 128U;
#endif
constexpr uint32_t kRanksPerNode = 8U;
constexpr uint32_t kGroupWidth = 16U;
constexpr uint32_t kHalfGroupWidth = 8U;
constexpr uint32_t kSendCoreCount = 32U;
constexpr uint32_t kCopyoutCoreCount = 1U;
constexpr uint32_t kBlockDim = kSendCoreCount + kCopyoutCoreCount;
constexpr uint32_t kPrimaryRouteParts = 6U;
constexpr uint32_t kRouteParts = 8U;
constexpr uint32_t kWorkspaceAlignment = 512U;
constexpr uint32_t kPlaneCount = 2U;
constexpr uint32_t kRouteCount = 2U;
constexpr uint32_t kRequiredQpCount = kRouteCount * kGroupWidth;
constexpr uint32_t kDirectQpBase = kRequiredQpCount;
constexpr uint32_t kDirectQpCount = kRanksPerNode - 1U;
constexpr uint32_t kDirectSharedQpCount = kDirectQpBase + kDirectQpCount;
constexpr uint32_t kSignalRouteStrideBytes = 512U;
constexpr uint32_t kSignalPeerStrideBytes =
    kRouteCount * kSignalRouteStrideBytes;
constexpr uint32_t kSignalSourceStrideBytes = 512U;
constexpr uint64_t kSignalSourceBytes =
    static_cast<uint64_t>(kSendCoreCount) * kSignalSourceStrideBytes;
constexpr uint32_t kCreditStrideBytes = TileXR::CREDIT_IPC_STRIDE;
constexpr uint32_t kCreditOffsetBytes = 128U;
constexpr uint32_t kStatusWords = 16U;
constexpr uint64_t kStatusBytes = kStatusWords * sizeof(uint64_t);
constexpr uint32_t kStatusSuccess = 0U;
constexpr uint32_t kStatusInvalid = 1U;
constexpr uint32_t kStatusCreditTimeout = 2U;
constexpr uint32_t kStatusPostFailed = 3U;
constexpr uint32_t kStatusQuietFailed = 4U;
constexpr uint32_t kStatusReadyTimeout = 5U;
constexpr uint32_t kStatusSdmaFailed = 6U;
constexpr uint32_t kStatusSdmaPrewarmFailed = 7U;
constexpr uint32_t kWorkspaceRegion = 0U;
constexpr uint32_t kProfileRegionCount = 1U;

struct alignas(8) KernelArgs {
    uint8_t *commArgs;
    uint8_t *profileInfo;
    uint8_t *profileRegistry;
    uint8_t *input;
    uint8_t *output;
    uint8_t *workspace;
    uint8_t *status;
    uint64_t elementsPerPeer;
    uint64_t bytesPerPeer;
    uint64_t bytesPerRank;
    uint64_t waitIterations;
    int64_t magic;
    int64_t previousMagic;
    uint32_t rank;
    uint32_t rankSize;
    uint32_t groupCount;
    uint32_t primaryQp;
    uint32_t secondaryQp;
    uint32_t primaryWeight;
    uint32_t secondaryWeight;
    uint32_t firstInvocation;
    uint32_t reservedProfiling;
    uint64_t payloadOffset[kPlaneCount];
    uint64_t signalOffset[kPlaneCount];
    uint64_t signalSourceOffset;
    uint8_t *profilingGM;
};

struct Layout {
    bool valid = false;
    uint32_t rankSize = 0U;
    uint32_t groupCount = 0U;
    uint64_t elementsPerPeer = 0U;
    uint64_t bytesPerPeer = 0U;
    uint64_t bytesPerRank = 0U;
    uint64_t payloadOffset[kPlaneCount] = {0U, 0U};
    uint64_t signalPlaneBytes = 0U;
    uint64_t signalOffset[kPlaneCount] = {0U, 0U};
    uint64_t signalSourceOffset = 0U;
    uint64_t statusOffset = 0U;
    uint64_t workspaceBytes = 0U;
};

#if !defined(__CCE__) || !defined(__CCE_IS_AICORE__)

inline bool CheckedMul(uint64_t lhs, uint64_t rhs, uint64_t &out)
{
    if (lhs != 0U && rhs > std::numeric_limits<uint64_t>::max() / lhs) {
        return false;
    }
    out = lhs * rhs;
    return true;
}

inline bool CheckedAdd(uint64_t lhs, uint64_t rhs, uint64_t &out)
{
    if (rhs > std::numeric_limits<uint64_t>::max() - lhs) {
        return false;
    }
    out = lhs + rhs;
    return true;
}

inline bool AlignUp(uint64_t value, uint64_t &out)
{
    uint64_t expanded = 0U;
    if (!CheckedAdd(value, kWorkspaceAlignment - 1U, expanded)) {
        return false;
    }
    out = expanded & ~(static_cast<uint64_t>(kWorkspaceAlignment) - 1U);
    return true;
}

#endif

TILEXR_GROUPED_HOST_DEVICE_INLINE bool ValidRankSize(uint32_t rankSize)
{
    return rankSize >= kMinRankSize && rankSize <= kMaxRankSize &&
        rankSize % kRanksPerNode == 0U;
}

enum class KernelVariant : uint32_t {
    Invalid = 0U,
    Rank64 = 1U,
    Rank128 = 2U,
};

TILEXR_GROUPED_HOST_DEVICE_INLINE KernelVariant SelectKernelVariant(uint32_t rankSize)
{
    if (!ValidRankSize(rankSize)) {
        return KernelVariant::Invalid;
    }
    return rankSize <= 64U ? KernelVariant::Rank64 : KernelVariant::Rank128;
}

TILEXR_GROUPED_HOST_DEVICE_INLINE bool SupportedQpCount(uint32_t qpCount)
{
    return qpCount == kDirectSharedQpCount;
}

TILEXR_GROUPED_HOST_DEVICE_INLINE uint32_t DirectQp(
    uint32_t rank, uint32_t peer, uint32_t qpCount)
{
    if (qpCount != kDirectSharedQpCount ||
        rank / kRanksPerNode != peer / kRanksPerNode ||
        rank == peer) {
        return qpCount;
    }
    const uint32_t localRank = rank % kRanksPerNode;
    const uint32_t peerLocalRank = peer % kRanksPerNode;
    return kDirectQpBase +
        (peerLocalRank < localRank ? peerLocalRank : peerLocalRank - 1U);
}

TILEXR_GROUPED_HOST_DEVICE_INLINE uint32_t GroupCount(uint32_t rankSize)
{
    return ValidRankSize(rankSize) ? (rankSize - 1U + kGroupWidth - 1U) / kGroupWidth : 0U;
}

TILEXR_GROUPED_HOST_DEVICE_INLINE int32_t Peer(
    uint32_t rank, uint32_t rankSize, uint32_t group, uint32_t lane)
{
    if (!ValidRankSize(rankSize) || rank >= rankSize || lane >= kGroupWidth ||
        group >= GroupCount(rankSize)) {
        return -1;
    }
#if !defined(TILEXR_GROUPED_KERNEL_VARIANT_64)
    if (rankSize == 128U) {
        constexpr uint32_t partitionSize = 64U;
        const uint32_t partitionBase = rank / partitionSize * partitionSize;
        const uint32_t oppositeBase = partitionSize - partitionBase;
        const uint32_t partitionRank = rank % partitionSize;
        if (lane < kHalfGroupWidth) {
            constexpr uint32_t sideWidth = kHalfGroupWidth / 2U;
            const uint32_t index = lane < sideWidth ? lane : lane - sideWidth;
            const uint32_t distance = group * sideWidth + index + 1U;
            if (distance > partitionSize / 2U ||
                (lane >= sideWidth && distance == partitionSize / 2U)) {
                return -1;
            }
            const uint32_t peer = lane < sideWidth ?
                (partitionRank + distance) % partitionSize :
                (partitionRank + partitionSize - distance) % partitionSize;
            return static_cast<int32_t>(partitionBase + peer);
        }

        const uint32_t remoteLane = lane - kHalfGroupWidth;
        int32_t offset = 0;
        if (group == 0U) {
            // Zero and the diameter are self-inverse, so they share one group.
            if (remoteLane < 4U) {
                offset = static_cast<int32_t>(remoteLane);
            } else if (remoteLane == 4U) {
                offset = static_cast<int32_t>(partitionSize / 2U);
            } else {
                offset = -static_cast<int32_t>(remoteLane - 4U);
            }
        } else {
            const uint32_t index = remoteLane < 4U ?
                remoteLane : remoteLane - 4U;
            const int32_t distance = static_cast<int32_t>(group * 4U + index);
            offset = remoteLane < 4U ? distance : -distance;
        }
        const int32_t remoteRank = static_cast<int32_t>(partitionRank) + offset;
        return static_cast<int32_t>(oppositeBase) +
            (remoteRank + static_cast<int32_t>(partitionSize)) %
                static_cast<int32_t>(partitionSize);
    }
#endif
    const uint32_t index = lane < kHalfGroupWidth ? lane : lane - kHalfGroupWidth;
    const uint32_t distance = group * kHalfGroupWidth + index + 1U;
    const uint32_t diameter = rankSize / 2U;
    if (distance > diameter || (lane >= kHalfGroupWidth && distance == diameter)) {
        return -1;
    }
    return static_cast<int32_t>(lane < kHalfGroupWidth ?
        (rank + distance) % rankSize : (rank + rankSize - distance) % rankSize);
}

TILEXR_GROUPED_HOST_DEVICE_INLINE uint32_t NextCreditGroup(
    uint32_t rank, uint32_t rankSize,
    uint32_t completedGroup, uint32_t lane)
{
    const uint32_t groupCount = GroupCount(rankSize);
    if (completedGroup >= groupCount || lane >= kGroupWidth ||
        Peer(rank, rankSize, completedGroup, lane) < 0) {
        return UINT32_MAX;
    }
    for (uint32_t offset = 1U; offset <= groupCount; ++offset) {
        const uint32_t candidate = (completedGroup + offset) % groupCount;
        if (Peer(rank, rankSize, candidate, lane) >= 0) {
            return candidate;
        }
    }
    return UINT32_MAX;
}

TILEXR_GROUPED_HOST_DEVICE_INLINE int32_t NextCreditPeer(
    uint32_t rank, uint32_t rankSize,
    uint32_t completedGroup, uint32_t lane)
{
    const uint32_t nextGroup = NextCreditGroup(
        rank, rankSize, completedGroup, lane);
    return nextGroup == UINT32_MAX ? -1 :
        Peer(rank, rankSize, nextGroup, lane);
}

TILEXR_GROUPED_HOST_DEVICE_INLINE uint32_t NextCreditTokenGroup(
    uint32_t rank, uint32_t rankSize,
    uint32_t completedGroup, uint32_t lane)
{
    const uint32_t nextGroup = NextCreditGroup(
        rank, rankSize, completedGroup, lane);
    return nextGroup == UINT32_MAX ? UINT32_MAX :
        (nextGroup > completedGroup ? nextGroup : GroupCount(rankSize));
}

TILEXR_GROUPED_HOST_DEVICE_INLINE uint64_t ReadyToken(
    int64_t magic, uint32_t group, uint32_t route)
{
    return (static_cast<uint64_t>(magic) << 16U) |
        (static_cast<uint64_t>(group) << 2U) | static_cast<uint64_t>(route + 1U);
}

TILEXR_GROUPED_HOST_DEVICE_INLINE uint64_t CreditToken(
    int64_t magic, uint32_t group)
{
    return (static_cast<uint64_t>(magic) << 16U) |
        (static_cast<uint64_t>(group) << 2U) | 3U;
}

TILEXR_GROUPED_HOST_DEVICE_INLINE uint64_t IpcEntryOffset(
    uint32_t sourceRank, uint32_t fieldOffset)
{
    return static_cast<uint64_t>(sourceRank) * kCreditStrideBytes + fieldOffset;
}

TILEXR_GROUPED_HOST_DEVICE_INLINE uint64_t SignalByteOffset(
    uint32_t sourceRank, uint32_t route)
{
    return static_cast<uint64_t>(sourceRank) * kSignalPeerStrideBytes +
        static_cast<uint64_t>(route) * kSignalRouteStrideBytes;
}

TILEXR_GROUPED_HOST_DEVICE_INLINE uint64_t SignalSourceByteOffset(uint32_t core)
{
    return static_cast<uint64_t>(core) * kSignalSourceStrideBytes;
}

#if !defined(__CCE__) || !defined(__CCE_IS_AICORE__)

inline Layout BuildLayout(uint32_t rankSize, uint64_t elementsPerPeer)
{
    Layout layout {};
    if (!ValidRankSize(rankSize) || elementsPerPeer == 0U ||
        !CheckedMul(elementsPerPeer, sizeof(int32_t), layout.bytesPerPeer) ||
        layout.bytesPerPeer > UINT32_MAX ||
        !CheckedMul(layout.bytesPerPeer, rankSize, layout.bytesPerRank)) {
        return layout;
    }
    layout.rankSize = rankSize;
    layout.groupCount = GroupCount(rankSize);
    layout.elementsPerPeer = elementsPerPeer;
    layout.payloadOffset[0] = 0U;
    uint64_t cursor = 0U;
    if (!AlignUp(layout.bytesPerRank, layout.payloadOffset[1]) ||
        !CheckedAdd(layout.payloadOffset[1], layout.bytesPerRank, cursor) ||
        !AlignUp(cursor, layout.signalOffset[0]) ||
        !CheckedMul(rankSize, kSignalPeerStrideBytes, layout.signalPlaneBytes) ||
        !CheckedAdd(layout.signalOffset[0], layout.signalPlaneBytes, cursor) ||
        !AlignUp(cursor, layout.signalOffset[1]) ||
        !CheckedAdd(layout.signalOffset[1], layout.signalPlaneBytes, cursor) ||
        !AlignUp(cursor, layout.signalSourceOffset) ||
        !CheckedAdd(layout.signalSourceOffset, kSignalSourceBytes, cursor) ||
        !AlignUp(cursor, layout.statusOffset) ||
        !CheckedAdd(layout.statusOffset, kStatusBytes, layout.workspaceBytes) ||
        !AlignUp(layout.workspaceBytes, layout.workspaceBytes)) {
        return Layout {};
    }
    layout.valid = true;
    return layout;
}

#endif

} // namespace TileXRGrouped

#undef TILEXR_GROUPED_HOST_DEVICE_INLINE

#endif
