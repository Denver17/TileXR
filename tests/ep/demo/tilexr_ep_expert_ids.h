#ifndef TILEXR_TESTS_EP_DEMO_EXPERT_IDS_H
#define TILEXR_TESTS_EP_DEMO_EXPERT_IDS_H

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <numeric>
#include <random>
#include <vector>

namespace TileXREpDemo {

struct ExpertIdsConfig {
    int64_t bs = 0;
    int64_t topK = 0;
    int64_t moeExpertNum = 0;
    int64_t moeRankNum = 0;
    int64_t epWorldSize = 0;
    int64_t epRankId = 0;
    uint32_t seed = 0;
};

inline bool IsValidExpertIdsConfig(const ExpertIdsConfig &config)
{
    return config.bs > 0 && config.topK > 0 && config.moeExpertNum > 0 &&
        config.topK <= config.moeExpertNum && config.moeRankNum > 0 &&
        config.epWorldSize > 0 && config.epRankId >= 0 && config.epRankId < config.epWorldSize;
}

inline std::vector<int32_t> BuildBalancedExpertIds(const ExpertIdsConfig &config)
{
    if (!IsValidExpertIdsConfig(config)) {
        return {};
    }

    const int64_t localExpertNum =
        (config.moeExpertNum + config.moeRankNum - 1) / config.moeRankNum;
    std::vector<int32_t> baseOrder;
    baseOrder.reserve(static_cast<std::size_t>(config.moeExpertNum));
    for (int64_t localExpertId = 0; localExpertId < localExpertNum; ++localExpertId) {
        for (int64_t expertRankId = 0; expertRankId < config.moeRankNum; ++expertRankId) {
            const int64_t expertId = expertRankId * localExpertNum + localExpertId;
            if (expertId < config.moeExpertNum) {
                baseOrder.push_back(static_cast<int32_t>(expertId));
            }
        }
    }

    const std::size_t routeCount = static_cast<std::size_t>(config.bs * config.topK);
    const std::size_t start = static_cast<std::size_t>(
        (config.epRankId * config.bs * config.topK) % config.moeExpertNum);
    std::vector<int32_t> expertIds(routeCount);
    for (std::size_t route = 0; route < routeCount; ++route) {
        expertIds[route] = baseOrder[(start + route) % baseOrder.size()];
    }
    return expertIds;
}

inline std::vector<int32_t> BuildRandomExpertIds(const ExpertIdsConfig &config)
{
    if (!IsValidExpertIdsConfig(config)) {
        return {};
    }

    const std::size_t routeCount = static_cast<std::size_t>(config.bs * config.topK);
    std::vector<int32_t> expertIds(routeCount);
    std::vector<int32_t> candidates(static_cast<std::size_t>(config.moeExpertNum));
    for (int64_t token = 0; token < config.bs; ++token) {
        std::iota(candidates.begin(), candidates.end(), 0);
        const uint64_t globalToken = static_cast<uint64_t>(config.epRankId * config.bs + token);
        std::seed_seq tokenSeed {
            config.seed,
            static_cast<uint32_t>(globalToken),
            static_cast<uint32_t>(globalToken >> 32U),
        };
        std::mt19937 generator(tokenSeed);
        std::shuffle(candidates.begin(), candidates.end(), generator);
        for (int64_t topKId = 0; topKId < config.topK; ++topKId) {
            expertIds[static_cast<std::size_t>(token * config.topK + topKId)] =
                candidates[static_cast<std::size_t>(topKId)];
        }
    }
    return expertIds;
}

} // namespace TileXREpDemo

#endif // TILEXR_TESTS_EP_DEMO_EXPERT_IDS_H
