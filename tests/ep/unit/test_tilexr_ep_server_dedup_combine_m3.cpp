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

int main()
{
    try {
        DirectRows(0U, 17U, false, 0U, 0U);
        DirectRows(1U, 17U, false, 1U, 1U);
        DirectRows(97U, 240U, false, 0U, 0U);
        DirectRows(193U, 241U, false, 1U, 1U);
        DirectRows(512U, 7168U, false, 0U, 0U); // wrap input/output rings
        DirectRows(10000U, 17U, true, 1U, 1U); // sparse table-cache span transition
    } catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
    AscendC::GmWriteObserver() = {};
    if (ServerDedupTest::Failures() != 0) return 1;
    std::cout << "EP_SERVER_DEDUP_COMBINE_M3_PASS\n";
    return 0;
}
