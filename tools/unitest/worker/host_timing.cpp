// Measures real public operator calls on the rank's stream with a local steady
// clock. Validation and serialization follow the end timestamp; failures escape
// to Worker's _Exit boundary and never publish a usable timing sample.
#include "host_timing.h"
#include "adapter.h"
#include "session.h"
#include <chrono>
#include <cmath>

namespace unitest {
HostTiming::HostTiming(const Json &options) {
    require(string(field(options, "schema")) == "tilexr.unitest.run.v1" &&
        string(field(options, "mode")) == "perf", "invalid Host batch run options");
    warmup_ = static_cast<int>(integer(options, "warmup", 0, 10000));
    iterations_ = static_cast<int>(integer(options, "iterations", 1, 10000));
    batches_ = static_cast<int>(integer(options, "batches", 1, 10000));
}
void HostTiming::warmup(Adapter &adapter, Session &session) {
    require(!warmed_, "duplicate Host warmup");
    adapter.reset(); session.sync();
    for (int index = 0; index < warmup_; ++index) adapter.launch();
    session.sync(); warmed_ = true;
}
void HostTiming::ready(Adapter &adapter, Session &session) {
    require(warmed_ && !ready_ && completed_ < batches_, "unexpected Host batch ready");
    adapter.reset(); session.sync(); ready_ = true;
}
Json HostTiming::measure(Adapter &adapter, Session &session) {
    require(ready_, "Host batch requires completed global ready phase");
    const auto start = std::chrono::steady_clock::now();
    for (int index = 0; index < iterations_; ++index) adapter.launch();
    session.sync();
    const auto end = std::chrono::steady_clock::now();
    const auto elapsed_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
    require(elapsed_ns > 0, "nonpositive Host batch duration");
    const double elapsed_us = static_cast<double>(elapsed_ns) / 1000.0;
    const double average_us = elapsed_us / iterations_;
    require(std::isfinite(average_us) && average_us > 0, "invalid Host batch average");
    ready_ = false;
    Json out = object();
    put(out, "batch", number(completed_++));
    put(out, "iterations", number(iterations_));
    put(out, "elapsed_ns", number(elapsed_ns));
    put(out, "elapsed_us", Json(elapsed_us));
    put(out, "host_batch_average_us", Json(average_us));
    return out;
}
} // namespace unitest
