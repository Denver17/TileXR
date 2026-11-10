/*
 * 计算 MoonEP payload、scratch 与控制区的有界工作区布局，供 Host 参数校验与 kernel launch 使用。
 */
#ifndef TILEXR_MOONEP_COMBINE_V2_LAYOUT_H
#define TILEXR_MOONEP_COMBINE_V2_LAYOUT_H

#include <cstdint>

#include "combine_v2_schedule.h"
#include "combine_v2_weight.h"

namespace TileXRMoonEp {

constexpr uint64_t kCombineV2ScratchAlignmentBytes = 64;
constexpr uint64_t kCombineV2RegistrationAlignmentBytes = UINT64_C(2) * 1024 * 1024;

struct CombineV2Layout {
    uint64_t slots = 0;
    uint64_t rowBytes = 0;
    uint64_t expertBytes = 0;
    uint64_t scratchOffset[kMoonEpCombineV2EpochCount] = {};
    uint64_t scratchBytes = 0;
    uint64_t doneOffset = 0;
    uint64_t doneBytes = 0;
    uint64_t controlSourceOffset = 0;
    uint64_t controlSourceBytes = 0;
    uint64_t failureOffset = 0;
    uint64_t failureBytes = 0;
    uint64_t collectiveStatusOffset = 0;
    uint64_t collectiveStatusBytes = 0;
    uint64_t outputOffset = 0;
    uint64_t outputBytes = 0;
    uint64_t totalBytes = 0;
};

struct CombineV2WeightLayout {
    uint64_t recordOffset = 0;
    uint64_t recordEpochBytes = 0;
    uint64_t recordBytes = 0;
    uint64_t doneOffset = 0;
    uint64_t doneEpochBytes = 0;
    uint64_t doneBytes = 0;
    uint64_t totalBytes = 0;
};

int TileXRMoonEpBuildCombineV2Layout(int64_t bs, int64_t h,
    int64_t topK, int64_t nvS, uint32_t dtype,
    CombineV2Layout *layout);

int TileXRMoonEpBuildCombineV2WeightLayout(
    int64_t nvS, int32_t rankSize, CombineV2WeightLayout *layout);

} // namespace TileXRMoonEp

#endif // TILEXR_MOONEP_COMBINE_V2_LAYOUT_H
