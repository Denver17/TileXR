#include <iostream>
#include <numeric>
#include <random>

#include "tilexr_ep_server_dedup_counts.h"
#include "tilexr_ep_server_dedup_round.h"
#include "tilexr_ep_server_dedup_receive_tasks.h"
#include "server_dedup_test_checks.h"

namespace AscendC {
uint32_t testCore = 0;
uint64_t testCopies = 0;
}

namespace {
using TileXREp::ServerDedupCounts;
using ServerDedupTest::Check;

struct Rank {
    TileXR::CommArgs args {};
    std::vector<uint32_t> state;
    std::vector<int32_t> routes, output;
    std::vector<int64_t> expertOutput;
    std::vector<AscendC::TPipe> pipes;
    std::vector<ServerDedupCounts> cores;
};

void CheckReceiveTasks(Rank &rank, const std::vector<int32_t> &counts,
    uint32_t world, uint32_t experts, uint32_t bs, uint32_t k)
{
    // Independently enumerate the sender's actual destination slots in output
    // row order. The listener must recover those addresses from its UB table.
    const uint32_t localExperts = experts / world;
    const auto data = TileXREp::TileXREpServerDedupDataLayout(bs, 7168, k, experts, false);
    std::vector<uint64_t> addresses;
    for (uint32_t expert = 0; expert < localExperts; ++expert)
        for (uint32_t source = 0; source < world; ++source)
            for (uint32_t q = 0; q < uint32_t(counts[expert * world + source]); ++q)
                addresses.push_back(TileXREp::TileXREpServerDedupDispatchOffset(
                    data, source, rank.args.rank, expert, q, world, localExperts, bs));

    uint32_t nextRow = 0;
    for (uint32_t receiver = 0; receiver < 16U; ++receiver) {
        auto &pipe = rank.pipes[48U + receiver];
        const uint64_t bytesBefore = pipe.bytes;
        const uint64_t copiesBefore = AscendC::testCopies;
        const uint64_t syncBefore = AscendC::TestSyncOperations();
        TileXREp::ServerDedupReceiveTasks tasks;
        tasks.Build(rank.cores[48U + receiver].Prefix(), rank.args.rank,
            world, experts, bs, receiver, &pipe);
        const uint32_t end = uint32_t(uint64_t(receiver + 1U) * addresses.size() / 16U);
        Check(tasks.Begin() == nextRow && tasks.Count() == end - nextRow,
            "sixteen receive ranges cover every output row once using floor boundaries");
        Check(pipe.bytes - bytesBefore == 2U * TileXREp::TileXREpDedupAlign(uint64_t(tasks.Count()) * 4U, 32U),
            "receive tables allocate only two aligned UB arrays, including empty cores");
        std::vector<uint32_t> pending(tasks.Count());
        std::iota(pending.begin(), pending.end(), 0U);
        std::vector<uint32_t> positions;
        for (uint32_t j = 0; j < tasks.Count(); ++j) {
            positions.push_back(tasks.Position(j));
            Check(data.dispatchOffset + uint64_t(tasks.Position(j)) * data.tokenStride == addresses[nextRow + j],
                "listener physical slot matches the original source/expert/q send address");
            Check(tasks.PendingIndex(j) == j, "pending list initially contains every stable index");
        }
        // Simulate arbitrary ready order without changing stable slot or row
        // ownership. Removing a task is an issue decision, with no MTE wait.
        while (!pending.empty()) {
            const uint32_t p = uint32_t(pending.size() / 2U);
            Check(tasks.PendingIndex(p) == pending[p], "pending swap retains the correct stable task");
            tasks.RemovePending(p);
            pending[p] = pending.back();
            pending.pop_back();
            Check(tasks.PendingCount() == pending.size(), "pending count tracks issued tasks");
            if (p < pending.size())
                Check(tasks.PendingIndex(p) == pending[p], "replacement at p is available for immediate recheck");
        }
        for (uint32_t j = 0; j < tasks.Count(); ++j)
            Check(tasks.Position(j) == positions[j], "removed tasks retain stable positions for final flag cleanup");
        Check(AscendC::testCopies == copiesBefore && AscendC::TestSyncOperations() == syncBefore,
            "table construction and removal perform no GM IO or synchronization");
        nextRow = end;
    }
    Check(nextRow == addresses.size(), "all expected receive slots have an owner");
}

void CheckReceiveTaskBoundaries()
{
    constexpr uint32_t bs = 96U;
    std::mt19937 rng(2026U);
    for (uint32_t experts : {96U, 384U, 1008U, 1024U}) {
        const uint32_t world = experts == 1008U ? 24U : experts == 1024U ? 128U : 16U;
        for (uint32_t pattern = 0; pattern < 6U; ++pattern) {
            std::vector<int32_t> counts(experts, 0);
            if (pattern == 1U) counts.back() = 7; // Empty cores and a long leading plateau.
            if (pattern == 2U) counts.front() = counts.back() = 16; // Exact boundary across zeros.
            if (pattern == 3U) { counts[experts / 2U] = 17; counts.back() = 20; } // Split q ranges.
            if (pattern == 4U) std::fill(counts.begin(), counts.end(), 1); // Cross-record partitions.
            if (pattern == 5U)
                for (auto &count : counts) count = rng() % 3U == 0U ? int32_t(rng() % (bs + 1U)) : 0;

            AscendC::TPipe prefixPipe;
            AscendC::TBuf<> prefixBuf;
            prefixPipe.InitBuffer(prefixBuf, experts * sizeof(int32_t));
            auto prefix = prefixBuf.Get<int32_t>();
            int32_t cumulative = 0;
            for (uint32_t i = 0; i < experts; ++i) {
                cumulative += counts[i];
                prefix.SetValue(i, cumulative);
            }
            for (uint32_t rank : {0U, world - 1U}) {
                const uint32_t localExperts = experts / world;
                const auto data = TileXREp::TileXREpServerDedupDataLayout(bs, 7168U, 6U, experts, false);
                std::vector<uint64_t> addresses;
                // Enumerate every sender slot independently of the receiver's search/partition.
                for (uint32_t expert = 0; expert < localExperts; ++expert)
                    for (uint32_t source = 0; source < world; ++source) {
                        const uint32_t record = expert * world + source;
                        for (uint32_t q = 0; q < uint32_t(counts[record]); ++q)
                            addresses.push_back(TileXREp::TileXREpServerDedupDispatchOffset(
                                data, source, rank, expert, q, world, localExperts, bs));
                    }
                for (uint32_t receiver = 0; receiver < 16U; ++receiver) {
                    AscendC::TPipe pipe;
                    TileXREp::ServerDedupReceiveTasks tasks;
                    tasks.Build(prefix, rank, world, experts, bs, receiver, &pipe);
                    const uint32_t begin = uint32_t(receiver * addresses.size() / 16U);
                    const uint32_t end = uint32_t((receiver + 1U) * addresses.size() / 16U);
                    Check(tasks.Begin() == begin && tasks.Count() == end - begin &&
                        tasks.PendingCount() == end - begin, "sparse prefix retains exact floor ownership");
                    for (uint32_t j = 0; j < end - begin; ++j) {
                        Check(data.dispatchOffset + uint64_t(tasks.Position(j)) * data.tokenStride == addresses[begin + j],
                            "prefix search preserves original q across zero counts and split records");
                        Check(tasks.PendingIndex(j) == j, "prefix search retains stable pending indices");
                    }
                }
            }
        }
    }
}

void CheckCompletionEpochs()
{
    constexpr uint32_t world = 16;
    constexpr uint64_t bytes = TileXREp::kEpServerDedupRoundBytes + 32U;
    const uint64_t offset = TileXREp::TileXREpServerDedupRoundOffset(bytes);
    Check(TileXREp::TileXREpServerDedupRoundOffset(UINT64_C(4) << 30) ==
        (UINT64_C(4) << 30) - 4096U, "round records stay in the final flag window");
    std::vector<std::vector<int64_t>> windows(world, std::vector<int64_t>((bytes + 32U) / 8U, 0));
    TileXR::CommArgs args {};
    args.rank = 0; args.rankSize = world; args.peerMemBytes = bytes;
    for (uint32_t peer = 0; peer < world; ++peer) {
        args.peerMems[peer] = reinterpret_cast<uint8_t *>(windows[peer].data());
        std::fill_n(windows[peer].begin(), 4U, -9);
        std::fill(windows[peer].end() - 4U, windows[peer].end(), -9);
    }
    for (int64_t epoch = 1; epoch <= 4; ++epoch) {
        std::array<uint32_t, world> reads {};
        uint32_t publications = 0;
        AscendC::GmWriteObserver() = [&](const void *destination, const void *source, uint32_t count) {
            Check(count == 32U, "one complete epoch record per peer");
            Check(static_cast<const int64_t *>(source)[0] == epoch, "publish the current runtime magic");
            bool owned = false;
            for (uint32_t peer = 0; peer < world; ++peer)
                owned = owned || destination == args.peerMems[peer] + offset;
            Check(owned, "rank zero only writes its own slot on each peer");
            ++publications;
        };
        AscendC::GmReadObserver() = [&](const void *source, uint32_t count) {
            Check(publications == world, "publish to every peer before polling completion");
            Check(count == 32U, "poll complete epoch records");
            const uint64_t byte = static_cast<const uint8_t *>(source) - args.peerMems[0] - offset;
            const uint32_t peer = uint32_t(byte / 32U);
            if (peer >= world || ++reads[peer] > 4U) throw std::runtime_error("completion polling failed to progress");
            auto *record = windows[0].data() + offset / 8U + peer * 4U;
            if (peer != 0) {
                // A delayed peer first exposes a partial new record. Old words
                // must keep us waiting; a complete later epoch is also valid.
                if (reads[peer] == 1U) record[0] = epoch;
                if (reads[peer] == 3U) std::fill_n(record, 4U, epoch + (peer == 2 ? 1 : 0));
            }
        };
        AscendC::TPipe pipe;
        TileXREp::CompleteServerDedupRound(&args, epoch, &pipe);
        AscendC::GmReadObserver() = {};
        AscendC::GmWriteObserver() = {};
        for (uint32_t peer = 1; peer < world; ++peer)
            Check(reads[peer] == (peer == 2 && epoch % 2 == 0 ? 1U : 3U), "stale/partial epochs cannot release a new call");
        for (const auto &window : windows) {
            Check(std::all_of(window.begin(), window.begin() + 4U, [](int64_t v) { return v == -9; }) &&
                std::all_of(window.end() - 4U, window.end(), [](int64_t v) { return v == -9; }),
                "epoch writes preserve adjacent memory");
        }
    }
}

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
        // Alternate outputs and routes only after all simulated ranks finish each call.
        const uint32_t outputType = round % 2;
        const auto offset = layout.cntOffset;
        std::vector<std::vector<int32_t>> expected(world, std::vector<int32_t>(experts));
        for (uint32_t s = 0; s < world; ++s) {
            auto &rank = ranks[s];
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
        for (uint32_t r = 0; r < world; ++r)
            CheckReceiveTasks(ranks[r], expected[r], world, experts, bs, k);
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
            for (uint32_t matrix = 0; matrix < 2; ++matrix) {
                const uint64_t begin = matrix == 0 ? layout.cumsumOffset : layout.cumsumFlagOffset;
                for (uint64_t byte = begin; byte < begin + 16U * 16U * 32U; byte += 4)
                    Check(rank.state[(offset + byte) / 4] == 0, "segment and full-prefix readiness cleaned");
            }
            Check(rank.output.front() == -777 && rank.output.back() == -777 &&
                rank.expertOutput.front() == -888 && rank.expertOutput.back() == -888,
                "exact output bytes preserve adjacent canaries");
            Check(std::all_of(rank.state.begin() + layout.counterOffset / 4, rank.state.end(),
                [](uint32_t v) { return v == 0xa5a5a5a5; }), "count-only increment does not touch counter or payload space");
        }
    }
}
} // namespace

int main()
{
    try {
        CheckReceiveTaskBoundaries();
        CheckCompletionEpochs();
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
    return ServerDedupTest::Finish("counts");
}
