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
#include "ep_memory_layout.h"
#include "ep_memory_server_dedup_layout.h"
#include "ep_memory_server_dedup_profile.h"
#include "../demo/tilexr_ep_expert_ids.h"
#include "server_dedup_dispatch_reference.h"
#ifdef TILEXR_EP_MEMORY_BOUNDARY_TEST
#include "memory_boundary_test.h"
#endif

namespace {
using Clock = std::chrono::steady_clock;
using Deadline = Clock::time_point;
// Fixed validation capacity through Dispatch/Combine Server Dedup qualification.
constexpr uint64_t kServerDedupPeerBytes = UINT64_C(4) * 1024 * 1024 * 1024;
constexpr int kServerDedupDataMiB = static_cast<int>(
    (kServerDedupPeerBytes - TileXR::TILEXR_FLAG_BUFF_BYTES) / (1024 * 1024));

void Require(bool condition, const std::string &message)
{
    if (!condition) { throw std::runtime_error(message); }
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
    throw std::runtime_error(error.str());
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
    int64_t bs, h, topK, experts;
    TileXR::TileXRDataType dtype;
    int64_t quantMode = 0;
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
    Require(argc == 10, "expected run build commit host host_index local_rank device rank rank_size");
    Config c {};
    c.run = argv[1]; c.build = argv[2]; c.commit = argv[3]; c.host = argv[4];
    const char *checkpoint = std::getenv("TILEXR_VALIDATION_CHECKPOINT");
    c.checkpoint = checkpoint == nullptr ? "ep-sdedup-p2-dispatch" : checkpoint;
    Require(c.checkpoint == "ep-sdedup-p2-dispatch" ||
        c.checkpoint == "ep-sdedup-p1-memory-boundary",
        "unsupported TILEXR_VALIDATION_CHECKPOINT");
    const char *mode = std::getenv("TILEXR_VALIDATION_RUN_MODE");
    c.runMode = mode == nullptr ? "functional" : mode;
    Require(c.runMode == "functional" || c.runMode == "performance" || c.runMode == "breakdown",
        "unsupported TILEXR_VALIDATION_RUN_MODE");
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
    c.bs = Number(std::getenv("TILEXR_VALIDATION_BS"), "TILEXR_VALIDATION_BS");
    c.h = Number(std::getenv("TILEXR_VALIDATION_H"), "TILEXR_VALIDATION_H");
    c.topK = Number(std::getenv("TILEXR_VALIDATION_TOPK"), "TILEXR_VALIDATION_TOPK");
    c.experts = Number(std::getenv("TILEXR_VALIDATION_MOE_EXPERT_NUM"), "TILEXR_VALIDATION_MOE_EXPERT_NUM");
    const char *dtype = std::getenv("TILEXR_VALIDATION_DTYPE");
    Require(dtype != nullptr && (std::strcmp(dtype, "bf16") == 0 || std::strcmp(dtype, "fp16") == 0),
        "TILEXR_VALIDATION_DTYPE must be bf16 or fp16");
    c.dtype = std::strcmp(dtype, "bf16") == 0 ? TileXR::TILEXR_DATA_TYPE_BFP16 : TileXR::TILEXR_DATA_TYPE_FP16;
    if (const char *quant = std::getenv("TILEXR_VALIDATION_QUANT_MODE")) c.quantMode = Number(quant, "quant_mode");
    if (const char *format = std::getenv("TILEXR_VALIDATION_FP8_FORMAT")) c.fp8Format = format;
    Require((c.quantMode == 0 || c.quantMode == 4) && (c.fp8Format == "e4m3" || c.fp8Format == "e5m2"),
        "quant_mode must be 0/4; fp8_format must be e4m3/e5m2");
    c.barrier = Endpoint("TILEXR_DEMO_BARRIER_ADDR");
    const auto comm = Endpoint("TILEXR_COMM_ID");
    Require(comm.sin_port != c.barrier.sin_port || comm.sin_addr.s_addr != c.barrier.sin_addr.s_addr,
        "communicator and Host barrier need distinct endpoints");
    return c;
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

void Marker(const Config &c, const char *name, const std::string &details)
{
    std::cout << name << " {\"run_id\":" << Json(c.run) << ",\"build_id\":" << Json(c.build)
        << ",\"commit\":" << (c.commit == "unknown" ? "null" : Json(c.commit))
        << ",\"checkpoint\":" << Json(c.checkpoint) << ",\"status\":\"PASS\",\"host\":" << Json(c.host)
        << ",\"run_mode\":" << Json(c.runMode) << ",\"loops\":" << c.loops << ",\"warmup\":" << c.warmup
        << ",\"host_index\":" << c.hostIndex << ",\"local_rank\":" << c.localRank << ",\"device\":" << c.device
        << ",\"rank\":" << c.rank << ",\"rank_size\":" << c.rankSize << ",\"pid\":" << getpid()
        << ",\"backend\":\"memory\",\"bs\":" << c.bs << ",\"h\":" << c.h << ",\"topk\":" << c.topK
        << ",\"route_mode\":\"balance_shift\",\"seed\":2026,\"quant_mode\":" << c.quantMode
        << ",\"fp8_format\":" << Json(c.fp8Format)
        << ",\"moe_expert_num\":" << c.experts << ",\"dtype\":"
        << Json(c.dtype == TileXR::TILEXR_DATA_TYPE_BFP16 ? "bf16" : "fp16") << details << '}' << std::endl;
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
    Require(args.peerMemBytes == kServerDedupPeerBytes,
        "peer window is not exactly 4 GiB; actual=" + std::to_string(args.peerMemBytes));
    for (int rank = 0; rank < c.rankSize; ++rank) {
        const uintptr_t begin = reinterpret_cast<uintptr_t>(args.peerMems[rank]);
        Require(begin != 0 && begin % 32 == 0 && args.peerMemBytes <= UINTPTR_MAX - begin,
            "invalid peer window rank=" + std::to_string(rank));
    }
}

#include "server_dedup_source_send_consumer.h"
#include "server_dedup_dispatch_consumer.h"

#ifdef TILEXR_EP_MEMORY_BOUNDARY_TEST
#include "memory_boundary_consumer.h"
#endif

int Run(const Config &c)
{
    if (c.checkpoint == "ep-sdedup-p1-memory-boundary") {
#ifdef TILEXR_EP_MEMORY_BOUNDARY_TEST
        return RunMemoryBoundary(c);
#else
        throw std::runtime_error("memory-boundary test kernel was not built");
#endif
    }
    TileXREp::EpMemoryServerDedupLayout layout {};
    Check(TileXREp::TileXREpBuildMemoryServerDedupLayout(c.rankSize, c.bs, c.h, c.topK, c.experts, &layout, c.quantMode == 4),
        "layout");
    bool initialized = false, deviceSet = false, safeCleanup = true, success = false;
    aclrtStream stream = nullptr;
    TileXRCommPtr comm = nullptr;
    std::vector<void *> buffers;
    uint64_t submission = 0, submitted = 0, completed = 0, warmupSubmitted = 0, warmupCompleted = 0;
    std::string stage = "acl-init", batch = "initialization", breakdown, dispatch;
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
        const uint64_t sizes[] = {layout.xBytes, layout.expertIdsBytes, layout.expandXBytes,
            layout.expertTokenNumsBytes, layout.sendCountsBytes, layout.assistExBytes,
            layout.sourceMaskBytes, layout.groupBytes, layout.weightBytes, layout.weightBytes, layout.dynamicScalesBytes};
        const uint32_t alignments[] = {2, 4, 2, 8, 4, 32, 4, 32, 4, 4, 1};
        std::vector<TileXREp::EpServerDedupAllocation> allocations;
        for (std::size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); ++i) {
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
        const auto xHost = ServerDedupReference::Hidden(shape, c.rank);
        const auto idsHost = ServerDedupReference::Ids(shape, c.rank);
        const auto weightsHost = ServerDedupReference::Weights(shape, c.rank);
        SaveSourceSendInputs(c, xHost, idsHost, weightsHost);
        Check(aclrtMemcpy(buffers[0], layout.xBytes, xHost.data(), layout.xBytes, ACL_MEMCPY_HOST_TO_DEVICE), "copy x");
        Check(aclrtMemcpy(buffers[1], layout.expertIdsBytes, idsHost.data(), idsHost.size() * sizeof(int32_t), ACL_MEMCPY_HOST_TO_DEVICE), "copy expert ids");
        Check(aclrtMemcpy(buffers[8], layout.weightBytes, weightsHost.data(),
            weightsHost.size() * sizeof(float), ACL_MEMCPY_HOST_TO_DEVICE), "copy expert scales");
        Check(aclrtMemset(buffers[6], layout.sourceMaskBytes, 0xff, layout.sourceMaskBytes), "initialize source mask");
        Check(aclrtMemset(buffers[9], layout.weightBytes, 0xff, layout.weightBytes), "initialize source weights");
        for (const uint32_t index : {2U, 5U, 7U, 10U})
            if (sizes[index]) Check(aclrtMemset(buffers[index], sizes[index], 0xa5, sizes[index]), "initialize dispatch output");
        // Valid count/prefix outputs are nonnegative. Preserve a distinct value
        // when a successful launch fails to write an output element.
        Check(aclrtMemset(buffers[3], layout.expertTokenNumsBytes, 0xff, layout.expertTokenNumsBytes), "initialize token counts");
        Check(aclrtMemset(buffers[4], layout.sendCountsBytes, 0xff, layout.sendCountsBytes), "initialize send counts");
        Check(TileXREp::TileXREpValidateServerDedupAllocations(allocations.data(), allocations.size(), c.device),
            "caller allocation validation");
        enterStage("chain-barrier");
        safeCleanup = false;
        barrier.Run(0);
        Marker(c, "EP_SERVER_DEDUP_CHAIN_PASS", ",\"peer_count\":" + std::to_string(c.rankSize) +
            ",\"peer_mem_bytes\":" + std::to_string(host->peerMemBytes));
        const auto outputDtype = c.quantMode == 0 ? TileXR::TILEXR_DATA_TYPE_RESERVED :
            (c.fp8Format == "e4m3" ? TileXR::TILEXR_DATA_TYPE_FP8E4M3 : TileXR::TILEXR_DATA_TYPE_FP8E5M2);
        // One communicator, stream and buffer set for the entire continuous
        // batch. Device completion agreement protects cross-rank window reuse.
        const uint64_t calls = uint64_t(c.warmup) + c.loops;
        for (uint64_t call = 0; call < calls; ++call) {
            const bool warming = call < c.warmup;
            batch = warming ? "warmup" : "measurement";
            submission = warming ? call + 1U : call - c.warmup + 1U;
            stage = "launch";
            // One entry per warmup/measurement phase, independent of loop
            // count. Counters in this message describe phase entry only.
            if (submission == 1U) enterStage("launch");
            Check(TileXRMoeEpDispatchMemoryServerDedup(buffers[0], static_cast<int32_t *>(buffers[1]), nullptr,
                nullptr, TileXREp::TILEXR_EP_ACTIVE_MASK_NONE, buffers[8], comm, c.bs, c.h, c.topK, c.experts,
                c.rankSize, c.rank, 1, 0, 0, 0, 0, c.quantMode, static_cast<int64_t>(layout.globalBs), 1, buffers[2],
                buffers[10], nullptr, static_cast<int64_t *>(buffers[3]), static_cast<int32_t *>(buffers[4]),
                nullptr, nullptr, static_cast<uint32_t *>(buffers[5]), static_cast<uint32_t *>(buffers[6]),
                static_cast<uint32_t *>(buffers[7]), static_cast<float *>(buffers[9]), c.dtype, outputDtype, stream),
                "TileXRMoeEpDispatchMemoryServerDedup");
            if (warming) ++warmupSubmitted;
            else ++submitted;
        }
        enterStage("stream-sync");
        Check(aclrtSynchronizeStream(stream), "aclrtSynchronizeStream");
        enterStage("batch-barrier");
        barrier.Run(2);
        completed = submitted;
        warmupCompleted = warmupSubmitted;
        safeCleanup = true;
        enterStage("round-read");
        std::vector<int64_t> epochs(uint64_t(c.rankSize) * 4U);
        Check(aclrtMemcpy(epochs.data(), epochs.size() * sizeof(int64_t),
            host->peerMems[c.rank] + TileXREp::TileXREpServerDedupRoundOffset(host->peerMemBytes),
            epochs.size() * sizeof(int64_t), ACL_MEMCPY_DEVICE_TO_HOST), "read completion epochs");
        for (size_t word = 0; word < epochs.size(); ++word)
            Require(epochs[word] == int64_t(calls), "completion epoch mismatch peer=" +
                std::to_string(word / 4U) + " expected=" + std::to_string(calls) +
                " actual=" + std::to_string(epochs[word]));
        enterStage("count-read");
        std::vector<int32_t> actualPrefix(c.experts), referenceCounts(c.experts, 0);
        std::vector<int64_t> actualExperts(c.experts / c.rankSize);
        Check(aclrtMemcpy(actualPrefix.data(), layout.sendCountsBytes, buffers[4],
            layout.sendCountsBytes, ACL_MEMCPY_DEVICE_TO_HOST), "read sendCountsOut");
        Check(aclrtMemcpy(actualExperts.data(), layout.expertTokenNumsBytes, buffers[3],
            layout.expertTokenNumsBytes, ACL_MEMCPY_DEVICE_TO_HOST), "read expertTokenNumsOut");
        const int64_t localExperts = c.experts / c.rankSize;
        for (int source = 0; source < c.rankSize; ++source) {
            TileXREpDemo::ExpertIdsConfig sourceConfig {c.bs, c.topK, c.experts,
                localExperts, c.rankSize, source, 2026};
            for (int32_t expert : TileXREpDemo::BuildBalancedShuffleExpertIds(sourceConfig)) {
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
        dispatch = ValidateDispatch(c, *host, layout, buffers, weightsHost);
        if (c.runMode == "breakdown") {
            enterStage("breakdown-read");
            breakdown = ReadDispatchProfile(c, *host);
        }
        success = true;
    } catch (const std::exception &error) {
        std::cerr << "EP_SERVER_DEDUP_FAIL rank=" << c.rank << " stage=" << stage << " batch=" << batch
            << " submission=" << submission << " submitted=" << submitted << " completed=" << completed
            << " warmup_submitted=" << warmupSubmitted << " warmup_completed=" << warmupCompleted
            << " " << error.what() << std::endl;
    }
    if (!safeCleanup) {
        std::cerr << "rank=" << c.rank << " cleanup=deferred-to-process-exit: batch completion unconfirmed" << std::endl;
        return 1;
    }
    auto cleanup = [&](int code, const char *name) {
        if (code != 0) {
            success = false;
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
    if (success) {
        std::string details = ",\"submitted\":" + std::to_string(submitted) + ",\"completed\":" +
            std::to_string(completed) + ",\"warmup_submitted\":" + std::to_string(warmupSubmitted) +
            ",\"warmup_completed\":" + std::to_string(warmupCompleted) + ",\"cleanup\":\"complete\"";
        if (!breakdown.empty()) Marker(c, "EP_SERVER_DEDUP_BREAKDOWN_PASS", breakdown);
        Marker(c, "EP_SERVER_DEDUP_DISPATCH_PASS", details + dispatch);
    }
    return success ? 0 : 1;
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
