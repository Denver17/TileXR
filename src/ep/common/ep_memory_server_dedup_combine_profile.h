#ifndef TILEXR_EP_MEMORY_SERVER_DEDUP_COMBINE_PROFILE_H
#define TILEXR_EP_MEMORY_SERVER_DEDUP_COMBINE_PROFILE_H
#include <cstdint>
#include "ep_memory_server_dedup_combine_config.h"
namespace TileXREp {
// Latest capture only. Dedup token intervals and M3 (<=10000 total rows)
// fit the floor. Non-dedup M6 needs 8 records/token plus 4/top-k member.
// Keep Host reservation, device writer and runner allocation identical.
constexpr uint32_t kEpServerDedupCombineProfilingRecordsPerLaunch = 65536U;
#if defined(__CCE__) && defined(__CCE_IS_AICORE__)
__aicore__ inline
#endif
constexpr uint32_t EpCombineProfilingRecordsPerLaunch(uint32_t bs, uint32_t k)
{
    const uint32_t required = kEpServerDedupEnable ? 0U :
        EpCombineAlign(EpCombineM5TokenCount(bs) * (8U + 4U * k) + 128U, 16U);
    return required > kEpServerDedupCombineProfilingRecordsPerLaunch ? required :
        kEpServerDedupCombineProfilingRecordsPerLaunch;
}
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
// Non-dedup preparation and loop intervals; no additional completion fences.
constexpr uint32_t kEpServerDedupCombineProfileDirectPrepareBegin = 0x302eU;
constexpr uint32_t kEpServerDedupCombineProfileDirectPrepareEnd = 0x302fU;
constexpr uint32_t kEpServerDedupCombineProfileDirectSelectBegin = 0x3030U;
constexpr uint32_t kEpServerDedupCombineProfileDirectSelectEnd = 0x3031U;
constexpr uint32_t kEpServerDedupCombineProfileDirectReadBegin = 0x3032U;
constexpr uint32_t kEpServerDedupCombineProfileDirectReadEnd = 0x3033U;
constexpr uint32_t kEpServerDedupCombineProfileDirectSendBegin = 0x3034U;
constexpr uint32_t kEpServerDedupCombineProfileDirectSendEnd = 0x3035U;
constexpr uint32_t kEpServerDedupCombineProfileDirectDrainBegin = 0x3036U;
constexpr uint32_t kEpServerDedupCombineProfileDirectDrainEnd = 0x3037U;
constexpr uint32_t kEpServerDedupCombineProfileM6PrepareBegin = 0x3038U;
constexpr uint32_t kEpServerDedupCombineProfileM6PrepareEnd = 0x3039U;
constexpr uint32_t kEpServerDedupCombineProfileM6ReadyBegin = 0x303aU;
constexpr uint32_t kEpServerDedupCombineProfileM6ReadyEnd = 0x303bU;
constexpr uint32_t kEpServerDedupCombineProfileM6PrefetchBegin = 0x303cU;
constexpr uint32_t kEpServerDedupCombineProfileM6PrefetchEnd = 0x303dU;
constexpr uint32_t kEpServerDedupCombineProfileM6InputWaitBegin = 0x303eU;
constexpr uint32_t kEpServerDedupCombineProfileM6InputWaitEnd = 0x303fU;
constexpr uint32_t kEpServerDedupCombineProfileM6ComputeBegin = 0x3040U;
constexpr uint32_t kEpServerDedupCombineProfileM6ComputeEnd = 0x3041U;
constexpr uint32_t kEpServerDedupCombineProfileM6OutputBegin = 0x3042U;
constexpr uint32_t kEpServerDedupCombineProfileM6OutputEnd = 0x3043U;
constexpr uint32_t kEpServerDedupCombineProfileM6RetireBegin = 0x3044U;
constexpr uint32_t kEpServerDedupCombineProfileM6RetireEnd = 0x3045U;
} // namespace TileXREp
#endif
