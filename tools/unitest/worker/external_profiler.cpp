// Thin official collector adapter: no operator launches, timing calculations,
// per-round toggles, stream synchronization, or profiler-output decoding.
#include "external_profiler.h"
#include "session.h"
#include <acl/acl_prof.h>
#include <dlfcn.h>
#include <sys/stat.h>
#include <cerrno>
#include <cstring>

namespace unitest {
namespace {
template<class Function> Function symbol(void *library, const char *name) {
    auto function = reinterpret_cast<Function>(dlsym(library, name));
    if (!function) throw UnsupportedError(std::string("official profiler API unavailable: ") + name);
    return function;
}
void checked(aclError code, const char *name) {
    require(code == ACL_SUCCESS, std::string(name) + " returned " + std::to_string(code));
}
}
struct ExternalProfiler::Impl {
    bool enabled = false, started = false, stopped = false;
    void *library = nullptr;
    aclprofConfig *config = nullptr;
    std::string raw_dir, library_path;
    Array calls;
    decltype(&aclprofInit) initialize = nullptr;
    decltype(&aclprofCreateConfig) create_config = nullptr;
    decltype(&aclprofStart) start = nullptr;
    decltype(&aclprofStop) stop = nullptr;
    decltype(&aclprofDestroyConfig) destroy_config = nullptr;
    decltype(&aclprofFinalize) finalize = nullptr;

    Impl(Session &session, const Json &run, const std::string &output) {
        const Json &profile = field(run, "profiling");
        const Json &flag = field(profile, "enabled");
        require(flag.is<bool>(), "profiling.enabled must be boolean");
        enabled = flag.get<bool>() && string(field(profile, "tool")) == "msprof";
        if (!enabled) return;
        if (string(field(field(profile, "selection"), "kind")) != "all")
            throw UnsupportedError("msprof ACLprof collection supports only all measured rounds; selected rounds are not supported in this version");
        if (profile.contains("options")) {
            const Json &options = field(profile, "options");
            require(options.is<Object>(), "profiling.options must be an object");
            if (!options.get<Object>().empty())
                throw UnsupportedError("msprof ACLprof collector does not support custom collection options");
        }
        raw_dir = output + "/msprof-rank-" + std::to_string(session.rank);
        require(mkdir(raw_dir.c_str(), 0700) == 0,
            "cannot create fresh profiler output directory: " + raw_dir + ": " + std::strerror(errno));
        library = dlopen("libmsprofiler.so", RTLD_NOW | RTLD_LOCAL);
        if (!library) throw UnsupportedError("official libmsprofiler.so is unavailable");
        initialize = symbol<decltype(initialize)>(library, "aclprofInit");
        create_config = symbol<decltype(create_config)>(library, "aclprofCreateConfig");
        start = symbol<decltype(start)>(library, "aclprofStart");
        stop = symbol<decltype(stop)>(library, "aclprofStop");
        destroy_config = symbol<decltype(destroy_config)>(library, "aclprofDestroyConfig");
        finalize = symbol<decltype(finalize)>(library, "aclprofFinalize");
        Dl_info identity{};
        if (dladdr(reinterpret_cast<void *>(initialize), &identity) && identity.dli_fname)
            library_path = identity.dli_fname;
        checked(initialize(raw_dir.c_str(), raw_dir.size()), "aclprofInit");
        calls.emplace_back("aclprofInit");
        uint32_t device = static_cast<uint32_t>(session.device);
        config = create_config(&device, 1U, ACL_AICORE_NONE, nullptr, ACL_PROF_TASK_TIME);
        require(config != nullptr, "aclprofCreateConfig returned null");
        calls.emplace_back("aclprofCreateConfig");
    }
};
ExternalProfiler::ExternalProfiler(Session &session, const Json &run, const std::string &output)
    : impl_(new Impl(session, run, output)) {}
// Raw CANN handles intentionally have no destructor cleanup. Normal release must
// call stop(); exceptions go directly to Worker _Exit before Session destruction.
ExternalProfiler::~ExternalProfiler() { delete impl_; }
void ExternalProfiler::start() {
    auto &p = *impl_;
    if (!p.enabled) return;
    require(!p.started && !p.stopped, "profiler start must occur exactly once");
    checked(p.start(p.config), "aclprofStart");
    p.calls.emplace_back("aclprofStart");
    p.started = true;
}
Json ExternalProfiler::stop() {
    auto &p = *impl_;
    Json result = object();
    put(result, "enabled", Json(p.enabled));
    if (!p.enabled) return result;
    require(p.started && !p.stopped, "profiler stop requires active collection");
    // Caller has already completed its normal synchronization and validation.
    checked(p.stop(p.config), "aclprofStop");
    p.calls.emplace_back("aclprofStop");
    checked(p.destroy_config(p.config), "aclprofDestroyConfig");
    p.config = nullptr;
    p.calls.emplace_back("aclprofDestroyConfig");
    checked(p.finalize(), "aclprofFinalize");
    p.calls.emplace_back("aclprofFinalize");
    p.stopped = true;
    put(result, "status", Json(std::string("collected")));
    put(result, "tool", Json(std::string("msprof")));
    put(result, "raw_output_dir", Json(p.raw_dir));
    put(result, "loaded_library", Json(p.library_path));
    put(result, "official_api_calls", Json(p.calls));
    put(result, "version", Json());
    put(result, "version_status", Json(std::string("not_queried; record external msprof tool version during export")));
    put(result, "collection_completeness", Json(std::string("unverified")));
    put(result, "requires_official_export", Json(true));
    return result;
}
} // namespace unitest
