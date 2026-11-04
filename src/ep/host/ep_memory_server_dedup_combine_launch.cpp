#include "ep_memory_server_dedup_combine_launch.h"
#include "acl/acl_rt.h"
#include "ep_kernel_launch.h"
#include "ep_memory_server_dedup_combine_profile.h"
#include "../../comm/tilexr_internal.h"

namespace TileXREp {
int TileXREpLaunchCombineMemoryServerDedupKernel(const EpMemoryServerDedupCombineParams &params,
    const EpHostLaunchContext &context, const EpMemoryServerDedupCombineLayout &)
{
    if (!TileXREpIsA5Soc()) {
        return TileXR::TILEXR_ERROR_NOT_SUPPORT;
    }
    int32_t device = 0;
    int64_t vectorCores = 0;
    if (aclrtGetDevice(&device) != ACL_SUCCESS ||
        aclrtGetDeviceInfo(static_cast<uint32_t>(device), ACL_DEV_ATTR_VECTOR_CORE_NUM, &vectorCores) != ACL_SUCCESS ||
        vectorCores <= 0 || vectorCores > UINT32_MAX) {
        return TileXR::TILEXR_ERROR_INTERNAL;
    }
    if (vectorCores < kEpServerDedupCombineCoreNum) {
        return TileXR::TILEXR_ERROR_NOT_SUPPORT;
    }
    int ret = TileXREpRegisterCombineMemoryServerDedupKernel();
    if (ret != TileXR::TILEXR_SUCCESS) {
        return ret;
    }

    const auto &p = params.combine;
    EpServerDedupCombineTilingData tiling {};
    tiling.bs = static_cast<uint32_t>(p.bs);
    tiling.h = static_cast<uint32_t>(p.h);
    tiling.moeExpertNum = static_cast<uint16_t>(p.moeExpertNum);
    tiling.topK = static_cast<uint8_t>(p.topK);
    tiling.dtype = static_cast<uint8_t>(p.dtype);
    tiling.quantMode = static_cast<uint8_t>(p.quantMode);
#if defined(TILEXR_EP_SERVER_DEDUP_ENABLE_PROFILING) && TILEXR_EP_SERVER_DEDUP_ENABLE_PROFILING
    uint32_t profilingSlot = UINT32_MAX;
    // Independent latest state: Dispatch's reservation width never constrains
    // Combine. Capture failure is retained by the runtime for export validation.
    TileXRProfilingReserveLatestCombineLaunch(
        p.comm, EpCombineProfilingRecordsPerLaunch(tiling.bs, tiling.topK), &profilingSlot);
    tiling.profilingEnabled = profilingSlot == 0U ? 1U : 0U;
#endif
    // The communicator owns immutable GM snapshots; never overwrite a tiling
    // buffer that an earlier asynchronous Dispatch or Combine may still read.
    GM_ADDR tilingData = nullptr;
    ret = TileXRCommGetKernelTiling(p.comm, &tiling, sizeof(tiling), &tilingData);
    if (ret != TileXR::TILEXR_SUCCESS) {
        return ret;
    }
    int64_t magic = 0;
    ret = TileXRCommNextMagic(p.comm, &magic);
    if (ret != TileXR::TILEXR_SUCCESS) {
        return ret;
    }

    EpServerDedupCombineKernelArgs args {context.devArgs, static_cast<GM_ADDR>(p.expertOut),
        reinterpret_cast<GM_ADDR>(p.assistInfoForCombine), reinterpret_cast<GM_ADDR>(p.epRecvCounts),
        reinterpret_cast<GM_ADDR>(p.expertScales), reinterpret_cast<GM_ADDR>(const_cast<uint32_t *>(params.tokenType)),
        static_cast<GM_ADDR>(const_cast<void *>(params.relayReadIndex)),
        reinterpret_cast<GM_ADDR>(const_cast<uint32_t *>(params.sourceMask)), static_cast<GM_ADDR>(p.yOut), tilingData,
        magic};
    return TileXREpSubmitCombineMemoryServerDedupKernel(args, kEpServerDedupCombineCoreNum, p.stream);
}
} // namespace TileXREp
