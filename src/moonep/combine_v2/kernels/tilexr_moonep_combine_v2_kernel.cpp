/*
 * 执行 MoonEP Combine V2 的传输、归约及失败传播；Host combine_v2_launch.cpp 提供工作区与路由参数。
 */
#include "tilexr_moonep_combine_v2_kernel.h"

extern "C" __global__ __aicore__ void
tilexr_moonep_combine_v2_kernel(GM_ADDR commArgs,
    GM_ADDR registeredWorkspace, GM_ADDR dstLocal,
    uint64_t scratchEpoch0Offset, uint64_t scratchEpoch1Offset,
    uint64_t doneOffset, uint64_t reservedOffset0,
    uint64_t controlSourceOffset, uint64_t failureOffset,
    uint64_t reservedSyncReceiveOffset, uint64_t reservedSyncSourceOffset,
    uint64_t collectiveStatusOffset,
    uint64_t outputOffset, int64_t bs, int64_t h, int64_t topK,
    int64_t nvS, uint64_t rowBytes, uint64_t reduceHidden,
    int64_t magic, GM_ADDR weightMemoryCommArgs,
    GM_ADDR routeWeightsNvs, GM_ADDR routeWeightsSk,
    uint64_t weightRecordOffset, uint64_t weightDoneOffset,
    uint64_t weightWindowBytes, uint64_t weightOutputElements,
    uint64_t hasRouteWeight)
{
    AscendC::TPipe pipe;
    MoonEpCombineV2 op;
    op.Init(commArgs, registeredWorkspace, dstLocal,
        scratchEpoch0Offset, scratchEpoch1Offset, doneOffset,
        reservedOffset0, controlSourceOffset, failureOffset,
        reservedSyncReceiveOffset, reservedSyncSourceOffset,
        collectiveStatusOffset, outputOffset, bs, h,
        topK, nvS, rowBytes, reduceHidden != 0U, magic,
        weightMemoryCommArgs, routeWeightsNvs, routeWeightsSk,
        weightRecordOffset, weightDoneOffset, weightWindowBytes,
        weightOutputElements, hasRouteWeight != 0U, &pipe);
    op.Process();
}
