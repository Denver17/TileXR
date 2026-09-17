#ifndef TILEXR_EP_MEMORY_SERVER_DEDUP_HOST_H
#define TILEXR_EP_MEMORY_SERVER_DEDUP_HOST_H

#include "ep_host.h"
#include "ep_memory_server_dedup_layout.h"

namespace TileXREp {

struct EpMemoryServerDedupParams {
    EpDispatchParams dispatch {};
    int64_t sendCoreNum = 32;
    bool enableServerDedup = true;
    uint32_t *assistExOut = nullptr;
    uint32_t *sourceMaskOut = nullptr;
    uint32_t *groupOut = nullptr;
    float *expertScalesOut = nullptr;
};

int TileXREpValidateBasicMemoryServerDedupParams(const EpMemoryServerDedupParams &params);
int TileXREpValidateMemoryServerDedupConfig(const EpMemoryServerDedupParams &params,
    const TileXR::CommArgs &commArgs, EpMemoryServerDedupLayout *layout);

} // namespace TileXREp

#endif // TILEXR_EP_MEMORY_SERVER_DEDUP_HOST_H
