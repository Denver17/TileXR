/*
 * 计算 MoonEP payload、scratch 与控制区的有界工作区布局，供 Host 参数校验与 kernel launch 使用。
 */
#include "dispatch_layout.h"

#include <cstddef>
#include <limits>

#include "comm_args.h"
#include "../common/dispatch_credit.h"
#include "../common/dispatch_diagnostics.h"
#include "../common/dispatch_wqe_batch.h"
#include "tilexr_types.h"
#include "tilexr_udma_types.h"

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

bool AppendBytes(uint64_t bytes, uint64_t *cursor, uint64_t *offset)
{
    if (cursor == nullptr || offset == nullptr) {
        return false;
    }
    const uint64_t aligned = TileXRMoonEpDispatchUrmaAlignUp(
        *cursor, kDispatchInternalAlignmentBytes);
    uint64_t end = 0;
    if (aligned == std::numeric_limits<uint64_t>::max() ||
        !CheckedAdd(aligned, bytes, &end)) {
        return false;
    }
    *offset = aligned;
    *cursor = end;
    return true;
}

bool BuildActiveLayout(uint64_t sourceRows, uint64_t destinationCapacity,
    uint64_t rowBytes, uint64_t *cursor, DispatchUrmaActiveLayout *out)
{
    if (cursor == nullptr || out == nullptr || sourceRows == 0 ||
        destinationCapacity == 0 || rowBytes == 0) {
        return false;
    }
    DispatchUrmaActiveLayout next {};
    const uint64_t begin = *cursor;
    next.rowBytes = rowBytes;
    if (!CheckedMul(sourceRows, rowBytes, &next.sourceBytes) ||
        !AppendBytes(next.sourceBytes, cursor, &next.sourceOffset) ||
        !CheckedMul(destinationCapacity, rowBytes, &next.scratchSlotBytes) ||
        !CheckedMul(next.scratchSlotBytes, kDispatchScratchBufferCount,
            &next.scratchBytes) ||
        !AppendBytes(next.scratchBytes, cursor, &next.scratchOffset)) {
        return false;
    }
    next.activeDataBytes = *cursor - begin;
    *out = next;
    return true;
}

} // namespace

uint64_t TileXRMoonEpDispatchUrmaAlignUp(uint64_t value, uint64_t alignment)
{
    if (alignment == 0 || value > std::numeric_limits<uint64_t>::max() - (alignment - 1U)) {
        return std::numeric_limits<uint64_t>::max();
    }
    return ((value + alignment - 1U) / alignment) * alignment;
}

int TileXRMoonEpBuildDispatchUrmaLayout(int64_t rankSize, int64_t s, int64_t k,
    int64_t h, int64_t destinationCapacity, MoonEpDispatchUrmaLayout *out)
{
    if (out == nullptr || rankSize <= 0 || rankSize > TileXR::TILEXR_MAX_RANK_SIZE ||
        s <= 0 || k <= 0 || h <= 0 || destinationCapacity <= 0) {
        return TileXR::TILEXR_ERROR_PARA_CHECK_FAIL;
    }

    uint64_t routeCount = 0;
    uint64_t hiddenRowBytes = 0;
    uint64_t encodedCapacity = 0;
    if (!CheckedMul(static_cast<uint64_t>(s), static_cast<uint64_t>(k), &routeCount) ||
        routeCount == 0 || routeCount > static_cast<uint64_t>(INT64_MAX) ||
        static_cast<uint64_t>(destinationCapacity) < routeCount ||
        static_cast<uint64_t>(destinationCapacity) > UINT32_MAX ||
        !CheckedMul(static_cast<uint64_t>(h), sizeof(uint16_t), &hiddenRowBytes) ||
        hiddenRowBytes == 0 || hiddenRowBytes > static_cast<uint64_t>(UINT32_MAX) ||
        !CheckedMul(static_cast<uint64_t>(rankSize),
            static_cast<uint64_t>(destinationCapacity), &encodedCapacity) ||
        encodedCapacity > static_cast<uint64_t>(INT32_MAX) + 1ULL) {
        return TileXR::TILEXR_ERROR_PARA_CHECK_FAIL;
    }

    MoonEpDispatchUrmaLayout next {};
    uint64_t cursor = 0;
    if (!BuildActiveLayout(static_cast<uint64_t>(s),
            static_cast<uint64_t>(destinationCapacity), hiddenRowBytes,
            &cursor, &next.hidden) ||
        !BuildActiveLayout(routeCount, static_cast<uint64_t>(destinationCapacity),
            sizeof(float), &cursor, &next.weight)) {
        return TileXR::TILEXR_ERROR_PARA_CHECK_FAIL;
    }

    next.commonOffset = TileXRMoonEpDispatchUrmaAlignUp(
        cursor, kDispatchInternalAlignmentBytes);
    cursor = next.commonOffset;
    next.creditBytes = kDispatchCreditBytes;
    next.creditSourceBytes = kDispatchCreditSourceBytes;
    if (next.commonOffset == std::numeric_limits<uint64_t>::max() ||
        !CheckedMul(kDispatchMaxDesignRankCount * kDispatchQpCount, sizeof(uint64_t),
            &next.completionFlagsBytes) ||
        !AppendBytes(next.completionFlagsBytes, &cursor, &next.completionFlagsOffset) ||
        !AppendBytes(next.creditBytes, &cursor, &next.creditOffset) ||
        !AppendBytes(next.creditSourceBytes, &cursor, &next.creditSourceOffset) ||
        !CheckedMul(kDispatchAivCoreCount, kDispatchSignalStrideBytes,
            &next.signalBytes) ||
        !AppendBytes(next.signalBytes, &cursor, &next.signalOffset) ||
        !CheckedMul(kDispatchDfxRecordCount, sizeof(DispatchDfxRecord),
            &next.dfxBytes) ||
        !AppendBytes(next.dfxBytes, &cursor, &next.hiddenDfxOffset) ||
        !AppendBytes(next.dfxBytes, &cursor, &next.weightDfxOffset)) {
        return TileXR::TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    next.kernelStatusBytes = sizeof(DispatchKernelStatus);
    if (!AppendBytes(next.kernelStatusBytes, &cursor, &next.kernelStatusOffset)) {
        return TileXR::TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    next.requiredBytes = cursor;
    next.totalBytes = TileXRMoonEpDispatchUrmaAlignUp(
        cursor, kDispatchRegistrationAlignmentBytes);
    if (next.totalBytes == std::numeric_limits<uint64_t>::max() ||
        next.totalBytes > static_cast<uint64_t>(std::numeric_limits<std::size_t>::max())) {
        return TileXR::TILEXR_ERROR_PARA_CHECK_FAIL;
    }

    const uint64_t commonBytes = cursor - next.commonOffset;
    const uint64_t boundCommonOffset = next.totalBytes - commonBytes;
    const uint64_t commonShift = boundCommonOffset - next.commonOffset;
    next.commonOffset = boundCommonOffset;
    next.completionFlagsOffset += commonShift;
    next.creditOffset += commonShift;
    next.creditSourceOffset += commonShift;
    next.signalOffset += commonShift;
    next.hiddenDfxOffset += commonShift;
    next.weightDfxOffset += commonShift;
    next.kernelStatusOffset += commonShift;

    next.rankSize = rankSize;
    next.s = s;
    next.k = k;
    next.h = h;
    next.routeCount = static_cast<int64_t>(routeCount);
    next.destinationCapacity = destinationCapacity;
    *out = next;
    return TileXR::TILEXR_SUCCESS;
}

int TileXRMoonEpBindDispatchUrmaWorkspace(uint64_t workspaceBytes,
    MoonEpDispatchUrmaLayout *layout)
{
    if (layout == nullptr || workspaceBytes == 0 ||
        workspaceBytes % kDispatchRegistrationAlignmentBytes != 0 ||
        layout->totalBytes == 0 || workspaceBytes < layout->totalBytes) {
        return TileXR::TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    const uint64_t commonBytes = layout->totalBytes - layout->commonOffset;
    const uint64_t nextCommonOffset = workspaceBytes - commonBytes;
    if (nextCommonOffset < layout->commonOffset) {
        return TileXR::TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    const uint64_t shift = nextCommonOffset - layout->commonOffset;
    layout->commonOffset = nextCommonOffset;
    layout->completionFlagsOffset += shift;
    layout->creditOffset += shift;
    layout->creditSourceOffset += shift;
    layout->signalOffset += shift;
    layout->hiddenDfxOffset += shift;
    layout->weightDfxOffset += shift;
    layout->kernelStatusOffset += shift;
    layout->totalBytes = workspaceBytes;
    return TileXR::TILEXR_SUCCESS;
}

const DispatchUrmaActiveLayout *TileXRMoonEpGetActiveDispatchUrmaLayout(
    const MoonEpDispatchUrmaLayout &layout, DispatchPayloadMode mode)
{
    switch (mode) {
        case DispatchPayloadMode::Hidden:
            return &layout.hidden;
        case DispatchPayloadMode::RouteWeight:
            return &layout.weight;
    }
    return nullptr;
}

} // namespace TileXRMoonEp
