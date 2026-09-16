#include <iostream>
#include <numeric>
#include <random>

#include "tilexr_ep_server_dedup_counts.h"
#include "tilexr_ep_server_dedup_receive_tasks.h"
#include "server_dedup_test_checks.h"
#include "server_dedup_test_memory.h"

namespace AscendC {
uint32_t testCore = 0;
uint64_t testCopies = 0;
}

namespace {
using TileXREp::ServerDedupCounts;
using TileXREp::kEpServerDedupSendCoreNum;
using TileXREp::kEpServerDedupRecvCoreNum;
using TileXREp::kEpServerDedupAllCoreCountBsThreshold;
using ServerDedupTest::Check;

struct Rank {
    TileXR::CommArgs args {};
    std::array<std::vector<uint32_t>, 2> states;
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
    for (uint32_t receiver = 0; receiver < kEpServerDedupRecvCoreNum; ++receiver) {
        auto &pipe = rank.pipes[kEpServerDedupSendCoreNum + receiver];
        const uint64_t bytesBefore = pipe.bytes;
        const uint64_t copiesBefore = AscendC::testCopies;
        const uint64_t syncBefore = AscendC::TestSyncOperations();
        TileXREp::ServerDedupReceiveTasks tasks;
        tasks.Build(rank.cores[kEpServerDedupSendCoreNum + receiver].Prefix(), rank.args.rank,
            world, experts, bs, receiver, &pipe);
        const uint32_t end = uint32_t(uint64_t(receiver + 1U) * addresses.size() / kEpServerDedupRecvCoreNum);
        Check(tasks.Begin() == nextRow && tasks.Count() == end - nextRow,
            "receive ranges cover every output row once using floor boundaries");
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
                for (uint32_t receiver = 0; receiver < kEpServerDedupRecvCoreNum; ++receiver) {
                    AscendC::TPipe pipe;
                    TileXREp::ServerDedupReceiveTasks tasks;
                    tasks.Build(prefix, rank, world, experts, bs, receiver, &pipe);
                    const uint32_t begin = uint32_t(receiver * addresses.size() / kEpServerDedupRecvCoreNum);
                    const uint32_t end = uint32_t((receiver + 1U) * addresses.size() / kEpServerDedupRecvCoreNum);
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

void Run(uint32_t world, uint32_t experts, uint32_t bs, uint32_t k, uint32_t rounds)
{
    const uint32_t localExperts = experts / world;
    const auto layout = TileXREp::TileXREpServerDedupCountLayout(experts);
    std::vector<Rank> ranks(world);
    ServerDedupTest::SparsePeerMemory memory;
    for (uint32_t r = 0; r < world; ++r) {
        auto &rank = ranks[r];
        for (uint32_t bank = 0; bank < 2U; ++bank) {
            auto &state = rank.states[bank];
            state.assign(layout.stateBytes / 4 + 32, 0);
            std::fill(state.begin() + layout.counterOffset / 4, state.end(), 0xa5a5a5a5);
            memory.Add(r, bank, state.data(), state.size() * sizeof(uint32_t));
        }
        rank.args.rank = r;
        rank.args.rankSize = world;
        rank.args.peerMemBytes = TileXREp::kEpServerDedupPeerBytes;
    }
    for (auto &rank : ranks) for (uint32_t r = 0; r < world; ++r)
        rank.args.peerMems[r] = reinterpret_cast<uint8_t *>(ServerDedupTest::SparsePeerMemory::Base(r));

    std::mt19937 rng(17 + world + bs + k);
    std::vector<uint32_t> order(world * 64);
    std::iota(order.begin(), order.end(), 0);
    for (uint32_t round = 1; round <= rounds; ++round) {
        // Alternate outputs and routes only after all simulated ranks finish each call.
        const uint32_t bank = round % 2U;
        std::vector<std::vector<uint32_t>> inactive;
        for (const auto &rank : ranks) inactive.push_back(rank.states[1U - bank]);
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
            // Each simulated core enters with the mode established by Init.
            AscendC::SetCtrlSpr<60, 60>(0);
            ranks[id / 64].cores[id % 64].SendCnt();
            Check(AscendC::GetCtrlSpr<60, 60>() == 0,
                "Count preserves the FP overflow mode for subsequent quantization, including after pipe reset");
            if (bs >= kEpServerDedupAllCoreCountBsThreshold && id % 64 < kEpServerDedupSendCoreNum)
                Check(AscendC::testCopies == before, "BS at/above the independent threshold leaves send cores out of Count");
            if (bs < kEpServerDedupAllCoreCountBsThreshold && experts >= 64U)
                Check(AscendC::testCopies > before, "BS below the independent threshold lets all 64 cores publish Count");
        }
        for (uint32_t r = 0; r < world; ++r) for (uint32_t i = 0; i < experts; ++i) {
            Check(ranks[r].states[bank][offset / 4 + i * 8] == 0x3f800000, "every cnt record, including zero, is notified");
            Check(int32_t(ranks[r].states[bank][offset / 4 + i * 8 + 1]) == expected[r][i], "raw per-expert/source count matches independent route histogram");
        }
        for (uint32_t id : order) if (id % 64 >= kEpServerDedupSendCoreNum) ranks[id / 64].cores[id % 64].RecvCnt();
        std::shuffle(order.begin(), order.end(), rng);
        for (uint32_t id : order) if (id % 64 >= kEpServerDedupSendCoreNum) ranks[id / 64].cores[id % 64].CalcCumsum();
        std::shuffle(order.begin(), order.end(), rng);
        for (uint32_t id : order) if (id % 64 >= kEpServerDedupSendCoreNum) ranks[id / 64].cores[id % 64].CompleteCumsum();
        for (uint32_t r = 0; r < world; ++r)
            CheckReceiveTasks(ranks[r], expected[r], world, experts, bs, k);
        for (auto &rank : ranks) for (auto &core : rank.cores) core.Finish();

        for (uint32_t r = 0; r < world; ++r) {
            const auto &rank = ranks[r];
            Check(rank.states[1U - bank] == inactive[r], "count phases preserve the other round bank");
            int64_t cumulative = 0, previousExpert = 0;
            for (uint32_t i = 0; i < experts; ++i) {
                cumulative += expected[r][i];
                Check(rank.output[i + 1] == cumulative, "inclusive sendCountsOut follows localExpert/source order");
                Check(rank.states[bank][offset / 4 + i * 8] == 0, "cnt ready flag cleared by its receiver");
                for (uint32_t receiver = 0; receiver < kEpServerDedupRecvCoreNum; ++receiver)
                    Check(int32_t(rank.states[bank][(offset + layout.prefixOffset) / 4 + receiver * experts + i]) == cumulative,
                        "all receive consumers receive the complete prefix");
                if ((i + 1) % world == 0) {
                    Check(rank.expertOutput[(i + 1) / world] == (outputType ? cumulative - previousExpert : cumulative),
                        "int64 per-expert count/prefix output");
                    previousExpert = cumulative;
                }
            }
            for (uint32_t matrix = 0; matrix < 2; ++matrix) {
                const uint64_t begin = matrix == 0 ? layout.cumsumOffset : layout.cumsumFlagOffset;
                for (uint64_t byte = begin; byte < begin + kEpServerDedupRecvCoreNum * kEpServerDedupRecvCoreNum * 32U; byte += 4)
                    Check(rank.states[bank][(offset + byte) / 4] == 0, "segment and full-prefix readiness cleaned");
            }
            Check(rank.output.front() == -777 && rank.output.back() == -777 &&
                rank.expertOutput.front() == -888 && rank.expertOutput.back() == -888,
                "exact output bytes preserve adjacent canaries");
            Check(std::all_of(rank.states[bank].begin() + layout.counterOffset / 4, rank.states[bank].end(),
                [](uint32_t v) { return v == 0xa5a5a5a5; }), "count-only increment does not touch counter or payload space");
        }
    }
}
} // namespace

int main()
{
    try {
        CheckReceiveTaskBoundaries();
        Run(16, 16, 1, 1, 4);       // L=1, idle count/source cores, zero receive ranks.
        Run(24, 24, 5, 6, 4);       // Non-divisible receiver segments and output tails.
        Run(16, 64, 7, 6, 4);       // Per-expert reduction below 64 routes.
        Run(16, 64, 8, 8, 4);       // Vector-range selection at 64 routes.
        Run(32, 96, 47, 6, 4);      // Exercises BS between send-core count and the default threshold.
        Run(64, 384, 48, 6, 4);     // Default threshold boundary, independent of the split.
        Run(64, 384, 96, 6, 2);     // Reported FP8 failure: threshold 48 bypasses Count, 128 resets the pipe.
        Run(24, 1008, 49, 32, 4);   // Non-power-of-two topology and 63 records/core.
        Run(16, 1024, 256, 32, 4);  // New per-card L=64, at most 64 cnt records/core.
        Run(128, 1024, 48, 8, 4);   // 128P / eight experts per card.
        if (kEpServerDedupAllCoreCountBsThreshold > 0U && kEpServerDedupAllCoreCountBsThreshold <= 256U) {
            if (kEpServerDedupAllCoreCountBsThreshold > 1U)
                Run(16, 64, kEpServerDedupAllCoreCountBsThreshold - 1U, 1, 2);
            Run(16, 64, kEpServerDedupAllCoreCountBsThreshold, 1, 2);
        }
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return ServerDedupTest::Finish("counts");
}
