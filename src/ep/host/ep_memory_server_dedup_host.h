#ifndef TILEXR_EP_MEMORY_SERVER_DEDUP_HOST_H
#define TILEXR_EP_MEMORY_SERVER_DEDUP_HOST_H

#include "ep_host.h"
#include "ep_memory_server_dedup_layout.h"

namespace TileXREp {

struct EpMemoryServerDedupParams {
    EpDispatchParams dispatch {};
    uint32_t *tokenTypeOut = nullptr;
    uint32_t *destinationIndexOut = nullptr;
    uint32_t *relayReadIndexOut = nullptr;
    uint32_t *sourceMaskOut = nullptr;
    float *expertScalesOut = nullptr;
};

int TileXREpValidateBasicMemoryServerDedupParams(const EpMemoryServerDedupParams &params);
int TileXREpValidateMemoryServerDedupConfig(const EpMemoryServerDedupParams &params,
    const TileXR::CommArgs &commArgs, EpMemoryServerDedupLayout *layout);

} // namespace TileXREp

#endif // TILEXR_EP_MEMORY_SERVER_DEDUP_HOST_H
