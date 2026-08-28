#ifndef TILEXR_MOONEP_PLANNER_COMMON_H
#define TILEXR_MOONEP_PLANNER_COMMON_H

#include <cstdint>

namespace TileXRMoonEpV3 {

constexpr int64_t kPlannerAivBlockCount = 64;
constexpr uint64_t kPlannerWorkspaceAlignment = 32;
constexpr uint64_t kPlannerPeerTransferBytes = 512;
constexpr uint64_t kPlannerRegisteredWorkspaceAlignment = 2 * 1024 * 1024;
constexpr int32_t kPlannerReadyStep = 91;
constexpr int32_t kPlannerDstReadyStep = 92;
constexpr int32_t kPlannerReadyEventId = 4096;
constexpr int32_t kPlannerStatusSuccess = 0;
constexpr int32_t kPlannerStatusTimeoutBase = 1000;
constexpr int32_t kPlannerStatusTransportBase = 2000;
constexpr int64_t kPlannerMaxTopK = 32;
constexpr int64_t kPlannerMaxExpertCount = 4096;
constexpr uint32_t kPlannerSyncUbBytes = 4 * 1024;
constexpr uint32_t kPlannerWorkUbBytes = 192 * 1024;
constexpr int64_t kPlannerRouteTileInts = 16 * 1024;

static_assert(kPlannerPeerTransferBytes % kPlannerWorkspaceAlignment == 0,
    "planner peer transfers must preserve workspace alignment");
static_assert(kPlannerRegisteredWorkspaceAlignment % kPlannerPeerTransferBytes == 0,
    "planner registered workspace must preserve peer-transfer alignment");

} // namespace TileXRMoonEpV3

#endif // TILEXR_MOONEP_PLANNER_COMMON_H
