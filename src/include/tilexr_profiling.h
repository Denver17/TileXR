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
constexpr uint32_t TILEXR_PROFILING_COUNT_STRIDE = 8U;
constexpr uint32_t TILEXR_PROFILING_MAX_CORE_COUNT = 64U;
constexpr uint32_t TILEXR_PROFILING_DEFAULT_RECORDS_PER_CORE = 4096U;
constexpr uint32_t TILEXR_PROFILING_MAX_RECORDS_PER_CORE = 65536U;

constexpr uint32_t TileXRProfilingStreamId(uint32_t rank, uint32_t core)
{
    return (rank << 16U) | core;
}

#if TILEXR_PROFILING_AICORE && defined(TILEXR_ENABLE_PROFILING)

template <int32_t HardwareEventId>
__attribute__((always_inline)) inline __aicore__ void TileXRProfilingPrepareMte3()
{
    AscendC::SetFlag<AscendC::HardEvent::S_MTE3>(HardwareEventId);
    AscendC::WaitFlag<AscendC::HardEvent::S_MTE3>(HardwareEventId);
}

template <int32_t HardwareEventId>
__attribute__((always_inline)) inline __aicore__ void TileXRProfilingWaitMte3()
{
    AscendC::SetFlag<AscendC::HardEvent::MTE3_S>(HardwareEventId);
    AscendC::WaitFlag<AscendC::HardEvent::MTE3_S>(HardwareEventId);
}

template <int32_t HardwareEventId>
__attribute__((always_inline)) inline __aicore__ void TileXRProfilingWriteCounterLine(
    AscendC::GlobalTensor<uint32_t> destination,
    AscendC::LocalTensor<uint32_t> scratch, uint32_t value)
{
    for (uint32_t index = 0U; index < TILEXR_PROFILING_COUNT_STRIDE; ++index) {
        scratch.SetValue(index, 0U);
    }
    scratch.SetValue(0U, value);
    TileXRProfilingPrepareMte3<HardwareEventId>();
    AscendC::DataCopy(destination, scratch, TILEXR_PROFILING_COUNT_STRIDE);
    TileXRProfilingWaitMte3<HardwareEventId>();
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

    template <int32_t HardwareEventId = EVENT_ID0>
    __attribute__((always_inline)) inline __aicore__ void Flush(
        GM_ADDR commArgsGM, AscendC::LocalTensor<uint64_t> scratch)
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
            recordsPerCore % TILEXR_PROFILING_GM_ALIGNMENT_RECORDS != 0U) {
            return;
        }

        AscendC::GlobalTensor<uint32_t> recordCountOut;
        recordCountOut.SetGlobalBuffer(
            reinterpret_cast<__gm__ uint32_t *>(args->tilexrProfilingRecordCountsGM) +
            static_cast<uint64_t>(core) * TILEXR_PROFILING_COUNT_STRIDE,
            TILEXR_PROFILING_COUNT_STRIDE);
        const uint32_t occupiedBefore = recordCountOut.GetValue(0U);
        if (occupiedBefore > recordsPerCore ||
            occupiedBefore % TILEXR_PROFILING_GM_ALIGNMENT_RECORDS != 0U ||
            occupiedSlots > recordsPerCore - occupiedBefore) {
            AscendC::GlobalTensor<uint32_t> droppedCountOut;
            droppedCountOut.SetGlobalBuffer(
                reinterpret_cast<__gm__ uint32_t *>(args->tilexrProfilingDroppedCountsGM) +
                static_cast<uint64_t>(core) * TILEXR_PROFILING_COUNT_STRIDE,
                TILEXR_PROFILING_COUNT_STRIDE);
            const uint32_t oldDropped = droppedCountOut.GetValue(0U);
            const uint32_t maxValue = static_cast<uint32_t>(-1);
            const uint32_t nextDropped = oldDropped > maxValue - RecordCapacity ?
                maxValue : oldDropped + RecordCapacity;
            TileXRProfilingWriteCounterLine<HardwareEventId>(
                droppedCountOut, scratch.ReinterpretCast<uint32_t>(), nextDropped);
            return;
        }

        for (uint32_t slot = 0U; slot < RecordCapacity; ++slot) {
            scratch.SetValue(slot, tilexrProfilingEventStamps_[slot]);
        }
        for (uint32_t slot = RecordCapacity; slot < occupiedSlots; ++slot) {
            scratch.SetValue(slot, 0U);
        }
        AscendC::GlobalTensor<uint64_t> timestampsOut;
        timestampsOut.SetGlobalBuffer(
            reinterpret_cast<__gm__ uint64_t *>(args->tilexrProfilingTimestampsGM) +
            static_cast<uint64_t>(core) * recordsPerCore + occupiedBefore,
            occupiedSlots);
        TileXRProfilingPrepareMte3<HardwareEventId>();
        AscendC::DataCopy(timestampsOut, scratch, occupiedSlots);
        TileXRProfilingWaitMte3<HardwareEventId>();

        AscendC::LocalTensor<uint32_t> eventIdScratch =
            scratch.ReinterpretCast<uint32_t>();
        for (uint32_t slot = 0U; slot < RecordCapacity; ++slot) {
            eventIdScratch.SetValue(slot, tilexrProfilingEventIds_[slot]);
        }
        for (uint32_t slot = RecordCapacity; slot < occupiedSlots; ++slot) {
            eventIdScratch.SetValue(slot, 0U);
        }
        AscendC::GlobalTensor<uint32_t> eventIdsOut;
        eventIdsOut.SetGlobalBuffer(
            reinterpret_cast<__gm__ uint32_t *>(args->tilexrProfilingEventIdsGM) +
            static_cast<uint64_t>(core) * recordsPerCore + occupiedBefore,
            occupiedSlots);
        TileXRProfilingPrepareMte3<HardwareEventId>();
        AscendC::DataCopy(eventIdsOut, eventIdScratch, occupiedSlots);
        TileXRProfilingWaitMte3<HardwareEventId>();

        TileXRProfilingWriteCounterLine<HardwareEventId>(
            recordCountOut, eventIdScratch, occupiedBefore + occupiedSlots);
    }

private:
    uint64_t tilexrProfilingEventStamps_[RecordCapacity];
    uint32_t tilexrProfilingEventIds_[RecordCapacity];
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
    template <int32_t HardwareEventId = EVENT_ID0>
    __attribute__((always_inline)) inline __aicore__ void Flush(
        GM_ADDR, AscendC::LocalTensor<uint64_t>)
    {
    }
#endif
};

#endif

} // namespace TileXR

#undef TILEXR_PROFILING_AICORE

#endif // TILEXR_PROFILING_H
