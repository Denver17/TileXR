#include <cstdint>
#include <iostream>
#include <set>

#include "../common/int32_pattern.h"

int main()
{
    using TileXRCollectivesTest::CanUseCollisionFreeInt32Pattern;
    using TileXRCollectivesTest::ExpectedAllGatherValue;
    using TileXRCollectivesTest::ExpectedAllToAllValue;

    constexpr int rankSize = 8;
    constexpr int64_t reviewedCollisionA = 9646;
    constexpr int64_t reviewedCollisionB = 76425;
    if (ExpectedAllGatherValue(rankSize, 0, reviewedCollisionA) ==
        ExpectedAllGatherValue(rankSize, 0, reviewedCollisionB)) {
        std::cerr << "reviewed allgather collision was reintroduced" << std::endl;
        return 1;
    }
    if (!CanUseCollisionFreeInt32Pattern(128, 262144)) {
        std::cerr << "pattern should cover 1MiB INT32 validation at rank_size=128" << std::endl;
        return 1;
    }

    std::set<int32_t> seen;
    constexpr int64_t sampleCount = 4096;
    for (int src = 0; src < rankSize; ++src) {
        for (int64_t index = 0; index < sampleCount; ++index) {
            const int32_t value = ExpectedAllGatherValue(rankSize, src, index);
            if (!seen.insert(value).second) {
                std::cerr << "allgather int32 pattern collision src=" << src
                          << " index=" << index << std::endl;
                return 1;
            }
        }
    }

    seen.clear();
    for (int src = 0; src < rankSize; ++src) {
        for (int dst = 0; dst < rankSize; ++dst) {
            for (int64_t index = 0; index < sampleCount; ++index) {
                const int32_t value = ExpectedAllToAllValue(rankSize, src, dst, index);
                if (!seen.insert(value).second) {
                    std::cerr << "alltoall int32 pattern collision src=" << src
                              << " dst=" << dst << " index=" << index << std::endl;
                    return 1;
                }
            }
        }
    }

    return 0;
}
