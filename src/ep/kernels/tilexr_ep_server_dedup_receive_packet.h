#ifndef TILEXR_EP_SERVER_DEDUP_RECEIVE_PACKET_H
#define TILEXR_EP_SERVER_DEDUP_RECEIVE_PACKET_H

#include "kernel_operator.h"
#include "ep_memory_server_dedup_data_layout.h"

namespace TileXREp {
using namespace AscendC;

// Immediate events use different HardEvents from the fixed data lane IDs.
template<HardEvent event>
__aicore__ inline void ServerDedupReceiveSync()
{
    SetFlag<event>(0U);
    WaitFlag<event>(0U);
}

__aicore__ inline uint32_t ServerDedupWireOffset(uint32_t logical)
{
    return logical + logical / 480U * 32U;
}

// The 32B-aligned header and each 16B-aligned entry fit within one 480B
// payload block. The complete tail can cross Flag gaps, so map each entry
// separately. The caller establishes MTE2_S before Scalar reads.
class ServerDedupPacketTail {
public:
    __aicore__ inline ServerDedupPacketTail(LocalTensor<uint8_t> wire, uint32_t offset)
        : wire_(wire[offset / 480U * 512U]), withinBlock_(offset % 480U) {}
    __aicore__ inline LocalTensor<uint32_t> Header()
    {
        return wire_[withinBlock_].ReinterpretCast<uint32_t>();
    }
    __aicore__ inline LocalTensor<uint32_t> Entry(uint32_t index)
    {
        return wire_[ServerDedupWireOffset(withinBlock_ + 32U + index * 16U)].ReinterpretCast<uint32_t>();
    }
private:
    LocalTensor<uint8_t> wire_; // Origin is the block containing the header.
    uint32_t withinBlock_;
};

__aicore__ inline void ReadServerDedupFlags(GM_ADDR slot, uint32_t blocks, LocalTensor<float> flags)
{
    GlobalTensor<float> input;
    input.SetGlobalBuffer(reinterpret_cast<__gm__ float *>(slot + 480U));
    DataCopy(flags, input, DataCopyParams{uint16_t(blocks), 1U, 15U, 0U});
}

// Copy one logical output interval from the original packet, skipping Flag
// blocks. Logical starts are 32B aligned; GM starts and final lengths may not be.
__aicore__ inline void WriteServerDedupPayloadRange(GlobalTensor<uint8_t> output,
    LocalTensor<uint8_t> wire, uint32_t logical, uint32_t bytes)
{
    uint32_t written = 0U;
    const uint32_t within = logical % 480U;
    if (within != 0U && bytes != 0U) {
        const uint32_t available = 480U - within;
        const uint32_t head = bytes < available ? bytes : available;
        DataCopyPad(output, wire[ServerDedupWireOffset(logical)], {1U, head, 0U, 0U, 0U});
        logical += head; bytes -= head; written += head;
    }
    const uint32_t full = bytes / 480U;
    if (full != 0U) {
        DataCopyPad(output[written], wire[ServerDedupWireOffset(logical)],
            {uint16_t(full), 480U, 1U, 0U, 0U});
        logical += full * 480U; bytes -= full * 480U; written += full * 480U;
    }
    if (bytes != 0U)
        DataCopyPad(output[written], wire[ServerDedupWireOffset(logical)], {1U, bytes, 0U, 0U, 0U});
}

__aicore__ inline void BuildServerDedupAssist(LocalTensor<uint32_t> meta,
    ServerDedupPacketTail tail, int32_t role, uint32_t myExpert, uint32_t relayRank, uint32_t relayIndex)
{
    auto header = tail.Header();
    const uint32_t entries = header.GetValue(5U);
    auto route = header;
    if (role == -1) {
        for (uint32_t i = 0; i < entries; ++i) {
            auto entry = tail.Entry(i);
            if (entry.GetValue(0U) != myExpert) continue;
            route = entry;
            break;
        }
    }
    meta.SetValue(0U, header.GetValue(0U));
    meta.SetValue(1U, header.GetValue(1U));
    meta.SetValue(2U, route.GetValue(2U));
    meta.SetValue(3U, route.GetValue(3U));
    meta.SetValue(4U, uint32_t(role));
    meta.SetValue(5U, entries);
    meta.SetValue(6U, role > 0 ? relayRank : header.GetValue(6U));
    meta.SetValue(7U, role > 0 ? relayIndex : header.GetValue(7U));
}

__aicore__ inline void BuildServerDedupGroup(LocalTensor<uint32_t> group,
    ServerDedupPacketTail tail, uint32_t row, uint32_t relayIndex, uint32_t topK)
{
    group.SetValue(0U, row);
    const uint32_t entries = tail.Header().GetValue(5U);
    const uint32_t returnBase = relayIndex * topK;
    for (uint32_t i = 0; i < entries; ++i) {
        auto entry = tail.Entry(i);
        group.SetValue(1U + i * 2U, returnBase + entry.GetValue(2U));
        group.SetValue(2U + i * 2U, entry.GetValue(3U));
    }
}

// Save first-hop metadata before changing this same packet. All subsequent
// destination writes consume it unchanged until the lane completion event.
__aicore__ inline void PrepareServerDedupForward(ServerDedupPacketTail tail,
    uint32_t relayRank, uint32_t relayIndex)
{
    auto header = tail.Header();
    header.SetValue(4U, UINT32_MAX);
    header.SetValue(6U, relayRank);
    header.SetValue(7U, relayIndex);
}
} // namespace TileXREp
#endif
