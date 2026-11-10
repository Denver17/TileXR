/*
 * 定义 URMA Dispatch 的数据布局、core 分工与完成信号常量，供 Host 布局与 Device kernel 共同使用。
 */
#ifndef TILEXR_MOONEP_DISPATCH_COMMON_H
#define TILEXR_MOONEP_DISPATCH_COMMON_H

#include <cstdint>

namespace TileXRMoonEp {

enum class DispatchPayloadMode : uint32_t {
    Hidden = 0,
    RouteWeight = 1,
};

enum class DispatchPeerMode : uint32_t {
    Legacy = 0,
    Group = 1,
    GroupCredit = 2,
};

constexpr uint32_t kDispatchAivCoreCount = 64U;
constexpr int64_t kDispatchRankGroupSize = 64;
constexpr uint32_t kDispatchValidationGroupWidth = 8U;
constexpr uint32_t kDispatchDefaultGroupWidth = 16U;
constexpr uint32_t kDispatchMaxDesignRankCount = 512U;
constexpr uint32_t kDispatchScratchBufferCount = 2U;
constexpr uint64_t kDispatchInternalAlignmentBytes = 64U;
constexpr uint64_t kDispatchRegistrationAlignmentBytes = 2ULL * 1024ULL * 1024ULL;
constexpr uint64_t kDispatchSignalStrideBytes = 64U;
constexpr uint32_t kDispatchDfxRecordCount = kDispatchAivCoreCount;
constexpr uint64_t kDispatchProductionTimeoutSeconds = 60U;

constexpr int32_t kDispatchStatusSuccess = 0;
constexpr int32_t kDispatchStatusInvalidConfig = 2000;
constexpr int32_t kDispatchStatusInvalidRoute = 2001;
constexpr int32_t kDispatchStatusRouteCountMismatch = 2002;
constexpr int32_t kDispatchStatusUpstreamPlanner = 2003;
constexpr int32_t kDispatchStatusQuietError = 2004;
constexpr int32_t kDispatchStatusCompletionTimeout = 2005;
constexpr int32_t kDispatchStatusCreditTimeout = 2006;
constexpr int32_t kDispatchStatusCqError = 2007;

#if defined(__CCE__) && defined(__CCE_IS_AICORE__)
#define TILEXR_MOONEP_DISPATCH_INLINE __attribute__((always_inline)) inline __aicore__
#else
#define TILEXR_MOONEP_DISPATCH_INLINE inline
#endif

TILEXR_MOONEP_DISPATCH_INLINE bool DispatchDecodeDestination(
    int32_t encoded, int64_t capacity, int64_t world, int64_t *targetRank,
    int64_t *targetSlot)
{
    if (capacity <= 0 || world <= 0 || targetRank == nullptr || targetSlot == nullptr) {
        return false;
    }
    if (encoded < 0) {
        return false;
    }
    const int64_t raw = static_cast<int64_t>(encoded);
    if (raw / capacity >= world) {
        return false;
    }
    *targetRank = raw / capacity;
    *targetSlot = raw % capacity;
    return true;
}

TILEXR_MOONEP_DISPATCH_INLINE bool DispatchPayloadModeValid(uint32_t mode)
{
    return mode == static_cast<uint32_t>(DispatchPayloadMode::Hidden) ||
        mode == static_cast<uint32_t>(DispatchPayloadMode::RouteWeight);
}

TILEXR_MOONEP_DISPATCH_INLINE bool DispatchPeerModeValid(uint32_t mode)
{
    return mode == static_cast<uint32_t>(DispatchPeerMode::Legacy) ||
        mode == static_cast<uint32_t>(DispatchPeerMode::Group) ||
        mode == static_cast<uint32_t>(DispatchPeerMode::GroupCredit);
}

TILEXR_MOONEP_DISPATCH_INLINE bool DispatchPeerModeUsesGroups(uint32_t mode)
{
    return mode == static_cast<uint32_t>(DispatchPeerMode::Group) ||
        mode == static_cast<uint32_t>(DispatchPeerMode::GroupCredit);
}

TILEXR_MOONEP_DISPATCH_INLINE bool DispatchPeerModeUsesCredit(uint32_t mode)
{
    return mode == static_cast<uint32_t>(DispatchPeerMode::GroupCredit);
}

TILEXR_MOONEP_DISPATCH_INLINE bool DispatchGroupWidthValid(uint32_t groupWidth)
{
    return groupWidth == kDispatchValidationGroupWidth ||
        groupWidth == kDispatchDefaultGroupWidth;
}

} // namespace TileXRMoonEp

#undef TILEXR_MOONEP_DISPATCH_INLINE

#endif // TILEXR_MOONEP_DISPATCH_COMMON_H
