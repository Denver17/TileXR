/*
 * Copyright (c) 2026 TileXR Project
 * Licensed under the CANN Open Software License Agreement Version 2.0.
 */

#ifndef TILEXR_PROFILING_H
#define TILEXR_PROFILING_H

#include <cstdint>

#include "comm_args.h"

#if defined(__CCE__) && defined(__CCE_IS_AICORE__)
#include "kernel_operator.h"
#define TILEXR_PROFILING_AICORE 1
#else
#define TILEXR_PROFILING_AICORE 0
#endif

namespace TileXR {

constexpr uint32_t TILEXR_PROFILING_LOCAL_RECORD_CAPACITY = 256U;
constexpr uint32_t TILEXR_PROFILING_GM_ALIGNMENT_RECORDS = 8U;
// CANN 9.1 / 910B and A5 scalar DataCache lines are 64 bytes. Different
// logical cores must never write any part of the same cache line.
constexpr uint32_t TILEXR_PROFILING_CACHE_LINE_BYTES = 64U;
constexpr uint32_t TILEXR_PROFILING_CORE_ALIGNMENT_RECORDS =
    TILEXR_PROFILING_CACHE_LINE_BYTES / sizeof(uint32_t);
constexpr uint32_t TILEXR_PROFILING_COUNT_STRIDE =
    TILEXR_PROFILING_CACHE_LINE_BYTES / sizeof(uint32_t);
constexpr uint32_t TILEXR_PROFILING_MAX_CORE_COUNT = 64U;
constexpr uint32_t TILEXR_PROFILING_DEFAULT_RECORDS_PER_CORE = 4096U;
// The whole capture may contain multiple large launches. Bound the worst-case
// 64-core records.v1 export (including headers) below 64 GiB.
constexpr uint32_t TILEXR_PROFILING_MAX_RECORDS_PER_CORE = 0x03fffff0U;

constexpr uint32_t TileXRProfilingStreamId(uint32_t rank, uint32_t core)
{
    return (rank << 16U) | core;
}

#if TILEXR_PROFILING_AICORE && defined(TILEXR_ENABLE_PROFILING)

// Flush only the lines intersecting this writer's range. A reserved launch
// may start halfway through an event-ID cache line (8 records = 32 bytes).
// Construct each tensor at its actual address with an explicit extent; do
// not index a tensor whose default extent is zero.
template <typename T>
__attribute__((always_inline)) inline __aicore__ void TileXRProfilingFlushGm(
    __gm__ T *data, uint32_t count)
{
    __asm__ __volatile__("" ::: "memory");
    __gm__ uint8_t *bytes = reinterpret_cast<__gm__ uint8_t *>(data);
    const uint64_t length = static_cast<uint64_t>(count) * sizeof(T);
    for (uint64_t offset = 0U; offset < length;) {
        __gm__ uint8_t *address = bytes + offset;
        AscendC::GlobalTensor<uint8_t> line;
        line.SetGlobalBuffer(address, 1U);
        AscendC::DataCacheCleanAndInvalid<uint8_t,
            AscendC::CacheLine::SINGLE_CACHE_LINE,
            AscendC::DcciDst::CACHELINE_OUT>(line);
        offset += TILEXR_PROFILING_CACHE_LINE_BYTES -
            reinterpret_cast<uint64_t>(address) % TILEXR_PROFILING_CACHE_LINE_BYTES;
    }
    __asm__ __volatile__("" ::: "memory");
}

template <uint32_t RecordCapacity>
class ProfilingRecorder {
    static_assert(RecordCapacity > 0U,
        "TileXR profiling recorder must contain at least one slot");
    static_assert(RecordCapacity <= TILEXR_PROFILING_LOCAL_RECORD_CAPACITY,
        "TileXR profiling recorder exceeds local capacity");

public:
    template <uint32_t Slot, uint32_t EventId>
    __attribute__((always_inline)) inline __aicore__ void RecordMarker()
    {
        static_assert(Slot < RecordCapacity,
            "TileXR profiling marker slot exceeds recorder capacity");
        static_assert(EventId != 0U,
            "TileXR profiling event ID zero is reserved");
        tilexrProfilingEventStamps_[Slot] =
            static_cast<uint64_t>(AscendC::GetSystemCycle());
        tilexrProfilingEventIds_[Slot] = EventId;
    }

    __attribute__((always_inline)) inline __aicore__ void Flush(GM_ADDR commArgsGM)
    {
        constexpr uint32_t occupiedSlots =
            ((RecordCapacity + TILEXR_PROFILING_GM_ALIGNMENT_RECORDS - 1U) /
            TILEXR_PROFILING_GM_ALIGNMENT_RECORDS) *
            TILEXR_PROFILING_GM_ALIGNMENT_RECORDS;

        if (commArgsGM == nullptr) {
            return;
        }
        __gm__ CommArgs *args = reinterpret_cast<__gm__ CommArgs *>(commArgsGM);
        if ((args->extraFlag & ExtraFlag::PROFILING) == 0U ||
            args->tilexrProfilingTimestampsGM == nullptr ||
            args->tilexrProfilingEventIdsGM == nullptr ||
            args->tilexrProfilingRecordCountsGM == nullptr ||
            args->tilexrProfilingDroppedCountsGM == nullptr) {
            return;
        }

        const uint32_t core = static_cast<uint32_t>(AscendC::GetBlockIdx());
        const uint32_t coreCapacity = args->tilexrProfilingCoreCapacity;
        const uint32_t recordsPerCore = args->tilexrProfilingRecordsPerCore;
        if (core >= coreCapacity || recordsPerCore < occupiedSlots ||
            recordsPerCore % TILEXR_PROFILING_CORE_ALIGNMENT_RECORDS != 0U) {
            return;
        }

        __gm__ uint32_t *recordCountOut =
            reinterpret_cast<__gm__ uint32_t *>(args->tilexrProfilingRecordCountsGM) +
            static_cast<uint64_t>(core) * TILEXR_PROFILING_COUNT_STRIDE;
        const uint32_t occupiedBefore = recordCountOut[0];
        if (occupiedBefore > recordsPerCore ||
            occupiedBefore % TILEXR_PROFILING_GM_ALIGNMENT_RECORDS != 0U ||
            occupiedSlots > recordsPerCore - occupiedBefore) {
            __gm__ uint32_t *droppedCountOut =
                reinterpret_cast<__gm__ uint32_t *>(args->tilexrProfilingDroppedCountsGM) +
                static_cast<uint64_t>(core) * TILEXR_PROFILING_COUNT_STRIDE;
            const uint32_t oldDropped = droppedCountOut[0];
            const uint32_t maxValue = static_cast<uint32_t>(-1);
            const uint32_t nextDropped = oldDropped > maxValue - RecordCapacity ?
                maxValue : oldDropped + RecordCapacity;
            droppedCountOut[0] = nextDropped;
            TileXRProfilingFlushGm(droppedCountOut, 1U);
            return;
        }

        __gm__ uint64_t *timestampsOut =
            reinterpret_cast<__gm__ uint64_t *>(args->tilexrProfilingTimestampsGM) +
            static_cast<uint64_t>(core) * recordsPerCore + occupiedBefore;
        __gm__ uint32_t *eventIdsOut =
            reinterpret_cast<__gm__ uint32_t *>(args->tilexrProfilingEventIdsGM) +
            static_cast<uint64_t>(core) * recordsPerCore + occupiedBefore;
        for (uint32_t slot = 0U; slot < RecordCapacity; ++slot) {
            timestampsOut[slot] = tilexrProfilingEventStamps_[slot];
            eventIdsOut[slot] = tilexrProfilingEventIds_[slot];
        }
        for (uint32_t slot = RecordCapacity; slot < occupiedSlots; ++slot) {
            timestampsOut[slot] = 0U;
            eventIdsOut[slot] = 0U;
        }
        TileXRProfilingFlushGm(timestampsOut, occupiedSlots);
        TileXRProfilingFlushGm(eventIdsOut, occupiedSlots);
        recordCountOut[0] = occupiedBefore + occupiedSlots;
        TileXRProfilingFlushGm(recordCountOut, 1U);
    }

private:
    uint64_t tilexrProfilingEventStamps_[RecordCapacity];
    uint32_t tilexrProfilingEventIds_[RecordCapacity];
};

class DirectProfilingWriter {
public:
    // Pair with TileXRProfilingReserveLatestLaunch. Every launch must use the
    // same logical cores and execute in order, with no other writers in that region.
    // Reset metadata only: the exporter ignores the stale tail beyond count.
    __attribute__((always_inline)) inline __aicore__ void InitializeLatest(
        GM_ADDR commArgsGM, uint32_t logicalCore,
        uint32_t launchSlot, uint32_t recordsPerLaunch)
    {
        Initialize(commArgsGM, logicalCore,
            launchSlot == 0U ? 0U : static_cast<uint32_t>(-1), recordsPerLaunch);
        if (enabled_) {
            recordCount_[0] = 0U;
            droppedCount_[0] = 0U;
        }
    }

    // Select Combine bases directly; never rewrite the shared CommArgs.
    __attribute__((always_inline)) inline __aicore__ void InitializeLatestCombine(
        GM_ADDR commArgsGM, uint32_t logicalCore,
        uint32_t launchSlot, uint32_t recordsPerLaunch)
    {
        InitializeRegion(commArgsGM, logicalCore,
            launchSlot == 0U ? 0U : static_cast<uint32_t>(-1), recordsPerLaunch, true);
        if (enabled_) {
            recordCount_[0] = 0U;
            droppedCount_[0] = 0U;
        }
    }

    __attribute__((always_inline)) inline __aicore__ void Initialize(
        GM_ADDR commArgsGM, uint32_t logicalCore,
        uint32_t launchSlot, uint32_t recordsPerLaunch)
    {
        InitializeRegion(commArgsGM, logicalCore, launchSlot, recordsPerLaunch, false);
    }

    __attribute__((always_inline)) inline __aicore__ void Record(uint32_t eventId)
    {
        if (enabled_ && eventId != 0U) {
            RecordAt(eventId, static_cast<uint64_t>(AscendC::GetSystemCycle()));
        }
    }

    __attribute__((always_inline)) inline __aicore__ void RecordAt(
        uint32_t eventId, uint64_t timestamp)
    {
        if (!enabled_ || eventId == 0U || timestamp == 0U) {
            return;
        }
        if (cursor_ < capacity_) {
            timestamps_[cursor_] = timestamp;
            eventIds_[cursor_] = eventId;
            ++cursor_;
            return;
        }
        if (dropped_ != static_cast<uint32_t>(-1)) {
            ++dropped_;
        }
    }

    __attribute__((always_inline)) inline __aicore__ void Publish()
    {
        if (!enabled_) {
            return;
        }
        const uint32_t aligned =
            (cursor_ + TILEXR_PROFILING_GM_ALIGNMENT_RECORDS - 1U) /
            TILEXR_PROFILING_GM_ALIGNMENT_RECORDS *
            TILEXR_PROFILING_GM_ALIGNMENT_RECORDS;
        if (aligned <= capacity_) {
            for (uint32_t slot = cursor_; slot < aligned; ++slot) {
                timestamps_[slot] = 0U;
                eventIds_[slot] = 0U;
            }
            cursor_ = aligned;
        }
        // Store publication metadata; consumers read after kernel completion.
        // Rely on kernel-completion cache maintenance instead of per-line DCCI.
        // Publish is not a mid-kernel visibility fence.
        if (dropped_ != 0U) {
            // Launches sharing a core stream must execute in order. Preserve
            // overflow from earlier launches, including after repeated Publish.
            const uint32_t previous = droppedCount_[0];
            const uint32_t maximum = static_cast<uint32_t>(-1);
            droppedCount_[0] = previous > maximum - dropped_ ?
                maximum : previous + dropped_;
            dropped_ = 0U;
        }
        if (recordCount_[0] < launchBase_ + cursor_) {
            recordCount_[0] = launchBase_ + cursor_;
        }
    }

private:
    __attribute__((always_inline)) inline __aicore__ void InitializeRegion(
        GM_ADDR commArgsGM, uint32_t logicalCore,
        uint32_t launchSlot, uint32_t recordsPerLaunch, bool combine)
    {
        enabled_ = false;
        cursor_ = 0U;
        dropped_ = 0U;
        if (commArgsGM == nullptr) {
            return;
        }
        __gm__ CommArgs *args = reinterpret_cast<__gm__ CommArgs *>(commArgsGM);
        __gm__ uint8_t * const timestampsGM = combine ? args->tilexrCombineProfilingTimestampsGM :
            args->tilexrProfilingTimestampsGM;
        __gm__ uint8_t * const eventIdsGM = combine ? args->tilexrCombineProfilingEventIdsGM :
            args->tilexrProfilingEventIdsGM;
        __gm__ uint8_t * const recordCountsGM = combine ? args->tilexrCombineProfilingRecordCountsGM :
            args->tilexrProfilingRecordCountsGM;
        __gm__ uint8_t * const droppedCountsGM = combine ? args->tilexrCombineProfilingDroppedCountsGM :
            args->tilexrProfilingDroppedCountsGM;
        const uint32_t coreCapacity = combine ? args->tilexrCombineProfilingCoreCapacity :
            args->tilexrProfilingCoreCapacity;
        const uint32_t recordsPerCore = combine ? args->tilexrCombineProfilingRecordsPerCore :
            args->tilexrProfilingRecordsPerCore;
        if ((args->extraFlag & ExtraFlag::PROFILING) == 0U ||
            timestampsGM == nullptr ||
            eventIdsGM == nullptr ||
            recordCountsGM == nullptr ||
            droppedCountsGM == nullptr ||
            logicalCore >= coreCapacity ||
            recordsPerCore == 0U ||
            recordsPerCore %
                TILEXR_PROFILING_CORE_ALIGNMENT_RECORDS != 0U ||
            recordsPerLaunch == 0U ||
            recordsPerLaunch % TILEXR_PROFILING_GM_ALIGNMENT_RECORDS != 0U) {
            return;
        }

        if (launchSlot >= recordsPerCore / recordsPerLaunch) {
            return;
        }
        const uint32_t launchBase = launchSlot * recordsPerLaunch;
        __gm__ uint32_t *recordCount =
            reinterpret_cast<__gm__ uint32_t *>(recordCountsGM) +
            static_cast<uint64_t>(logicalCore) * TILEXR_PROFILING_COUNT_STRIDE;

        timestamps_ =
            reinterpret_cast<__gm__ uint64_t *>(timestampsGM) +
            static_cast<uint64_t>(logicalCore) * recordsPerCore + launchBase;
        eventIds_ =
            reinterpret_cast<__gm__ uint32_t *>(eventIdsGM) +
            static_cast<uint64_t>(logicalCore) * recordsPerCore + launchBase;
        recordCount_ = recordCount;
        droppedCount_ =
            reinterpret_cast<__gm__ uint32_t *>(droppedCountsGM) +
            static_cast<uint64_t>(logicalCore) * TILEXR_PROFILING_COUNT_STRIDE;
        launchBase_ = launchBase;
        capacity_ = recordsPerLaunch;
        enabled_ = true;
    }

    __gm__ uint64_t *timestamps_ = nullptr;
    __gm__ uint32_t *eventIds_ = nullptr;
    __gm__ uint32_t *recordCount_ = nullptr;
    __gm__ uint32_t *droppedCount_ = nullptr;
    uint32_t launchBase_ = 0U;
    uint32_t capacity_ = 0U;
    uint32_t cursor_ = 0U;
    uint32_t dropped_ = 0U;
    bool enabled_ = false;
};

#else

template <uint32_t RecordCapacity>
class ProfilingRecorder {
    static_assert(RecordCapacity > 0U,
        "TileXR profiling recorder must contain at least one slot");
    static_assert(RecordCapacity <= TILEXR_PROFILING_LOCAL_RECORD_CAPACITY,
        "TileXR profiling recorder exceeds local capacity");

public:
    template <uint32_t Slot, uint32_t EventId>
#if TILEXR_PROFILING_AICORE
    __attribute__((always_inline)) inline __aicore__ void RecordMarker()
#else
    inline void RecordMarker()
#endif
    {
        static_assert(Slot < RecordCapacity,
            "TileXR profiling marker slot exceeds recorder capacity");
        static_assert(EventId != 0U,
            "TileXR profiling event ID zero is reserved");
    }

#if TILEXR_PROFILING_AICORE
    __attribute__((always_inline)) inline __aicore__ void Flush(GM_ADDR)
    {
    }
#endif
};

class DirectProfilingWriter {
public:
#if TILEXR_PROFILING_AICORE
    __attribute__((always_inline)) inline __aicore__ void InitializeLatest(
        GM_ADDR, uint32_t, uint32_t, uint32_t)
    {
    }

    __attribute__((always_inline)) inline __aicore__ void InitializeLatestCombine(
        GM_ADDR, uint32_t, uint32_t, uint32_t)
    {
    }

    __attribute__((always_inline)) inline __aicore__ void Initialize(
        GM_ADDR, uint32_t, uint32_t, uint32_t)
    {
    }

    __attribute__((always_inline)) inline __aicore__ void RecordAt(uint32_t, uint64_t)
    {
    }

    __attribute__((always_inline)) inline __aicore__ void Record(uint32_t)
    {
    }

    __attribute__((always_inline)) inline __aicore__ void Publish()
    {
    }
#endif
};

#endif

} // namespace TileXR

#undef TILEXR_PROFILING_AICORE

#endif // TILEXR_PROFILING_H
