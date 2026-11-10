/*
 * 声明 Combine V2 工作区查询与执行入口；调用者提供已注册的 workspace 和 output，设备执行由对应 Host library 提交。
 */
#ifndef TILEXR_MOONEP_COMBINE_V2_H
#define TILEXR_MOONEP_COMBINE_V2_H

#include <stdint.h>

#include "tilexr_moonep.h"

#ifdef __cplusplus
extern "C" {
#endif

int TileXRMoonEpCombineGetWorkspaceSizeV2(
    int64_t bs, int64_t h, int64_t topK, int64_t nvS, uint32_t dtype,
    uint64_t *workspaceBytes,
    uint64_t *outputEpoch0Offset, uint64_t *outputEpoch1Offset);

// Supports rank sizes 2 through 8, 16, 32, 64, and 128. For 2 through 8
// ranks, localRankSize must equal rankSize; larger configurations require
// localRankSize == 8. The shared-QP communicator must expose at least 32 QPs.
// aivCoreNum is retained for API compatibility and must equal 16. The current
// device must report at least 16 vector cores.
int TileXRMoonEpCombineV2(void *registeredWorkspace,
    const int32_t *dstLocal, TileXRCommPtr comm, int64_t bs, int64_t h,
    int64_t topK, int64_t nvS, uint32_t aivCoreNum,
    uint64_t *activeOutputOffset,
    uint32_t dtype, aclrtStream stream);

int TileXRMoonEpCombineStageV2(void *registeredWorkspace,
    uint64_t registeredWorkspaceBytes, const int32_t *dstLocal,
    TileXRCommPtr comm, int64_t bs, int64_t h, int64_t topK, int64_t nvS,
    uint32_t aivCoreNum, const void *hiddenNvsh, void *hiddenSh,
    const float *routeWeightsNvs, float *routeWeightsSk,
    uint32_t dtype, aclrtStream stream);

int TileXRMoonEpCombineStageV2Fused(void *registeredWorkspace,
    uint64_t registeredWorkspaceBytes, const int32_t *dstLocal,
    TileXRCommPtr comm, TileXRCommPtr weightMemoryComm,
    int64_t bs, int64_t h, int64_t topK, int64_t nvS,
    uint32_t aivCoreNum, const void *hiddenNvsh, void *hiddenSh,
    const float *routeWeightsNvs, float *routeWeightsSk,
    uint32_t dtype, aclrtStream stream);

#ifdef __cplusplus
}
#endif

#endif // TILEXR_MOONEP_COMBINE_V2_H
