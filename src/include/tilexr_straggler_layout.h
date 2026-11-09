/*
 * 功能：定义 clock mailbox、采样、观测记录和 RDMA profiling 的共享布局及 byte offset。
 * 调用关系：Host session 按这些布局分配/读取内存，clock kernel 和 producer instrumentation 写入对应区域。
 * 资源与生命周期：这里只定义布局；实际内存由 session 或 communicator 持有。
 * Purpose: Define shared clock mailboxes, samples, observation/profile records and byte offsets.
 * Calls: Host sessions allocate/read these layouts; clock kernels and producer instrumentation write them.
 * Lifetime: This header defines layouts only; sessions or communicators own the backing memory.
 */
#ifndef TILEXR_STRAGGLER_LAYOUT_H
#define TILEXR_STRAGGLER_LAYOUT_H

#include <cstddef>
#include <cstdint>

#include "comm_args.h"

namespace TileXR {

constexpr uint32_t TILEXR_STRAGGLER_ABI_VERSION = 2u;
constexpr uint32_t TILEXR_CLOCK_SYNC_MIN_SAMPLES = 4u;
constexpr uint32_t TILEXR_CLOCK_SYNC_MAX_SAMPLES = 256u;
constexpr uint32_t TILEXR_CLOCK_SYNC_MAX_WARMUP_SAMPLES = 256u;
constexpr uint32_t TILEXR_CLOCK_SYNC_MIN_VALID_PERCENT = 75u;
constexpr uint32_t TILEXR_CLOCK_SYNC_SAMPLE_SUCCESS = 0u;
constexpr uint32_t TILEXR_CLOCK_SYNC_SAMPLE_TERMINAL_FAILURE = 1u;
constexpr uint32_t TILEXR_CLOCK_SYNC_SAMPLE_INVALID_TIMING = 2u;
constexpr uint64_t TILEXR_CLOCK_SYNC_REGION_BYTES = 64u * 1024u;
constexpr uint64_t TILEXR_CLOCK_SYNC_REGION_OFFSET =
    static_cast<uint64_t>(IPC_DATA_OFFSET) - TILEXR_CLOCK_SYNC_REGION_BYTES;
constexpr uint64_t TILEXR_CLOCK_SYNC_SLOT_BYTES = 64u;
// Private clock RDMA allocation: mode word, views, synchronous staging, optional
// profiling and distinct deferred sources precede the existing protocol slots.
// Register only this rank's allocation.
constexpr uint64_t TILEXR_CLOCK_RDMA_BYTES = static_cast<uint64_t>(IPC_DATA_OFFSET);
constexpr uint64_t TILEXR_CLOCK_RDMA_VIEWS_OFFSET = 64u;
constexpr uint64_t TILEXR_CLOCK_RDMA_STAGING_OFFSET = 32u * 1024u;
constexpr uint64_t TILEXR_CLOCK_RDMA_PROFILE_OFFSET = 64u * 1024u;
constexpr uint64_t TILEXR_CLOCK_RDMA_ASYNC_STAGING_OFFSET = 128u * 1024u;
constexpr uint64_t TILEXR_CLOCK_RDMA_PROFILE_MAGIC = 0x434C4F434B505246ULL;
constexpr uint32_t TILEXR_CLOCK_RDMA_PROFILE_STAGES = 14u;
// Optional compile-time kernel instrumentation. These counters are written once
// after the protocol; no timestamp records are published between messages.
struct TileXRClockRdmaSendProfile {
    uint64_t sends, polls, pending, failures;
    uint64_t sum[TILEXR_CLOCK_RDMA_PROFILE_STAGES];
    uint64_t minimum[TILEXR_CLOCK_RDMA_PROFILE_STAGES];
    uint64_t maximum[TILEXR_CLOCK_RDMA_PROFILE_STAGES];
    uint64_t padding[2];
};
struct TileXRClockRdmaPollProfile {
    uint64_t calls, matches, invalidateCycles, readCycles;
    // Successful REQUEST/RESPONSE snapshot windows, including invalidate and
    // validation. Aggregated separately from unsuccessful polling iterations.
    uint64_t matchWindowCycles, minMatchWindowCycles, maxMatchWindowCycles, windowMatches;
};
struct TileXRClockRdmaProfile {
    uint64_t magic, generation, bytes, reserved[5];
    TileXRClockRdmaSendProfile send[5];
    TileXRClockRdmaPollProfile receive[5];
};
static_assert(sizeof(TileXRClockRdmaSendProfile) == 384u, "clock send profile layout");
static_assert(sizeof(TileXRClockRdmaProfile) == 2304u, "clock profile layout");
constexpr uint64_t TILEXR_CLOCK_SYNC_READY = 1u;
constexpr uint64_t TILEXR_CLOCK_SYNC_REQUEST = 2u;
constexpr uint64_t TILEXR_CLOCK_SYNC_RESPONSE = 3u;
constexpr uint64_t TILEXR_CLOCK_SYNC_TIMESTAMP = 4u;
constexpr uint64_t TILEXR_CLOCK_SYNC_ACK = 5u;
constexpr uint32_t TILEXR_TIMING_RECORD_STATUS_STARTED = 1u;
constexpr uint32_t TILEXR_TIMING_RECORD_STATUS_COMPLETE = 2u;
constexpr uint32_t TILEXR_TIMING_RECORD_STATUS_ERROR = 3u;

struct TileXRClockSyncProtocolSlot {
    uint64_t magic;
    uint64_t sequence;
    uint64_t state;
    uint64_t t0;
    uint64_t t1;
    uint64_t t2;
    uint64_t reserved0;
    uint64_t reserved1;
};

struct TileXRClockSyncDeviceSample {
    uint64_t t0;
    uint64_t t1;
    uint64_t t2;
    uint64_t t3;
    uint64_t roundTripCycles;
    int64_t offsetCycles;
    uint64_t reserved;
    uint32_t sequence;
    uint32_t status;
};

struct TileXRStragglerTimingRecord {
    uint64_t launchId;
    uint64_t operationId;
    uint64_t messageBytes;
    uint64_t startCycle;
    uint64_t readyCycle;
    uint64_t endCycle;
    uint32_t generation;
    uint32_t componentKind;
    uint32_t operationKind;
    uint32_t stageId;
    uint32_t rank;
    uint32_t status;
    uint32_t blockCount;
    uint32_t reserved;
};

struct TileXRStragglerDeviceControl {
    uint64_t launchId;
    uint64_t operationId;
    uint64_t messageBytes;
    uint64_t delayCycles;
    uint32_t generation;
    uint32_t componentKind;
    uint32_t operationKind;
    uint32_t stageId;
    uint32_t rank;
    uint32_t reserved0;
    uint64_t reserved1;
};

struct TileXRStragglerDeviceSlot {
    TileXRStragglerDeviceControl control;
    TileXRStragglerTimingRecord record;
    uint8_t padding[48];
};

#if TILEXR_ASCENDC_AICORE_COMPILE
#define TILEXR_STRAGGLER_INLINE FORCE_INLINE_AICORE
#else
#define TILEXR_STRAGGLER_INLINE inline
#endif

TILEXR_STRAGGLER_INLINE uint64_t ClockSyncProtocolSlotOffset(uint32_t rank)
{
    return TILEXR_CLOCK_SYNC_REGION_OFFSET +
        static_cast<uint64_t>(rank) * TILEXR_CLOCK_SYNC_SLOT_BYTES;
}

TILEXR_STRAGGLER_INLINE uint64_t ClockSyncAckSlotOffset(uint32_t rank)
{
    return TILEXR_CLOCK_SYNC_REGION_OFFSET +
        static_cast<uint64_t>(TILEXR_MAX_RANK_SIZE + rank) * TILEXR_CLOCK_SYNC_SLOT_BYTES;
}

TILEXR_STRAGGLER_INLINE uint64_t ClockSyncTimestampSlotOffset(uint32_t rank)
{
    return TILEXR_CLOCK_SYNC_REGION_OFFSET +
        static_cast<uint64_t>(TILEXR_MAX_RANK_SIZE * 2u + rank) * TILEXR_CLOCK_SYNC_SLOT_BYTES;
}

TILEXR_STRAGGLER_INLINE uint64_t StragglerTimingRecordOffset(uint32_t slot)
{
    return static_cast<uint64_t>(slot) * sizeof(TileXRStragglerDeviceSlot) +
        offsetof(TileXRStragglerDeviceSlot, record);
}

TILEXR_STRAGGLER_INLINE uint64_t StragglerDeviceControlOffset(uint32_t slot)
{
    return static_cast<uint64_t>(slot) * sizeof(TileXRStragglerDeviceSlot) +
        offsetof(TileXRStragglerDeviceSlot, control);
}

#undef TILEXR_STRAGGLER_INLINE

static_assert(sizeof(TileXRClockSyncProtocolSlot) == TILEXR_CLOCK_SYNC_SLOT_BYTES,
              "clock-sync protocol slot ABI changed");
static_assert(sizeof(TileXRClockSyncDeviceSample) == 64u,
              "clock-sync sample ABI changed");
static_assert(sizeof(TileXRStragglerTimingRecord) == 80u,
              "straggler timing record ABI changed");
static_assert(sizeof(TileXRStragglerDeviceControl) == 64u,
              "straggler device control ABI changed");
static_assert(offsetof(TileXRStragglerDeviceSlot, record) == 64u,
              "straggler device record offset changed");
static_assert(sizeof(TileXRStragglerDeviceSlot) == 192u,
              "straggler device slot ABI changed");
static_assert(TILEXR_CLOCK_SYNC_REGION_OFFSET >= 1024u * 1024u,
              "clock-sync region must remain outside low collective flag slots");
static_assert(TILEXR_CLOCK_SYNC_REGION_OFFSET +
                  TILEXR_CLOCK_SYNC_SLOT_BYTES * TILEXR_MAX_RANK_SIZE * 3u <=
              static_cast<uint64_t>(IPC_DATA_OFFSET),
              "clock-sync protocol slots exceed the reserved flag region");

} // namespace TileXR

#endif // TILEXR_STRAGGLER_LAYOUT_H
