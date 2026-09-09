#include "ep_memory_server_dedup_launch.h"

namespace TileXREp {

int TileXREpLaunchDispatchMemoryServerDedupKernel(const EpMemoryServerDedupParams &,
    const EpHostLaunchContext &, const EpMemoryServerDedupLayout &)
{
    // The A5 artifact and registered launch belong to the next checkpoint.
    return TileXR::TILEXR_ERROR_NOT_SUPPORT;
}

} // namespace TileXREp
