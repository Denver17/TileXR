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

// Server Dedup Combine: A5 only, FP16/BFP16, quantMode=0, fixed 64 AIV.
// P2.4 stages unweighted type3 rows, completes server readiness (M1/M2),
// returns raw type1 rows (M3) and FP32-weighted type2 partials (M4) to source
// result slots, without a fixed wait. Partials round back to the input dtype.
// M5 receives SourceMask-selected slots, weights them in FP32 and rounds yOut
// to the input dtype. It clears all result Flags while preserving payload.
// Success means asynchronous submission; input tensors remain unchanged.
// Repeated calls require the initial zeroed buffer and the same shape/layout.
// C=R*bs*min(topK,moeExpertNum/R); expertOut[C,h], assistInfoForCombine[C,3],
// tokenType[C], relayReadIndex[C,topK,2] (raw 32-bit pairs), sourceMask[bs],
// expertScales[bs,topK]. sendCounts is the full int32[moeExpertNum] prefix;
// its last item is the device-side valid row count, in [0,min(C,10000)].
// SourceMask has at least one valid bit per token; zero weights still receive.
// Host never reads tensors and rejects shapes that cannot fit the M1-M5 UB budget.
// yOut[bs,h] has dtype. Tensor capacity/device ownership are caller obligations.
// Table pointers retain Dispatch row order and remain alive through completion.
// epWorldSize/epRankId/globalBs accept 0 as the existing infer-from-comm sentinel.
int TileXRMoeEpCombineMemoryServerDedup(void *expertOut, int32_t *assistInfoForCombine, int32_t *sendCounts,
    float *expertScales, bool *xActiveMask, int64_t activeMaskType, void *sharedExpertX,
    TileXRCommPtr comm, int64_t bs, int64_t h, int64_t topK, int64_t moeExpertNum,
    int64_t epWorldSize, int64_t epRankId, int64_t tpWorldSize, int64_t tpRankId,
    int64_t expertShardType, int64_t sharedExpertNum, int64_t sharedExpertRankNum,
    int64_t quantMode, int64_t globalBs, void *yOut, TileXR::TileXRDataType dtype, aclrtStream stream,
    const uint32_t *tokenType, const void *relayReadIndex, const uint32_t *sourceMask);

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
// TILEXR_EP_SERVER_DEDUP_ENABLE 在 src/ep/common/ep_memory_server_dedup_count_layout.h 中配置，默认开启；
// 修改后须重新编译 Host 与 kernel。关闭时按 server 轮转逐 route DIRECT。
// 接收协议和输出容量不随模式改变。
// sendCountsOut 为 int32[moeExpertNum] 含当前项前缀；expertTokenNumsOut 为 int64[moeExpertNum/epWorldSize]，
// expertTokenNumsType=0 输出按 expert 累计前缀，=1 输出逐 expert count。
// 成功表示异步提交成功。非 A5 返回 NOT_SUPPORT；同一 communicator 须在同一 stream 串行调用。
// expertScales 必须为非空、4B 对齐的 FP32[bs,topK] 路由权重输入。
// expertScalesOut 是独立 FP32[bs,topK] 输出；sourceMask 有效位中 DIRECT 为真实权重，第一跳首 k 为 1.0。
// 关闭去重时 sourceMask 的全部 topK 位有效，expertScalesOut 全部保留原始权重。
// C=epWorldSize*bs*min(topK,moeExpertNum/epWorldSize)，有效 row 数为 sendCountsOut[moeExpertNum-1]。
// tokenTypeOut 为 uint32[C]，1=DIRECT、2=第一跳、3=转发终端，与 expandXOut 行序一致。
// destinationIndexOut 为 uint32[C][3]：类型 1/2 为 [srcRank,srcTokenId,originalK]，
// 类型 3 为 [本卡 Combine stage 逻辑 slot,UINT32_MAX,UINT32_MAX]，slot 必须按无符号解释。
// relayReadIndexOut 行距为 topK*8B，仅类型 2 有效；首块为 uint32 N 与 FP32 selfWeight，
// 后续 N 块为 uint16 expertId、uint16 q、FP32 weight，其余块不消费。
// sourceMaskOut 为 uint32[bs]；五表基地址至少 4B 对齐，行距不另行向 32B 取整。
// 五表输出应持有到对应 Combine 完成；旧 Server Dedup metadata 接口不再兼容。
int TileXRMoeEpDispatchMemoryServerDedup(void *x, int32_t *expertIds, void *scales, bool *xActiveMask,
    int64_t activeMaskType, void *expertScales, TileXRCommPtr comm, int64_t bs, int64_t h, int64_t topK,
    int64_t moeExpertNum, int64_t epWorldSize, int64_t epRankId, int64_t tpWorldSize, int64_t tpRankId,
    int64_t expertShardType, int64_t sharedExpertNum, int64_t sharedExpertRankNum, int64_t quantMode,
    int64_t globalBs, int64_t expertTokenNumsType, void *expandXOut, void *dynamicScalesOut,
    int64_t *expertTokenNumsOut, int32_t *sendCountsOut, int32_t *tpRecvCountsOut, void *expandScalesOut,
    uint32_t *tokenTypeOut, uint32_t *destinationIndexOut, uint32_t *relayReadIndexOut,
    uint32_t *sourceMaskOut, float *expertScalesOut, TileXR::TileXRDataType dtype, TileXR::TileXRDataType expandXOutDtype,
    aclrtStream stream);

}

#endif

#endif // TILEXR_EP_H
