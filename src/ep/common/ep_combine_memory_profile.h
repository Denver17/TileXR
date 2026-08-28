#ifndef TILEXR_EP_COMMON_EP_COMBINE_MEMORY_PROFILE_H
#define TILEXR_EP_COMMON_EP_COMBINE_MEMORY_PROFILE_H

#include <cstdint>

namespace TileXREp {

constexpr uint32_t kEpCombineMemoryProfileMarker = 0x45504350U; // EPCP
constexpr uint16_t kEpCombineMemoryProfileVersion = 1U;
constexpr uint32_t kEpCombineMemoryProfileCyclesPerUs = 1000U;
constexpr uint32_t kEpCombineMemoryProfileTimePointCapacity = 40U;
constexpr uint32_t kEpCombineMemoryProfileMetricCapacity = 16U;

enum EpCombineMemoryProfileTimePointIndex : uint32_t {
    EP_COMBINE_MEMORY_TIME_INIT_BEGIN = 0U,
    EP_COMBINE_MEMORY_TIME_STATIC_INIT_END,
    EP_COMBINE_MEMORY_TIME_REUSE_WAIT_END,
    EP_COMBINE_MEMORY_TIME_SEND_COUNT_SPLIT_END,
    EP_COMBINE_MEMORY_TIME_SEND_BUFFER_INIT_END,
    EP_COMBINE_MEMORY_TIME_ASSIST_LOAD_END,
    EP_COMBINE_MEMORY_TIME_SEND_ISSUE_END,
    EP_COMBINE_MEMORY_TIME_DISPATCH_END,
    EP_COMBINE_MEMORY_TIME_RECEIVE_BUFFER_INIT_END,
    EP_COMBINE_MEMORY_TIME_RECEIVE_PREPARE_END,
    EP_COMBINE_MEMORY_TIME_FIRST_TOKEN_READY,
    EP_COMBINE_MEMORY_TIME_ALL_TOKENS_READY,
    EP_COMBINE_MEMORY_TIME_LOCAL_COPY_END,
    EP_COMBINE_MEMORY_TIME_FINAL_BARRIER_END,
    EP_COMBINE_MEMORY_TIME_RETIRE_BEGIN,
    EP_COMBINE_MEMORY_TIME_RETIRE_END,
    EP_COMBINE_MEMORY_TIME_FINAL_END,
};

constexpr uint32_t kEpCombineMemoryProfileTimePointCount =
    EP_COMBINE_MEMORY_TIME_FINAL_END + 1U;

enum EpCombineMemoryProfileDiagnosticIndex : uint32_t {
    EP_COMBINE_MEMORY_DIAG_SEND_ROWS = kEpCombineMemoryProfileTimePointCount,
    EP_COMBINE_MEMORY_DIAG_RECEIVE_TOKENS,
    EP_COMBINE_MEMORY_DIAG_FLAG_POLL_ATTEMPTS,
    EP_COMBINE_MEMORY_DIAG_FLAG_POLL_MISSES,
    EP_COMBINE_MEMORY_DIAG_BLOCKS_PER_TOKEN,
    EP_COMBINE_MEMORY_DIAG_DISPATCH_BUFFER_NUM,
    EP_COMBINE_MEMORY_DIAG_RECEIVE_BUFFER_NUM,
};

enum EpCombineMemoryProfileMetricIndex : uint32_t {
    EP_COMBINE_MEMORY_METRIC_ASSIST_LOAD = 0U,
    EP_COMBINE_MEMORY_METRIC_DISPATCH_INPUT_LOAD,
    EP_COMBINE_MEMORY_METRIC_DISPATCH_PACK,
    EP_COMBINE_MEMORY_METRIC_DISPATCH_STORE_ISSUE,
    EP_COMBINE_MEMORY_METRIC_SCALE_LOAD,
    EP_COMBINE_MEMORY_METRIC_FLAG_PREPARE,
    EP_COMBINE_MEMORY_METRIC_FLAG_LOAD,
    EP_COMBINE_MEMORY_METRIC_FLAG_COMPARE_SCAN,
    EP_COMBINE_MEMORY_METRIC_PAYLOAD_LOAD,
    EP_COMBINE_MEMORY_METRIC_REDUCE_ISSUE,
    EP_COMBINE_MEMORY_METRIC_FLAG_CLEAR_ISSUE,
    EP_COMBINE_MEMORY_METRIC_OUTPUT_CAST_STORE_ISSUE,
};

constexpr uint32_t kEpCombineMemoryProfileMetricCount =
    EP_COMBINE_MEMORY_METRIC_OUTPUT_CAST_STORE_ISSUE + 1U;

struct alignas(64) EpCombineMemoryProfileRecord {
    uint32_t marker;
    uint16_t version;
    uint16_t recordBytes;
    uint32_t rank;
    uint32_t core;
    uint32_t blockDim;
    uint32_t timePointCount;
    uint32_t metricCount;
    uint64_t generation;
    uint64_t timePoint[kEpCombineMemoryProfileTimePointCapacity];
    uint64_t metric[kEpCombineMemoryProfileMetricCapacity];
};

static_assert(kEpCombineMemoryProfileTimePointCount <
        kEpCombineMemoryProfileTimePointCapacity,
    "EP Combine Memory diagnostics require reserved timestamp slots");
static_assert(EP_COMBINE_MEMORY_DIAG_RECEIVE_BUFFER_NUM <
        kEpCombineMemoryProfileTimePointCapacity,
    "EP Combine Memory diagnostics exceed the profile record");
static_assert(kEpCombineMemoryProfileMetricCount <=
        kEpCombineMemoryProfileMetricCapacity,
    "EP Combine Memory metrics exceed the profile record");
static_assert(sizeof(EpCombineMemoryProfileRecord) == 512U,
    "EP Combine Memory profile record ABI changed");

} // namespace TileXREp

#endif // TILEXR_EP_COMMON_EP_COMBINE_MEMORY_PROFILE_H
