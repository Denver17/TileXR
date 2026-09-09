#include "kernel_operator.h"
#include "tilexr_types.h"

#if defined(TILEXR_EP_SERVER_DEDUP_ENABLE_PROFILING) && TILEXR_EP_SERVER_DEDUP_ENABLE_PROFILING
#include "comm_args.h"
#include "ep_memory_server_dedup_profile.h"
#endif

// 第一阶段空入口：不读取路由/hidden，也不读写 Dispatch 输出或通信协议状态。
extern "C" __global__ __aicore__ void tilexr_ep_dispatch_memory_server_dedup_kernel(
    GM_ADDR commArgs, GM_ADDR x, GM_ADDR expertIds, GM_ADDR expertScales,
    GM_ADDR expandXOut, GM_ADDR expertTokenNumsOut, GM_ADDR sendCountsOut,
    GM_ADDR assistInfoForCombineOut, GM_ADDR assistExOut, GM_ADDR sourceMaskOut, GM_ADDR groupOut,
    int64_t bs, int64_t h, int64_t topK, int64_t moeExpertNum, int64_t globalBs,
    int64_t expertTokenNumsType, int64_t dtype, int64_t expandXOutDtype, int64_t magic)
{
#if defined(TILEXR_EP_SERVER_DEDUP_ENABLE_PROFILING) && TILEXR_EP_SERVER_DEDUP_ENABLE_PROFILING
    const uint64_t start = static_cast<uint64_t>(AscendC::GetSystemCycle());
#endif
    if (commArgs == nullptr || x == nullptr || expertIds == nullptr || expandXOut == nullptr ||
        expertTokenNumsOut == nullptr || sendCountsOut == nullptr || assistExOut == nullptr ||
        sourceMaskOut == nullptr || groupOut == nullptr || bs <= 0 || h <= 0 || topK <= 0 ||
        topK > 32 || moeExpertNum <= 0 || globalBs <= 0 || expertTokenNumsType < 0 ||
        expertTokenNumsType > 1 || magic <= 0 || expandXOutDtype != dtype ||
        (dtype != TileXR::TILEXR_DATA_TYPE_FP16 && dtype != TileXR::TILEXR_DATA_TYPE_BFP16)) {
        return;
    }
    // Weight 与旧 assist 只传参；默认构建不解引用 comm，也不创建 TPipe、UB 或 ready。
    (void)expertScales;
    (void)assistInfoForCombineOut;
#if defined(TILEXR_EP_SERVER_DEDUP_ENABLE_PROFILING) && TILEXR_EP_SERVER_DEDUP_ENABLE_PROFILING
    const uint64_t checked = static_cast<uint64_t>(AscendC::GetSystemCycle());
    const uint32_t core = AscendC::GetBlockIdx();
    const uint32_t blockDim = AscendC::GetBlockNum();
    const __gm__ TileXR::CommArgs *args = reinterpret_cast<const __gm__ TileXR::CommArgs *>(commArgs);
    if (args->rank < 0 || args->rankSize <= 0 || args->rankSize > TileXR::TILEXR_MAX_RANK_SIZE ||
        args->rank >= args->rankSize || (args->extraFlag & TileXR::ExtraFlag::MEMORY_ONLY) == 0U ||
        blockDim == 0U || blockDim > TileXREp::kEpServerDedupProfileMaxCores || core >= blockDim) {
        return;
    }
    const GM_ADDR localData = args->peerMems[args->rank];
    const uint64_t profileBytes = static_cast<uint64_t>(blockDim) *
        sizeof(TileXREp::EpServerDedupProfileRecord);
    if (localData == nullptr || reinterpret_cast<uint64_t>(localData) % 64U != 0U ||
        args->peerMemBytes < TileXREp::kEpServerDedupProfileOffset + profileBytes) {
        return;
    }
    // MemoryDomain reserves its first 8 MiB for data; this empty kernel uses only these records.
    // Each AIV owns one cache line and the final same-stream launch replaces the prior generation.
    __gm__ TileXREp::EpServerDedupProfileRecord *record =
        reinterpret_cast<__gm__ TileXREp::EpServerDedupProfileRecord *>(localData +
            TileXREp::kEpServerDedupProfileOffset + static_cast<uint64_t>(core) * sizeof(*record));
    const uint64_t end = static_cast<uint64_t>(AscendC::GetSystemCycle());
    record->marker = TileXREp::kEpServerDedupProfileMarker;
    record->version = TileXREp::kEpServerDedupProfileVersion;
    record->recordBytes = sizeof(*record);
    record->rank = static_cast<uint32_t>(args->rank);
    record->core = core;
    record->blockDim = blockDim;
    record->reserved = 0U;
    record->generation = static_cast<uint64_t>(magic);
    record->start = start;
    record->checked = checked;
    record->end = end;
    record->reserved2 = 0U;
    AscendC::GlobalTensor<uint8_t> recordGlobal;
    recordGlobal.SetGlobalBuffer(reinterpret_cast<__gm__ uint8_t *>(record), sizeof(*record));
    AscendC::DataCacheCleanAndInvalid<uint8_t, AscendC::CacheLine::SINGLE_CACHE_LINE,
        AscendC::DcciDst::CACHELINE_OUT>(recordGlobal[0]);
#endif
}
