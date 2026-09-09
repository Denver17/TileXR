#ifndef TILEXR_EP_MEMORY_SERVER_DEDUP_LAUNCH_H
#define TILEXR_EP_MEMORY_SERVER_DEDUP_LAUNCH_H

#include "ep_memory_server_dedup_host.h"

namespace TileXREp {

int TileXREpLaunchDispatchMemoryServerDedupKernel(const EpMemoryServerDedupParams &params,
    const EpHostLaunchContext &context, const EpMemoryServerDedupLayout &layout);

} // namespace TileXREp

#endif // TILEXR_EP_MEMORY_SERVER_DEDUP_LAUNCH_H
