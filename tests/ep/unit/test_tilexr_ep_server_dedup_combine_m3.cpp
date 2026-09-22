// Execute the production kernel against byte-exact GM and deferred MTE3.
// This proves routing/packing/ownership, not device visibility or performance.
#include "combine_m12_test_fixture.h"
#include "../../../src/ep/kernels/tilexr_ep_combine_memory_server_dedup_kernel.h"
#include <set>

using ServerDedupTest::Check;
using CombineM12Test::Fixture;

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
        const uint32_t base = uint32_t(selected.size()) / sendCores, extra = uint32_t(selected.size()) % sendCores;
        const uint32_t start = core * base + std::min(core, extra);
        std::set<uint32_t> expected;
        if (core < sendCores) for (uint32_t task = 0; task < base + (core < extra ? 1U : 0U); ++task)
            expected.insert(selected[start + task]);
        Check(writes == expected, "original DIRECT rows divide densely across Send cores only");
        AscendC::GmWriteObserver() = {};
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
    uint32_t window, bool onlyLocal)
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
        // Interleave servers within every core's segment; some lists are empty
        // when the segment has fewer tasks than servers. Local-only tests leave
        // every remote list empty while preserving the entire local segment.
        const uint32_t server = onlyLocal ? ownServer : (row * 5U + ownServer) % servers;
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
    std::set<uint32_t> allWrites;
    const uint32_t sendCores = TileXREp::kEpServerDedupCombineSendCoreNum;
    for (uint32_t core = 0; core < sendCores; ++core) {
        AscendC::testCore = core;
        AscendC::DeferMte3() = true;
        bool payloadStarted = false;
        std::vector<uint32_t> observed;
        AscendC::GmReadObserver() = [&](const void *src, uint32_t bytes) {
            const auto address = reinterpret_cast<uintptr_t>(src);
            const auto table = reinterpret_cast<uintptr_t>(f.table.data());
            const auto expert = reinterpret_cast<uintptr_t>(f.expert.data());
            if (count && address >= table && address < table + count * 12U)
                Check(!payloadStarted && address + bytes <= table + count * 12U,
                    "all Table2 scanning precedes DIRECT payloads and stays within bounds");
            if (count && address >= expert && address < expert + uint64_t(count) * h * 2U)
                payloadStarted = true;
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
        const uint32_t base = count / sendCores, extra = count % sendCores;
        const uint32_t start = core * base + std::min(core, extra);
        std::vector<uint32_t> expected;
        for (uint32_t i = 0; i < base + (core < extra ? 1U : 0U); ++i) expected.push_back(start + i);
        const auto distance = [&](uint32_t row) {
            const uint32_t server = uint32_t(f.table[row * 3U]) / 8U;
            return (server + servers - ownServer - 1U) % servers;
        };
        std::stable_sort(expected.begin(), expected.end(), [&](uint32_t a, uint32_t b) {
            return distance(a) < distance(b);
        });
        Check(observed == expected, "DIRECT visits next server first, wraps, sends local last, and is stable within each server");
        Check(AscendC::PendingMte3().empty(), "grouped DIRECT retires all sends before M5");
        AscendC::GmWriteObserver() = {};
        f.ResetHooks();
    }
    Check(allWrites.size() == count && f.table == tableBefore, "prescan covers every owned row and does not modify GM Table2");
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
        DirectServerOrder(16U, 2U, 1U, 17U, 0U, false);
        DirectServerOrder(24U, 10U, TileXREp::kEpServerDedupCombineSendCoreNum * 12U + 3U, 241U, 1U, false);
        DirectServerOrder(32U, 26U, TileXREp::kEpServerDedupCombineSendCoreNum * 12U, 17U, 0U, false);
        DirectServerOrder(128U, 122U, TileXREp::kEpServerDedupCombineSendCoreNum * 12U, 17U, 1U, false);
        DirectServerOrder(32U, 10U, TileXREp::kEpServerDedupCombineSendCoreNum * 12U, 17U, 0U, true);
    } catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
    AscendC::GmWriteObserver() = {};
    if (ServerDedupTest::Failures() != 0) return 1;
    std::cout << "EP_SERVER_DEDUP_COMBINE_M3_PASS\n";
    return 0;
}
