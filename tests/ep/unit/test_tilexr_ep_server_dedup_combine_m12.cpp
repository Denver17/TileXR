#include "combine_m12_test_fixture.h"
#include "../../../src/ep/kernels/tilexr_ep_combine_memory_server_dedup_kernel.h"

using ServerDedupTest::Check;
using CombineM12Test::Fixture;
void Run(Fixture &f, uint32_t core, uint32_t window)
{
    f.Hooks(core, window);
    TileXREp::CombineMemoryServerDedupKernel kernel;
    kernel.Init(reinterpret_cast<GM_ADDR>(&f.args), reinterpret_cast<GM_ADDR>(f.expert.data()),
        reinterpret_cast<GM_ADDR>(f.table.data()), reinterpret_cast<GM_ADDR>(f.counts.data()),
        f.untouched.data(), reinterpret_cast<GM_ADDR>(f.types.data()), f.untouched.data(),
        f.untouched.data(), f.untouched.data(), f.tiling.bs, f.h, 3U, 320U, 0U, 0U, window, 0U);
    kernel.Process();
    Check(AscendC::PendingMte3().empty(), "all DMA retired before local completion");
    Check(f.observedPrepare == 8U, "every core publishes all eight prepare copies");
    if (core >= 56U) {
        Check(f.observedNotify == TileXREp::kEpServerDedupCombineRecvCoreNum,
            "notifier publishes only actual Read core slots on assigned card");
        const uint8_t *done = f.Local(window, (core - 56U) * 2048U);
        Check(std::all_of(done, done + 2048U, [](uint8_t b) { return b == 0U; }), "prepare consumer clears own entire row");
        if (f.delayed) Check(f.prepareProbes == 2U, "partial prepare sum waits and rechecks");
    }
    if (core >= TileXREp::kEpServerDedupCombineSendCoreNum) {
        const uint8_t *ready = f.Local(window, TILEXR_EP_SERVER_DEDUP_COMBINE_SERVER_READY_OFFSET +
            (core - TileXREp::kEpServerDedupCombineSendCoreNum) * 512U);
        Check(std::all_of(ready, ready + 512U, [](uint8_t b) { return b == 0U; }), "Read core clears own complete512B block");
        if (f.delayed) Check(f.readyProbes == 2U, "partial server sum waits and rechecks");
    }
    f.ResetHooks();
}
void Rows(uint32_t count, uint32_t h, bool sparse)
{
    Fixture f(count, h);
    if (count == 1U) f.types[0] = 3;
    if (sparse) for (uint32_t row = 0; row < count; ++row) f.types[row] = (row == 0U || (row >= 600U && row < 728U)) ? 3 : 0;
    f.delayed = true;
    const auto expertBefore = f.expert;
    const auto tableBefore = f.table;
    const auto typesBefore = f.types;
    // Multiple invocations exercise both windows and same-window control reuse.
    for (uint32_t window : {0U, 1U, 1U}) {
        for (uint32_t core = 0; core < 64U; ++core) Run(f, core, window);
        for (uint32_t row = 0; row < count; ++row) {
            const uint64_t slot = uint32_t(f.table[row * 3U]);
            const uint8_t *dst = f.peers[f.rank].data() + window * f.half + f.stageOffset + slot * f.stageStride;
            if (f.types[row] == 3) Check(std::memcmp(dst, f.expert.data() + uint64_t(row) * h, h * 2U) == 0,
                "type3 original row uses table2 stage slot and unweighted raw payload");
            else Check(std::all_of(dst, dst + h * 2U, [](uint8_t b) { return b == 0xa5; }), "other token types never stage");
            Check(std::all_of(dst + h * 2U, dst + f.stageStride, [](uint8_t b) { return b == 0xa5; }), "stage alignment gap preserved");
        }
    }
    Check(f.expert == expertBefore && f.table == tableBefore && f.types == typesBefore, "input tensors unchanged");
    Check(std::all_of(f.untouched.begin(), f.untouched.end(), [](uint8_t b) { return b == 0x69; }), "unused tables and yOut unchanged");
}
int main()
{
    try {
        Rows(0U, 17U, false);
        Rows(1U, 17U, false);
        Rows(193U, 513U, false);
        Rows(193U, 7168U, false);
        Rows(10000U, 17U, false);
        Rows(10000U, 17U, true);
        // Check the exact one-lane admission boundary without allocating the
        // multi-gigabyte worst-case peer memories in a Host substitute.
        uint32_t h = 1U;
        while (TileXREp::EpCombinePayloadLaneCount(10000U, h + 1U) != 0U) ++h;
        Check(TileXREp::EpCombinePayloadLaneCount(10000U, h) == 1U, "legal maximum H retains one lane");
        Check(TileXREp::EpCombinePayloadLaneCount(10000U, h + 1U) == 0U, "oversized payload has no fabricated lane");
        Fixture bad(1U, 17U); bad.counts.back() = 10001;
        bool trapped = false;
        try { Run(bad, 0U, 0U); } catch (const std::runtime_error &) { trapped = true; }
        Check(trapped, "unsupported device TokenNum fails explicitly");
        bad.counts.back() = 1537;
        trapped = false;
        try { Run(bad, 0U, 0U); } catch (const std::runtime_error &) { trapped = true; }
        Check(trapped, "device TokenNum beyond declared tensor capacity fails before any row read");
    } catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
    if (ServerDedupTest::Failures() != 0) return 1;
    std::cout << "EP_SERVER_DEDUP_COMBINE_M12_PASS\n";
    return 0;
}
