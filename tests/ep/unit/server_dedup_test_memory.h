#pragma once
#include "kernel_operator.h"
#include "ep_memory_server_dedup_count_layout.h"
#include "server_dedup_test_checks.h"

namespace ServerDedupTest {
// Sparse backing covers only the exercised bytes in each actual-capacity bank. Accesses
// to the remaining synthetic communication domain fail instead of allocating it.
class SparsePeerMemory {
public:
    explicit SparsePeerMemory(uint64_t peerBytes = kPeerBytes) : peerBytes_(peerBytes) {
        AscendC::GmAddressResolver() = [this](uintptr_t address) -> void * {
            for (const auto &region : regions_)
                if (address >= region.address && address - region.address < region.bytes)
                    return region.storage + address - region.address;
            if (address >= Base(0) && address < Base(128))
                throw std::runtime_error("GM access outside mapped peer bank");
            return reinterpret_cast<void *>(address);
        };
    }
    ~SparsePeerMemory() { AscendC::GmAddressResolver() = {}; }
    SparsePeerMemory(const SparsePeerMemory &) = delete;
    SparsePeerMemory &operator=(const SparsePeerMemory &) = delete;
    static uintptr_t Base(uint32_t rank) {
        return UINT64_C(0x100000000000) + uint64_t(rank) * (16ULL * 1024U * 1024U * 1024U);
    }
    void Add(uint32_t rank, uint32_t bank, void *storage, uint64_t bytes) {
        regions_.push_back({Base(rank) + uint64_t(bank) * (peerBytes_ / 2U),
            static_cast<uint8_t *>(storage), bytes});
    }
private:
    struct Region { uintptr_t address; uint8_t *storage; uint64_t bytes; };
    std::vector<Region> regions_;
    uint64_t peerBytes_;
};
} // namespace ServerDedupTest
