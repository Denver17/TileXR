#ifndef TILEXR_EP_MEMORY_SERVER_DEDUP_COMBINE_PROFILE_H
#define TILEXR_EP_MEMORY_SERVER_DEDUP_COMBINE_PROFILE_H
#include <cstdint>
namespace TileXREp {
constexpr uint32_t kEpServerDedupCombineProfilingRecordsPerLaunch = 32U;
constexpr uint32_t kEpServerDedupCombineProfileStart = 0x3001U;
constexpr uint32_t kEpServerDedupCombineProfileEnd = 0x3002U;
constexpr uint32_t kEpServerDedupCombineProfileInitBegin = 0x3003U;
constexpr uint32_t kEpServerDedupCombineProfileInitEnd = 0x3004U;
constexpr uint32_t kEpServerDedupCombineProfileSelectBegin = 0x3005U;
constexpr uint32_t kEpServerDedupCombineProfileSelectEnd = 0x3006U;
constexpr uint32_t kEpServerDedupCombineProfileCopyBegin = 0x3007U;
constexpr uint32_t kEpServerDedupCombineProfileCopyEnd = 0x3008U;
constexpr uint32_t kEpServerDedupCombineProfilePrepareBegin = 0x3009U;
constexpr uint32_t kEpServerDedupCombineProfilePrepareEnd = 0x300aU;
constexpr uint32_t kEpServerDedupCombineProfileLocalWaitBegin = 0x300bU;
constexpr uint32_t kEpServerDedupCombineProfileLocalWaitEnd = 0x300cU;
constexpr uint32_t kEpServerDedupCombineProfileNotifyBegin = 0x300dU;
constexpr uint32_t kEpServerDedupCombineProfileNotifyEnd = 0x300eU;
constexpr uint32_t kEpServerDedupCombineProfileReadyWaitBegin = 0x300fU;
constexpr uint32_t kEpServerDedupCombineProfileReadyWaitEnd = 0x3010U;
constexpr uint32_t kEpServerDedupCombineProfileClearBegin = 0x3011U;
constexpr uint32_t kEpServerDedupCombineProfileClearEnd = 0x3012U;
constexpr uint32_t kEpServerDedupCombineProfileDrainBegin = 0x3013U;
constexpr uint32_t kEpServerDedupCombineProfileDrainEnd = 0x3014U;
constexpr uint32_t kEpServerDedupCombineProfileDelayBegin = 0x3015U;
constexpr uint32_t kEpServerDedupCombineProfileDelayEnd = 0x3016U;
} // namespace TileXREp
#endif
