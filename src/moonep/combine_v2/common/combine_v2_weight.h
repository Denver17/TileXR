#ifndef TILEXR_MOONEP_COMBINE_V2_WEIGHT_H
#define TILEXR_MOONEP_COMBINE_V2_WEIGHT_H

#include <cstdint>

#include "combine_v2_wqe_batch.h"

#if defined(__CCE__) && defined(__CCE_IS_AICORE__)
#define TILEXR_MOONEP_COMBINE_V2_WEIGHT_INLINE \
    __attribute__((always_inline)) inline __aicore__
#else
#define TILEXR_MOONEP_COMBINE_V2_WEIGHT_INLINE inline constexpr
#endif

namespace TileXRMoonEp {

constexpr uint64_t kMoonEpCombineV2WeightRecordBytes = 16U;
constexpr uint64_t kMoonEpCombineV2WeightDoneStrideBytes = 64U;

struct alignas(16) MoonEpCombineV2WeightRecord {
    float value;
    uint32_t reserved;
    uint64_t magic;
};

static_assert(sizeof(MoonEpCombineV2WeightRecord) ==
        kMoonEpCombineV2WeightRecordBytes,
    "Combine V2 weight record ABI changed");

TILEXR_MOONEP_COMBINE_V2_WEIGHT_INLINE uint32_t
MoonEpCombineV2WeightBatchRows(uint32_t selectedCount, uint32_t batchStart)
{
    if (batchStart >= selectedCount) {
        return 0U;
    }
    const uint32_t remaining = selectedCount - batchStart;
    return remaining < kMoonEpCombineV2PayloadBatchRows ? remaining :
        kMoonEpCombineV2PayloadBatchRows;
}

TILEXR_MOONEP_COMBINE_V2_WEIGHT_INLINE uint32_t
MoonEpCombineV2WeightBatchCount(uint32_t selectedCount)
{
    return selectedCount / kMoonEpCombineV2PayloadBatchRows +
        (selectedCount % kMoonEpCombineV2PayloadBatchRows == 0U ? 0U : 1U);
}

TILEXR_MOONEP_COMBINE_V2_WEIGHT_INLINE uint32_t
MoonEpCombineV2WeightStageIndex(uint32_t batchSequence)
{
    return batchSequence & 1U;
}

TILEXR_MOONEP_COMBINE_V2_WEIGHT_INLINE bool
MoonEpCombineV2WeightStageReuseWaitRequired(uint32_t batchSequence)
{
    return batchSequence >= 2U;
}

TILEXR_MOONEP_COMBINE_V2_WEIGHT_INLINE bool
MoonEpCombineV2WeightRunContinues(
    uint32_t firstTarget, uint32_t runLength, uint32_t nextTarget)
{
    return static_cast<uint64_t>(firstTarget) + runLength == nextTarget;
}

} // namespace TileXRMoonEp

#undef TILEXR_MOONEP_COMBINE_V2_WEIGHT_INLINE

#endif // TILEXR_MOONEP_COMBINE_V2_WEIGHT_H
