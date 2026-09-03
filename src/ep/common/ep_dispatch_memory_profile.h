#ifndef TILEXR_EP_COMMON_EP_DISPATCH_MEMORY_PROFILE_H
#define TILEXR_EP_COMMON_EP_DISPATCH_MEMORY_PROFILE_H

#include <cstdint>

namespace TileXREp {

constexpr uint32_t kEpDispatchMemoryProfileMarker = 0x45504450U; // EPDP
constexpr uint16_t kEpDispatchMemoryProfileVersion = 2U;
constexpr uint32_t kEpDispatchMemoryProfileCyclesPerUs = 1000U;
constexpr uint32_t kEpDispatchMemoryProfileTimePointCapacity = 40U;
constexpr uint32_t kEpDispatchMemoryProfileMetricCapacity = 16U;

enum EpDispatchMemoryProfileCoreRole : uint32_t {
    EP_DISPATCH_MEMORY_ROLE_SENDER = 0U,
    EP_DISPATCH_MEMORY_ROLE_COUNT_CUMSUM = 1U,
};

enum EpDispatchMemoryProfileTimePointIndex : uint32_t {
    EP_DISPATCH_MEMORY_TIME_INIT_BEGIN = 0U,
    EP_DISPATCH_MEMORY_TIME_INIT_END,
    EP_DISPATCH_MEMORY_TIME_TRANSFER_GROUP0_END,
    EP_DISPATCH_MEMORY_TIME_TRANSFER_END,
    EP_DISPATCH_MEMORY_TIME_CUMSUM_END,
    EP_DISPATCH_MEMORY_TIME_FIRST_PAYLOAD_READY,
    EP_DISPATCH_MEMORY_TIME_LAST_PAYLOAD_READY,
    EP_DISPATCH_MEMORY_TIME_FINAL_END,
};

constexpr uint32_t kEpDispatchMemoryProfileTimePointCount =
    EP_DISPATCH_MEMORY_TIME_FINAL_END + 1U;

enum EpDispatchMemoryProfileDiagnosticIndex : uint32_t {
    EP_DISPATCH_MEMORY_DIAG_ASSIGNED_ROUTES = kEpDispatchMemoryProfileTimePointCount,
    EP_DISPATCH_MEMORY_DIAG_ISSUED_ROUTES,
    EP_DISPATCH_MEMORY_DIAG_COUNT_RECORDS,
    EP_DISPATCH_MEMORY_DIAG_ALLTOALL_CORE_COUNT,
    EP_DISPATCH_MEMORY_DIAG_SEND_BUFFER_NUM,
    EP_DISPATCH_MEMORY_DIAG_STATUS_POLL_ATTEMPTS,
    EP_DISPATCH_MEMORY_DIAG_STATUS_POLL_MISSES,
    EP_DISPATCH_MEMORY_DIAG_CUMSUM_POLL_ATTEMPTS,
    EP_DISPATCH_MEMORY_DIAG_CUMSUM_GATE_ATTEMPTS,
    EP_DISPATCH_MEMORY_DIAG_VALID_EXPERTS,
    EP_DISPATCH_MEMORY_DIAG_RECEIVE_BATCHES,
    EP_DISPATCH_MEMORY_DIAG_RECEIVE_TOKENS,
    EP_DISPATCH_MEMORY_DIAG_PAYLOAD_POLL_ATTEMPTS,
    EP_DISPATCH_MEMORY_DIAG_PAYLOAD_POLL_MISSES,
    EP_DISPATCH_MEMORY_DIAG_MAX_COPY_TOKEN_COUNT,
    EP_DISPATCH_MEMORY_DIAG_BLOCKS_PER_TOKEN,
    EP_DISPATCH_MEMORY_DIAG_SEND_GROUP_COUNT,
};

constexpr uint32_t kEpDispatchMemoryProfileMetricCount = 0U;

struct alignas(64) EpDispatchMemoryProfileRecord {
    uint32_t marker;
    uint16_t version;
    uint16_t recordBytes;
    uint32_t rank;
    uint32_t core;
    uint32_t blockDim;
    uint32_t coreRole;
    uint32_t timePointCount;
    uint32_t metricCount;
    uint64_t generation;
    uint64_t timePoint[kEpDispatchMemoryProfileTimePointCapacity];
    uint64_t metric[kEpDispatchMemoryProfileMetricCapacity];
};

static_assert(kEpDispatchMemoryProfileTimePointCount <
        kEpDispatchMemoryProfileTimePointCapacity,
    "EP Dispatch Memory diagnostics require reserved timestamp slots");
static_assert(EP_DISPATCH_MEMORY_DIAG_SEND_GROUP_COUNT <
        kEpDispatchMemoryProfileTimePointCapacity,
    "EP Dispatch Memory diagnostics exceed the profile record");
static_assert(kEpDispatchMemoryProfileMetricCount <=
        kEpDispatchMemoryProfileMetricCapacity,
    "EP Dispatch Memory metrics exceed the profile record");
static_assert(sizeof(EpDispatchMemoryProfileRecord) == 512U,
    "EP Dispatch Memory profile record ABI changed");

} // namespace TileXREp

#endif // TILEXR_EP_COMMON_EP_DISPATCH_MEMORY_PROFILE_H
