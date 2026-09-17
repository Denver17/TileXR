#include "ep_memory_server_dedup_layout.h"

#include <algorithm>
#include <limits>

#include "comm_args.h"
#include "ep_memory_server_dedup_protocol.h"
#include "ep_memory_server_dedup_count_layout.h"
#include "tilexr_types.h"

namespace TileXREp {
namespace {

bool MultiplyBytes(uint64_t lhs, uint64_t rhs, uint64_t *out)
{
    const uint64_t limit = std::numeric_limits<std::size_t>::max();
    if (lhs != 0 && rhs > limit / lhs) {
        return false;
    }
    *out = lhs * rhs;
    return true;
}

// Sender reuses Count scratch but retains any in-flight count records. Budget
// the busiest sender and one packet lane before the device subtracts usedBytes.
uint64_t SenderMinimumUbBytes(int64_t rankSize, int64_t bs, int64_t topK, int64_t experts,
    int64_t sendCoreNum, bool quantized, const EpServerDedupDataLayout &data)
{
    const uint64_t tokens = (uint64_t(bs) + sendCoreNum - 1U) / sendCoreNum;
    const uint64_t routes = uint64_t(bs) * topK * sizeof(int32_t);
    const uint64_t qBytes = TileXREpDedupAlign(routes, 32U);
    const uint64_t scaleCount = data.quantCount / 32U;
    const uint64_t quantWorkBytes = TileXREpDedupAlign(
        TileXREpDedupAlign(scaleCount, 32U) * 4U + scaleCount * 2U, 32U);
    const uint64_t scratchBytes = quantized ? std::max(qBytes, quantWorkBytes) : qBytes;
    const uint64_t inputBytes = quantized ? data.quantCount * 2U : data.packedPayloadBytes;
    const uint64_t countRecords = (uint64_t(experts) + kEpServerDedupCoreNum - 1U) / kEpServerDedupCoreNum;
    const uint64_t reservedBytes = bs < kEpServerDedupAllCoreCountBsThreshold ?
        TileXREpDedupAlign(countRecords, 8U) * kEpServerDedupCountRecordBytes : 0U;
    const uint64_t maxGroups = std::min(uint64_t(rankSize / 8U - 1U), uint64_t(topK / 2U));
    return reservedBytes + 2U * inputBytes + TileXREpDedupAlign(routes, 256U) +
        2U * scratchBytes + qBytes + (quantized ? data.packedPayloadBytes : 0U) +
        2U * TileXREpDedupAlign(tokens * topK * sizeof(float), 32U) +
        TileXREpDedupAlign(tokens * sizeof(uint32_t), 32U) +
        TileXREpDedupAlign(tokens * topK * 16U, 32U) + 64U +
        TileXREpDedupAlign(maxGroups * sizeof(uint32_t), 32U) + data.tokenStride;
}

} // namespace

int TileXREpBuildMemoryServerDedupLayout(int64_t rankSize, int64_t bs, int64_t h,
    int64_t topK, int64_t moeExpertNum, EpMemoryServerDedupLayout *out, bool quantized, int64_t sendCoreNum)
{
    if (out == nullptr) {
        return TileXR::TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    *out = EpMemoryServerDedupLayout {};
    if (!TileXREpIsServerDedupSendCoreNum(sendCoreNum) || rankSize < 16 || rankSize > TileXR::TILEXR_MAX_RANK_SIZE ||
        rankSize % kEpServerDedupRanksPerServer != 0 || bs <= 0 || h <= 0 || h > UINT32_MAX ||
        topK <= 0 || topK > kEpServerDedupMaxTopK || moeExpertNum <= 0 ||
        moeExpertNum > kEpServerDedupMaxExperts || topK > moeExpertNum ||
        moeExpertNum % rankSize != 0 || bs > UINT32_MAX / topK) {
        return TileXR::TILEXR_ERROR_PARA_CHECK_FAIL;
    }

    const uint32_t recvCoreNum = kEpServerDedupCoreNum - static_cast<uint32_t>(sendCoreNum);
    const uint32_t countCores = bs < kEpServerDedupAllCoreCountBsThreshold ?
        kEpServerDedupCoreNum : recvCoreNum;
    if (TileXREpServerDedupCountSendUbBytes(uint64_t(bs) * topK, uint32_t(moeExpertNum),
        countCores) > kEpServerDedupCountUbBytes) {
        return TileXR::TILEXR_ERROR_PARA_CHECK_FAIL;
    }

    EpMemoryServerDedupLayout next {};
    next.globalBs = static_cast<uint64_t>(rankSize * bs);
    next.localExpertNum = static_cast<uint64_t>(moeExpertNum / rankSize);
    next.countStateBytes = TileXREpServerDedupCountLayout(uint32_t(moeExpertNum), recvCoreNum).stateBytes;
    next.data = TileXREpServerDedupDataLayout(uint32_t(bs), uint32_t(h), uint32_t(topK), uint32_t(moeExpertNum), quantized);
    if (next.countStateBytes > kEpServerDedupControlBytes ||
        next.data.requiredBytes > kEpServerDedupWindowUsableBytes ||
        SenderMinimumUbBytes(rankSize, bs, topK, moeExpertNum, sendCoreNum, quantized, next.data) >
            kEpServerDedupCountUbBytes) {
        return TileXR::TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    const uint64_t logicalGroupBytes = kEpServerDedupGroupMembersOffset +
        static_cast<uint64_t>(topK - 1) * sizeof(EpServerDedupGroupMember);
    next.groupRowStrideBytes = (logicalGroupBytes + 31U) / 32U * 32U;
    uint64_t rowBytes = 0;
    uint64_t routeCount = 0;
    if (!MultiplyBytes(next.globalBs, std::min<uint64_t>(topK, next.localExpertNum), &next.capacityRows) ||
        !MultiplyBytes(h, sizeof(uint16_t), &rowBytes) ||
        !MultiplyBytes(bs, rowBytes, &next.xBytes) ||
        !MultiplyBytes(bs, topK, &routeCount) ||
        !MultiplyBytes(routeCount, sizeof(int32_t), &next.expertIdsBytes) ||
        !MultiplyBytes(routeCount, sizeof(float), &next.weightBytes) ||
        !MultiplyBytes(next.capacityRows, quantized ? uint64_t(h) : rowBytes, &next.expandXBytes) ||
        !MultiplyBytes(next.capacityRows, next.data.scaleBytes, &next.dynamicScalesBytes) ||
        !MultiplyBytes(next.localExpertNum, sizeof(int64_t), &next.expertTokenNumsBytes) ||
        !MultiplyBytes(moeExpertNum, sizeof(int32_t), &next.sendCountsBytes) ||
        !MultiplyBytes(next.capacityRows, sizeof(EpServerDedupAssistEx), &next.assistExBytes) ||
        !MultiplyBytes(bs, sizeof(uint32_t), &next.sourceMaskBytes) ||
        // Sparse first-hop indices use the full Dispatch output-row capacity.
        !MultiplyBytes(next.capacityRows, next.groupRowStrideBytes, &next.groupBytes)) {
        return TileXR::TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    *out = next;
    return TileXR::TILEXR_SUCCESS;
}

int TileXREpValidateServerDedupAllocations(const EpServerDedupAllocation *allocations,
    std::size_t count, int32_t deviceId)
{
    if (allocations == nullptr || count == 0 || deviceId < 0) {
        return TileXR::TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    for (std::size_t i = 0; i < count; ++i) {
        const auto &allocation = allocations[i];
        const uintptr_t begin = reinterpret_cast<uintptr_t>(allocation.data);
        if (allocation.data == nullptr || allocation.requiredBytes == 0 ||
            allocation.bytes < allocation.requiredBytes || allocation.deviceId != deviceId ||
            allocation.alignment == 0 || (allocation.alignment & (allocation.alignment - 1U)) != 0 ||
            begin % allocation.alignment != 0 ||
            allocation.bytes > std::numeric_limits<uintptr_t>::max() - begin) {
            return TileXR::TILEXR_ERROR_PARA_CHECK_FAIL;
        }
        const uintptr_t end = begin + allocation.bytes;
        for (std::size_t j = 0; j < i; ++j) {
            const uintptr_t otherBegin = reinterpret_cast<uintptr_t>(allocations[j].data);
            const uintptr_t otherEnd = otherBegin + allocations[j].bytes;
            if (begin < otherEnd && otherBegin < end) {
                return TileXR::TILEXR_ERROR_PARA_CHECK_FAIL;
            }
        }
    }
    return TileXR::TILEXR_SUCCESS;
}

} // namespace TileXREp
