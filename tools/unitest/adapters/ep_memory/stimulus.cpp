// Builds deterministic Host inputs for Dispatch and standalone Combine.
// Combine payload and metadata are synthesized without running production Dispatch.
#include "stimulus.h"
namespace unitest {
DispatchInput dispatch_input(const Case &c, int rank) {
    DispatchInput out;
    for (int token = 0; token < c.bs; ++token) {
        for (int h = 0; h < c.h; ++h) out.x.push_back(dispatch_value(c, rank, token, h));
        for (int k = 0; k < c.topk; ++k) out.ids.push_back(expert_id(c, rank, token, k));
    }
    return out;
}
CombineInput combine_input(const Case &c, int rank) {
    CombineInput out;
    const auto packed = routes(c, rank);
    for (const auto &route : packed) {
        out.assist.insert(out.assist.end(), {route.source, route.token, route.k});
        for (int h = 0; h < c.h; ++h)
            out.expert_out.push_back(bf16(combine_value(c, route.source, route.token, route.k, h)));
    }
    // Cumulative expert/source counts are synthesized without running Dispatch.
    int32_t total = 0;
    const int local = c.experts/c.world;
    for (int e = rank*local; e < (rank+1)*local; ++e)
        for (int source = 0; source < c.world; ++source) {
            for (int token = 0; token < c.bs; ++token)
                for (int k = 0; k < c.topk; ++k)
                    if (expert_id(c, source, token, k) == e) ++total;
            out.counts.push_back(total);
        }
    for (int token = 0; token < c.bs; ++token)
        for (int k = 0; k < c.topk; ++k) out.weights.push_back(combine_weight(rank, token, k));
    return out;
}
} // namespace unitest
