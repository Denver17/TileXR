#include <iostream>
#include <numeric>
#include <random>

#include "tilexr_ep_server_dedup_counts.h"
#include "tilexr_ep_server_dedup_receive_tasks.h"
#include "server_dedup_test_checks.h"
#include "server_dedup_test_memory.h"
#include "server_dedup_profile_capture.h"

namespace AscendC {
uint32_t testCore = 0;
uint64_t testCopies = 0;
}

namespace {
using TileXREp::ServerDedupCounts;
constexpr uint32_t sendCoreNum = TileXREp::kEpServerDedupSendCoreNum;
constexpr uint32_t recvCoreNum = TileXREp::kEpServerDedupRecvCoreNum;
using TileXREp::kEpServerDedupCountCoreNum;
using TileXREp::kEpServerDedupFirstCountCore;
using ServerDedupTest::Check;
// Independent wire contract: 32B records occupy the start of 512B GM slots.
constexpr uint32_t countSlotWords = 128U;
constexpr uint32_t countGapPattern = 0xa5a5a5a5U;

void SeedCountSlots(std::vector<uint32_t> &state, uint64_t offset, uint32_t experts)
{
    auto begin = state.begin() + offset / 4U;
    std::fill(begin, begin + 1024U * countSlotWords, countGapPattern);
    for (uint32_t i = 0; i < experts; ++i)
        std::fill(begin + i * countSlotWords, begin + i * countSlotWords + 8U, 0U);
}

void CheckCountGaps(const std::vector<uint32_t> &state, uint64_t offset, uint32_t experts)
{
    const auto begin = state.begin() + offset / 4U;
    for (uint32_t i = 0; i < experts; ++i)
        Check(std::all_of(begin + i * countSlotWords + 8U, begin + (i + 1U) * countSlotWords,
            [](uint32_t value) { return value == countGapPattern; }), "cnt DMA preserves every 480B GM gap");
    Check(std::all_of(begin + experts * countSlotWords, begin + 1024U * countSlotWords,
        [](uint32_t value) { return value == countGapPattern; }), "cnt DMA preserves unused GM slots");
}

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
    for (uint32_t receiver = 0; receiver < recvCoreNum; ++receiver) {
        // Production resets the count pipe at the receiver boundary and reloads
        // the GM prefix. This isolated task test keeps its source alive instead.
        AscendC::TPipe pipe;
        const uint64_t bytesBefore = pipe.bytes;
        const uint64_t copiesBefore = AscendC::testCopies;
        const uint64_t syncBefore = AscendC::TestSyncOperations();
        TileXREp::ServerDedupReceiveTasks tasks;
        tasks.Build(rank.cores[sendCoreNum + receiver].Prefix(), rank.args.rank,
            world, experts, bs, receiver, &pipe);
        const uint32_t end = uint32_t(uint64_t(receiver + 1U) * addresses.size() / recvCoreNum);
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
                for (uint32_t receiver = 0; receiver < recvCoreNum; ++receiver) {
                    AscendC::TPipe pipe;
                    TileXREp::ServerDedupReceiveTasks tasks;
                    tasks.Build(prefix, rank, world, experts, bs, receiver, &pipe);
                    const uint32_t begin = uint32_t(receiver * addresses.size() / recvCoreNum);
                    const uint32_t end = uint32_t((receiver + 1U) * addresses.size() / recvCoreNum);
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

// Incoming records are already available, while this core's outgoing MTE3
// deliberately has made no progress. RecvCnt must compute/publish without
// forcing that unrelated source buffer to retire.
void CheckDeferredCountReceive()
{
    constexpr uint32_t world = 64U, experts = 384U;
    const uint32_t core = 63U, receiver = core - sendCoreNum;
    const uint32_t countCores = kEpServerDedupCountCoreNum;
    const uint32_t countCore = core - kEpServerDedupFirstCountCore;
    const uint32_t start = countCore * (experts / countCores) + std::min(countCore, experts % countCores);
    const uint32_t count = experts / countCores + (countCore < experts % countCores);
    const uint32_t recvCount = experts / recvCoreNum + (receiver < experts % recvCoreNum);
    const uint32_t recvStart = receiver * (experts / recvCoreNum) + std::min(receiver, experts % recvCoreNum);
    const uint32_t activeReceivers = std::min(experts, recvCoreNum);
    const auto layout = TileXREp::TileXREpServerDedupCountLayout(experts);
    std::vector<std::vector<uint32_t>> state(world, std::vector<uint32_t>(layout.stateBytes / 4U, 0U));
    for (auto &window : state) SeedCountSlots(window, layout.cntOffset, experts);
    TileXR::CommArgs args {};
    args.rankSize = world;
    args.peerMemBytes = ServerDedupTest::kPeerBytes;
    ServerDedupTest::SparsePeerMemory memory;
    for (uint32_t rank = 0; rank < world; ++rank) {
        memory.Add(rank, 1U, state[rank].data(), state[rank].size() * sizeof(uint32_t));
        args.peerMems[rank] = reinterpret_cast<uint8_t *>(ServerDedupTest::SparsePeerMemory::Base(rank));
    }
    uint32_t localSum = 0U;
    for (uint32_t i = 0; i < recvCount; ++i) {
        state[0][layout.cntOffset / 4U + (recvStart + i) * countSlotWords] = 0x3f800000U;
        state[0][layout.cntOffset / 4U + (recvStart + i) * countSlotWords + 1U] = i % 5U;
        localSum += i % 5U;
    }
    int32_t route = int32_t(start);
    std::vector<int32_t> prefix(experts);
    std::vector<int64_t> expertOutput(experts / world);
    AscendC::TPipe pipe;
    ServerDedupCounts counts;
    AscendC::testCore = core;
    counts.Init(reinterpret_cast<GM_ADDR>(&args), reinterpret_cast<GM_ADDR>(&route),
        reinterpret_cast<GM_ADDR>(expertOutput.data()), reinterpret_cast<GM_ADDR>(prefix.data()),
        1U, 1U, experts, 0U, 1, &pipe);
    ServerDedupTest::ProfileCapture capture(args);
    TileXREp::ServerDedupProfile profile;
    profile.Init(reinterpret_cast<GM_ADDR>(&args), 0U);
    AscendC::DeferMte3() = true;
    counts.SendCnt(profile);
    Check(!AscendC::PendingMte3().empty(), "SendCnt returns while its source is still pending");
    const auto countWrites = AscendC::PendingMte3().size();
    uint32_t mte3Waits = 0U;
    AscendC::EventObserver() = [&](AscendC::HardEvent event, AscendC::TEventID, bool set) {
        if (!set && (event == AscendC::HardEvent::MTE3_V || event == AscendC::HardEvent::MTE3_S ||
                event == AscendC::HardEvent::MTE3_MTE2)) ++mte3Waits;
    };
    counts.RecvCnt();
    AscendC::EventObserver() = {};
    Check(mte3Waits == 0U, "incoming polling, local sum and relative prefix do not wait for outgoing MTE3");
    Check(AscendC::PendingMte3().size() > countWrites, "local sum and cleanup are issued behind the still-pending cnt");
    Check(counts.ReservedBytes() == (((experts + countCores - 1U) / countCores + 7U) / 8U * 8U) * 32U,
        "only aligned count records remain reserved outside shared scratch");
    Check(pipe.bytes == 190U * 1024U, "count and receiver views share a single bounded UB reservation");
    AscendC::CompleteMte3();
    AscendC::DeferMte3() = false;
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t expert = start + i;
        const uint32_t index = layout.cntOffset / 4U + expert % (experts / world) * world * countSlotWords;
        Check(state[expert / (experts / world)][index] == 0x3f800000U &&
            state[expert / (experts / world)][index + 1U] == (i == 0U ? 1U : 0U),
            "deferred cnt reads retain every record after receiver scratch overwrites count scratch");
    }
    for (uint32_t consumer = 0; consumer < recvCoreNum; ++consumer) {
        const uint32_t index = (layout.cntOffset + layout.cumsumOffset) / 4U +
            (receiver * recvCoreNum + consumer) * 8U;
        Check(state[0][index] == (consumer < activeReceivers ? localSum : 0U) &&
            state[0][index + 1U] == (consumer < activeReceivers ? 0x3f800000U : 0U),
            "deferred local sum publication reaches only active cumsum consumers");
    }
    for (uint32_t i = 0; i < recvCount; ++i)
        Check(state[0][layout.cntOffset / 4U + (recvStart + i) * countSlotWords] == 0U,
            "deferred clear retires each consumed incoming ready flag");
    for (const auto &window : state) CheckCountGaps(window, layout.cntOffset, experts);
    counts.Finish();
}

void Run(uint32_t world, uint32_t experts, uint32_t bs, uint32_t k, uint32_t rounds)
{
    TileXR::CommArgs profileArgs {};
    ServerDedupTest::ProfileCapture capture(profileArgs);
    const uint32_t localExperts = experts / world;
    const uint32_t activeReceivers = std::min(experts, recvCoreNum);
    const auto layout = TileXREp::TileXREpServerDedupCountLayout(experts);
    std::vector<Rank> ranks(world);
    ServerDedupTest::SparsePeerMemory memory;
    for (uint32_t r = 0; r < world; ++r) {
        auto &rank = ranks[r];
        for (uint32_t bank = 0; bank < 2U; ++bank) {
            auto &state = rank.states[bank];
            state.assign(TILEXR_EP_SERVER_DEDUP_SHARED_DATA_OFFSET / 4 + 32, 0);
            SeedCountSlots(state, layout.cntOffset, experts);
            std::fill(state.begin() + (layout.cntOffset + layout.prefixOffset) / 4 + recvCoreNum * experts,
                state.begin() + (layout.cntOffset + layout.cumsumFlagOffset) / 4, 0xa5a5a5a5);
            std::fill(state.begin() + layout.counterOffset / 4, state.end(), 0xa5a5a5a5);
            memory.Add(r, bank, state.data(), state.size() * sizeof(uint32_t));
        }
        rank.args.rank = r;
        rank.args.rankSize = world;
        rank.args.peerMemBytes = ServerDedupTest::kPeerBytes;
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
            AscendC::testCore = id % 64;
            TileXREp::ServerDedupProfile profile;
            profile.Init(reinterpret_cast<GM_ADDR>(&profileArgs), 0U);
            AscendC::StridedCopyObserver() = [&](bool write, const void *, AscendC::DataCopyParams copy) {
                Check(write && copy.blockLen == 1U && copy.srcStride == 0U && copy.dstStride == world * 16U - 1U,
                    "cnt send transfers compact 32B UB records to expert/source GM slots");
            };
            ranks[id / 64].cores[id % 64].SendCnt(profile);
            AscendC::StridedCopyObserver() = {};
            profile.Publish();
            const auto events = capture.Events(id % 64);
#if defined(TILEXR_ENABLE_PROFILING) && TILEXR_ENABLE_PROFILING
            if (AscendC::testCopies > before) {
                Check(events == std::vector<uint32_t>{0x53000216U, 0x53000217U, 0x53000220U, 0x53000221U},
                    "active Count cores publish paired filter and issue intervals in order");
            } else {
                Check(events.empty(), "skipped and empty Count cores publish no artificial intervals");
            }
#else
            Check(events.empty(), "profiling OFF records no Count intervals");
#endif
            Check(AscendC::GetCtrlSpr<60, 60>() == 0,
                "Count preserves the FP overflow mode for subsequent quantization, without resetting the pipe");
            const uint32_t core = id % 64U;
            const bool publishes = core >= kEpServerDedupFirstCountCore &&
                core - kEpServerDedupFirstCountCore < experts;
            Check((AscendC::testCopies > before) == publishes,
                "only trailing Count cores with nonempty expert partitions publish, independently of BS");
            Check(ranks[id / 64U].cores[core].ReservedBytes() ==
                (publishes ? ((experts + kEpServerDedupCountCoreNum - 1U) / kEpServerDedupCountCoreNum + 7U) / 8U * 8U * 32U : 0U),
                "non-Count cores release all scratch while publishers retain aligned records");
        }
        for (uint32_t r = 0; r < world; ++r) for (uint32_t i = 0; i < experts; ++i) {
            Check(ranks[r].states[bank][offset / 4 + i * countSlotWords] == 0x3f800000, "every cnt record, including zero, is notified");
            Check(int32_t(ranks[r].states[bank][offset / 4 + i * countSlotWords + 1]) == expected[r][i], "raw per-expert/source count matches independent route histogram");
        }
        for (const auto &rank : ranks) CheckCountGaps(rank.states[bank], offset, experts);
        std::vector<std::vector<int32_t>> blockOwners(world, std::vector<int32_t>(experts, -1));
        for (uint32_t id : order) if (id % 64 >= sendCoreNum) {
            const uint32_t receiver = id % 64 - sendCoreNum;
            const uint32_t rank = id / 64;
            const auto before = AscendC::testCopies;
            AscendC::StridedCopyObserver() = [&](bool write, const void *source, AscendC::DataCopyParams copy) {
                const uintptr_t begin = reinterpret_cast<uintptr_t>(ranks[rank].states[bank].data()) + offset;
                const uintptr_t address = reinterpret_cast<uintptr_t>(source);
                if (address < begin || address >= begin + experts * 512U) {
                    Check(write, "cnt receive reads only its GM cnt slots");
                    return; // Local segment-sum publication.
                }
                Check(copy.blockCount > 0U && copy.blockLen == 1U &&
                    copy.srcStride == (write ? 0U : 15U) && copy.dstStride == (write ? 15U : 0U),
                    "poll and cleanup transfer only 32B records between sparse GM and compact UB");
                Check(receiver < activeReceivers && address + (copy.blockCount - 1U) * 512U + 32U <= begin + experts * 512U,
                    "only active receivers read cnt and stay within the valid records");
                Check((address - begin + offset) % 512U == 0U,
                    "each cnt polling range starts on a window-relative 512B boundary");
                const uint32_t first = uint32_t((address - begin) / 512U);
                const uint32_t expectedStart = receiver * (experts / recvCoreNum) +
                    std::min(receiver, experts % recvCoreNum);
                const uint32_t expectedCount = experts / recvCoreNum + (receiver < experts % recvCoreNum);
                Check(first == expectedStart && copy.blockCount == expectedCount,
                    "cnt polling balances individual 512B records without rounding ownership to 16");
                if (write) return;
                for (uint32_t block = first; block < first + copy.blockCount; ++block) {
                    auto &owner = blockOwners[rank][block];
                    Check(owner == -1 || owner == int32_t(receiver), "a 512B cnt block has only one polling core");
                    owner = int32_t(receiver);
                }
            };
            ranks[rank].cores[id % 64].RecvCnt();
            AscendC::StridedCopyObserver() = {};
            if (receiver >= activeReceivers)
                Check(AscendC::testCopies == before, "inactive cnt receivers issue no reads or zero-sum publications");
        }
        for (uint32_t rank = 0; rank < world; ++rank) {
            Check(std::all_of(blockOwners[rank].begin(), blockOwners[rank].end(),
                [](int32_t owner) { return owner >= 0; }), "all cnt blocks are polled");
            for (uint32_t publisher = 0; publisher < recvCoreNum; ++publisher) {
                int32_t sum = 0;
                for (uint32_t i = 0; i < experts; ++i)
                    if (blockOwners[rank][i] == int32_t(publisher)) sum += expected[rank][i];
                for (uint32_t consumer = 0; consumer < recvCoreNum; ++consumer) {
                    const uint64_t index = (offset + layout.cumsumOffset) / 4U +
                        (publisher * recvCoreNum + consumer) * 8U;
                    const bool active = publisher < activeReceivers && consumer < activeReceivers;
                    Check(ranks[rank].states[bank][index] == (active ? uint32_t(sum) : 0U) &&
                        ranks[rank].states[bank][index + 1U] == (active ? 0x3f800000U : 0U),
                        "only active publishers and consumers participate in segment-sum exchange");
                }
            }
        }
        std::shuffle(order.begin(), order.end(), rng);
        for (uint32_t id : order) if (id % 64 >= sendCoreNum) {
            const auto before = AscendC::testCopies;
            const auto syncBefore = AscendC::TestSyncOperations();
            ranks[id / 64].cores[id % 64].CalcCumsum();
            if (id % 64 - sendCoreNum >= activeReceivers)
                Check(AscendC::testCopies == before && AscendC::TestSyncOperations() == syncBefore,
                    "inactive receivers skip cumsum IO and synchronization");
        }
        std::shuffle(order.begin(), order.end(), rng);
        for (uint32_t id : order) if (id % 64 >= sendCoreNum) {
            ranks[id / 64].cores[id % 64].CompleteCumsum();
            auto prefix = ranks[id / 64].cores[id % 64].Prefix();
            int32_t cumulative = 0;
            for (uint32_t i = 0; i < experts; ++i) {
                cumulative += expected[id / 64][i];
                Check(prefix.GetValue(i) == cumulative, "every receiver, including inactive cumsum cores, loads the final prefix");
            }
        }
        for (uint32_t r = 0; r < world; ++r)
            CheckReceiveTasks(ranks[r], expected[r], world, experts, bs, k);
        for (auto &rank : ranks) for (auto &core : rank.cores) core.Finish();

        for (uint32_t r = 0; r < world; ++r) {
            const auto &rank = ranks[r];
            CheckCountGaps(rank.states[bank], offset, experts);
            Check(rank.states[1U - bank] == inactive[r], "count phases preserve the other round bank");
            int64_t cumulative = 0, previousExpert = 0;
            for (uint32_t i = 0; i < experts; ++i) {
                cumulative += expected[r][i];
                Check(rank.output[i + 1] == cumulative, "inclusive sendCountsOut follows localExpert/source order");
                Check(rank.states[bank][offset / 4 + i * countSlotWords] == 0, "cnt ready flag cleared by its receiver");
                for (uint32_t receiver = 0; receiver < recvCoreNum; ++receiver)
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
                for (uint64_t byte = begin; byte < begin + recvCoreNum * recvCoreNum * 32U; byte += 4)
                    Check(rank.states[bank][(offset + byte) / 4] == 0, "segment and full-prefix readiness cleaned");
            }
            Check(rank.output.front() == -777 && rank.output.back() == -777 &&
                rank.expertOutput.front() == -888 && rank.expertOutput.back() == -888,
                "exact output bytes preserve adjacent canaries");
            Check(std::all_of(rank.states[bank].begin() + layout.counterOffset / 4, rank.states[bank].end(),
                [](uint32_t v) { return v == 0xa5a5a5a5; }), "Count preserves counter, Combine control and payload space");
            Check(std::all_of(rank.states[bank].begin() + (offset + layout.prefixOffset) / 4 + recvCoreNum * experts,
                rank.states[bank].begin() + (offset + layout.cumsumFlagOffset) / 4,
                [](uint32_t v) { return v == 0xa5a5a5a5; }), "compact copies preserve all unused prefix reservation");
        }
    }
}
} // namespace

int main()
{
    try {
            CheckReceiveTaskBoundaries();
            CheckDeferredCountReceive();
            Run(16, 16, 1, 1, 4);       // L=1, idle count/source cores, zero receive ranks.
            Run(24, 24, 5, 6, 4);       // Non-divisible receiver segments and output tails.
            Run(16, 48, 1, 1, 4);       // One or two records per core; no 16-record ownership padding.
            Run(64, 384, 1, 6, 4);      // All receivers active, including fractional vector groups.
            Run(32, 384, 128, 6, 2);    // User's 32P case; 28/36 splits counts into 10/11-record ranges.
            Run(16, 64, 7, 6, 4);       // Per-expert reduction below 64 routes.
            Run(16, 64, 8, 8, 4);       // Vector-range selection at 64 routes.
            Run(32, 96, 47, 6, 4);      // Exercises BS between send-core count and the default threshold.
            Run(64, 384, 48, 6, 4);     // Historical small-BS boundary keeps configured Count ownership.
            Run(64, 384, 96, 6, 2);     // Common BS96 case keeps configured Count ownership.
            Run(24, 1008, 49, 32, 4);   // Non-power-of-two topology with unaligned partition tails.
            Run(16, 1024, 256, 32, 4);  // Per-card L=64, including the single Count core configuration.
            Run(128, 1024, 48, 8, 4);   // 128P / eight experts per card.
            Run(16, 64, 511U, 1, 2);
            Run(16, 64, 512U, 1, 2);
            Run(16, 64, 513U, 1, 2); // Former BS boundary no longer changes Count ownership.
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return ServerDedupTest::Finish("counts");
}
