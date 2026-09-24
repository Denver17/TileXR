#include "combine_m12_test_fixture.h"
#include "../integration/server_dedup_combine_m4_reference.h"
#include <set>

using ServerDedupTest::Check;
namespace Ref = CombineM4Reference;
using namespace TileXREp;

struct SourceFixture : CombineM12Test::Fixture {
    uint32_t bs, k, blocks;
    bool bf16, partial;
    uint64_t packet;
    std::vector<uint32_t> masks, relay;
    std::vector<float> weights;
    std::vector<uint16_t> output, expected;
    SourceFixture(uint32_t batch, uint32_t width, uint32_t topk, bool bfloat, bool withPartial)
        : Fixture(withPartial ? 2U : 0U, width), bs(batch), k(topk),
          blocks((width * 2U + 479U) / 480U), bf16(bfloat), partial(withPartial), packet(blocks * 512U)
    {
        tiling.bs = bs; tiling.topK = k; tiling.moeExpertNum = 32U;
        stageOffset = TILEXR_EP_SERVER_DEDUP_SHARED_DATA_OFFSET + uint64_t(bs) * k * packet;
        half = stageOffset + (partial ? uint64_t(8U * bs + 1U) * stageStride : 512U) + 512U;
        args.peerMemBytes = half * 2U;
        counts.assign(32U, int32_t(tokens));
        for (uint32_t r = 8; r < 16U; ++r) {
            peers[r].assign(half * 2U, 0xa5U); args.peerMems[r] = peers[r].data();
            for (uint32_t w = 0; w < 2U; ++w) std::memset(peers[r].data() + w * half +
                TILEXR_EP_SERVER_DEDUP_COMBINE_CONTROL_OFFSET, 0, TILEXR_EP_SERVER_DEDUP_COMBINE_CONTROL_BYTES);
        }
        masks.resize(bs); weights.resize(uint64_t(bs) * k);
        output.resize(uint64_t(bs) * h + 16U, 0x6b6bU); expected.resize(uint64_t(bs) * h);
        for (uint32_t p = 0; p < bs; ++p) {
            masks[p] = p % 3U == 0U ? (1U << (k - 1U)) : p % 3U == 1U ?
                (k == 32U ? 0xffffffffU : (1U << k) - 1U) : (1U | (1U << (k - 1U)));
            if (!kEpServerDedupEnable) masks[p] = k == 32U ? UINT32_MAX : (1U << k) - 1U;
            for (uint32_t j = 0; j < k; ++j) {
                const float choices[] = {0.0f, 0.1f, -1.25f, 0.375f, -0.3125f};
                weights[p * k + j] = choices[(p + j) % 5U];
            }
        }
        if (partial) {
            // M1 stages the same-card M4 member. M4 rounds its weighted partial
            // before M5 receives that slot and applies the source-side weight.
            types = {2, 3}; table = {int32_t(rank), 0, 0, int32_t(8U * bs), 0, 0};
            relay.assign(2U * k * 2U, 0U); relay[0] = 1U; relay[2] = rank * 2U;
            const float self = 1.0f, member = std::ldexp(1.0f, bf16 ? -8 : -11);
            std::memcpy(&relay[1], &self, 4U); std::memcpy(&relay[3], &member, 4U);
            for (uint32_t col = 0; col < h; ++col) {
                expert[col] = uint16_t((bf16 ? 0x3f80U : 0x3c00U) + col % 2U);
                expert[h + col] = bf16 ? 0x3f80U : 0x3c00U;
            }
            masks[0] = 3U; weights[0] = 1.0f; weights[1] = -1.0f;
        }
    }
    uint8_t *Slot(uint32_t w, uint32_t p, uint32_t j)
    {
        return peers[rank].data() + w * half + TILEXR_EP_SERVER_DEDUP_SHARED_DATA_OFFSET +
            (uint64_t(p) * k + j) * packet;
    }
    uint16_t Bits(uint32_t p, uint32_t j, uint32_t col) const
    {
        if (partial && p == 0U) {
            if (j == 0U) {
                volatile float add = Ref::Decode(expert[col], bf16) + std::ldexp(1.0f, bf16 ? -8 : -11);
                return Ref::Encode(add, bf16);
            }
            if (j == 1U) return bf16 ? 0x3f80U : 0x3c00U;
        }
        return Ref::ExpertBits(bf16, rank, uint64_t(p) * k + j, col);
    }
    void Seed(uint32_t w, bool freshWindow = true)
    {
        std::fill(output.begin(), output.end(), 0x6b6bU);
        for (uint32_t p = 0; p < bs; ++p) for (uint32_t j = 0; j < k; ++j) {
            auto slot = Slot(w, p, j);
            if (freshWindow) std::memset(slot, 0x93, packet);
            for (uint32_t col = 0; col < h; ++col) {
                const uint16_t bits = Bits(p, j, col);
                std::memcpy(slot + (col / 240U) * 512U + (col % 240U) * 2U, &bits, 2U);
            }
            for (uint32_t b = 0; b < blocks; ++b) {
                auto flag = slot + b * 512U + 480U;
                const bool active = (masks[p] >> j) & 1U;
                if (!freshWindow) Check(std::all_of(flag, flag + 32U,
                    [](uint8_t v) { return v == 0U; }), "reused window flags cleared by production M5");
                // On window reuse, publish active slots only. Inactive slots
                // rely entirely on the preceding production clear, not this fixture.
                if (freshWindow || active) {
                    const float value = active ? 1.0f : 0.0f;
                    std::memcpy(flag, &value, 4U);
                }
            }
        }
        for (uint32_t p = 0; p < bs; ++p) for (uint32_t col = 0; col < h; ++col) {
            float sum = 0.0f; bool first = true;
            for (uint32_t j = 0; j < k; ++j) if ((masks[p] >> j) & 1U) {
                volatile float product = Ref::Decode(Bits(p, j, col), bf16) * weights[p * k + j];
                if (first) { sum = product; first = false; }
                else { volatile float add = sum + product; sum = add; }
            }
            expected[uint64_t(p) * h + col] = Ref::Encode(sum, bf16);
        }
    }
    void Run(uint32_t core, uint32_t w, bool throughM4 = false)
    {
        CombineMemoryServerDedupKernel kernel;
        kernel.Init(reinterpret_cast<GM_ADDR>(&args), reinterpret_cast<GM_ADDR>(expert.data()),
            reinterpret_cast<GM_ADDR>(table.data()), reinterpret_cast<GM_ADDR>(counts.data()),
            reinterpret_cast<GM_ADDR>(weights.data()), kEpServerDedupEnable ? reinterpret_cast<GM_ADDR>(types.data()) : nullptr,
            kEpServerDedupEnable ? reinterpret_cast<GM_ADDR>(relay.data()) : nullptr,
            kEpServerDedupEnable ? reinterpret_cast<GM_ADDR>(masks.data()) : nullptr,
            reinterpret_cast<GM_ADDR>(output.data()), bs, h, k, 32U, bf16 ? TileXR::TILEXR_DATA_TYPE_BFP16 : TileXR::TILEXR_DATA_TYPE_FP16, 0U, w, 0U);
        if (throughM4) CombineModuleTestAccess::ThroughM4(kernel);
        else kernel.Process();
    }
};

void RunCase(uint32_t bs, uint32_t h, uint32_t k, bool bf16, bool partial)
{
    SourceFixture f(bs, h, k, bf16, partial);
    const auto weights = f.weights;
    for (uint32_t round = 0; round < 3U; ++round) {
        const uint32_t w = round % 2U;
        if (kEpServerDedupEnable && round == 2U && !partial) {
            // Reuse window zero with changed routing: previously active slots
            // become inactive, and a previous sparse token can gain a new slot.
            for (uint32_t p = 0; p < bs; ++p) f.masks[p] = 1U << (p % k);
        }
        const auto masks = f.masks;
        f.Seed(w, round < 2U);
        const auto inactive = f.peers[f.rank];
        // Schedule M1's local member before the Read producer. This Host model
        // has no concurrent cores; later hooks supply delayed result arrivals.
        if (partial) { f.Hooks(0U, w); f.Run(0U, w, true); f.ResetHooks(); }
        for (uint32_t core = 0U; core < kEpServerDedupCombineSendCoreNum; ++core) {
            f.Hooks(core, w); f.Run(core, w); f.ResetHooks();
        }
        Check(std::all_of(f.output.begin(), f.output.end(), [](uint16_t v) { return v == 0x6b6bU; }),
            "Send cores never aggregate source tokens or write yOut");
        for (uint32_t core = kEpServerDedupCombineSendCoreNum; core < 64U; ++core) {
            f.Hooks(core, w);
            const uint32_t local = core - kEpServerDedupCombineSendCoreNum;
            const uint32_t base = bs / kEpServerDedupCombineRecvCoreNum, extra = bs % kEpServerDedupCombineRecvCoreNum;
            const uint32_t begin = local * base + std::min(local, extra), count = base + (local < extra);
            std::vector<uint32_t> probes, writes;
            uint32_t firstAttempts = 0U, clears = 0U, payloadReads = 0U, reductions = 0U, flagCopies = 0U;
            auto priorRead = AscendC::GmReadObserver();
            auto priorWrite = AscendC::PaddedWriteObserver();
            const uintptr_t result = reinterpret_cast<uintptr_t>(f.Slot(w, 0U, 0U));
            AscendC::GmReadObserver() = [&](const void *src, uint32_t bytes) {
                priorRead(src, bytes);
                const auto address = reinterpret_cast<uintptr_t>(src);
                if (address < result || address >= result + uint64_t(bs) * k * f.packet) return;
                const uint64_t relative = address - result;
                const uint32_t p = uint32_t(relative / (k * f.packet));
                const uint32_t j = uint32_t(relative / f.packet % k);
                if (relative % f.packet == 480U && bytes == k * f.blocks * 32U) {
                    ++flagCopies;
                    Check(j == 0U, "full TopK probe starts at slot zero even for a sparse mask");
                    probes.push_back(p);
                    if (p == begin && count > 1U) {
                        const float ready = ++firstAttempts > 1U ? 1.0f : 0.0f;
                        // Only the last block of the highest active slot is late;
                        // every other required flag is ready, including zero-weight slots.
                        uint32_t lastK = k - 1U;
                        while (((f.masks[p] >> lastK) & 1U) == 0U) --lastK;
                        std::memcpy(f.Slot(w, p, lastK) + (f.blocks - 1U) * 512U + 480U, &ready, 4U);
                    }
                    Check(probes.size() < count * 3U + 3U, "pending scan makes bounded progress");
                } else if (relative % f.packet == 0U && bytes == f.blocks * 480U) {
                    ++payloadReads;
                    Check(((f.masks[p] >> j) & 1U) != 0U, "payload read only for effective mask bits");
                }
            };
            AscendC::ReduceSumObserver() = [&](uint32_t mask, uint32_t repeats, uint32_t stride) {
                if (probes.empty()) return; // preceding M2 reductions use their own buffers
                Check(mask == 1U && stride == 1U && repeats == k * f.blocks,
                    "one reduction consumes full TopK flag blocks using only their leading float");
                ++reductions;
            };
            AscendC::PaddedWriteObserver() = [&](const void *dst, const void *src, uint32_t bytes) {
                priorWrite(dst, src, bytes);
                const auto address = reinterpret_cast<uintptr_t>(dst);
                if (address >= result && address < result + uint64_t(bs) * k * f.packet) {
                    Check((address - result) % 512U == 480U && bytes == 32U, "M5 writes only flag bytes in result arena");
                    const auto zero = static_cast<const uint8_t *>(src);
                    Check(std::all_of(zero, zero + bytes, [](uint8_t v) { return v == 0; }), "all 32 flag bytes cleared");
                    ++clears;
                }
                const uintptr_t out = reinterpret_cast<uintptr_t>(f.output.data());
                if (address >= out && address < out + uint64_t(bs) * h * 2U) {
                    Check(bytes == h * 2U, "one contiguous valid-H yOut write");
                    writes.push_back(uint32_t((address - out) / (h * 2U)));
                }
            };
            f.Run(core, w);
            Check(kEpServerDedupEnable ? f.readyProbes != 0U : f.readyProbes == 0U, "Recv cores execute M2 before M4/M5");
            Check(AscendC::PendingMte3().empty(), "full Process M6 drains flag clears and yOut");
            Check(writes.size() == count && std::set<uint32_t>(writes.begin(), writes.end()).size() == count,
                "pending swap deletes each true TokenId once");
            if (count > 1U) Check(writes.front() == begin + 1U && writes.back() == begin,
                "unready first token does not block later ready tokens");
            Check(reductions == probes.size(), "exactly one ReduceSum per nonempty probe");
            Check(flagCopies == probes.size(), "one full TopK multiblock flag copy per probe");
            AscendC::ReduceSumObserver() = {};
            Check(clears == count * k * f.blocks, "cleanup covers full K, including inactive slots and nonleading poison bytes");
            uint32_t expectedReads = 0U;
            for (uint32_t p = begin; p < begin + count; ++p) for (uint32_t j = 0; j < k; ++j)
                expectedReads += (f.masks[p] >> j) & 1U;
            Check(payloadReads == expectedReads, "one payload copy per effective k including zero weights");
            f.ResetHooks();
        }
        Check(std::equal(f.expected.begin(), f.expected.end(), f.output.begin()), "ascending-k separate FP32 arithmetic and RNE yOut");
        Check(std::all_of(f.output.end() - 16U, f.output.end(), [](uint16_t v) { return v == 0x6b6bU; }), "yOut tail guard");
        for (uint32_t p = 0; p < bs; ++p) for (uint32_t j = 0; j < k; ++j) {
            const auto slot = f.Slot(w, p, j);
            for (uint32_t b = 0; b < f.blocks; ++b) {
                const uint64_t offset = w * f.half + TILEXR_EP_SERVER_DEDUP_SHARED_DATA_OFFSET +
                    (uint64_t(p) * k + j) * f.packet + b * 512U;
                // M4's active partial may update payload padding; valid values
                // remain independently checked through yOut and Bits.
                if (!(partial && p == 0U && j == 0U))
                    Check(std::memcmp(slot + b * 512U, inactive.data() + offset, 480U) == 0, "flag clear preserves all payload bytes");
                Check(std::all_of(slot + b * 512U + 480U, slot + (b + 1U) * 512U,
                    [](uint8_t v) { return v == 0; }), "all source result flags are zero after M6");
            }
        }
        Check(std::memcmp(f.peers[f.rank].data() + (1U - w) * f.half,
            inactive.data() + (1U - w) * f.half, f.half) == 0, "alternate window untouched");
        Check(f.masks == masks && f.weights == weights, "source metadata read only");
        if (partial) {
            const float unrounded = std::ldexp(1.0f, bf16 ? -8 : -11);
            Check(f.expected[0] != Ref::Encode(unrounded, bf16), "M4 rounding precedes M5, unlike single global FP32 sum");
        }
    }
}

void TestDirectThenFinal(bool bf16)
{
    SourceFixture f(1U, 257U, 6U, bf16, true);
    // Send core returns DIRECT; a separate Recv core owns the final token.
    f.partial = false; f.types[0] = 1;
    f.masks[0] = kEpServerDedupEnable ? 3U : 63U;
    if (!kEpServerDedupEnable) { f.tokens = 1U; f.counts.assign(32U, 1); }
    f.weights[0] = 0.375f; f.weights[1] = -0.125f;
    for (uint32_t col = 0; col < f.h; ++col) f.expert[col] = f.Bits(0U, 0U, col);
    for (uint32_t round = 0; round < 3U; ++round) {
        const uint32_t w = round % 2U;
        f.Seed(w);
        std::memset(f.Slot(w, 0U, 0U), 0, f.packet); // Only M3 may supply this slot.
        f.Hooks(0U, w);
        auto previousRead = AscendC::GmReadObserver();
        uint32_t probes = 0U;
        AscendC::GmReadObserver() = [&](const void *src, uint32_t bytes) {
            previousRead(src, bytes);
            if (src == f.Slot(w, 0U, 0U) + 480U) {
                Check(++probes == 1U, "M3 DIRECT has retired before the first M5 probe");
                if (probes > 1U) throw std::runtime_error("DIRECT missing at M5 entry");
            }
        };
        f.Run(0U, w);
        f.ResetHooks();
        f.Hooks(kEpServerDedupCombineSendCoreNum, w);
        auto controlRead = AscendC::GmReadObserver();
        AscendC::GmReadObserver() = [&](const void *src, uint32_t bytes) {
            controlRead(src, bytes);
            if (src == f.Slot(w, 0U, 0U) + 480U) ++probes;
        };
        f.Run(kEpServerDedupCombineSendCoreNum, w);
        Check(probes == 1U && std::equal(f.expected.begin(), f.expected.end(), f.output.begin()),
            "nonempty M3 followed by M5 preserves DIRECT payload and produces yOut");
        Check(AscendC::PendingMte3().empty(), "Send core drains output and flag clear");
        f.ResetHooks();
    }
}

// Deferred copies retire up to the waited event's publication position.
// This checks ownership and issue order, not physical hardware overlap.
void TestPartialTailOverlap(bool bf16)
{
    SourceFixture f(2U * kEpServerDedupCombineRecvCoreNum, 257U, 6U, bf16, true);
    f.Seed(0U);
    f.Hooks(0U, 0U); f.Run(0U, 0U, true); f.ResetHooks(); // M1 stage producer
    std::memset(f.Slot(0U, 0U, 0U), 0, f.packet); // real M4 must publish this partial
    f.Hooks(kEpServerDedupCombineSendCoreNum, 0U);
    const auto controlRead = AscendC::GmReadObserver();
    bool probedPending = false, loadedPending = false, computedPending = false, partialWritten = false;
    AscendC::GmReadObserver() = [&](const void *src, uint32_t bytes) {
        controlRead(src, bytes);
        if (src == f.Slot(0U, 0U, 0U) + 480U && !partialWritten) {
            probedPending = !AscendC::PendingMte3().empty();
            // Model eventual DMA progress after M5 has processed the ready
            // token. Reusing an unused output seed no longer flushes M4.
            if (computedPending) AscendC::CompleteMte3();
        }
        if (src == f.Slot(0U, 1U, 0U) && bytes == f.blocks * 480U)
            loadedPending = !AscendC::PendingMte3().empty();
    };
    AscendC::MulsObserver() = [&]() {
        if (loadedPending && !partialWritten && !AscendC::PendingMte3().empty()) computedPending = true;
    };
    AscendC::GmWriteObserver() = [&](const void *dst, const void *src, uint32_t bytes) {
        if (dst != f.Slot(0U, 0U, 0U)) return;
        Check(probedPending && loadedPending && computedPending,
            "M5 probes, reads and computes before M4 tail MTE3 completes");
        Check(bytes == f.packet, "retained M4 output has original packet size");
        const auto packet = static_cast<const uint8_t *>(src);
        for (uint32_t col = 0; col < f.h; ++col) {
            uint16_t actual; std::memcpy(&actual, packet + col / 240U * 512U + col % 240U * 2U, 2U);
            Check(actual == f.Bits(0U, 0U, col), "M5 metadata/compute did not overwrite in-flight partial");
        }
        partialWritten = true;
    };
    f.Run(kEpServerDedupCombineSendCoreNum, 0U);
    AscendC::GmWriteObserver() = {}; AscendC::MulsObserver() = {}; f.ResetHooks();
    Check(partialWritten && computedPending, "nonempty M4 and M5 asynchronous handoff exercised");
    Check(std::equal(f.expected.begin(), f.expected.begin() + 2U * f.h, f.output.begin()),
        "ready token then delayed partial produce correct yOut");
    Check(AscendC::PendingMte3().empty(), "all leftover M4 slots and M5 clears drained");
}

void TestEightPartialSlots(bool bf16)
{
    // One source token on this Recv core; last-token flag clear occurs while
    // all eight M4 output slots (including V_MTE3 event 7) are still in flight.
    SourceFixture f(kEpServerDedupCombineRecvCoreNum, 257U, 6U, bf16, true);
    f.partial = false;
    f.tokens = 8U * kEpServerDedupCombineRecvCoreNum;
    f.types.assign(f.tokens, 2); f.counts.assign(32U, int32_t(f.tokens));
    f.expert.resize(uint64_t(f.tokens) * f.h);
    f.table.resize(f.tokens * 3U); f.relay.assign(f.tokens * f.k * 2U, 0U);
    f.peers[6].assign(f.half * 2U, 0xa5U); f.args.peerMems[6] = f.peers[6].data();
    const float one = 1.0f;
    for (uint32_t row = 0; row < f.tokens; ++row) {
        const uint32_t ordinal = row / kEpServerDedupCombineRecvCoreNum;
        // Three local-server sends, then five remote-server sends per core.
        f.table[row * 3U] = ordinal < 5U ? 6 : 8;
        f.table[row * 3U + 1U] = ordinal; f.table[row * 3U + 2U] = 0;
        f.relay[row * f.k * 2U] = 1; f.relay[row * f.k * 2U + 2U] = f.rank * 2U;
        std::memcpy(&f.relay[row * f.k * 2U + 1U], &one, 4U);
        for (uint32_t col = 0; col < f.h; ++col) f.expert[uint64_t(row) * f.h + col] = Ref::ExpertBits(bf16, 8, row, col);
    }
    auto stage = f.peers[f.rank].data() + f.stageOffset + uint64_t(4U * f.bs) * f.stageStride;
    std::memset(stage, 0, f.h * 2U); // zero-weight member, still read from a valid slot
    std::memset(f.peers[f.rank].data() + f.stageOffset, 0, f.h * 2U);
    f.Seed(0U);
    f.Hooks(kEpServerDedupCombineSendCoreNum, 0U);
    const auto controlRead = AscendC::GmReadObserver();
    bool loaded = false, computed = false, clearPublished = false, inheritedWait = false;
    uint32_t retired = 0U;
    AscendC::GmReadObserver() = [&](const void *src, uint32_t bytes) {
        controlRead(src, bytes);
        if (src == f.Slot(0U, 0U, 0U) && bytes == f.blocks * 480U)
            loaded = !AscendC::PendingMte3().empty();
    };
    AscendC::MulsObserver() = [&]() {
        if (loaded && retired == 0U) computed = !AscendC::PendingMte3().empty();
    };
    AscendC::EventObserver() = [&](AscendC::HardEvent event, AscendC::TEventID id, bool set) {
        if (event == AscendC::HardEvent::S_MTE3 && id == 7U && set) {
            Check(retired == 0U && AscendC::PendingMte3().size() == 8U,
                "clear publishes in a distinct event direction while all eight partials remain pending");
            clearPublished = true;
        }
        if (clearPublished && event == AscendC::HardEvent::MTE3_V && !set && !inheritedWait) {
            Check(id == 6U, "M5 keeps the latest two of eight sends, not the final server's subtotal");
            inheritedWait = true;
        }
    };
    AscendC::GmWriteObserver() = [&](const void *dst, const void *src, uint32_t bytes) {
        uint64_t offset = UINT64_MAX;
        for (uint32_t rank : {6U, 8U}) {
            const auto target = reinterpret_cast<uintptr_t>(f.peers[rank].data()) + TILEXR_EP_SERVER_DEDUP_SHARED_DATA_OFFSET;
            const auto address = reinterpret_cast<uintptr_t>(dst);
            if (address >= target && address < target + uint64_t(f.bs) * f.k * f.packet) offset = address - target;
        }
        Check(offset < uint64_t(8U) * f.k * f.packet && bytes == f.packet, "M4 writes one of eight original destinations");
        Check(loaded && computed && clearPublished, "M5 clear, payload and compute precede M4 tail retirement");
        const uint32_t row = uint32_t(offset / (f.k * f.packet)) * kEpServerDedupCombineRecvCoreNum;
        const auto packet = static_cast<const uint8_t *>(src);
        for (uint32_t col = 0; col < f.h; ++col) {
            uint16_t actual; std::memcpy(&actual, packet + col / 240U * 512U + col % 240U * 2U, 2U);
            Check(actual == f.expert[uint64_t(row) * f.h + col], "all eight retained M4 source slots preserve payload");
        }
        ++retired;
    };
    f.Run(kEpServerDedupCombineSendCoreNum, 0U);
    AscendC::GmWriteObserver() = {}; AscendC::MulsObserver() = {}; AscendC::EventObserver() = {}; f.ResetHooks();
    Check(retired == 8U && inheritedWait && std::equal(f.expected.begin(), f.expected.begin() + f.h, f.output.begin()),
        "O8/O5=2 handoff produces yOut and retires retained slot7 exactly once");
    Check(AscendC::PendingMte3().empty(), "no inherited packet or clear remains after M6");
}

void TestPartialWithoutSource(bool bf16)
{
    SourceFixture f(1U, 17U, 6U, bf16, true);
    f.types = {2, 2};
    std::copy(f.table.begin(), f.table.begin() + 3, f.table.begin() + 3);
    std::copy(f.relay.begin(), f.relay.begin() + 12, f.relay.begin() + 12);
    f.Seed(0U);
    // Both partials address a valid staged member; only Recv local 1 runs here.
    auto stage = f.peers[f.rank].data() + f.stageOffset + 8U * f.stageStride;
    std::memcpy(stage, f.expert.data() + f.h, f.h * 2U);
    f.Hooks(kEpServerDedupCombineSendCoreNum + 1U, 0U);
    uint32_t writes = 0;
    AscendC::GmWriteObserver() = [&](const void *dst, const void *, uint32_t) {
        if (dst == f.Slot(0U, 0U, 0U)) ++writes;
    };
    f.Run(kEpServerDedupCombineSendCoreNum + 1U, 0U);
    AscendC::GmWriteObserver() = {}; f.ResetHooks();
    Check(writes == 1 && AscendC::PendingMte3().empty(), "empty M5 still retires nonempty M4 and all seeded outputs");
    Check(std::all_of(f.output.begin(), f.output.end(), [](uint16_t x) { return x == 0x6b6b; }),
        "zero-owned source role never writes yOut");
}

// Wide inputs cross old output boundaries; deferred M4 copies inspect their UB
// sources at retirement, catching premature overwrite rather than just layout errors.
void TestInputReclaimsPartialSlots(bool bf16, uint32_t tasksPerCore, uint32_t topk = 6U)
{
    SourceFixture f(std::max(kEpServerDedupCombineRecvCoreNum, tasksPerCore), 7168U, topk, bf16, false);
    f.tokens = tasksPerCore * kEpServerDedupCombineRecvCoreNum;
    f.types.assign(f.tokens, 2); f.counts.assign(32U, int32_t(f.tokens));
    f.expert.resize(uint64_t(f.tokens) * f.h);
    f.table.resize(f.tokens * 3U); f.relay.assign(f.tokens * f.k * 2U, 0U);
    f.half = f.stageOffset + f.stageStride + 512U; f.args.peerMemBytes = 2U * f.half;
    for (uint32_t r = 8; r < 16; ++r) {
        f.peers[r].resize(2U * f.half, 0U); f.args.peerMems[r] = f.peers[r].data();
    }
    f.peers[6].resize(2U * f.half, 0U); f.args.peerMems[6] = f.peers[6].data();
    std::memset(f.peers[f.rank].data() + f.stageOffset, 0, f.h * 2U);
    const float one = 1.0f;
    for (uint32_t row = 0; row < f.tokens; ++row) {
        f.table[row * 3U] = 6; f.table[row * 3U + 1U] = row / kEpServerDedupCombineRecvCoreNum;
        f.table[row * 3U + 2U] = 0;
        f.relay[row * f.k * 2U] = 1; f.relay[row * f.k * 2U + 2U] = f.rank * 2U;
        std::memcpy(&f.relay[row * f.k * 2U + 1U], &one, 4U);
        for (uint32_t col = 0; col < f.h; ++col)
            f.expert[uint64_t(row) * f.h + col] = Ref::ExpertBits(bf16, 8U, row, col);
    }
    f.masks[0] = topk == 32U ? 0xffffffffU : (1U << topk) - 1U; f.Seed(0U);
    f.Hooks(kEpServerDedupCombineSendCoreNum, 0U);
    const auto controlRead = AscendC::GmReadObserver();
    uint32_t retired = 0U, payloadReads = 0U, reclaimWaits = 0U;
    bool m5Started = false, bridgePending = false;
    AscendC::GmReadObserver() = [&](const void *src, uint32_t bytes) {
        controlRead(src, bytes);
        if (src == f.Slot(0U, 0U, 0U) + 480U) m5Started = true;
        if (bytes != f.blocks * 480U) return;
        for (uint32_t k = 0; k < f.k; ++k) if (src == f.Slot(0U, 0U, k)) {
            ++payloadReads;
            Check(!bridgePending, "MTE3-to-Vector reclaim is bridged to MTE2 before overwrite");
            Check(retired <= tasksPerCore - 2U, "new input reads do not drain retained M4 tail sends");
        }
    };
    AscendC::EventObserver() = [&](AscendC::HardEvent event, AscendC::TEventID, bool set) {
        if (m5Started && payloadReads < f.k && event == AscendC::HardEvent::MTE3_V && !set) {
            bridgePending = true; ++reclaimWaits;
        }
        if (event == AscendC::HardEvent::V_MTE2 && !set) bridgePending = false;
    };
    AscendC::GmWriteObserver() = [&](const void *dst, const void *src, uint32_t bytes) {
        const uintptr_t base = reinterpret_cast<uintptr_t>(f.peers[6].data()) + TILEXR_EP_SERVER_DEDUP_SHARED_DATA_OFFSET;
        const uintptr_t address = reinterpret_cast<uintptr_t>(dst);
        Check(address >= base && address < base + uint64_t(tasksPerCore) * f.k * f.packet,
            "partial uses original remote destination");
        const uint32_t row = uint32_t((address - base) / (f.k * f.packet)) * kEpServerDedupCombineRecvCoreNum;
        Check(bytes == f.packet, "partial retains its original wire stride");
        const auto packet = static_cast<const uint8_t *>(src);
        for (uint32_t col = 0; col < f.h; ++col) {
            uint16_t actual;
            std::memcpy(&actual, packet + (col / 240U) * 512U + (col % 240U) * 2U, 2U);
            Check(actual == f.expert[uint64_t(row) * f.h + col], "input reclamation preserves pending M4 payload");
        }
        ++retired;
    };
    f.Run(kEpServerDedupCombineSendCoreNum, 0U);
    AscendC::GmWriteObserver() = {}; AscendC::EventObserver() = {}; f.ResetHooks();
    Check(payloadReads == topk && reclaimWaits != 0U, "payload inputs exercise inherited output reclamation and lane reuse");
    Check(retired == tasksPerCore && AscendC::PendingMte3().empty(), "every M4 send retires once");
    Check(std::equal(f.expected.begin(), f.expected.begin() + f.h, f.output.begin()), "reclaimed inputs produce exact yOut");
}

void TestBudget()
{
    for (uint32_t k : {1U, 6U, 32U}) {
        const auto layout = EpCombineM45Layout(10000U, 96U, 7168U, k);
        Check(layout.inputLanes != 0U && layout.totalBytes <= kEpServerDedupCombineRecvUbBytes,
            "simultaneous shared M4/M5 resources fit Recv arena");
    }
    Check(EpCombineM45Layout(0U, UINT32_MAX, 7168U, 32U).inputLanes == 0U, "metadata cannot wrap");
    Check(EpCombineM45Layout(0U, 1U, 0U, 1U).inputLanes == 0U, "zero H rejected");
}

int main()
{
    try {
        TestBudget();
        for (bool bf16 : {true, false}) {
            TestDirectThenFinal(bf16);
            if (kEpServerDedupEnable) {
            TestPartialTailOverlap(bf16);
            TestEightPartialSlots(bf16);
            TestInputReclaimsPartialSlots(bf16, 6U);
            TestInputReclaimsPartialSlots(bf16, 7U); // retained outputs wrap the physical ring
            TestInputReclaimsPartialSlots(bf16, 11U);
            TestInputReclaimsPartialSlots(bf16, 11U, 32U);
            TestPartialWithoutSource(bf16);
            }
            RunCase(1U, 17U, 1U, bf16, false); // zero-owned Recv roles, single-bit mask
            RunCase(3U * kEpServerDedupCombineRecvCoreNum + 1U, 257U, 6U, bf16, kEpServerDedupEnable);
            RunCase(3U * kEpServerDedupCombineRecvCoreNum + 1U, 257U, 32U, bf16, false);
            RunCase(9U * kEpServerDedupCombineRecvCoreNum + 1U, 17U, 6U, bf16, false); // wrap inherited output slots
            RunCase(1U, 7168U, 32U, bf16, false); // >255 total flag repeats and wide tails
        }
    } catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
    if (ServerDedupTest::Failures()) return 1;
    std::cout << "EP_SERVER_DEDUP_COMBINE_M5_PASS\n";
}
