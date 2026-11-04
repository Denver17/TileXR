// Standalone (from the checkout root):
// g++ -std=c++14 -O2 -Wall -Wextra -Werror tests/rdma/test_rdma_sge_layout.cpp -o test_rdma_sge_layout
// ./test_rdma_sge_layout
#include "../../tools/rdma_compare/case_layout.h"
#include "../../tools/rdma_compare/bench_abi.h"
#include "../../src/include/tilexr_rdma_wire.h"
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <set>
#include <vector>

#define CHECK(condition) do { if (!(condition)) { \
    std::fprintf(stderr, "line %d: %s\n", __LINE__, #condition); std::exit(1); \
} } while (false)

namespace {
constexpr uint64_t guard = 0xA5A5A5A5A5A5A5A5ULL;
constexpr uint32_t sgeCounts[] = {1, 2, 4, 8, 16};

uint64_t GoldenPattern(uint32_t rank, uint32_t entry, uint32_t slot, uint32_t n)
{
    const uint64_t base = rank == 0 ? 0xECAC4F6D875F76DBULL : 0x4EF5C5A60681F2E4ULL;
    return n == 1 ? base : base ^ (uint64_t(entry + 1) * 0xD6E8FEB86659FD93ULL) ^
        (uint64_t(slot + 1) * 0xA0761D6478BD642FULL);
}

void TestGoldenLayouts()
{
    struct Golden {
        uint32_t n, payload, count;
        bool streaming;
        uint32_t slots;
        uint64_t sourceStride, targetStride, target, bytes;
    };
    const Golden cases[] = {
        {1, 512, 65, false, 32, 512, 512, 24576, 45056},
        {1, 512, 65, true, 65, 576, 576, 49152, 90688},
        {2, 512, 65, false, 32, 1152, 1088, 45056, 83968},
        {4, 512, 65, false, 32, 2304, 2112, 81920, 153600},
        {8, 512, 65, false, 32, 4608, 4160, 155648, 292864},
        {16, 512, 65, false, 32, 9216, 8256, 303104, 571392},
        {16, 1048576, 65, false, 1, 16778240, 16777280, 16789504, 33570880},
        {2, 512, 3, true, 3, 1152, 1088, 12288, 19648}
    };
    for (const auto& g : cases) {
        const CaseLayout l(g.payload, g.count, g.streaming, g.n);
        CHECK(l.source == 4096 && l.target == g.target && l.bytes == g.bytes);
        CHECK(l.stride == g.sourceStride && l.targetStride == g.targetStride);
        CHECK(l.slots == g.slots && l.cycles == (g.streaming ? 0U : g.slots));
        CHECK(l.sgeCount == g.n && l.sgeBytes == g.payload);
        CHECK(l.totalWqeBytes == uint64_t(g.n) * g.payload);
    }
    const CaseLayout four(512, 65, false, 4);
    const uint64_t reversed[] = {5824, 5248, 4672, 4096};
    for (uint32_t entry = 0; entry < 4; ++entry) {
        CHECK(four.SourceOffset(0, entry) == reversed[entry]);
        CHECK(four.SourceOffset(1, entry) == reversed[entry] + 2304);
    }
    const uint32_t blocks[] = {1, 2, 2, 3, 5};
    for (uint32_t i = 0; i < 5; ++i) {
        CHECK(TileXR::RdmaWire::WqeBlocks(sgeCounts[i]) == blocks[i]);
    }
    CHECK(CasePattern(0, 0, 0, 1) == 0xECAC4F6D875F76DBULL);
    CHECK(CasePattern(1, 15, 999, 1) == 0x4EF5C5A60681F2E4ULL);
    CHECK(CasePattern(0, 0, 0, 2) == 0x9A32ACB199BBEF67ULL);
    CHECK(CasePattern(0, 1, 0, 2) == 0xE10BAF793351E9D2ULL);
    CHECK(CasePattern(0, 0, 1, 2) == 0x7AA88B1D107C4316ULL);
    CHECK(CasePattern(1, 15, 999, 16) == 0xED18FE57C0D17C4CULL);
}

void TestSizesAndLegacy()
{
    for (uint32_t payload = 512; payload <= 1048576; payload *= 2) {
        const uint32_t oldSlots = payload <= 8192 ? 32 : payload <= 16384 ? 16 :
            payload <= 32768 ? 8 : payload <= 65536 ? 4 : payload <= 131072 ? 2 : 1;
        for (const bool streaming : {false, true}) {
            for (const uint32_t count : {1U, 48U, 64U, 65U, 128U, 512U, 1000U}) {
                const CaseLayout old(payload, count, streaming);
                const uint64_t oldStride = payload + (streaming ? 64U : 0U);
                const uint32_t slots = streaming ? count : oldSlots;
                const uint64_t oldTarget = (4096 + uint64_t(slots) * oldStride + 8191) & ~uint64_t(4095);
                CHECK(old.source == 4096 && old.stride == oldStride && old.targetStride == oldStride);
                CHECK(old.slots == slots && old.cycles == (streaming ? 0U : slots));
                CHECK(old.target == oldTarget && old.bytes == oldTarget + slots * oldStride + 4096);
                CHECK(old.SourceOffset(slots - 1, 0) == 4096 + (slots - 1) * oldStride);
                for (const uint32_t n : sgeCounts) {
                    const CaseLayout l(payload, count, streaming, n);
                    CHECK(l.slots == (streaming ? count : CyclicSlots(uint64_t(payload) * n)));
                    CHECK(l.source + uint64_t(l.slots) * l.stride + 4096 <= l.target);
                    CHECK(l.target % 4096 == 0 && l.bytes % 8 == 0);
                    CHECK(l.target + uint64_t(l.slots) * l.targetStride + 4096 == l.bytes);
                    CHECK(l.totalWqeBytes <= 16U * 1048576U);
                    if (n > 1) { CHECK(l.stride != l.targetStride); }
                    for (uint32_t e = 0; e < n; ++e) {
                        const uint64_t offset = l.SourceOffset(l.slots - 1, e);
                        CHECK(offset >= l.source && offset + payload <= l.target - 4096);
                        if (e + 1 < n) { CHECK(offset - l.SourceOffset(l.slots - 1, e + 1) == payload + 64); }
                    }
                }
            }
        }
    }
    // No rank/entry/slot can impersonate another or a guard in supported cases.
    std::set<uint64_t> patterns;
    for (uint32_t rank = 0; rank < 2; ++rank) {
        for (uint32_t slot = 0; slot < 1000; ++slot) {
            for (uint32_t entry = 0; entry < 16; ++entry) {
                const auto value = CasePattern(rank, entry, slot, 16);
                CHECK(value != guard && patterns.insert(value).second);
            }
        }
    }
}

using Memory = std::array<std::vector<uint64_t>, 2>;

Memory InitialMemory(const CaseLayout& l)
{
    Memory memory;
    for (uint32_t rank = 0; rank < 2; ++rank) {
        memory[rank].assign(size_t(l.bytes / 8), guard);
        // Initialize in increasing physical order, independently of the kernel's
        // reversed descriptor helper and the verifier's address classification.
        for (uint32_t slot = 0; slot < l.slots; ++slot) {
            for (uint32_t physical = 0; physical < l.sgeCount; ++physical) {
                const uint64_t pitch = l.sgeBytes + (l.sgeCount == 1 ? 0U : 64U);
                const uint64_t offset = l.source + slot * l.stride + physical * pitch;
                std::fill_n(memory[rank].begin() + offset / 8, l.sgeBytes / 8,
                    GoldenPattern(rank, l.sgeCount - 1 - physical, slot, l.sgeCount));
            }
        }
    }
    return memory;
}

enum class Fault { None, SortedSources, ContiguousSource, EqualStrides };

void Transfer(Memory& memory, const CaseLayout& l, uint32_t count, uint32_t direction, Fault fault = Fault::None)
{
    for (uint32_t rank = 0; rank < 2; ++rank) {
        if (direction != 2 && direction != rank) { continue; }
        for (uint32_t wr = 0; wr < count; ++wr) {
            const uint32_t slot = l.cycles == 0 ? wr : (wr + 1) % l.cycles;
            for (uint32_t entry = 0; entry < l.sgeCount; ++entry) {
                const uint32_t sourceEntry = fault == Fault::SortedSources ? l.sgeCount - 1 - entry : entry;
                const uint64_t source = fault == Fault::ContiguousSource ?
                    l.source + slot * l.stride + uint64_t(entry) * l.sgeBytes : l.SourceOffset(slot, sourceEntry);
                const uint64_t targetStride = fault == Fault::EqualStrides ? l.stride : l.targetStride;
                const uint64_t target = l.target + slot * targetStride + uint64_t(entry) * l.sgeBytes;
                // The wrong-stride fault uses small slot indices to stay in MR.
                CHECK(source + l.sgeBytes <= l.bytes && target + l.sgeBytes <= l.bytes);
                std::copy_n(memory[rank].begin() + source / 8, l.sgeBytes / 8, memory[1 - rank].begin() + target / 8);
            }
        }
    }
}

bool Matches(const Memory& memory, const CaseLayout& l, uint32_t count, uint32_t direction)
{
    for (uint32_t rank = 0; rank < 2; ++rank) {
        for (uint64_t off = 0; off < l.bytes; off += 8) {
            if (memory[rank][size_t(off / 8)] != ExpectedCaseWord(l, off, rank, count, direction)) { return false; }
        }
    }
    return true;
}

void TestFullMemoryProof()
{
    for (const uint32_t n : sgeCounts) {
        for (const bool streaming : {false, true}) {
            for (const uint32_t count : {1U, 31U, 32U, 33U, 48U, 64U, 65U, 128U, 512U, 1000U}) {
                const CaseLayout l(512, count, streaming, n);
                for (const uint32_t direction : {0U, 1U, 2U}) {
                    auto memory = InitialMemory(l);
                    Transfer(memory, l, count, direction);
                    CHECK(Matches(memory, l, count, direction));
                }
            }
        }
    }
    // At the largest WQE, cyclic slot zero is used immediately; no 32-bit
    // truncation, omitted final entry, or guard overwrite is hidden by size.
    const CaseLayout largest(1048576, 1, false, 16);
    auto memory = InitialMemory(largest);
    Transfer(memory, largest, 1, 0);
    CHECK(Matches(memory, largest, 1, 0));
}

void TestCorruptionDetection()
{
    const CaseLayout l(512, 1, false, 4);
    const auto initial = InitialMemory(l);
    for (const auto fault : {Fault::SortedSources, Fault::ContiguousSource, Fault::EqualStrides}) {
        auto memory = initial;
        Transfer(memory, l, 1, 0, fault);
        CHECK(!Matches(memory, l, 1, 0));
    }
    auto memory = initial;
    Transfer(memory, l, 1, 0);
    CHECK(Matches(memory, l, 1, 0));
    const uint64_t guardOffsets[] = {
        0, l.source - 8, l.source + 512, l.source + l.stride - 8,
        l.source + l.slots * l.stride, l.target - 8,
        l.target, // untouched cyclic slot zero when count=1, slots=32
        l.target + l.targetStride + l.totalWqeBytes,
        l.target + l.slots * l.targetStride, l.bytes - 8
    };
    for (const uint64_t offset : guardOffsets) {
        CHECK(memory[1][offset / 8] == guard);
        memory[1][offset / 8] ^= 1;
        CHECK(!Matches(memory, l, 1, 0));
        memory[1][offset / 8] ^= 1;
    }
    // Also catch modification of either rank's source, and payload corruption.
    for (const uint32_t rank : {0U, 1U}) {
        const auto offset = l.SourceOffset(0, 0);
        memory[rank][offset / 8] ^= 1;
        CHECK(!Matches(memory, l, 1, 0));
        memory[rank][offset / 8] ^= 1;
    }
    const uint64_t target = l.target + l.targetStride;
    std::swap(memory[1][target / 8], memory[1][(target + 512) / 8]);
    CHECK(!Matches(memory, l, 1, 0));
}
} // namespace

int main()
{
    TestGoldenLayouts();
    TestSizesAndLegacy();
    TestFullMemoryProof();
    TestCorruptionDetection();
    std::puts("rdma_sge_layout: PASS (legacy, golden layout/pattern, gather, full MR and fault detection)");
}
