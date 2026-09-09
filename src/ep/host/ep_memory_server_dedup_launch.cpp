#include "ep_memory_server_dedup_launch.h"
#include <cstdlib>
#include "acl/acl_rt.h"
#include "ep_kernel_launch.h"
#include "ep_memory_layout.h"

namespace TileXREp {

int TileXREpLaunchDispatchMemoryServerDedupKernel(const EpMemoryServerDedupParams &params,
    const EpHostLaunchContext &context, const EpMemoryServerDedupLayout &layout)
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
    uint32_t blockDim = 0;
    int ret = TileXREpResolveMemoryBlockDim(static_cast<uint32_t>(vectorCores), 1U,
        std::getenv("TILEXR_EP_DISPATCH_MEMORY_SERVER_DEDUP_BLOCK_DIM"), &blockDim);
    if (ret != TileXR::TILEXR_SUCCESS) {
        return ret;
    }
    ret = TileXREpRegisterDispatchMemoryServerDedupKernel();
    if (ret != TileXR::TILEXR_SUCCESS) {
        return ret;
    }
    const auto &p = params.dispatch;
    int64_t magic = 0;
    ret = TileXRCommNextMagic(p.comm, &magic);
    if (ret != TileXR::TILEXR_SUCCESS) {
        return ret;
    }
    EpServerDedupKernelArgs args {context.devArgs, static_cast<GM_ADDR>(p.x),
        reinterpret_cast<GM_ADDR>(p.expertIds), static_cast<GM_ADDR>(p.expertScales),
        static_cast<GM_ADDR>(p.expandXOut), reinterpret_cast<GM_ADDR>(p.expertTokenNumsOut),
        reinterpret_cast<GM_ADDR>(p.epRecvCountsOut), reinterpret_cast<GM_ADDR>(p.assistInfoForCombineOut),
        reinterpret_cast<GM_ADDR>(params.assistExOut), reinterpret_cast<GM_ADDR>(params.sourceMaskOut),
        reinterpret_cast<GM_ADDR>(params.groupOut), p.bs, p.h, p.topK, p.moeExpertNum,
        static_cast<int64_t>(layout.globalBs), p.expertTokenNumsType, static_cast<int64_t>(p.dtype),
        static_cast<int64_t>(p.expandXOutDtype == TileXR::TILEXR_DATA_TYPE_RESERVED ? p.dtype : p.expandXOutDtype),
        magic};
    return TileXREpSubmitDispatchMemoryServerDedupKernel(args, blockDim, p.stream);
}

} // namespace TileXREp
