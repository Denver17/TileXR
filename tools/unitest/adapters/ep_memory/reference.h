// Declares the supported case model, deterministic routing/value helpers and
// Host reference outputs used by the EP Memory adapters.
#pragma once
#include "json.h"
#include <vector>

namespace unitest {
struct Step { std::string id, op; };
struct Case {
    std::string id, op, validation;
    std::vector<Step> steps;
    int world, bs, h, topk, experts;
    uint32_t seed;
};
Case parse_case(const Json &value);
int expert_id(const Case &c, int source, int token, int k);
uint16_t dispatch_value(const Case &c, int source, int token, int hidden);
float combine_value(const Case &c, int source, int token, int k, int hidden);
float combine_weight(int source, int token, int k);
uint16_t bf16(float value);
struct Route { int source, token, k; };
std::vector<Route> routes(const Case &c, int destination);
struct DispatchReference {
    std::vector<uint16_t> payload;
    std::vector<int32_t> assist, counts;
    std::vector<int64_t> expert_counts;
};
DispatchReference dispatch_reference(const Case &c, int destination);
std::vector<uint16_t> combine_reference(const Case &c, int token_rank);
} // namespace unitest
