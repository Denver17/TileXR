// Owns runtime Host batch counts and readiness, not device resources. Only the
// production launch loop and checked final sync are inside the measured window.
#pragma once
#include "json.h"

namespace unitest {
class Adapter;
class Session;
class HostTiming {
public:
    explicit HostTiming(const Json &options);
    void warmup(Adapter &adapter, Session &session);
    void ready(Adapter &adapter, Session &session);
    Json measure(Adapter &adapter, Session &session);
private:
    int warmup_, iterations_, batches_, completed_ = 0;
    bool warmed_ = false, ready_ = false;
};
} // namespace unitest
