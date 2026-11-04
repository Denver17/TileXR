// Validates Worker cases and computes deterministic routing, BF16 values and
// independent Dispatch/Combine expectations entirely on Host, without NPU calls.
#include "reference.h"
#include <cstring>
#include <set>

namespace unitest {
Case parse_case(const Json &value) {
    require(string(field(value, "schema")) == "tilexr.unitest.case.v1", "unsupported case schema");
    require(string(field(value, "backend")) == "memory", "unsupported backend");
    require(string(field(value, "dtype")) == "bf16", "only BF16 is supported");
    require(integer(value, "quant_mode", 0, 0) == 0, "quantization unsupported");
    Case c;
    c.id = string(field(value, "id"));
    require(!c.id.empty() && c.id.size() <= 128, "invalid case id");
    const Json &steps = field(value, "steps");
    require(steps.is<Array>() && !steps.get<Array>().empty() && steps.get<Array>().size() <= 2,
        "steps must contain one registered operator or a supported two-step chain");
    std::set<std::string> ids;
    for (const Json &step : steps.get<Array>()) {
        const std::string id = string(field(step, "id")), op = string(field(step, "operator"));
        require(!id.empty() && id.size() <= 64 && id.find_first_not_of(
            "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_") == std::string::npos,
            "invalid step id");
        require(ids.insert(id).second, "duplicate step id");
        require(op == "ep_dispatch" || op == "ep_combine" || op == "barrier", "unregistered step operator");
        const auto &fields = step.get<Object>();
        for (const auto &entry : fields)
            require(entry.first == "id" || entry.first == "operator" || entry.first == "inputs",
                "unknown step field: " + entry.first);
        c.steps.push_back({id, op});
    }
    const auto &first = c.steps.front();
    require(first.op == "ep_dispatch" || first.op == "ep_combine", "first step must be Dispatch or Combine");
    if (steps.get<Array>().front().contains("inputs")) {
        const Json &inputs = field(steps.get<Array>().front(), "inputs");
        require(inputs.is<Object>() && inputs.get<Object>().empty(), "first step inputs come from Host stimulus");
    }
    c.op = first.op;
    c.validation = string(field(value, "validate"));
    if (c.steps.size() == 2) {
        require(first.op == "ep_dispatch", "only Dispatch may feed a second registered step");
        const auto &second = c.steps[1];
        require(second.op == "ep_combine" || second.op == "barrier", "unsupported ordered step pair");
        if (second.op == "barrier")
            throw UnsupportedError("device Barrier is unsupported: installed public TileXR APIs expose no stream Barrier entry");
        const Json &bindings = field(steps.get<Array>()[1], "inputs");
        require(bindings.is<Object>() && bindings.get<Object>().size() == 3, "Combine requires payload/assist/counts bindings");
        for (const char *name : {"payload", "assist", "counts"})
            require(string(field(bindings, name)) == first.id + "." + name,
                "Combine input must bind the preceding Dispatch output");
        c.op = "ep_dispatch_combine";
    }
    require(c.validation == c.steps.back().id + ".output", "validate must select the final step output");
    if (value.contains("operator"))
        require(string(field(value, "operator")) == c.op, "operator disagrees with registered steps");
    c.world = static_cast<int>(integer(value, "world_size", 2, 128));
    c.bs = static_cast<int>(integer(value, "bs", 1, 256));
    c.h = static_cast<int>(integer(value, "h", 16, 4096));
    c.topk = static_cast<int>(integer(value, "topk", 1, 8));
    c.experts = static_cast<int>(integer(value, "moe_expert_num", c.topk, 128));
    c.seed = static_cast<uint32_t>(integer(value, "seed", 0, UINT32_MAX));
    require(c.h % 16 == 0, "H must be divisible by 16");
    require(c.experts % c.world == 0 && c.experts / c.world <= 32,
        "experts must divide across ranks with at most 32 per rank");
    // Dense routes allow standalone Combine without a test-owned device barrier.
    require(c.op == "ep_dispatch" || c.bs*c.topk >= c.experts,
        "Combine requires BS*TopK >= experts for full peer coverage");
    return c;
}
int expert_id(const Case &c, int source, int token, int k) {
    return static_cast<int>((uint64_t(token)*c.topk + k + source + c.seed) % c.experts);
}
uint16_t dispatch_value(const Case &c, int source, int token, int hidden) {
    uint32_t x = c.seed ^ (0x9e3779b9U * (source + 1U));
    x ^= 0x85ebca6bU * (token + 1U);
    x ^= 0xc2b2ae35U * (hidden + 1U);
    x ^= x >> 16; x *= 0x7feb352dU; x ^= x >> 15; x *= 0x846ca68bU; x ^= x >> 16;
    return static_cast<uint16_t>((x & 0x807fU) | (uint32_t(120 + ((x >> 7) % 15)) << 7));
}
float combine_value(const Case &c, int source, int token, int k, int hidden) {
    uint32_t x = c.seed ^ (0x9e3779b9U * (source + 1U));
    x ^= 0x85ebca6bU * (token + 1U);
    x ^= 0xc2b2ae35U * (k + 1U);
    x ^= 0x27d4eb2dU * (expert_id(c, source, token, k) + 1U);
    x ^= 0x165667b1U * (hidden + 1U);
    x ^= x >> 16; x *= 0x7feb352dU; x ^= x >> 15; x *= 0x846ca68bU; x ^= x >> 16;
    return float(int(x % 17U) - 8);
}
float combine_weight(int source, int token, int k) {
    static const float weights[] = {0.0f, 0.25f, -0.5f, 1.0f, 0.5f, -1.0f, 0.75f};
    return weights[(source*3 + token*2 + k) % 7];
}
uint16_t bf16(float value) {
    uint32_t bits;
    std::memcpy(&bits, &value, sizeof(bits));
    return static_cast<uint16_t>((bits + 0x7fffU + ((bits >> 16) & 1U)) >> 16);
}
std::vector<Route> routes(const Case &c, int destination) {
    std::vector<Route> out;
    const int local = c.experts/c.world;
    for (int e = destination*local; e < (destination+1)*local; ++e)
        for (int source = 0; source < c.world; ++source)
            for (int token = 0; token < c.bs; ++token)
                for (int k = 0; k < c.topk; ++k)
                    if (expert_id(c, source, token, k) == e) out.push_back({source, token, k});
    return out;
}
DispatchReference dispatch_reference(const Case &c, int destination) {
    DispatchReference out;
    int64_t total = 0;
    const int local = c.experts/c.world;
    // Enumerate public expert/source/token/TopK order without production layouts.
    for (int e = destination*local; e < (destination+1)*local; ++e) {
        int64_t expert_total = 0;
        for (int source = 0; source < c.world; ++source) {
            for (int token = 0; token < c.bs; ++token)
                for (int k = 0; k < c.topk; ++k) {
                    if (expert_id(c, source, token, k) != e) continue;
                    out.assist.insert(out.assist.end(), {source, token, k});
                    for (int h = 0; h < c.h; ++h)
                        out.payload.push_back(dispatch_value(c, source, token, h));
                    ++total; ++expert_total;
                }
            out.counts.push_back(static_cast<int32_t>(total));
        }
        out.expert_counts.push_back(expert_total);
    }
    return out;
}
std::vector<uint16_t> combine_reference(const Case &c, int token_rank) {
    std::vector<uint16_t> out;
    for (int token = 0; token < c.bs; ++token)
        for (int h = 0; h < c.h; ++h) {
            float sum = 0;
            // Work from original token choices, independently of packed inputs.
            for (int k = 0; k < c.topk; ++k)
                {
                    float input = combine_value(c, token_rank, token, k, h);
                    if (c.op == "ep_dispatch_combine") {
                        const uint32_t bits = uint32_t(dispatch_value(c, token_rank, token, h)) << 16;
                        std::memcpy(&input, &bits, sizeof(input));
                    }
                    sum += input*combine_weight(token_rank, token, k);
                }
            out.push_back(bf16(sum));
        }
    return out;
}
} // namespace unitest
