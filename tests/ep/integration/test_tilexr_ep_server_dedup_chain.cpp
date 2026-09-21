#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <fstream>
#include <set>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include "acl/acl.h"
#include "tilexr_ep.h"
#include "ep_memory_server_dedup_count_layout.h"
#include "ep_memory_layout.h"
#include "ep_memory_server_dedup_layout.h"
#include "ep_memory_server_dedup_combine_host.h"
#include "ep_memory_server_dedup_combine_config.h"
#include "tilexr_profiling.h"
#include "../demo/tilexr_ep_expert_ids.h"
#include "server_dedup_dispatch_reference.h"
#include "server_dedup_combine_m4_reference.h"
#include <map>
#ifdef TILEXR_EP_MEMORY_BOUNDARY_TEST
#include "memory_boundary_test.h"
#endif
#ifdef TILEXR_EP_COMBINE_MEMORY_BOUNDARY_TEST
#include "combine_memory_boundary_test.h"
#endif

namespace {
using Clock = std::chrono::steady_clock;
using Deadline = Clock::time_point;
// Fixed validation capacity through Dispatch/Combine Server Dedup qualification.
constexpr uint64_t kServerDedupPeerBytes = 8ULL * 1024U * 1024U * 1024U;
constexpr int kServerDedupDataMiB = static_cast<int>(
    (kServerDedupPeerBytes - TileXR::TILEXR_FLAG_BUFF_BYTES) / (1024 * 1024));

void Require(bool condition, const std::string &message)
{
    if (!condition) { throw std::runtime_error(message); }
}

struct OutputValidationFailure : std::runtime_error {
    using std::runtime_error::runtime_error;
};

void ValidateOutput(bool condition, const std::string &message)
{
    if (!condition) { throw OutputValidationFailure(message); }
}

void Check(int code, const char *operation)
{
    Require(code == 0, std::string(operation) + " ret=" + std::to_string(code));
}

template <typename T>
void CheckCountValue(const char *name, const std::vector<T> &actual, size_t index,
    int64_t expected, int64_t globalExpert, int sourceRank = -1)
{
    if (actual[index] == expected) { return; }
    const size_t begin = index > 2 ? index - 2 : 0;
    const size_t end = std::min(actual.size(), index + 3);
    std::ostringstream error;
    error << name << " mismatch at " << index << " expected=" << expected
        << " actual=" << actual[index] << " global_expert=" << globalExpert;
    if (sourceRank >= 0) { error << " source_rank=" << sourceRank; }
    error << " actual_window_start=" << begin << " actual_window=[";
    for (size_t i = begin; i < end; ++i) {
        if (i != begin) { error << ','; }
        error << actual[i];
    }
    error << ']';
    throw OutputValidationFailure(error.str());
}

int64_t Number(const char *text, const char *name)
{
    Require(text != nullptr && *text != '\0', std::string("missing ") + name);
    for (const char *p = text; *p != '\0'; ++p) {
        Require(*p >= '0' && *p <= '9', std::string("invalid ") + name);
    }
    errno = 0;
    const long long value = std::strtoll(text, nullptr, 10);
    Require(errno == 0, std::string("overflow ") + name);
    return value;
}

struct Config {
    std::string run, build, commit, host, checkpoint, runMode;
    int hostIndex, localRank, device, rank, rankSize;
    uint32_t loops = 1;
    uint32_t warmup = 0;
    TileXREpDemo::RouteMode routeMode = TileXREpDemo::RouteMode::BalanceShift;
    uint32_t seed = 2026;
    int64_t bs, h, topK, experts;
    TileXR::TileXRDataType dtype;
    int64_t quantMode = 0; // Dispatch; retain the legacy configuration alias.
    int64_t combineQuantMode = 0;
    std::string operatorMode = "dispatch";
    std::string combineFixture = "balanced";
    std::string combineObservation = "batch";
    bool dryRun = false;
    bool boundaryProbe = false;
    std::string fp8Format = "e4m3";
    sockaddr_in barrier {};
};

sockaddr_in Endpoint(const char *name)
{
    const char *value = std::getenv(name);
    Require(value != nullptr, std::string("missing ") + name);
    const std::string text(value);
    const auto colon = text.find(':');
    Require(colon != std::string::npos, std::string("expected IPv4:port for ") + name);
    const int64_t port = Number(text.substr(colon + 1).c_str(), name);
    sockaddr_in result {};
    result.sin_family = AF_INET;
    Require(port > 0 && port <= 65535 &&
        inet_pton(AF_INET, text.substr(0, colon).c_str(), &result.sin_addr) == 1,
        std::string("invalid IPv4:port for ") + name);
    result.sin_port = htons(static_cast<uint16_t>(port));
    return result;
}

Config Parse(int argc, char **argv)
{
    Config c {};
    if (argc > 1 && std::strcmp(argv[1], "--dry-run") == 0) {
        c.dryRun = true;
        --argc; ++argv;
    }
    Require(argc == 10, "expected [--dry-run] run build commit host host_index local_rank device rank rank_size");
    c.run = argv[1]; c.build = argv[2]; c.commit = argv[3]; c.host = argv[4];
    const char *checkpoint = std::getenv("TILEXR_VALIDATION_CHECKPOINT");
    c.checkpoint = checkpoint == nullptr ? "ep-sdedup-p2-dispatch" : checkpoint;
    Require(c.checkpoint == "ep-sdedup-p2-dispatch" ||
        c.checkpoint == "ep-sdedup-p1-memory-boundary" ||
        c.checkpoint == "ep-sdedup-combine-p1-dry-run" ||
        c.checkpoint == "ep-sdedup-combine-p1-chain-single" ||
        c.checkpoint == "ep-sdedup-combine-p1-chain-multi" ||
        c.checkpoint == "ep-sdedup-combine-p1-profile" ||
        c.checkpoint == "ep-sdedup-combine-p1-memory-boundary" ||
        c.checkpoint == "ep-sdedup-combine-p2-m12" ||
        c.checkpoint == "ep-sdedup-combine-p2-m3" || c.checkpoint == "ep-sdedup-combine-p2-m4" || c.checkpoint == "ep-sdedup-combine-p2-m5",
        "unsupported TILEXR_VALIDATION_CHECKPOINT");
    if (const char *probe = std::getenv("TILEXR_VALIDATION_BOUNDARY_PROBE")) {
        Require(std::strcmp(probe, "0") == 0 || std::strcmp(probe, "1") == 0,
            "boundary_probe must be 0/1");
        c.boundaryProbe = std::strcmp(probe, "1") == 0;
    }
    Require(!c.boundaryProbe || c.checkpoint == "ep-sdedup-combine-p1-memory-boundary",
        "boundary_probe requires C7 checkpoint");
    Require(c.checkpoint != "ep-sdedup-combine-p1-dry-run" || c.dryRun,
        "Combine C3 requires --dry-run");
    if (const char *op = std::getenv("TILEXR_VALIDATION_OPERATOR_MODE")) c.operatorMode = op;
    Require(c.operatorMode == "dispatch" || c.operatorMode == "combine" || c.operatorMode == "both",
        "operator_mode must be dispatch/combine/both");
    Require(c.dryRun || c.operatorMode == "dispatch" ||
        c.checkpoint == "ep-sdedup-combine-p1-chain-single" ||
        c.checkpoint == "ep-sdedup-combine-p1-chain-multi" ||
        c.checkpoint == "ep-sdedup-combine-p1-profile" ||
        c.checkpoint == "ep-sdedup-combine-p1-memory-boundary" ||
        c.checkpoint == "ep-sdedup-combine-p2-m12" ||
        c.checkpoint == "ep-sdedup-combine-p2-m3" || c.checkpoint == "ep-sdedup-combine-p2-m4" || c.checkpoint == "ep-sdedup-combine-p2-m5",
        "Combine device execution requires a Combine acceptance checkpoint");
    if (const char *value = std::getenv("TILEXR_VALIDATION_COMBINE_FIXTURE")) c.combineFixture = value;
    if (const char *value = std::getenv("TILEXR_VALIDATION_COMBINE_OBSERVATION")) c.combineObservation = value;
    Require(c.combineFixture == "balanced" || c.combineFixture == "sparse" || c.combineFixture == "empty",
        "combine_fixture must be balanced/sparse/empty");
    Require(c.combineObservation == "batch" || c.combineObservation == "per-round",
        "combine_observation must be batch/per-round");
    Require(c.combineFixture == "balanced" || (c.checkpoint == "ep-sdedup-combine-p2-m12" && c.operatorMode == "combine"),
        "nonbalanced Combine fixtures require P2.1 combine-only");
    const char *mode = std::getenv("TILEXR_VALIDATION_RUN_MODE");
    c.runMode = mode == nullptr ? "functional" : mode;
    Require(c.runMode == "functional" || c.runMode == "performance" || c.runMode == "breakdown",
        "unsupported TILEXR_VALIDATION_RUN_MODE");
    Require(c.combineObservation == "batch" || (c.checkpoint == "ep-sdedup-combine-p2-m12" &&
        c.runMode == "functional" && c.operatorMode != "dispatch"), "per-round observation requires P2.1 functional Combine");
    const char *loopsText = std::getenv("TILEXR_VALIDATION_LOOPS");
    const char *warmupText = std::getenv("TILEXR_VALIDATION_WARMUP");
    const int64_t loops = loopsText == nullptr ? 1 :
        Number(loopsText, "TILEXR_VALIDATION_LOOPS");
    const int64_t warmup = warmupText == nullptr ? 0 :
        Number(warmupText, "TILEXR_VALIDATION_WARMUP");
    Require(loops > 0 && loops <= UINT32_MAX, "TILEXR_VALIDATION_LOOPS must be in [1, UINT32_MAX]");
    Require(warmup <= UINT32_MAX, "TILEXR_VALIDATION_WARMUP must be in [0, UINT32_MAX]");
    c.loops = static_cast<uint32_t>(loops);
    c.warmup = static_cast<uint32_t>(warmup);
    Require((c.checkpoint != "ep-sdedup-combine-p2-m3" && c.checkpoint != "ep-sdedup-combine-p2-m4") ||
        (c.loops == 1 && c.warmup == 0 && c.combineObservation == "batch"),
        "M3/M4 require loops=1, warmup=0 and batch observation in every mode");
    Require(c.dryRun || c.checkpoint != "ep-sdedup-combine-p1-chain-single" ||
        (c.runMode == "functional" && c.loops == 1 && c.warmup == 0),
        "C4 requires functional mode, loops=1, warmup=0");
    Require(c.dryRun || c.checkpoint != "ep-sdedup-combine-p1-chain-multi" ||
        c.runMode == "functional", "C5 requires functional mode");
    Require(c.checkpoint != "ep-sdedup-combine-p1-memory-boundary" || c.runMode == "functional",
        "C7 requires functional mode");
    Require(!c.boundaryProbe || (!c.dryRun && c.loops == 1 && c.warmup == 0),
        "C7 boundary probe requires real functional mode, loops=1, warmup=0");
    if (const char *routeMode = std::getenv("TILEXR_VALIDATION_ROUTE_MODE"))
        c.routeMode = TileXREpDemo::ParseRouteMode(routeMode);
    if (const char *seedText = std::getenv("TILEXR_VALIDATION_SEED")) {
        Require(std::strlen(seedText) <= 10 && (seedText[0] != '0' || seedText[1] == '\0'),
            "TILEXR_VALIDATION_SEED must be canonical unsigned decimal");
        const int64_t seed = Number(seedText, "TILEXR_VALIDATION_SEED");
        Require(seed <= UINT32_MAX, "TILEXR_VALIDATION_SEED must be in [0, UINT32_MAX]");
        c.seed = static_cast<uint32_t>(seed);
    }
    Require(c.checkpoint != "ep-sdedup-p1-memory-boundary" ||
        (c.runMode == "functional" && c.loops == 1 && c.warmup == 0),
        "memory-boundary requires functional mode, loops=1, warmup=0");
    Require(!c.run.empty() && c.run.size() < 128 && !c.build.empty() && c.build.size() < 128 &&
        !c.host.empty(), "invalid run/build/host identity");
    int *fields[] = {&c.hostIndex, &c.localRank, &c.device, &c.rank, &c.rankSize};
    for (int i = 0; i < 5; ++i) {
        const int64_t value = Number(argv[i + 5], "rank identity");
        Require(value <= 255, "rank/device identity out of range");
        *fields[i] = static_cast<int>(value);
    }
    Require(c.rankSize >= 16 && c.rankSize <= 128 && c.rankSize % 8 == 0 && c.localRank < 8 &&
        c.rank < c.rankSize && c.rank == c.hostIndex * 8 + c.localRank, "invalid 8P/server mapping");
    Require(!c.dryRun || c.device == c.localRank, "dry-run requires device == local_rank");
    c.bs = Number(std::getenv("TILEXR_VALIDATION_BS"), "TILEXR_VALIDATION_BS");
    c.h = Number(std::getenv("TILEXR_VALIDATION_H"), "TILEXR_VALIDATION_H");
    c.topK = Number(std::getenv("TILEXR_VALIDATION_TOPK"), "TILEXR_VALIDATION_TOPK");
    c.experts = Number(std::getenv("TILEXR_VALIDATION_MOE_EXPERT_NUM"), "TILEXR_VALIDATION_MOE_EXPERT_NUM");
    if ((c.checkpoint == "ep-sdedup-p2-dispatch" || c.checkpoint == "ep-sdedup-combine-p1-chain-single" ||
        c.checkpoint == "ep-sdedup-combine-p1-chain-multi" ||
        c.checkpoint == "ep-sdedup-combine-p1-profile" ||
        c.checkpoint == "ep-sdedup-combine-p1-memory-boundary" ||
        c.checkpoint == "ep-sdedup-combine-p2-m12" || c.checkpoint == "ep-sdedup-combine-p2-m3" || c.checkpoint == "ep-sdedup-combine-p2-m4" || c.checkpoint == "ep-sdedup-combine-p2-m5" || c.dryRun) &&
        !c.boundaryProbe && c.operatorMode != "combine") {
        Require(c.experts > 0 && c.experts % c.rankSize == 0,
            "total experts must be divisible by rank count");
        if (c.routeMode == TileXREpDemo::RouteMode::BalanceShift && c.topK > 0) {
            int64_t divisor = c.experts;
            for (int64_t remainder = c.topK; remainder != 0;) {
                const int64_t next = divisor % remainder;
                divisor = remainder;
                remainder = next;
            }
            const int64_t epoch = c.experts / divisor;
            divisor = epoch;
            for (int64_t remainder = c.rankSize; remainder != 0;) {
                const int64_t next = divisor % remainder;
                divisor = remainder;
                remainder = next;
            }
            Require(c.bs % (epoch / divisor) == 0,
                "balance_shift requires complete global token epochs");
        }
    }
    const char *dtype = std::getenv("TILEXR_VALIDATION_DTYPE");
    Require(dtype != nullptr && (std::strcmp(dtype, "bf16") == 0 || std::strcmp(dtype, "fp16") == 0),
        "TILEXR_VALIDATION_DTYPE must be bf16 or fp16");
    c.dtype = std::strcmp(dtype, "bf16") == 0 ? TileXR::TILEXR_DATA_TYPE_BFP16 : TileXR::TILEXR_DATA_TYPE_FP16;
    const char *legacyQuant = std::getenv("TILEXR_VALIDATION_QUANT_MODE");
    const char *dispatchQuant = std::getenv("TILEXR_VALIDATION_DISPATCH_QUANT_MODE");
    if (legacyQuant) c.quantMode = Number(legacyQuant, "quant_mode");
    if (dispatchQuant) {
        const int64_t value = Number(dispatchQuant, "dispatch_quant_mode");
        Require(!legacyQuant || c.quantMode == value, "conflicting Dispatch quantization aliases");
        c.quantMode = value;
    }
    if (const char *quant = std::getenv("TILEXR_VALIDATION_COMBINE_QUANT_MODE"))
        c.combineQuantMode = Number(quant, "combine_quant_mode");
    Require(c.combineQuantMode == 0, "combine_quant_mode currently supports only 0");
    if (const char *block = std::getenv("TILEXR_EP_DISPATCH_MEMORY_SERVER_DEDUP_BLOCK_DIM"))
        Require(std::strcmp(block, "auto") == 0 || std::strcmp(block, "64") == 0,
            "block_dim must be auto/64");
    if (const char *format = std::getenv("TILEXR_VALIDATION_FP8_FORMAT")) c.fp8Format = format;
    Require((c.quantMode == 0 || c.quantMode == 4) && (c.fp8Format == "e4m3" || c.fp8Format == "e5m2"),
        "quant_mode must be 0/4; fp8_format must be e4m3/e5m2");
    if (c.dryRun) return c;
    c.barrier = Endpoint("TILEXR_DEMO_BARRIER_ADDR");
    const auto comm = Endpoint("TILEXR_COMM_ID");
    Require(comm.sin_port != c.barrier.sin_port || comm.sin_addr.s_addr != c.barrier.sin_addr.s_addr,
        "communicator and Host barrier need distinct endpoints");
    return c;
}

// The standard runtime owns allocation and export. No profiling data uses the
// communication window; capture configuration must precede communicator init.
struct ProfilePaths {
    std::string dispatch, combine;
};

ProfilePaths ConfigureProfiling(const Config &c)
{
    ProfilePaths paths;
    const bool split = c.checkpoint == "ep-sdedup-combine-p1-profile" ||
        c.checkpoint == "ep-sdedup-combine-p2-m12" || c.checkpoint == "ep-sdedup-combine-p2-m3" || c.checkpoint == "ep-sdedup-combine-p2-m4" || c.checkpoint == "ep-sdedup-combine-p2-m5";
    const bool dispatch = c.runMode == "breakdown" && c.operatorMode != "combine";
    const bool combine = c.runMode == "breakdown" && split && c.operatorMode != "dispatch";
    const auto configure = [&](const char *name, bool enabled, std::string &path) {
        if (!enabled) {
            Check(unsetenv(name), "disable inactive operator capture");
            return;
        }
        const char *value = std::getenv(name);
        Require(value && *value, std::string("breakdown requires ") + name);
        path = value;
    };
    configure("TILEXR_PROFILING_RECORDS_PATH", dispatch, paths.dispatch);
    configure("TILEXR_PROFILING_COMBINE_RECORDS_PATH", combine, paths.combine);
    if (dispatch || combine) {
        const char *capture = std::getenv("TILEXR_PROFILING_CAPTURE_ID");
        Require(capture && *capture, "breakdown requires TILEXR_PROFILING_CAPTURE_ID");
        Require(!dispatch || !combine || paths.dispatch != paths.combine, "operator capture paths must differ");
        Check(setenv("TILEXR_PROFILING_RECORDS_PER_CORE", "65536", 1), "configure profiling capacity");
    }
    return paths;
}

std::string Json(const std::string &text)
{
    std::ostringstream out;
    out << '"';
    for (unsigned char ch : text) {
        if (ch == '"' || ch == '\\') { out << '\\' << ch; }
        else if (ch < 32) { out << "\\u" << std::hex << std::setw(4) << std::setfill('0') << unsigned(ch); }
        else { out << ch; }
    }
    return out.str() + '"';
}

void Marker(const Config &c, const char *name, const std::string &details, const char *status = "PASS")
{
    std::cout << name << " {\"run_id\":" << Json(c.run) << ",\"build_id\":" << Json(c.build)
        << ",\"commit\":" << (c.commit == "unknown" ? "null" : Json(c.commit))
        << ",\"checkpoint\":" << Json(c.checkpoint) << ",\"status\":" << Json(status) << ",\"host\":" << Json(c.host)
        << ",\"run_mode\":" << Json(c.runMode) << ",\"loops\":" << c.loops << ",\"warmup\":" << c.warmup
        << ",\"host_index\":" << c.hostIndex << ",\"local_rank\":" << c.localRank << ",\"device\":" << c.device
        << ",\"rank\":" << c.rank << ",\"rank_size\":" << c.rankSize << ",\"pid\":" << getpid()
        << ",\"backend\":\"memory\",\"bs\":" << c.bs << ",\"h\":" << c.h << ",\"topk\":" << c.topK
        << ",\"server_dedup_enabled\":" << (TileXREp::kEpServerDedupEnable ? "true" : "false")
        << ",\"route_mode\":" << Json(TileXREpDemo::RouteModeName(c.routeMode))
        << ",\"seed\":" << c.seed << ",\"quant_mode\":" << c.quantMode
        << ",\"operator_mode\":" << Json(c.operatorMode)
        << ",\"boundary_probe\":" << (c.boundaryProbe ? "true" : "false")
        << ",\"dispatch_quant_mode\":" << c.quantMode
        << ",\"combine_quant_mode\":" << c.combineQuantMode
        << ",\"fp8_format\":" << Json(c.fp8Format)
        << ",\"moe_expert_num\":" << c.experts << ",\"dtype\":"
        << Json(c.dtype == TileXR::TILEXR_DATA_TYPE_BFP16 ? "bf16" : "fp16");
    if (c.checkpoint == "ep-sdedup-combine-p2-m12" || c.checkpoint == "ep-sdedup-combine-p2-m3" || c.checkpoint == "ep-sdedup-combine-p2-m4" || c.checkpoint == "ep-sdedup-combine-p2-m5") {
        std::cout << ",\"implemented_through\":" << Json(c.checkpoint == "ep-sdedup-combine-p2-m5" ? "M5" : c.checkpoint == "ep-sdedup-combine-p2-m4" ? "M4" :
            c.checkpoint == "ep-sdedup-combine-p2-m3" ? "M3" : "M2") << ",\"scope\":" <<
            Json(c.checkpoint == "ep-sdedup-combine-p2-m5" ? "full" : "partial")
            << ",\"combine_fixture\":" << Json(c.combineFixture)
            << ",\"combine_observation\":" << Json(c.combineObservation);
    }
    std::cout << details << '}' << std::endl;
}

struct Socket {
    int fd = -1;
    explicit Socket(int value) : fd(value) { Require(fd >= 0, "socket: " + std::string(std::strerror(errno))); }
    ~Socket() { if (fd >= 0) { close(fd); } }
    Socket(const Socket &) = delete;
    Socket &operator=(const Socket &) = delete;
    Socket(Socket &&other) noexcept : fd(other.fd) { other.fd = -1; }
};

void Wait(int fd, short events, Deadline deadline)
{
    for (;;) {
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
        Require(ms > 0, "Host barrier timeout");
        pollfd item {fd, events, 0};
        const int result = poll(&item, 1, static_cast<int>(std::min<int64_t>(ms, 1000)));
        if (result < 0 && errno == EINTR) { continue; }
        Require(result >= 0, "Host barrier poll failed");
        if (result != 0) {
            Require((item.revents & events) != 0, "Host barrier connection closed");
            return;
        }
    }
}

void Transfer(int fd, void *data, std::size_t bytes, bool sendData, Deadline deadline)
{
    char *cursor = static_cast<char *>(data);
    while (bytes != 0) {
        Wait(fd, sendData ? POLLOUT : POLLIN, deadline);
        const ssize_t size = sendData ? send(fd, cursor, bytes, MSG_NOSIGNAL | MSG_DONTWAIT) :
            recv(fd, cursor, bytes, MSG_DONTWAIT);
        if (size < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) { continue; }
        Require(size > 0, "Host barrier I/O failed: " + std::string(std::strerror(errno)));
        bytes -= static_cast<std::size_t>(size);
        cursor += size;
    }
}

// 建链与测量完成共用监听 socket 及总期限；报文核对 run/build/rank/phase。
class Barrier {
public:
    Barrier(const Config &config, Deadline deadline) : c_(config), deadline_(deadline) {}
    ~Barrier() { if (listener_ >= 0) { close(listener_); } }
    void Run(uint32_t phase)
    {
        if (c_.rank == 0) { Server(phase); } else { Client(phase); }
    }
private:
    using Message = std::array<unsigned char, 272>;
    Message Hello(uint32_t phase, uint32_t rank) const
    {
        Message message {};
        uint32_t fields[] = {htonl(1), htonl(phase), htonl(rank), htonl(c_.rankSize)};
        std::memcpy(message.data(), fields, sizeof(fields));
        std::memcpy(message.data() + 16, c_.run.data(), c_.run.size());
        std::memcpy(message.data() + 144, c_.build.data(), c_.build.size());
        return message;
    }
    void Server(uint32_t phase)
    {
        if (listener_ < 0) {
            listener_ = socket(AF_INET, SOCK_STREAM, 0);
            Require(listener_ >= 0, "Host barrier listener creation failed");
            int reuse = 1;
            Check(setsockopt(listener_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)), "barrier setsockopt");
            Check(bind(listener_, reinterpret_cast<const sockaddr *>(&c_.barrier), sizeof(c_.barrier)), "barrier bind");
            Check(listen(listener_, c_.rankSize), "barrier listen");
        }
        std::vector<Socket> clients;
        std::vector<Message> messages;
        std::vector<bool> seen(c_.rankSize, false);
        seen[0] = true;
        while (clients.size() != static_cast<std::size_t>(c_.rankSize - 1)) {
            Wait(listener_, POLLIN, deadline_);
            Socket client(accept(listener_, nullptr, nullptr));
            Message message {};
            Transfer(client.fd, message.data(), message.size(), false, deadline_);
            uint32_t wireRank = 0;
            std::memcpy(&wireRank, message.data() + 8, sizeof(wireRank));
            const uint32_t rank = ntohl(wireRank);
            Require(rank < static_cast<uint32_t>(c_.rankSize) && !seen[rank] && message == Hello(phase, rank),
                "Host barrier duplicate or mismatched run/build/rank/phase");
            seen[rank] = true;
            clients.push_back(std::move(client));
            messages.push_back(message);
        }
        for (std::size_t i = 0; i < clients.size(); ++i) {
            Transfer(clients[i].fd, messages[i].data(), messages[i].size(), true, deadline_);
        }
    }
    void Client(uint32_t phase)
    {
        for (;;) {
            Require(Clock::now() < deadline_, "Host barrier connect timeout");
            Socket client(socket(AF_INET, SOCK_STREAM, 0));
            Check(fcntl(client.fd, F_SETFL, O_NONBLOCK), "barrier nonblocking socket");
            int result = connect(client.fd, reinterpret_cast<const sockaddr *>(&c_.barrier), sizeof(c_.barrier));
            int error = result == 0 ? 0 : errno;
            if (error == EINPROGRESS) {
                Wait(client.fd, POLLOUT, deadline_);
                socklen_t length = sizeof(error);
                Check(getsockopt(client.fd, SOL_SOCKET, SO_ERROR, &error, &length), "barrier connect status");
            }
            if (error == ECONNREFUSED) {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                continue;
            }
            Check(error, "barrier connect");
            Message message = Hello(phase, c_.rank);
            Transfer(client.fd, message.data(), message.size(), true, deadline_);
            Message reply {};
            Transfer(client.fd, reply.data(), reply.size(), false, deadline_);
            Require(reply == message, "Host barrier acknowledgement identity mismatch");
            return;
        }
    }
    const Config &c_;
    Deadline deadline_;
    int listener_ = -1;
};

void ValidateComm(const Config &c, const TileXR::CommArgs &args)
{
    Require(args.rank == c.rank && args.rankSize == c.rankSize && args.localRank == c.localRank &&
        args.localRankSize == 8 && args.commDomain == 0, "communicator rank mapping mismatch");
    // The standalone boundary probe retains its historical 4 GiB allocation.
    const uint64_t expectedBytes = c.checkpoint == "ep-sdedup-p1-memory-boundary" ?
        UINT64_C(4) * 1024 * 1024 * 1024 : kServerDedupPeerBytes;
    Require(args.peerMemBytes == expectedBytes,
        "peer window is not exactly " + std::to_string(expectedBytes >> 30) +
        " GiB; actual=" + std::to_string(args.peerMemBytes));
    for (int rank = 0; rank < c.rankSize; ++rank) {
        const uintptr_t begin = reinterpret_cast<uintptr_t>(args.peerMems[rank]);
        Require(begin != 0 && begin % 512 == 0 && args.peerMemBytes <= UINTPTR_MAX - begin,
            "invalid peer window rank=" + std::to_string(rank));
    }
}

#include "server_dedup_source_send_consumer.h"
#include "server_dedup_dispatch_consumer.h"
#include "server_dedup_combine_consumer.h"
#include "server_dedup_combine_m12_consumer.h"
#include "server_dedup_combine_m3_consumer.h"
#include "server_dedup_combine_m4_consumer.h"
#include "server_dedup_combine_m5_consumer.h"

#ifdef TILEXR_EP_MEMORY_BOUNDARY_TEST
#include "memory_boundary_consumer.h"
#endif
#ifdef TILEXR_EP_COMBINE_MEMORY_BOUNDARY_TEST
#include "combine_memory_boundary_consumer.h"
#endif

// Resolve configuration with production layout arithmetic before any runtime,
// profiling, socket or device operation. This describes C4 wiring, not tensors.
int DryRun(const Config &c)
{
    const bool dispatch = c.operatorMode != "combine";
    const bool combine = c.operatorMode != "dispatch";
    if (dispatch) {
        TileXREp::EpMemoryServerDedupLayout layout {};
        Check(TileXREp::TileXREpBuildMemoryServerDedupLayout(c.rankSize, c.bs, c.h,
            c.topK, c.experts, &layout, c.quantMode == 4), "dispatch layout");
    }
    if (combine) {
        TileXREp::EpMemoryServerDedupCombineLayout layout {};
        Check(TileXREp::TileXREpBuildMemoryServerDedupCombineLayout(c.rankSize, c.bs, c.h,
            c.topK, c.experts, &layout), "combine layout");
    }
    const char *input = !combine ? "none" : !dispatch ? "standalone_expert_out" :
        c.quantMode == 0 ? "dispatch_expand_x" : "separate_expert_out";
    std::ostringstream details;
    details << ",\"dispatch_active\":" << (dispatch ? "true" : "false")
        << ",\"combine_active\":" << (combine ? "true" : "false")
        << ",\"dispatch_quantization_active\":" << (dispatch && c.quantMode == 4 ? "true" : "false")
        << ",\"fp8_format_effective\":" << Json(dispatch && c.quantMode == 4 ? c.fp8Format : "none")
        << ",\"combine_input_source\":" << Json(input)
        << ",\"tables_source\":" << Json(!combine ? "none" : dispatch ? "dispatch_outputs" : "standalone")
        << ",\"device_allocated\":false,\"communicator_created\":false"
        << ",\"kernel_launched\":false,\"network_used\":false";
    Marker(c, "EP_SERVER_DEDUP_COMBINE_DRYRUN_PASS", details.str());
    return 0;
}

int Run(const Config &c)
{
    if (c.dryRun) return DryRun(c);
    if (c.boundaryProbe) {
#ifdef TILEXR_EP_COMBINE_MEMORY_BOUNDARY_TEST
        return RunCombineMemoryBoundary(c);
#else
        throw std::runtime_error("Combine boundary test kernel was not built");
#endif
    }
    if (c.checkpoint == "ep-sdedup-p1-memory-boundary") {
#ifdef TILEXR_EP_MEMORY_BOUNDARY_TEST
        return RunMemoryBoundary(c);
#else
        throw std::runtime_error("memory-boundary test kernel was not built");
#endif
    }
    const bool m12 = c.checkpoint == "ep-sdedup-combine-p2-m12";
    const bool m3 = c.checkpoint == "ep-sdedup-combine-p2-m3";
    const bool m4 = c.checkpoint == "ep-sdedup-combine-p2-m4";
    const bool m5 = c.checkpoint == "ep-sdedup-combine-p2-m5";
    const bool observeRounds = c.combineObservation == "per-round";
    const bool multi = m12 || m3 || m4 || m5 || c.checkpoint == "ep-sdedup-combine-p1-chain-multi" ||
        c.checkpoint == "ep-sdedup-combine-p1-profile" ||
        c.checkpoint == "ep-sdedup-combine-p1-memory-boundary";
    const bool runDispatch = c.operatorMode != "combine";
    const bool runCombine = c.operatorMode != "dispatch";
    TileXREp::EpMemoryServerDedupLayout layout {};
    TileXREp::EpMemoryServerDedupCombineLayout combineLayout {};
    if (runDispatch) Check(TileXREp::TileXREpBuildMemoryServerDedupLayout(
        c.rankSize, c.bs, c.h, c.topK, c.experts, &layout, c.quantMode == 4), "layout");
    if (runCombine) Check(TileXREp::TileXREpBuildMemoryServerDedupCombineLayout(
        c.rankSize, c.bs, c.h, c.topK, c.experts, &combineLayout), "combine layout");
    bool initialized = false, deviceSet = false, safeCleanup = true, executionSucceeded = false;
    aclrtStream stream = nullptr;
    TileXRCommPtr comm = nullptr;
    std::vector<void *> buffers;
    uint64_t submission = 0, submitted = 0, completed = 0, warmupSubmitted = 0, warmupCompleted = 0;
    std::string stage = "acl-init", batch = "initialization", dispatch;
    std::string validationError, validationStage, combineDetails;
    OperatorProgress dispatchProgress, combineProgress;
    int64_t sequenceMagic = 0;
    CombineM12Validation m12Validation;
    CombineM3Validation m3Validation;
    CombineM4Validation m4Validation;
    CombineM5Validation m5Validation;
    uint64_t streamSynchronizations = 0;
    const auto profilePaths = ConfigureProfiling(c);
    auto enterStage = [&](const char *name) {
        stage = name;
        // Flush before a potentially blocking call: an external timeout cannot
        // report the in-memory stage through the exception handler.
        if (c.runMode == "breakdown") {
            std::cerr << "EP_SERVER_DEDUP_PROGRESS rank=" << c.rank << " stage=" << stage << " batch=" << batch
                << " submission=" << submission << " submitted=" << submitted << " completed=" << completed
                << " warmup_submitted=" << warmupSubmitted << " warmup_completed=" << warmupCompleted << std::endl;
        }
    };
    Barrier barrier(c, Clock::now() + std::chrono::seconds(90));
    try {
        enterStage("acl-init");
        Check(aclInit(nullptr), "aclInit"); initialized = true;
        enterStage("device");
        Check(aclrtSetDevice(c.device), "aclrtSetDevice"); deviceSet = true;
        const char *soc = aclrtGetSocName();
        Require(soc != nullptr && (std::strncmp(soc, "Ascend950", 9) == 0 ||
            std::strncmp(soc, "Ascend910_9", 11) == 0 || std::strncmp(soc, "A5", 2) == 0), "A5 required");
        Check(aclrtCreateStream(&stream), "aclrtCreateStream");
        enterStage("communicator");
        Check(TileXRCommInitRankMemoryDomainWithSize(0, kServerDedupDataMiB, c.rankSize, c.rank, &comm),
            "TileXRCommInitRankMemoryDomainWithSize");
        TileXR::CommArgs *host = nullptr;
        GM_ADDR dev = nullptr;
        Check(TileXRGetCommArgsHost(comm, host), "TileXRGetCommArgsHost");
        Check(TileXRGetCommArgsDev(comm, dev), "TileXRGetCommArgsDev");
        Require(host != nullptr && dev != nullptr, "missing Host/device CommArgs");
        ValidateComm(c, *host);
        TileXR::CommArgs deviceArgs {};
        Check(aclrtMemcpy(&deviceArgs, sizeof(deviceArgs), dev, sizeof(deviceArgs), ACL_MEMCPY_DEVICE_TO_HOST),
            "read device CommArgs");
        ValidateComm(c, deviceArgs);
        Require(host->peerMemBytes == deviceArgs.peerMemBytes, "device peer window size differs from Host");
        for (int rank = 0; rank < c.rankSize; ++rank) {
            Require(host->peerMems[rank] == deviceArgs.peerMems[rank], "device peer mapping differs from Host");
        }
        enterStage("allocation");
        std::array<uint64_t, 14> sizes {};
        if (runDispatch) {
            sizes = {{layout.xBytes, layout.expertIdsBytes, layout.expandXBytes,
                layout.expertTokenNumsBytes, layout.sendCountsBytes, layout.tokenTypeBytes,
                layout.destinationIndexBytes, layout.relayReadIndexBytes, layout.sourceMaskBytes,
                layout.weightBytes, layout.weightBytes, layout.dynamicScalesBytes, 0, 0}};
        } else {
            sizes[4] = combineLayout.sendCountsBytes;
            sizes[5] = combineLayout.tokenTypeBytes;
            sizes[6] = combineLayout.assistInfoBytes;
            sizes[7] = combineLayout.relayReadIndexBytes;
            sizes[8] = combineLayout.sourceMaskBytes;
            sizes[10] = combineLayout.expertScalesBytes;
        }
        if (runCombine) {
            sizes[12] = !runDispatch || c.quantMode != 0 ? combineLayout.expertOutBytes : 0;
            sizes[13] = combineLayout.yOutBytes;
        }
        const uint32_t alignments[] = {2, 4, 2, 8, 4, 4, 4, 8, 4, 4, 4, 1, 2, 2};
        std::vector<TileXREp::EpServerDedupAllocation> allocations;
        for (std::size_t i = 0; i < sizes.size(); ++i) {
            if (sizes[i] == 0) { buffers.push_back(nullptr); continue; }
            void *data = nullptr;
            Require(sizes[i] > 0 && sizes[i] <= SIZE_MAX, "allocation size overflow");
            Check(aclrtMalloc(&data, static_cast<std::size_t>(sizes[i]), ACL_MEM_MALLOC_HUGE_FIRST), "aclrtMalloc");
            buffers.push_back(data);
            int32_t device = -1;
            Check(aclrtGetDevice(&device), "aclrtGetDevice");
            allocations.push_back({data, sizes[i], sizes[i], alignments[i], device});
        }
        const auto shape = SourceSendShape(c);
        std::vector<float> weightsHost;
        if (runDispatch) {
            const auto xHost = ServerDedupReference::Hidden(shape, c.rank);
            const auto idsHost = ServerDedupReference::Ids(shape, c.rank);
            weightsHost = ServerDedupReference::Weights(shape, c.rank);
            SaveSourceSendInputs(c, xHost, idsHost, weightsHost);
            Check(aclrtMemcpy(buffers[0], layout.xBytes, xHost.data(), layout.xBytes, ACL_MEMCPY_HOST_TO_DEVICE), "copy x");
            Check(aclrtMemcpy(buffers[1], layout.expertIdsBytes, idsHost.data(), idsHost.size() * sizeof(int32_t), ACL_MEMCPY_HOST_TO_DEVICE), "copy expert ids");
            Check(aclrtMemcpy(buffers[9], layout.weightBytes, weightsHost.data(),
                weightsHost.size() * sizeof(float), ACL_MEMCPY_HOST_TO_DEVICE), "copy expert scales");
            Check(aclrtMemset(buffers[8], layout.sourceMaskBytes, 0xff, layout.sourceMaskBytes), "initialize source mask");
            Check(aclrtMemset(buffers[10], layout.weightBytes, 0xff, layout.weightBytes), "initialize source weights");
            for (const uint32_t index : {2U, 5U, 6U, 7U, 11U})
                if (sizes[index]) Check(aclrtMemset(buffers[index], sizes[index], 0xa5, sizes[index]), "initialize dispatch output");
            // Valid count/prefix outputs are nonnegative. Preserve a distinct value
            // when a successful launch fails to write an output element.
            Check(aclrtMemset(buffers[3], layout.expertTokenNumsBytes, 0xff, layout.expertTokenNumsBytes), "initialize token counts");
            Check(aclrtMemset(buffers[4], layout.sendCountsBytes, 0xff, layout.sendCountsBytes), "initialize send counts");
        } else {
            InitializeStandaloneCombine(c, combineLayout, buffers);
        }
        if (runCombine) {
            if (sizes[12]) InitializeCombineExpert(c, buffers[12], sizes[12]);
            Check(aclrtMemset(buffers[13], sizes[13], 0xa5, sizes[13]), "initialize combine yOut");
        }
        if (m12 && runCombine) m12Validation.Initialize(c, *host);
        if (m3 && runCombine) m3Validation.Initialize(c, *host);
        if (m4 && runCombine) m4Validation.Initialize(c, *host);
        if (m5 && runCombine) m5Validation.Initialize(c, *host);
        Check(TileXREp::TileXREpValidateServerDedupAllocations(allocations.data(), allocations.size(), c.device),
            "caller allocation validation");
        enterStage("chain-barrier");
        safeCleanup = false;
        barrier.Run(0);
        Marker(c, "EP_SERVER_DEDUP_CHAIN_PASS", ",\"peer_count\":" + std::to_string(c.rankSize) +
            ",\"peer_mem_bytes\":" + std::to_string(host->peerMemBytes));
        const auto outputDtype = c.quantMode == 0 ? TileXR::TILEXR_DATA_TYPE_RESERVED :
            (c.fp8Format == "e4m3" ? TileXR::TILEXR_DATA_TYPE_FP8E4M3 : TileXR::TILEXR_DATA_TYPE_FP8E5M2);
        // C4 retains its synchronized full-snapshot boundary. C5 submits the
        // entire D/C batch on one stream with no per-round Host observation.
        const uint64_t calls = uint64_t(c.warmup) + c.loops;
        std::vector<CombineTensorSnapshot> snapshots;
        if (multi && runCombine) {
            enterStage("combine-snapshot");
            snapshots = CaptureCombineTensors(c, combineLayout, buffers, runDispatch);
        }
        const auto launchCombine = [&](bool warming) {
            enterStage("combine-launch");
            safeCleanup = false;
            void *expertOut = runDispatch && c.quantMode == 0 ? buffers[2] : buffers[12];
            Check(TileXRMoeEpCombineMemoryServerDedup(expertOut,
                static_cast<int32_t *>(buffers[6]), static_cast<int32_t *>(buffers[4]),
                static_cast<float *>(buffers[10]), nullptr, TileXREp::TILEXR_EP_ACTIVE_MASK_NONE,
                nullptr, comm, c.bs, c.h, c.topK, c.experts, c.rankSize, c.rank, 1, 0, 0, 0, 0,
                c.combineQuantMode, static_cast<int64_t>(combineLayout.globalBs), buffers[13],
                c.dtype, stream, static_cast<const uint32_t *>(buffers[5]), buffers[7],
                static_cast<const uint32_t *>(buffers[8])), "TileXRMoeEpCombineMemoryServerDedup");
            combineProgress.Submit(warming, ++sequenceMagic);
        };
        for (uint64_t call = 0; (runDispatch || multi) && call < calls; ++call) {
            const bool warming = call < c.warmup;
            if (m12 && observeRounds && !runDispatch) {
                InitializeCombineExpert(c, buffers[12], sizes[12], call + 1U);
                snapshots = CaptureCombineTensors(c, combineLayout, buffers);
            }
            batch = warming ? "warmup" : "measurement";
            submission = warming ? call + 1U : call - c.warmup + 1U;
            if (runDispatch) {
                stage = "launch";
                // One entry per warmup/measurement phase, independent of loop
                // count. Counters in this message describe phase entry only.
                if (submission == 1U) enterStage("launch");
                Check(TileXRMoeEpDispatchMemoryServerDedup(buffers[0], static_cast<int32_t *>(buffers[1]), nullptr,
                    nullptr, TileXREp::TILEXR_EP_ACTIVE_MASK_NONE, buffers[9], comm, c.bs, c.h, c.topK, c.experts,
                    c.rankSize, c.rank, 1, 0, 0, 0, 0, c.quantMode, static_cast<int64_t>(layout.globalBs), 1, buffers[2],
                    buffers[11], static_cast<int64_t *>(buffers[3]), static_cast<int32_t *>(buffers[4]),
                    nullptr, nullptr, static_cast<uint32_t *>(buffers[5]), static_cast<uint32_t *>(buffers[6]),
                    static_cast<uint32_t *>(buffers[7]), static_cast<uint32_t *>(buffers[8]), static_cast<float *>(buffers[10]), c.dtype, outputDtype, stream),
                    "TileXRMoeEpDispatchMemoryServerDedup");
                dispatchProgress.Submit(warming, ++sequenceMagic);
            }
            if (multi && runCombine) {
                launchCombine(warming);
                if (m5) m5Validation.NoteLaunch(combineProgress.lastMagic, 0U);
                if (m4) m4Validation.NoteLaunch(combineProgress.lastMagic, 0U);
                if (m3) m3Validation.NoteLaunch(combineProgress.lastMagic, 0U);
                if (m12) m12Validation.NoteLaunch(combineProgress.lastMagic, observeRounds && !runDispatch ? call + 1U : 0U);
            }
            if (warming) ++warmupSubmitted;
            else ++submitted;
            if (m12 && observeRounds) {
                enterStage("combine-round-sync");
                Check(aclrtSynchronizeStream(stream), "P2.1 diagnostic round synchronize");
                ++streamSynchronizations;
                barrier.Run(static_cast<uint32_t>(10U + call * 2U));
                safeCleanup = true;
                ValidateCombineTensors(snapshots);
                m12Validation.Validate(c, *host, combineLayout, buffers);
                barrier.Run(static_cast<uint32_t>(11U + call * 2U));
                safeCleanup = false;
            }
        }
        if (runDispatch || multi) {
            enterStage("stream-sync");
            if (!observeRounds) {
                Check(aclrtSynchronizeStream(stream), "aclrtSynchronizeStream");
                ++streamSynchronizations;
            }
            enterStage("batch-barrier");
            barrier.Run(2);
            completed = submitted;
            warmupCompleted = warmupSubmitted;
            dispatchProgress.Complete();
            if (multi) combineProgress.Complete();
            safeCleanup = true;
        }
        if (runDispatch) {
            enterStage("count-read");
            std::vector<int32_t> actualPrefix(c.experts), referenceCounts(c.experts, 0);
            std::vector<int64_t> actualExperts(c.experts / c.rankSize);
            Check(aclrtMemcpy(actualPrefix.data(), layout.sendCountsBytes, buffers[4],
                layout.sendCountsBytes, ACL_MEMCPY_DEVICE_TO_HOST), "read sendCountsOut");
            Check(aclrtMemcpy(actualExperts.data(), layout.expertTokenNumsBytes, buffers[3],
                layout.expertTokenNumsBytes, ACL_MEMCPY_DEVICE_TO_HOST), "read expertTokenNumsOut");
            const int64_t localExperts = c.experts / c.rankSize;
            for (int source = 0; source < c.rankSize; ++source) {
                for (int32_t expert : ServerDedupReference::Ids(shape, source)) {
                    if (expert / localExperts == c.rank)
                        ++referenceCounts[(expert % localExperts) * c.rankSize + source];
                }
            }
            int64_t cumulative = 0, previousExpert = 0;
            for (int64_t index = 0; index < c.experts; ++index) {
                cumulative += referenceCounts[index];
                const int64_t globalExpert = c.rank * localExperts + index / c.rankSize;
                CheckCountValue("sendCountsOut", actualPrefix, static_cast<size_t>(index),
                    cumulative, globalExpert, static_cast<int>(index % c.rankSize));
                if ((index + 1) % c.rankSize == 0) {
                    const int64_t expected = cumulative - previousExpert;
                    CheckCountValue("expertTokenNumsOut", actualExperts, static_cast<size_t>(index / c.rankSize),
                        expected, globalExpert);
                    previousExpert = cumulative;
                }
            }
            enterStage("dispatch-read");
            dispatch = ValidateDispatch(c, *host, layout, buffers, weightsHost, dispatchProgress.lastMagic);
            if (multi) dispatch += ",\"dispatch_first_magic\":" + std::to_string(dispatchProgress.firstMagic) +
                ",\"dispatch_window_mask\":" + std::to_string(dispatchProgress.windowMask) +
                ",\"stream_synchronizations\":" + std::to_string(streamSynchronizations);
        }
        if (runCombine) {
            if (!multi) {
                enterStage("combine-snapshot");
                snapshots = CaptureCombineTensors(c, combineLayout, buffers);
                launchCombine(false);
                enterStage("combine-sync");
                Check(aclrtSynchronizeStream(stream), "combine aclrtSynchronizeStream");
                enterStage("combine-barrier");
                barrier.Run(3);
                combineProgress.Complete();
                safeCleanup = true;
            }
            enterStage("combine-read");
            ValidateCombineTensors(snapshots);
            if (m5) {
                m5Validation.Validate(c, *host, combineLayout, buffers);
                combineDetails = m5Validation.Details(c, host->peerMemBytes, dispatchProgress,
                    combineProgress, streamSynchronizations);
            } else if (m4) {
                m4Validation.Validate(c, *host, combineLayout, buffers);
                combineDetails = m4Validation.Details(c, host->peerMemBytes, dispatchProgress,
                    combineProgress, streamSynchronizations);
            } else if (m3) {
                m3Validation.Validate(c, *host, combineLayout, buffers);
                combineDetails = m3Validation.Details(c, host->peerMemBytes, dispatchProgress,
                    combineProgress, streamSynchronizations);
            } else if (m12) {
                if (!observeRounds) m12Validation.Validate(c, *host, combineLayout, buffers);
                combineDetails = m12Validation.Details(c, host->peerMemBytes, dispatchProgress,
                    combineProgress, streamSynchronizations);
            } else combineDetails = multi ? CombineMultiDetails(c, host->peerMemBytes, dispatchProgress, combineProgress) :
                CombineSingleDetails(c, host->peerMemBytes);
        }
        executionSucceeded = true;
    } catch (const OutputValidationFailure &error) {
        validationError = error.what();
        validationStage = stage;
        if (m5 && runCombine) combineDetails = m5Validation.Details(c, kServerDedupDataMiB * 1024ULL * 1024ULL,
            dispatchProgress, combineProgress, streamSynchronizations, false);
        if (m4 && runCombine) combineDetails = m4Validation.Details(c, kServerDedupDataMiB * 1024ULL * 1024ULL,
            dispatchProgress, combineProgress, streamSynchronizations, false);
        if (m3 && runCombine) combineDetails = m3Validation.Details(c, kServerDedupDataMiB * 1024ULL * 1024ULL,
            dispatchProgress, combineProgress, streamSynchronizations, false);
        std::cerr << "EP_SERVER_DEDUP_VALIDATION_FAIL rank=" << c.rank << " stage=" << stage
            << " " << validationError << std::endl;
        // Only output mismatches are recoverable for capture. API/stream and
        // teardown failures must still prevent a completed-execution marker.
        // The coordinator owns the failed correctness verdict. Let every rank
        // finish cleanup and profiler export before it reports that verdict.
        executionSucceeded = true;
    } catch (const std::exception &error) {
        std::cerr << "EP_SERVER_DEDUP_FAIL rank=" << c.rank << " stage=" << stage << " batch=" << batch
            << " submission=" << submission << " submitted=" << submitted << " completed=" << completed
            << " warmup_submitted=" << warmupSubmitted << " warmup_completed=" << warmupCompleted
            << " dispatch_submitted=" << dispatchProgress.submitted
            << " dispatch_completed=" << dispatchProgress.completed
            << " dispatch_warmup_submitted=" << dispatchProgress.warmupSubmitted
            << " dispatch_warmup_completed=" << dispatchProgress.warmupCompleted
            << " combine_submitted=" << combineProgress.submitted << " combine_completed=" << combineProgress.completed
            << " combine_warmup_submitted=" << combineProgress.warmupSubmitted
            << " combine_warmup_completed=" << combineProgress.warmupCompleted
            << " " << error.what() << std::endl;
    }
    if (!safeCleanup) {
        std::cerr << "rank=" << c.rank << " cleanup=deferred-to-process-exit: batch completion unconfirmed" << std::endl;
        return 1;
    }
    auto cleanup = [&](int code, const char *name) {
        if (code != 0) {
            executionSucceeded = false;
            std::cerr << "EP_SERVER_DEDUP_FAIL rank=" << c.rank << " stage=cleanup batch=" << batch
                << " submission=" << submission
                << " submitted=" << submitted << " completed=" << completed << " warmup_submitted=" << warmupSubmitted
                << " warmup_completed=" << warmupCompleted << " " << name << " ret=" << code << std::endl;
        }
    };
    enterStage("cleanup-buffers");
    for (auto data : buffers) { if (data != nullptr) cleanup(aclrtFree(data), "aclrtFree"); }
    if (comm != nullptr) { enterStage("cleanup-communicator"); cleanup(TileXRCommDestroy(comm), "TileXRCommDestroy"); }
    if (stream != nullptr) { enterStage("cleanup-stream"); cleanup(aclrtDestroyStream(stream), "aclrtDestroyStream"); }
    if (deviceSet) { enterStage("cleanup-device"); cleanup(aclrtResetDevice(c.device), "aclrtResetDevice"); }
    if (initialized) { enterStage("cleanup-acl"); cleanup(aclFinalize(), "aclFinalize"); }
    if (executionSucceeded) {
        std::string details = ",\"submitted\":" + std::to_string(submitted) + ",\"completed\":" +
            std::to_string(completed) + ",\"warmup_submitted\":" + std::to_string(warmupSubmitted) +
            ",\"warmup_completed\":" + std::to_string(warmupCompleted) + ",\"cleanup\":\"complete\"";
        const auto profileMarker = [&](const char *op, const std::string &path) {
            if (path.empty()) return;
            const bool split = c.checkpoint == "ep-sdedup-combine-p1-profile" ||
                c.checkpoint == "ep-sdedup-combine-p2-m12" || c.checkpoint == "ep-sdedup-combine-p2-m3" || c.checkpoint == "ep-sdedup-combine-p2-m4" || c.checkpoint == "ep-sdedup-combine-p2-m5";
            Marker(c, split ? "EP_SERVER_DEDUP_OPERATOR_BREAKDOWN_PASS" : "EP_SERVER_DEDUP_BREAKDOWN_PASS",
                (split ? ",\"operator\":" + Json(op) : "") +
                ",\"schema\":\"tilexr_profiling_records.v1\",\"records_path\":" + Json(path) +
                ",\"capture_id\":" + Json(std::getenv("TILEXR_PROFILING_CAPTURE_ID")) +
                ",\"launches\":" + std::to_string(uint64_t(c.warmup) + c.loops) +
                ",\"capture_mode\":\"latest_launch\",\"captured_launches\":1,\"captured_launch\":" +
                std::to_string(uint64_t(c.warmup) + c.loops - 1U));
        };
        profileMarker("dispatch", profilePaths.dispatch);
        profileMarker("combine", profilePaths.combine);
        if (validationError.empty()) {
            if (runDispatch) Marker(c, "EP_SERVER_DEDUP_DISPATCH_PASS", details + dispatch);
            if (runCombine) Marker(c, "EP_SERVER_DEDUP_COMBINE_CHAIN_PASS",
                ",\"submitted\":" + std::to_string(combineProgress.submitted) +
                ",\"completed\":" + std::to_string(combineProgress.completed) +
                ",\"warmup_submitted\":" + std::to_string(combineProgress.warmupSubmitted) +
                ",\"warmup_completed\":" + std::to_string(combineProgress.warmupCompleted) +
                ",\"cleanup\":\"complete\"" + combineDetails);
        } else if (runCombine) {
            if ((m3 || m4 || m5) && runDispatch) {
                if (!dispatch.empty()) Marker(c, "EP_SERVER_DEDUP_DISPATCH_PASS", details + dispatch);
                else Marker(c, "EP_SERVER_DEDUP_DISPATCH_INVALID", details +
                    dispatchProgress.Details("dispatch", kServerDedupDataMiB * 1024ULL * 1024ULL) +
                    ",\"kernel_scope\":\"dispatch\",\"native_status\":\"dispatch\",\"token_stride\":" +
                    std::to_string(layout.data.tokenStride) + ",\"validation_status\":\"FAIL\",\"validation_stage\":" +
                    Json(validationStage) + ",\"validation_error\":" + Json(validationError), "FAIL");
            }
            Marker(c, "EP_SERVER_DEDUP_COMBINE_CHAIN_INVALID",
                details + combineDetails + ",\"execution_completed\":true,\"validation_status\":\"FAIL\"" +
                ",\"validation_stage\":" + Json(validationStage) +
                ",\"validation_error\":" + Json(validationError), "FAIL");
        } else {
            Marker(c, "EP_SERVER_DEDUP_DISPATCH_INVALID", details +
                ",\"kernel_scope\":\"dispatch\",\"native_status\":\"dispatch\",\"token_stride\":" +
                std::to_string(layout.data.tokenStride) + ",\"validation_status\":\"FAIL\",\"validation_stage\":" +
                Json(validationStage) + ",\"validation_error\":" + Json(validationError), "FAIL");
        }
    }
    // M3/M4/M5 mirror Dispatch's capture contract: a completed execution emits a
    // structured INVALID result for the coordinator, which exports available
    // measurements/trace and then returns validation FAIL.
    return executionSucceeded && (m3 || m4 || m5 || !runCombine || validationError.empty()) ? 0 : 1;
}
} // namespace

int main(int argc, char **argv)
{
    try { return Run(Parse(argc, argv)); }
    catch (const std::exception &error) {
        std::cerr << "EP_SERVER_DEDUP_FAIL stage=config " << error.what() << std::endl;
        return 1;
    }
}
