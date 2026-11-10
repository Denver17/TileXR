/*
 * 根据 communicator 的 invocation slot 取得本轮 SDK DeviceView，并登记实际 lane 数。
 * 普通 Memory 与 ServerDedup kernel 使用此入口；Host 在 launch 前准备描述符表，
 * 在全部设备工作完成后采集。这里借用 GM 地址，不分配、复用或释放 capture。
 */
#ifndef TILEXR_EP_DEVICE_PROFILING_H
#define TILEXR_EP_DEVICE_PROFILING_H

#include "comm_args.h"
#include <tilexr/profiling.h>

namespace TileXREp {
__aicore__ inline GM_ADDR ProfileDescriptor(GM_ADDR commArgs, int64_t magic, uint32_t lanes)
{
#if defined(TILEXR_ENABLE_PROFILING) && TILEXR_ENABLE_PROFILING == 1
    const auto *args = reinterpret_cast<const __gm__ TileXR::CommArgs *>(commArgs);
    if (args->profilingDescriptorsGM && magic >= args->profilingMagicBase) {
        const uint64_t slot = uint64_t(magic - args->profilingMagicBase);
        if (slot < args->profilingSlotCount) {
            const auto *descriptors = reinterpret_cast<const __gm__ uint64_t *>(args->profilingDescriptorsGM);
            GM_ADDR descriptor = reinterpret_cast<GM_ADDR>(descriptors[slot]);
            if (descriptor && AscendC::GetBlockIdx() == 0) {
                reinterpret_cast<__gm__ uint32_t *>(args->profilingLaneCountsGM)[slot * 16] = lanes;
            }
            return descriptor;
        }
    }
#else
    (void)commArgs; (void)magic; (void)lanes;
#endif
    return nullptr;
}
}
#endif
