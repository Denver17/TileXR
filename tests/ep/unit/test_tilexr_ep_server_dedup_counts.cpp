#include <iostream>
#include <numeric>
#include <random>

#include "tilexr_ep_dispatch_memory_server_dedup_kernel.h"
#include "server_dedup_test_checks.h"

namespace AscendC {
uint32_t testCore = 0;
uint64_t testCopies = 0;
}

namespace {
using TileXREp::DispatchMemoryServerDedupKernel;
using ServerDedupTest::Check;

struct Rank {
    TileXR::CommArgs args {};
    std::vector<uint32_t> state;
    std::vector<int32_t> routes, output;
    std::vector<int64_t> expertOutput;
    std::vector<AscendC::TPipe> pipes;
    std::vector<DispatchMemoryServerDedupKernel> cores;
};

void Run(uint32_t world, uint32_t experts, uint32_t bs, uint32_t k, uint32_t rounds)
{
    const uint32_t localExperts = experts / world;
    const auto layout = TileXREp::TileXREpServerDedupCountLayout(experts);
    std::vector<Rank> ranks(world);
    for (uint32_t r = 0; r < world; ++r) {
        auto &rank = ranks[r];
        rank.state.assign(layout.stateBytes / 4 + 32, 0);
        std::fill(rank.state.begin() + layout.counterOffset / 4, rank.state.end(), 0xa5a5a5a5);
        rank.args.rank = r;
        rank.args.rankSize = world;
        rank.args.peerMemBytes = layout.stateBytes;
    }
    for (auto &rank : ranks) for (uint32_t r = 0; r < world; ++r)
        rank.args.peerMems[r] = reinterpret_cast<uint8_t *>(ranks[r].state.data());

    std::mt19937 rng(17 + world + bs + k);
    std::vector<uint32_t> order(world * 64);
    std::iota(order.begin(), order.end(), 0);
    for (uint32_t round = 1; round <= rounds; ++round) {
        // Alternate count/prefix outputs, rotate routes and wrap both state banks.
        const uint32_t outputType = round % 2;
        const auto offset = TileXREp::TileXREpServerDedupCountRoundOffset(layout, round);
        const auto other = TileXREp::TileXREpServerDedupCountRoundOffset(layout, round + 1);
        std::vector<std::vector<uint32_t>> inactive(world);
        std::vector<std::vector<int32_t>> expected(world, std::vector<int32_t>(experts));
        for (uint32_t s = 0; s < world; ++s) {
            auto &rank = ranks[s];
            inactive[s].assign(rank.state.begin() + other / 4, rank.state.begin() + (other + layout.roundBytes) / 4);
            rank.routes.clear();
            rank.output.assign(experts + 2, -777);
            rank.expertOutput.assign(localExperts + 2, -888);
            rank.pipes.clear(); rank.pipes.resize(64);
            rank.cores.clear(); rank.cores.resize(64);
            std::vector<int32_t> permutation(experts);
            std::iota(permutation.begin(), permutation.end(), 0);
            for (uint32_t token = 0; token < bs; ++token) {
                // Every third round concentrates traffic: many ranks have tokenNum=0.
                if (round % 3 != 0) std::shuffle(permutation.begin(), permutation.end(), rng);
                for (uint32_t route = 0; route < k; ++route) {
                    const uint32_t expert = round % 3 == 0 ? route : uint32_t(permutation[route]);
                    rank.routes.push_back(expert);
                    ++expected[expert / localExperts][(expert % localExperts) * world + s];
                }
            }
            for (uint32_t core = 0; core < 64; ++core) {
                AscendC::testCore = core;
                rank.cores[core].Init(reinterpret_cast<uint8_t *>(&rank.args),
                    reinterpret_cast<uint8_t *>(rank.routes.data()),
                    reinterpret_cast<uint8_t *>(rank.expertOutput.data() + 1),
                    reinterpret_cast<uint8_t *>(rank.output.data() + 1),
                    bs, k, experts, outputType, round, &rank.pipes[core]);
            }
        }
        // Drive the actual production phases with all sends posted before polling.
        // Random core order covers partition ownership without simulating device timing.
        std::shuffle(order.begin(), order.end(), rng);
        for (uint32_t id : order) {
            const auto before = AscendC::testCopies;
            ranks[id / 64].cores[id % 64].SendCnt();
            if (bs >= 48 && id % 64 < 48)
                Check(AscendC::testCopies == before, "large-BS source cores perform no count IO");
        }
        for (uint32_t r = 0; r < world; ++r) for (uint32_t i = 0; i < experts; ++i) {
            Check(ranks[r].state[offset / 4 + i * 8] == 0x3f800000, "every cnt record, including zero, is notified");
            Check(int32_t(ranks[r].state[offset / 4 + i * 8 + 1]) == expected[r][i], "raw per-expert/source count matches independent route histogram");
        }
        for (uint32_t id : order) if (id % 64 >= 48) ranks[id / 64].cores[id % 64].RecvCnt();
        std::shuffle(order.begin(), order.end(), rng);
        for (uint32_t id : order) if (id % 64 >= 48) ranks[id / 64].cores[id % 64].CalcCumsum();
        std::shuffle(order.begin(), order.end(), rng);
        for (uint32_t id : order) if (id % 64 >= 48) ranks[id / 64].cores[id % 64].CompleteCumsum();
        for (auto &rank : ranks) for (auto &core : rank.cores) core.Finish();

        for (uint32_t r = 0; r < world; ++r) {
            const auto &rank = ranks[r];
            int64_t cumulative = 0, previousExpert = 0;
            for (uint32_t i = 0; i < experts; ++i) {
                cumulative += expected[r][i];
                Check(rank.output[i + 1] == cumulative, "inclusive sendCountsOut follows localExpert/source order");
                Check(rank.state[offset / 4 + i * 8] == 0, "cnt ready flag cleared by its receiver");
                for (uint32_t receiver = 0; receiver < 16; ++receiver)
                    Check(int32_t(rank.state[(offset + layout.prefixOffset) / 4 + receiver * experts + i]) == cumulative,
                        "all sixteen consumers receive the complete prefix");
                if ((i + 1) % world == 0) {
                    Check(rank.expertOutput[(i + 1) / world] == (outputType ? cumulative - previousExpert : cumulative),
                        "int64 per-expert count/prefix output");
                    previousExpert = cumulative;
                }
            }
            for (uint64_t byte = layout.cumsumOffset; byte < layout.prefixOffset; byte += 4)
                Check(rank.state[(offset + byte) / 4] == 0, "segment and full-prefix readiness cleaned");
            Check(rank.output.front() == -777 && rank.output.back() == -777 &&
                rank.expertOutput.front() == -888 && rank.expertOutput.back() == -888,
                "exact output bytes preserve adjacent canaries");
            Check(std::equal(inactive[r].begin(), inactive[r].end(), rank.state.begin() + other / 4),
                "other round state stays untouched");
            Check(std::all_of(rank.state.begin() + layout.counterOffset / 4, rank.state.end(),
                [](uint32_t v) { return v == 0xa5a5a5a5; }), "count-only increment does not touch counter or payload space");
        }
    }
}
} // namespace

int main()
{
    try {
        Run(16, 16, 1, 1, 4);       // L=1, idle count/source cores, zero receive ranks.
        Run(24, 24, 5, 6, 4);       // Non-divisible receiver segments and output tails.
        Run(16, 64, 7, 6, 4);       // Per-expert reduction below 64 routes.
        Run(16, 64, 8, 8, 4);       // Vector-range selection at 64 routes.
        Run(32, 96, 47, 6, 4);      // All 64 cores send cnt.
        Run(64, 384, 48, 6, 4);     // Only the fixed 16 receive cores send cnt.
        Run(24, 1008, 49, 32, 4);   // Non-power-of-two topology and 63 records/core.
        Run(16, 1024, 256, 32, 4);  // New per-card L=64, at most 64 cnt records/core.
        Run(128, 1024, 48, 8, 4);   // 128P / eight experts per card.
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return ServerDedupTest::Finish("counts_cpu");
}
