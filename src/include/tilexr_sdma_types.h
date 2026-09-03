/*
 * Copyright (c) 2024-2026 TileXR Project
 * Licensed under the Apache License, Version 2.0
 */

#ifndef TILEXR_SDMA_TYPES_H
#define TILEXR_SDMA_TYPES_H

#include <cstdint>

namespace TileXR {

constexpr uint32_t TILEXR_SDMA_SCRATCH_BYTES = 256U;
constexpr uint64_t TILEXR_SDMA_DEFAULT_BLOCK_BYTES = 1024ULL * 1024ULL;
constexpr uint64_t TILEXR_SDMA_DEFAULT_COMM_BLOCK_OFFSET = 0ULL;
constexpr uint32_t TILEXR_SDMA_DEFAULT_QUEUE_NUM = 1U;
constexpr uint32_t TILEXR_SDMA_AUTO_CHANNEL_GROUP = 0xFFFFFFFFU;
constexpr int32_t TILEXR_SDMA_DEMO_MAGIC = 0x53444D41; // "SDMA"

enum class SDMAInitStatus : int32_t {
    DISABLED_BY_ENV = 0,
    INITIALIZED = 1,
    PTO_UNAVAILABLE = 2,
    INIT_FAILED = 3,
    NULL_WORKSPACE = 4,
};

struct SDMASubmitTrace {
    uint64_t prepareBegin;
    uint64_t prepareEnd;
    uint64_t cacheCleanBegin;
    uint64_t cacheCleanEnd;
    uint64_t dsbBegin;
    uint64_t dsbEnd;
    uint64_t doorbellBegin;
    uint64_t doorbellEnd;
    uint32_t head;
    uint32_t tail;
    uint32_t newTail;
    uint32_t depth;
    uint32_t generation;
};

} // namespace TileXR

#endif // TILEXR_SDMA_TYPES_H
