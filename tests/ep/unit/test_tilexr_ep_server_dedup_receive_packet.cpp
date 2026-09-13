#include <cstring>
#include <vector>
#include "tilexr_ep_server_dedup_recv.h"
#include "server_dedup_test_checks.h"

namespace AscendC { uint32_t testCore = 0; uint64_t testCopies = 0; }
namespace {
using namespace TileXREp;
using ServerDedupTest::Check;

struct Packet {
    EpServerDedupDataLayout layout;
    uint32_t k, entries;
    std::vector<uint8_t> payload, wire;
    Packet(uint32_t h, uint32_t topK, bool quantized, int32_t role) :
        layout(TileXREpServerDedupDataLayout(2, h, topK, 96, quantized)), k(topK),
        entries(role == 0 ? 0U : (topK == 6U ? 3U : topK - 1U)),
        payload(layout.packedPayloadBytes), wire(layout.tokenStride)
    {
        for (uint32_t i = 0; i < payload.size(); ++i) payload[i] = uint8_t((i * 37U + i / 480U) % 251U);
        const uint32_t header[] = {2U, 1U, 0U, 0x3eaaaaabU,
            role > 0 ? entries : uint32_t(role), entries,
            role == -1 ? 8U : UINT32_MAX, role == -1 ? 7U : UINT32_MAX};
        std::memcpy(payload.data() + layout.tailOffset, header, sizeof(header));
        for (uint32_t i = 0; i < entries; ++i) {
            const uint32_t entry[] = {60U + i, i % 2U, topK == 6U ? 1U + 2U * i : i + 1U,
                0x3f000001U + i * 0x101U};
            std::memcpy(payload.data() + layout.tailOffset + 32U + i * 16U, entry, sizeof(entry));
        }
        Pack();
    }
    uint32_t Word(uint32_t i) const {
        uint32_t value;
        std::memcpy(&value, payload.data() + layout.tailOffset + i * 4U, 4U);
        return value;
    }
    void SetWord(uint32_t i, uint32_t value) {
        std::memcpy(payload.data() + layout.tailOffset + i * 4U, &value, 4U);
    }
    void Pack() {
        for (uint32_t block = 0; block < layout.blockCount; ++block) {
            std::memcpy(wire.data() + block * 512U, payload.data() + block * 480U, 480U);
            for (uint32_t word = 0; word < 8U; ++word) SetFlagWord(block, word, 0x3f800000U);
        }
    }
    void SetFlagWord(uint32_t block, uint32_t word, uint32_t bits) {
        std::memcpy(wire.data() + block * 512U + 480U + word * 4U, &bits, 4U);
    }
};

void CheckPacket(uint32_t h, uint32_t k, bool quantized)
{
    Packet fixture(h, k, quantized, k == 1 ? 0 : 1);
    const auto &layout = fixture.layout;
    AscendC::TPipe pipe;
    AscendC::TBuf<> flagsBuf, sumBuf, workBuf, wireBuf, payloadBuf, metaBuf, groupBuf, forwardBuf;
    pipe.InitBuffer(flagsBuf, layout.blockCount * 32U);
    pipe.InitBuffer(sumBuf, 32U);
    pipe.InitBuffer(workBuf, layout.blockCount * 32U);
    pipe.InitBuffer(wireBuf, layout.tokenStride);
    pipe.InitBuffer(payloadBuf, layout.packedPayloadBytes + 64U);
    pipe.InitBuffer(metaBuf, 96U);
    const uint32_t groupStride = uint32_t(TileXREpDedupAlign(4U + 8U * (k - 1U), 32U));
    pipe.InitBuffer(groupBuf, groupStride + 64U);
    pipe.InitBuffer(forwardBuf, layout.tokenStride + 64U);
    auto flags = flagsBuf.Get<float>(), sum = sumBuf.Get<float>(), work = workBuf.Get<float>();
    auto wire = wireBuf.Get<uint8_t>(), payload = payloadBuf.Get<uint8_t>()[32U];
    auto meta = metaBuf.Get<uint32_t>()[8U], group = groupBuf.Get<uint32_t>()[8U];
    auto forward = forwardBuf.Get<uint8_t>()[32U];

    // An unrelated send event remains outstanding across all probes.
    AscendC::SetFlag<AscendC::HardEvent::MTE3_V>(7U);
    for (uint32_t block = 0; block < layout.blockCount; ++block) {
        for (uint32_t word : {0U, 7U}) {
            fixture.SetFlagWord(block, word, 0U);
            Check(!CheckServerDedupPayloadReady(fixture.wire.data(), layout.blockCount, flags, sum, work, &pipe),
                "one missing flag word, including tail-only blocks, keeps the packet pending");
            fixture.SetFlagWord(block, word, 0x3f800000U);
        }
    }
    Check(AscendC::Events<AscendC::HardEvent::MTE3_V>()[7U], "probing never consumes an unrelated send event");
    AscendC::WaitFlag<AscendC::HardEvent::MTE3_V>(7U);
    Check(CheckServerDedupPayloadReady(fixture.wire.data(), layout.blockCount, flags, sum, work, &pipe),
        "all payload and metadata flags make the complete packet ready");
    Check(std::all_of(payloadBuf.storage->bytes.begin(), payloadBuf.storage->bytes.end(),
        [](uint8_t v) { return v == 0xcd; }), "probes leave payload storage untouched");
    LoadServerDedupPayload(fixture.wire.data(), layout.blockCount, wire, payload, &pipe);
    Check(std::memcmp(payload.Data(fixture.payload.size()), fixture.payload.data(), fixture.payload.size()) == 0,
        "load strips every flag and restores hidden, quantization scales and the complete tail");
    auto tail = payload[layout.tailOffset].ReinterpretCast<uint32_t>();
    const auto originalPayload = payloadBuf.storage->bytes;

    BuildServerDedupAssist(meta, tail, 48U, 8U, 7U);
    for (uint32_t word = 0; word < 8U; ++word) {
        const uint32_t expected = fixture.entries && word >= 6U ? (word == 6U ? 8U : 7U) : fixture.Word(word);
        Check(meta.GetValue(word) == expected, "DIRECT/first-hop assist preserves identity and assigns only the relay reference");
    }
    if (fixture.entries) {
        BuildServerDedupGroup(group, tail, 19U, 7U, k);
        Check(group.GetValue(0) == 19U, "group keeps the first-hop output row, not source token or q");
        for (uint32_t i = 0; i < fixture.entries; ++i) {
            Check(group.GetValue(1U + 2U * i) == 7U * k + fixture.Word(10U + 4U * i),
                "dense group members keep their sparse original-k return slots");
            Check(group.GetValue(2U + 2U * i) == fixture.Word(11U + 4U * i), "group preserves exact FP32 weight bits");
        }
        PrepareServerDedupForward(forward, payload, layout, 8U, 7U, &pipe);
        auto expected = fixture;
        expected.SetWord(4U, UINT32_MAX); expected.SetWord(6U, 8U); expected.SetWord(7U, 7U); expected.Pack();
        Check(std::memcmp(forward.Data(expected.wire.size()), expected.wire.data(), expected.wire.size()) == 0,
            "one common forwarded packet changes only role/return reference and retains all flags and entries");
        Check(payloadBuf.storage->bytes == originalPayload, "first-hop metadata and forwarding leave input payload read-only");
        LoadServerDedupPayload(expected.wire.data(), layout.blockCount, wire, payload, &pipe);
        tail = payload[layout.tailOffset].ReinterpretCast<uint32_t>();
        for (uint32_t i = 0; i < fixture.entries; ++i) {
            BuildServerDedupAssist(meta, tail, fixture.Word(8U + 4U * i), 99U, 99U);
            for (uint32_t word = 0; word < 8U; ++word) {
                const uint32_t value = word == 2U ? fixture.Word(10U + 4U * i) :
                    word == 3U ? fixture.Word(11U + 4U * i) : expected.Word(word);
                Check(meta.GetValue(word) == value, "terminal assist selects its expert entry and retains the common return reference");
            }
        }
        Check(std::memcmp(payload.Data(expected.payload.size()), expected.payload.data(), expected.payload.size()) == 0,
            "terminal metadata extraction leaves the common payload read-only");
    } else Check(payloadBuf.storage->bytes == originalPayload, "DIRECT metadata extraction leaves payload read-only");
    for (const auto *buffer : {&payloadBuf, &metaBuf, &groupBuf, &forwardBuf}) {
        const auto &bytes = buffer->storage->bytes;
        Check(std::all_of(bytes.begin(), bytes.begin() + 32U, [](uint8_t v) { return v == 0xcd; }) &&
            std::all_of(bytes.end() - 32U, bytes.end(), [](uint8_t v) { return v == 0xcd; }),
            "packet and metadata operations preserve both UB guards");
    }
}

template<bool Quantized>
void CheckStagedReceiver(int32_t role)
{
    constexpr uint32_t h = 209U, k = 6U, rank = 8U, world = 16U, experts = 96U, bs = 2U;
    Packet fixture(h, k, Quantized, role);
    if (role > 0) {
        fixture.SetWord(8U, 49U); fixture.SetWord(9U, 0U); // Same-card relay to another expert.
        fixture.SetWord(12U, 54U); fixture.SetWord(13U, 0U);
        fixture.SetWord(16U, 55U); fixture.SetWord(17U, 1U);
    } else if (role == -1) fixture.SetWord(16U, 48U); // Match the final entry, not header k/weight.
    fixture.Pack();
    const auto &data = fixture.layout;
    const auto counts = TileXREpServerDedupCountLayout(experts);
    std::vector<uint8_t> local(data.dispatchOffset + data.dispatchBytes + 32U, 0);
    std::vector<uint8_t> peer(local.size(), 0);
    const auto slot = TileXREpServerDedupDispatchOffset(data, 2U, rank, 0U, 0U, world, 6U, bs);
    std::memcpy(local.data() + slot, fixture.wire.data(), fixture.wire.size());
    auto *prefix = reinterpret_cast<uint32_t *>(local.data() + counts.cntOffset + counts.prefixOffset + 15U * counts.prefixStride);
    std::fill(prefix + 2U, prefix + experts, 1U); // One row, owned by receive core 15.
    const uint32_t initialCounter = 7U;
    std::memcpy(local.data() + counts.counterOffset, &initialCounter, 4U);
    TileXR::CommArgs args {};
    args.rank = rank; args.rankSize = world;
    args.peerMems[rank] = local.data(); args.peerMems[9U] = peer.data();
    const uint32_t hiddenBytes = Quantized ? h : h * 2U;
    std::vector<uint8_t> hidden(hiddenBytes + 64U, 0xcd), scales(data.scaleBytes + 64U, 0xcd);
    std::vector<uint32_t> assist(24U, 0xcdcdcdcdU);
    const uint32_t groupStride = uint32_t(TileXREpDedupAlign(4U + 8U * (k - 1U), 32U));
    std::vector<uint32_t> groups((8U * bs * groupStride + 64U) / 4U, 0xcdcdcdcdU);
    AscendC::testCore = 63U;
    AscendC::TPipe pipe;
    ServerDedupReceiver<uint16_t, Quantized, false> receiver;
    receiver.Init(&args, hidden.data() + 32U, scales.data() + 32U,
        reinterpret_cast<uint8_t *>(assist.data() + 8U), reinterpret_cast<uint8_t *>(groups.data() + 8U),
        bs, h, k, experts, &pipe);
    uint32_t sends = 0;
    uint64_t sendSync = 0;
    std::vector<uint8_t> firstCopy;
    AscendC::GmWriteObserver() = [&](const void *, const void *bytes, uint32_t size) {
        Check(size == data.tokenStride, "each staged relay sends the complete packet");
        if (sends == 0) {
            sendSync = AscendC::TestSyncOperations();
            firstCopy.assign(static_cast<const uint8_t *>(bytes), static_cast<const uint8_t *>(bytes) + size);
        }
        Check(sendSync == AscendC::TestSyncOperations(), "a Token's relay writes have no intervening event or barrier");
        Check(std::memcmp(firstCopy.data(), bytes, size) == 0, "all destinations receive an identical common packet");
        ++sends;
    };
    receiver.Process();
    AscendC::GmWriteObserver() = {};
    Check(sends == (role > 0 ? fixture.entries : 0U), "DIRECT and terminal rows do not start relay groups");
    Check(std::memcmp(hidden.data() + 32U, fixture.payload.data(), hiddenBytes) == 0, "staged receiver writes the exact hidden row");
    if (Quantized) Check(std::memcmp(scales.data() + 32U, fixture.payload.data() + data.quantCount, data.scaleBytes) == 0,
        "staged receiver writes MXFP8 scale bytes from their logical payload offset");
    for (uint32_t word = 0; word < 8U; ++word) {
        uint32_t expected = fixture.Word(word);
        if (role > 0 && word >= 6U) expected = word == 6U ? rank : initialCounter;
        if (role == -1 && word == 2U) expected = fixture.Word(18U);
        if (role == -1 && word == 3U) expected = fixture.Word(19U);
        Check(assist[8U + word] == expected, "staged receiver writes a complete route-specific assist row");
    }
    uint32_t counter;
    std::memcpy(&counter, local.data() + counts.counterOffset, 4U);
    Check(counter == initialCounter + (role > 0), "only a first-hop owner allocates one group number");
    if (role > 0) {
        const uint32_t row = 8U + initialCounter * groupStride / 4U;
        Check(groups[row] == 0U, "group byte stride is converted to uint32 element indexing");
        for (uint32_t i = 0; i < fixture.entries; ++i) {
            Check(groups[row + 1U + i * 2U] == initialCounter * k + fixture.Word(10U + i * 4U) &&
                groups[row + 2U + i * 2U] == fixture.Word(11U + i * 4U), "staged group contains dense original-k/weight members");
            const uint32_t expert = fixture.Word(8U + i * 4U), q = fixture.Word(9U + i * 4U);
            const auto address = TileXREpServerDedupDispatchOffset(data, 2U, expert / 6U, expert % 6U, q, world, 6U, bs);
            auto expected = fixture;
            expected.SetWord(4U, UINT32_MAX); expected.SetWord(6U, rank); expected.SetWord(7U, initialCounter); expected.Pack();
            Check(std::memcmp(args.peerMems[expert / 6U] + address, expected.wire.data(), data.tokenStride) == 0,
                "local and peer relay slots retain their complete common packet and ready flags");
        }
    } else Check(std::all_of(groups.begin(), groups.end(), [](uint32_t v) { return v == 0xcdcdcdcdU; }),
        "non-first-hop packets leave group storage untouched");
    for (uint32_t block = 0; block < data.blockCount; ++block)
        Check(std::all_of(local.begin() + slot + block * 512U + 480U, local.begin() + slot + (block + 1U) * 512U,
            [](uint8_t v) { return v == 0; }), "staged serial cleanup touches the consumed input flags");
    for (const auto *bytes : {&hidden, &scales})
        Check(std::all_of(bytes->begin(), bytes->begin() + 32U, [](uint8_t v) { return v == 0xcd; }) &&
            std::all_of(bytes->end() - 32U, bytes->end(), [](uint8_t v) { return v == 0xcd; }), "output writes preserve guards");
    for (const auto *words : {&assist, &groups})
        Check(std::all_of(words->begin(), words->begin() + 8U, [](uint32_t v) { return v == 0xcdcdcdcdU; }) &&
            std::all_of(words->end() - 8U, words->end(), [](uint32_t v) { return v == 0xcdcdcdcdU; }),
            "assist and group writes preserve both output guards");
}
} // namespace

int main()
{
    try {
        CheckPacket(1U, 1U, false);
        for (bool quantized : {false, true}) {
            for (uint32_t h : {209U, 225U, 7168U}) CheckPacket(h, 6U, quantized);
            CheckPacket(225U, 32U, quantized);
        }
        for (int32_t role : {0, 1, -1}) {
            CheckStagedReceiver<false>(role);
            CheckStagedReceiver<true>(role);
        }
    } catch (const std::exception &error) {
        AscendC::GmWriteObserver() = {};
        std::cerr << error.what() << '\n';
        return 1;
    }
    return ServerDedupTest::Finish("receive_packet");
}
