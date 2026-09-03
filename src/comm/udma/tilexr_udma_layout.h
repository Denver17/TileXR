/*
 * Copyright (c) 2024-2026 TileXR Project
 * Licensed under the Apache License, Version 2.0
 */

#ifndef TILEXR_UDMA_LAYOUT_H
#define TILEXR_UDMA_LAYOUT_H

#include <cstddef>
#include <cstdint>
#include <vector>

#include "tilexr_udma_types.h"

namespace TileXR {

constexpr int TILEXR_UDMA_LAYOUT_SUCCESS = 0;
constexpr int TILEXR_UDMA_LAYOUT_INVALID = -3;

uint32_t UDMAGroupedSharedQpLane(
    int rank, int peer, int rankSize, uint32_t laneCount);

uint32_t UDMAGroupedRemoteQpIndex(
    int rank, int peer, int rankSize, uint32_t qpIdx,
    uint32_t qpCount, uint32_t laneCount, uint32_t localRankSize = 8U);

uint32_t UDMARemoteQpIndex(bool groupedQpProfile,
    int rank, int peer, int rankSize, uint32_t qpIdx,
    uint32_t qpCount, uint32_t laneCount, uint32_t localRankSize = 8U);

uint32_t UDMAGroupedDirectQpIndex(
    int rank, int peer, int rankSize, uint32_t qpCount,
    uint32_t localRankSize = 8U);

int UDMAGroupedLocalPeerFromSlot(
    int rank, int rankSize, uint32_t slot, uint32_t localRankSize = 8U);

int BuildUDMAInfoImage(
    uintptr_t deviceBase,
    uint32_t qpNum,
    const std::vector<UDMAWQCtx>& sq,
    const std::vector<UDMAWQCtx>& rq,
    const std::vector<UDMACQCtx>& scq,
    const std::vector<UDMACQCtx>& rcq,
    const std::vector<UDMAMemInfo>& mem,
    UDMAInfo& info,
    std::vector<uint8_t>& bytes);

} // namespace TileXR

#endif // TILEXR_UDMA_LAYOUT_H
