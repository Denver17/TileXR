/*
 * Copyright (c) 2024-2026 TileXR Project
 * Licensed under the Apache License, Version 2.0
 */

#ifndef TILEXR_UDMA_PEERMEM_PROBE_LAYOUT_H
#define TILEXR_UDMA_PEERMEM_PROBE_LAYOUT_H

#include <cstdint>

#include "comm_args.h"
#include "tilexr_udma_types.h"

namespace TileXRUDMAPeerMemProbe {

constexpr int32_t kRankSize = 3;
constexpr int32_t kProducerRank = 0;
constexpr int32_t kOwnerRank = 1;
constexpr int32_t kConsumerRank = 2;
constexpr uint32_t kPayloadBytes = 4U * 1024U;
constexpr uint32_t kTokenBytes = TileXR::TILEXR_UDMA_CACHE_LINE_SIZE;
constexpr uint32_t kMaxGenerations = 16U;
constexpr uint32_t kDefaultGenerations = 8U;
constexpr uint64_t kMaxPollIterations = UINT64_C(1000000);
constexpr uint64_t kStatusMagic = UINT64_C(0x5458504d414c4941); // TXPMALIA
constexpr uint64_t kTokenPrefix = UINT64_C(0x5458504d00000000); // TXPM

constexpr uint64_t kPayloadOffset = static_cast<uint64_t>(TileXR::IPC_DATA_OFFSET);
constexpr uint64_t kReadyOffset = kPayloadOffset + kPayloadBytes;
constexpr uint64_t kAckOffset = kReadyOffset + kTokenBytes;
constexpr uint64_t kInitAckOffset = kAckOffset + kTokenBytes;
constexpr uint64_t kPreparedOffset = kInitAckOffset + kTokenBytes;
constexpr uint64_t kDoneOffset = kPreparedOffset + kTokenBytes;
constexpr uint64_t kSourcePayloadOffset = kPayloadOffset + 64U * 1024U;
constexpr uint64_t kReadySourceOffset =
    kSourcePayloadOffset + static_cast<uint64_t>(kMaxGenerations) * kPayloadBytes;
constexpr uint64_t kInitSourceOffset =
    kReadySourceOffset + static_cast<uint64_t>(kMaxGenerations) * kTokenBytes;
constexpr uint64_t kRequiredPeerMemBytes = kInitSourceOffset + kTokenBytes;

constexpr uint64_t GenerationToken(uint32_t generation)
{
    return kTokenPrefix | static_cast<uint64_t>(generation + 1U);
}

constexpr uint64_t kInitToken = kTokenPrefix | UINT64_C(0x00fffffe);

struct Status {
    uint64_t magic = 0U;
    uint64_t result = 0U;
    uint64_t completedGenerations = 0U;
    uint64_t role = 0U;
    uint64_t localPeerMem = 0U;
    uint64_t observedOwnerPeerMem = 0U;
    uint64_t registryBase = 0U;
    uint64_t registryOwnerBase = 0U;
    uint64_t failureStep = 0U;
};

static_assert(kPayloadOffset % TileXR::TILEXR_UDMA_CACHE_LINE_SIZE == 0U,
    "payload must be cache-line aligned");
static_assert(kReadyOffset % TileXR::TILEXR_UDMA_CACHE_LINE_SIZE == 0U,
    "ready must be cache-line aligned");
static_assert(kAckOffset % TileXR::TILEXR_UDMA_CACHE_LINE_SIZE == 0U,
    "ack must be cache-line aligned");
static_assert(kSourcePayloadOffset >= kDoneOffset + kTokenBytes,
    "source payload must not overlap remote protocol slots");

} // namespace TileXRUDMAPeerMemProbe

#endif // TILEXR_UDMA_PEERMEM_PROBE_LAYOUT_H
