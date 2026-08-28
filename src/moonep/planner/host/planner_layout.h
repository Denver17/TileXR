#ifndef TILEXR_MOONEP_PLANNER_LAYOUT_H
#define TILEXR_MOONEP_PLANNER_LAYOUT_H

#include <cstdint>

namespace TileXRMoonEpV3 {

struct PlannerLayout {
    int64_t rankSize = 0;
    int64_t s = 0;
    int64_t k = 0;
    int64_t expertCount = 0;
    int64_t expertsPerRank = 0;
    int64_t b = 0;
    int64_t tokenPadding = 0;
    int64_t routeCount = 0;
    int64_t nvS = 0;
    int64_t blockDim = 0;

    uint64_t tpePrefixOffset = 0;
    uint64_t blockHistogramOffset = 0;
    uint64_t allocPrefixOffset = 0;
    uint64_t expertOffsetsOffset = 0;
    uint64_t zOffset = 0;
    uint64_t groupTotalsOffset = 0;
    uint64_t compatZeroFillOffset = 0;
    uint64_t compatDupCountsOffset = 0;
    uint64_t dstLocalOffset = 0;
    uint64_t peerDstOffset = 0;
    uint64_t workspaceBytes = 0;
    uint64_t peerPublicationBytes = 0;
    uint64_t peerReadyOffset = 0;
    uint64_t peerWorkspaceBytes = 0;
};

uint64_t TileXRMoonEpAlignUp(uint64_t value, uint64_t alignment);

int TileXRMoonEpBuildPlannerLayout(int64_t rankSize, int64_t s, int64_t k,
    int64_t expertCount, int64_t b, int64_t tokenPadding, PlannerLayout *out);

} // namespace TileXRMoonEpV3

#endif // TILEXR_MOONEP_PLANNER_LAYOUT_H
