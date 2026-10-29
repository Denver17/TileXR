#include "ep_memory_server_dedup_launch.h"
#include "acl/acl_rt.h"
#include "ep_kernel_launch.h"
#include "../../comm/tilexr_internal.h"
#include "ep_memory_server_dedup_count_layout.h"
#include "ep_memory_server_dedup_profile.h"

namespace TileXREp {

int TileXREpLaunchDispatchMemoryServerDedupKernel(
    const EpMemoryServerDedupParams &params, const EpHostLaunchContext &context, const EpMemoryServerDedupLayout &)
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
    if (vectorCores < kEpServerDedupCoreNum) {
        return TileXR::TILEXR_ERROR_NOT_SUPPORT;
    }
    int ret = TileXREpRegisterDispatchMemoryServerDedupKernel();
    if (ret != TileXR::TILEXR_SUCCESS) {
        return ret;
    }
    const auto &p = params.dispatch;
    int64_t magic = 0;
    ret = TileXRCommNextMagic(p.comm, &magic);
    if (ret != TileXR::TILEXR_SUCCESS) {
        return ret;
    }
    uint32_t profilingLaunchSlot = UINT32_MAX;
#if defined(TILEXR_EP_SERVER_DEDUP_ENABLE_PROFILING) && TILEXR_EP_SERVER_DEDUP_ENABLE_PROFILING
    // Sequential launches reuse one standard capture slot, independent of
    // communication magic. An unavailable capture never prevents submission.
    TileXRProfilingReserveLatestLaunch(p.comm, kEpServerDedupProfilingRecordsPerLaunch, &profilingLaunchSlot);
#endif
    // Basic/config validation has already checked every range and supported enum.
    EpServerDedupTilingData tiling {};
    tiling.bs = static_cast<uint32_t>(p.bs);
    tiling.h = static_cast<uint32_t>(p.h);
    tiling.profilingLaunchSlot = profilingLaunchSlot;
    tiling.moeExpertNum = static_cast<uint16_t>(p.moeExpertNum);
    tiling.topK = static_cast<uint8_t>(p.topK);
    tiling.expertTokenNumsType = static_cast<uint8_t>(p.expertTokenNumsType);
    tiling.quantMode = static_cast<uint8_t>(p.quantMode);
    tiling.dtype = static_cast<uint8_t>(p.dtype);
    tiling.expandXOutDtype =
        static_cast<uint8_t>(p.expandXOutDtype == TileXR::TILEXR_DATA_TYPE_RESERVED ? p.dtype : p.expandXOutDtype);
    GM_ADDR tilingData = nullptr;
    ret = TileXRCommGetKernelTiling(p.comm, &tiling, sizeof(tiling), &tilingData);
    if (ret != TileXR::TILEXR_SUCCESS) {
        return ret;
    }
    EpServerDedupKernelArgs args {context.devArgs, static_cast<GM_ADDR>(p.x), reinterpret_cast<GM_ADDR>(p.expertIds),
        static_cast<GM_ADDR>(p.expertScales), static_cast<GM_ADDR>(p.expandXOut),
        static_cast<GM_ADDR>(p.dynamicScalesOut), reinterpret_cast<GM_ADDR>(p.expertTokenNumsOut),
        reinterpret_cast<GM_ADDR>(p.epRecvCountsOut), reinterpret_cast<GM_ADDR>(params.tokenTypeOut),
        reinterpret_cast<GM_ADDR>(params.destinationIndexOut), reinterpret_cast<GM_ADDR>(params.relayReadIndexOut),
        reinterpret_cast<GM_ADDR>(params.sourceMaskOut), reinterpret_cast<GM_ADDR>(params.expertScalesOut), tilingData,
        magic};
    return TileXREpSubmitDispatchMemoryServerDedupKernel(args, kEpServerDedupCoreNum, p.stream);
}

} // namespace TileXREp
