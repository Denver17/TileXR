#include "../../tools/rdma_compare/multiqp_layout.h"
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdio>
#include <set>
#include <vector>

namespace {
uint64_t Golden(uint32_t rank, uint32_t qp, uint32_t entry, uint32_t slot, uint32_t n)
{
    const uint64_t base = rank == 0 ? 0xECAC4F6D875F76DBULL : 0x4EF5C5A60681F2E4ULL;
    return n == 1 ? base ^ (uint64_t(qp) * 0xD6E8FEB86659FD93ULL) :
        base ^ (uint64_t(qp) * 0xE7037ED1A0B428DBULL) ^
        (uint64_t(entry + 1) * 0xD6E8FEB86659FD93ULL) ^ (uint64_t(slot + 1) * 0xA0761D6478BD642FULL);
}

using Memory = std::array<std::vector<uint64_t>,2>;
Memory Initial(const CaseLayout& l, uint32_t qp)
{
    Memory memory;
    for (uint32_t rank = 0; rank < 2; ++rank) {
        memory[rank].assign(size_t(l.bytes / 8), kCaseGuardWord);
        for (uint32_t slot = 0; slot < l.slots; ++slot) {
            for (uint32_t physical = 0; physical < l.sgeCount; ++physical) {
                const auto offset = l.source + slot*l.stride + physical*(uint64_t(l.sgeBytes)+(l.sgeCount>1 ? 64 : 0));
                std::fill_n(memory[rank].begin()+offset/8,l.sgeBytes/8,
                    Golden(rank,qp,l.sgeCount-1-physical,slot,l.sgeCount));
            }
        }
    }
    return memory;
}

void Transfer(Memory& memory, const CaseLayout& l, uint32_t count, uint32_t direction)
{
    // Every source slot is constant across wraps. Copy each touched slot once.
    for (uint32_t wr = 0; wr < std::min(count,l.slots); ++wr) {
        const uint32_t slot = (wr+1)%l.slots;
        for (uint32_t entry = 0; entry < l.sgeCount; ++entry) {
            const auto source = l.SourceOffset(slot,entry);
            const auto target = l.target + slot*l.targetStride + entry*uint64_t(l.sgeBytes);
            std::copy_n(memory[direction].begin()+source/8,l.sgeBytes/8,memory[1-direction].begin()+target/8);
        }
    }
}

bool Matches(const Memory& memory, const CaseLayout& l, uint32_t qp, uint32_t count, uint32_t direction)
{
    for (uint32_t rank = 0; rank < 2; ++rank) {
        for (uint64_t off = 0; off < l.bytes; off += 8) {
            if (memory[rank][off/8] != ExpectedMultiQpWord(l,off,rank,count,direction,qp)) { return false; }
        }
    }
    return true;
}
}

int main()
{
    // QP/entry/slot swaps must never impersonate another payload or a guard.
    for (uint32_t n : {1U,2U,4U,8U,16U}) {
        std::set<uint64_t> tags;
        for (uint32_t rank = 0; rank < 2; ++rank) {
            for (uint32_t qp = 0; qp < 16; ++qp) {
                for (uint32_t slot = 0; slot < (n==1 ? 1U : 32U); ++slot) {
                    for (uint32_t entry = 0; entry < n; ++entry) {
                        const auto tag = CasePattern(rank,entry,slot,n)^MultiQpPattern(qp,n);
                        assert(tag == Golden(rank,qp,entry,slot,n));
                        assert(tag != kCaseGuardWord && tags.insert(tag).second);
                    }
                }
            }
        }
        for (uint32_t qp : {0U,15U}) {
            for (uint32_t bytes : {512U,1048576U}) {
                for (uint32_t count : {1U,512U}) {
                    const CaseLayout l(bytes,count,false,n);
                    for (uint32_t direction : {0U,1U}) {
                        auto memory = Initial(l,qp);
                        Transfer(memory,l,count,direction);
                        assert(Matches(memory,l,qp,count,direction));
                        assert(!Matches(memory,l,qp^1U,count,direction));
                        const auto dst = l.target + (l.slots>1 ? l.targetStride : 0);
                        if (n>1) {
                            std::swap(memory[1-direction][dst/8],memory[1-direction][(dst+bytes)/8]);
                            assert(!Matches(memory,l,qp,count,direction));
                            std::swap(memory[1-direction][dst/8],memory[1-direction][(dst+bytes)/8]);
                        }
                        for (uint64_t off : {uint64_t(0),l.source+bytes,l.target-8,l.bytes-8}) {
                            memory[1-direction][off/8] ^= 1;
                            assert(!Matches(memory,l,qp,count,direction));
                            memory[1-direction][off/8] ^= 1;
                        }
                    }
                }
            }
        }
    }
    std::puts("rdma_multiqp_layout: PASS (QP/SGE identity, gather, full guards, cross-QP corruption)");
}
