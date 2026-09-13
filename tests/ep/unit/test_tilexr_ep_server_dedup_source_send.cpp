#include <cstring>
#include <map>
#include <numeric>
#include <random>
#include "tilexr_ep_server_dedup_send.h"
#include "tilexr_ep_server_dedup_recv.h"
#include "server_dedup_test_checks.h"
#include "../integration/server_dedup_source_send_reference.h"

namespace AscendC { uint32_t testCore = 0; uint64_t testCopies = 0; }
namespace {
using ServerDedupTest::Check;

void CheckReceiveUnpack(uint32_t h, bool quantized)
{
    const auto layout = TileXREp::TileXREpServerDedupDataLayout(96, h, 6, 96, quantized);
    const uint32_t bytes = uint32_t(quantized ? layout.quantCount + layout.scaleBytes : h * 2U);
    const uint32_t blocks = (bytes + 479U) / 480U;
    AscendC::TPipe pipe;
    AscendC::TBuf<> packetBuf, hiddenBuf;
    pipe.InitBuffer(packetBuf, blocks * 512U);
    pipe.InitBuffer(hiddenBuf, blocks * 480U + 64U);
    auto packet = packetBuf.Get<uint8_t>();
    auto hidden = hiddenBuf.Get<uint8_t>()[32];
    std::vector<uint8_t> expected(blocks * 480U);
    for (uint32_t i = 0; i < expected.size(); ++i) {
        expected[i] = uint8_t((i * 37U + i / 480U) % 251U);
        packet.SetValue(i / 480U * 512U + i % 480U, expected[i]);
    }
    for (uint32_t block = 0; block < blocks; ++block)
        for (uint32_t i = 0; i < 32U; ++i) packet.SetValue(block * 512U + 480U + i, 0xff);
    const auto originalPacket = packetBuf.storage->bytes;
    TileXREp::UnpackServerDedupData(hidden, packet, uint8_t(blocks));
    Check(std::memcmp(hidden.Data(expected.size()), expected.data(), expected.size()) == 0,
        "receiver restores all payload bytes and quantization scales without flag bytes");
    Check(packetBuf.storage->bytes == originalPacket, "receiver unpack leaves the packet unchanged");
    const auto output = hiddenBuf.Get<uint8_t>();
    for (uint32_t i = 0; i < 32U; ++i)
        Check(output.GetValue(i) == 0xcd && output.GetValue(32U + expected.size() + i) == 0xcd,
            "receiver unpack respects both UB guards");
}

void Run(uint32_t ranks, uint32_t experts, uint32_t bs, uint32_t h, uint32_t k, bool grouped,
    uint32_t source = 0, uint32_t rounds = 1)
{
    TileXR::CommArgs args {};
    args.rankSize = ranks;
    args.rank = source;
    const uint32_t localExperts = experts / ranks;
    auto data = TileXREp::TileXREpServerDedupDataLayout(bs, h, k, experts, false);
    std::vector<int32_t> ids(bs * k);
    std::vector<float> weights(bs * k);
    std::vector<uint16_t> hidden(bs * h);
    for (uint32_t token = 0; token < bs; ++token) {
        for (uint32_t route = 0; route < k; ++route) {
            // Pairs target remote servers with interleaved k; some rows reuse
            // old server state, and every fourth row has only local DIRECTs.
            const uint32_t server = grouped && (token % 4 != 3 || k > 8U * localExperts) ?
                (source / 8U + (route % ((ranks / 8U) - 1U)) + 1U) % (ranks / 8U) : source / 8U;
            ids[token * k + route] = server * 8U * localExperts +
                (server != source / 8U ? route / ((ranks / 8U) - 1U) : route);
            weights[token * k + route] = float(route + 1U) / float(k * (k + 1U) / 2U);
        }
        for (uint32_t column = 0; column < h; ++column) hidden[token * h + column] = uint16_t(token * 73U + column);
    }
    if (ranks == 24U && experts == 72U && k == 6U && bs >= 2U) {
        // Design §2.3: local DIRECTs, one remote singleton, and an interleaved
        // three-route group. The next token changes which servers are grouped.
        const int32_t mixed[] = {4, 25, 28, 50, 26, 5, 26, 50, 27, 51, 28, 4};
        std::copy(mixed, mixed + 12U, ids.begin());
    }
    const auto originalWeights = weights;
    auto packets = ServerDedupReference::SourcePackets(ids, weights, source, ranks, experts, k);
    struct Write { uint32_t core, token, k, entries; const void *destination; uintptr_t lane; };
    std::vector<Write> writes;
    AscendC::GmWriteObserver() = [&](const void *destination, const void *payload, uint32_t bytes) {
        Check(bytes == data.tokenStride, "each source packet uses the complete shared stride");
        uint32_t header[8] {};
        for (uint32_t i = 0; i < sizeof(header); ++i) {
            const uint64_t offset = data.tailOffset + i;
            reinterpret_cast<uint8_t *>(header)[i] = static_cast<const uint8_t *>(payload)[offset / 480U * 512U + offset % 480U];
        }
        writes.push_back({AscendC::testCore, header[1], header[2], header[4], destination, reinterpret_cast<uintptr_t>(payload)});
    };
    std::map<uint32_t, std::vector<uint8_t>> windows;
    for (const auto &packet : packets) {
        const uint32_t destination = packet.members.front().expert / localExperts;
        if (!windows.count(destination)) windows[destination].assign(data.dispatchOffset + data.dispatchBytes + 32U, 0xa5);
    }
#if defined(TILEXR_EP_SERVER_DEDUP_ENABLE_PROFILING) && TILEXR_EP_SERVER_DEDUP_ENABLE_PROFILING
    const uint64_t flagBytes = 4ULL * 1024U * 1024U;
    const auto size = TileXREp::TileXREpServerDedupSendProfileLayout(0, bs, k);
    args.peerMemBytes = TileXREp::TileXREpDedupAlign(data.requiredBytes, 512U) + size.bytes + flagBytes;
    const auto detail = TileXREp::TileXREpServerDedupSendProfileLayout(args.peerMemBytes, bs, k);
    windows[source].resize(args.peerMemBytes, 0xa5);
#endif
    for (auto &window : windows) args.peerMems[window.first] = window.second.data();
    std::vector<uint32_t> masks(bs + 2U, 0xdeadbeefU);
    std::vector<float> output(bs * k + 2U, -777.0f);
    const auto observeWrite = AscendC::GmWriteObserver();
    for (uint32_t round = 0; round < rounds; ++round) {
        writes.clear();
        AscendC::GmWriteObserver() = observeWrite;
        if (round != 0) for (auto &value : hidden) value = uint16_t(value + 0x100U);
        for (uint32_t core = 0; core < 48U; ++core) {
            const uint32_t start = core * (bs / 48U) + std::min(core, bs % 48U);
            const uint32_t count = bs / 48U + (core < bs % 48U);
            const auto priorMasks = masks;
            const auto priorOutput = output;
            AscendC::testCore = core;
            AscendC::TPipe pipe;
            TileXREp::ServerDedupSender<uint16_t, false, false> sender;
            sender.Init(&args, reinterpret_cast<uint8_t *>(hidden.data()), reinterpret_cast<uint8_t *>(ids.data()),
                reinterpret_cast<uint8_t *>(weights.data()), reinterpret_cast<uint8_t *>(masks.data() + 1),
                reinterpret_cast<uint8_t *>(output.data() + 1), bs, h, k, experts, &pipe);
            sender.Process();
#if defined(TILEXR_EP_SERVER_DEDUP_ENABLE_PROFILING) && TILEXR_EP_SERVER_DEDUP_ENABLE_PROFILING
            const uint64_t syncBefore = AscendC::TestSyncOperations();
            sender.FlushProfile(7U + round);
            Check(AscendC::TestSyncOperations() == syncBefore, "profile flush adds no synchronization operations");
            TileXREp::EpServerDedupSendProfileHeader header {};
            const auto *base = windows[source].data() + detail.offset + core * detail.coreStride;
            std::memcpy(&header, base, sizeof(header));
            Check(header.marker == TileXREp::kEpServerDedupProfileMarker && header.version == 4 &&
                header.recordBytes == sizeof(TileXREp::EpServerDedupSendProfilePacket) && header.rank == source &&
                header.core == core && header.generation == 7U + round, "issue header identifies core/rank/generation, including empty cores");
            uint32_t observed = 0, firstHops = 0;
            uint64_t previous = 0;
            for (const auto &write : writes) if (write.core == core) {
                TileXREp::EpServerDedupSendProfilePacket p {};
                std::memcpy(&p, base + sizeof(header) + observed * sizeof(p), sizeof(p));
                Check(p.issue > previous && p.token == write.token && p.k == write.k,
                    "every observed MTE3 packet has the corresponding ordered issue record");
                previous = p.issue; ++observed;
                if (write.entries != 0U) ++firstHops;
            }
            Check(header.packetCount == observed && header.firstHopCount == firstHops,
                "profile counts match actual first-hop and DIRECT sends");
            Check(std::all_of(base + sizeof(header) + observed * sizeof(TileXREp::EpServerDedupSendProfilePacket),
                base + detail.coreStride, [](uint8_t v) { return v == 0xa5; }), "detail write stays within issued packets");
#else
            sender.FlushProfile(7); // Disabled diagnostics must be a no-op even without a local peer allocation.
#endif
            for (uint32_t token = 0; token < bs; ++token) if (token < start || token >= start + count) {
                Check(masks[token + 1U] == priorMasks[token + 1U], "mask write stays within this core's rows");
                for (uint32_t route = 0; route < k; ++route)
                    Check(output[1U + token * k + route] == priorOutput[1U + token * k + route], "weight write stays within this core's rows");
            }
        }
        AscendC::GmWriteObserver() = {};
        Check(writes.size() == packets.size(), "exactly one send per expected physical packet");
        for (uint32_t core = 0; core < 48U; ++core) {
            bool directStarted = false;
            uint32_t previousDirect = 0;
            std::vector<uintptr_t> lanes;
            uint32_t issued = 0;
            for (const auto &write : writes) if (write.core == core) {
                const uint32_t start = core * (bs / 48U) + std::min(core, bs % 48U);
                const uint32_t count = bs / 48U + (core < bs % 48U);
                Check(write.token >= start && write.token < start + count, "same core owns both stages of a token");
                if (write.entries == 0) {
                    const uint32_t index = write.token * k + write.k;
                    Check(!directStarted || index > previousDirect, "DIRECT consumes cached tasks in token/k order");
                    directStarted = true; previousDirect = index;
                } else Check(!directStarted, "all first hops on a core precede its DIRECT sends");
                if (std::find(lanes.begin(), lanes.end(), write.lane) == lanes.end()) lanes.push_back(write.lane);
                Check(lanes.size() <= 8U && write.lane == lanes[issued % lanes.size()], "both stages share a continuous lane sequence");
                ++issued;
            }
        }
        std::vector<uint32_t> expectedMasks(bs, 0U);
        for (const auto &packet : packets) {
            const auto &first = packet.members.front();
            const uint32_t destination = first.expert / localExperts;
            const uint64_t offset = data.dispatchOffset +
                (((uint64_t(source) + destination) % ranks * localExperts + first.expert % localExperts) * bs + first.q) * data.tokenStride;
            const uint8_t *wire = windows[destination].data() + offset;
            Check(std::count_if(writes.begin(), writes.end(), [&](const Write &write) {
                return write.destination == wire && write.token == packet.token && write.k == first.k;
            }) == 1, "each original q slot is sent exactly once");
            std::vector<uint8_t> payload(data.packedPayloadBytes);
            for (uint32_t block = 0; block < data.blockCount; ++block) {
                std::memcpy(payload.data() + block * 480U, wire + block * 512U, 480U);
                for (uint32_t word = 0; word < 8U; ++word) {
                    uint32_t flag = 0;
                    std::memcpy(&flag, wire + block * 512U + 480U + word * 4U, 4U);
                    Check(flag == 0x3f800000U, "all payload/tail blocks have the Memory flag");
                }
            }
            Check(std::memcmp(payload.data(), hidden.data() + packet.token * h, h * 2U) == 0, "full hidden bytes preserve token identity");
            std::vector<uint32_t> tail((32U + 16U * (k - 1U)) / 4U);
            std::memcpy(tail.data(), payload.data() + data.tailOffset, tail.size() * 4U);
            Check(tail[0] == source && tail[1] == packet.token && tail[2] == first.k &&
                tail[4] == packet.members.size() - 1U && tail[5] == tail[4] && tail[6] == UINT32_MAX && tail[7] == UINT32_MAX,
                "DIRECT/first-hop header identity/count/return fields");
            Check(std::memcmp(&tail[3], &first.weight, 4U) == 0, "header retains real weight");
            for (uint32_t i = 1; i < packet.members.size(); ++i) {
                const auto &member = packet.members[i];
                const uint32_t index = 8U + (i - 1U) * 4U;
                Check(tail[index] == member.expert && tail[index + 1U] == member.q && tail[index + 2U] == member.k &&
                    std::memcmp(&tail[index + 3U], &member.weight, 4U) == 0, "entries follow original k/q/weight, excluding the first hop");
            }
            expectedMasks[packet.token] |= uint32_t(1) << first.k;
            Check(output[1U + packet.token * k + first.k] == (packet.members.size() > 1U ? 1.0f : first.weight),
                "group contribution has weight one and DIRECT retains the real weight");
        }
        Check(std::equal(expectedMasks.begin(), expectedMasks.end(), masks.begin() + 1), "mask contains group heads and all DIRECT routes");
        Check(weights == originalWeights, "input weights remain read-only");
        Check(masks.front() == 0xdeadbeefU && masks.back() == 0xdeadbeefU && output.front() == -777.0f && output.back() == -777.0f,
            "non-aligned per-core writes preserve adjacent outputs");
        for (const auto &window : windows) {
            Check(std::all_of(window.second.begin(), window.second.begin() + data.dispatchOffset,
                [](uint8_t value) { return value == 0xa5; }), "source packets do not touch state or source return regions");
            Check(std::all_of(window.second.end() - 32, window.second.end(), [](uint8_t value) { return value == 0xa5; }), "dispatch end canary");
        }
#if defined(TILEXR_EP_SERVER_DEDUP_ENABLE_PROFILING) && TILEXR_EP_SERVER_DEDUP_ENABLE_PROFILING
        Check(std::all_of(windows[source].end() - flagBytes, windows[source].end(), [](uint8_t v) { return v == 0xa5; }),
            "detailed profiling leaves the entire communicator flag region untouched");
#endif
    }
}
}

int main()
{
    try {
        CheckReceiveUnpack(32, false);
        CheckReceiveUnpack(240, false); // Exactly one 480B data block.
        CheckReceiveUnpack(241, false); // Hidden data crosses the first flag.
        CheckReceiveUnpack(7168, false); // 30 blocks; old blocks*15 repeat truncates.
        CheckReceiveUnpack(7168, true); // Scales extend into an additional block.
        Run(16, 96, 96, 64, 6, true);
        Run(16, 96, 96, 64, 6, true, 15); // Cyclic (source+destination)%R slot mapping wraps.
        Run(24, 72, 256, 224, 6, true); // per-core 5/6 rows; metadata crosses a 480B boundary.
        Run(128, 128, 129, 32, 32, true); // many first hops per core wrap the shared lane pool.
        Run(16, 96, 6, 7168, 6, true, 0, 3);
        Run(16, 96, 1, 33, 1, false);
        Run(16, 96, 47, 64, 6, false);
        Run(16, 96, 256, 224, 6, false, 0, 3); // all DIRECT, ring and call reuse.
        Run(16, 96, 256, 224, 32, false); // k31 is a DIRECT route with its original q.
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n'; return 1;
    }
    std::cout << "sender synchronization operations=" << AscendC::TestSyncOperations() << '\n';
#if defined(TILEXR_EP_SERVER_DEDUP_ENABLE_PROFILING) && TILEXR_EP_SERVER_DEDUP_ENABLE_PROFILING
    return ServerDedupTest::Finish("source_send_profile");
#else
    return ServerDedupTest::Finish("source_send");
#endif
}
