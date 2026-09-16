#ifndef TILEXR_EP_MEMORY_SERVER_DEDUP_DATA_LAYOUT_H
#define TILEXR_EP_MEMORY_SERVER_DEDUP_DATA_LAYOUT_H

#include "ep_memory_server_dedup_count_layout.h"

namespace TileXREp {
#if TILEXR_ASCENDC_AICORE_COMPILE
#define TILEXR_EP_DATA_INLINE FORCE_INLINE_AICORE
#else
#define TILEXR_EP_DATA_INLINE inline
#endif

struct EpServerDedupDataLayout {
    uint64_t quantCount = 0;
    uint64_t scaleBytes = 0;
    uint64_t tailOffset = 0;
    uint64_t payloadBytes = 0;
    uint64_t blockCount = 0;
    uint64_t packedPayloadBytes = 0;
    uint64_t tokenStride = 0;
    uint64_t returnStride = 0;
    uint64_t sourceOffset = 0, sourceBytes = 0;
    uint64_t dispatchOffset = 0, dispatchBytes = 0;
    uint64_t relayOffset = 0, relayBytes = 0, requiredBytes = 0;
};

TILEXR_EP_DATA_INLINE uint64_t TileXREpDedupAlign(uint64_t value, uint64_t alignment)
{
    return (value + alignment - 1U) / alignment * alignment;
}

// Host validates positive uint32 shapes and the count UB bound before calling;
// E<=1024, BS*K<2^14 and H<2^32 bound the entire domain below 2^59 bytes.
// Public buffer products are also checked against size_t by the Host builder.
TILEXR_EP_DATA_INLINE EpServerDedupDataLayout TileXREpServerDedupDataLayout(
    uint32_t bs, uint32_t h, uint32_t topK, uint32_t experts, bool quantized)
{
    EpServerDedupDataLayout layout {};
    layout.quantCount = TileXREpDedupAlign(h, 256U);
    layout.scaleBytes = quantized ? TileXREpDedupAlign((uint64_t(h) + 31U) / 32U, 2U) : 0;
    layout.tailOffset = TileXREpDedupAlign(quantized ? layout.quantCount + layout.scaleBytes : uint64_t(h) * 2U, 32U);
    layout.payloadBytes = layout.tailOffset + TileXREpDedupAlign(32U + uint64_t(topK - 1U) * 16U, 32U);
    layout.blockCount = (layout.payloadBytes + 479U) / 480U;
    layout.packedPayloadBytes = layout.blockCount * 480U;
    layout.tokenStride = layout.blockCount * 512U;
    layout.returnStride = (uint64_t(h) * 2U + 479U) / 480U * 512U;
    layout.sourceOffset = kEpServerDedupControlBytes;
    layout.sourceBytes = uint64_t(bs) * topK * layout.returnStride;
    layout.dispatchOffset = TileXREpDedupAlign(layout.sourceOffset + layout.sourceBytes, 512U);
    layout.dispatchBytes = uint64_t(experts) * bs * layout.tokenStride;
    layout.relayOffset = TileXREpDedupAlign(layout.dispatchOffset + layout.dispatchBytes, 512U);
    // Historical Combine reservation, unused by Dispatch. Prefix-index Combine
    // must resize this to capacityRows*K*returnStride before consuming new t.
    layout.relayBytes = uint64_t(8U) * bs * topK * layout.returnStride;
    layout.requiredBytes = layout.relayOffset + layout.relayBytes;
    return layout;
}

TILEXR_EP_DATA_INLINE uint64_t TileXREpServerDedupDispatchOffset(const EpServerDedupDataLayout &layout,
    uint32_t source, uint32_t destination, uint32_t localExpert, uint32_t q,
    uint32_t ranks, uint32_t localExperts, uint32_t bs)
{
    return layout.dispatchOffset + (((uint64_t(source) + destination) % ranks * localExperts + localExpert) * bs + q) * layout.tokenStride;
}

#undef TILEXR_EP_DATA_INLINE
} // namespace TileXREp
#endif
