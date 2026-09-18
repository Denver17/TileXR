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

// Server Dedup Dispatch 使用 A5 固定 64 AIV；发送/接收核数由
// TILEXR_EP_SERVER_DEDUP_SEND_CORE_NUM 编译期配置（默认 32/32，支持 16/24/32/40/48）。
// 剩余 AIV 用于接收；同一 communicator 生命周期内所有 Rank 必须保持相同且固定的核数配置。
// TILEXR_EP_SERVER_DEDUP_ENABLE 未设置或为 1 时开启去重；为 0 时按 server 轮转逐 route DIRECT。
// 每次调用读取该环境变量，其他值返回 PARA_CHECK_FAIL；接收协议和输出容量不随模式改变。
// sendCountsOut 为 int32[moeExpertNum] 含当前项前缀；expertTokenNumsOut 为 int64[moeExpertNum/epWorldSize]，
// expertTokenNumsType=0 输出按 expert 累计前缀，=1 输出逐 expert count。
// 成功表示异步提交成功。非 A5 返回 NOT_SUPPORT；同一 communicator 须在同一 stream 串行调用。
// expertScales 必须为非空、4B 对齐的 FP32[bs,topK] 路由权重输入。
// expertScalesOut 是独立 FP32[bs,topK] 输出；sourceMask 有效位中 DIRECT 为真实权重，第一跳首 k 为 1.0。
// 关闭去重时 sourceMask 的全部 topK 位有效，expertScalesOut 全部保留原始权重。
// groupOut 按 epWorldSize*bs*min(topK,moeExpertNum/epWorldSize) 行预留；仅第一跳 row 有效。
// forwardTokenIdx 为 relay 第一跳的零基输出 row；行字节数为 align32(4*(1+2*(topK-1)))。
// 调用方须按新容量重新分配 groupOut；sourceMaskOut 仍为 bs 个 uint32。
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
