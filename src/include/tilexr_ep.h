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

// 当前增量实现 cnt/cumsum 和远端多 route group 的第一跳发送；A5 固定 64 AIV。
// sendCountsOut 为 int32[moeExpertNum] 含当前项前缀；expertTokenNumsOut 为 int64[moeExpertNum/epWorldSize]，
// expertTokenNumsType=0 输出按 expert 累计前缀，=1 输出逐 expert count。
// DIRECT、接收/转发及最终 expert 输出尚未实现；成功表示异步提交成功。非 A5 返回 NOT_SUPPORT。
// 当前仅支持新建 communicator 上的单次调用；数据 Flag 清理和跨调用复用尚未启用。
// expertScales 必须为非空、4B 对齐的 FP32[bs,topK] 路由权重输入。
// expertScalesOut 是独立 FP32[bs,topK] 输出；当前仅 sourceMask 中的第一跳首 k 有效，权重为 1.0。
// groupOut 预留 8*bs 行，行字节数为 align32(4*(1+2*(topK-1)))；sourceMaskOut 仍为 bs 个 uint32。
int TileXRMoeEpDispatchMemoryServerDedup(void *x, int32_t *expertIds, void *scales, bool *xActiveMask,
    int64_t activeMaskType, void *expertScales, TileXRCommPtr comm, int64_t bs, int64_t h, int64_t topK,
    int64_t moeExpertNum, int64_t epWorldSize, int64_t epRankId, int64_t tpWorldSize, int64_t tpRankId,
    int64_t expertShardType, int64_t sharedExpertNum, int64_t sharedExpertRankNum, int64_t quantMode,
    int64_t globalBs, int64_t expertTokenNumsType, void *expandXOut, void *dynamicScalesOut,
    int32_t *assistInfoForCombineOut, int64_t *expertTokenNumsOut, int32_t *sendCountsOut,
    int32_t *tpRecvCountsOut, void *expandScalesOut, uint32_t *assistExOut, uint32_t *sourceMaskOut,
    uint32_t *groupOut, float *expertScalesOut, TileXR::TileXRDataType dtype, TileXR::TileXRDataType expandXOutDtype,
    aclrtStream stream);

}

#endif

#endif // TILEXR_EP_H
