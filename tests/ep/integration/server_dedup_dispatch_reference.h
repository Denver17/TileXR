#ifndef TILEXR_TEST_SERVER_DEDUP_DISPATCH_REFERENCE_H
#define TILEXR_TEST_SERVER_DEDUP_DISPATCH_REFERENCE_H

#include <algorithm>
#include <array>
#include "ep_memory_server_dedup_count_layout.h"
#include "server_dedup_source_send_fixture.h"

namespace ServerDedupReference {
struct DispatchRow {
    uint32_t source, token, expert, q, k, relay;
    Packet packet;
    bool Direct() const { return packet.members.size() == 1U; }
    bool FirstHop() const { return !Direct() && k == packet.members.front().k; }
};

// Construct public rows from every original route, in expert/source/q order.
// The source packet classifier and device prefix/address helpers are not used.
inline std::vector<DispatchRow> DispatchRows(const Shape &s, uint32_t destination)
{
    const uint32_t local = s.experts / s.ranks;
    std::vector<std::vector<DispatchRow>> byExpert(local);
    for (uint32_t source = 0; source < s.ranks; ++source) {
        const auto ids = Ids(s, source);
        const auto weights = Weights(s, source);
        std::vector<uint32_t> q(s.experts, 0U);
        for (uint32_t token = 0; token < s.bs; ++token) {
            std::vector<Route> routes;
            for (uint32_t k = 0; k < s.k; ++k) {
                const uint32_t expert = uint32_t(ids[token * s.k + k]);
                routes.push_back({expert, q[expert]++, k, weights[token * s.k + k]});
            }
            for (const auto &route : routes) {
                if (route.expert / local != destination) continue;
                Packet packet {token, {}};
                if (!TileXREp::kEpServerDedupEnable || destination / 8U == source / 8U) packet.members.push_back(route);
                else for (const auto &candidate : routes)
                    if (candidate.expert / local / 8U == destination / 8U) packet.members.push_back(candidate);
                byExpert[route.expert % local].push_back({source, token, route.expert, route.q, route.k,
                    packet.members.front().expert / local, packet});
            }
        }
    }
    std::vector<DispatchRow> rows;
    for (const auto &expert : byExpert) rows.insert(rows.end(), expert.begin(), expert.end());
    return rows;
}

inline uint64_t DispatchSlot(const Shape &s, uint32_t destination, const DispatchRow &row,
    const TileXREp::EpServerDedupDataLayout &layout)
{
    const uint32_t local = s.experts / s.ranks;
    return layout.dispatchOffset +
        (((uint64_t(row.source) + destination) % s.ranks * local + row.expert % local) * s.bs + row.q) * layout.tokenStride;
}

inline uint32_t FloatBits(float value)
{
    uint32_t bits;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

inline std::array<uint32_t, 8> DispatchAssist(const DispatchRow &row, uint32_t t)
{
    const auto &members = row.packet.members;
    const auto &route = *std::find_if(members.begin(), members.end(), [&](const Route &r) { return r.k == row.k; });
    return {{row.source, row.token, row.k, FloatBits(route.weight),
        row.Direct() ? 0U : row.FirstHop() ? uint32_t(members.size() - 1U) : UINT32_MAX,
        uint32_t(members.size() - 1U), row.Direct() ? UINT32_MAX : row.relay, row.Direct() ? UINT32_MAX : t}};
}
} // namespace ServerDedupReference
#endif
