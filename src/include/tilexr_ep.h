#ifndef TILEXR_EP_H
#define TILEXR_EP_H

#ifdef __cplusplus

#include <cstdint>

#include "acl/acl_base.h"
#include "tilexr_api.h"
#include "tilexr_types.h"

namespace TileXREp {

enum TileXREpActiveMaskType : int64_t {
    TILEXR_EP_ACTIVE_MASK_NONE = 0,
    TILEXR_EP_ACTIVE_MASK_TOKEN = 1,
    TILEXR_EP_ACTIVE_MASK_EXPERT = 2,
};

} // namespace TileXREp

// This public API is C++ header-compatible because it reuses TileXR namespace datatypes.
extern "C" {

int TileXRMoeEpCombine(void *expertOut, int32_t *assistInfoForCombine, int32_t *epRecvCounts,
    TileXRCommPtr comm, int64_t bs, int64_t h, int64_t topK, int64_t moeExpertNum,
    void *yOut, void *workspace, TileXR::TileXRDataType dtype, aclrtStream stream);

int TileXRMoeEpCombineMemory(void *expertOut, int32_t *assistInfoForCombine, int32_t *sendCounts,
    float *expertScales, bool *xActiveMask, int64_t activeMaskType, void *sharedExpertX,
    TileXRCommPtr comm, int64_t bs, int64_t h, int64_t topK, int64_t moeExpertNum,
    int64_t epWorldSize, int64_t epRankId, int64_t tpWorldSize, int64_t tpRankId,
    int64_t expertShardType, int64_t sharedExpertNum, int64_t sharedExpertRankNum,
    int64_t quantMode, int64_t globalBs, void *yOut, TileXR::TileXRDataType dtype, aclrtStream stream);

int TileXRMoeEpDispatch(void *x, int32_t *expertIds, void *scales, bool *xActiveMask, void *expertScales,
    TileXRCommPtr comm, int64_t bs, int64_t h, int64_t topK, int64_t moeExpertNum, int64_t epWorldSize,
    int64_t epRankId, int64_t tpWorldSize, int64_t tpRankId, int64_t expertShardType, int64_t sharedExpertNum,
    int64_t sharedExpertRankNum, int64_t quantMode, int64_t globalBs, int64_t expertTokenNumsType, void *expandXOut,
    void *dynamicScalesOut, int32_t *assistInfoForCombineOut, int64_t *expertTokenNumsOut, int32_t *epRecvCountsOut,
    int32_t *tpRecvCountsOut, void *expandScalesOut, void *workspace, TileXR::TileXRDataType dtype,
    aclrtStream stream);

int TileXRMoeEpDispatchMemory(void *x, int32_t *expertIds, void *scales, bool *xActiveMask,
    int64_t activeMaskType, void *expertScales, TileXRCommPtr comm, int64_t bs, int64_t h, int64_t topK,
    int64_t moeExpertNum,
    int64_t epWorldSize, int64_t epRankId, int64_t tpWorldSize, int64_t tpRankId, int64_t expertShardType,
    int64_t sharedExpertNum, int64_t sharedExpertRankNum, int64_t quantMode, int64_t globalBs,
    int64_t expertTokenNumsType, void *expandXOut, void *dynamicScalesOut, int32_t *assistInfoForCombineOut,
    int64_t *expertTokenNumsOut, int32_t *sendCountsOut, int32_t *tpRecvCountsOut, void *expandScalesOut,
    TileXR::TileXRDataType dtype, TileXR::TileXRDataType expandXOutDtype, aclrtStream stream);

// ABI/Host checkpoint only: returns NOT_SUPPORT until the independent kernel is available.
// Metadata is caller-owned. No outputs are written and no launch magic is consumed yet.
int TileXRMoeEpDispatchMemoryServerDedup(void *x, int32_t *expertIds, void *scales, bool *xActiveMask,
    int64_t activeMaskType, void *expertScales, TileXRCommPtr comm, int64_t bs, int64_t h, int64_t topK,
    int64_t moeExpertNum, int64_t epWorldSize, int64_t epRankId, int64_t tpWorldSize, int64_t tpRankId,
    int64_t expertShardType, int64_t sharedExpertNum, int64_t sharedExpertRankNum, int64_t quantMode,
    int64_t globalBs, int64_t expertTokenNumsType, void *expandXOut, void *dynamicScalesOut,
    int32_t *assistInfoForCombineOut, int64_t *expertTokenNumsOut, int32_t *sendCountsOut,
    int32_t *tpRecvCountsOut, void *expandScalesOut, uint32_t *assistExOut, uint32_t *sourceMaskOut,
    uint32_t *groupOut, TileXR::TileXRDataType dtype, TileXR::TileXRDataType expandXOutDtype,
    aclrtStream stream);

}

#endif

#endif // TILEXR_EP_H
