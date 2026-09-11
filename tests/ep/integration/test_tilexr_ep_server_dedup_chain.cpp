#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iomanip>
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
#ifdef TILEXR_EP_MEMORY_BOUNDARY_TEST
#include "memory_boundary_test.h"
#endif

namespace {
using Clock = std::chrono::steady_clock;
using Deadline = Clock::time_point;

void Require(bool condition, const std::string &message)
{
    if (!condition) { throw std::runtime_error(message); }
}

void Check(int code, const char *operation)
{
    Require(code == 0, std::string(operation) + " ret=" + std::to_string(code));
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
    c.checkpoint = checkpoint == nullptr ? "ep-sdedup-p1-chain-single" : checkpoint;
    Require(c.checkpoint == "ep-sdedup-p1-chain-single" || c.checkpoint == "ep-sdedup-p1-chain-multi" ||
        c.checkpoint == "ep-sdedup-p1-memory-boundary",
        "unsupported TILEXR_VALIDATION_CHECKPOINT");
    const char *mode = std::getenv("TILEXR_VALIDATION_RUN_MODE");
    c.runMode = mode == nullptr ? "functional" : mode;
    Require(c.runMode == "functional" || c.runMode == "performance" || c.runMode == "breakdown",
        "unsupported TILEXR_VALIDATION_RUN_MODE");
    const char *loopsText = std::getenv("TILEXR_VALIDATION_LOOPS");
    const char *warmupText = std::getenv("TILEXR_VALIDATION_WARMUP");
    const int64_t loops = loopsText == nullptr ? (c.runMode == "functional" ? 1 : 50) :
        Number(loopsText, "TILEXR_VALIDATION_LOOPS");
    const int64_t warmup = warmupText == nullptr ? (c.runMode == "functional" ? 0 : 50) :
        Number(warmupText, "TILEXR_VALIDATION_WARMUP");
    Require(loops > 0 && loops <= UINT32_MAX, "TILEXR_VALIDATION_LOOPS must be in [1, UINT32_MAX]");
    Require(warmup <= UINT32_MAX, "TILEXR_VALIDATION_WARMUP must be in [0, UINT32_MAX]");
    c.loops = static_cast<uint32_t>(loops);
    c.warmup = static_cast<uint32_t>(warmup);
    Require(c.checkpoint != "ep-sdedup-p1-chain-single" ||
        (c.runMode == "functional" && c.loops == 1 && c.warmup == 0),
        "chain-single requires functional mode, loops=1, warmup=0");
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

// 建链、预热批和测量批共用监听 socket 及总期限；报文核对 run/build/rank/phase。
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
    // MemoryDomain 使用既有 8 MiB 数据区 + flag 区；空 kernel 不使用旧 Dispatch 的 2 GiB 分区。
    const uint64_t frameworkBytes = static_cast<uint64_t>(TileXR::TILEXR_MEMORY_DOMAIN_BUFFER_SIZE) *
        1024U * 1024U + TileXR::TILEXR_FLAG_BUFF_BYTES;
    Require(args.peerMemBytes >= frameworkBytes, "peer window does not cover communicator allocation");
    for (int rank = 0; rank < c.rankSize; ++rank) {
        const uintptr_t begin = reinterpret_cast<uintptr_t>(args.peerMems[rank]);
        Require(begin != 0 && begin % 32 == 0 && args.peerMemBytes <= UINTPTR_MAX - begin,
            "invalid peer window rank=" + std::to_string(rank));
    }
}

uint32_t ProfileBlockDim(const Config &c)
{
    int64_t vectorCores = 0;
    Check(aclrtGetDeviceInfo(static_cast<uint32_t>(c.device), ACL_DEV_ATTR_VECTOR_CORE_NUM, &vectorCores),
        "aclrtGetDeviceInfo");
    Require(vectorCores > 0 && vectorCores <= UINT32_MAX, "invalid vector core count");
    uint32_t blockDim = 0;
    Check(TileXREp::TileXREpResolveMemoryBlockDim(static_cast<uint32_t>(vectorCores), 1U,
        std::getenv("TILEXR_EP_DISPATCH_MEMORY_SERVER_DEDUP_BLOCK_DIM"), &blockDim), "profile block dimension");
    Require(blockDim <= TileXREp::kEpServerDedupProfileMaxCores, "profile core capacity exceeded");
    return blockDim;
}

std::string ReadProfile(const Config &c, const TileXR::CommArgs &args, uint32_t blockDim)
{
    using namespace TileXREp;
    const uint64_t bytes = static_cast<uint64_t>(blockDim) * sizeof(EpServerDedupProfileRecord);
    const uint64_t dataBytes = static_cast<uint64_t>(TileXR::TILEXR_MEMORY_DOMAIN_BUFFER_SIZE) * 1024U * 1024U;
    Require(kEpServerDedupProfileOffset <= dataBytes && bytes <= dataBytes - kEpServerDedupProfileOffset &&
        kEpServerDedupProfileOffset <= args.peerMemBytes && bytes <= args.peerMemBytes - kEpServerDedupProfileOffset,
        "profile records exceed own peer data region");
    std::array<EpServerDedupProfileRecord, kEpServerDedupProfileMaxCores> records {};
    Check(aclrtMemcpy(records.data(), sizeof(records), args.peerMems[c.rank] + kEpServerDedupProfileOffset,
        static_cast<std::size_t>(bytes), ACL_MEMCPY_DEVICE_TO_HOST), "read device breakdown records");
    const uint64_t generation = static_cast<uint64_t>(c.warmup) + c.loops;
    std::ostringstream out;
    out << ",\"generation\":" << generation << ",\"block_dim\":" << blockDim << ",\"records\":[";
    for (uint32_t core = 0; core < blockDim; ++core) {
        const auto &record = records[core];
        Require(record.marker == kEpServerDedupProfileMarker && record.version == kEpServerDedupProfileVersion &&
            record.recordBytes == sizeof(EpServerDedupProfileRecord) && record.rank == static_cast<uint32_t>(c.rank) &&
            record.core == core && record.blockDim == blockDim && record.generation == generation &&
            record.start > 0 && record.checked >= record.start && record.end >= record.checked,
            "invalid breakdown record core=" + std::to_string(core));
        if (core != 0) { out << ','; }
        out << "{\"core\":" << core << ",\"start\":" << record.start << ",\"checked\":" << record.checked
            << ",\"end\":" << record.end << '}';
    }
    return out.str() + ']';
}

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
    Check(TileXREp::TileXREpBuildMemoryServerDedupLayout(c.rankSize, c.bs, c.h, c.topK, c.experts, &layout),
        "layout");
    bool initialized = false, deviceSet = false, safeCleanup = true, success = false;
    aclrtStream stream = nullptr;
    TileXRCommPtr comm = nullptr;
    std::vector<void *> buffers;
    uint64_t submission = 0, submitted = 0, completed = 0, warmupSubmitted = 0, warmupCompleted = 0;
    std::string stage = "acl-init", batch = "initialization", breakdown;
    Barrier barrier(c, Clock::now() + std::chrono::seconds(90));
    try {
        Check(aclInit(nullptr), "aclInit"); initialized = true;
        stage = "device";
        Check(aclrtSetDevice(c.device), "aclrtSetDevice"); deviceSet = true;
        const char *soc = aclrtGetSocName();
        Require(soc != nullptr && (std::strncmp(soc, "Ascend950", 9) == 0 ||
            std::strncmp(soc, "Ascend910_9", 11) == 0 || std::strncmp(soc, "A5", 2) == 0), "A5 required");
        const uint32_t profileCores = c.runMode == "breakdown" ? ProfileBlockDim(c) : 0;
        Check(aclrtCreateStream(&stream), "aclrtCreateStream");
        stage = "communicator";
        Check(TileXRCommInitRankMemoryDomain(0, c.rankSize, c.rank, &comm), "TileXRCommInitRankMemoryDomain");
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
        stage = "allocation";
        const uint64_t sizes[] = {layout.xBytes, layout.expertIdsBytes, layout.expandXBytes,
            layout.expertTokenNumsBytes, layout.sendCountsBytes, layout.assistExBytes,
            layout.sourceMaskBytes, layout.groupBytes};
        const uint32_t alignments[] = {2, 4, 2, 8, 4, 32, 4, 32};
        std::vector<TileXREp::EpServerDedupAllocation> allocations;
        for (std::size_t i = 0; i < 8; ++i) {
            void *data = nullptr;
            Require(sizes[i] > 0 && sizes[i] <= SIZE_MAX, "allocation size overflow");
            Check(aclrtMalloc(&data, static_cast<std::size_t>(sizes[i]), ACL_MEM_MALLOC_HUGE_FIRST), "aclrtMalloc");
            buffers.push_back(data);
            int32_t device = -1;
            Check(aclrtGetDevice(&device), "aclrtGetDevice");
            allocations.push_back({data, sizes[i], sizes[i], alignments[i], device});
        }
        // Build deterministic, valid caller inputs. The stage-1 kernel may be
        // empty, but its ABI must receive real token and routing buffers.
        std::vector<uint8_t> xHost(static_cast<size_t>(layout.xBytes), 0);
        std::vector<int32_t> idsHost(static_cast<size_t>(c.bs * c.topK));
        TileXREpDemo::ExpertIdsConfig idsConfig {c.bs, c.topK, c.experts,
            c.experts / c.rankSize, c.rankSize, c.rank, 2026};
        idsHost = TileXREpDemo::BuildBalancedExpertIds(idsConfig);
        Require(idsHost.size() == static_cast<size_t>(c.bs * c.topK), "expert id construction failed");
        for (size_t i = 0; i < xHost.size(); ++i) xHost[i] = static_cast<uint8_t>(i & 0xffU);
        Check(aclrtMemcpy(buffers[0], xHost.size(), xHost.data(), xHost.size(), ACL_MEMCPY_HOST_TO_DEVICE), "copy x");
        Check(aclrtMemcpy(buffers[1], layout.expertIdsBytes, idsHost.data(), idsHost.size() * sizeof(int32_t), ACL_MEMCPY_HOST_TO_DEVICE), "copy expert ids");
        std::vector<uint8_t> zero(layout.expertTokenNumsBytes + layout.sendCountsBytes, 0);
        Check(aclrtMemset(buffers[3], layout.expertTokenNumsBytes, 0, layout.expertTokenNumsBytes), "clear token counts");
        Check(aclrtMemset(buffers[4], layout.sendCountsBytes, 0, layout.sendCountsBytes), "clear send counts");
        Check(TileXREp::TileXREpValidateServerDedupAllocations(allocations.data(), allocations.size(), c.device),
            "caller allocation validation");
        stage = "chain-barrier";
        safeCleanup = false;
        barrier.Run(0);
        Marker(c, "EP_SERVER_DEDUP_CHAIN_PASS", ",\"peer_count\":" + std::to_string(c.rankSize) +
            ",\"peer_mem_bytes\":" + std::to_string(host->peerMemBytes));
        auto runBatch = [&](uint32_t count, uint32_t phase, uint64_t &batchSubmitted, uint64_t &batchCompleted) {
            for (submission = 1; submission <= count; ++submission) {
                stage = "launch";
                Check(TileXRMoeEpDispatchMemoryServerDedup(buffers[0], static_cast<int32_t *>(buffers[1]), nullptr,
                    nullptr, TileXREp::TILEXR_EP_ACTIVE_MASK_NONE, nullptr, comm, c.bs, c.h, c.topK, c.experts,
                    c.rankSize, c.rank, 1, 0, 0, 0, 0, 0, static_cast<int64_t>(layout.globalBs), 0, buffers[2],
                    nullptr, nullptr, static_cast<int64_t *>(buffers[3]), static_cast<int32_t *>(buffers[4]),
                    nullptr, nullptr, static_cast<uint32_t *>(buffers[5]), static_cast<uint32_t *>(buffers[6]),
                    static_cast<uint32_t *>(buffers[7]), c.dtype, TileXR::TILEXR_DATA_TYPE_RESERVED, stream),
                    "TileXRMoeEpDispatchMemoryServerDedup");
                ++batchSubmitted;
            }
            submission = count;
            stage = "stream-sync";
            Check(aclrtSynchronizeStream(stream), "aclrtSynchronizeStream");
            stage = "batch-barrier";
            barrier.Run(phase);
            batchCompleted = count;
        };
        if (c.warmup != 0) {
            batch = "warmup";
            runBatch(c.warmup, 1, warmupSubmitted, warmupCompleted);
        }
        batch = "measurement";
        runBatch(c.loops, 2, submitted, completed);
        safeCleanup = true;
        if (c.runMode == "breakdown") {
            stage = "breakdown-read";
            breakdown = ReadProfile(c, *host, profileCores);
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
    for (auto data : buffers) { cleanup(aclrtFree(data), "aclrtFree"); }
    if (comm != nullptr) { cleanup(TileXRCommDestroy(comm), "TileXRCommDestroy"); }
    if (stream != nullptr) { cleanup(aclrtDestroyStream(stream), "aclrtDestroyStream"); }
    if (deviceSet) { cleanup(aclrtResetDevice(c.device), "aclrtResetDevice"); }
    if (initialized) { cleanup(aclFinalize(), "aclFinalize"); }
    if (success) {
        std::string details = ",\"submitted\":" + std::to_string(submitted) + ",\"completed\":" +
            std::to_string(completed) + ",\"warmup_submitted\":" + std::to_string(warmupSubmitted) +
            ",\"warmup_completed\":" + std::to_string(warmupCompleted) + ",\"cleanup\":\"complete\"";
        const bool multi = c.checkpoint == "ep-sdedup-p1-chain-multi";
        if (!breakdown.empty()) { Marker(c, "EP_SERVER_DEDUP_BREAKDOWN_PASS", breakdown); }
        Marker(c, multi ? "EP_SERVER_DEDUP_MULTI_ROUND_PASS" : "EP_SERVER_DEDUP_SINGLE_ROUND_PASS", details);
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
