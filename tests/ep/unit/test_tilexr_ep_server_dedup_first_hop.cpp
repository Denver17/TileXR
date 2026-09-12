#include <cstring>
#include <map>
#include <numeric>
#include <random>
#include "tilexr_ep_server_dedup_send.h"
#include "server_dedup_test_checks.h"
#include "../integration/server_dedup_first_hop_reference.h"

namespace AscendC { uint32_t testCore = 0; uint64_t testCopies = 0; }
namespace {
using ServerDedupTest::Check;

void Run(uint32_t ranks, uint32_t experts, uint32_t bs, uint32_t h, uint32_t k, bool grouped, uint32_t source = 0)
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
    auto packets = ServerDedupReference::FirstHops(ids, weights, source, ranks, experts, k);
    std::map<uint32_t, std::vector<uint8_t>> windows;
    for (const auto &packet : packets) {
        const uint32_t destination = packet.members.front().expert / localExperts;
        if (!windows.count(destination)) windows[destination].assign(data.dispatchOffset + data.dispatchBytes + 32U, 0xa5);
    }
    for (auto &window : windows) args.peerMems[window.first] = window.second.data();
    std::vector<uint32_t> masks(bs + 2U, 0xdeadbeefU);
    std::vector<float> output(bs * k + 2U, -777.0f);
    for (uint32_t core = 0; core < 48U; ++core) {
        AscendC::testCore = core;
        AscendC::TPipe pipe;
        TileXREp::ServerDedupSender<uint16_t, false, false> sender;
        sender.Init(&args, reinterpret_cast<uint8_t *>(hidden.data()), reinterpret_cast<uint8_t *>(ids.data()),
            reinterpret_cast<uint8_t *>(weights.data()), reinterpret_cast<uint8_t *>(masks.data() + 1),
            reinterpret_cast<uint8_t *>(output.data() + 1), bs, h, k, experts, &pipe);
        sender.Process();
    }
    std::vector<uint32_t> expectedMasks(bs, 0U);
    for (const auto &packet : packets) {
        const auto &first = packet.members.front();
        const uint32_t destination = first.expert / localExperts;
        const uint64_t offset = data.dispatchOffset +
            (((uint64_t(source) + destination) % ranks * localExperts + first.expert % localExperts) * bs + first.q) * data.tokenStride;
        const uint8_t *wire = windows[destination].data() + offset;
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
            "first-hop header identity/count/return fields");
        Check(std::memcmp(&tail[3], &first.weight, 4U) == 0, "header retains real weight");
        for (uint32_t i = 1; i < packet.members.size(); ++i) {
            const auto &member = packet.members[i];
            const uint32_t index = 8U + (i - 1U) * 4U;
            Check(tail[index] == member.expert && tail[index + 1U] == member.q && tail[index + 2U] == member.k &&
                std::memcmp(&tail[index + 3U], &member.weight, 4U) == 0, "entries follow original k/q/weight, excluding the first hop");
        }
        expectedMasks[packet.token] |= uint32_t(1) << first.k;
        Check(output[1U + packet.token * k + first.k] == 1.0f, "group source contribution has weight one");
    }
    Check(std::equal(expectedMasks.begin(), expectedMasks.end(), masks.begin() + 1), "only actually sent first-hop bits are published");
    Check(weights == originalWeights, "input weights remain read-only");
    Check(masks.front() == 0xdeadbeefU && masks.back() == 0xdeadbeefU && output.front() == -777.0f && output.back() == -777.0f,
        "non-aligned per-core writes preserve adjacent outputs");
    for (const auto &window : windows) {
        Check(std::all_of(window.second.begin(), window.second.begin() + data.dispatchOffset,
            [](uint8_t value) { return value == 0xa5; }), "source packets do not touch state or source return regions");
        Check(std::all_of(window.second.end() - 32, window.second.end(), [](uint8_t value) { return value == 0xa5; }), "dispatch end canary");
    }
}
}

int main()
{
    try {
        Run(16, 96, 96, 64, 6, true);
        Run(16, 96, 96, 64, 6, true, 15); // Cyclic (source+destination)%R slot mapping wraps.
        Run(24, 72, 256, 224, 6, true); // per-core 5/6 rows; metadata crosses a 480B boundary.
        Run(128, 128, 129, 32, 32, true); // many first hops per core wrap the shared lane pool.
        Run(16, 96, 6, 7168, 6, true);
        Run(16, 96, 1, 33, 1, false);
        Run(16, 96, 47, 64, 6, false);
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n'; return 1;
    }
    return ServerDedupTest::Finish("first_hop");
}
