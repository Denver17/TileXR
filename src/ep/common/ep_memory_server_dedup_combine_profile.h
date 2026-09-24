#ifndef TILEXR_EP_MEMORY_SERVER_DEDUP_COMBINE_PROFILE_H
#define TILEXR_EP_MEMORY_SERVER_DEDUP_COMBINE_PROFILE_H
#include <cstdint>
namespace TileXREp {
// Latest capture only. Covers two records per token plus module boundaries:
// Each Recv core owns M4 <=1250 rows and M5 <20480 tokens (metadata alone
// costs >=12 bytes/token in 240 KiB). M6 costs >=8 bytes/token and
// owns fewer than 30720 tokens. Both modes fit 65536 records.
constexpr uint32_t kEpServerDedupCombineProfilingRecordsPerLaunch = 65536U;
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
// P2.2 module intervals; earlier fine-grained IDs remain reserved.
constexpr uint32_t kEpServerDedupCombineProfileStageBegin = 0x3017U;
constexpr uint32_t kEpServerDedupCombineProfileStageEnd = 0x3018U;
constexpr uint32_t kEpServerDedupCombineProfileControlBegin = 0x3019U;
constexpr uint32_t kEpServerDedupCombineProfileControlEnd = 0x301aU;
constexpr uint32_t kEpServerDedupCombineProfileDirectBegin = 0x301bU;
constexpr uint32_t kEpServerDedupCombineProfileDirectEnd = 0x301cU;
constexpr uint32_t kEpServerDedupCombineProfilePartialBegin = 0x301dU;
constexpr uint32_t kEpServerDedupCombineProfilePartialEnd = 0x301eU;
constexpr uint32_t kEpServerDedupCombineProfileFinalBegin = 0x301fU;
constexpr uint32_t kEpServerDedupCombineProfileFinalEnd = 0x3020U;
constexpr uint32_t kEpServerDedupCombineProfileDirectMte3End = 0x3021U;
constexpr uint32_t kEpServerDedupCombineProfilePartialTokenBegin = 0x3022U;
constexpr uint32_t kEpServerDedupCombineProfilePartialTokenEnd = 0x3023U;
constexpr uint32_t kEpServerDedupCombineProfileFinalTokenBegin = 0x3024U;
constexpr uint32_t kEpServerDedupCombineProfileFinalTokenEnd = 0x3025U;
constexpr uint32_t kEpServerDedupCombineProfilePrepareNotifyBegin = 0x3026U;
constexpr uint32_t kEpServerDedupCombineProfilePrepareNotifyEnd = 0x3027U;
constexpr uint32_t kEpServerDedupCombineProfileServerWaitBegin = 0x3028U;
constexpr uint32_t kEpServerDedupCombineProfileServerWaitEnd = 0x3029U;
constexpr uint32_t kEpServerDedupCombineProfileM6Begin = 0x302aU;
constexpr uint32_t kEpServerDedupCombineProfileM6End = 0x302bU;
constexpr uint32_t kEpServerDedupCombineProfileM6TokenBegin = 0x302cU;
constexpr uint32_t kEpServerDedupCombineProfileM6TokenEnd = 0x302dU;
} // namespace TileXREp
#endif
