#include "reduce_grad_layout.h"

#include <cerrno>
#include <cstdlib>
#include <limits>

#include "tilexr_types.h"

namespace TileXRMoonEp {
namespace {

bool CheckedAdd(uint64_t lhs, uint64_t rhs, uint64_t *out)
{
    if (out == nullptr || rhs > std::numeric_limits<uint64_t>::max() - lhs) {
        return false;
    }
    *out = lhs + rhs;
    return true;
}

bool CheckedMul(uint64_t lhs, uint64_t rhs, uint64_t *out)
{
    if (out == nullptr || (lhs != 0 && rhs > std::numeric_limits<uint64_t>::max() / lhs)) {
        return false;
    }
    *out = lhs * rhs;
    return true;
}

bool AlignUp(uint64_t value, uint64_t alignment, uint64_t *out)
{
    if (out == nullptr || alignment == 0) {
        return false;
    }
    const uint64_t remainder = value % alignment;
    return remainder == 0 ? (*out = value, true) :
        CheckedAdd(value, alignment - remainder, out);
}

bool DivideRoundUp(uint64_t value, uint64_t divisor, uint64_t *out)
{
    if (out == nullptr || divisor == 0) {
        return false;
    }
    *out = value / divisor + (value % divisor != 0 ? 1U : 0U);
    return true;
}

bool ScoreGreater(uint64_t lhsBytes, uint32_t lhsCount,
    uint64_t rhsBytes, uint32_t rhsCount)
{
    const uint64_t lhsQuotient = lhsBytes / lhsCount;
    const uint64_t rhsQuotient = rhsBytes / rhsCount;
    if (lhsQuotient != rhsQuotient) {
        return lhsQuotient > rhsQuotient;
    }
    const uint64_t lhsRemainder = lhsBytes % lhsCount;
    const uint64_t rhsRemainder = rhsBytes % rhsCount;
    return lhsRemainder * rhsCount > rhsRemainder * lhsCount;
}

bool AllocateProjectionQps(ReduceGradLayout *layout)
{
    if (layout == nullptr || layout->qpCount < kReduceGradMinMultiRankQpCount ||
        layout->qpCount > kReduceGradMaxUdmaQpCount) {
        return false;
    }
    for (uint32_t projection = 0; projection < kReduceGradProjectionCount; ++projection) {
        layout->projectionQpCounts[projection] = 1;
    }
    for (uint32_t assigned = kReduceGradProjectionCount;
        assigned < layout->qpCount; ++assigned) {
        uint32_t selected = 0;
        for (uint32_t projection = 1; projection < kReduceGradProjectionCount; ++projection) {
            if (ScoreGreater(layout->rowBytes[projection],
                    layout->projectionQpCounts[projection],
                    layout->rowBytes[selected], layout->projectionQpCounts[selected])) {
                selected = projection;
            }
        }
        ++layout->projectionQpCounts[selected];
    }

    uint32_t cursor = 0;
    for (uint32_t projection = 0; projection < kReduceGradProjectionCount; ++projection) {
        layout->projectionQpBase[projection] = cursor;
        for (uint32_t lane = 0; lane < layout->projectionQpCounts[projection]; ++lane) {
            if (cursor >= layout->qpCount || cursor >= kReduceGradMaxUdmaQpCount) {
                return false;
            }
            layout->qpProjection[cursor++] = projection;
        }
    }
    return cursor == layout->qpCount;
}

bool MapPhysicalQps(ReduceGradLayout *layout)
{
    if (layout == nullptr || layout->laneCount == 0 ||
        layout->laneCount > kReduceGradMaxUdmaQpCount ||
        layout->transportQpCount < layout->laneCount) {
        return false;
    }
    for (uint32_t lane = 0; lane < layout->laneCount; ++lane) {
        layout->lanePhysicalQps[lane] = lane;
    }
    if (layout->transportQpCount == kReduceGradMaxTransportQpCount) {
        layout->lanePhysicalQps[kReduceGradDown] = 16U;
    }
    return true;
}

bool ResolveBlockDim(uint32_t laneCount, int64_t *blockDim)
{
    if (blockDim == nullptr || laneCount == 0) {
        return false;
    }
    const char *value = std::getenv("TILEXR_MOONEP_REDUCE_GRAD_BLOCK_DIM");
    int64_t selected = kReduceGradMaxAivBlockCount;
    if (value != nullptr && value[0] != '\0') {
        errno = 0;
        char *end = nullptr;
        const long long parsed = std::strtoll(value, &end, 10);
        if (errno != 0 || end == value || end == nullptr || *end != '\0') {
            return false;
        }
        selected = static_cast<int64_t>(parsed);
    }
    const uint64_t minimum = 2U * laneCount;
    if (selected <= 0 || selected > kReduceGradMaxAivBlockCount ||
        static_cast<uint64_t>(selected) < minimum) {
        return false;
    }
    *blockDim = selected;
    return true;
}

} // namespace

int TileXRMoonEpBuildReduceGradLayout(int64_t rank, int64_t rankSize,
    int64_t localRankSize, int64_t expertCount, int64_t prefetchSlots,
    const uint64_t rowElements[kReduceGradProjectionCount],
    uint32_t transportQpCount, uint64_t requestedChunkBytes,
    ReduceGradLayout *out)
{
    if (out == nullptr || rowElements == nullptr || rankSize < kReduceGradMinRankCount ||
        rankSize > TileXR::TILEXR_MAX_RANK_SIZE || rank < 0 || rank >= rankSize ||
        localRankSize <= 0 || localRankSize > rankSize ||
        rankSize % localRankSize != 0 || expertCount <= 0 ||
        expertCount % rankSize != 0 || prefetchSlots <= 0 ||
        expertCount > std::numeric_limits<int32_t>::max() ||
        prefetchSlots > std::numeric_limits<int32_t>::max() ||
        prefetchSlots > std::numeric_limits<int32_t>::max() / rankSize ||
        transportQpCount < kReduceGradMinMultiRankQpCount ||
        transportQpCount > kReduceGradMaxTransportQpCount ||
        (transportQpCount > kReduceGradMaxUdmaQpCount &&
            transportQpCount != kReduceGradMaxTransportQpCount)) {
        return TileXR::TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    const bool crossNode = localRankSize < rankSize;
    if (crossNode && transportQpCount == kReduceGradMinMultiRankQpCount) {
        return TileXR::TILEXR_ERROR_NOT_SUPPORT;
    }

    ReduceGradLayout next {};
    next.rank = rank;
    next.rankSize = rankSize;
    next.expertCount = expertCount;
    next.expertsPerRank = expertCount / rankSize;
    next.prefetchSlots = prefetchSlots;
    next.transportQpCount = transportQpCount;
    next.qpCount = transportQpCount == kReduceGradMaxTransportQpCount ?
        kReduceGradProjectionCount : transportQpCount - (crossNode ? 1U : 0U);
    next.laneCount = next.qpCount;
    next.barrierPhysicalQp = crossNode ? transportQpCount - 1U :
        kReduceGradInvalidPhysicalQp;

    for (uint32_t projection = 0; projection < kReduceGradProjectionCount; ++projection) {
        uint64_t bytes = 0;
        if (rowElements[projection] == 0 ||
            !CheckedMul(rowElements[projection], sizeof(float), &bytes)) {
            return TileXR::TILEXR_ERROR_PARA_CHECK_FAIL;
        }
        next.rowElements[projection] = rowElements[projection];
        next.rowBytes[projection] = bytes;
    }

    if (!ResolveBlockDim(next.laneCount, &next.blockDim)) {
        return TileXR::TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    if (!AllocateProjectionQps(&next)) {
        return TileXR::TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    if (!MapPhysicalQps(&next)) {
        return TileXR::TILEXR_ERROR_PARA_CHECK_FAIL;
    }

    const uint64_t desiredChunk = requestedChunkBytes == 0 ?
        kReduceGradDefaultChunkBytes : requestedChunkBytes;
    if (desiredChunk == 0 || desiredChunk > UINT32_MAX ||
        !AlignUp(desiredChunk, kReduceGradUdmaAlignment, &next.chunkBytes) ||
        next.chunkBytes > UINT32_MAX) {
        return TileXR::TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    for (uint32_t projection = 0; projection < kReduceGradProjectionCount; ++projection) {
        if (!DivideRoundUp(next.rowBytes[projection], next.chunkBytes,
                &next.chunkCounts[projection])) {
            return TileXR::TILEXR_ERROR_PARA_CHECK_FAIL;
        }
    }

    uint64_t stateBytes = 0;
    uint64_t payloadBytes = 0;
    uint64_t cursor = 0;
    if (!CheckedMul(next.laneCount, kReduceGradLaneStateStrideBytes, &stateBytes) ||
        !AlignUp(stateBytes, kReduceGradUdmaAlignment, &next.laneStateBytes) ||
        !CheckedMul(static_cast<uint64_t>(rankSize), next.chunkBytes,
            &next.bankStrideBytes) ||
        !CheckedMul(kReduceGradBankCount, next.bankStrideBytes,
            &next.laneStrideBytes) ||
        !CheckedMul(next.laneCount, next.laneStrideBytes, &payloadBytes)) {
        return TileXR::TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    next.stagingOffset = next.laneStateBytes;
    cursor = next.stagingOffset;
    if (!CheckedAdd(cursor, payloadBytes, &cursor)) {
        return TileXR::TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    if (crossNode) {
        if (!AlignUp(cursor, TileXR::TILEXR_UDMA_CACHE_LINE_SIZE,
                &next.barrierOffset) ||
            !CheckedMul(static_cast<uint64_t>(rankSize),
                TileXR::TILEXR_UDMA_CACHE_LINE_SIZE, &next.barrierBytes) ||
            !CheckedAdd(next.barrierOffset, next.barrierBytes, &cursor)) {
            return TileXR::TILEXR_ERROR_PARA_CHECK_FAIL;
        }
    }
    if (!AlignUp(cursor, kReduceGradWorkspaceAlignment, &next.workspaceBytes)) {
        return TileXR::TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    if (crossNode && (next.barrierPhysicalQp >= next.transportQpCount ||
            next.barrierPhysicalQp == next.lanePhysicalQps[0] ||
            next.barrierPhysicalQp == next.lanePhysicalQps[1] ||
            next.barrierPhysicalQp == next.lanePhysicalQps[2])) {
        return TileXR::TILEXR_ERROR_PARA_CHECK_FAIL;
    }

    *out = next;
    return TileXR::TILEXR_SUCCESS;
}

int TileXRMoonEpBuildReduceGradLayout(int64_t rank, int64_t rankSize,
    int64_t expertCount, int64_t prefetchSlots,
    const uint64_t rowElements[kReduceGradProjectionCount],
    uint32_t transportQpCount, uint64_t requestedChunkBytes,
    ReduceGradLayout *out)
{
    return TileXRMoonEpBuildReduceGradLayout(rank, rankSize, rankSize,
        expertCount, prefetchSlots, rowElements, transportQpCount,
        requestedChunkBytes, out);
}

int TileXRMoonEpBuildReduceGradLayout(int64_t rank, int64_t rankSize,
    int64_t expertCount, const uint64_t rowElements[kReduceGradProjectionCount],
    uint32_t transportQpCount, uint64_t requestedChunkBytes,
    ReduceGradLayout *out)
{
    const int64_t prefetchSlots = rankSize > 0 && expertCount > 0 ?
        expertCount / rankSize : 0;
    return TileXRMoonEpBuildReduceGradLayout(rank, rankSize, rankSize,
        expertCount, prefetchSlots, rowElements, transportQpCount,
        requestedChunkBytes, out);
}

} // namespace TileXRMoonEp
