#include <cstring>
#include <map>
#include <numeric>
#include <random>
#include "tilexr_ep_server_dedup_send.h"
#include "tilexr_ep_server_dedup_counts.h"
#include "tilexr_ep_server_dedup_recv.h"
#include "server_dedup_test_checks.h"
#include "server_dedup_test_memory.h"
#include "server_dedup_profile_capture.h"
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
    AscendC::TBuf<> packetBuf;
    pipe.InitBuffer(packetBuf, blocks * 512U);
    auto packet = packetBuf.Get<uint8_t>();
    std::vector<uint8_t> hidden(blocks * 480U + 64U, 0xcd);
    std::vector<uint8_t> expected(blocks * 480U);
    for (uint32_t i = 0; i < expected.size(); ++i) {
        expected[i] = uint8_t((i * 37U + i / 480U) % 251U);
        packet.SetValue(i / 480U * 512U + i % 480U, expected[i]);
    }
    for (uint32_t block = 0; block < blocks; ++block)
        for (uint32_t i = 0; i < 32U; ++i) packet.SetValue(block * 512U + 480U + i, 0xff);
    const auto originalPacket = packetBuf.storage->bytes;
    AscendC::GlobalTensor<uint8_t> outputGm;
    outputGm.SetGlobalBuffer(hidden.data() + 32U);
    TileXREp::WriteServerDedupPayloadRange(outputGm, packet, 0U, uint32_t(expected.size()));
    Check(std::memcmp(hidden.data() + 32U, expected.data(), expected.size()) == 0,
        "receiver restores all payload bytes and quantization scales without flag bytes");
    Check(packetBuf.storage->bytes == originalPacket, "receiver unpack leaves the packet unchanged");
    for (uint32_t i = 0; i < 32U; ++i)
        Check(hidden[i] == 0xcd && hidden[32U + expected.size() + i] == 0xcd,
            "receiver unpack respects both output guards");
}

void CheckDeferredCountSender()
{
    if (TileXREp::kEpServerDedupAllCoreCountBsThreshold <= 1U) return;
    constexpr uint32_t experts = 384U, world = 64U, h = 32U;
    const auto data = TileXREp::TileXREpServerDedupDataLayout(1U, h, 1U, experts, false);
    const auto countLayout = TileXREp::TileXREpServerDedupCountLayout(experts);
    std::vector<uint8_t> window(data.dispatchOffset + data.dispatchBytes + 32U, 0U);
    TileXR::CommArgs args {};
    args.rankSize = world;
    args.peerMemBytes = TileXREp::kEpServerDedupPeerBytes;
    args.peerMems[0] = reinterpret_cast<uint8_t *>(ServerDedupTest::SparsePeerMemory::Base(0));
    ServerDedupTest::SparsePeerMemory memory;
    memory.Add(0U, 1U, window.data(), window.size());
    ServerDedupTest::ProfileCapture capture(args);
    AscendC::TPipe pipe;
    AscendC::testCore = 0U;
    int32_t route = 1, prefix[experts] {};
    int64_t expertOutput[experts / world] {};
    uint16_t hidden[h] {};
    float weight = 0.75f, output = 0.0f;
    uint32_t mask = 0U;
    TileXREp::ServerDedupCounts counts;
    counts.Init(reinterpret_cast<GM_ADDR>(&args), reinterpret_cast<GM_ADDR>(&route),
        reinterpret_cast<GM_ADDR>(expertOutput), reinterpret_cast<GM_ADDR>(prefix),
        1U, 1U, experts, 0U, 1, &pipe);
    TileXREp::ServerDedupProfile profile;
    profile.Init(reinterpret_cast<GM_ADDR>(&args), 0U);
    AscendC::DeferMte3() = true;
    counts.SendCnt(profile);
    Check(!AscendC::PendingMte3().empty(), "sender starts with count records still pending");
    bool preparationOverlapped = false;
    AscendC::GmReadObserver() = [&](const void *source, uint32_t) {
        if (source == &route) preparationOverlapped = !AscendC::PendingMte3().empty();
    };
    TileXREp::ServerDedupSender<uint16_t, false, false> sender;
    sender.Init(&args, reinterpret_cast<GM_ADDR>(hidden), reinterpret_cast<GM_ADDR>(&route),
        reinterpret_cast<GM_ADDR>(&weight), reinterpret_cast<GM_ADDR>(&mask), reinterpret_cast<GM_ADDR>(&output),
        1U, h, 1U, experts, &pipe, 1);
    sender.Process(profile, counts.ScratchPool(), counts.ReservedBytes());
    AscendC::GmReadObserver() = {};
    Check(preparationOverlapped, "sender route preparation advances before count MTE3 completes");
    Check(AscendC::PendingMte3().empty(), "sender completion retires pending count and payload writes");
    AscendC::DeferMte3() = false;
    Check(counts.ReservedBytes() == 256U && pipe.bytes == 190U * 1024U,
        "sender accounts for the retained 256-byte count records inside its UB budget");
    for (uint32_t expert = 0; expert < experts / world; ++expert) {
        uint32_t record[2] {};
        std::memcpy(record, window.data() + countLayout.cntOffset + expert * world * 32U, sizeof(record));
        Check(record[0] == 0x3f800000U && record[1] == (expert == 1U ? 1U : 0U),
            "sender preparation and lane reuse preserve the delayed count source");
    }
    Check(mask == 1U && output == weight, "count-to-sender handoff preserves token outputs");
    counts.Finish();
}

void Run(uint32_t ranks, uint32_t experts, uint32_t bs, uint32_t h, uint32_t k, bool grouped,
    uint32_t source = 0, uint32_t rounds = 1, const std::vector<int32_t> &routePattern = {})
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
    if (!routePattern.empty()) {
        Check(routePattern.size() % k == 0U, "route pattern contains complete token rows");
        for (uint32_t i = 0; i < ids.size(); ++i) ids[i] = routePattern[i % routePattern.size()];
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
    std::map<uint32_t, std::array<std::vector<uint8_t>, 2>> windows;
    ServerDedupTest::SparsePeerMemory memory;
    for (const auto &packet : packets) {
        const uint32_t destination = packet.members.front().expert / localExperts;
        if (!windows.count(destination)) {
            auto &banks = windows[destination];
            for (uint32_t bank = 0; bank < 2U; ++bank) {
                banks[bank].assign(data.dispatchOffset + data.dispatchBytes + 32U, 0xa5);
                memory.Add(destination, bank, banks[bank].data(), banks[bank].size());
            }
        }
    }
    ServerDedupTest::ProfileCapture capture(args);
    for (const auto &window : windows)
        args.peerMems[window.first] = reinterpret_cast<uint8_t *>(ServerDedupTest::SparsePeerMemory::Base(window.first));
    args.peerMemBytes = TileXREp::kEpServerDedupPeerBytes;
    std::vector<uint32_t> masks(bs + 2U, 0xdeadbeefU);
    std::vector<float> output(bs * k + 2U, -777.0f);
    const auto observeWrite = AscendC::GmWriteObserver();
    for (uint32_t round = 0; round < rounds; ++round) {
        const uint32_t bank = (round + 1U) % 2U;
        std::map<uint32_t, std::vector<uint8_t>> inactive;
        for (const auto &window : windows) inactive[window.first] = window.second[1U - bank];
        writes.clear();
        AscendC::GmWriteObserver() = observeWrite;
        if (round != 0) for (auto &value : hidden) value = uint16_t(value + 0x100U);
        for (uint32_t core = 0; core < TileXREp::kEpServerDedupSendCoreNum; ++core) {
            const uint32_t start = core * (bs / TileXREp::kEpServerDedupSendCoreNum) + std::min(core, bs % TileXREp::kEpServerDedupSendCoreNum);
            const uint32_t count = bs / TileXREp::kEpServerDedupSendCoreNum + (core < bs % TileXREp::kEpServerDedupSendCoreNum);
            const auto priorMasks = masks;
            const auto priorOutput = output;
            AscendC::testCore = core;
            AscendC::TPipe pipe;
            TileXREp::ServerDedupSender<uint16_t, false, false> sender;
            sender.Init(&args, reinterpret_cast<uint8_t *>(hidden.data()), reinterpret_cast<uint8_t *>(ids.data()),
                reinterpret_cast<uint8_t *>(weights.data()), reinterpret_cast<uint8_t *>(masks.data() + 1),
                reinterpret_cast<uint8_t *>(output.data() + 1), bs, h, k, experts, &pipe, round + 1U);
            TileXREp::ServerDedupProfile profile;
            profile.Init(reinterpret_cast<GM_ADDR>(&args), 0U);
            profile.Mark(2);
            AscendC::TBufPool<AscendC::TPosition::VECCALC, 16> scratch;
            pipe.InitBufPool(scratch, 190U * 1024U);
            sender.Process(profile, scratch, 0U);
            profile.Mark(3);
            const auto syncBefore = AscendC::TestSyncOperations();
            profile.Publish();
            Check(AscendC::TestSyncOperations() == syncBefore, "standard publication adds no synchronization");
            const auto events = capture.Events(core);
#if defined(TILEXR_ENABLE_PROFILING)
            std::vector<uint32_t> expected {TileXREp::kEpServerDedupMainEventBase + 2U};
            if (count != 0U) {
                for (const auto operation : {TileXREp::EpDedupDetailMetric::SendFirstHop,
                        TileXREp::EpDedupDetailMetric::SendDirect}) {
                    const uint32_t begin = TileXREp::kEpServerDedupOperationEventBase + 2U * uint32_t(operation);
                    expected.push_back(begin);
                    expected.push_back(begin + 1U);
                }
            }
            expected.push_back(TileXREp::kEpServerDedupMainEventBase + 3U);
            Check(events == expected, "send stages retain two loop intervals without per-packet records");
#else
            Check(events.empty(), "OFF builds emit no issue records");
#endif
            for (uint32_t token = 0; token < bs; ++token) if (token < start || token >= start + count) {
                Check(masks[token + 1U] == priorMasks[token + 1U], "mask write stays within this core's rows");
                for (uint32_t route = 0; route < k; ++route)
                    Check(output[1U + token * k + route] == priorOutput[1U + token * k + route], "weight write stays within this core's rows");
            }
        }
        AscendC::GmWriteObserver() = {};
        Check(writes.size() == packets.size(), "exactly one send per expected physical packet");
        for (uint32_t core = 0; core < TileXREp::kEpServerDedupSendCoreNum; ++core) {
            bool directStarted = false;
            uint32_t previousDirect = 0, previousServerStep = 0;
            std::vector<uintptr_t> lanes;
            uint32_t issued = 0;
            for (const auto &write : writes) if (write.core == core) {
                const uint32_t start = core * (bs / TileXREp::kEpServerDedupSendCoreNum) + std::min(core, bs % TileXREp::kEpServerDedupSendCoreNum);
                const uint32_t count = bs / TileXREp::kEpServerDedupSendCoreNum + (core < bs % TileXREp::kEpServerDedupSendCoreNum);
                Check(write.token >= start && write.token < start + count, "same core owns both stages of a token");
                if (write.entries == 0) {
                    const uint32_t index = write.token * k + write.k;
                    const uint32_t server = uint32_t(ids[index]) / (8U * localExperts);
                    const uint32_t serverStep = (server + ranks / 8U - source / 8U) % (ranks / 8U);
                    Check(!directStarted || serverStep >= previousServerStep,
                        "DIRECT visits servers cyclically starting at the source server");
                    Check(!directStarted || serverStep != previousServerStep || index > previousDirect,
                        "DIRECT preserves token/k order within each destination server");
                    directStarted = true; previousDirect = index; previousServerStep = serverStep;
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
            const uint8_t *wire = windows[destination][bank].data() + offset;
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
            Check(window.second[1U - bank] == inactive.at(window.first), "sender preserves the previous round bank");
            Check(std::all_of(window.second[bank].begin(), window.second[bank].begin() + data.dispatchOffset,
                [](uint8_t value) { return value == 0xa5; }), "source packets do not touch state or source return regions");
            Check(std::all_of(window.second[bank].end() - 32, window.second[bank].end(), [](uint8_t value) { return value == 0xa5; }), "dispatch end canary");
        }

    }
}
}

int main()
{
    try {
        CheckDeferredCountSender();
        CheckReceiveUnpack(32, false);
        CheckReceiveUnpack(240, false); // Exactly one 480B data block.
        CheckReceiveUnpack(241, false); // Hidden data crosses the first flag.
        CheckReceiveUnpack(7168, false); // 30 blocks; old blocks*15 repeat truncates.
        CheckReceiveUnpack(7168, true); // Scales extend into an additional block.
        for (uint32_t source : {0U, 24U, 56U}) {
            const uint32_t local = source / 8U;
            const auto expert = [local](uint32_t step, uint32_t offset = 0U) {
                return int32_t((local + step) % 8U * 48U + offset);
            };
            // Two rows per core: late local routes (own card and peer card),
            // interleaved remote servers, and no tasks for relative server 6.
            Run(64, 384, 96, 64, 6, false, source, 3, {
                expert(2), expert(7), expert(1), expert(0, 42), expert(5), expert(0),
                expert(4), expert(1, 1), expert(0, 1), expert(3), expert(7, 1), expert(2, 1)});
            // First hops and singleton DIRECTs share the ring across the wrap.
            Run(64, 384, 96, 64, 6, false, source, 3, {
                expert(2), expert(5), expert(2, 1), expert(1), expert(0, 42), expert(0),
                expert(7), expert(3), expert(0, 1), expert(4), expert(7, 1), expert(1, 1)});
        }
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
#if defined(TILEXR_ENABLE_PROFILING)
    return ServerDedupTest::Finish("source_send_profile");
#else
    return ServerDedupTest::Finish("source_send");
#endif
}
