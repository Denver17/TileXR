#ifndef TILEXR_EP_SERVER_DEDUP_RECEIVE_PACKET_H
#define TILEXR_EP_SERVER_DEDUP_RECEIVE_PACKET_H

#include "kernel_operator.h"
#include "ep_memory_server_dedup_data_layout.h"

namespace TileXREp {
using namespace AscendC;

template<HardEvent event>
__aicore__ inline void ServerDedupReceiveSync(TPipe *pipe)
{
    const TEventID id = pipe->FetchEventID(event);
    SetFlag<event>(id);
    WaitFlag<event>(id);
}

// One probe only: a miss returns to the caller without reading the payload or
// waiting for MTE3. Scratch sizes are blocks*32B, 32B and blocks*32B.
__aicore__ inline bool CheckServerDedupPayloadReady(GM_ADDR slot, uint32_t blocks,
    LocalTensor<float> flags, LocalTensor<float> sum, LocalTensor<float> work, TPipe *pipe)
{
    GlobalTensor<float> input;
    input.SetGlobalBuffer(reinterpret_cast<__gm__ float *>(slot + 480U));
    DataCopy(flags, input, DataCopyParams{uint16_t(blocks), 1U, 15U, 0U});
    ServerDedupReceiveSync<HardEvent::MTE2_V>(pipe);
    // Memory's strided Flag reduction, covering all eight words of each Flag
    // and every payload block, including blocks containing only metadata.
    ReduceSum(sum, flags, work, 8U, uint8_t(blocks), 1U);
    ServerDedupReceiveSync<HardEvent::V_S>(pipe);
    return sum.GetValue(0) == float(blocks * 8U);
}

// b32 Copy masks count elements; repeat strides count 32B blocks. Caller
// supplies distinct packed (blocks*512B) and unpacked (blocks*480B) buffers.
__aicore__ inline void UnpackServerDedupData(LocalTensor<uint8_t> payload,
    LocalTensor<uint8_t> wire, uint8_t blocks)
{
    auto output = payload.template ReinterpretCast<int32_t>();
    auto input = wire.template ReinterpretCast<int32_t>();
    Copy(output, input, uint64_t(64), blocks, {1, 1, 15, 16});
    Copy(output[64], input[64], uint64_t(56), blocks, {1, 1, 15, 16});
}

// Call after a successful full-packet probe. Metadata offsets are meaningful
// only in the unpacked payload; include its entire reserved tail, not just H.
__aicore__ inline void LoadServerDedupPayload(GM_ADDR slot, uint32_t blocks,
    LocalTensor<uint8_t> wire, LocalTensor<uint8_t> payload, TPipe *pipe)
{
    GlobalTensor<uint8_t> input;
    input.SetGlobalBuffer(slot);
    DataCopy(wire, input, blocks * 512U);
    ServerDedupReceiveSync<HardEvent::MTE2_V>(pipe);
    UnpackServerDedupData(payload, wire, uint8_t(blocks));
    ServerDedupReceiveSync<HardEvent::V_S>(pipe);
}

// meta is an independent 32B UB. The caller owns its S_MTE3 publication and
// lifetime. Valid terminal packets have one matching expert in [0,n).
__aicore__ inline void BuildServerDedupAssist(LocalTensor<uint32_t> meta,
    LocalTensor<uint32_t> tail, uint32_t myExpert, uint32_t relayRank, uint32_t relayIndex)
{
    for (uint32_t word = 0; word < 8U; ++word) meta.SetValue(word, tail.GetValue(word));
    const int32_t role = int32_t(tail.GetValue(4U));
    if (role > 0) {
        meta.SetValue(6U, relayRank);
        meta.SetValue(7U, relayIndex);
    } else if (role == -1) {
        const uint32_t entries = tail.GetValue(5U);
        for (uint32_t i = 0; i < entries; ++i) {
            const uint32_t entry = 8U + i * 4U;
            if (tail.GetValue(entry) != myExpert) continue;
            meta.SetValue(2U, tail.GetValue(entry + 2U));
            meta.SetValue(3U, tail.GetValue(entry + 3U));
            break;
        }
    }
}

// Only a first-hop owner builds a group; members preserve original weight bits.
__aicore__ inline void BuildServerDedupGroup(LocalTensor<uint32_t> group,
    LocalTensor<uint32_t> tail, uint32_t row, uint32_t relayIndex, uint32_t topK)
{
    group.SetValue(0U, row);
    for (uint32_t i = 0; i < tail.GetValue(5U); ++i) {
        const uint32_t entry = 8U + i * 4U;
        group.SetValue(1U + i * 2U, relayIndex * topK + tail.GetValue(entry + 2U));
        group.SetValue(2U + i * 2U, tail.GetValue(entry + 3U));
    }
}

// Prepare one common packed copy for the whole first-hop group. Once ready,
// all destination writes read it unchanged; buffer completion belongs to caller.
__aicore__ inline void PrepareServerDedupForward(LocalTensor<uint8_t> wire,
    LocalTensor<uint8_t> payload, const EpServerDedupDataLayout &layout,
    uint32_t relayRank, uint32_t relayIndex, TPipe *pipe)
{
    auto output = wire.template ReinterpretCast<int32_t>();
    auto input = payload.template ReinterpretCast<int32_t>();
    uint64_t mask[2] = {0xffU, 0U};
    Duplicate<float>(wire.template ReinterpretCast<float>()[120U], 1.0f,
        mask, uint8_t(layout.blockCount), 1U, 16U);
    Copy(output, input, uint64_t(64), uint8_t(layout.blockCount), {1, 1, 16, 15});
    Copy(output[64], input[64], uint64_t(56), uint8_t(layout.blockCount), {1, 1, 16, 15});
    ServerDedupReceiveSync<HardEvent::V_S>(pipe);
    // The 32B aligned header fits within a 480B payload block. Only its role
    // and return reference change; k/weight, entry count and all entries stay.
    const uint32_t header = uint32_t(layout.tailOffset / 480U * 512U + layout.tailOffset % 480U);
    auto tail = wire[header].template ReinterpretCast<uint32_t>();
    tail.SetValue(4U, UINT32_MAX);
    tail.SetValue(6U, relayRank);
    tail.SetValue(7U, relayIndex);
    ServerDedupReceiveSync<HardEvent::S_MTE3>(pipe);
}
} // namespace TileXREp

#endif
