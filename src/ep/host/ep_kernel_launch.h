#ifndef TILEXR_EP_HOST_EP_KERNEL_LAUNCH_H
#define TILEXR_EP_HOST_EP_KERNEL_LAUNCH_H

#include "ep_host.h"

namespace TileXREp {

bool TileXREpIsA5Soc();
int TileXREpLaunchDispatchKernel(const EpDispatchParams &params, const EpHostLaunchContext &context);
int TileXREpLaunchDispatchMemoryKernel(const EpDispatchParams &params, const EpHostLaunchContext &context);
int TileXREpLaunchCombineKernel(const EpCombineParams &params, const EpHostLaunchContext &context);
int TileXREpLaunchCombineMemoryKernel(const EpCombineParams &params, const EpHostLaunchContext &context);

} // namespace TileXREp

#endif // TILEXR_EP_HOST_EP_KERNEL_LAUNCH_H
