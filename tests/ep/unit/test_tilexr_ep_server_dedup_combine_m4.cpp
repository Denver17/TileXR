// Production M4 execution with byte-addressed peers and deferred MTE3. Numeric
// reference uses scalar bit conversion; fake vector Cast uses frexp-style math.
#include "combine_m12_test_fixture.h"
#include "../../../src/ep/kernels/tilexr_ep_combine_memory_server_dedup_kernel.h"
#include "../integration/server_dedup_combine_m4_reference.h"
#include <set>

using ServerDedupTest::Check;
using CombineM12Test::Fixture;
namespace Ref = CombineM4Reference;

struct PartialFixture : Fixture {
    uint32_t k, bs, slots, stageRows, window;
    bool bf16, ties;
    uint64_t packet;
    std::vector<uint32_t> relay, selected;
    PartialFixture(uint32_t n, uint32_t width, uint32_t topk, uint32_t selectedWindow,
        bool bfloat, bool sparse, bool rounding = false)
        : Fixture(n, width), k(topk), bs(std::max(16U, (n + 31U) / 32U)), slots(bs * k),
          stageRows(32U * bs), window(selectedWindow), bf16(bfloat), ties(rounding)
    {
        packet = ((uint64_t(h) * 2U + 479U) / 480U) * 512U;
        stageOffset = TILEXR_EP_SERVER_DEDUP_SHARED_DATA_OFFSET + slots * packet;
        half = stageOffset + stageRows * stageStride + 512U;
        args.peerMemBytes = half * 2U;
        counts.assign(32U, int32_t(n));
        relay.assign(uint64_t(n) * k * 2U, 0x7fa12345U); // unused entries must never be parsed
        for (uint32_t r = 0; r < 16U; ++r) {
            peers[r].assign(half * 2U, 0xa5U);
            args.peerMems[r] = peers[r].data();
            for (uint32_t w = 0; w < 2U; ++w) std::memset(LocalFor(r, w,
                TILEXR_EP_SERVER_DEDUP_COMBINE_CONTROL_OFFSET), 0, TILEXR_EP_SERVER_DEDUP_COMBINE_CONTROL_BYTES);
            for (uint32_t slot = 0; slot < stageRows; ++slot) for (uint32_t col = 0; col < h; ++col) {
                const uint16_t value = StageBits(r, slot, col);
                std::memcpy(LocalFor(r, window, stageOffset + slot * stageStride) + col * 2U, &value, 2U);
            }
        }
        for (uint32_t row = 0; row < n; ++row) {
            const bool active = !sparse || row < 32U || (row >= 9500U && row < 9533U);
            types[row] = active ? 2 : 0; // inactive synthetic rows isolate M4 selection
            table[row * 3U] = int32_t(row / slots);
            table[row * 3U + 1U] = int32_t((row % slots) / k);
            table[row * 3U + 2U] = int32_t(row % k);
            for (uint32_t col = 0; col < h; ++col) expert[uint64_t(row) * h + col] = ties ?
                uint16_t((bf16 ? 0x3f80U : 0x3c00U) + (col % 2U)) : Ref::ExpertBits(bf16, rank, row, col);
            if (!active) continue;
            selected.push_back(row);
            const uint32_t members = ties ? 1U : 1U + row % (k - 1U);
            relay[uint64_t(row) * k * 2U] = members;
            for (uint32_t j = 0; j <= members; ++j) {
                const uint32_t memberRank = j == 1U ? rank : 8U + (row + j) % 8U;
                const uint32_t q = (row * 3U + j * 7U) % bs;
                if (j) relay[(uint64_t(row) * k + j) * 2U] = (memberRank * 2U + (row + j) % 2U) | (q << 16U);
                const float weight = ties ? (j ? std::ldexp(1.0f, bf16 ? -8 : -11) : 1.0f) :
                    Ref::Weight(row / slots, row, j);
                std::memcpy(&relay[(uint64_t(row) * k + j) * 2U + 1U], &weight, 4U);
            }
        }
    }
    uint8_t *LocalFor(uint32_t r, uint32_t w, uint64_t relative)
    {
        return peers[r].data() + w * half + relative;
    }
    uint16_t StageBits(uint32_t r, uint32_t slot, uint32_t col) const
    {
        return ties ? (bf16 ? 0x3f80U : 0x3c00U) : Ref::ExpertBits(bf16, r, uint64_t(slot) + 12345U, col);
    }
    float Weight(uint32_t row, uint32_t member) const
    {
        float value; std::memcpy(&value, &relay[(uint64_t(row) * k + member) * 2U + 1U], 4U); return value;
    }
    uint16_t Expected(uint32_t row, uint32_t col) const
    {
        volatile float sum = Ref::Decode(expert[uint64_t(row) * h + col], bf16) * Weight(row, 0U);
        const uint32_t source = row / slots, members = relay[uint64_t(row) * k * 2U];
        for (uint32_t j = 1; j <= members; ++j) {
            const uint32_t entry = relay[(uint64_t(row) * k + j) * 2U];
            const uint32_t expertId = entry & 0xffffU, q = entry >> 16U;
            const uint32_t r = expertId / 2U, localExpert = expertId % 2U;
            const uint32_t slot = (((source + r) % 16U) * 2U + localExpert) * bs + q;
            sum = Ref::Accumulate(sum, Ref::Decode(StageBits(r, slot, col), bf16), Weight(row, j));
        }
        return Ref::Encode(sum, bf16);
    }
    void Invoke(uint32_t core)
    {
        TileXREp::CombineMemoryServerDedupKernel kernel;
        kernel.Init(reinterpret_cast<GM_ADDR>(&args), reinterpret_cast<GM_ADDR>(expert.data()),
            reinterpret_cast<GM_ADDR>(table.data()), reinterpret_cast<GM_ADDR>(counts.data()), untouched.data(),
            reinterpret_cast<GM_ADDR>(types.data()), reinterpret_cast<GM_ADDR>(relay.data()), untouched.data(),
            untouched.data(), bs, h, k, 32U, bf16 ? TileXR::TILEXR_DATA_TYPE_BFP16 : TileXR::TILEXR_DATA_TYPE_FP16,
            0U, window, 0U);
        TileXREp::CombineModuleTestAccess::ThroughM4(kernel);
    }
};

void TestPartial(uint32_t count, uint32_t h, uint32_t k, bool bf16, bool sparse, uint32_t window, bool ties = false)
{
    PartialFixture f(count, h, k, window, bf16, sparse, ties);
    const auto savedExpert = f.expert;
    const auto beforeTable = f.table;
    const auto beforeRelay = f.relay;
    std::set<uint32_t> written;
    uint64_t selfReads = 0, stageReads = 0, sameCardReads = 0, tableReads = 0;
    bool overlapped = false;
    for (uint32_t core = 0; core < 64U; ++core) {
        f.Hooks(core, window);
        const auto controlRead = AscendC::GmReadObserver();
        std::set<uint32_t> coreWrites;
        uint64_t tableBaseRow = 0, tableSpan = 0;
        bool expectingTable3 = false;
        AscendC::GmReadObserver() = [&](const void *src, uint32_t bytes) {
            controlRead(src, bytes);
            const auto p = reinterpret_cast<uintptr_t>(src);
            const auto expert = reinterpret_cast<uintptr_t>(f.expert.data());
            const auto table = reinterpret_cast<uintptr_t>(f.table.data());
            const auto relay = reinterpret_cast<uintptr_t>(f.relay.data());
            if (count && p >= expert && p < expert + uint64_t(count) * h * 2U) {
                ++selfReads;
                Check(bytes == h * 2U && (p - expert) % (h * 2U) == 0U, "M4 self reads exactly H elements from original expert row");
                if (!AscendC::PendingMte3().empty()) overlapped = true;
            }
            if (count && p >= table && p < table + uint64_t(count) * 12U) {
                ++tableReads;
                tableBaseRow = (p - table) / 12U; tableSpan = bytes / 12U;
                Check(!expectingTable3 && bytes % 12U == 0 && bytes <= 32U * 12U && tableBaseRow + tableSpan <= count,
                    "M4 table2 bounded span has no extra GM rows");
                expectingTable3 = true;
            }
            if (count && p >= relay && p < relay + uint64_t(count) * k * 8U) {
                Check(expectingTable3 && p - relay == tableBaseRow * k * 8U && bytes == tableSpan * k * 8U,
                    "M4 table3 uses exactly the table2 baseRow and span");
                expectingTable3 = false;
            }
            for (uint32_t rank = 0; rank < 16U; ++rank) {
                const auto stage = reinterpret_cast<uintptr_t>(f.LocalFor(rank, window, f.stageOffset));
                if (p >= stage && p < stage + f.stageRows * f.stageStride) {
                    ++stageReads;
                    if (rank == f.rank) ++sameCardReads;
                    Check(bytes == h * 2U && (p - stage) % f.stageStride == 0U,
                        "all members including self-card use raw stage payload without alignment gaps");
                }
            }
        };
        AscendC::GmWriteObserver() = [&](const void *dst, const void *, uint32_t bytes) {
            bool matched = false;
            for (uint32_t rank = 0; rank < 16U; ++rank) {
                const auto begin = reinterpret_cast<uintptr_t>(f.LocalFor(rank, window, TILEXR_EP_SERVER_DEDUP_SHARED_DATA_OFFSET));
                const auto p = reinterpret_cast<uintptr_t>(dst);
                if (p < begin || p >= begin + f.slots * f.packet) continue;
                Check(bytes == f.packet && (p - begin) % f.packet == 0U, "partial send writes one complete result packet");
                const uint32_t row = rank * f.slots + uint32_t((p - begin) / f.packet);
                Check(written.insert(row).second && coreWrites.insert(row).second, "each type2 row produces one partial");
                matched = true;
            }
            Check(matched, "partial packet stays inside selected-window results");
        };
        f.Invoke(core);
        Check(!expectingTable3 && AscendC::PendingMte3().empty(), "table pair and all M4 sends complete before return");
        std::set<uint32_t> expected;
        if (core >= TileXREp::kEpServerDedupCombineSendCoreNum) {
            const uint32_t readers = TileXREp::kEpServerDedupCombineRecvCoreNum;
            const uint32_t local = core - TileXREp::kEpServerDedupCombineSendCoreNum;
            const uint32_t base = uint32_t(f.selected.size()) / readers, extra = uint32_t(f.selected.size()) % readers;
            const uint32_t start = local * base + std::min(local, extra);
            for (uint32_t i = 0; i < base + (local < extra ? 1U : 0U); ++i) expected.insert(f.selected[start + i]);
        }
        Check(coreWrites == expected, "type2 original rows divide densely across Read cores only");
        AscendC::GmWriteObserver() = {};
        f.ResetHooks();
    }
    uint64_t expectedMembers = 0;
    for (uint32_t row : f.selected) expectedMembers += f.relay[uint64_t(row) * k * 2U];
    Check(selfReads == f.selected.size() && stageReads == expectedMembers, "zero-weight entries still read self and every valid member");
    if (!f.selected.empty()) Check(sameCardReads >= f.selected.size(), "same-card members always use stage");
    if (f.selected.size() > TileXREp::kEpServerDedupCombineRecvCoreNum)
        Check(overlapped, "next token reads and aggregates while prior output MTE3 remains in flight");
    if (sparse) Check(tableReads > TileXREp::kEpServerDedupCombineRecvCoreNum, "sparse original-row gap splits both table caches");
    for (uint32_t rank = 0; rank < 16U; ++rank) for (uint32_t slot = 0; slot < f.slots; ++slot) {
        const uint32_t row = rank * f.slots + slot;
        const auto dst = f.LocalFor(rank, window, TILEXR_EP_SERVER_DEDUP_SHARED_DATA_OFFSET + slot * f.packet);
        const auto other = f.LocalFor(rank, 1U - window, TILEXR_EP_SERVER_DEDUP_SHARED_DATA_OFFSET + slot * f.packet);
        Check(std::all_of(other, other + f.packet, [](uint8_t v) { return v == 0xa5; }), "inactive result window is unchanged");
        if (!written.count(row)) {
            Check(std::all_of(dst, dst + f.packet, [](uint8_t v) { return v == 0xa5; }), "unwritten result slots are unchanged");
            continue;
        }
        bool correct = true;
        for (uint32_t col = 0; col < h; ++col) {
            const uint32_t byte = col * 2U;
            uint16_t actual;
            std::memcpy(&actual, dst + byte / 480U * 512U + byte % 480U, 2U);
            correct = correct && actual == f.Expected(row, col);
            if (ties) Check(actual == uint16_t((bf16 ? 0x3f80U : 0x3c00U) + (col % 2U ? 2U : 0U)),
                "both even and odd midpoint ties round to nearest even");
        }
        Check(correct, "self then ordered members use separate FP32 multiply/add and one final low-precision rounding");
        for (uint32_t block = 0; block < f.packet / 512U; ++block) {
            float flag; std::memcpy(&flag, dst + block * 512U + 480U, 4U);
            Check(flag == 1.0f, "each partial DataAsFlag block publishes ready");
        }
    }
    Check(f.expert == savedExpert && f.table == beforeTable && f.relay == beforeRelay, "M4 preserves expert and routing inputs");
    Check(std::all_of(f.untouched.begin(), f.untouched.end(), [](uint8_t v) { return v == 0x69; }), "M4 leaves yOut and other inputs unchanged");
}

void TestArenaBudget()
{
    for (uint32_t tokens : {0U, 10000U}) for (uint32_t k : {1U, 6U, 32U}) {
        AscendC::TPipe pipe;
        AscendC::TBuf<> arena;
        pipe.InitBuffer(arena, TileXREp::kEpServerDedupCombineRecvUbBytes);
        TileXR::CommArgs args {}; args.rankSize = 16;
        TileXREp::ServerDedupPartial partial;
        const auto layout = TileXREp::EpCombineM45Layout(tokens, 96U, 7168U, k);
        for (uint32_t lane = 0; lane < layout.outputLanes; ++lane)
            AscendC::SetFlag<AscendC::HardEvent::MTE3_V>(lane);
        partial.Init(&args, &pipe, arena.Get<uint8_t>()[TileXREp::kEpServerDedupCombineResidentUbBytes],
            nullptr, nullptr, nullptr, tokens, 0U, 96U, 7168U, k, 96U, TileXR::TILEXR_DATA_TYPE_BFP16, 0U, layout);
        partial.Process(); // touches every output lane's last flag, checking actual UB bounds
        for (uint32_t lane = 0; lane < layout.outputLanes; ++lane)
            AscendC::WaitFlag<AscendC::HardEvent::MTE3_V>(lane);
    }
}
void TestFullWidthHeader()
{
    PartialFixture f(1U, 17U, 2U, 0U, true, false);
    f.relay[0] = 0x10001U; // Low 16 bits look valid; full uint32 N is invalid.
    f.Hooks(TileXREp::kEpServerDedupCombineSendCoreNum, 0U);
    bool trapped = false;
    try { f.Invoke(TileXREp::kEpServerDedupCombineSendCoreNum); }
    catch (const std::runtime_error &error) { trapped = std::string(error.what()) == "AscendC device trap"; }
    Check(trapped, "relay header N is read as a full uint32 and checked before members");
    // A device Trap aborts the launch. Reset only the substitute's event table
    // so test teardown does not leak synthetic state into another Host case.
    AscendC::Events<AscendC::HardEvent::V_MTE2>().fill(false);
    AscendC::Events<AscendC::HardEvent::MTE3_V>().fill(false);
    f.ResetHooks();
}
int main()
{
    try {
        TestArenaBudget();
        TestPartial(0U, 17U, 2U, true, false, 0U);
        for (bool bf16 : {false, true}) {
            TestPartial(1U, 17U, 2U, bf16, false, 1U, true);
            TestPartial(257U, 257U, 6U, bf16, false, 0U);
            TestPartial(97U, 7168U, 6U, bf16, false, 1U);
            // More than six tasks per Read core wraps the larger output ring;
            // K=6 also forces rolling reuse of the three input lanes.
            TestPartial(7U * TileXREp::kEpServerDedupCombineRecvCoreNum + 1U,
                7168U, 6U, bf16, false, 0U);
        }
        TestPartial(10000U, 17U, 6U, true, true, 0U);
        // Dense tiny-H tasks overlap the old type staging and new phase rows;
        // relocation must preserve unread source indices (backward memmove).
        TestPartial(10000U, 1U, 2U, true, false, 0U);
        TestFullWidthHeader();
    } catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
    AscendC::GmWriteObserver() = {};
    if (ServerDedupTest::Failures() != 0) return 1;
    std::cout << "EP_SERVER_DEDUP_COMBINE_M4_PASS\n";
    return 0;
}
