#ifndef TILEXR_TEST_SERVER_DEDUP_FIRST_HOP_REFERENCE_H
#define TILEXR_TEST_SERVER_DEDUP_FIRST_HOP_REFERENCE_H

#include <cstdint>
#include <map>
#include <vector>

namespace ServerDedupReference {
struct Route { uint32_t expert, q, k; float weight; };
struct Packet { uint32_t token; std::vector<Route> members; };

// Independent Host oracle: histogram the unfiltered route order, then group by
// destination server. It does not call the device classifier or q reduction.
inline std::vector<Packet> FirstHops(const std::vector<int32_t> &ids, const std::vector<float> &weights,
    uint32_t source, uint32_t ranks, uint32_t experts, uint32_t k)
{
    std::vector<uint32_t> histogram(experts, 0U);
    std::vector<Packet> packets;
    const uint32_t localExperts = experts / ranks;
    for (uint32_t token = 0; token < ids.size() / k; ++token) {
        std::map<uint32_t, std::vector<Route>> groups;
        for (uint32_t route = 0; route < k; ++route) {
            const uint32_t expert = uint32_t(ids[token * k + route]);
            groups[expert / localExperts / 8U].push_back({expert, histogram[expert]++, route, weights[token * k + route]});
        }
        for (auto &group : groups) if (group.first != source / 8U && group.second.size() > 1U)
            packets.push_back({token, group.second});
    }
    return packets;
}
} // namespace ServerDedupReference
#endif
