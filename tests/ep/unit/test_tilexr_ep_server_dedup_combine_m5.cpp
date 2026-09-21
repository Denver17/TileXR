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
    void Seed(uint32_t w)
    {
        std::fill(output.begin(), output.end(), 0x6b6bU);
        for (uint32_t p = 0; p < bs; ++p) for (uint32_t j = 0; j < k; ++j) {
            auto slot = Slot(w, p, j); std::memset(slot, 0x93, packet);
            for (uint32_t col = 0; col < h; ++col) {
                const uint16_t bits = Bits(p, j, col);
                std::memcpy(slot + (col / 240U) * 512U + (col % 240U) * 2U, &bits, 2U);
            }
            for (uint32_t b = 0; b < blocks; ++b) {
                const float value = (masks[p] >> j) & 1U ? 1.0f : 7.0f;
                std::memcpy(slot + b * 512U + 480U, &value, 4U);
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
            reinterpret_cast<GM_ADDR>(weights.data()), reinterpret_cast<GM_ADDR>(types.data()),
            reinterpret_cast<GM_ADDR>(relay.data()), reinterpret_cast<GM_ADDR>(masks.data()),
            reinterpret_cast<GM_ADDR>(output.data()), bs, h, k, 32U, bf16 ? TileXR::TILEXR_DATA_TYPE_BFP16 : TileXR::TILEXR_DATA_TYPE_FP16, 0U, w, 0U);
        if (throughM4) CombineModuleTestAccess::ThroughM4(kernel);
        else kernel.Process();
    }
};

void RunCase(uint32_t bs, uint32_t h, uint32_t k, bool bf16, bool partial)
{
    SourceFixture f(bs, h, k, bf16, partial);
    const auto masks = f.masks; const auto weights = f.weights;
    for (uint32_t round = 0; round < 3U; ++round) {
        const uint32_t w = round % 2U;
        f.Seed(w);
        const auto inactive = f.peers[f.rank];
        // Schedule M1's local member before the Read producer. This Host model
        // has no concurrent cores; later hooks supply delayed result arrivals.
        if (partial) { f.Hooks(0U, w); f.Run(0U, w, true); f.ResetHooks(); }
        for (uint32_t core = kEpServerDedupCombineSendCoreNum; core < 64U; ++core) {
            f.Hooks(core, w); f.Run(core, w); f.ResetHooks();
        }
        Check(std::all_of(f.output.begin(), f.output.end(), [](uint16_t v) { return v == 0x6b6bU; }),
            "Read cores never aggregate source tokens or write yOut");
        for (uint32_t core = 0; core < kEpServerDedupCombineSendCoreNum; ++core) {
            f.Hooks(core, w);
            const uint32_t base = bs / kEpServerDedupCombineSendCoreNum, extra = bs % kEpServerDedupCombineSendCoreNum;
            const uint32_t begin = core * base + std::min(core, extra), count = base + (core < extra);
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
                if (relative % f.packet == 480U && bytes == f.blocks * 32U) {
                    ++flagCopies;
                    uint32_t firstK = 0U; while (((f.masks[p] >> firstK) & 1U) == 0U) ++firstK;
                    if (j == firstK) {
                        probes.push_back(p);
                        if (p == begin && count > 1U) {
                            const float ready = ++firstAttempts > 1U ? 1.0f : 0.0f;
                            std::memcpy(f.Slot(w, p, j) + 480U, &ready, 4U);
                        }
                    }
                    Check(probes.size() < count * 3U + 3U, "pending scan makes bounded progress");
                } else if (relative % f.packet == 0U && bytes == f.blocks * 480U) {
                    ++payloadReads;
                    Check(((f.masks[p] >> j) & 1U) != 0U, "payload read only for effective mask bits");
                }
            };
            AscendC::ReduceSumObserver() = [&](uint32_t mask, uint32_t repeats, uint32_t stride) {
                if (probes.empty()) return; // preceding M2 reductions use their own buffers
                uint32_t expectedFlags = 0U;
                for (uint32_t j = 0; j < k; ++j) expectedFlags += ((f.masks[probes.back()] >> j) & 1U) * f.blocks;
                Check(mask == 1U && stride == 1U && repeats == expectedFlags,
                    "one reduction consumes all effective 32B flag blocks using only their leading float");
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
            Check(f.prepareProbes == 0U && f.readyProbes == 0U, "M5 Send cores do not wait on M2");
            Check(AscendC::PendingMte3().empty(), "full Process M6 drains flag clears and yOut");
            Check(writes.size() == count && std::set<uint32_t>(writes.begin(), writes.end()).size() == count,
                "pending swap deletes each true TokenId once");
            if (count > 1U) Check(writes.front() == begin + 1U && writes.back() == begin,
                "unready first token does not block later ready tokens");
            Check(reductions == probes.size(), "exactly one ReduceSum per nonempty probe");
            uint32_t expectedFlagCopies = 0U;
            for (uint32_t p : probes) for (uint32_t j = 0; j < k; ++j) expectedFlagCopies += (f.masks[p] >> j) & 1U;
            Check(flagCopies == expectedFlagCopies, "one multiblock flag copy per effective k");
            AscendC::ReduceSumObserver() = {};
            Check(clears == count * k * f.blocks, "cleanup covers full K, including inactive poison flags");
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
    // Reuse the two-row backing for one DIRECT and one stage row. Send core 0
    // must return its real DIRECT before repurposing the scratch for M5.
    f.partial = false; f.types[0] = 1;
    f.masks[0] = 3U; f.weights[0] = 0.375f; f.weights[1] = -0.125f;
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
        Check(probes == 1U && std::equal(f.expected.begin(), f.expected.end(), f.output.begin()),
            "nonempty M3 followed by M5 preserves DIRECT payload and produces yOut");
        Check(AscendC::PendingMte3().empty(), "Send core drains output and flag clear");
        f.ResetHooks();
    }
}

void TestBudget()
{
    for (uint32_t k : {1U, 6U, 32U}) {
        Check(EpCombineM5InputLaneCount(96U, 7168U, k) != 0U, "main H7168 shape fits for K1/K6/K32");
        const uint64_t used = EpCombineM5FixedUbBytes(96U, 7168U, k) +
            EpCombineM5InputLaneCount(96U, 7168U, k) * ((7168U * 2U + 479U) / 480U * 480U) +
            EpCombineM5OutputLaneCount(96U, 7168U, k) * EpCombineAlign(7168U * 2U, 32U);
        Check(used <= kEpServerDedupCombineUbBytes, "M5 simultaneous resource budget");
    }
    Check(EpCombineM5InputLaneCount(UINT32_MAX, 7168U, 32U) == 0U, "large BS cannot wrap metadata budget");
    Check(EpCombineM5InputLaneCount(1U, 0U, 1U) == 0U, "zero H rejected");
}

int main()
{
    try {
        TestBudget();
        for (bool bf16 : {true, false}) {
            TestDirectThenFinal(bf16);
            RunCase(1U, 17U, 1U, bf16, false); // zero-owned Send roles, single-bit mask
            RunCase(3U * kEpServerDedupCombineSendCoreNum + 1U, 257U, 6U, bf16, true);
            RunCase(3U * kEpServerDedupCombineSendCoreNum + 1U, 257U, 32U, bf16, false);
            RunCase(1U, 7168U, 32U, bf16, false); // >255 total flag repeats and wide tails
        }
    } catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
    if (ServerDedupTest::Failures()) return 1;
    std::cout << "EP_SERVER_DEDUP_COMBINE_M5_PASS\n";
}
