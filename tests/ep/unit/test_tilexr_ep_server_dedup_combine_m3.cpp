// Execute the production kernel against byte-exact GM and deferred MTE3.
// This proves routing/packing/ownership, not device visibility or performance.
#include "combine_m12_test_fixture.h"
#include "../../../src/ep/kernels/tilexr_ep_combine_memory_server_dedup_kernel.h"
#include <set>

using ServerDedupTest::Check;
using CombineM12Test::Fixture;

// Reference ordering is independent of the kernel's per-server online scan:
// sort the complete routed stream, then stripe its positions across Send cores.
std::vector<uint32_t> DirectSchedule(const Fixture &f, const std::vector<uint32_t> &selected)
{
    auto schedule = selected;
    const uint32_t servers = uint32_t(f.args.rankSize) / 8U;
    const uint32_t ownServer = uint32_t(f.args.rank) / 8U;
    const auto distance = [&](uint32_t row) {
        return (uint32_t(f.table[row * 3U]) / 8U + servers - ownServer - 1U) % servers;
    };
    std::stable_sort(schedule.begin(), schedule.end(), [&](uint32_t a, uint32_t b) {
        return distance(a) < distance(b);
    });
    return schedule;
}

void DirectRows(uint32_t count, uint32_t h, bool sparse, uint32_t window, uint8_t dtype)
{
    Fixture f(count, h);
    const uint64_t resultStride = ((uint64_t(h) * 2U + 479U) / 480U) * 512U;
    const uint32_t slots = f.tiling.bs * 3U;
    // M3 also targets other servers; M2 retains its same-server peer hooks.
    for (uint32_t rank = 0; rank < 16U; ++rank) if (f.peers[rank].empty()) {
        f.peers[rank].resize(f.half * 2U, 0xa5);
        f.args.peerMems[rank] = f.peers[rank].data();
    }
    std::vector<uint32_t> selected;
    for (uint32_t row = 0; row < count; ++row) {
        bool direct = sparse ? (row < 32U || (row >= 9500U && row < 9533U)) : row % 3U != 1U;
        f.types[row] = direct ? 1 : (sparse ? 0 : 3);
        const uint32_t source = row / slots;
        const uint32_t slot = row % slots;
        f.table[row * 3U] = int32_t(f.types[row] == 3 ? count - 1U - row : source);
        f.table[row * 3U + 1U] = int32_t(slot / 3U);
        f.table[row * 3U + 2U] = int32_t(slot % 3U);
        if (direct) selected.push_back(row);
    }
    const auto expertBefore = f.expert;
    const auto tableBefore = f.table;
    const auto typesBefore = f.types;
    std::set<uint32_t> allWrites;
    uint32_t cacheCopies = 0U;
    for (uint32_t core = 0; core < 64U; ++core) {
        f.Hooks(core, window);
        const auto controlRead = AscendC::GmReadObserver();
        AscendC::GmReadObserver() = [&, controlRead](const void *src, uint32_t bytes) {
            controlRead(src, bytes);
            const auto address = reinterpret_cast<uintptr_t>(src);
            const auto expertBegin = reinterpret_cast<uintptr_t>(f.expert.data());
            const auto tableBegin = reinterpret_cast<uintptr_t>(f.table.data());
            if (!f.expert.empty() && address >= expertBegin && address < expertBegin + f.expert.size() * 2U) {
                Check(bytes == h * 2U && address + bytes <= expertBegin + f.expert.size() * 2U,
                    "M1/M3 read only the H valid GM elements, including last row");
            }
            if (!f.table.empty() && address >= tableBegin && address < tableBegin + f.table.size() * 4U) {
                ++cacheCopies;
                Check(bytes <= 512U * 12U && bytes % 12U == 0U &&
                    address + bytes <= tableBegin + f.table.size() * 4U,
                    "table cache is one bounded original-row span without GM overread");
            }
        };
        std::set<uint32_t> writes;
        AscendC::GmWriteObserver() = [&](const void *dst, const void *, uint32_t bytes) {
            bool matched = false;
            for (uint32_t rank = 0; rank < 16U; ++rank) {
                const auto begin = reinterpret_cast<uintptr_t>(f.peers[rank].data()) + window * f.half +
                    TILEXR_EP_SERVER_DEDUP_SHARED_DATA_OFFSET;
                const auto address = reinterpret_cast<uintptr_t>(dst);
                if (address >= begin && address < begin + slots * resultStride) {
                    Check(bytes == resultStride && (address - begin) % resultStride == 0U,
                        "DIRECT writes exactly one DataAsFlag result slot");
                    const uint32_t row = rank * slots + uint32_t((address - begin) / resultStride);
                    Check(writes.insert(row).second && allWrites.insert(row).second,
                        "every DIRECT destination is written exactly once across Send cores");
                    matched = true;
                }
            }
            Check(matched, "M3 has no write outside selected-window result slots");
        };
        TileXREp::CombineMemoryServerDedupKernel kernel;
        kernel.Init(reinterpret_cast<GM_ADDR>(&f.args), reinterpret_cast<GM_ADDR>(f.expert.data()),
            reinterpret_cast<GM_ADDR>(f.table.data()), reinterpret_cast<GM_ADDR>(f.counts.data()),
            f.untouched.data(), reinterpret_cast<GM_ADDR>(f.types.data()), f.untouched.data(),
            f.untouched.data(), f.untouched.data(), f.tiling.bs, f.h, 3U, 320U, dtype, 0U, window, 0U);
        TileXREp::CombineModuleTestAccess::ThroughM4(kernel);
        Check(AscendC::PendingMte3().empty(), "M3 locally drains all outstanding sends");
        const uint32_t sendCores = TileXREp::kEpServerDedupCombineSendCoreNum;
        const auto schedule = DirectSchedule(f, selected);
        std::set<uint32_t> expected;
        if (core < sendCores) for (uint32_t task = core; task < schedule.size(); task += sendCores)
            expected.insert(schedule[task]);
        Check(writes == expected, "server-ordered DIRECT stream divides evenly across Send cores only");
        AscendC::GmWriteObserver() = {};
        AscendC::GatherMaskObserver() = {};
        f.ResetHooks();
    }
    Check(allWrites.size() == selected.size(), "every selected row returned once");
    if (sparse) Check(cacheCopies > std::min<size_t>(selected.size(), TileXREp::kEpServerDedupCombineSendCoreNum),
        "a sparse gap wider than table cache causes an additional bounded read");
    for (uint32_t rank = 0; rank < 16U; ++rank) for (uint32_t slot = 0; slot < slots; ++slot) {
        const uint32_t row = rank * slots + slot;
        const uint8_t *dst = f.peers[rank].data() + window * f.half +
            TILEXR_EP_SERVER_DEDUP_SHARED_DATA_OFFSET + slot * resultStride;
        const uint8_t *other = f.peers[rank].data() + (1U - window) * f.half +
            TILEXR_EP_SERVER_DEDUP_SHARED_DATA_OFFSET + slot * resultStride;
        Check(std::all_of(other, other + resultStride, [](uint8_t v) { return v == 0xa5; }),
            "alternate communication window remains untouched");
        if (row < count && f.types[row] == 1) {
            const uint8_t *src = reinterpret_cast<const uint8_t *>(f.expert.data() + uint64_t(row) * h);
            for (uint32_t offset = 0; offset < h * 2U; ++offset)
                Check(dst[(offset / 480U) * 512U + offset % 480U] == src[offset],
                    "raw unweighted original-row BF16/FP16 payload preserves every bit");
            for (uint32_t block = 0; block < resultStride / 512U; ++block) {
                float flag; std::memcpy(&flag, dst + block * 512U + 480U, 4U);
                Check(flag == 1.0f, "every DataAsFlag block publishes ready first float");
            }
        } else Check(std::all_of(dst, dst + resultStride, [](uint8_t v) { return v == 0xa5; }),
            "inactive, unused and non-produced result slots remain unchanged");
    }
    Check(f.expert == expertBefore && f.table == tableBefore && f.types == typesBefore,
        "M3 does not alter input tensors");
    Check(std::all_of(f.untouched.begin(), f.untouched.end(), [](uint8_t x) { return x == 0x69; }),
        "M3 leaves yOut and unused inputs unchanged");
}

void DirectServerOrder(uint32_t ranks, uint32_t ownRank, uint32_t count, uint32_t h,
    uint32_t window, bool onlyLocal, bool clustered = false)
{
    Fixture f(count, h, ownRank);
    f.args.rankSize = int(ranks);
    f.tiling.bs = std::max(1U, (count + 2U) / 3U);
    const uint32_t slots = f.tiling.bs * 3U;
    const uint32_t packet = (h * 2U + 479U) / 480U * 512U;
    f.half = TILEXR_EP_SERVER_DEDUP_SHARED_DATA_OFFSET + uint64_t(slots) * packet + 512U;
    f.args.peerMemBytes = f.half * 2U;
    const uint32_t servers = ranks / 8U, ownServer = ownRank / 8U;
    for (uint32_t row = 0; row < count; ++row) {
        f.types[row] = 1;
        // Cover both periodic routing and expert-sorted source-local clusters.
        // Uneven cluster lengths require the ownership cursor to carry between
        // servers; resetting it each round would overload the first few cores.
        const uint32_t route = clustered ? row / (3U * TileXREp::kEpServerDedupCombineSendCoreNum + 1U) : row * 5U;
        const uint32_t server = onlyLocal ? ownServer : (route + ownServer) % servers;
        const uint32_t source = server * 8U + row % 8U;
        f.table[row * 3U] = int32_t(source);
        f.table[row * 3U + 1U] = int32_t(row / 3U);
        f.table[row * 3U + 2U] = int32_t(row % 3U);
        if (f.peers[source].size() != f.half * 2U) {
            f.peers[source].assign(f.half * 2U, 0xa5U);
            f.args.peerMems[source] = f.peers[source].data();
        }
    }
    const auto tableBefore = f.table;
    std::vector<uint32_t> selected;
    for (uint32_t row = 0; row < count; ++row) selected.push_back(row);
    const auto schedule = DirectSchedule(f, selected);
    std::set<uint32_t> allWrites;
    const uint32_t sendCores = TileXREp::kEpServerDedupCombineSendCoreNum;
    std::vector<std::vector<uint32_t>> serverCounts(servers, std::vector<uint32_t>(sendCores));
    std::vector<uint32_t> totalCounts(sendCores);
    bool observedOverlap = false;
    for (uint32_t core = 0; core < sendCores; ++core) {
        AscendC::testCore = core;
        AscendC::DeferMte3() = true;
        std::vector<uint32_t> expected;
        for (uint32_t task = core; task < schedule.size(); task += sendCores) expected.push_back(schedule[task]);
        bool payloadStarted = false;
        uint32_t scan = 0U, currentRow = 0U;
        uint64_t lastIssuePosition = AscendC::CompletedMte3();
        std::set<uint32_t> seededLanes;
        std::vector<uint32_t> issued, observed;
        AscendC::GmReadObserver() = [&](const void *src, uint32_t bytes) {
            const auto address = reinterpret_cast<uintptr_t>(src);
            const auto table = reinterpret_cast<uintptr_t>(f.table.data());
            const auto expert = reinterpret_cast<uintptr_t>(f.expert.data());
            if (count && address >= table && address < table + count * 12U) {
                Check(address + bytes <= table + count * 12U, "Table2 scanning stays within bounds");
                if (!seededLanes.empty()) Check(bytes == 12U,
                    "after initialization M3 reads only an owned route, never rescans Table2");
            }
            if (count && address >= expert && address < expert + uint64_t(count) * h * 2U) {
                payloadStarted = true;
                currentRow = uint32_t((address - expert) / (h * 2U));
                const uint32_t server = uint32_t(f.table[currentRow * 3U]) / 8U;
                Check(scan != 0U && server == (ownServer + scan) % servers,
                    "each server's payload begins immediately after its own scan");
            }
        };
        AscendC::GatherMaskObserver() = [&](uint32_t candidates, uint64_t matches) {
            if (seededLanes.empty()) return; // type compaction and rank validation
            Check(candidates == count, "one Vector compaction covers the full DIRECT list");
            uint32_t expectedIssues = 0U, expectedMatches = 0U;
            const uint32_t server = (ownServer + scan + 1U) % servers;
            for (uint32_t row : selected) if (uint32_t(f.table[row * 3U]) / 8U == server) ++expectedMatches;
            Check(matches == expectedMatches, "GatherMask returns the exact server token count");
            for (uint32_t row : expected) {
                const uint32_t distance = (uint32_t(f.table[row * 3U]) / 8U + servers - ownServer - 1U) % servers;
                if (distance < scan) ++expectedIssues;
            }
            Check(issued.size() == expectedIssues, "prior server issues precede the next Vector selection");
            if (scan != 0U) {
                const uint32_t previous = (ownServer + scan) % servers;
                if (std::any_of(expected.begin(), expected.end(), [&](uint32_t row) {
                        return uint32_t(f.table[row * 3U]) / 8U == previous; })) {
                    Check(!AscendC::PendingMte3().empty(), "Vector selection leaves previous sends in flight");
                    observedOverlap = true;
                }
            }
            ++scan;
        };
        AscendC::EventObserver() = [&](AscendC::HardEvent event, AscendC::TEventID lane, bool publish) {
            if (event != AscendC::HardEvent::MTE3_V || !publish) return;
            const uint64_t position = AscendC::CompletedMte3() + AscendC::PendingMte3().size();
            if (position == lastIssuePosition) {
                Check(!payloadStarted && seededLanes.insert(lane).second,
                    "DIRECT output lanes are seeded once before the first payload");
                return;
            }
            Check(position == lastIssuePosition + 1U && !seededLanes.empty(),
                "each DIRECT completion event follows one newly issued MTE3");
            Check(lane == issued.size() % seededLanes.size(),
                "DIRECT lane cursor rotates continuously across server boundaries");
            lastIssuePosition = position;
            issued.push_back(currentRow);
        };
        AscendC::GmWriteObserver() = [&](const void *dst, const void *src, uint32_t bytes) {
            const auto address = reinterpret_cast<uintptr_t>(dst);
            bool matched = false;
            for (uint32_t rank = 0; rank < ranks; ++rank) {
                if (f.peers[rank].empty()) continue;
                const auto begin = reinterpret_cast<uintptr_t>(f.peers[rank].data()) +
                    window * f.half + TILEXR_EP_SERVER_DEDUP_SHARED_DATA_OFFSET;
                if (address < begin || address >= begin + uint64_t(slots) * packet) continue;
                const uint32_t row = uint32_t((address - begin) / packet);
                Check(row < count && bytes == packet && (address - begin) % packet == 0U,
                    "grouped sends preserve the exact token/topk slot");
                Check(uint32_t(f.table[row * 3U]) == rank && allWrites.insert(row).second,
                    "grouped DIRECT ownership and destination rank are unchanged");
                observed.push_back(row);
                const auto packed = static_cast<const uint8_t *>(src);
                const auto original = reinterpret_cast<const uint8_t *>(f.expert.data() + uint64_t(row) * h);
                for (uint32_t byte = 0; byte < h * 2U; ++byte)
                    Check(packed[byte / 480U * 512U + byte % 480U] == original[byte],
                        "grouping preserves the original expert row, including packet tail");
                for (uint32_t block = 0; block < packet / 512U; ++block) {
                    float flag; std::memcpy(&flag, packed + block * 512U + 480U, 4U);
                    Check(flag == 1.0f, "grouped DIRECT publishes every block Flag");
                }
                matched = true;
            }
            Check(matched, "grouped send remains inside the active result window");
        };
        TileXREp::CombineMemoryServerDedupKernel kernel;
        kernel.Init(reinterpret_cast<GM_ADDR>(&f.args), reinterpret_cast<GM_ADDR>(f.expert.data()),
            reinterpret_cast<GM_ADDR>(f.table.data()), reinterpret_cast<GM_ADDR>(f.counts.data()),
            f.untouched.data(), reinterpret_cast<GM_ADDR>(f.types.data()), f.untouched.data(),
            f.untouched.data(), f.untouched.data(), f.tiling.bs, h, 3U, ranks * 2U, 0U, 0U, window, 0U);
        TileXREp::CombineModuleTestAccess::Direct(kernel);
        Check(scan == servers, "exactly one Vector selection per server, including empty shares");
        Check(issued == expected && observed == expected,
            "striped DIRECT ownership is exact, next-server first, wrapped, and stable within each server");
        Check(AscendC::PendingMte3().empty(), "DIRECT retires all sends at M3 completion");
        for (uint32_t row : observed) ++serverCounts[uint32_t(f.table[row * 3U]) / 8U][core];
        totalCounts[core] = uint32_t(observed.size());
        AscendC::EventObserver() = {};
        AscendC::GmWriteObserver() = {};
        AscendC::GatherMaskObserver() = {};
        f.ResetHooks();
    }
    Check(allWrites.size() == count && f.table == tableBefore, "online scans cover every row without modifying GM Table2");
    for (const auto &counts : serverCounts)
        Check(*std::max_element(counts.begin(), counts.end()) - *std::min_element(counts.begin(), counts.end()) <= 1U,
            "each server's DIRECT work is balanced across all Send cores");
    Check(*std::max_element(totalCounts.begin(), totalCounts.end()) - *std::min_element(totalCounts.begin(), totalCounts.end()) <= 1U,
        "aggregate DIRECT work remains balanced across server remainders");
    if (servers > 1U && !onlyLocal && count > sendCores * servers)
        Check(observedOverlap, "multi-server routing exercised metadata scanning with MTE3 still pending");
}

int main()
{
    try {
        DirectRows(0U, 17U, false, 0U, 0U);
        DirectRows(1U, 17U, false, 1U, 1U);
        DirectRows(97U, 240U, false, 0U, 0U);
        DirectRows(193U, 241U, false, 1U, 1U);
        DirectRows(512U, 7168U, false, 0U, 0U); // wrap input/output rings
        DirectRows(10000U, 17U, true, 1U, 1U); // sparse table-cache span transition
        DirectServerOrder(8U, 2U, 33U, 17U, 0U, false);
        DirectServerOrder(16U, 2U, 1U, 17U, 0U, false);
        DirectServerOrder(24U, 10U, TileXREp::kEpServerDedupCombineSendCoreNum * 12U + 3U, 241U, 1U, false);
        DirectServerOrder(32U, 26U, TileXREp::kEpServerDedupCombineSendCoreNum * 12U, 17U, 0U, false);
        DirectServerOrder(64U, 10U, 403U, 17U, 0U, false); // ~70% of 96*6, only 1-2 hits/core/server
        DirectServerOrder(128U, 122U, TileXREp::kEpServerDedupCombineSendCoreNum * 12U, 17U, 1U, false);
        DirectServerOrder(32U, 10U, TileXREp::kEpServerDedupCombineSendCoreNum * 12U, 17U, 0U, true);
        DirectServerOrder(32U, 26U, TileXREp::kEpServerDedupCombineSendCoreNum * 16U + 5U, 17U, 1U, false, true);
        DirectServerOrder(16U, 10U, 10000U, 1U, 0U, false, true);
    } catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
    AscendC::EventObserver() = {};
    AscendC::GmWriteObserver() = {};
    if (ServerDedupTest::Failures() != 0) return 1;
    std::cout << "EP_SERVER_DEDUP_COMBINE_M3_PASS\n";
    return 0;
}
