#pragma once
#include <array>
#include <limits>
#include <iostream>
#include "kernel_operator.h"
#include "comm_args.h"
#include "ep_memory_server_dedup_combine_config.h"
#include "ep_memory_server_dedup_buffer_layout.h"
#include "ep_memory_server_dedup_combine_tiling.h"
#include "server_dedup_test_checks.h"

namespace CombineM12Test {
inline std::vector<uint64_t> &ClockSamples() { static std::vector<uint64_t> samples; return samples; }
inline size_t &ClockIndex() { static size_t index = 0; return index; }
inline uint64_t &Clock() { static uint64_t clock = 100; return clock; }
struct Fixture {
    TileXR::CommArgs args {};
    TileXREp::EpServerDedupCombineTilingData tiling {};
    std::array<std::vector<uint8_t>, 16> peers;
    std::vector<uint16_t> expert;
    std::vector<int32_t> table, counts, types;
    std::array<uint8_t, 128> untouched;
    uint32_t tokens, h, rank;
    uint64_t half, stageOffset, stageStride;
    uint32_t observedPrepare = 0, observedNotify = 0, prepareProbes = 0, readyProbes = 0;
    bool delayed = false;
    Fixture(uint32_t n, uint32_t width, uint32_t ownRank = 10U) : tokens(n), h(width), rank(ownRank)
    {
        const uint32_t bs = std::max(32U, (n + 47U) / 48U);
        tiling = {bs, h, 320U, 3U, 0U, 0U, 0U, {0U, 0U}};
        stageStride = (uint64_t(h) * 2U + 511U) / 512U * 512U;
        stageOffset = TILEXR_EP_SERVER_DEDUP_SHARED_DATA_OFFSET + uint64_t(bs) * 3U * ((h * 2U + 479U) / 480U * 512U);
        half = stageOffset + uint64_t(n == 0U ? 1U : n) * stageStride + 512U;
        args.rank = int(rank); args.rankSize = 16; args.peerMemBytes = half * 2U;
        for (uint32_t r = rank / 8U * 8U; r < rank / 8U * 8U + 8U; ++r) {
            // Both windows have real backing; tests observe the exact untouched
            // target gaps, inactive Read blocks and alternate window.
            peers[r].resize(half * 2U, 0xa5);
            args.peerMems[r] = peers[r].data();
            for (uint32_t w = 0; w < 2U; ++w) std::memset(peers[r].data() + w * half +
                TILEXR_EP_SERVER_DEDUP_COMBINE_CONTROL_OFFSET, 0, TILEXR_EP_SERVER_DEDUP_COMBINE_CONTROL_BYTES);
        }
        expert.resize(uint64_t(n) * h);
        table.resize(uint64_t(n) * 3U, -1); types.resize(n);
        counts.resize(320U, int32_t(n)); untouched.fill(0x69);
        for (uint32_t row = 0; row < n; ++row) {
            types[row] = row % 5U == 0U ? 2 : 3; // M1/M2 fixture excludes DIRECT routes.
            table[row * 3U] = int32_t(n - 1U - row); // reversed slots detect packed-index misuse
            for (uint32_t col = 0; col < h; ++col) expert[uint64_t(row) * h + col] = uint16_t(row * 31U + col);
        }
    }
    uint8_t *Local(uint32_t window, uint64_t relative)
    {
        return peers[rank].data() + window * half + TILEXR_EP_SERVER_DEDUP_COMBINE_CONTROL_OFFSET + relative;
    }
    void Hooks(uint32_t core, uint32_t window)
    {
        using namespace AscendC;
        testCore = core; observedPrepare = observedNotify = prepareProbes = readyProbes = 0;
        DeferMte3() = true;
        PaddedWriteObserver() = [this, core, window](const void *dst, const void *src, uint32_t bytes) {
            auto address = reinterpret_cast<uintptr_t>(dst);
            for (uint32_t j = 0; j < 8U; ++j) {
                auto expected = Local(window, j * 2048U + core * 32U);
                if (address == reinterpret_cast<uintptr_t>(expected) && bytes == 32U) {
                    float value; std::memcpy(&value, src, 4U);
                    ServerDedupTest::Check(value == 1.0f, "prepare publication owns complete32B slot");
                    ++observedPrepare;
                    if (j == 0U) {
                        std::vector<uint32_t> selected;
                        for (uint32_t row = 0; row < tokens; ++row) if (types[row] == 3) selected.push_back(row);
                        const uint32_t base = uint32_t(selected.size()) / 64U, extra = uint32_t(selected.size()) % 64U;
                        const uint32_t start = core * base + std::min(core, extra);
                        const uint32_t count = base + (core < extra ? 1U : 0U);
                        for (uint32_t task = start; task < start + count; ++task) {
                            const uint32_t row = selected[task], slot = uint32_t(table[row * 3U]);
                            const auto staged = peers[rank].data() + window * half + stageOffset + slot * stageStride;
                            ServerDedupTest::Check(std::memcmp(staged, expert.data() + uint64_t(row) * h, h * 2U) == 0,
                                "prepare publishes only after all of this core's stage writes retire");
                        }
                    }
                }
            }
            if (core >= 56U) {
                const uint32_t target = rank / 8U * 8U + core - 56U;
                for (uint32_t r = 0; r < TileXREp::kEpServerDedupCombineRecvCoreNum; ++r) {
                    auto expected = peers[target].data() + window * half + TILEXR_EP_SERVER_DEDUP_COMBINE_CONTROL_OFFSET +
                        TILEXR_EP_SERVER_DEDUP_COMBINE_SERVER_READY_OFFSET + r * 512U + rank % 8U * 32U;
                    if (address == reinterpret_cast<uintptr_t>(expected) && bytes == 32U) ++observedNotify;
                }
            }
        };
        GmReadObserver() = [this, core, window](const void *src, uint32_t bytes) {
            // A bounded external-producer scheduler supplies the remaining
            // source records. First probe is incomplete, later probes complete;
            // nonleading floats are poison and must not participate in the sum.
            if (core >= 56U && src == Local(window, (core - 56U) * 2048U) && bytes == 2048U) {
                ++prepareProbes;
                FillFlags(const_cast<void *>(src), 64U, !delayed || prepareProbes > 1U);
            }
            if (core >= TileXREp::kEpServerDedupCombineSendCoreNum &&
                src == Local(window, TILEXR_EP_SERVER_DEDUP_COMBINE_SERVER_READY_OFFSET +
                    (core - TileXREp::kEpServerDedupCombineSendCoreNum) * 512U) && bytes == 256U) {
                ++readyProbes;
                FillFlags(const_cast<void *>(src), 8U, !delayed || readyProbes > 1U);
            }
        };
    }
    static void FillFlags(void *address, uint32_t count, bool complete)
    {
        auto bytes = static_cast<uint8_t *>(address);
        for (uint32_t i = 0; i < count; ++i) {
            std::memset(bytes + i * 32U, 0x40, 32U);
            float flag = complete || i + 1U < count ? 1.0f : 0.0f;
            std::memcpy(bytes + i * 32U, &flag, 4U);
        }
    }
    void ResetHooks()
    {
        AscendC::GmReadObserver() = {}; AscendC::PaddedWriteObserver() = {};
        AscendC::DeferMte3() = false;
    }
    ~Fixture() { ResetHooks(); }
};
} // namespace CombineM12Test

namespace AscendC {
uint32_t testCore = 0;
uint64_t testCopies = 0;
int64_t GetSystemCycle()
{
    auto &samples = CombineM12Test::ClockSamples();
    uint64_t value;
    if (samples.empty()) value = ++CombineM12Test::Clock();
    else {
        if (CombineM12Test::ClockIndex() == samples.size()) throw std::runtime_error("extra delay clock read");
        value = samples[CombineM12Test::ClockIndex()++];
    }
    int64_t result; std::memcpy(&result, &value, sizeof(result)); return result;
}
}
