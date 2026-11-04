// Executes one rank's controller-directed prepare/execute/validate/release phases.
// Synchronizes before validation and release; exceptions report failure then
// _Exit without unwinding Session resources whose completion may be unknown.
#include "adapter.h"
#include "protocol.h"
#include "session.h"
#include "reference.h"
#include "host_timing.h"
#include "external_profiler.h"
#include <cstdlib>
#include <iostream>
#include <signal.h>
#include <algorithm>
#include <cctype>

int main(int argc, char **argv) {
    using namespace unitest;
    // Private positional interface, used only by the installed controller.
    if (argc != 5) { std::cerr << "Worker requires rank device control-fd resolved-case\n"; return 2; }
    int rank = -1, fd = -1;
    std::string case_id, phase = "init";
    signal(SIGPIPE, SIG_IGN);
    try {
        rank = std::stoi(argv[1]);
        const int device = std::stoi(argv[2]);
        fd = std::stoi(argv[3]);
        const Json case_json = read_case(argv[4]);
        case_id = string(field(case_json, "id"));
        const Case c = parse_case(case_json);
        require(rank >= 0 && rank < c.world && device >= 0 && fd >= 0, "invalid Worker arguments");
        // Keep resources outside exception-driven destruction. On any failure,
        // process teardown owns resources whose completion state is unknown.
        Session *session = new Session(rank, c.world, device);
        Adapter *adapter = nullptr;
        reply(fd, rank, case_id, phase, "passed", session->info());
        bool executed = false, segment_ready = false;
        uint64_t completed = 0;
        const std::string case_path(argv[4]);
        const auto slash = case_path.find_last_of('/');
        const auto output_dir = slash == std::string::npos ? "" : case_path.substr(0, slash + 1);
        std::unique_ptr<HostTiming> host_timing;
        ExternalProfiler *external_profiler = nullptr;
        bool external_enabled = false, profiler_started = false, profiler_stopped = false;
        bool perf_run = false;
        bool validated = false;
        while (std::getline(std::cin, phase)) {
            Json data = object();
            if (phase == "prepare") {
                require(!adapter, "duplicate prepare");
                const Json run = read_case(output_dir + "run.json");
                // Reject unsupported external selection before any operator call.
                external_profiler = new ExternalProfiler(*session, run, output_dir);
                perf_run = string(field(run, "mode")) == "perf";
                const Json &profile = field(run, "profiling");
                external_enabled = field(profile, "enabled").get<bool>() &&
                    string(field(profile, "tool")) == "msprof";
                adapter = make_adapter(c, *session).release();
            } else if (phase == "profiler_start") {
                require(adapter && external_profiler && !profiler_started && !profiler_stopped,
                    "unexpected profiler start");
                require(!external_enabled || (perf_run ? host_timing != nullptr : !executed),
                    "profiler start must follow perf warmup or precede the first actual call");
                external_profiler->start(); profiler_started = true;
            } else if (phase == "profiler_stop") {
                require(external_profiler && profiler_started && !profiler_stopped &&
                    validated && !segment_ready, "profiler stop requires completed validated execution");
                data = external_profiler->stop(); profiler_stopped = true;
            } else if (phase == "execute") {
                require(adapter && !executed && !(external_enabled && profiler_stopped), "unexpected execute");
                require(!external_enabled || perf_run || profiler_started,
                    "actual execution requires external profiler start");
                validated = false;
                adapter->launch(); session->sync(); executed = true; ++completed;
            } else if (phase == "ready") {
                require(adapter && !segment_ready, "unexpected segment ready");
                session->sync(); segment_ready = true;
            } else if (phase.compare(0, 12, "run_segment ") == 0) {
                require(adapter && segment_ready && !(external_enabled && profiler_stopped),
                    "segment requires global ready phase before profiler stop");
                require(!external_enabled || profiler_started, "correctness segment requires external profiler start");
                validated = false;
                const std::string count = phase.substr(12);
                require(!count.empty() && count.size() <= 5 &&
                    std::all_of(count.begin(), count.end(), [](unsigned char ch) { return ch >= '0' && ch <= '9'; }),
                    "invalid segment count");
                const int iterations = std::stoi(count);
                require(iterations >= 1 && iterations <= 10000, "segment count outside 1..10000");
                for (int index = 0; index < iterations; ++index) adapter->launch();
                session->sync(); segment_ready = false; executed = true;
                completed += iterations;
                put(data, "iterations", number(iterations));
                put(data, "completed_iterations", number(static_cast<int64_t>(completed)));
            } else if (phase == "validate") {
                require(adapter && executed, "validate before execute");
                Json checks = adapter->validate(); bool passed = true;
                for (const auto &item : checks.get<Array>())
                    if (string(field(item, "status")) != "passed") passed = false;
                validated = passed;
                put(data, "checks", std::move(checks));
                reply(fd, rank, case_id, phase, passed ? "passed" : "mismatch", std::move(data)); continue;
            } else if (phase == "host_warmup") {
                require(adapter && executed && !host_timing, "warmup requires checked execution");
                host_timing.reset(new HostTiming(read_case(output_dir + "run.json")));
                require(!external_enabled || !profiler_started, "warmup must precede profiler start");
                validated = false;
                host_timing->warmup(*adapter, *session);
            } else if (phase == "host_ready") {
                require(host_timing != nullptr, "Host ready before warmup");
                host_timing->ready(*adapter, *session);
            } else if (phase == "host_batch") {
                require(host_timing != nullptr && (!external_enabled || (profiler_started && !profiler_stopped)),
                    "Host batch requires warmup and active external profiler when enabled");
                validated = false;
                data = host_timing->measure(*adapter, *session); executed = true;
            } else if (phase == "release") {
                require(adapter && (!external_enabled || profiler_stopped),
                    "release requires prepare and successful external profiler stop");
                delete external_profiler;
                delete adapter; session->close(); delete session;
                reply(fd, rank, case_id, phase, "passed", data); return 0;
            } else throw std::runtime_error("unexpected controller command: " + phase);
            reply(fd, rank, case_id, phase, "passed", std::move(data));
        }
        throw std::runtime_error("controller disconnected before release");
    } catch (const std::exception &error) {
        try {
            Json data = object(); put(data, "message", Json(std::string(error.what())));
            reply(fd, rank, case_id, phase,
                dynamic_cast<const UnsupportedError *>(&error) ? "unsupported" : "runtime_error", data);
        } catch (...) {}
        std::cerr << phase << ": " << error.what() << '\n';
        std::cerr.flush();
        std::_Exit(1);
    }
}
