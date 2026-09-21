#ifndef TILEXR_EP_MEMORY_SERVER_DEDUP_COMBINE_PROFILE_H
#define TILEXR_EP_MEMORY_SERVER_DEDUP_COMBINE_PROFILE_H
#include <cstdint>
namespace TileXREp {
constexpr uint32_t kEpServerDedupCombineProfilingRecordsPerLaunch = 8U;
constexpr uint32_t kEpServerDedupCombineProfileStart = 0x3001U;
constexpr uint32_t kEpServerDedupCombineProfileEnd = 0x3002U;
} // namespace TileXREp
#endif
