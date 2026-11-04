// Owns one rank's ACL device, stream, TileXR communicator and buffers.
// Success releases resources after synchronized validation. On failure the Worker
// uses _Exit, avoiding unwinding resources whose completion state is unknown.
#include "session.h"
#include <tilexr_ep.h>
#include <algorithm>
#include <cctype>
#include <dlfcn.h>
#include <limits.h>
#include <cstdlib>

namespace unitest {
Session::Session(int r, int w, int d) : rank(r), world(w), device(d) {
    check(aclInit(nullptr), "aclInit");
    check(aclrtSetDevice(device), "aclrtSetDevice");
    const char *name = aclrtGetSocName();
    soc_ = name ? name : "";
    std::string lower = soc_;
    std::transform(lower.begin(), lower.end(), lower.begin(),
        [](unsigned char c) { return std::tolower(c); });
    if (!(lower.find("ascend950") == 0 || lower.find("ascend910_9") == 0 ||
          lower.find("a5") == 0))
        throw UnsupportedError("EP requires A5/Ascend950; detected " + soc_);
    check(aclrtCreateStream(&stream), "aclrtCreateStream");
    check(TileXRCommInitRankLocal(world, rank, &comm), "TileXRCommInitRankLocal");
}
Buffer &Session::buffer(size_t bytes) {
    buffers_.emplace_back(new Buffer(bytes));
    return *buffers_.back();
}
void Session::sync() { check(aclrtSynchronizeStream(stream), "aclrtSynchronizeStream"); }
void Session::close() {
    // Called only after all ranks completed validation. Failed Workers _Exit,
    // avoiding unsafe frees or destructor-driven drains after uncertain launches.
    sync();
    for (auto &buffer : buffers_) buffer->release();
    buffers_.clear();
    check(TileXRCommDestroy(comm), "TileXRCommDestroy"); comm = nullptr;
    check(aclrtDestroyStream(stream), "aclrtDestroyStream"); stream = nullptr;
    check(aclrtResetDevice(device), "aclrtResetDevice");
    check(aclFinalize(), "aclFinalize");
}
Json Session::info() const {
    Json out = object(), libraries = object();
    put(out, "soc", Json(soc_));
    for (const char *symbol : {"aclInit", "TileXRCommInitRankLocal", "TileXRMoeEpDispatchMemory",
                              "TileXRMoeEpCombineMemory"}) {
        Dl_info info{};
        void *address = dlsym(RTLD_DEFAULT, symbol);
        require(address && dladdr(address, &info) && info.dli_fname, "cannot identify loaded library");
        char resolved[PATH_MAX];
        require(realpath(info.dli_fname, resolved), "cannot resolve loaded library path");
        put(libraries, symbol, Json(std::string(resolved)));
    }
    put(out, "loaded_libraries", libraries);
    return out;
}
} // namespace unitest
