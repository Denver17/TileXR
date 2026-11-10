/*
 * 计算 MoonEP payload、scratch 与控制区的有界工作区布局，供 Host 参数校验与 kernel launch 使用。
 */
#ifndef TILEXR_MOONEP_DISPATCH_URMA_LAYOUT_H
#define TILEXR_MOONEP_DISPATCH_URMA_LAYOUT_H

#include <cstdint>

#include "../common/dispatch_common.h"

namespace TileXRMoonEp {

struct DispatchUrmaActiveLayout {
    uint64_t rowBytes = 0;
    uint64_t sourceOffset = 0;
    uint64_t sourceBytes = 0;
    uint64_t scratchOffset = 0;
    uint64_t scratchSlotBytes = 0;
    uint64_t scratchBytes = 0;
    uint64_t activeDataBytes = 0;
};

struct MoonEpDispatchUrmaLayout {
    int64_t rankSize = 0;
    int64_t s = 0;
    int64_t k = 0;
    int64_t h = 0;
    int64_t routeCount = 0;
    int64_t destinationCapacity = 0;
    DispatchUrmaActiveLayout hidden;
    DispatchUrmaActiveLayout weight;
    uint64_t commonOffset = 0;
    uint64_t completionFlagsOffset = 0;
    uint64_t completionFlagsBytes = 0;
    uint64_t creditOffset = 0;
    uint64_t creditBytes = 0;
    uint64_t creditSourceOffset = 0;
    uint64_t creditSourceBytes = 0;
    uint64_t signalOffset = 0;
    uint64_t signalBytes = 0;
    uint64_t hiddenDfxOffset = 0;
    uint64_t weightDfxOffset = 0;
    uint64_t dfxBytes = 0;
    uint64_t kernelStatusOffset = 0;
    uint64_t kernelStatusBytes = 0;
    uint64_t requiredBytes = 0;
    uint64_t totalBytes = 0;
};

uint64_t TileXRMoonEpDispatchUrmaAlignUp(uint64_t value, uint64_t alignment);

int TileXRMoonEpBuildDispatchUrmaLayout(int64_t rankSize, int64_t s, int64_t k,
    int64_t h, int64_t destinationCapacity, MoonEpDispatchUrmaLayout *out);

int TileXRMoonEpBindDispatchUrmaWorkspace(uint64_t workspaceBytes,
    MoonEpDispatchUrmaLayout *layout);

const DispatchUrmaActiveLayout *TileXRMoonEpGetActiveDispatchUrmaLayout(
    const MoonEpDispatchUrmaLayout &layout, DispatchPayloadMode mode);

} // namespace TileXRMoonEp

#endif // TILEXR_MOONEP_DISPATCH_URMA_LAYOUT_H
