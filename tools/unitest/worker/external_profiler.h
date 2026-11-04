// Owns official ACLprof collection boundaries outside Host timing. Successful
// stop() closes the collector; failures require Worker _Exit, never RAII teardown
// of profiler state that may still be interacting with device execution.
#pragma once
#include "json.h"
#include <string>

namespace unitest {
class Session;
class ExternalProfiler {
public:
    ExternalProfiler(Session &session, const Json &run_options, const std::string &output_dir);
    ~ExternalProfiler();
    ExternalProfiler(const ExternalProfiler &) = delete;
    ExternalProfiler &operator=(const ExternalProfiler &) = delete;
    void start();
    Json stop();
private:
    struct Impl;
    Impl *impl_;
};
} // namespace unitest
