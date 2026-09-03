/*
 * Copyright (c) 2024-2026 TileXR Project
 * Licensed under the Apache License, Version 2.0
 */

#include "udma/tilexr_udma_layout.h"

#include "udma/tilexr_udma_config.h"

#include <cstring>
#include <limits>

namespace TileXR {
namespace {

bool CheckedMultiply(size_t lhs, size_t rhs, size_t& result)
{
    if (lhs != 0 && rhs > std::numeric_limits<size_t>::max() / lhs) {
        return false;
    }
    result = lhs * rhs;
    return true;
}

bool CheckedAdd(size_t lhs, size_t rhs, size_t& result)
{
    if (rhs > std::numeric_limits<size_t>::max() - lhs) {
        return false;
    }
    result = lhs + rhs;
    return true;
}

bool CheckedArrayEnd(size_t offset, size_t count, size_t elementSize, size_t& end)
{
    size_t arrayBytes = 0;
    return CheckedMultiply(count, elementSize, arrayBytes) && CheckedAdd(offset, arrayBytes, end);
}

bool CheckedDeviceAddress(uintptr_t deviceBase, size_t offset, uint64_t& address)
{
    constexpr uint64_t maxAddress = std::numeric_limits<uint64_t>::max();
    if (deviceBase > static_cast<uintptr_t>(maxAddress) || offset > maxAddress) {
        return false;
    }
    const uint64_t base = static_cast<uint64_t>(deviceBase);
    const uint64_t deviceOffset = static_cast<uint64_t>(offset);
    if (deviceOffset > maxAddress - base) {
        return false;
    }
    address = base + deviceOffset;
    return true;
}

template <typename T>
void CopyVector(std::vector<uint8_t>& dst, size_t offset, const std::vector<T>& src)
{
    if (!src.empty()) {
        std::memcpy(dst.data() + offset, src.data(), src.size() * sizeof(T));
    }
}

} // namespace

uint32_t UDMAGroupedSharedQpLane(
    int rank, int peer, int rankSize, uint32_t laneCount)
{
    if (rankSize <= 1 || rank < 0 || peer < 0 || rank >= rankSize ||
        peer >= rankSize || rank == peer || laneCount == 0U ||
        laneCount % 2U != 0U) {
        return laneCount;
    }

    if (rankSize == 128 && laneCount == 16U) {
        constexpr int partitionSize = 64;
        const int rankPartition = rank / partitionSize;
        const int peerPartition = peer / partitionSize;
        const uint32_t rankInPartition = static_cast<uint32_t>(rank % partitionSize);
        const uint32_t peerInPartition = static_cast<uint32_t>(peer % partitionSize);
        const uint32_t forward =
            (peerInPartition + partitionSize - rankInPartition) % partitionSize;
        if (rankPartition == peerPartition) {
            if (forward <= partitionSize / 2U) {
                return (forward - 1U) % 4U;
            }
            const uint32_t backward = partitionSize - forward;
            return 4U + (backward - 1U) % 4U;
        }
        if (forward == 0U) {
            return 8U;
        }
        if (forward < partitionSize / 2U) {
            return 8U + forward % 4U;
        }
        if (forward == partitionSize / 2U) {
            return 12U;
        }
        const uint32_t backward = partitionSize - forward;
        return 12U + backward % 4U;
    }

    const uint32_t lanesPerDirection = laneCount / 2U;
    const uint32_t forward = static_cast<uint32_t>(
        (peer - rank + rankSize) % rankSize);
    const uint32_t backward = static_cast<uint32_t>(rankSize) - forward;
    return forward <= backward ?
        (forward - 1U) % lanesPerDirection :
        lanesPerDirection + (backward - 1U) % lanesPerDirection;
}

uint32_t UDMAGroupedRemoteQpIndex(
    int rank, int peer, int rankSize, uint32_t qpIdx,
    uint32_t qpCount, uint32_t laneCount, uint32_t localRankSize)
{
    if (laneCount == 0U || laneCount > UINT32_MAX / 2U || qpIdx >= qpCount) {
        return qpCount;
    }
    if (qpCount == TILEXR_UDMA_GROUPED_SHARED_QP_COUNT &&
        qpIdx >= TILEXR_UDMA_GROUPED_DIRECT_QP_BASE) {
        const uint32_t localDirect = UDMAGroupedDirectQpIndex(
            rank, peer, rankSize, qpCount, localRankSize);
        if (qpIdx != localDirect) {
            return qpCount;
        }
        return UDMAGroupedDirectQpIndex(
            peer, rank, rankSize, qpCount, localRankSize);
    }
    const uint32_t aggregateQpCount = qpCount == TILEXR_UDMA_GROUPED_SHARED_QP_COUNT ?
        TILEXR_UDMA_GROUPED_AGGREGATE_QP_COUNT : qpCount;
    if (aggregateQpCount != laneCount * 2U ||
        qpIdx >= aggregateQpCount) {
        return qpCount;
    }
    const uint32_t localLane = UDMAGroupedSharedQpLane(
        rank, peer, rankSize, laneCount);
    const uint32_t remoteLane = UDMAGroupedSharedQpLane(
        peer, rank, rankSize, laneCount);
    if (localLane >= laneCount || remoteLane >= laneCount) {
        return qpCount;
    }
    const uint32_t routeBase = qpIdx / laneCount * laneCount;
    const uint32_t lane = qpIdx % laneCount;
    if (lane == localLane) {
        return routeBase + remoteLane;
    }
    if (lane == remoteLane) {
        return routeBase + localLane;
    }
    return qpIdx;
}

uint32_t UDMARemoteQpIndex(bool groupedQpProfile,
    int rank, int peer, int rankSize, uint32_t qpIdx,
    uint32_t qpCount, uint32_t laneCount, uint32_t localRankSize)
{
    if (qpIdx >= qpCount) {
        return qpCount;
    }
    return groupedQpProfile ? UDMAGroupedRemoteQpIndex(
        rank, peer, rankSize, qpIdx, qpCount, laneCount, localRankSize) : qpIdx;
}

uint32_t UDMAGroupedDirectQpIndex(
    int rank, int peer, int rankSize, uint32_t qpCount, uint32_t localRankSize)
{
    if (qpCount != TILEXR_UDMA_GROUPED_SHARED_QP_COUNT ||
        rankSize <= 1 || rank < 0 || peer < 0 || rank >= rankSize ||
        peer >= rankSize || rank == peer || localRankSize < 2U ||
        localRankSize - 1U != TILEXR_UDMA_GROUPED_DIRECT_QP_COUNT ||
        rank / static_cast<int>(localRankSize) !=
            peer / static_cast<int>(localRankSize)) {
        return qpCount;
    }
    const uint32_t localRank = static_cast<uint32_t>(rank) % localRankSize;
    const uint32_t peerLocalRank = static_cast<uint32_t>(peer) % localRankSize;
    const uint32_t slot = peerLocalRank < localRank ?
        peerLocalRank : peerLocalRank - 1U;
    return TILEXR_UDMA_GROUPED_DIRECT_QP_BASE + slot;
}

int UDMAGroupedLocalPeerFromSlot(
    int rank, int rankSize, uint32_t slot, uint32_t localRankSize)
{
    if (rankSize <= 1 || rank < 0 || rank >= rankSize || localRankSize < 2U ||
        rankSize % static_cast<int>(localRankSize) != 0 ||
        slot >= localRankSize - 1U) {
        return -1;
    }
    const uint32_t localRank = static_cast<uint32_t>(rank) % localRankSize;
    const uint32_t peerLocalRank = slot < localRank ? slot : slot + 1U;
    return rank / static_cast<int>(localRankSize) *
        static_cast<int>(localRankSize) + static_cast<int>(peerLocalRank);
}

int BuildUDMAInfoImage(
    uintptr_t deviceBase,
    uint32_t qpNum,
    const std::vector<UDMAWQCtx>& sq,
    const std::vector<UDMAWQCtx>& rq,
    const std::vector<UDMACQCtx>& scq,
    const std::vector<UDMACQCtx>& rcq,
    const std::vector<UDMAMemInfo>& mem,
    UDMAInfo& info,
    std::vector<uint8_t>& bytes)
{
    const size_t entryCount = sq.size();
    if (qpNum == 0 || entryCount == 0 || entryCount % qpNum != 0 ||
        rq.size() != entryCount || scq.size() != entryCount ||
        rcq.size() != entryCount || mem.size() != entryCount) {
        return TILEXR_UDMA_LAYOUT_INVALID;
    }

    const size_t sqOffset = sizeof(UDMAInfo);
    size_t rqOffset = 0;
    size_t scqOffset = 0;
    size_t rcqOffset = 0;
    size_t memOffset = 0;
    size_t totalBytes = 0;
    if (!CheckedArrayEnd(sqOffset, entryCount, sizeof(UDMAWQCtx), rqOffset) ||
        !CheckedArrayEnd(rqOffset, entryCount, sizeof(UDMAWQCtx), scqOffset) ||
        !CheckedArrayEnd(scqOffset, entryCount, sizeof(UDMACQCtx), rcqOffset) ||
        !CheckedArrayEnd(rcqOffset, entryCount, sizeof(UDMACQCtx), memOffset) ||
        !CheckedArrayEnd(memOffset, entryCount, sizeof(UDMAMemInfo), totalBytes) ||
        totalBytes > bytes.max_size()) {
        return TILEXR_UDMA_LAYOUT_INVALID;
    }

    UDMAInfo nextInfo {};
    uint64_t imageEnd = 0;
    nextInfo.qpNum = qpNum;
    if (!CheckedDeviceAddress(deviceBase, sqOffset, nextInfo.sqPtr) ||
        !CheckedDeviceAddress(deviceBase, rqOffset, nextInfo.rqPtr) ||
        !CheckedDeviceAddress(deviceBase, scqOffset, nextInfo.scqPtr) ||
        !CheckedDeviceAddress(deviceBase, rcqOffset, nextInfo.rcqPtr) ||
        !CheckedDeviceAddress(deviceBase, memOffset, nextInfo.memPtr) ||
        !CheckedDeviceAddress(deviceBase, totalBytes, imageEnd)) {
        return TILEXR_UDMA_LAYOUT_INVALID;
    }

    std::vector<uint8_t> nextBytes(totalBytes, 0);
    std::memcpy(nextBytes.data(), &nextInfo, sizeof(nextInfo));
    CopyVector(nextBytes, sqOffset, sq);
    CopyVector(nextBytes, rqOffset, rq);
    CopyVector(nextBytes, scqOffset, scq);
    CopyVector(nextBytes, rcqOffset, rcq);
    CopyVector(nextBytes, memOffset, mem);

    info = nextInfo;
    bytes.swap(nextBytes);
    return TILEXR_UDMA_LAYOUT_SUCCESS;
}

} // namespace TileXR
