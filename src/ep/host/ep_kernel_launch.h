/*
 * 声明 EP Host 的注册、launch 和 SDK capture 准入 helper；实现由 ep_kernel_launch.cpp 提供。
 */
#ifndef TILEXR_EP_HOST_EP_KERNEL_LAUNCH_H
#define TILEXR_EP_HOST_EP_KERNEL_LAUNCH_H

#include "ep_host.h"

namespace TileXREp {

bool TileXREpIsA5Soc();
int TileXREpValidateProfilingCapture(const TileXR::CommArgs &args, int64_t magic, uint32_t lanes);
int TileXREpLaunchDispatchKernel(const EpDispatchParams &params, const EpHostLaunchContext &context);
int TileXREpLaunchDispatchMemoryKernel(const EpDispatchParams &params, const EpHostLaunchContext &context);
int TileXREpLaunchCombineKernel(const EpCombineParams &params, const EpHostLaunchContext &context);
int TileXREpLaunchCombineMemoryKernel(const EpCombineParams &params, const EpHostLaunchContext &context);

} // namespace TileXREp

#endif // TILEXR_EP_HOST_EP_KERNEL_LAUNCH_H
