/*
 * Copyright (c) 2024-2026 TileXR Project
 * Licensed under the Apache License, Version 2.0
 */

#ifndef TILEXR_SDMA_A5_BACKEND_H
#define TILEXR_SDMA_A5_BACKEND_H

#include <cstddef>
#include <cstdint>
#include <memory>

#include "comm_args.h"
#include "tilexr_sdma_a5_types.h"

namespace TileXR {

namespace detail {

struct alignas(64) A5BuiltinStreamInfo {
    uint64_t stream;
    uint64_t context;
    int32_t streamId;
    uint32_t sqId;
    uint32_t cqId;
    uint32_t logicalCqId;
    uint64_t cqeAddress;
    int32_t deviceId;
    uint8_t reserved[20];
};

struct alignas(64) A5BuiltinOpResource {
    uint64_t size;
    uint64_t streamsAddress;
    uint64_t workspaceAddress;
    uint8_t reserved[40];
};

struct alignas(64) A5BuiltinChannelInfo {
    uint32_t sqHead;
    uint32_t sqTail;
    uint64_t sqBase;
    uint64_t sqRegisterBase;
    uint32_t sqDepth;
    uint32_t sqId;
    uint32_t cqId;
    uint32_t logicalCqId;
    uint64_t cqeAddress;
    uint32_t reportCqeCount;
    uint32_t streamId;
    uint32_t deviceId;
    uint8_t reserved[4];
};

struct alignas(64) A5BuiltinWorkspaceHeader {
    uint32_t flag;
    uint32_t totalQueueCount;
    uint8_t reserved[56];
};

struct A5HostChannelIdentity {
    uint32_t streamId;
    uint32_t sqId;
    uint32_t cqId;
    uint32_t logicalCqId;
    uint32_t physicalDieId;
};

constexpr int32_t TILEXR_SDMA_A5_EXPECTED_QUERY_STATUS = 507018;

enum class A5QueryResultKind : uint32_t {
    INVALID = 0U,
    COMPLETE = 1U,
    EXPECTED_PARTIAL = 2U,
};

inline bool ValidateA5BuiltinChannel(const A5BuiltinChannelInfo& channel,
                                     const A5HostChannelIdentity& expected,
                                     bool requireRegisterBase)
{
    return channel.sqBase != 0U &&
        (!requireRegisterBase || channel.sqRegisterBase != 0U) &&
        A5SdmaQueueHasCapacity(channel.sqHead, channel.sqTail, channel.sqDepth) &&
        channel.streamId == expected.streamId && channel.sqId == expected.sqId &&
        channel.cqId == expected.cqId && channel.logicalCqId == expected.logicalCqId &&
        channel.deviceId == expected.physicalDieId;
}

inline A5QueryResultKind ClassifyA5QueryResult(int32_t syncStatus,
                                               uint32_t flag,
                                               uint32_t totalQueueCount,
                                               const A5BuiltinChannelInfo& firstChannel,
                                               const A5HostChannelIdentity& expected)
{
    if (syncStatus == 0 && flag == 1U &&
        totalQueueCount == TILEXR_SDMA_A5_CHANNEL_COUNT &&
        ValidateA5BuiltinChannel(firstChannel, expected, true)) {
        return A5QueryResultKind::COMPLETE;
    }
    if (syncStatus == TILEXR_SDMA_A5_EXPECTED_QUERY_STATUS && flag == 0U &&
        totalQueueCount == 0U && firstChannel.sqRegisterBase == 0U &&
        ValidateA5BuiltinChannel(firstChannel, expected, false)) {
        return A5QueryResultKind::EXPECTED_PARTIAL;
    }
    return A5QueryResultKind::INVALID;
}

static_assert(sizeof(A5BuiltinStreamInfo) == 64U, "unexpected built-in stream ABI");
static_assert(sizeof(A5BuiltinOpResource) == 64U, "unexpected built-in resource ABI");
static_assert(sizeof(A5BuiltinChannelInfo) == 64U, "unexpected built-in channel ABI");
static_assert(sizeof(A5BuiltinWorkspaceHeader) == 64U, "unexpected built-in header ABI");
static_assert(offsetof(A5BuiltinChannelInfo, sqBase) == 8U, "unexpected built-in SQ base offset");
static_assert(offsetof(A5BuiltinChannelInfo, sqRegisterBase) == 16U,
              "unexpected built-in register offset");
static_assert(offsetof(A5BuiltinChannelInfo, streamId) == 52U,
              "unexpected built-in stream ID offset");

} // namespace detail

class TileXRA5SDMABackend {
public:
    TileXRA5SDMABackend();
    ~TileXRA5SDMABackend();
    TileXRA5SDMABackend(const TileXRA5SDMABackend&) = delete;
    TileXRA5SDMABackend& operator=(const TileXRA5SDMABackend&) = delete;

    bool Init(int32_t deviceId);
    bool Shutdown();
    GM_ADDR GetWorkspaceDev() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace TileXR

#endif // TILEXR_SDMA_A5_BACKEND_H
