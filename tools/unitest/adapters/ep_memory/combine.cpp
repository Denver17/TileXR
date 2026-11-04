// Implements EP Memory Combine validation and selects the requested adapter.
// Inputs are synthesized without Dispatch; Worker synchronizes the public API
// launch before output checks, and Session owns the device buffers.
#include "adapter.h"
#include "comparator.h"
#include "session.h"
#include "stimulus.h"
#include <tilexr_ep.h>
#include <algorithm>

namespace unitest {
class Combine final : public Adapter {
public:
    Combine(const Case &c, Session &s)
        : c_(c), s_(s), expected_(combine_reference(c, s.rank)),
          expert_(s.buffer(std::max<size_t>(routes(c, s.rank).size(), 1)*c.h*2)),
          assist_(s.buffer(std::max<size_t>(routes(c, s.rank).size(), 1)*12)),
          counts_(s.buffer(size_t(c.experts)*4)), weights_(s.buffer(size_t(c.bs)*c.topk*4)),
          y_(s.buffer(size_t(c.bs)*c.h*2)) {
        const auto input = combine_input(c, s.rank);
        expert_.upload(input.expert_out); assist_.upload(input.assist);
        counts_.upload(input.counts); weights_.upload(input.weights);
    }
    void reset() override { y_.poison(); }
    void launch() override {
        check(TileXRMoeEpCombineMemory(expert_.data(), static_cast<int32_t *>(assist_.data()),
            static_cast<int32_t *>(counts_.data()), static_cast<float *>(weights_.data()),
            nullptr, 0, nullptr, s_.comm, c_.bs, c_.h, c_.topk, c_.experts,
            c_.world, s_.rank, 1, 0, 0, 0, 0, 0, int64_t(c_.bs)*c_.world,
            y_.data(), TileXR::TILEXR_DATA_TYPE_BFP16, s_.stream), "TileXRMoeEpCombineMemory");
    }
    Json validate() override {
        // Integer inputs and quarter weights make the bounded sum exactly
        // representable in BF16, so bit equality needs no arbitrary epsilon.
        return Json(Array{compare("yOut", y_.read<uint16_t>(expected_.size()), expected_)});
    }
private:
    Case c_;
    Session &s_;
    std::vector<uint16_t> expected_;
    Buffer &expert_, &assist_, &counts_, &weights_, &y_;
};
std::unique_ptr<Adapter> make_dispatch(const Case &, Session &);
std::unique_ptr<Adapter> make_chain(const Case &, Session &);
std::unique_ptr<Adapter> make_adapter(const Case &c, Session &s) {
    if (c.op == "ep_dispatch_combine") return make_chain(c, s);
    if (c.op == "ep_dispatch") return make_dispatch(c, s);
    require(c.op == "ep_combine", "unsupported operator");
    return std::unique_ptr<Adapter>(new Combine(c, s));
}
} // namespace unitest
