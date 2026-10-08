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
    uint64_t headerPayloadOffset = 0, readIndexPayloadOffset = 0;
    uint64_t headerWireOffset = 0, relayReadIndexWireOffset = 0;
    uint64_t payloadBytes = 0;
    uint64_t blockCount = 0;
    uint64_t packedPayloadBytes = 0;
    uint64_t tokenStride = 0;
    uint64_t dispatchOffset = 0, dispatchBytes = 0;
    uint64_t requiredBytes = 0;
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
    layout.headerPayloadOffset = TileXREpDedupAlign(quantized ? layout.quantCount + layout.scaleBytes : uint64_t(h) * 2U, 32U);
    layout.readIndexPayloadOffset = layout.headerPayloadOffset + 32U;
    const uint64_t readIndexBytes = kEpServerDedupEnable ? uint64_t(topK) * 8U : 0U;
    if (layout.readIndexPayloadOffset % 480U + readIndexBytes > 480U)
        layout.readIndexPayloadOffset = TileXREpDedupAlign(layout.readIndexPayloadOffset, 480U);
    layout.headerWireOffset = layout.headerPayloadOffset / 480U * 512U + layout.headerPayloadOffset % 480U;
    layout.relayReadIndexWireOffset = layout.readIndexPayloadOffset / 480U * 512U + layout.readIndexPayloadOffset % 480U;
    layout.payloadBytes = TileXREpDedupAlign(layout.readIndexPayloadOffset + readIndexBytes, 32U);
    layout.blockCount = (layout.payloadBytes + 479U) / 480U;
    layout.packedPayloadBytes = layout.blockCount * 480U;
    layout.tokenStride = layout.blockCount * 512U;
    layout.dispatchOffset = TILEXR_EP_SERVER_DEDUP_SHARED_DATA_OFFSET;
    layout.dispatchBytes = uint64_t(experts) * bs * layout.tokenStride;
    layout.requiredBytes = TileXREpDedupAlign(layout.dispatchOffset + layout.dispatchBytes, 512U);
    return layout;
}

// One uint32 state per server. Keep the existing minimum for smaller worlds.
TILEXR_EP_DATA_INLINE uint64_t TileXREpServerDedupServerStateBytes(uint32_t ranks)
{
    static_assert(TileXR::TILEXR_MAX_RANK_SIZE <= 32 * 8,
        "Dispatch server masks require at most 32 servers");
    const uint64_t bytes = TileXREpDedupAlign(uint64_t(ranks / 8U) * sizeof(uint32_t), 32U);
    return bytes < 64U ? 64U : bytes;
}

// Fixed sender allocation shared by Host admission and device lane planning.
// owned is the weight-copy partition, not the non-dedup route ownership.
TILEXR_EP_DATA_INLINE uint64_t TileXREpServerDedupSendFixedUbBytes(uint32_t bs, uint32_t k,
    uint32_t ranks, uint32_t owned, uint32_t reserved, bool quantized, const EpServerDedupDataLayout &data)
{
    const uint64_t input = quantized ? data.quantCount * 2U : data.packedPayloadBytes;
    const uint64_t q = TileXREpDedupAlign(uint64_t(bs) * k * 4U, 32U);
    const uint64_t scaleCount = data.quantCount / 32U;
    const uint64_t quantWork = TileXREpDedupAlign(TileXREpDedupAlign(scaleCount, 32U) * 4U + scaleCount * 2U, 32U);
    const uint64_t work = quantized && quantWork > q ? quantWork : q;
    uint64_t bytes = reserved + 2U * input + TileXREpDedupAlign(uint64_t(bs) * k * 4U, 256U) +
        2U * work + q + (quantized ? data.packedPayloadBytes : 0U) + TileXREpDedupAlign(uint64_t(owned) * k * 4U, 32U);
    if (kEpServerDedupEnable) {
        const uint32_t groups = ranks / 8U - 1U < k / 2U ? ranks / 8U - 1U : k / 2U;
        bytes += TileXREpDedupAlign(uint64_t(owned) * k * 4U, 32U) + TileXREpDedupAlign(uint64_t(owned) * 4U, 32U) +
            TileXREpDedupAlign(uint64_t(owned) * k * 16U, 32U) +
            TileXREpServerDedupServerStateBytes(ranks) + TileXREpDedupAlign(groups * 4U, 32U);
    }
    return bytes;
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
