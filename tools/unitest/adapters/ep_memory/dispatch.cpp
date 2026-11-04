// Prepares EP Memory Dispatch inputs, calls the public API and checks payload
// and routing metadata. Worker synchronizes execution; Session owns device buffers.
#include "adapter.h"
#include "comparator.h"
#include "session.h"
#include "stimulus.h"
#include <tilexr_ep.h>
#include <algorithm>

namespace unitest {
class Dispatch final : public Adapter {
public:
    Dispatch(const Case &c, Session &s)
        : c_(c), s_(s), expected_(dispatch_reference(c, s.rank)),
          x_(s.buffer(size_t(c.bs)*c.h*2)), ids_(s.buffer(size_t(c.bs)*c.topk*4)),
          payload_(s.buffer(std::max<size_t>(expected_.payload.size(), c.h)*2)),
          assist_(s.buffer(std::max<size_t>(expected_.assist.size(), 3)*4)),
          counts_(s.buffer(size_t(c.experts)*4)), experts_(s.buffer(size_t(c.experts/c.world)*8)) {
        const auto input = dispatch_input(c, s.rank);
        x_.upload(input.x); ids_.upload(input.ids);
    }
    void reset() override { payload_.poison(); assist_.poison(); counts_.poison(); experts_.poison(); }
    void launch() override {
        const auto dtype = TileXR::TILEXR_DATA_TYPE_BFP16;
        check(TileXRMoeEpDispatchMemory(x_.data(), static_cast<int32_t *>(ids_.data()),
            nullptr, nullptr, 0, nullptr, s_.comm, c_.bs, c_.h, c_.topk, c_.experts,
            c_.world, s_.rank, 1, 0, 0, 0, 0, 0, int64_t(c_.bs)*c_.world, 1,
            payload_.data(), nullptr, static_cast<int32_t *>(assist_.data()),
            static_cast<int64_t *>(experts_.data()), static_cast<int32_t *>(counts_.data()),
            nullptr, nullptr, dtype, dtype, s_.stream), "TileXRMoeEpDispatchMemory");
    }
    Json validate() override {
        Array checks{
            compare("expandXOut", payload_.read<uint16_t>(expected_.payload.size()), expected_.payload),
            compare("assistInfoForCombineOut", assist_.read<int32_t>(expected_.assist.size()), expected_.assist),
            compare("sendCountsOut", counts_.read<int32_t>(expected_.counts.size()), expected_.counts),
            compare("expertTokenNumsOut", experts_.read<int64_t>(expected_.expert_counts.size()), expected_.expert_counts)};
        return Json(checks);
    }
    Buffer &payload() { return payload_; }
    Buffer &assist() { return assist_; }
    Buffer &counts() { return counts_; }
private:
    Case c_;
    Session &s_;
    DispatchReference expected_;
    Buffer &x_, &ids_, &payload_, &assist_, &counts_, &experts_;
};
// Both stages borrow the same device buffers for their full lifetime. Combine
// consumes this round's actual Dispatch output on the same stream, without a
// Host snapshot or an intervening Host barrier.
class Chain final : public Adapter {
public:
    Chain(const Case &c, Session &s)
        : c_(c), s_(s), dispatch_(c, s), expected_(combine_reference(c, s.rank)),
          weights_(s.buffer(size_t(c.bs)*c.topk*4)), y_(s.buffer(size_t(c.bs)*c.h*2)) {
        std::vector<float> weights;
        for (int token = 0; token < c.bs; ++token)
            for (int k = 0; k < c.topk; ++k) weights.push_back(combine_weight(s.rank, token, k));
        weights_.upload(weights);
    }
    void reset() override { dispatch_.reset(); y_.poison(); }
    void launch() override {
        dispatch_.launch();
        check(TileXRMoeEpCombineMemory(dispatch_.payload().data(),
            static_cast<int32_t *>(dispatch_.assist().data()),
            static_cast<int32_t *>(dispatch_.counts().data()), static_cast<float *>(weights_.data()),
            nullptr, 0, nullptr, s_.comm, c_.bs, c_.h, c_.topk, c_.experts,
            c_.world, s_.rank, 1, 0, 0, 0, 0, 0, int64_t(c_.bs)*c_.world,
            y_.data(), TileXR::TILEXR_DATA_TYPE_BFP16, s_.stream), "TileXRMoeEpCombineMemory");
    }
    Json validate() override {
        return Json(Array{compare("combine.yOut", y_.read<uint16_t>(expected_.size()), expected_)});
    }
private:
    Case c_;
    Session &s_;
    Dispatch dispatch_;
    std::vector<uint16_t> expected_;
    Buffer &weights_, &y_;
};
std::unique_ptr<Adapter> make_chain(const Case &c, Session &s) {
    return std::unique_ptr<Adapter>(new Chain(c, s));
}
std::unique_ptr<Adapter> make_dispatch(const Case &c, Session &s) {
    return std::unique_ptr<Adapter>(new Dispatch(c, s));
}
} // namespace unitest
