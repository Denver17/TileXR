#ifndef TILEXR_MOONEP_REDUCE_GRAD_LAYOUT_H
#define TILEXR_MOONEP_REDUCE_GRAD_LAYOUT_H

#include <cstdint>

#include "reduce_grad_common.h"

namespace TileXRMoonEp {

int TileXRMoonEpBuildReduceGradLayout(int64_t rank, int64_t rankSize,
    int64_t localRankSize, int64_t expertCount, int64_t prefetchSlots,
    const uint64_t rowElements[kReduceGradProjectionCount],
    uint32_t transportQpCount, uint64_t requestedChunkBytes,
    ReduceGradLayout *out);

int TileXRMoonEpBuildReduceGradLayout(int64_t rank, int64_t rankSize,
    int64_t expertCount, int64_t prefetchSlots,
    const uint64_t rowElements[kReduceGradProjectionCount],
    uint32_t transportQpCount, uint64_t requestedChunkBytes,
    ReduceGradLayout *out);

int TileXRMoonEpBuildReduceGradLayout(int64_t rank, int64_t rankSize,
    int64_t expertCount, const uint64_t rowElements[kReduceGradProjectionCount],
    uint32_t transportQpCount, uint64_t requestedChunkBytes,
    ReduceGradLayout *out);

} // namespace TileXRMoonEp

#endif // TILEXR_MOONEP_REDUCE_GRAD_LAYOUT_H
