/*
 * 计算 MoonEP payload、scratch 与控制区的有界工作区布局，供 Host 参数校验与 kernel launch 使用。
 */
#include "combine_v2_layout.h"

#include <cstddef>
#include <limits>

#include "comm_args.h"
#include "tilexr_moonep.h"

namespace TileXRMoonEp {
namespace {

bool CheckedAdd(uint64_t lhs, uint64_t rhs, uint64_t *result)
{
    if (result == nullptr || rhs > std::numeric_limits<uint64_t>::max() - lhs) {
        return false;
    }
    *result = lhs + rhs;
    return true;
}

bool CheckedMultiply(uint64_t lhs, uint64_t rhs, uint64_t *result)
{
    if (result == nullptr || (lhs != 0 && rhs > std::numeric_limits<uint64_t>::max() / lhs)) {
        return false;
    }
    *result = lhs * rhs;
    return true;
}

bool CheckedAlign(uint64_t value, uint64_t alignment, uint64_t *result)
{
    if (result == nullptr || alignment == 0 || value >
        std::numeric_limits<uint64_t>::max() - (alignment - 1)) {
        return false;
    }
    *result = (value + alignment - 1) / alignment * alignment;
    return true;
}

} // namespace

int TileXRMoonEpBuildCombineV2Layout(int64_t bs, int64_t h,
    int64_t topK, int64_t nvS, uint32_t dtype,
    CombineV2Layout *layout)
{
    if (layout == nullptr || !MoonEpCombineV2ShapeValid(bs, h, topK, nvS) ||
        (dtype != TILEXR_MOONEP_DTYPE_BFLOAT16 &&
            dtype != TILEXR_MOONEP_DTYPE_FLOAT32) ||
        (dtype == TILEXR_MOONEP_DTYPE_FLOAT32 && h != 1)) {
        return TILEXR_MOONEP_ERROR_INVALID_ARGUMENT;
    }

    const uint64_t slots = static_cast<uint64_t>(nvS);
    const uint64_t elementBytes = dtype == TILEXR_MOONEP_DTYPE_BFLOAT16 ?
        sizeof(uint16_t) : sizeof(float);
    uint64_t rowBytes = 0;
    uint64_t expertBytes = 0;
    uint64_t scratch0Offset = 0;
    uint64_t scratch1Offset = 0;
    uint64_t requiredBytes = 0;
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

    if (!CheckedMultiply(static_cast<uint64_t>(h), elementBytes, &rowBytes) ||
        !CheckedMultiply(slots, rowBytes, &expertBytes) ||
        !CheckedAlign(expertBytes, kCombineV2ScratchAlignmentBytes, &scratch0Offset) ||
        !CheckedAdd(scratch0Offset, expertBytes, &scratch1Offset) ||
        !CheckedAdd(scratch1Offset, expertBytes, &requiredBytes) ||
        !CheckedAlign(requiredBytes, kMoonEpCombineV2TokenStrideBytes, &doneOffset) ||
        !CheckedMultiply(static_cast<uint64_t>(kMoonEpCombineV2EpochCount) *
                kMoonEpCombineV2RankCount * kMoonEpCombineV2LaneCount,
            kMoonEpCombineV2TokenStrideBytes, &doneBytes) ||
        !CheckedAdd(doneOffset, doneBytes, &controlSourceOffset) ||
        !CheckedMultiply(static_cast<uint64_t>(kMoonEpCombineV2CoreCount) *
                kMoonEpCombineV2LaneCount,
            kMoonEpCombineV2TokenStrideBytes, &controlSourceBytes) ||
        !CheckedAdd(controlSourceOffset, controlSourceBytes, &failureOffset) ||
        !CheckedMultiply(static_cast<uint64_t>(kMoonEpCombineV2EpochCount) *
                kMoonEpCombineV2CoreCount,
            kMoonEpCombineV2TokenStrideBytes, &failureBytes) ||
        !CheckedAdd(failureOffset, failureBytes, &collectiveStatusOffset) ||
        !CheckedMultiply(static_cast<uint64_t>(kMoonEpCombineV2EpochCount) *
                kMoonEpCombineV2CollectiveStatusSlotCount,
            kMoonEpCombineV2CollectiveStatusSlotBytes,
            &collectiveStatusBytes) ||
        !CheckedAdd(collectiveStatusOffset, collectiveStatusBytes,
            &requiredBytes) ||
        !CheckedAlign(requiredBytes, kCombineV2ScratchAlignmentBytes,
            &outputOffset) ||
        !CheckedMultiply(static_cast<uint64_t>(bs), rowBytes, &outputBytes) ||
        !CheckedAdd(outputOffset, outputBytes, &requiredBytes) ||
        !CheckedAlign(requiredBytes, kCombineV2RegistrationAlignmentBytes, &totalBytes) ||
        totalBytes > std::numeric_limits<uint32_t>::max() ||
        totalBytes > std::numeric_limits<std::size_t>::max()) {
        return TILEXR_MOONEP_ERROR_INVALID_ARGUMENT;
    }

    CombineV2Layout next {};
    next.slots = slots;
    next.rowBytes = rowBytes;
    next.expertBytes = expertBytes;
    next.scratchOffset[0] = scratch0Offset;
    next.scratchOffset[1] = scratch1Offset;
    next.scratchBytes = expertBytes;
    next.doneOffset = doneOffset;
    next.doneBytes = doneBytes;
    next.controlSourceOffset = controlSourceOffset;
    next.controlSourceBytes = controlSourceBytes;
    next.failureOffset = failureOffset;
    next.failureBytes = failureBytes;
    next.collectiveStatusOffset = collectiveStatusOffset;
    next.collectiveStatusBytes = collectiveStatusBytes;
    next.outputOffset = outputOffset;
    next.outputBytes = outputBytes;
    next.totalBytes = totalBytes;
    *layout = next;
    return TILEXR_MOONEP_SUCCESS;
}

int TileXRMoonEpBuildCombineV2WeightLayout(
    int64_t nvS, int32_t rankSize, CombineV2WeightLayout *layout)
{
    if (layout == nullptr || nvS <= 0 || rankSize <= 0 ||
        rankSize > TileXR::TILEXR_MAX_RANK_SIZE) {
        return TILEXR_MOONEP_ERROR_INVALID_ARGUMENT;
    }

    uint64_t recordPayloadBytes = 0U;
    uint64_t recordEpochBytes = 0U;
    uint64_t recordBytes = 0U;
    uint64_t doneEpochBytes = 0U;
    uint64_t doneBytes = 0U;
    uint64_t requiredBytes = 0U;
    uint64_t totalBytes = 0U;
    if (!CheckedMultiply(static_cast<uint64_t>(nvS),
            kMoonEpCombineV2WeightRecordBytes, &recordPayloadBytes) ||
        !CheckedAlign(recordPayloadBytes, kCombineV2ScratchAlignmentBytes,
            &recordEpochBytes) ||
        !CheckedMultiply(recordEpochBytes, kMoonEpCombineV2EpochCount,
            &recordBytes) ||
        !CheckedMultiply(static_cast<uint64_t>(rankSize),
            kMoonEpCombineV2WeightDoneStrideBytes, &doneEpochBytes) ||
        !CheckedMultiply(doneEpochBytes, kMoonEpCombineV2EpochCount,
            &doneBytes) ||
        !CheckedAdd(recordBytes, doneBytes, &requiredBytes) ||
        !CheckedAlign(requiredBytes, kCombineV2ScratchAlignmentBytes,
            &totalBytes) ||
        totalBytes > static_cast<uint64_t>(TileXR::IPC_BUFF_MAX_SIZE)) {
        return TILEXR_MOONEP_ERROR_INVALID_ARGUMENT;
    }

    CombineV2WeightLayout next {};
    next.recordOffset = 0U;
    next.recordEpochBytes = recordEpochBytes;
    next.recordBytes = recordBytes;
    next.doneOffset = recordBytes;
    next.doneEpochBytes = doneEpochBytes;
    next.doneBytes = doneBytes;
    next.totalBytes = totalBytes;
    *layout = next;
    return TILEXR_MOONEP_SUCCESS;
}

} // namespace TileXRMoonEp
