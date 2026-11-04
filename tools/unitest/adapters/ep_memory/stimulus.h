// Declares Host input containers and builders for the EP Memory public APIs.
#pragma once
#include "reference.h"
namespace unitest {
struct DispatchInput {
    std::vector<uint16_t> x;
    std::vector<int32_t> ids;
};
struct CombineInput {
    std::vector<uint16_t> expert_out;
    std::vector<int32_t> assist, counts;
    std::vector<float> weights;
};
DispatchInput dispatch_input(const Case &c, int rank);
CombineInput combine_input(const Case &c, int rank);
} // namespace unitest
