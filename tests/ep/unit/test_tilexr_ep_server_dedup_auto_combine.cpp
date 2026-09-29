#include "combine_m12_test_fixture.h"
#include "../integration/server_dedup_combine_m4_reference.h"
#include <map>

using namespace TileXREp;
using ServerDedupTest::Check;
namespace Ref = CombineM4Reference;

// Reuse the same communicator, peer windows, and metadata allocations while BS
// crosses the threshold. The CPU event model validates ownership/drain ordering;
// it does not model physical device concurrency or timing.
void CheckModeTransitions(uint32_t h, uint32_t k, bool bf16)
{
    constexpr uint32_t maxBs = 33U;
    CombineM12Test::Fixture f(maxBs * k, h);
    const uint32_t blocks = (h * 2U + 479U) / 480U;
    const uint64_t packet = blocks * 512U;
    f.stageOffset = TILEXR_EP_SERVER_DEDUP_SHARED_DATA_OFFSET + uint64_t(maxBs) * k * packet;
    f.half = f.stageOffset + 512U;
    f.args.peerMemBytes = 2U * f.half;
    for (uint32_t r = 8U; r < 16U; ++r) {
        f.peers[r].assign(2U * f.half, 0xa5U);
        f.args.peerMems[r] = f.peers[r].data();
        for (uint32_t bank = 0; bank < 2U; ++bank)
            std::memset(f.Local(bank, 0), 0, TILEXR_EP_SERVER_DEDUP_COMBINE_CONTROL_BYTES);
    }
    std::vector<float> weights(maxBs * k);
    std::vector<uint32_t> masks(maxBs), relay(maxBs * k * k * 2U, 0xdeadbeefU);
    std::vector<uint16_t> output(uint64_t(maxBs) * h + 16U), expected;
    uint32_t round = 0;
    for (uint32_t bs : {1U, 31U, 32U, 33U, 32U}) {
        const bool dedup = bs > 32U; // Independent policy oracle.
        const uint32_t bank = round++ % 2U;
        f.tokens = bs * k;
        f.counts.assign(32U, int32_t(f.tokens));
        std::fill(f.types.begin(), f.types.end(), dedup ? 1 : 3);
        std::fill(masks.begin(), masks.end(), dedup ? (k == 32U ? UINT32_MAX : (1U << k) - 1U) : 0U);
        std::fill(output.begin(), output.end(), 0x6b6bU);
        expected.assign(uint64_t(bs) * h, 0);
        // Poison unused relay/type/mask fields deliberately. Direct mode must
        // neither consume them nor mistake terminal markers for M1 work.
        for (uint32_t p = 0; p < bs; ++p) {
            for (uint32_t j = 0; j < k; ++j) {
                const uint32_t row = p * k + j;
                f.table[row * 3U] = int32_t(f.rank);
                f.table[row * 3U + 1U] = int32_t(p);
                f.table[row * 3U + 2U] = int32_t(j);
                weights[row] = (j % 3U == 0U ? -0.125f : 0.25f);
                for (uint32_t col = 0; col < h; ++col)
                    f.expert[uint64_t(row) * h + col] = Ref::ExpertBits(bf16, f.rank, row, col);
            }
            for (uint32_t col = 0; col < h; ++col) {
                float sum = 0.0f;
                for (uint32_t j = 0; j < k; ++j) {
                    volatile float product = Ref::Decode(f.expert[(uint64_t(p) * k + j) * h + col], bf16) * weights[p * k + j];
                    if (j == 0U) sum = product;
                    else { volatile float next = sum + product; sum = next; }
                }
                expected[uint64_t(p) * h + col] = Ref::Encode(sum, bf16);
            }
        }
        const auto savedMasks = masks;
        const auto savedRelay = relay;
        const auto savedTypes = f.types;
        const auto inactive = f.peers[f.rank];
        for (uint32_t core = 0; core < 64U; ++core) {
            f.Hooks(core, bank);
            const auto priorRead = AscendC::GmReadObserver();
            AscendC::GmReadObserver() = [&](const void *src, uint32_t bytes) {
                priorRead(src, bytes);
                if (dedup) return;
                const uintptr_t addr = reinterpret_cast<uintptr_t>(src);
                const auto forbid = [&](const void *base, size_t size) {
                    const uintptr_t begin = reinterpret_cast<uintptr_t>(base);
                    if (addr >= begin && addr < begin + size)
                        throw std::runtime_error("auto direct path consumed poisoned dedup metadata");
                };
                forbid(masks.data(), masks.size() * sizeof(uint32_t));
                forbid(relay.data(), relay.size() * sizeof(uint32_t));
                forbid(f.types.data(), f.types.size() * sizeof(int32_t));
            };
            // The fake does not itself reject waits on unseeded MTE3_V events.
            // Track their balance explicitly for the direct shared M5 arena.
            std::map<uint32_t, int> credits;
            AscendC::EventObserver() = [&](AscendC::HardEvent event, AscendC::TEventID id, bool set) {
                if (dedup || core < kEpServerDedupCombineSendCoreNum || event != AscendC::HardEvent::MTE3_V) return;
                if (set) ++credits[id];
                else {
                    Check(credits[id] > 0, "shared M6 waits only on explicitly seeded or published output slots");
                    --credits[id];
                }
            };
            CombineMemoryServerDedupKernel kernel;
            kernel.Init(reinterpret_cast<GM_ADDR>(&f.args), reinterpret_cast<GM_ADDR>(f.expert.data()),
                reinterpret_cast<GM_ADDR>(f.table.data()), reinterpret_cast<GM_ADDR>(f.counts.data()),
                reinterpret_cast<GM_ADDR>(weights.data()), reinterpret_cast<GM_ADDR>(f.types.data()),
                reinterpret_cast<GM_ADDR>(relay.data()), reinterpret_cast<GM_ADDR>(masks.data()),
                reinterpret_cast<GM_ADDR>(output.data()), bs, h, k, 32U,
                bf16 ? TileXR::TILEXR_DATA_TYPE_BFP16 : TileXR::TILEXR_DATA_TYPE_FP16, 0U, bank, 0U);
            kernel.Process();
            Check(AscendC::PendingMte3().empty(), "each core retires its direct sends, result clears, and output writes");
            if (core >= kEpServerDedupCombineSendCoreNum)
                Check(dedup ? f.readyProbes != 0U : f.readyProbes == 0U, "BS boundary selects M2/M5 or shared-layout M6");
            for (const auto &credit : credits) Check(credit.second == 0, "shared M6 drains every output seed exactly once");
            AscendC::EventObserver() = {};
            f.ResetHooks();
        }
        Check(std::equal(expected.begin(), expected.end(), output.begin()), "auto combine matches ascending-k FP32 reference");
        Check(std::all_of(output.begin() + expected.size(), output.end(), [](uint16_t v) { return v == 0x6b6bU; }), "BS change preserves output tail");
        Check(masks == savedMasks && relay == savedRelay && f.types == savedTypes, "metadata remains read-only through mode changes");
        Check(std::memcmp(f.peers[f.rank].data() + (1U - bank) * f.half,
            inactive.data() + (1U - bank) * f.half, f.half) == 0, "mode changes preserve inactive ping-pong bank");
        for (uint32_t row = 0; row < bs * k; ++row) for (uint32_t block = 0; block < blocks; ++block) {
            const auto *flag = f.peers[f.rank].data() + bank * f.half + TILEXR_EP_SERVER_DEDUP_SHARED_DATA_OFFSET + row * packet + block * 512U + 480U;
            Check(std::all_of(flag, flag + 32U, [](uint8_t v) { return v == 0; }), "shared M6 and M5 clear every result flag before reuse");
        }
    }
}

int main()
{
    try {
        for (bool bf16 : {false, true}) {
            CheckModeTransitions(257U, 6U, bf16);
            CheckModeTransitions(17U, 32U, bf16);
            CheckModeTransitions(7168U, 6U, bf16);
        }
    } catch (const std::exception &error) {
        AscendC::EventObserver() = {}; AscendC::GmReadObserver() = {};
        std::cerr << error.what() << '\n'; return 1;
    }
    return ServerDedupTest::Finish("auto_combine");
}
