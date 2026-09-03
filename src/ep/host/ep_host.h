#ifndef TILEXR_EP_HOST_EP_HOST_H
#define TILEXR_EP_HOST_EP_HOST_H

#include <cstdint>

#include "acl/acl_base.h"
#include "ep_layout.h"
#include "ep_memory_layout.h"
#include "tilexr_api.h"
#include "tilexr_ep.h"

namespace TileXREp {

constexpr int64_t kEpExpertTokenNumsTypePrefixSum = 0;
constexpr int64_t kEpExpertTokenNumsTypeCount = 1;

struct EpDispatchParams {
    void *x = nullptr;
    int32_t *expertIds = nullptr;
    void *scales = nullptr;
    bool *xActiveMask = nullptr;
    int64_t activeMaskType = TILEXR_EP_ACTIVE_MASK_NONE;
    void *expertScales = nullptr;
    TileXRCommPtr comm = nullptr;
    int64_t bs = 0;
    int64_t h = 0;
    int64_t topK = 0;
    int64_t moeExpertNum = 0;
    int64_t epWorldSize = 0;
    int64_t epRankId = 0;
    int64_t tpWorldSize = 0;
    int64_t tpRankId = 0;
    int64_t expertShardType = 0;
    int64_t sharedExpertNum = 0;
    int64_t sharedExpertRankNum = 0;
    int64_t quantMode = 0;
    int64_t globalBs = 0;
    int64_t expertTokenNumsType = 0;
    void *expandXOut = nullptr;
    void *dynamicScalesOut = nullptr;
    int32_t *assistInfoForCombineOut = nullptr;
    int64_t *expertTokenNumsOut = nullptr;
    int32_t *epRecvCountsOut = nullptr;
    int32_t *tpRecvCountsOut = nullptr;
    void *expandScalesOut = nullptr;
    void *workspace = nullptr;
    TileXR::TileXRDataType dtype = TileXR::TILEXR_DATA_TYPE_RESERVED;
    TileXR::TileXRDataType expandXOutDtype = TileXR::TILEXR_DATA_TYPE_RESERVED;
    aclrtStream stream = nullptr;
};

struct EpCombineParams {
    void *expertOut = nullptr;
    int32_t *assistInfoForCombine = nullptr;
    int32_t *epRecvCounts = nullptr;
    float *expertScales = nullptr;
    bool *xActiveMask = nullptr;
    int64_t activeMaskType = TILEXR_EP_ACTIVE_MASK_NONE;
    void *sharedExpertX = nullptr;
    TileXRCommPtr comm = nullptr;
    int64_t bs = 0;
    int64_t h = 0;
    int64_t topK = 0;
    int64_t moeExpertNum = 0;
    int64_t epWorldSize = 0;
    int64_t epRankId = 0;
    int64_t tpWorldSize = 0;
    int64_t tpRankId = 0;
    int64_t expertShardType = 0;
    int64_t sharedExpertNum = 0;
    int64_t sharedExpertRankNum = 0;
    int64_t quantMode = 0;
    int64_t globalBs = 0;
    void *yOut = nullptr;
    void *workspace = nullptr;
    TileXR::TileXRDataType dtype = TileXR::TILEXR_DATA_TYPE_RESERVED;
    aclrtStream stream = nullptr;
};

struct EpHostLaunchContext {
    TileXR::CommArgs *hostArgs = nullptr;
    GM_ADDR devArgs = nullptr;
    EpWindowConfig window {};
};

int TileXREpValidateBasicDispatchParams(const EpDispatchParams &params);
int TileXREpValidateDispatchConfig(const EpDispatchParams &params, const TileXR::CommArgs &commArgs,
    EpWindowConfig *window);
int TileXREpValidateDispatchMemoryConfig(const EpDispatchParams &params, const TileXR::CommArgs &commArgs,
    uint32_t blockDim, EpMemoryDispatchReferenceConfig *config);
int TileXREpValidateDispatchCapabilities(const EpDispatchParams &params, const TileXR::CommArgs &commArgs);
int TileXREpPrepareLaunchContext(const EpDispatchParams &params, EpHostLaunchContext *context);
int TileXREpPrepareMemoryLaunchContext(const EpDispatchParams &params, EpHostLaunchContext *context);

int TileXREpValidateBasicCombineParams(const EpCombineParams &params);
int TileXREpValidateCombineConfig(const EpCombineParams &params, const TileXR::CommArgs &commArgs,
    EpWindowConfig *window);
int TileXREpValidateCombineMemoryConfig(const EpCombineParams &params, const TileXR::CommArgs &commArgs,
    uint32_t blockDim, EpMemoryCombineReferenceConfig *config);
int TileXREpPrepareCombineLaunchContext(const EpCombineParams &params, EpHostLaunchContext *context);
int TileXREpPrepareMemoryCombineLaunchContext(const EpCombineParams &params, EpHostLaunchContext *context);

} // namespace TileXREp

#endif // TILEXR_EP_HOST_EP_HOST_H
