#include "ep_memory_server_dedup_combine_host.h"

#include <algorithm>
#include <limits>
#include "ep_memory_server_dedup_protocol.h"
#include "ep_memory_server_dedup_count_layout.h"

namespace TileXREp {
namespace {
bool Multiply(uint64_t a, uint64_t b, uint64_t &out)
{
    if (a != 0 && b > std::numeric_limits<size_t>::max() / a) { return false; }
    out = a * b;
    return true;
}
bool Add(uint64_t a, uint64_t b, uint64_t &out)
{
    if (b > std::numeric_limits<size_t>::max() - a) { return false; }
    out = a + b;
    return true;
}
} // namespace

int TileXREpBuildMemoryServerDedupCombineLayout(int64_t ranks, int64_t bs, int64_t h,
    int64_t topK, int64_t experts, EpMemoryServerDedupCombineLayout *out)
{
    if (out == nullptr) { return TileXR::TILEXR_ERROR_PARA_CHECK_FAIL; }
    *out = {};
    if (ranks < 16 || ranks > TileXR::TILEXR_MAX_RANK_SIZE || ranks % kEpServerDedupRanksPerServer != 0 ||
        bs <= 0 || h <= 0 || h > UINT32_MAX || topK <= 0 || topK > kEpServerDedupMaxTopK ||
        experts <= 0 || experts > kEpServerDedupMaxExperts || experts % ranks != 0 ||
        topK > experts || bs > UINT32_MAX / topK) {
        return TileXR::TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    EpMemoryServerDedupCombineLayout next {};
    next.globalBs = uint64_t(ranks) * uint64_t(bs);
    next.relayReadIndexRowStrideBytes = uint64_t(topK) * 8U;
    const uint64_t rowBytes = uint64_t(h) * 2U;
    if (!Multiply(next.globalBs, std::min(topK, experts / ranks), next.capacityRows) ||
        !Multiply(next.capacityRows, rowBytes, next.expertOutBytes) ||
        !Multiply(next.capacityRows, 12U, next.assistInfoBytes) ||
        !Multiply(experts, 4U, next.sendCountsBytes) ||
        !Multiply(uint64_t(bs) * topK, 4U, next.expertScalesBytes) ||
        !Multiply(next.capacityRows, 4U, next.tokenTypeBytes) ||
        !Multiply(next.capacityRows, next.relayReadIndexRowStrideBytes, next.relayReadIndexBytes) ||
        !Multiply(bs, 4U, next.sourceMaskBytes) || !Multiply(bs, rowBytes, next.yOutBytes)) {
        return TileXR::TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    *out = next;
    return TileXR::TILEXR_SUCCESS;
}

int TileXREpBuildServerDedupCombineDataLayout(int64_t ranks, int64_t bs, int64_t h,
    int64_t topK, int64_t experts, uint64_t resultSlotBytes, EpServerDedupCombineDataLayout *out)
{
    if (out == nullptr) { return TileXR::TILEXR_ERROR_PARA_CHECK_FAIL; }
    *out = {};
    EpMemoryServerDedupCombineLayout tensors {};
    int ret = TileXREpBuildMemoryServerDedupCombineLayout(ranks, bs, h, topK, experts, &tensors);
    if (ret != TileXR::TILEXR_SUCCESS) { return ret; }
    if (resultSlotBytes == 0 || resultSlotBytes % 512U != 0) { return TileXR::TILEXR_ERROR_PARA_CHECK_FAIL; }
    EpServerDedupCombineDataLayout next {};
    next.resultOffset = TILEXR_EP_SERVER_DEDUP_SHARED_DATA_OFFSET;
    next.stageRowBytes = (uint64_t(h) * 2U + 511U) / 512U * 512U;
    next.stageRows = uint64_t(experts) * bs;
    // resultBytes is already aligned because resultSlotBytes is aligned.
    if (!Multiply(uint64_t(bs) * topK, resultSlotBytes, next.resultBytes) ||
        !Add(next.resultOffset, next.resultBytes, next.stageOffset) ||
        !Multiply(next.stageRows, next.stageRowBytes, next.stageBytes) ||
        !Add(next.stageOffset, next.stageBytes, next.requiredWindowBytes)) {
        return TileXR::TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    *out = next;
    return TileXR::TILEXR_SUCCESS;
}
} // namespace TileXREp
