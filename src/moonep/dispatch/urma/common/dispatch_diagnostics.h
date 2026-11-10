/*
 * 定义 URMA Dispatch 的 DFX 错误记录与 kernel 完成状态，供 Device 写入和 Host 诊断读取。
 */
#ifndef TILEXR_MOONEP_DISPATCH_DIAGNOSTICS_H
#define TILEXR_MOONEP_DISPATCH_DIAGNOSTICS_H

#include <cstdint>

namespace TileXRMoonEp {

constexpr uint32_t kDispatchDfxMarker = 0x54584444U; // TXDD
constexpr uint32_t kDispatchKernelStatusMarker = 0x54584453U; // TXDS
constexpr uint16_t kDispatchDiagnosticVersion = 3U;
constexpr uint64_t kDispatchKernelStatusFeatureDfxEnabled = 1U << 0;
constexpr uint64_t kDispatchKernelStatusFeatureFusedEpoch = 1U << 2;

enum DispatchDfxFlag : uint32_t {
    kDispatchDfxInvalidConfig = 1U << 0,
    kDispatchDfxInvalidRoute = 1U << 1,
    kDispatchDfxRouteCountMismatch = 1U << 2,
    kDispatchDfxQuietError = 1U << 3,
    kDispatchDfxCompletionTimeout = 1U << 4,
    kDispatchDfxUpstreamPlannerError = 1U << 5,
    kDispatchDfxCreditTimeout = 1U << 6,
    kDispatchDfxCqError = 1U << 7,
};

struct alignas(64) DispatchDfxRecord {
    uint32_t marker;
    uint16_t version;
    uint16_t recordBytes;
    uint32_t payloadMode;
    uint32_t rank;
    uint32_t core;
    uint32_t flags;
    uint32_t firstInvalidRouteId;
    int32_t firstInvalidRawDst;
    uint32_t firstQuietStatus;
    uint32_t firstQuietPhase;
    uint32_t timeoutPeer;
    uint32_t timeoutPhase;
    uint32_t reserved0;
    uint64_t expectedRouteCount;
    uint64_t processedRouteCount;
    uint64_t magic;
    uint64_t timeoutExpectedMagic;
    uint64_t timeoutObservedFlag;
    uint64_t reserved[4];
};

static_assert(sizeof(DispatchDfxRecord) == 128U,
    "Dispatch DFX record ABI changed");

struct alignas(64) DispatchKernelStatus {
    uint32_t marker;
    uint16_t version;
    uint16_t recordBytes;
    int32_t status;
    uint32_t payloadMode;
    uint64_t magic;
    uint64_t reserved[5];
};

static_assert(sizeof(DispatchKernelStatus) == 64U,
    "Dispatch kernel status ABI changed");

} // namespace TileXRMoonEp

#endif // TILEXR_MOONEP_DISPATCH_DIAGNOSTICS_H
