/*
 * Copyright (c) 2024-2026 TileXR Project
 * Licensed under the Apache License, Version 2.0
 */

#ifndef TILEXR_SDMA_A5_TYPES_H
#define TILEXR_SDMA_A5_TYPES_H

#include <cstddef>
#include <cstdint>

namespace TileXR {
namespace detail {

#if defined(__CCE__) && defined(__CCE_IS_AICORE__)
#define TILEXR_SDMA_A5_HOST_DEVICE_INLINE __aicore__ inline
#else
#define TILEXR_SDMA_A5_HOST_DEVICE_INLINE inline
#endif

constexpr uint32_t TILEXR_SDMA_A5_WORKSPACE_MAGIC = 0x41355344U; // "A5SD"
constexpr uint16_t TILEXR_SDMA_A5_ABI_VERSION = 1U;
constexpr uint16_t TILEXR_SDMA_A5_BACKEND_KIND = 2U;
constexpr uint32_t TILEXR_SDMA_A5_CHANNEL_COUNT = 48U;
constexpr uint32_t TILEXR_SDMA_A5_SQE_BYTES = 64U;
constexpr uint32_t TILEXR_SDMA_A5_COMPLETION_BYTES = 64U;
constexpr uint32_t TILEXR_SDMA_A5_SQ_PAGE_BYTES = 4096U;
constexpr uint32_t TILEXR_SDMA_A5_PREWARM_BYTES = 64U;
constexpr uint64_t TILEXR_SDMA_A5_MAX_TRANSFER_BYTES = 0xFFFFFFFFULL;
constexpr uint32_t TILEXR_SDMA_A5_SQE_TYPE = 11U;
constexpr uint32_t TILEXR_SDMA_A5_KERNEL_CREDIT = 254U;
constexpr uint32_t TILEXR_SDMA_A5_QOS = 6U;
constexpr uint64_t TILEXR_SDMA_A5_WAIT_TIMEOUT_CYCLES = 10000000000ULL;

constexpr uint64_t TILEXR_SDMA_A5_EVENT_MAGIC = 0xA5D5ULL;
constexpr uint32_t TILEXR_SDMA_A5_EVENT_MAGIC_SHIFT = 48U;
constexpr uint32_t TILEXR_SDMA_A5_EVENT_CHANNEL_SHIFT = 40U;
constexpr uint64_t TILEXR_SDMA_A5_EVENT_RESERVED_MASK = 0x000000FF00000000ULL;
constexpr uint64_t TILEXR_SDMA_A5_EVENT_GENERATION_MASK = 0xFFFFFFFFULL;

struct alignas(64) A5SdmaWorkspaceHeader {
    uint32_t magic;
    uint16_t abiVersion;
    uint16_t backendKind;
    uint32_t channelCount;
    uint32_t sqeSize;
    uint32_t channelStride;
    uint32_t workspaceSize;
    uint32_t maxTransferBytes;
    uint32_t reserved[9];
};

struct alignas(64) A5SdmaChannel {
    uint64_t sqBase;
    uint64_t rtsqAddress;
    uint64_t completionPayloadAddress;
    uint64_t completionRecordAddress;
    uint32_t depth;
    uint32_t head;
    uint32_t tail;
    uint32_t taskId;
    uint32_t rtsqLength;
    uint32_t streamId;
    uint32_t sqId;
    uint32_t cqId;
    uint32_t logicalCqId;
    uint32_t physicalDieId;
    uint32_t generation;
    uint32_t reserved0[13];
    uint32_t outstanding;
    uint32_t reserved1[15];
};

struct alignas(64) A5SdmaCompletionLine {
    uint32_t generation;
    uint32_t reserved[15];
};

struct A5SdmaSqe {
    uint8_t typeLockUnlock;
    uint8_t controlFlags;
    uint16_t numBlocks;
    uint16_t rtStreamId;
    uint16_t taskId;
    uint32_t reserved0;
    uint16_t reserved1;
    uint8_t kernelCredit;
    uint8_t reserved2;
    uint32_t transferFlags;
    uint16_t sqeId;
    uint8_t mpamPartId;
    uint8_t mpamFlags;
    uint16_t srcStreamId;
    uint16_t srcSubStreamId;
    uint16_t dstStreamId;
    uint16_t dstSubStreamId;
    uint32_t srcAddressLow;
    uint32_t srcAddressHigh;
    uint32_t dstAddressLow;
    uint32_t dstAddressHigh;
    uint32_t length;
    uint32_t srcOffsetLow;
    uint32_t dstOffsetLow;
    uint16_t srcOffsetHigh;
    uint16_t dstOffsetHigh;
};

struct alignas(64) A5SdmaWorkspace {
    A5SdmaWorkspaceHeader header;
    A5SdmaChannel channels[TILEXR_SDMA_A5_CHANNEL_COUNT];
    A5SdmaCompletionLine completionPayloads[TILEXR_SDMA_A5_CHANNEL_COUNT];
    A5SdmaCompletionLine completionRecords[TILEXR_SDMA_A5_CHANNEL_COUNT];
};

TILEXR_SDMA_A5_HOST_DEVICE_INLINE bool A5SdmaChannelValid(uint32_t channel)
{
    return channel < TILEXR_SDMA_A5_CHANNEL_COUNT;
}

TILEXR_SDMA_A5_HOST_DEVICE_INLINE bool A5SdmaTransferLengthValid(uint64_t bytes)
{
    return bytes != 0U && bytes <= TILEXR_SDMA_A5_MAX_TRANSFER_BYTES;
}

TILEXR_SDMA_A5_HOST_DEVICE_INLINE bool A5SdmaQueueStateValid(uint32_t tail, uint32_t depth)
{
    return depth >= 3U && tail < depth;
}

TILEXR_SDMA_A5_HOST_DEVICE_INLINE uint64_t A5SdmaSqBytes(uint32_t depth)
{
    return static_cast<uint64_t>(depth) * TILEXR_SDMA_A5_SQE_BYTES;
}

TILEXR_SDMA_A5_HOST_DEVICE_INLINE uint32_t A5SdmaSqPageCount(uint32_t depth)
{
    const uint64_t bytes = A5SdmaSqBytes(depth);
    return static_cast<uint32_t>(
        (bytes + TILEXR_SDMA_A5_SQ_PAGE_BYTES - 1U) /
        TILEXR_SDMA_A5_SQ_PAGE_BYTES);
}

TILEXR_SDMA_A5_HOST_DEVICE_INLINE uint64_t A5SdmaSqPageOffset(uint32_t page)
{
    return static_cast<uint64_t>(page) * TILEXR_SDMA_A5_SQ_PAGE_BYTES;
}

TILEXR_SDMA_A5_HOST_DEVICE_INLINE uint32_t A5SdmaAdvanceTail(uint32_t tail, uint32_t depth)
{
    return (tail + 2U) % depth;
}

TILEXR_SDMA_A5_HOST_DEVICE_INLINE uint32_t A5SdmaQueueDistance(
    uint32_t head, uint32_t tail, uint32_t depth)
{
    return (tail + depth - head) % depth;
}

TILEXR_SDMA_A5_HOST_DEVICE_INLINE bool A5SdmaQueueHasCapacity(
    uint32_t head, uint32_t tail, uint32_t depth)
{
    if (depth < 3U || head >= depth || tail >= depth) {
        return false;
    }
    return A5SdmaQueueDistance(head, tail, depth) <= depth - 3U;
}

TILEXR_SDMA_A5_HOST_DEVICE_INLINE uint32_t A5SdmaAdvanceTaskId(uint32_t taskId)
{
    return (taskId + 2U) & 0xFFFFU;
}

TILEXR_SDMA_A5_HOST_DEVICE_INLINE uint32_t A5SdmaNextGeneration(uint32_t generation)
{
    return generation == 0xFFFFFFFFU ? 1U : generation + 1U;
}

TILEXR_SDMA_A5_HOST_DEVICE_INLINE uint64_t A5SdmaEncodeEvent(
    uint32_t channel, uint32_t generation)
{
    return !A5SdmaChannelValid(channel) || generation == 0U
        ? 0ULL
        : (TILEXR_SDMA_A5_EVENT_MAGIC << TILEXR_SDMA_A5_EVENT_MAGIC_SHIFT) |
            (static_cast<uint64_t>(channel) << TILEXR_SDMA_A5_EVENT_CHANNEL_SHIFT) |
            static_cast<uint64_t>(generation);
}

TILEXR_SDMA_A5_HOST_DEVICE_INLINE bool A5SdmaDecodeEvent(
    uint64_t event, uint32_t& channel, uint32_t& generation)
{
    if ((event >> TILEXR_SDMA_A5_EVENT_MAGIC_SHIFT) != TILEXR_SDMA_A5_EVENT_MAGIC ||
        (event & TILEXR_SDMA_A5_EVENT_RESERVED_MASK) != 0U) {
        return false;
    }
    channel = static_cast<uint32_t>((event >> TILEXR_SDMA_A5_EVENT_CHANNEL_SHIFT) & 0xFFULL);
    generation = static_cast<uint32_t>(event & TILEXR_SDMA_A5_EVENT_GENERATION_MASK);
    return A5SdmaChannelValid(channel) && generation != 0U;
}

static_assert(sizeof(A5SdmaWorkspaceHeader) == 64U, "A5 workspace header must be 64 bytes");
static_assert(sizeof(A5SdmaChannel) == 192U, "A5 channel ABI must be 192 bytes");
static_assert(sizeof(A5SdmaCompletionLine) == 64U, "A5 completion line must be 64 bytes");
static_assert(sizeof(A5SdmaSqe) == TILEXR_SDMA_A5_SQE_BYTES, "A5 SQE must be 64 bytes");
static_assert(TILEXR_SDMA_A5_SQ_PAGE_BYTES % TILEXR_SDMA_A5_SQE_BYTES == 0U,
    "A5 SQ pages must contain whole SQEs");
static_assert(TILEXR_SDMA_A5_PREWARM_BYTES == TILEXR_SDMA_A5_SQE_BYTES,
    "A5 SQ prewarm must preserve one complete SQE");
static_assert(alignof(A5SdmaWorkspace) == 64U, "A5 workspace must be cache-line aligned");
static_assert(sizeof(A5SdmaWorkspace) % 64U == 0U, "A5 workspace size must be cache-line aligned");
static_assert(offsetof(A5SdmaChannel, sqBase) == 0U, "unexpected A5 SQ base offset");
static_assert(offsetof(A5SdmaChannel, completionPayloadAddress) == 16U,
              "unexpected A5 completion payload offset");
static_assert(offsetof(A5SdmaChannel, generation) == 72U, "unexpected A5 generation offset");
static_assert(offsetof(A5SdmaChannel, outstanding) == 128U, "unexpected A5 outstanding offset");
static_assert(offsetof(A5SdmaSqe, srcAddressLow) == 32U, "unexpected A5 source address offset");
static_assert(offsetof(A5SdmaSqe, length) == 48U, "unexpected A5 length offset");
static_assert(offsetof(A5SdmaWorkspace, channels) == 64U, "unexpected A5 channel array offset");

#undef TILEXR_SDMA_A5_HOST_DEVICE_INLINE

} // namespace detail
} // namespace TileXR

#endif // TILEXR_SDMA_A5_TYPES_H
