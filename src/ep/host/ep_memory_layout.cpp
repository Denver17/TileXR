#include "ep_memory_layout.h"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <limits>

#include "comm_args.h"
#include "ep_layout.h"

namespace TileXREp {
namespace {

bool MulInt64(int64_t lhs, int64_t rhs, int64_t *out)
{
    if (out == nullptr || lhs < 0 || rhs < 0 || (lhs != 0 && rhs > std::numeric_limits<int64_t>::max() / lhs)) {
        return false;
    }
    *out = lhs * rhs;
    return true;
}

bool AddInt64(int64_t lhs, int64_t rhs, int64_t *out)
{
    if (out == nullptr || lhs < 0 || rhs < 0 || rhs > std::numeric_limits<int64_t>::max() - lhs) {
        return false;
    }
    *out = lhs + rhs;
    return true;
}

bool AlignUpInt64(int64_t value, int64_t alignment, int64_t *out)
{
    if (out == nullptr) {
        return false;
    }
    const int64_t aligned = TileXREpAlignUp(value, alignment);
    if (aligned == TileXR::TILEXR_INVALID_VALUE) {
        return false;
    }
    *out = aligned;
    return true;
}

bool CeilDivInt64(int64_t value, int64_t divisor, int64_t *out)
{
    if (out == nullptr || value < 0 || divisor <= 0) {
        return false;
    }
    *out = value / divisor + (value % divisor == 0 ? 0 : 1);
    return true;
}

} // namespace

int TileXREpResolveMemoryBlockDim(uint32_t vectorCoreNum, uint32_t minimumBlockDim,
    const char *overrideValue, uint32_t *blockDim)
{
    if (blockDim == nullptr || minimumBlockDim == 0U || vectorCoreNum < minimumBlockDim ||
        vectorCoreNum > kEpMemoryMaxVectorCoreNum) {
        return TileXR::TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    if (overrideValue == nullptr || overrideValue[0] == '\0') {
        *blockDim = vectorCoreNum;
        return TileXR::TILEXR_SUCCESS;
    }
    for (const char *cursor = overrideValue; *cursor != '\0'; ++cursor) {
        if (*cursor < '0' || *cursor > '9') {
            return TileXR::TILEXR_ERROR_PARA_CHECK_FAIL;
        }
    }

    errno = 0;
    char *end = nullptr;
    const unsigned long long parsed = std::strtoull(overrideValue, &end, 10);
    if (errno != 0 || end == overrideValue || end == nullptr || *end != '\0' ||
        parsed < minimumBlockDim || parsed > vectorCoreNum || parsed > kEpMemoryMaxVectorCoreNum) {
        return TileXR::TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    *blockDim = static_cast<uint32_t>(parsed);
    return TileXR::TILEXR_SUCCESS;
}

uint32_t TileXREpMemoryCountCoreNum(int64_t totalExpertNum, int64_t rscvStatusNum, uint32_t blockDim)
{
    if (totalExpertNum <= 0 || rscvStatusNum <= 0 || blockDim < 2) {
        return 0;
    }
    uint64_t count = static_cast<uint64_t>(totalExpertNum / 16);
    count = std::max<uint64_t>(count, 1);
    count = std::min<uint64_t>(count, blockDim / 2);
    count = std::min<uint64_t>(count, kEpMemoryMaxCountCoreNum);
    count = std::min<uint64_t>(count, static_cast<uint64_t>(rscvStatusNum));
    return static_cast<uint32_t>(count);
}

int TileXREpBuildMemoryDispatchReferenceConfig(int64_t rankSize, int64_t rank, int64_t bs, int64_t h,
    int64_t topK, int64_t moeExpertNum, int64_t sharedExpertNum, int64_t sharedExpertRankNum,
    int64_t globalBs, TileXR::TileXRDataType dtype, TileXR::TileXRDataType expandXOutDtype,
    int64_t quantMode, uint32_t blockDim,
    EpMemoryDispatchReferenceConfig *out)
{
    const int64_t moeRankNum = rankSize - sharedExpertRankNum;
    const bool useMxfp8 = quantMode == 4;
    const bool validOutputType = useMxfp8 ?
        (expandXOutDtype == TileXR::TILEXR_DATA_TYPE_FP8E4M3 ||
            expandXOutDtype == TileXR::TILEXR_DATA_TYPE_FP8E5M2) : expandXOutDtype == dtype;
    if (out == nullptr || rankSize <= 0 || rankSize > TileXR::TILEXR_MAX_RANK_SIZE || rank < 0 ||
        rank >= rankSize || bs <= 0 || h <= 0 || topK <= 0 || moeExpertNum <= 0 || sharedExpertNum < 0 ||
        sharedExpertRankNum < 0 || globalBs <= 0 || globalBs % rankSize != 0 || blockDim < 2 ||
        blockDim > kEpMemoryMaxVectorCoreNum ||
        (dtype != TileXR::TILEXR_DATA_TYPE_FP16 && dtype != TileXR::TILEXR_DATA_TYPE_BFP16) ||
        (quantMode != 0 && quantMode != 4) || !validOutputType ||
        ((sharedExpertNum == 0) != (sharedExpertRankNum == 0)) || sharedExpertRankNum >= rankSize ||
        (sharedExpertNum > 0 && sharedExpertRankNum % sharedExpertNum != 0) || moeRankNum <= 0 ||
        moeExpertNum % moeRankNum != 0) {
        return TileXR::TILEXR_ERROR_PARA_CHECK_FAIL;
    }

    EpMemoryDispatchReferenceConfig next {};
    next.blockDim = blockDim;
    const int64_t moeExpertNumPerRank = moeExpertNum / moeRankNum;
    const bool isSharedExpertRank = rank < sharedExpertRankNum;
    next.localExpertNum = isSharedExpertRank ? 1 : moeExpertNumPerRank;
    next.rscvStatusNum = isSharedExpertRank ? rankSize : rankSize * moeExpertNumPerRank;

    int64_t totalExpertNum = 0;
    if (!AddInt64(sharedExpertRankNum, moeExpertNum, &totalExpertNum)) {
        return TileXR::TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    const bool isSmallBs = static_cast<uint64_t>(bs) < kEpMemorySmallBsThreshold;
    const bool useAllCoreCount = isSmallBs;
    if (useAllCoreCount) {
        uint64_t count = static_cast<uint64_t>(next.rscvStatusNum) /
            kEpMemoryCumsumStatusPerCore;
        if (static_cast<uint64_t>(next.rscvStatusNum) %
            kEpMemoryCumsumStatusPerCore != 0U) {
            ++count;
        }
        count = std::max<uint64_t>(count, 1U);
        count = std::min<uint64_t>(count, blockDim / 2U);
        count = std::min<uint64_t>(count, kEpMemoryMaxCountCoreNum);
        count = std::min<uint64_t>(count, static_cast<uint64_t>(next.rscvStatusNum));
        next.aivUsedCumSum = static_cast<uint32_t>(count);
    } else {
        next.aivUsedCumSum = TileXREpMemoryCountCoreNum(
            totalExpertNum, next.rscvStatusNum, blockDim);
    }
    if (next.aivUsedCumSum == 0 || next.aivUsedCumSum >= blockDim) {
        return TileXR::TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    next.aivUsedAllToAll = blockDim - next.aivUsedCumSum;
    if (sharedExpertRankNum > 0) {
        const int64_t routeKinds = topK + sharedExpertNum;
        next.sharedUsedAivNum = static_cast<uint32_t>(
            (static_cast<uint64_t>(next.aivUsedAllToAll) * static_cast<uint64_t>(sharedExpertNum)) /
            static_cast<uint64_t>(routeKinds));
        if (next.sharedUsedAivNum == 0) {
            next.sharedUsedAivNum = 1;
        }
    }
    if (next.sharedUsedAivNum >= next.aivUsedAllToAll) {
        return TileXR::TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    next.moeUsedAivNum = next.aivUsedAllToAll - next.sharedUsedAivNum;

    const int64_t dtypeBytes = TileXREpDataTypeSize(dtype);
    const int64_t outputDtypeBytes = useMxfp8 ? 1 : dtypeBytes;
    int64_t inputBytes = 0;
    int64_t hOutBytes = 0;
    int64_t tokenPayloadBytes = 0;
    int64_t payloadAndScaleBytes = 0;
    int64_t hOutSizeAlign = 0;
    int64_t blockCntPerToken = 0;
    int64_t packedPayloadBytes = 0;
    int64_t scaleBlockCount = 0;
    if (!MulInt64(h, dtypeBytes, &inputBytes) ||
        !MulInt64(h, outputDtypeBytes, &hOutBytes) ||
        !AlignUpInt64(hOutBytes, useMxfp8 ? 256 : kEpMemoryWindowAlignmentBytes, &tokenPayloadBytes)) {
        return TileXR::TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    if (useMxfp8 && (!CeilDivInt64(h, 32, &scaleBlockCount) ||
        !AlignUpInt64(scaleBlockCount, 2, &next.scaleOutBytes))) {
        return TileXR::TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    if (!AddInt64(tokenPayloadBytes, next.scaleOutBytes, &payloadAndScaleBytes) ||
        !AlignUpInt64(payloadAndScaleBytes, kEpMemoryWindowAlignmentBytes, &next.tokenQuantAlignBytes) ||
        !AddInt64(next.tokenQuantAlignBytes, kEpMemoryWindowAlignmentBytes, &hOutSizeAlign) ||
        !CeilDivInt64(hOutSizeAlign, kEpMemorySplitPayloadBytes, &blockCntPerToken) ||
        !MulInt64(blockCntPerToken, kEpMemorySplitPayloadBytes, &packedPayloadBytes) ||
        !MulInt64(blockCntPerToken, kEpMemorySplitBlockBytes, &next.hCommuSize)) {
        return TileXR::TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    next.hOutSize = hOutBytes;

    const int64_t axisMaxBs = globalBs / rankSize;
    if (!MulInt64(axisMaxBs, next.hCommuSize, &next.expertPerSizeOnWin)) {
        return TileXR::TILEXR_ERROR_PARA_CHECK_FAIL;
    }

    int64_t combinePayloadBytes = 0;
    int64_t combineBlockCount = 0;
    int64_t combineRowBytes = 0;
    int64_t routesPerToken = 0;
    int64_t combineRows = 0;
    if (!MulInt64(h, dtypeBytes, &combinePayloadBytes) ||
        !CeilDivInt64(combinePayloadBytes, kEpMemorySplitPayloadBytes, &combineBlockCount) ||
        !MulInt64(combineBlockCount, kEpMemorySplitBlockBytes, &combineRowBytes) ||
        !AddInt64(topK, sharedExpertNum, &routesPerToken) ||
        !MulInt64(axisMaxBs, routesPerToken, &combineRows) ||
        !MulInt64(combineRows, combineRowBytes, &next.combineReserveBytes)) {
        return TileXR::TILEXR_ERROR_PARA_CHECK_FAIL;
    }

    int64_t workspaceRawBytes = 0;
    if (!MulInt64(static_cast<int64_t>(blockDim), next.rscvStatusNum, &workspaceRawBytes) ||
        !MulInt64(workspaceRawBytes, static_cast<int64_t>(sizeof(int32_t)), &workspaceRawBytes) ||
        !AlignUpInt64(workspaceRawBytes, kEpMemoryWindowAlignmentBytes, &next.workspaceBytes)) {
        return TileXR::TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    next.roundPayloadBytes = TileXR::IPC_BUFF_MAX_SIZE - kEpMemoryStateWindowBytes;
    if (next.roundPayloadBytes <= 0 ||
        next.workspaceBytes > static_cast<int64_t>(kEpMemoryDispatchDfxOffset)) {
        return TileXR::TILEXR_ERROR_PARA_CHECK_FAIL;
    }

    int64_t dispatchDataBytes = 0;
    int64_t requiredRoundBytes = 0;
    int64_t statusBytes = 0;
    const int64_t workspaceStartOffset =
        static_cast<int64_t>(kEpMemoryDispatchDfxOffset) - next.workspaceBytes;
    if (!MulInt64(next.rscvStatusNum, next.expertPerSizeOnWin, &dispatchDataBytes) ||
        !AddInt64(next.combineReserveBytes, dispatchDataBytes, &requiredRoundBytes) ||
        !MulInt64(next.rscvStatusNum, kEpMemoryWindowAlignmentBytes, &statusBytes) ||
        statusBytes > workspaceStartOffset || requiredRoundBytes > next.roundPayloadBytes) {
        return TileXR::TILEXR_ERROR_PARA_CHECK_FAIL;
    }

    int64_t expertIdsBytes = 0;
    int64_t expertIdsAligned = 0;
    if (!MulInt64(bs, topK, &expertIdsBytes) ||
        !MulInt64(expertIdsBytes, static_cast<int64_t>(sizeof(int32_t)), &expertIdsBytes) ||
        !AlignUpInt64(expertIdsBytes, kEpMemoryWindowAlignmentBytes, &expertIdsAligned)) {
        return TileXR::TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    next.maxSizeForUbBuffer = expertIdsAligned;
    if (useMxfp8) {
        int64_t alignedScaleCount = 0;
        int64_t quantWorkBytes = 0;
        if (!AlignUpInt64(next.scaleOutBytes, 32, &alignedScaleCount) ||
            !MulInt64(alignedScaleCount, static_cast<int64_t>(sizeof(float)), &quantWorkBytes) ||
            !AddInt64(quantWorkBytes, next.scaleOutBytes * static_cast<int64_t>(sizeof(uint16_t)),
                &quantWorkBytes) ||
            !AlignUpInt64(quantWorkBytes, kEpMemoryWindowAlignmentBytes, &quantWorkBytes)) {
            return TileXR::TILEXR_ERROR_PARA_CHECK_FAIL;
        }
        next.maxSizeForUbBuffer = std::max(next.maxSizeForUbBuffer, quantWorkBytes);
    }
    next.totalUbSize = kEpMemoryFullUbBytes;
    if (next.maxSizeForUbBuffer <= 0 || next.maxSizeForUbBuffer >= next.totalUbSize) {
        return TileXR::TILEXR_ERROR_PARA_CHECK_FAIL;
    }

    int64_t hAlignBytes = 0;
    int64_t inputBuffersPerSlotBytes = 0;
    int64_t inputPoolBytes = 0;
    int64_t expertIdsVecBytes = 0;
    int64_t senderStatusCount = 0;
    int64_t senderStatusBytes = 0;
    int64_t allToAllUbBytes = 2 * kEpMemoryWindowAlignmentBytes;
    int64_t term = 0;
    if ((useAllCoreCount &&
            (!CeilDivInt64(totalExpertNum, blockDim, &senderStatusCount) ||
                !AlignUpInt64(senderStatusCount, 8, &senderStatusCount) ||
                !MulInt64(senderStatusCount, kEpMemoryWindowAlignmentBytes, &senderStatusBytes))) ||
        !AlignUpInt64(inputBytes, useMxfp8 ? 512 : kEpMemoryWindowAlignmentBytes, &hAlignBytes) ||
        !AddInt64(useMxfp8 ? hAlignBytes : packedPayloadBytes, hAlignBytes, &inputBuffersPerSlotBytes) ||
        !MulInt64(2, inputBuffersPerSlotBytes, &inputPoolBytes) ||
        !AlignUpInt64(expertIdsBytes, 256, &expertIdsVecBytes) ||
        !AddInt64(allToAllUbBytes, inputPoolBytes, &allToAllUbBytes) ||
        !MulInt64(2, expertIdsVecBytes, &term) || !AddInt64(allToAllUbBytes, term, &allToAllUbBytes) ||
        !MulInt64(2, next.maxSizeForUbBuffer, &term) || !AddInt64(allToAllUbBytes, term, &allToAllUbBytes) ||
        !AddInt64(allToAllUbBytes, expertIdsAligned, &allToAllUbBytes) ||
        (useMxfp8 && !AddInt64(allToAllUbBytes, packedPayloadBytes, &allToAllUbBytes)) ||
        !AddInt64(allToAllUbBytes, senderStatusBytes, &allToAllUbBytes) ||
        !AddInt64(allToAllUbBytes, next.hCommuSize, &allToAllUbBytes) ||
        allToAllUbBytes > next.totalUbSize) {
        return TileXR::TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    next.allToAllUbBytes = allToAllUbBytes;

    const int64_t recvStatusPerCore = next.rscvStatusNum / next.aivUsedCumSum +
        (next.rscvStatusNum % next.aivUsedCumSum == 0 ? 0 : 1);
    int64_t waitStatusBytes = 0;
    int64_t recvStatusScalarBytes = 0;
    int64_t sumContinueBytes = 0;
    int64_t tokenNumBytes = 0;
    int64_t workLocalBytes = 0;
    int64_t statusCountAligned = 0;
    int64_t countUbBytes = kEpMemoryWindowAlignmentBytes;
    if (!MulInt64(recvStatusPerCore, kEpMemoryWindowAlignmentBytes, &term) ||
        !AlignUpInt64(term, 256, &waitStatusBytes) ||
        !MulInt64(recvStatusPerCore, static_cast<int64_t>(sizeof(float)), &term) ||
        !AlignUpInt64(term, kEpMemoryWindowAlignmentBytes, &recvStatusScalarBytes) ||
        !MulInt64(blockDim, static_cast<int64_t>(sizeof(float)), &term) ||
        !AlignUpInt64(term, kEpMemoryWindowAlignmentBytes, &sumContinueBytes) ||
        !MulInt64(moeExpertNumPerRank, static_cast<int64_t>(sizeof(int64_t)), &term) ||
        !AlignUpInt64(term, kEpMemoryWindowAlignmentBytes, &tokenNumBytes) ||
        !MulInt64(rankSize, static_cast<int64_t>(sizeof(float)), &term) ||
        !AlignUpInt64(term, kEpMemoryWindowAlignmentBytes, &workLocalBytes) ||
        ((useAllCoreCount &&
            (!CeilDivInt64(totalExpertNum, blockDim, &statusCountAligned) ||
                !AlignUpInt64(statusCountAligned, 8, &statusCountAligned))) ||
            (!useAllCoreCount && !AlignUpInt64(totalExpertNum, 8, &statusCountAligned))) ||
        !MulInt64(2, next.maxSizeForUbBuffer, &term) || !AddInt64(countUbBytes, term, &countUbBytes) ||
        !MulInt64(2, expertIdsVecBytes, &term) || !AddInt64(countUbBytes, term, &countUbBytes) ||
        !MulInt64(statusCountAligned, kEpMemoryWindowAlignmentBytes, &term) ||
        !AddInt64(countUbBytes, term, &countUbBytes) ||
        !AddInt64(countUbBytes, waitStatusBytes, &countUbBytes) ||
        !AddInt64(countUbBytes, recvStatusScalarBytes, &countUbBytes) ||
        !MulInt64(recvStatusPerCore, kEpMemoryWindowAlignmentBytes, &term) ||
        !AddInt64(countUbBytes, term, &countUbBytes) ||
        !MulInt64(2 * static_cast<int64_t>(blockDim), kEpMemoryWindowAlignmentBytes, &term) ||
        !AddInt64(countUbBytes, term, &countUbBytes) ||
        !AddInt64(countUbBytes, sumContinueBytes, &countUbBytes) ||
        !AddInt64(countUbBytes, 3 * kEpMemoryWindowAlignmentBytes, &countUbBytes) ||
        !MulInt64(next.rscvStatusNum, kEpMemoryWindowAlignmentBytes, &term) ||
        !AddInt64(countUbBytes, term, &countUbBytes) ||
        !AddInt64(countUbBytes, tokenNumBytes, &countUbBytes) ||
        !AddInt64(countUbBytes, workLocalBytes, &countUbBytes) || countUbBytes > next.totalUbSize) {
        return TileXR::TILEXR_ERROR_PARA_CHECK_FAIL;
    }

    int64_t receiveCountBytes = 0;
    int64_t sourceInfoBytes = 0;
    int64_t localCopyFixedBytes = kEpMemoryWindowAlignmentBytes;
    int64_t receiverTokenBufferBytes = 0;
    int64_t reduceWorkBytes = 0;
    const int64_t statesPerAiv = next.rscvStatusNum / blockDim +
        (next.rscvStatusNum % blockDim == 0 ? 0 : 1);
    const int64_t blockCountPerToken = next.hCommuSize / kEpMemorySplitBlockBytes;
    if (!MulInt64(next.rscvStatusNum, static_cast<int64_t>(sizeof(int32_t)), &term) ||
        !AlignUpInt64(term, kEpMemoryWindowAlignmentBytes, &receiveCountBytes) ||
        !MulInt64(statesPerAiv, static_cast<int64_t>(sizeof(uint32_t)), &term) ||
        !AlignUpInt64(term, kEpMemoryWindowAlignmentBytes, &sourceInfoBytes) ||
        !AddInt64(localCopyFixedBytes, receiveCountBytes, &localCopyFixedBytes) ||
        !MulInt64(2 * static_cast<int64_t>(next.aivUsedCumSum), kEpMemoryWindowAlignmentBytes, &term) ||
        !AddInt64(localCopyFixedBytes, term, &localCopyFixedBytes) ||
        !MulInt64(3, sourceInfoBytes, &term) || !AddInt64(localCopyFixedBytes, term, &localCopyFixedBytes) ||
        !MulInt64(blockCountPerToken, static_cast<int64_t>(sizeof(float)), &term) ||
        !AlignUpInt64(term, 256, &reduceWorkBytes) ||
        !MulInt64(2, packedPayloadBytes, &receiverTokenBufferBytes) ||
        !MulInt64(blockCountPerToken, kEpMemoryWindowAlignmentBytes, &term) ||
        !AddInt64(receiverTokenBufferBytes, term, &receiverTokenBufferBytes) ||
        !AddInt64(receiverTokenBufferBytes, reduceWorkBytes, &receiverTokenBufferBytes) ||
        localCopyFixedBytes >= next.totalUbSize ||
        next.totalUbSize - localCopyFixedBytes < receiverTokenBufferBytes) {
        return TileXR::TILEXR_ERROR_PARA_CHECK_FAIL;
    }

    *out = next;
    return TileXR::TILEXR_SUCCESS;
}

int TileXREpBuildMemoryCombineReferenceConfig(int64_t rankSize, int64_t rank, int64_t bs, int64_t h,
    int64_t topK, int64_t moeExpertNum, int64_t sharedExpertNum, int64_t sharedExpertRankNum,
    int64_t globalBs, TileXR::TileXRDataType dtype, int64_t quantMode, uint32_t blockDim,
    EpMemoryCombineReferenceConfig *out)
{
    const int64_t moeRankNum = rankSize - sharedExpertRankNum;
    const bool useMxfp8 = quantMode == 3 || quantMode == 4;
    if (out == nullptr || rankSize <= 0 || rankSize > TileXR::TILEXR_MAX_RANK_SIZE || rank < 0 ||
        rank >= rankSize || bs <= 0 || h <= 0 || topK <= 0 || moeExpertNum <= 0 || sharedExpertNum < 0 ||
        sharedExpertRankNum < 0 || globalBs <= 0 || globalBs % rankSize != 0 || blockDim == 0 ||
        blockDim > kEpMemoryMaxVectorCoreNum ||
        (dtype != TileXR::TILEXR_DATA_TYPE_FP16 && dtype != TileXR::TILEXR_DATA_TYPE_BFP16) ||
        (quantMode != 0 && !useMxfp8) ||
        ((sharedExpertNum == 0) != (sharedExpertRankNum == 0)) || sharedExpertRankNum >= rankSize ||
        (sharedExpertNum > 0 && sharedExpertRankNum % sharedExpertNum != 0) || moeRankNum <= 0 ||
        moeExpertNum % moeRankNum != 0) {
        return TileXR::TILEXR_ERROR_PARA_CHECK_FAIL;
    }

    EpMemoryCombineReferenceConfig next {};
    next.blockDim = blockDim;
    next.moeExpertNumPerRank = moeExpertNum / moeRankNum;

    const int64_t dtypeBytes = TileXREpDataTypeSize(dtype);
    int64_t rowBytes = 0;
    int64_t commDataBytes = 0;
    int64_t inputAlignBytes = 0;
    int64_t scaleCount = 0;
    int64_t quantComputeScaleCount = 0;
    if (!MulInt64(h, dtypeBytes, &rowBytes)) {
        return TileXR::TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    if (useMxfp8) {
        int64_t fp8PayloadBytes = 0;
        if (!AlignUpInt64(h, 256, &fp8PayloadBytes) ||
            !CeilDivInt64(h, 32, &scaleCount) ||
            !AlignUpInt64(scaleCount, 2, &scaleCount) ||
            !AddInt64(fp8PayloadBytes, scaleCount, &commDataBytes) ||
            !MulInt64(fp8PayloadBytes, dtypeBytes, &inputAlignBytes)) {
            return TileXR::TILEXR_ERROR_PARA_CHECK_FAIL;
        }
        quantComputeScaleCount = fp8PayloadBytes / 32;
    } else if (!AlignUpInt64(rowBytes, kEpMemoryWindowAlignmentBytes, &inputAlignBytes)) {
        return TileXR::TILEXR_ERROR_PARA_CHECK_FAIL;
    } else {
        commDataBytes = rowBytes;
    }
    if (!CeilDivInt64(commDataBytes, kEpMemorySplitPayloadBytes, &next.blockCntPerToken) ||
        next.blockCntPerToken > std::numeric_limits<uint8_t>::max() ||
        !MulInt64(next.blockCntPerToken, kEpMemorySplitBlockBytes, &next.packedRowBytes)) {
        return TileXR::TILEXR_ERROR_PARA_CHECK_FAIL;
    }

    int64_t routesPerToken = 0;
    int64_t combineRows = 0;
    if (!AddInt64(topK, sharedExpertNum, &routesPerToken) ||
        !MulInt64(globalBs / rankSize, routesPerToken, &combineRows) ||
        !MulInt64(combineRows, next.packedRowBytes, &next.combineReserveBytes)) {
        return TileXR::TILEXR_ERROR_PARA_CHECK_FAIL;
    }

    int64_t workspaceStatusNum = 0;
    int64_t workspaceBytes = 0;
    if (!MulInt64(rankSize, next.moeExpertNumPerRank, &workspaceStatusNum) ||
        !MulInt64(static_cast<int64_t>(blockDim), workspaceStatusNum, &workspaceBytes) ||
        !MulInt64(workspaceBytes, static_cast<int64_t>(sizeof(int32_t)), &workspaceBytes) ||
        !AlignUpInt64(workspaceBytes, kEpMemoryWindowAlignmentBytes, &next.workspaceBytes)) {
        return TileXR::TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    next.roundPayloadBytes = TileXR::IPC_BUFF_MAX_SIZE - kEpMemoryStateWindowBytes - next.workspaceBytes;
    if (next.roundPayloadBytes <= 0 || next.combineReserveBytes > next.roundPayloadBytes) {
        return TileXR::TILEXR_ERROR_PARA_CHECK_FAIL;
    }

    int64_t totalPackedBlocks = 0;
    int64_t flagFloatCount = 0;
    int64_t compareCount = 0;
    int64_t checkFlagBytes = 0;
    int64_t checkCompareBytes = 0;
    int64_t checkFlagsBytes = 0;
    int64_t compactPayloadBytes = 0;
    int64_t floatRowBytes = 0;
    int64_t alignedFloatRowBytes = 0;
    int64_t alignedMulRowBytes = 0;
    int64_t alignedOutputBytes = 0;
    int64_t receiveInputBytes = 0;
    int64_t sendInputBytes = 0;
    int64_t scaleBytes = 0;
    int64_t maskBytes = 0;
    int64_t clearFlagBytes = 0;
    int64_t tokenStatusBytes = 0;
    int64_t maxTokensPerCore = 0;
    int64_t receiveBytes = 4 * kEpMemoryWindowAlignmentBytes;
    int64_t term = 0;
    if (!MulInt64(routesPerToken, next.blockCntPerToken, &totalPackedBlocks) ||
        !MulInt64(totalPackedBlocks,
            kEpMemoryWindowAlignmentBytes / static_cast<int64_t>(sizeof(float)), &flagFloatCount) ||
        !AlignUpInt64(flagFloatCount, 64, &compareCount) ||
        !MulInt64(compareCount, static_cast<int64_t>(sizeof(float)), &checkFlagBytes) ||
        !MulInt64(compareCount, static_cast<int64_t>(sizeof(uint8_t)), &checkCompareBytes) ||
        !AlignUpInt64(checkCompareBytes, 256, &checkCompareBytes) ||
        !AddInt64(checkFlagBytes, checkCompareBytes, &checkFlagsBytes) ||
        !MulInt64(next.blockCntPerToken, kEpMemorySplitPayloadBytes, &compactPayloadBytes) ||
        !MulInt64(h, static_cast<int64_t>(sizeof(float)), &floatRowBytes) ||
        !AlignUpInt64(floatRowBytes, kEpMemoryWindowAlignmentBytes, &alignedFloatRowBytes) ||
        !AlignUpInt64(floatRowBytes, useMxfp8 ? 512 : 256, &alignedMulRowBytes) ||
        !AlignUpInt64(rowBytes, kEpMemoryWindowAlignmentBytes, &alignedOutputBytes)) {
        return TileXR::TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    receiveInputBytes = std::max(compactPayloadBytes, alignedOutputBytes);
    sendInputBytes = useMxfp8 ? inputAlignBytes : std::max(inputAlignBytes, compactPayloadBytes);
    if (!CeilDivInt64(bs, blockDim, &maxTokensPerCore) ||
        !MulInt64(maxTokensPerCore, topK, &term) ||
        !MulInt64(term, static_cast<int64_t>(sizeof(float)), &term) ||
        !AlignUpInt64(term, kEpMemoryWindowAlignmentBytes, &scaleBytes) ||
        !MulInt64(bs, static_cast<int64_t>(sizeof(bool)), &term) ||
        !AlignUpInt64(term, kEpMemoryWindowAlignmentBytes, &maskBytes) ||
        !MulInt64(maxTokensPerCore, static_cast<int64_t>(sizeof(int32_t)), &term) ||
        !AlignUpInt64(term, kEpMemoryWindowAlignmentBytes, &tokenStatusBytes) ||
        !MulInt64(next.blockCntPerToken, kEpMemoryWindowAlignmentBytes, &clearFlagBytes) ||
        !AddInt64(receiveBytes, checkFlagsBytes, &receiveBytes) ||
        !AddInt64(receiveBytes, alignedOutputBytes, &receiveBytes) ||
        !MulInt64(2, alignedFloatRowBytes, &term) || !AddInt64(receiveBytes, term, &receiveBytes) ||
        !AddInt64(receiveBytes, alignedMulRowBytes, &receiveBytes) ||
        !AddInt64(receiveBytes, scaleBytes, &receiveBytes) ||
        !AddInt64(receiveBytes, maskBytes, &receiveBytes) ||
        !AddInt64(receiveBytes, clearFlagBytes, &receiveBytes) ||
        !AddInt64(receiveBytes, tokenStatusBytes, &receiveBytes)) {
        return TileXR::TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    if (useMxfp8) {
        int64_t dequantScaleBytes = 0;
        if (!AlignUpInt64(scaleCount, 128, &dequantScaleBytes) ||
            !MulInt64(dequantScaleBytes, static_cast<int64_t>(sizeof(float)) * 2, &dequantScaleBytes) ||
            !AddInt64(receiveBytes, dequantScaleBytes, &receiveBytes)) {
            return TileXR::TILEXR_ERROR_PARA_CHECK_FAIL;
        }
    }
    int64_t doubleReceiveInputBytes = 0;
    int64_t doubleReceiveBytes = 0;
    if (!MulInt64(2, receiveInputBytes, &doubleReceiveInputBytes) ||
        !AddInt64(receiveBytes, doubleReceiveInputBytes, &doubleReceiveBytes)) {
        return TileXR::TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    if (doubleReceiveBytes <= kEpMemoryFullUbBytes) {
        next.receiveBufferNum = 2U;
        next.receiveUbBytes = doubleReceiveBytes;
    } else {
        next.receiveBufferNum = 1U;
        if (!AddInt64(receiveBytes, receiveInputBytes, &next.receiveUbBytes)) {
            return TileXR::TILEXR_ERROR_PARA_CHECK_FAIL;
        }
    }
    int64_t sendUbBytes = 4 * kEpMemoryWindowAlignmentBytes;
    if (!AddInt64(sendUbBytes, sendInputBytes, &sendUbBytes) ||
        !AddInt64(sendUbBytes, next.packedRowBytes, &sendUbBytes)) {
        return TileXR::TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    if (useMxfp8) {
        int64_t alignedScaleCount = 0;
        int64_t quantWorkBytes = 0;
        if (!AlignUpInt64(quantComputeScaleCount, 32, &alignedScaleCount) ||
            !MulInt64(alignedScaleCount, static_cast<int64_t>(sizeof(float)), &quantWorkBytes) ||
            !MulInt64(quantComputeScaleCount, static_cast<int64_t>(sizeof(uint16_t)), &term) ||
            !AddInt64(quantWorkBytes, term, &quantWorkBytes) ||
            !AlignUpInt64(quantWorkBytes, kEpMemoryWindowAlignmentBytes, &quantWorkBytes) ||
            !AddInt64(sendUbBytes, compactPayloadBytes, &sendUbBytes) ||
            !AddInt64(sendUbBytes, quantWorkBytes, &sendUbBytes)) {
            return TileXR::TILEXR_ERROR_PARA_CHECK_FAIL;
        }
    }
    next.sendUbBytes = sendUbBytes;
    if (sendUbBytes > kEpMemoryFullUbBytes || next.receiveUbBytes > kEpMemoryFullUbBytes) {
        return TileXR::TILEXR_ERROR_PARA_CHECK_FAIL;
    }

    *out = next;
    return TileXR::TILEXR_SUCCESS;
}

} // namespace TileXREp
