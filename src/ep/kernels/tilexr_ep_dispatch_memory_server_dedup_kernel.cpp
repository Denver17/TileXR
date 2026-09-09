#include "kernel_operator.h"
#include "tilexr_types.h"

// 第一阶段空入口：仅检查入口参数，不读取输入、comm 或任何输出/通信状态。
extern "C" __global__ __aicore__ void tilexr_ep_dispatch_memory_server_dedup_kernel(
    GM_ADDR commArgs, GM_ADDR x, GM_ADDR expertIds, GM_ADDR expertScales,
    GM_ADDR expandXOut, GM_ADDR expertTokenNumsOut, GM_ADDR sendCountsOut,
    GM_ADDR assistInfoForCombineOut, GM_ADDR assistExOut, GM_ADDR sourceMaskOut, GM_ADDR groupOut,
    int64_t bs, int64_t h, int64_t topK, int64_t moeExpertNum, int64_t globalBs,
    int64_t expertTokenNumsType, int64_t dtype, int64_t expandXOutDtype, int64_t magic)
{
    if (commArgs == nullptr || x == nullptr || expertIds == nullptr || expandXOut == nullptr ||
        expertTokenNumsOut == nullptr || sendCountsOut == nullptr || assistExOut == nullptr ||
        sourceMaskOut == nullptr || groupOut == nullptr || bs <= 0 || h <= 0 || topK <= 0 ||
        topK > 32 || moeExpertNum <= 0 || globalBs <= 0 || expertTokenNumsType < 0 ||
        expertTokenNumsType > 1 || magic <= 0 || expandXOutDtype != dtype ||
        (dtype != TileXR::TILEXR_DATA_TYPE_FP16 && dtype != TileXR::TILEXR_DATA_TYPE_BFP16)) {
        return;
    }
    // Weight 与旧 assist 只传参；空算子不创建 TPipe、UB、ready 或 profiling 记录。
    (void)expertScales;
    (void)assistInfoForCombineOut;
}
