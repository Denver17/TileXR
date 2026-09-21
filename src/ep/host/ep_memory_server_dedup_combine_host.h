#ifndef TILEXR_EP_MEMORY_SERVER_DEDUP_COMBINE_HOST_H
#define TILEXR_EP_MEMORY_SERVER_DEDUP_COMBINE_HOST_H

#include "ep_host.h"
#include "ep_memory_server_dedup_buffer_layout.h"
#include "ep_memory_server_dedup_combine_config.h"

namespace TileXREp {

struct EpMemoryServerDedupCombineParams {
    EpCombineParams combine {};
    const uint32_t *tokenType = nullptr;
    const void *relayReadIndex = nullptr;
    const uint32_t *sourceMask = nullptr;
};

struct EpServerDedupCombineDataLayout {
    uint64_t resultOffset = 0;
    uint64_t resultBytes = 0;
    uint64_t stageOffset = 0;
    uint64_t stageRowBytes = 0;
    uint64_t stageRows = 0;
    uint64_t stageBytes = 0;
    uint64_t requiredWindowBytes = 0;
};

struct EpMemoryServerDedupCombineLayout {
    uint64_t globalBs = 0;
    uint64_t capacityRows = 0;
    uint64_t expertOutBytes = 0;
    uint64_t assistInfoBytes = 0;
    uint64_t sendCountsBytes = 0;
    uint64_t expertScalesBytes = 0;
    uint64_t tokenTypeBytes = 0;
    uint64_t relayReadIndexRowStrideBytes = 0;
    uint64_t relayReadIndexBytes = 0;
    uint64_t sourceMaskBytes = 0;
    uint64_t yOutBytes = 0;
    // Reserve the full result region even while P2.1 writes only stage.
    EpServerDedupCombineDataLayout data {};
    uint64_t requiredWindowBytes = 0;
};

int TileXREpBuildMemoryServerDedupCombineLayout(int64_t ranks, int64_t bs, int64_t h,
    int64_t topK, int64_t experts, EpMemoryServerDedupCombineLayout *out);
// Address arithmetic only: caller supplies a complete, 512B-aligned result slot.
// This does not choose a payload dtype, wire format or DataAsFlag protocol.
int TileXREpBuildServerDedupCombineDataLayout(int64_t ranks, int64_t bs, int64_t h,
    int64_t topK, int64_t experts, uint64_t resultSlotBytes, EpServerDedupCombineDataLayout *out);
int TileXREpValidateBasicMemoryServerDedupCombineParams(const EpMemoryServerDedupCombineParams &params);
int TileXREpValidateMemoryServerDedupCombineConfig(const EpMemoryServerDedupCombineParams &params,
    const TileXR::CommArgs &args, EpMemoryServerDedupCombineLayout *layout);
} // namespace TileXREp
#endif
