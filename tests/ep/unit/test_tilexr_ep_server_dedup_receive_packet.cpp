#include <cstring>
#include <vector>
#include <set>
#include "tilexr_ep_server_dedup_recv.h"
#include "server_dedup_test_checks.h"
#include "server_dedup_test_memory.h"
#include "server_dedup_profile_capture.h"

namespace AscendC { uint32_t testCore = 0; uint64_t testCopies = 0; }
namespace {
using namespace TileXREp;
using ServerDedupTest::Check;

void CheckReceiveProfile(ServerDedupProfile &profile, const ServerDedupTest::ProfileCapture &capture,
    uint32_t rows, uint32_t firstHops)
{
    const auto syncBefore = AscendC::TestSyncOperations();
    profile.Publish();
    Check(AscendC::TestSyncOperations() == syncBefore, "profile publication adds no synchronization");
    const auto events = capture.Events(AscendC::testCore);
#if defined(TILEXR_ENABLE_PROFILING)
    Check(!events.empty() && events.front() == kEpServerDedupDetailEventBase, "receive begin is retained");
    const auto probes = std::count(events.begin(), events.end(), kEpServerDedupOperationEventBase);
    Check(rows == 0U ? probes == 0 : probes > 0, "probe samples reflect actual work");
    const uint32_t expected[] = {uint32_t(probes), 0U, rows, firstHops, rows, firstHops, 0U,
        rows, rows, uint32_t(probes), uint32_t(probes)};
    for (uint32_t op = 0; op < sizeof(expected) / sizeof(expected[0]); ++op) {
        const auto begin = std::count(events.begin(), events.end(), kEpServerDedupOperationEventBase + 2U * op);
        const auto end = std::count(events.begin(), events.end(), kEpServerDedupOperationEventBase + 2U * op + 1U);
        Check(begin == end, "each observed receive operation retains both timestamps");
        Check(begin == expected[op], "every repeated operation is recorded without collapsing samples");
    }
    std::vector<uint32_t> active;
    for (const auto event : events) {
        if (event < kEpServerDedupOperationEventBase) continue;
        const uint32_t offset = event - kEpServerDedupOperationEventBase;
        const uint32_t op = offset / 2U;
        if (offset % 2U == 0U) {
            if (op == uint32_t(EpDedupDetailMetric::ProbeLoadFlag) || op == uint32_t(EpDedupDetailMetric::ProbeCalSum))
                Check(!active.empty() && active.back() == uint32_t(EpDedupDetailMetric::Probe),
                    "flag loading and sum/ready comparison remain nested within Probe");
            active.push_back(op);
        } else {
            Check(!active.empty() && active.back() == op, "receive intervals close in nested order");
            active.pop_back();
        }
    }
    Check(active.empty(), "no receive interval remains open");
    if (rows == 0U) Check(events.size() == 4U, "empty receive contains only real prefix/task/prepare observations");
    else Check(events.back() == kEpServerDedupDetailEventBase + 7U, "clear completion remains the final receive point");
#else
    (void)rows; (void)firstHops;
    Check(events.empty(), "OFF builds emit no profiling data");
#endif
}

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
    AscendC::TBuf<> wireBuf, metaBuf, groupBuf;
    pipe.InitBuffer(wireBuf, layout.tokenStride + 64U);
    pipe.InitBuffer(metaBuf, 96U);
    const uint32_t groupStride = uint32_t(TileXREpDedupAlign(4U + 8U * (k - 1U), 32U));
    pipe.InitBuffer(groupBuf, groupStride + 64U);
    auto wire = wireBuf.Get<uint8_t>()[32U];
    auto meta = metaBuf.Get<uint32_t>()[8U], group = groupBuf.Get<uint32_t>()[8U];
    std::memcpy(wire.Data(fixture.wire.size()), fixture.wire.data(), fixture.wire.size());
    ServerDedupPacketTail tail(wire, uint32_t(layout.tailOffset));
    const auto originalWire = wireBuf.storage->bytes;

    // Device output ranges include block tails and MXFP8 scale starts inside a block.
    for (const auto &range : {std::pair<uint32_t,uint32_t>{0U, uint32_t(fixture.payload.size())},
        {uint32_t(layout.quantCount), uint32_t(layout.scaleBytes)},
        {32U, uint32_t(fixture.payload.size()) - 32U}}) {
        std::vector<uint8_t> output(range.second + 64U, 0xcd);
        AscendC::GlobalTensor<uint8_t> gm;
        gm.SetGlobalBuffer(output.data() + 32U);
        WriteServerDedupPayloadRange(gm, wire, range.first, range.second);
        Check(std::memcmp(output.data() + 32U, fixture.payload.data() + range.first, range.second) == 0,
            "direct UB-to-GM range strips Flag blocks and copies only valid bytes");
        Check(std::all_of(output.begin(), output.begin() + 32U, [](uint8_t v) { return v == 0xcd; }) &&
            std::all_of(output.end() - 32U, output.end(), [](uint8_t v) { return v == 0xcd; }), "range output guards");
    }
    BuildServerDedupAssist(meta, tail, int32_t(tail.Header().GetValue(4U)), 48U, 8U, 7U);
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
        Check(wireBuf.storage->bytes == originalWire, "metadata extraction leaves input unchanged");
        PrepareServerDedupForward(tail, 8U, 7U);
        auto expected = fixture;
        expected.SetWord(4U, UINT32_MAX); expected.SetWord(6U, 8U); expected.SetWord(7U, 7U); expected.Pack();
        Check(std::memcmp(wire.Data(expected.wire.size()), expected.wire.data(), expected.wire.size()) == 0,
            "in-place common forwarded packet changes only role/return reference and retains all flags and entries");
        for (uint32_t i = 0; i < fixture.entries; ++i) {
            BuildServerDedupAssist(meta, tail, int32_t(tail.Header().GetValue(4U)), fixture.Word(8U + 4U * i), 99U, 99U);
            for (uint32_t word = 0; word < 8U; ++word) {
                const uint32_t value = word == 2U ? fixture.Word(10U + 4U * i) :
                    word == 3U ? fixture.Word(11U + 4U * i) : expected.Word(word);
                Check(meta.GetValue(word) == value, "terminal assist selects its expert entry and retains the common return reference");
            }
        }
        Check(std::memcmp(wire.Data(expected.wire.size()), expected.wire.data(), expected.wire.size()) == 0,
            "terminal metadata extraction leaves the common payload read-only");
    } else Check(wireBuf.storage->bytes == originalWire, "DIRECT metadata extraction leaves payload read-only");
    for (const auto *buffer : {&wireBuf, &metaBuf, &groupBuf}) {
        const auto &bytes = buffer->storage->bytes;
        Check(std::all_of(bytes.begin(), bytes.begin() + 32U, [](uint8_t v) { return v == 0xcd; }) &&
            std::all_of(bytes.end() - 32U, bytes.end(), [](uint8_t v) { return v == 0xcd; }),
            "packet and metadata operations preserve both UB guards");
    }
}

template<bool Quantized>
void CheckReceiver(int32_t role, int64_t magic)
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
    // Keep the opposite bank poisoned; prefix, counter, relay and flag cleanup
    // must all follow the same magic-selected address above the 4 GiB boundary.
    const uint32_t bank = uint32_t(magic) % 2U;
    std::vector<uint8_t> otherLocal(local.size(), 0xa5), otherPeer(peer.size(), 0xa5);
    ServerDedupTest::SparsePeerMemory memory;
    memory.Add(rank, bank, local.data(), local.size());
    memory.Add(9U, bank, peer.data(), peer.size());
    memory.Add(rank, 1U - bank, otherLocal.data(), otherLocal.size());
    memory.Add(9U, 1U - bank, otherPeer.data(), otherPeer.size());
    args.peerMems[rank] = reinterpret_cast<uint8_t *>(ServerDedupTest::SparsePeerMemory::Base(rank));
    args.peerMems[9U] = reinterpret_cast<uint8_t *>(ServerDedupTest::SparsePeerMemory::Base(9U));
    args.peerMemBytes = kEpServerDedupPeerBytes;
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
        bs, h, k, experts, &pipe, magic);
    uint32_t sends = 0;
    uint64_t sendSync = 0;
    std::vector<uint8_t> firstCopy;
    AscendC::GmWriteObserver() = [&](const void *, const void *bytes, uint32_t size) {
        Check(size == data.tokenStride, "each relay sends the complete packet");
        if (sends == 0) {
            sendSync = AscendC::TestSyncOperations();
            firstCopy.assign(static_cast<const uint8_t *>(bytes), static_cast<const uint8_t *>(bytes) + size);
        }
        Check(sendSync == AscendC::TestSyncOperations(), "a Token's relay writes have no intervening event or barrier");
        Check(std::memcmp(firstCopy.data(), bytes, size) == 0, "all destinations receive an identical common packet");
        ++sends;
    };
    ServerDedupTest::ProfileCapture capture(args);
    ServerDedupProfile profile;
    profile.Init(reinterpret_cast<GM_ADDR>(&args), 0U);
    receiver.Process(profile);
    AscendC::GmWriteObserver() = {};
    CheckReceiveProfile(profile, capture, 1U, role > 0 ? 1U : 0U);
    Check(sends == (role > 0 ? fixture.entries : 0U), "DIRECT and terminal rows do not start relay groups");
    Check(std::memcmp(hidden.data() + 32U, fixture.payload.data(), hiddenBytes) == 0, "receiver writes the exact hidden row");
    if (Quantized) Check(std::memcmp(scales.data() + 32U, fixture.payload.data() + data.quantCount, data.scaleBytes) == 0,
        "receiver writes MXFP8 scale bytes from their logical payload offset");
    for (uint32_t word = 0; word < 8U; ++word) {
        uint32_t expected = fixture.Word(word);
        if (role > 0 && word >= 6U) expected = word == 6U ? rank : initialCounter;
        if (role == -1 && word == 2U) expected = fixture.Word(18U);
        if (role == -1 && word == 3U) expected = fixture.Word(19U);
        Check(assist[8U + word] == expected, "receiver writes a complete route-specific assist row");
    }
    uint32_t counter;
    std::memcpy(&counter, local.data() + counts.counterOffset, 4U);
    Check(counter == initialCounter + (role > 0), "only a first-hop owner allocates one group number");
    if (role > 0) {
        const uint32_t row = 8U + initialCounter * groupStride / 4U;
        Check(groups[row] == 0U, "group byte stride is converted to uint32 element indexing");
        for (uint32_t i = 0; i < fixture.entries; ++i) {
            Check(groups[row + 1U + i * 2U] == initialCounter * k + fixture.Word(10U + i * 4U) &&
                groups[row + 2U + i * 2U] == fixture.Word(11U + i * 4U), "group contains dense original-k/weight members");
            const uint32_t expert = fixture.Word(8U + i * 4U), q = fixture.Word(9U + i * 4U);
            const auto address = TileXREpServerDedupDispatchOffset(data, 2U, expert / 6U, expert % 6U, q, world, 6U, bs);
            auto expected = fixture;
            expected.SetWord(4U, UINT32_MAX); expected.SetWord(6U, rank); expected.SetWord(7U, initialCounter); expected.Pack();
            Check(std::memcmp((expert / 6U == rank ? local.data() : peer.data()) + address, expected.wire.data(), data.tokenStride) == 0,
                "local and peer relay slots retain their complete common packet and ready flags");
        }
    } else Check(std::all_of(groups.begin(), groups.end(), [](uint32_t v) { return v == 0xcdcdcdcdU; }),
        "non-first-hop packets leave group storage untouched");
    for (uint32_t block = 0; block < data.blockCount; ++block)
        Check(std::all_of(local.begin() + slot + block * 512U + 480U, local.begin() + slot + (block + 1U) * 512U,
            [](uint8_t v) { return v == 0; }), "kernel-end cleanup touches the consumed input flags");
    for (const auto *bytes : {&hidden, &scales})
        Check(std::all_of(bytes->begin(), bytes->begin() + 32U, [](uint8_t v) { return v == 0xcd; }) &&
            std::all_of(bytes->end() - 32U, bytes->end(), [](uint8_t v) { return v == 0xcd; }), "output writes preserve guards");
    for (const auto *words : {&assist, &groups})
        Check(std::all_of(words->begin(), words->begin() + 8U, [](uint32_t v) { return v == 0xcdcdcdcdU; }) &&
            std::all_of(words->end() - 8U, words->end(), [](uint32_t v) { return v == 0xcdcdcdcdU; }),
            "assist and group writes preserve both output guards");
    Check(std::all_of(otherLocal.begin(), otherLocal.end(), [](uint8_t v) { return v == 0xa5; }) &&
        std::all_of(otherPeer.begin(), otherPeer.end(), [](uint8_t v) { return v == 0xa5; }),
        "receiver leaves the other bank's prefix, counter, payload and flags untouched");
}
// CPU hooks model buffer ownership, not asynchronous device execution. Keep
// each issued MTE3 source snapshot until the corresponding lane wait, so an
// early Scalar overwrite of packet/meta/group storage is observable.
template<bool Quantized>
void CheckBatchReceiver(uint32_t h, uint32_t expectedLanes)
{
    constexpr uint32_t bs = 64U, k = 6U, experts = 96U, world = 16U, rank = 8U, owned = 24U;
    const auto data = TileXREpServerDedupDataLayout(bs, h, k, experts, Quantized);
    const auto counts = TileXREpServerDedupCountLayout(experts);
    const auto start = TileXREpServerDedupDispatchOffset(data, 0U, rank, 0U, 0U, world, 6U, bs);
    std::vector<uint8_t> local(start + bs * data.tokenStride * 4U + 32U, 0xa5);
    auto *prefix = reinterpret_cast<uint32_t *>(local.data() + counts.cntOffset + counts.prefixOffset);
    for (uint32_t i = 0; i < experts; ++i) prefix[i] = (i / world + 1U) * bs;
    uint32_t zero = 0;
    std::memcpy(local.data() + counts.counterOffset, &zero, 4U);
    std::vector<Packet> fixtures;
    for (uint32_t q = 0; q < bs; ++q) {
        fixtures.emplace_back(h, k, Quantized, q % 3U == 0U ? 1 : 0);
        auto &packet = fixtures.back();
        packet.SetWord(0U, 0U); packet.SetWord(1U, q);
        packet.payload[0] = uint8_t(q);
        for (uint32_t entry = 0; entry < packet.entries; ++entry) {
            packet.SetWord(8U + entry * 4U, 49U + entry);
            packet.SetWord(9U + entry * 4U, q);
        }
        packet.Pack();
        std::memcpy(local.data() + start + q * data.tokenStride, packet.wire.data(), data.tokenStride);
        if (q < 16U) {
            // A single missing word anywhere, including tail-only blocks,
            // must defer the whole packet; later rows are already available.
            const uint32_t block = q % 2U ? uint32_t(data.blockCount) - 1U : q / 2U % data.blockCount;
            const uint32_t word = q % 8U;
            std::memcpy(local.data() + start + q * data.tokenStride + block * 512U + 480U + word * 4U, &zero, 4U);
        }
    }
    TileXR::CommArgs args {};
    args.rank = rank; args.rankSize = world; args.peerMems[rank] = local.data();
    const uint32_t hiddenBytes = Quantized ? h : 2U * h;
    const uint32_t groupStride = uint32_t(TileXREpDedupAlign(4U + 8U * (k - 1U), 32U));
    const uint32_t fixed = experts * 4U + 2U * 96U + uint32_t(data.blockCount) * 544U + 576U;
    const uint32_t lanes = std::min(8U, (190U * 1024U - fixed) / (uint32_t(data.tokenStride) + 32U + groupStride));
    Check(lanes == expectedLanes, "fixture exercises the intended physical lane count");
    const uint32_t depth = std::min(2U, lanes);
    std::vector<uint8_t> hidden(owned * hiddenBytes + 64U, 0xcd), scales(owned * data.scaleBytes + 64U, 0xcd);
    std::vector<uint32_t> assist(owned * 8U + 16U, 0xcdcdcdcdU);
    std::vector<uint32_t> groups((8U * bs * groupStride + 64U) / 4U, 0xcdcdcdcdU);
    AscendC::testCore = 48U;
    AscendC::TPipe pipe;
    ServerDedupReceiver<uint16_t, Quantized, false> receiver;
    receiver.Init(&args, hidden.data() + 32U, scales.data() + 32U,
        reinterpret_cast<uint8_t *>(assist.data() + 8U), reinterpret_cast<uint8_t *>(groups.data() + 8U),
        bs, h, k, experts, &pipe);
    bool released = false, inLane = false;
    uint32_t flagReads = 0U, peak = 0U, currentLane = 0U, loadLane = 0U, peakLoads = 0U;
    std::vector<uint32_t> loaded, processed;
    struct Read { const uint8_t *pointer; std::vector<uint8_t> bytes; };
    std::array<std::vector<Read>, 8> reads;
    std::array<bool, 8> queued {};
    std::array<bool, 8> loadPending {};
    std::array<uint32_t, 8> laneTask {};
    AscendC::EventObserver() = [&](AscendC::HardEvent event, uint32_t lane, bool set) {
        if (event == AscendC::HardEvent::MTE2_S && loadPending[lane]) {
            if (set) {
                const uint32_t pending = uint32_t(loaded.size() - processed.size());
                Check(pending <= depth, "at most two unconsumed loads, or one for a single lane");
                peakLoads = std::max(peakLoads, pending);
            } else {
                if (processed.empty()) Check(loaded.size() == depth, "prefetch fills before the first consumer wait");
                Check(lane == processed.size() % lanes && laneTask[lane] == loaded[processed.size()],
                    "loads are consumed FIFO using the same lane and stable task identity");
                processed.push_back(laneTask[lane]);
                loadPending[lane] = false; currentLane = lane; inLane = true;
            }
        }
        if (event != AscendC::HardEvent::MTE3_MTE2) return;
        if (!set) {
            for (const auto &read : reads[lane])
                Check(std::memcmp(read.pointer, read.bytes.data(), read.bytes.size()) == 0,
                    "lane packet/meta/group remain unchanged until the completion wait");
            reads[lane].clear(); queued[lane] = false; loadLane = lane;
        } else if (!reads[lane].empty()) {
            queued[lane] = true;
            peak = std::max(peak, uint32_t(std::count(queued.begin(), queued.end(), true)));
            inLane = false;
        }
    };
    const auto preserve = [&](const void *, const void *pointer, uint32_t bytes) {
        if (!inLane) {
            Check(processed.size() == owned && std::none_of(queued.begin(), queued.end(), [](bool v) { return v; }),
                "Flag cleanup starts only after every data/metadata lane has been drained");
            return;
        }
        const auto *p = static_cast<const uint8_t *>(pointer);
        reads[currentLane].push_back({p, std::vector<uint8_t>(p, p + bytes)});
    };
    AscendC::GmWriteObserver() = preserve;
    AscendC::PaddedWriteObserver() = preserve;
    AscendC::GmReadObserver() = [&](const void *pointer, uint32_t size) {
        const auto offset = reinterpret_cast<uintptr_t>(pointer) - reinterpret_cast<uintptr_t>(local.data() + start);
        if (offset >= bs * data.tokenStride) return;
        const uint32_t q = uint32_t(offset / data.tokenStride);
        if (offset % data.tokenStride == 480U) {
            Check(++flagReads < 256U, "bounded pending progress");
            if (q >= 16U && !released) {
                Check(processed.empty(), "none of an unready first batch is consumed");
                for (uint32_t pending = 0; pending < 16U; ++pending)
                    std::memcpy(local.data() + start + pending * data.tokenStride,
                        fixtures[pending].wire.data(), data.tokenStride);
                released = true;
            }
        } else if (size == data.tokenStride) {
            Check(released && q < owned, "ready packet belongs to this receive owner");
            Check(std::find(loaded.begin(), loaded.end(), q) == loaded.end(), "swap removal never loads a stable row twice");
            Check(!loadPending[loadLane], "prefetch never reuses an unconsumed lane");
            loaded.push_back(q); laneTask[loadLane] = q; loadPending[loadLane] = true;
        }
    };
    ServerDedupTest::ProfileCapture capture(args);
    ServerDedupProfile profile;
    profile.Init(reinterpret_cast<GM_ADDR>(&args), 0U);
    receiver.Process(profile);
    AscendC::GmReadObserver() = {}; AscendC::GmWriteObserver() = {};
    AscendC::PaddedWriteObserver() = {}; AscendC::EventObserver() = {};
    CheckReceiveProfile(profile, capture, owned, 8U);
    Check(processed.size() == owned && processed.front() == 23U && processed[7U] == 16U,
        "scan advances past an unready full batch and consumes a ready tail in reverse order");
    Check(loaded == processed && peakLoads == depth, "every prefetched packet is consumed with the requested depth");
    for (const auto &events : {AscendC::Events<AscendC::HardEvent::MTE2_S>(),
            AscendC::Events<AscendC::HardEvent::MTE2_MTE3>(), AscendC::Events<AscendC::HardEvent::MTE3_MTE2>()})
        Check(std::none_of(events.begin(), events.end(), [](bool v) { return v; }), "all per-lane events are consumed after drain");
    Check(peak == lanes, "ready batches larger than the lane pool retain concurrent ownership");
    Check(pipe.bytes == fixed + lanes * (data.tokenStride + 32U + groupStride), "UB contains one packet/meta/group per lane and an independent 16-slot probe pool");
    std::set<uint32_t> groupIds;
    for (uint32_t q = 0; q < owned; ++q) {
        const auto &packet = fixtures[q];
        Check(std::memcmp(hidden.data() + 32U + q * hiddenBytes, packet.payload.data(), hiddenBytes) == 0,
            "each stable output row retains its source Token despite pending permutation");
        if (Quantized) Check(std::memcmp(scales.data() + 32U + q * data.scaleBytes,
            packet.payload.data() + data.quantCount, data.scaleBytes) == 0, "scale rows follow hidden rows");
        const auto *meta = assist.data() + 8U + q * 8U;
        Check(meta[0] == 0U && meta[1] == q && meta[3] == packet.Word(3U), "metadata lanes follow stable rows");
        if (packet.entries) {
            Check(meta[4] == packet.entries && meta[6] == rank && groupIds.insert(meta[7]).second,
                "a first hop allocates a unique t and keeps its original role");
            const auto *group = groups.data() + 8U + meta[7] * groupStride / 4U;
            Check(group[0] == q, "group refers to the first-hop output row");
            for (uint32_t i = 0; i < packet.entries; ++i) {
                Check(group[1U + i * 2U] == meta[7] * k + packet.Word(10U + 4U * i) &&
                    group[2U + i * 2U] == packet.Word(11U + 4U * i), "group lane preserves members and exact weights");
                const uint64_t target = start + (uint64_t(i + 1U) * bs + q) * data.tokenStride;
                auto expected = packet;
                expected.SetWord(4U, UINT32_MAX); expected.SetWord(6U, rank); expected.SetWord(7U, meta[7]); expected.Pack();
                Check(std::memcmp(local.data() + target, expected.wire.data(), expected.wire.size()) == 0,
                    "same-card terminal slots remain ready for their other receive owner");
            }
        }
        for (uint32_t block = 0; block < data.blockCount; ++block)
            Check(std::all_of(local.begin() + start + q * data.tokenStride + block * 512U + 480U,
                local.begin() + start + q * data.tokenStride + (block + 1U) * 512U, [](uint8_t v) { return v == 0; }),
                "kernel-end cleanup clears every owned Flag block");
    }
    for (uint32_t q = owned; q < bs; ++q)
        Check(std::memcmp(local.data() + start + q * data.tokenStride, fixtures[q].wire.data(), data.tokenStride) == 0,
            "cleanup does not touch neighboring cores' slots");
    for (const auto *bytes : {&hidden, &scales})
        Check(std::all_of(bytes->begin(), bytes->begin() + 32U, [](uint8_t v) { return v == 0xcd; }) &&
            std::all_of(bytes->end() - 32U, bytes->end(), [](uint8_t v) { return v == 0xcd; }), "batch output guards");
    // The same object and TPipe must also handle an empty next receive set.
    std::fill(prefix, prefix + experts, 0U);
    const auto prior = hidden;
    ServerDedupTest::ProfileCapture emptyCapture(args);
    ServerDedupProfile emptyProfile;
    emptyProfile.Init(reinterpret_cast<GM_ADDR>(&args), 0U);
    receiver.Process(emptyProfile);
    CheckReceiveProfile(emptyProfile, emptyCapture, 0U, 0U);
    Check(hidden == prior && pipe.bytes == experts * 4U, "empty receive set has no data/probe/lane allocation or output writes");
}

} // namespace

int main()
{
    try {
        CheckPacket(1U, 1U, false);
        CheckBatchReceiver<false>(7168U, 8U);
        CheckBatchReceiver<true>(7168U, 8U);
        CheckBatchReceiver<false>(20000U, 3U);
        CheckBatchReceiver<false>(26000U, 2U);
        CheckBatchReceiver<false>(42000U, 1U);
        for (bool quantized : {false, true}) {
            for (uint32_t h : {209U, 225U, 7168U}) CheckPacket(h, 6U, quantized);
            CheckPacket(209U, 32U, quantized);
            CheckPacket(225U, 32U, quantized);
        }
        for (int64_t magic : {1, 2, 3}) for (int32_t role : {0, 1, -1}) {
            CheckReceiver<false>(role, magic);
            CheckReceiver<true>(role, magic);
        }
    } catch (const std::exception &error) {
        AscendC::GmWriteObserver() = {}; AscendC::GmReadObserver() = {};
        AscendC::PaddedWriteObserver() = {}; AscendC::EventObserver() = {};
        std::cerr << error.what() << '\n';
        return 1;
    }
#if defined(TILEXR_ENABLE_PROFILING)
    return ServerDedupTest::Finish("receive_profile");
#else
    return ServerDedupTest::Finish("receive_packet");
#endif
}
