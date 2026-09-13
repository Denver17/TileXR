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
    return logical / 480U * 512U + logical % 480U;
}

// All words and entries are individually aligned; the complete tail need not
// be physically contiguous. The caller establishes MTE2_S before Scalar reads.
class ServerDedupPacketTail {
public:
    __aicore__ inline ServerDedupPacketTail(LocalTensor<uint8_t> wire, uint32_t offset)
        : wire_(wire), offset_(offset) {}
    __aicore__ inline uint32_t Word(uint32_t index)
    {
        return wire_[ServerDedupWireOffset(offset_ + index * 4U)].ReinterpretCast<uint32_t>().GetValue(0);
    }
    __aicore__ inline void SetWord(uint32_t index, uint32_t value)
    {
        wire_[ServerDedupWireOffset(offset_ + index * 4U)].ReinterpretCast<uint32_t>().SetValue(0, value);
    }
private:
    LocalTensor<uint8_t> wire_;
    uint32_t offset_;
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
    ServerDedupPacketTail tail, uint32_t myExpert, uint32_t relayRank, uint32_t relayIndex)
{
    for (uint32_t word = 0; word < 8U; ++word) meta.SetValue(word, tail.Word(word));
    const int32_t role = int32_t(tail.Word(4U));
    if (role > 0) {
        meta.SetValue(6U, relayRank);
        meta.SetValue(7U, relayIndex);
    } else if (role == -1) {
        for (uint32_t i = 0; i < tail.Word(5U); ++i) {
            const uint32_t entry = 8U + i * 4U;
            if (tail.Word(entry) != myExpert) continue;
            meta.SetValue(2U, tail.Word(entry + 2U));
            meta.SetValue(3U, tail.Word(entry + 3U));
            break;
        }
    }
}

__aicore__ inline void BuildServerDedupGroup(LocalTensor<uint32_t> group,
    ServerDedupPacketTail tail, uint32_t row, uint32_t relayIndex, uint32_t topK)
{
    group.SetValue(0U, row);
    for (uint32_t i = 0; i < tail.Word(5U); ++i) {
        const uint32_t entry = 8U + i * 4U;
        group.SetValue(1U + i * 2U, relayIndex * topK + tail.Word(entry + 2U));
        group.SetValue(2U + i * 2U, tail.Word(entry + 3U));
    }
}

// Save first-hop metadata before changing this same packet. All subsequent
// destination writes consume it unchanged until the lane completion event.
__aicore__ inline void PrepareServerDedupForward(ServerDedupPacketTail tail,
    uint32_t relayRank, uint32_t relayIndex)
{
    tail.SetWord(4U, UINT32_MAX);
    tail.SetWord(6U, relayRank);
    tail.SetWord(7U, relayIndex);
}
} // namespace TileXREp
#endif
