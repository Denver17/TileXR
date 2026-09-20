#include "ep_memory_server_dedup_combine_host.h"

namespace TileXREp {
int TileXREpLaunchCombineMemoryServerDedupKernel(const EpMemoryServerDedupCombineParams &,
    const EpHostLaunchContext &, const EpMemoryServerDedupCombineLayout &)
{
    // C1 delivers ABI and Host validation. C2 adds the registered empty kernel.
    // Until then do not report a successful launch or consume communicator magic.
    return TileXR::TILEXR_ERROR_NOT_SUPPORT;
}
} // namespace TileXREp
