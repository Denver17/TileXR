#ifndef TILEXR_TEST_SERVER_DEDUP_DISPATCH_REFERENCE_H
#define TILEXR_TEST_SERVER_DEDUP_DISPATCH_REFERENCE_H

#include <algorithm>
#include <array>
#include <set>
#include <tuple>
#include "ep_memory_server_dedup_count_layout.h"
#include "server_dedup_source_send_fixture.h"

namespace ServerDedupReference {
struct DispatchRow {
    uint32_t source, token, expert, q, k;
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
                byExpert[route.expert % local].push_back({source, token, route.expert, route.q, route.k, packet});
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

inline uint32_t TokenType(const DispatchRow &row)
{
    return row.Direct() ? 1U : row.FirstHop() ? 2U : 3U;
}

inline uint32_t MemberSlot(const Shape &s, uint32_t source, const Route &route)
{
    const uint32_t local = s.experts / s.ranks, destination = route.expert / local;
    return (((source + destination) % s.ranks) * local + route.expert % local) * s.bs + route.q;
}

inline std::array<uint32_t, 3> DestinationIndex(const Shape &s, const DispatchRow &row)
{
    if (row.Direct() || row.FirstHop()) return {{row.source, row.token, row.k}};
    return {{MemberSlot(s, row.source, {row.expert, row.q, row.k, 0.0f}), UINT32_MAX, UINT32_MAX}};
}

// Only the valid blocks are defined; callers must not compare the unused suffix.
inline std::vector<uint32_t> RelayReadIndex(const DispatchRow &row)
{
    const auto &members = row.packet.members;
    std::vector<uint32_t> words {uint32_t(members.size() - 1U), FloatBits(members.front().weight)};
    for (size_t member = 1; member < members.size(); ++member) {
        const auto &route = members[member];
        words.push_back(route.expert | (route.q << 16U));
        words.push_back(FloatBits(route.weight));
    }
    return words;
}

// Compare complete identities and multiplicity, not a set of slot addresses.
// Each rank reconstructs this oracle locally; no actual tables are exchanged.
inline bool RelayAddressesMatch(const Shape &s)
{
    using Identity = std::tuple<uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t>;
    std::multiset<Identity> reads, writes;
    const uint32_t local = s.experts / s.ranks;
    for (uint32_t destination = 0; destination < s.ranks; ++destination) {
        for (const auto &row : DispatchRows(s, destination)) {
            if (row.FirstHop()) {
                for (size_t i = 1; i < row.packet.members.size(); ++i) {
                    const auto &route = row.packet.members[i];
                    reads.emplace(row.source, row.token, row.k, route.k, route.expert / local,
                        MemberSlot(s, row.source, route), FloatBits(route.weight));
                }
            } else if (!row.Direct()) {
                const auto &route = *std::find_if(row.packet.members.begin(), row.packet.members.end(),
                    [&](const Route &candidate) { return candidate.k == row.k; });
                const uint32_t slot = (((row.source + destination) % s.ranks) * local +
                    row.expert % local) * s.bs + row.q;
                writes.emplace(row.source, row.token, row.packet.members.front().k, row.k, destination,
                    slot, FloatBits(route.weight));
            }
        }
    }
    return reads == writes;
}
} // namespace ServerDedupReference
#endif
