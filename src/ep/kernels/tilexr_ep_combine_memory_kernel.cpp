#include "tilexr_ep_combine_memory_kernel.h"
#include <tilexr/profiling/device.h>

extern "C" __global__ __aicore__ void tilexr_ep_combine_memory_kernel(GM_ADDR commArgsGM, GM_ADDR expertOutGM,
    GM_ADDR assistInfoForCombineGM, GM_ADDR sendCountsGM, GM_ADDR expertScalesGM, GM_ADDR xActiveMaskGM,
    GM_ADDR sharedExpertXGM, GM_ADDR yOutGM, int64_t bs, int64_t h, int64_t topK,
    int64_t moeExpertNum, int64_t sharedExpertNum, int64_t sharedExpertRankNum, int64_t globalBs,
    int64_t activeMaskType, int64_t quantMode, int64_t dtype, int64_t magic)
{
    if (commArgsGM == nullptr || expertOutGM == nullptr || assistInfoForCombineGM == nullptr ||
        sendCountsGM == nullptr || yOutGM == nullptr || bs <= 0 || h <= 0 || topK <= 0 ||
        moeExpertNum <= 0 || sharedExpertNum < 0 || sharedExpertRankNum < 0 || globalBs <= 0 || magic <= 0 ||
        (activeMaskType != Mc2Kernel::ACTIVE_MASK_NONE && activeMaskType != Mc2Kernel::ACTIVE_MASK_TOKEN) ||
        (activeMaskType == Mc2Kernel::ACTIVE_MASK_NONE && xActiveMaskGM != nullptr) ||
        (activeMaskType == Mc2Kernel::ACTIVE_MASK_TOKEN && xActiveMaskGM == nullptr) ||
        (quantMode != 0 && quantMode != Mc2Kernel::MXFP8_E5M2_COMM_QUANT &&
            quantMode != Mc2Kernel::MXFP8_E4M3_COMM_QUANT) ||
        (quantMode != 0 && expertScalesGM == nullptr)) {
        return;
    }
    GM_ADDR profileGM = nullptr;
#if defined(TILEXR_ENABLE_PROFILING) && TILEXR_ENABLE_PROFILING == 1
    auto *args = reinterpret_cast<__gm__ TileXR::CommArgs *>(commArgsGM);
    if (args->profilingDescriptorsGM && magic >= args->profilingMagicBase) {
        const uint64_t slot = uint64_t(magic - args->profilingMagicBase);
        if (slot < args->profilingSlotCount) {
            auto *descriptors = reinterpret_cast<__gm__ uint64_t *>(args->profilingDescriptorsGM);
            profileGM = reinterpret_cast<GM_ADDR>(descriptors[slot]);
            if (profileGM && AscendC::GetBlockIdx() == 0)
                reinterpret_cast<__gm__ uint32_t *>(args->profilingLaneCountsGM)[slot*16] = AscendC::GetBlockNum();
        }
    }
#endif
    TileXR::Profiling::Recorder<2> recorder;
    if (profileGM) recorder.RecordMarker<0, 1>();
    if (dtype == static_cast<int64_t>(TileXR::TILEXR_DATA_TYPE_FP16)) {
        Mc2Kernel::RunCombine<half>(commArgsGM, expertOutGM, assistInfoForCombineGM, sendCountsGM,
            expertScalesGM, xActiveMaskGM, sharedExpertXGM, yOutGM, bs, h, topK, moeExpertNum,
            sharedExpertNum, sharedExpertRankNum, globalBs, activeMaskType, quantMode, magic);
    } else if (dtype == static_cast<int64_t>(TileXR::TILEXR_DATA_TYPE_BFP16)) {
        Mc2Kernel::RunCombine<bfloat16_t>(commArgsGM, expertOutGM, assistInfoForCombineGM, sendCountsGM,
            expertScalesGM, xActiveMaskGM, sharedExpertXGM, yOutGM, bs, h, topK, moeExpertNum,
            sharedExpertNum, sharedExpertRankNum, globalBs, activeMaskType, quantMode, magic);
    }
    if (profileGM) {
        recorder.RecordMarker<1, 2>();
        recorder.Flush(profileGM, AscendC::GetBlockIdx());
    }
}
