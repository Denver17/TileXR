#include <set>
#include <tuple>
#include "../integration/server_dedup_dispatch_reference.h"
#include "server_dedup_test_checks.h"

using namespace ServerDedupReference;
using ServerDedupTest::Check;

// Compare public rows against freshly generated routes after interleaving
// configurations. A stale input cache can silently corrupt the Host oracle.
void CheckRows(const Shape &shape)
{
    using Identity = std::tuple<uint32_t, uint32_t, uint32_t>;
    std::set<Identity> seen;
    std::vector<std::vector<int32_t>> ids;
    std::vector<std::vector<float>> weights;
    std::vector<std::vector<uint32_t>> offsets(shape.ranks);
    for (uint32_t source = 0; source < shape.ranks; ++source) {
        ids.push_back(Ids(shape, source));
        weights.push_back(Weights(shape, source));
        std::vector<uint32_t> counts(shape.experts, 0U);
        for (int32_t expert : ids.back()) offsets[source].push_back(counts[expert]++);
    }
    for (uint32_t destination = 0; destination < shape.ranks; ++destination) {
        const auto rows = DispatchRows(shape, destination);
        Identity previous {};
        bool first = true;
        for (const auto &row : rows) {
            Check(row.source < shape.ranks && row.token < shape.bs && row.k < shape.k,
                "row identity stays within the current shape");
            const auto index = row.token * shape.k + row.k;
            Check(row.expert == uint32_t(ids[row.source][index]) && row.q == offsets[row.source][index],
                "row expert and offset match fresh source routes");
            Check(row.expert / (shape.experts / shape.ranks) == destination, "row reaches its expert owner");
            Check(seen.emplace(row.source, row.token, row.k).second, "each source route appears once");
            const Identity order(row.expert, row.source, row.q);
            Check(first || previous < order, "rows retain expert/source/q order");
            first = false;
            previous = order;
            Check(row.packet.token == row.token && !row.packet.members.empty(), "packet retains token identity");
            for (const auto &member : row.packet.members) {
                Check(member.k < shape.k, "packet member has a valid top-k position");
                const auto memberIndex = row.token * shape.k + member.k;
                Check(member.expert == uint32_t(ids[row.source][memberIndex]) &&
                    member.q == offsets[row.source][memberIndex] &&
                    FloatBits(member.weight) == FloatBits(weights[row.source][memberIndex]),
                    "packet members retain fresh route identities and exact weight bits");
            }
        }
    }
    Check(seen.size() == uint64_t(shape.ranks) * shape.bs * shape.k, "all current source routes are present");
}

int main()
{
    const Shape base {16, 64, 8, 128, 4, false, false, true};
    std::vector<Shape> shapes(7, base);
    shapes[0].ranks = 32;
    shapes[1].experts = 128;
    shapes[2].bs = 16;
    shapes[3].k = 8;
    shapes[4].routeMode = TileXREpDemo::RouteMode::Random;
    shapes[5].routeMode = TileXREpDemo::RouteMode::Balance;
    shapes[6].seed = 42;
    for (const auto &shape : shapes) {
        CheckRows(base);
        CheckRows(shape);
        CheckRows(shape);
        CheckRows(base);
    }
    return ServerDedupTest::Finish("reference");
}
