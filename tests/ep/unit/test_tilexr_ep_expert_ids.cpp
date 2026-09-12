#include <algorithm>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

#include "tilexr_ep_expert_ids.h"

namespace {

int g_failures = 0;

void CheckTrue(const std::string &label, bool condition)
{
    if (!condition) {
        std::cerr << label << std::endl;
        ++g_failures;
    }
}

void CheckVector(const std::string &label, const std::vector<int32_t> &actual,
    const std::vector<int32_t> &expected)
{
    if (actual != expected) {
        std::cerr << label << " actual:";
        for (int32_t value : actual) {
            std::cerr << " " << value;
        }
        std::cerr << " expected:";
        for (int32_t value : expected) {
            std::cerr << " " << value;
        }
        std::cerr << std::endl;
        ++g_failures;
    }
}

void TestBalancedMatchesCommercialOrderAndOffsets()
{
    TileXREpDemo::ExpertIdsConfig config {};
    config.bs = 2;
    config.topK = 2;
    config.moeExpertNum = 8;
    config.moeRankNum = 4;
    config.epWorldSize = 4;

    config.epRankId = 0;
    CheckVector("balanced rank 0", TileXREpDemo::BuildBalancedExpertIds(config), {0, 2, 4, 6});
    config.epRankId = 1;
    CheckVector("balanced rank 1", TileXREpDemo::BuildBalancedExpertIds(config), {1, 3, 5, 7});
    config.epRankId = 2;
    CheckVector("balanced rank 2", TileXREpDemo::BuildBalancedExpertIds(config), {0, 2, 4, 6});
    config.epRankId = 3;
    CheckVector("balanced rank 3", TileXREpDemo::BuildBalancedExpertIds(config), {1, 3, 5, 7});

    config.bs = 1;
    config.topK = 3;
    config.epRankId = 1;
    CheckVector("balanced non-half-cycle offset",
        TileXREpDemo::BuildBalancedExpertIds(config), {6, 1, 3});
}

void TestBalanced64pDistribution()
{
    TileXREpDemo::ExpertIdsConfig config {};
    config.bs = 96;
    config.topK = 6;
    config.moeExpertNum = 384;
    config.moeRankNum = 64;
    config.epWorldSize = 64;
    std::vector<int64_t> globalCounts(static_cast<std::size_t>(config.moeExpertNum), 0);

    for (int64_t epRank = 0; epRank < config.epWorldSize; ++epRank) {
        config.epRankId = epRank;
        const std::vector<int32_t> expertIds = TileXREpDemo::BuildBalancedExpertIds(config);
        CheckTrue("balanced 64p route count",
            expertIds.size() == static_cast<std::size_t>(config.bs * config.topK));
        for (int64_t token = 0; token < config.bs; ++token) {
            std::vector<bool> targetRanks(static_cast<std::size_t>(config.moeRankNum), false);
            for (int64_t topKId = 0; topKId < config.topK; ++topKId) {
                const int32_t expertId = expertIds[static_cast<std::size_t>(token * config.topK + topKId)];
                ++globalCounts[static_cast<std::size_t>(expertId)];
                const std::size_t targetRank = static_cast<std::size_t>(expertId / 6);
                CheckTrue("balanced token target rank is unique", !targetRanks[targetRank]);
                targetRanks[targetRank] = true;
            }
        }
    }

    for (int64_t count : globalCounts) {
        CheckTrue("balanced 64p expert count", count == 96);
    }
}

void CheckRandomConstraints(const std::string &label, const TileXREpDemo::ExpertIdsConfig &config,
    const std::vector<int32_t> &expertIds)
{
    CheckTrue(label + " route count",
        expertIds.size() == static_cast<std::size_t>(config.bs * config.topK));
    for (int64_t token = 0; token < config.bs; ++token) {
        std::vector<bool> selected(static_cast<std::size_t>(config.moeExpertNum), false);
        for (int64_t topKId = 0; topKId < config.topK; ++topKId) {
            const int32_t expertId = expertIds[static_cast<std::size_t>(token * config.topK + topKId)];
            const bool inRange = expertId >= 0 && expertId < config.moeExpertNum;
            CheckTrue(label + " expert range", inRange);
            if (inRange) {
                CheckTrue(label + " token expert is unique", !selected[static_cast<std::size_t>(expertId)]);
                selected[static_cast<std::size_t>(expertId)] = true;
            }
        }
    }
}

void TestRandomConstraintsAndRankVariation()
{
    TileXREpDemo::ExpertIdsConfig config {};
    config.bs = 64;
    config.topK = 6;
    config.moeExpertNum = 384;
    config.moeRankNum = 64;
    config.epWorldSize = 64;
    config.seed = 1740;

    const std::vector<int32_t> rank0 = TileXREpDemo::BuildRandomExpertIds(config);
    CheckRandomConstraints("random rank 0", config, rank0);
    CheckTrue("random same seed is reproducible", rank0 == TileXREpDemo::BuildRandomExpertIds(config));

    config.epRankId = 1;
    const std::vector<int32_t> rank1 = TileXREpDemo::BuildRandomExpertIds(config);
    CheckRandomConstraints("random rank 1", config, rank1);
    CheckTrue("random EP ranks differ", rank0 != rank1);

}

void TestRejectsInvalidConfig()
{
    TileXREpDemo::ExpertIdsConfig config {};
    CheckTrue("balanced invalid config", TileXREpDemo::BuildBalancedExpertIds(config).empty());
    CheckTrue("random invalid config", TileXREpDemo::BuildRandomExpertIds(config).empty());
}

void TestServerDedupBalancedShiftMainCase()
{
    TileXREpDemo::ExpertIdsConfig config {96, 6, 96, 16, 16, 0, 2026};
    std::vector<int64_t> counts(96, 0);
    const auto rank0 = TileXREpDemo::BuildBalancedShuffleExpertIds(config);
    CheckTrue("balance_shift reproducible", rank0 == TileXREpDemo::BuildBalancedShuffleExpertIds(config));
    for (int rank = 0; rank < 16; ++rank) {
        config.epRankId = rank;
        const auto routes = TileXREpDemo::BuildBalancedShuffleExpertIds(config);
        CheckRandomConstraints("balance_shift main case", config, routes);
        if (routes.size() != 96 * 6) return;
        std::vector<int64_t> perSource(96, 0);
        for (int32_t expert : routes) {
            if (expert >= 0 && expert < 96) { ++counts[expert]; ++perSource[expert]; }
        }
        CheckTrue("balance_shift six routes per expert/source",
            std::all_of(perSource.begin(), perSource.end(), [](int64_t value) { return value == 6; }));
        if (rank == 1) CheckTrue("balance_shift rank permutation differs", routes != rank0);
    }
    CheckTrue("balance_shift 96 routes per expert globally",
        std::all_of(counts.begin(), counts.end(), [](int64_t value) { return value == 96; }));
}

} // namespace

int main()
{
    TestBalancedMatchesCommercialOrderAndOffsets();
    TestBalanced64pDistribution();
    TestRandomConstraintsAndRankVariation();
    TestServerDedupBalancedShiftMainCase();
    TestRejectsInvalidConfig();
    return g_failures == 0 ? 0 : 1;
}
