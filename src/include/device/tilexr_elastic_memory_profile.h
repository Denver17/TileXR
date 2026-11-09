/*
 * 功能：定义 ElasticMemory Device profiling 的 slot、event ID 和条件 marker 调用。
 * 调用关系：分配入口及 compact Dispatch 使用 TileXR::Profiling::Recorder<64> 记录阶段；
 * Host 测试读取同一组 event ID。调用者创建 recorder，并负责 Flush 和 capture buffer。
 */
#ifndef TILEXR_ELASTIC_MEMORY_PROFILE_H
#define TILEXR_ELASTIC_MEMORY_PROFILE_H
#include <tilexr/profiling.h>
#ifndef TILEXR_ELASTIC_PROFILE_DETAIL
#define TILEXR_ELASTIC_PROFILE_DETAIL 0
#endif
#if TILEXR_ELASTIC_PROFILE_DETAIL != 0 && TILEXR_ELASTIC_PROFILE_DETAIL != 1
#error "TILEXR_ELASTIC_PROFILE_DETAIL must be 0 or 1"
#endif
namespace TileXR { namespace Elastic { namespace Profile {
#if defined(__CCE__) && defined(__CCE_IS_AICORE__)
using Recorder = TileXR::Profiling::Recorder<64>;
#endif
// Slots follow execution order. Event ID = slot + 1, including the two total
// markers. Conditional growth skips slots 14/15; only the final count owner uses
// slots 5..22. Output setup includes active-lane count waits, prefix/expert-map
// preparation and local descriptor waits; copy covers WaitAndFormatOutput only.
enum Slot : uint32_t {
    KernelBegin = 0,
    SendAndCountBegin = 1, SendAndCountEnd = 2,
    // Slots 3/4 retired when redundant all-core barriers were removed.
    AllocatorBegin = 5,
    StateLoadBegin = 6, StateLoadEnd = 7,
    SegmentSearchBegin = 8, SegmentSearchEnd = 9,
    // Slots 10/11 retired with the old free-range removal phase.
    TryAllocateBegin = 12, TryAllocateEnd = 13,
    GrowthWaitBegin = 14, GrowthWaitEnd = 15,
    StateWritebackBegin = 16, StateWritebackEnd = 17,
    ResultPublishBegin = 18, ResultPublishEnd = 19,
    AllocatorEnd = 20,
    RankGateBegin = 21, RankGateEnd = 22,
    OutputSetupBegin = 23,
    CountWaitBegin = 24, CountWaitEnd = 25,
    LocalReadyWaitBegin = 26, LocalReadyWaitEnd = 27,
    OutputSetupEnd = 28,
    LocalOutputCopyBegin = 29, LocalOutputCopyEnd = 30,
    KernelEnd = 31
};
#if defined(__CCE__) && defined(__CCE_IS_AICORE__)
template <uint32_t MarkerSlot>
__aicore__ inline void Mark(Recorder* recorder)
{
    static_assert(MarkerSlot < 64, "ElasticMemory profiling slot exceeds capacity");
#if defined(TILEXR_ENABLE_PROFILING) && TILEXR_ENABLE_PROFILING == 1 && TILEXR_ELASTIC_PROFILE_DETAIL == 1
    if (recorder != nullptr) recorder->RecordMarker<MarkerSlot, MarkerSlot + 1>();
#else
    (void)recorder;
#endif
}
#endif
} } }
#endif
