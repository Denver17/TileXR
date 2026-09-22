#pragma once
#include <algorithm>
#include <cstdint>
#include <vector>

namespace CombineServerOrderReference {
// Independent oracle: construct both ascending halves and rotate their lists.
inline std::vector<uint32_t> Order(uint32_t servers, uint32_t ownServer)
{
    std::vector<uint32_t> halves[2];
    for (uint32_t s = 0; s < servers; ++s) halves[s < servers / 2U ? 0 : 1].push_back(s);
    const uint32_t own = ownServer < servers / 2U ? 0U : 1U;
    const auto position = std::find(halves[own].begin(), halves[own].end(), ownServer);
    const auto next = size_t(position - halves[own].begin()) + 1U;
    std::vector<uint32_t> result;
    for (uint32_t half : {own, 1U - own}) {
        auto &list = halves[half];
        if (list.empty()) continue;
        std::rotate(list.begin(), list.begin() + next % list.size(), list.end());
        result.insert(result.end(), list.begin(), list.end());
    }
    return result;
}
inline uint32_t Position(const std::vector<uint32_t> &order, uint32_t server)
{
    return uint32_t(std::find(order.begin(), order.end(), server) - order.begin());
}
} // namespace CombineServerOrderReference
