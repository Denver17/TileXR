#include "tilexr_ep_dispatch_memory_kernel.h"

extern "C" __global__ __aicore__ void tilexr_ep_dispatch_memory_kernel(GM_ADDR commArgsGM, GM_ADDR xGM,
    GM_ADDR expertIdsGM, GM_ADDR xActiveMaskGM, GM_ADDR expandXOutGM, GM_ADDR dynamicScalesOutGM,
    GM_ADDR expertTokenNumsOutGM,
    GM_ADDR sendCountsOutGM, GM_ADDR assistInfoForCombineOutGM, int64_t bs, int64_t h, int64_t topK,
    int64_t moeExpertNum, int64_t sharedExpertNum, int64_t sharedExpertRankNum, int64_t globalBs,
    int64_t expertTokenNumsType, int64_t activeMaskType, int64_t quantMode,
    int64_t dtype, int64_t expandXOutDtype, int64_t magic)
{
#if !defined(__NPU_ARCH__) || (__NPU_ARCH__ != 3510)
    if (quantMode == Mc2Kernel::MX_QUANT) {
        return;
    }
#endif
    if (commArgsGM == nullptr || xGM == nullptr || expertIdsGM == nullptr || expandXOutGM == nullptr ||
        expertTokenNumsOutGM == nullptr || sendCountsOutGM == nullptr || assistInfoForCombineOutGM == nullptr ||
        bs <= 0 || h <= 0 || topK <= 0 || moeExpertNum <= 0 || sharedExpertNum < 0 ||
        sharedExpertRankNum < 0 || globalBs <= 0 || expertTokenNumsType < 0 || expertTokenNumsType > 1 || magic <= 0 ||
        activeMaskType < Mc2Kernel::ACTIVE_MASK_NONE || activeMaskType > Mc2Kernel::ACTIVE_MASK_TOKEN ||
        (activeMaskType == Mc2Kernel::ACTIVE_MASK_NONE && xActiveMaskGM != nullptr) ||
        (activeMaskType != Mc2Kernel::ACTIVE_MASK_NONE && xActiveMaskGM == nullptr) ||
        (quantMode != 0 && quantMode != Mc2Kernel::MX_QUANT) ||
        (quantMode == Mc2Kernel::MX_QUANT && (dynamicScalesOutGM == nullptr ||
            (expandXOutDtype != TileXR::TILEXR_DATA_TYPE_FP8E4M3 &&
                expandXOutDtype != TileXR::TILEXR_DATA_TYPE_FP8E5M2)))) {
        return;
    }

    AscendC::TPipe pipe;
    Mc2Kernel::RunMoeDispatchMemory(commArgsGM, xGM, expertIdsGM, xActiveMaskGM,
        expandXOutGM, dynamicScalesOutGM, expertTokenNumsOutGM, sendCountsOutGM,
        assistInfoForCombineOutGM, bs, h, topK, moeExpertNum, sharedExpertNum,
        sharedExpertRankNum, globalBs, expertTokenNumsType, activeMaskType, quantMode,
        dtype, expandXOutDtype, magic, &pipe);
}
