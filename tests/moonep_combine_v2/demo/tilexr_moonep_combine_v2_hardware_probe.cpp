/*
 * 调用真实 Combine V2 验证数据及批次 ACL event 时间；使用 Host 屏障协调 rank，不读取私有时间戳 dump。
 */
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
#include <string>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include "acl/acl.h"
#include "combine_v2_host.h"
#include "combine_v2_schedule.h"
#include "tilexr_api.h"
#include "tilexr_moonep_combine_v2.h"
#include "tilexr_types.h"

namespace {

constexpr int kDeviceCount = 8;
constexpr int kDefaultCommDomain = 141;
constexpr int64_t kDefaultHiddenSize = 3584;
constexpr int64_t kDefaultTopK = 16;
constexpr int kDefaultExpertCount = 64;
constexpr uint32_t kAivCoreNum = 16;
constexpr uint32_t kQpCount = 32;
constexpr int kBarrierPortOffset = 97;
constexpr int kBarrierTimeoutSeconds = 60;

struct HostPort {
    std::string host;
    int port = 0;
};

struct Options {
    std::vector<int64_t> batchSizes {128};
    int warmup = 20;
    int iterations = 80;
    int experts = kDefaultExpertCount;
    int64_t hiddenSize = kDefaultHiddenSize;
    int64_t topK = kDefaultTopK;
    int commDomain = kDefaultCommDomain;
    int rank = -1;
    int worldSize = 0;
    int device = -1;
    bool reduceHidden = false;
    bool fusedWeight = false;
};

enum class OutputCheckResult {
    Passed,
    SelfOnlyFailed,
    Failed,
};

const char *OutputCheckName(OutputCheckResult result)
{
    switch (result) {
        case OutputCheckResult::Passed:
            return "passed";
        case OutputCheckResult::SelfOnlyFailed:
            return "self_only_failed";
        default:
            return "failed";
    }
}

[[noreturn]] void Abort(int rank, const std::string &step, int status)
{
    std::cerr << "[rank " << rank << "] " << step
              << " failed, status=" << status << std::endl;
    std::exit(status == 0 ? 1 : status);
}

void Usage(std::ostream &out, const char *program)
{
    out << "Usage: " << program << " [options]\n"
        << "  --bs N                 Run one batch size (default: 128)\n"
        << "  --bs-list N[,N...]     Run multiple batch sizes after one initialization\n"
        << "  --warmup N             Warmup launches per batch size (default: 20)\n"
        << "  --iterations N         Timed launches per batch size (default: 80)\n"
        << "  --experts N            Total expert count (default: 64)\n"
        << "  --hidden-size N        Hidden size H (default: 3584)\n"
        << "  --topk N               Router TopK K (default: 16)\n"
        << "  --comm-domain N        Shared-QP communication domain (default: 141)\n"
        << "  --rank N               Global rank (required)\n"
        << "  --world-size N         Global rank count (required)\n"
        << "  --device N             Local device id (default: rank modulo 8)\n"
        << "  --skip-iteration-barriers\n"
        << "                         Deprecated no-op; launches are always continuous\n"
        << "  --reduce-hidden        Include BF16 TopK hidden reduction in the kernel\n"
        << "  --fused-weight         Transfer and validate FP32 route weights in the same launch\n"
        << "  --help                 Show this help\n";
}

bool ParseInteger(const std::string &text, int64_t minValue,
    int64_t maxValue, int64_t *value)
{
    if (value == nullptr || text.empty()) {
        return false;
    }
    std::size_t consumed = 0;
    try {
        const long long parsed = std::stoll(text, &consumed, 10);
        if (consumed != text.size() || parsed < minValue || parsed > maxValue) {
            return false;
        }
        *value = static_cast<int64_t>(parsed);
        return true;
    } catch (...) {
        return false;
    }
}

bool ParseBatchSizes(const std::string &text, std::vector<int64_t> *values)
{
    if (values == nullptr || text.empty()) {
        return false;
    }
    std::vector<int64_t> parsed;
    std::istringstream input(text);
    std::string item;
    while (std::getline(input, item, ',')) {
        int64_t value = 0;
        if (!ParseInteger(item, 1, std::numeric_limits<int32_t>::max(),
                &value)) {
            return false;
        }
        if (std::find(parsed.begin(), parsed.end(), value) == parsed.end()) {
            parsed.push_back(value);
        }
    }
    if (parsed.empty()) {
        return false;
    }
    *values = parsed;
    return true;
}

bool ParseOptions(int argc, char **argv, Options *options, bool *showHelp,
    std::string *error)
{
    if (options == nullptr || showHelp == nullptr || error == nullptr) {
        return false;
    }
    bool batchSizeSet = false;
    *showHelp = false;
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument == "--help" || argument == "-h") {
            *showHelp = true;
            return true;
        }
        if (argument == "--skip-iteration-barriers") {
            continue;
        }

        if (argument == "--reduce-hidden") {
            options->reduceHidden = true;
            continue;
        }
        if (argument == "--fused-weight") {
            options->fusedWeight = true;
            continue;
        }
        if (index + 1 >= argc) {
            *error = "missing value for " + argument;
            return false;
        }
        const std::string value = argv[++index];
        if (argument == "--bs" || argument == "--bs-list") {
            if (batchSizeSet) {
                *error = "--bs and --bs-list are mutually exclusive";
                return false;
            }
            batchSizeSet = true;
            if (!ParseBatchSizes(value, &options->batchSizes) ||
                (argument == "--bs" && options->batchSizes.size() != 1U)) {
                *error = "invalid batch size list: " + value;
                return false;
            }
        } else if (argument == "--warmup") {
            int64_t parsed = 0;
            if (!ParseInteger(value, 0, std::numeric_limits<int>::max(),
                    &parsed)) {
                *error = "invalid warmup count: " + value;
                return false;
            }
            options->warmup = static_cast<int>(parsed);
        } else if (argument == "--iterations") {
            int64_t parsed = 0;
            if (!ParseInteger(value, 1, std::numeric_limits<int>::max(),
                    &parsed)) {
                *error = "invalid iteration count: " + value;
                return false;
            }
            options->iterations = static_cast<int>(parsed);
        } else if (argument == "--experts") {
            int64_t parsed = 0;
            if (!ParseInteger(value, 1, std::numeric_limits<int>::max(),
                    &parsed)) {
                *error = "invalid expert count: " + value;
                return false;
            }
            options->experts = static_cast<int>(parsed);
        } else if (argument == "--hidden-size") {
            int64_t parsed = 0;
            if (!ParseInteger(value, 1, std::numeric_limits<int32_t>::max(),
                    &parsed)) {
                *error = "invalid hidden size: " + value;
                return false;
            }
            options->hiddenSize = parsed;
        } else if (argument == "--topk") {
            int64_t parsed = 0;
            if (!ParseInteger(value, 1, 32, &parsed)) {
                *error = "invalid TopK: " + value;
                return false;
            }
            options->topK = parsed;
        } else if (argument == "--comm-domain") {
            int64_t parsed = 0;
            if (!ParseInteger(value, 1, std::numeric_limits<int>::max(),
                    &parsed)) {
                *error = "invalid communication domain: " + value;
                return false;
            }
            options->commDomain = static_cast<int>(parsed);
        } else if (argument == "--rank") {
            int64_t parsed = 0;
            if (!ParseInteger(value, 0, std::numeric_limits<int>::max(),
                    &parsed)) {
                *error = "invalid rank: " + value;
                return false;
            }
            options->rank = static_cast<int>(parsed);
        } else if (argument == "--world-size") {
            int64_t parsed = 0;
            if (!ParseInteger(value, 1, std::numeric_limits<int>::max(),
                    &parsed)) {
                *error = "invalid world size: " + value;
                return false;
            }
            options->worldSize = static_cast<int>(parsed);
        } else if (argument == "--device") {
            int64_t parsed = 0;
            if (!ParseInteger(value, 0, kDeviceCount - 1, &parsed)) {
                *error = "invalid device id: " + value;
                return false;
            }
            options->device = static_cast<int>(parsed);
        } else {
            *error = "unknown argument: " + argument;
            return false;
        }
    }
    if (options->rank < 0 || options->worldSize <= 0 ||
        options->rank >= options->worldSize) {
        *error = "--rank and --world-size must identify a valid rank";
        return false;
    }
    if (options->fusedWeight && options->commDomain ==
            std::numeric_limits<int>::max()) {
        *error = "--comm-domain leaves no distinct domain for fused weight";
        return false;
    }
    return true;
}

uint16_t FloatToBfloat16(float value)
{
    uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    const uint32_t roundToEven = 0x7fffU + ((bits >> 16U) & 1U);
    return static_cast<uint16_t>((bits + roundToEven) >> 16U);
}

uint16_t SourceValue(int sourceRank)
{
    const float value = 1.0F + static_cast<float>(sourceRank % 16) * 0.25F +
        static_cast<float>(sourceRank / 16) * 0.0625F;
    return FloatToBfloat16(value);
}

float WeightValue(int sourceRank, int64_t sourceSlot, uint32_t generation)
{
    const uint32_t payload = (static_cast<uint32_t>(sourceRank) * 131071U +
        static_cast<uint32_t>(sourceSlot) + generation * 524287U) &
        0x007FFFFFU;
    const uint32_t bits = 0x3F000000U | payload;
    float value = 0.0F;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

void CheckAcl(int rank, const std::string &step, aclError status)
{
    if (status != ACL_SUCCESS) {
        Abort(rank, step, static_cast<int>(status));
    }
}

bool ParseHostPort(const std::string &text, HostPort *endpoint)
{
    const std::size_t separator = text.rfind(':');
    if (endpoint == nullptr || separator == std::string::npos ||
        separator == 0 || separator + 1 >= text.size()) {
        return false;
    }
    int64_t port = 0;
    if (!ParseInteger(text.substr(separator + 1), 1, 65535, &port)) {
        return false;
    }
    in_addr address {};
    const std::string host = text.substr(0, separator);
    if (inet_pton(AF_INET, host.c_str(), &address) != 1) {
        return false;
    }
    endpoint->host = host;
    endpoint->port = static_cast<int>(port);
    return true;
}

bool GetBarrierEndpoint(HostPort *endpoint)
{
    const char *configured = std::getenv("TILEXR_DEMO_BARRIER_ADDR");
    if (configured != nullptr && configured[0] != '\0') {
        return ParseHostPort(configured, endpoint);
    }
    const char *commId = std::getenv("TILEXR_COMM_ID");
    HostPort commEndpoint;
    if (commId == nullptr || !ParseHostPort(commId, &commEndpoint)) {
        return false;
    }
    endpoint->host = commEndpoint.host;
    endpoint->port = commEndpoint.port + kBarrierPortOffset;
    if (endpoint->port > 65535) {
        endpoint->port = commEndpoint.port - kBarrierPortOffset;
    }
    return endpoint->port > 0;
}

bool SendAll(int fd, const void *data, std::size_t bytes)
{
    const uint8_t *cursor = static_cast<const uint8_t *>(data);
    while (bytes > 0) {
        const ssize_t sent = send(fd, cursor, bytes, 0);
        if (sent < 0 && errno == EINTR) {
            continue;
        }
        if (sent <= 0) {
            return false;
        }
        cursor += sent;
        bytes -= static_cast<std::size_t>(sent);
    }
    return true;
}

bool RecvAll(int fd, void *data, std::size_t bytes)
{
    uint8_t *cursor = static_cast<uint8_t *>(data);
    while (bytes > 0) {
        const ssize_t received = recv(fd, cursor, bytes, 0);
        if (received < 0 && errno == EINTR) {
            continue;
        }
        if (received <= 0) {
            return false;
        }
        cursor += received;
        bytes -= static_cast<std::size_t>(received);
    }
    return true;
}

void SetSocketTimeout(int fd)
{
    timeval timeout {};
    timeout.tv_sec = kBarrierTimeoutSeconds;
    (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    (void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
}

bool BarrierServer(int world, const HostPort &endpoint, bool localSuccess)
{
    const int listenFd = socket(AF_INET, SOCK_STREAM, 0);
    if (listenFd < 0) {
        return false;
    }
    int reuse = 1;
    (void)setsockopt(listenFd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    SetSocketTimeout(listenFd);

    sockaddr_in address {};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(static_cast<uint16_t>(endpoint.port));
    if (bind(listenFd, reinterpret_cast<sockaddr *>(&address),
            sizeof(address)) != 0 || listen(listenFd, world) != 0) {
        close(listenFd);
        return false;
    }

    bool globalSuccess = localSuccess;
    bool exchangeOk = true;
    std::vector<int> clients;
    clients.reserve(static_cast<std::size_t>(world - 1));
    for (int peer = 1; peer < world; ++peer) {
        const int client = accept(listenFd, nullptr, nullptr);
        if (client < 0) {
            exchangeOk = false;
            break;
        }
        SetSocketTimeout(client);
        uint8_t arrived = 0;
        if (!RecvAll(client, &arrived, sizeof(arrived)) || arrived > 1U) {
            exchangeOk = false;
        }
        globalSuccess = globalSuccess && arrived == 1U;
        clients.push_back(client);
        if (!exchangeOk) {
            break;
        }
    }

    // Retire this barrier generation before any client can enter the next one.
    close(listenFd);
    const uint8_t release = exchangeOk && globalSuccess ? 1U : 0U;
    for (const int client : clients) {
        if (!SendAll(client, &release, sizeof(release))) {
            exchangeOk = false;
        }
        close(client);
    }
    return exchangeOk && globalSuccess;
}

bool BarrierClient(const HostPort &endpoint, bool localSuccess)
{
    sockaddr_in address {};
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<uint16_t>(endpoint.port));
    if (inet_pton(AF_INET, endpoint.host.c_str(), &address.sin_addr) != 1) {
        return false;
    }

    const auto deadline = std::chrono::steady_clock::now() +
        std::chrono::seconds(kBarrierTimeoutSeconds);
    int fd = -1;
    while (std::chrono::steady_clock::now() < deadline) {
        fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) {
            return false;
        }
        SetSocketTimeout(fd);
        if (connect(fd, reinterpret_cast<sockaddr *>(&address),
                sizeof(address)) == 0) {
            break;
        }
        close(fd);
        fd = -1;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    if (fd < 0) {
        return false;
    }

    const uint8_t arrived = localSuccess ? 1U : 0U;
    uint8_t release = 0;
    const bool ok = SendAll(fd, &arrived, sizeof(arrived)) &&
        RecvAll(fd, &release, sizeof(release)) && release == 1U;
    close(fd);
    return ok;
}

bool BarrierAll(int rank, int world, const std::string &step,
    bool localSuccess = true)
{
    if (world <= 1) {
        return localSuccess;
    }
    HostPort endpoint;
    if (!GetBarrierEndpoint(&endpoint)) {
        std::cerr << "[rank " << rank
                  << "] invalid TILEXR_DEMO_BARRIER_ADDR" << std::endl;
        return false;
    }
    const bool ok = rank == 0 ? BarrierServer(world, endpoint, localSuccess) :
        BarrierClient(endpoint, localSuccess);
    if (!ok) {
        std::cerr << "[rank " << rank << "] barrier failed after " << step
                  << " at " << endpoint.host << ':' << endpoint.port
                  << std::endl;
    }
    return ok;
}

void LaunchCombine(int rank, void *workspace, const int32_t *dst,
    TileXRCommPtr comm, TileXRCommPtr weightMemoryComm, int64_t bs,
    int64_t hiddenSize, int64_t topK, aclrtStream stream, bool reduceHidden,
    bool fusedWeight, const float *routeWeightsNvs, float *routeWeightsSk,
    uint64_t reduceOutputOffset, uint64_t *activeOutputOffset)
{
    const int64_t slots = bs * topK;
    int ret = TILEXR_MOONEP_SUCCESS;
    if (reduceHidden || fusedWeight) {
        TileXRMoonEp::CombineV2Params params {};
        params.registeredWorkspace = workspace;
        params.dstLocal = dst;
        params.comm = comm;
        params.bs = bs;
        params.h = hiddenSize;
        params.topK = topK;
        params.nvS = slots;
        params.aivCoreNum = kAivCoreNum;
        params.activeOutputOffset = activeOutputOffset;
        params.dtype = TILEXR_MOONEP_DTYPE_BFLOAT16;
        params.reduceHidden = reduceHidden;
        params.weightMemoryComm = fusedWeight ? weightMemoryComm : nullptr;
        params.routeWeightsNvs = fusedWeight ? routeWeightsNvs : nullptr;
        params.routeWeightsSk = fusedWeight ? routeWeightsSk : nullptr;
        params.stream = stream;
        ret = TileXRMoonEp::TileXRMoonEpRunCombineV2(params);
        if (ret == TILEXR_MOONEP_SUCCESS && reduceHidden) {
            *activeOutputOffset = reduceOutputOffset;
        }
    } else {
        ret = TileXRMoonEpCombineV2(workspace, dst, comm, bs,
            hiddenSize, topK, slots, kAivCoreNum, activeOutputOffset,
            TILEXR_MOONEP_DTYPE_BFLOAT16, stream);
    }
    if (ret != TILEXR_MOONEP_SUCCESS) {
        Abort(rank, reduceHidden ? "TileXRMoonEpRunCombineV2 reduce" :
            "TileXRMoonEpCombineV2", ret);
    }
}

bool CheckWeights(int rank, int world, int64_t bs, int64_t topK,
    const float *routeWeightsSk, uint32_t generation)
{
    const int64_t slots = bs * topK;
    const int64_t slotsPerRank = slots / world;
    std::vector<float> output(static_cast<std::size_t>(slots));
    const std::size_t bytes = output.size() * sizeof(float);
    CheckAcl(rank, "weight output D2H copy", aclrtMemcpy(output.data(), bytes,
        routeWeightsSk, bytes, ACL_MEMCPY_DEVICE_TO_HOST));
    for (int64_t targetSlot = 0; targetSlot < slots; ++targetSlot) {
        const int sourceRank = static_cast<int>(targetSlot / slotsPerRank);
        const int64_t sourceLocalIndex = targetSlot % slotsPerRank;
        const int64_t sourceSlot = sourceLocalIndex * world + rank;
        const float expected = WeightValue(
            sourceRank, sourceSlot, generation);
        if (output[static_cast<std::size_t>(targetSlot)] != expected) {
            std::cerr << "[rank " << rank << "] weight mismatch"
                      << " bs=" << bs
                      << " target_slot=" << targetSlot
                      << " source_rank=" << sourceRank
                      << " source_slot=" << sourceSlot
                      << " got=" << output[static_cast<std::size_t>(targetSlot)]
                      << " expected=" << expected << std::endl;
            return false;
        }
    }
    return true;
}

OutputCheckResult CheckOutput(int rank, int world, int64_t bs,
    int64_t hiddenSize, int64_t topK, const void *workspace,
    uint64_t activeOutputOffset, bool reduceHidden)
{
    const int64_t slots = bs * topK;
    const int64_t outputRows = reduceHidden ? bs : slots;
    const std::size_t outputElements = static_cast<std::size_t>(outputRows) *
        static_cast<std::size_t>(hiddenSize);
    const std::size_t outputBytes = outputElements * sizeof(uint16_t);
    std::vector<uint16_t> output(outputElements);
    const void *outputDevice = static_cast<const uint8_t *>(workspace) +
        activeOutputOffset;
    CheckAcl(rank, "output D2H copy", aclrtMemcpy(output.data(), outputBytes,
        outputDevice, outputBytes, ACL_MEMCPY_DEVICE_TO_HOST));

    const int64_t rowsPerSourceRank = outputRows / world;
    bool selfMismatch = false;
    for (int64_t row = 0; row < outputRows; ++row) {
        const int sourceRank = static_cast<int>(row / rowsPerSourceRank);
        if (sourceRank == rank && selfMismatch) {
            continue;
        }
        const uint16_t expected = reduceHidden ? FloatToBfloat16(
            (1.0F + static_cast<float>(sourceRank % 16) * 0.25F +
                static_cast<float>(sourceRank / 16) * 0.0625F) *
                    static_cast<float>(topK)) : SourceValue(sourceRank);
        const std::size_t rowOffset = static_cast<std::size_t>(row) *
            static_cast<std::size_t>(hiddenSize);
        for (int64_t column = 0; column < hiddenSize; ++column) {
            const std::size_t index = rowOffset +
                static_cast<std::size_t>(column);
            if (output[index] != expected) {
                std::cerr << "[rank " << rank << "] output mismatch"
                          << " bs=" << bs
                          << " row=" << row
                          << " column=" << column
                          << " source_rank=" << sourceRank
                          << " got=" << output[index]
                          << " expected=" << expected << std::endl;
                if (sourceRank != rank) {
                    return OutputCheckResult::Failed;
                }
                selfMismatch = true;
                break;
            }
        }
    }
    return selfMismatch ? OutputCheckResult::SelfOnlyFailed :
        OutputCheckResult::Passed;
}

void ReportFirstKernelFailure(int rank, const void *workspace,
    const TileXRMoonEp::CombineV2Layout &layout)
{
    const std::size_t recordCount = static_cast<std::size_t>(
        layout.failureBytes /
        sizeof(TileXRMoonEp::MoonEpCombineV2FailureRecord));
    std::vector<TileXRMoonEp::MoonEpCombineV2FailureRecord> records(
        recordCount);
    const void *failureDevice = static_cast<const uint8_t *>(workspace) +
        layout.failureOffset;
    CheckAcl(rank, "failure record D2H copy", aclrtMemcpy(records.data(),
        layout.failureBytes, failureDevice, layout.failureBytes,
        ACL_MEMCPY_DEVICE_TO_HOST));
    const TileXRMoonEp::MoonEpCombineV2FailureRecord *selected = nullptr;
    for (const auto &record : records) {
        if ((record.marker & ~1U) !=
                TileXRMoonEp::kMoonEpCombineV2FailureMarker ||
            record.status == TileXRMoonEp::MOONEP_COMBINE_V2_SUCCESS) {
            continue;
        }
        if (selected == nullptr ||
            (selected->status ==
                    TileXRMoonEp::MOONEP_COMBINE_V2_COLLECTIVE_STATUS_ERROR &&
                record.status !=
                    TileXRMoonEp::MOONEP_COMBINE_V2_COLLECTIVE_STATUS_ERROR)) {
            selected = &record;
        }
    }
    if (selected != nullptr) {
        std::cerr << "COMBINE_V2_FAILURE"
                  << " rank=" << rank
                  << " status=" << selected->status
                  << " source_rank=" << selected->rank
                  << " core=" << selected->core
                  << " step=" << selected->step
                  << " peer=" << selected->peer
                  << " lane=" << selected->lane
                  << " qp=" << selected->qp
                  << " cq_status=" << selected->cqStatus
                  << " expected=" << selected->expected
                  << " observed=" << selected->observed
                  << std::endl;
        return;
    }
    std::cerr << "COMBINE_V2_FAILURE rank=" << rank
              << " status=unavailable" << std::endl;
}

} // namespace

int main(int argc, char **argv)
{
    Options options;
    bool showHelp = false;
    std::string parseError;
    if (!ParseOptions(argc, argv, &options, &showHelp, &parseError)) {
        std::cerr << parseError << '\n';
        Usage(std::cerr, argv[0]);
        return 2;
    }
    if (showHelp) {
        Usage(std::cout, argv[0]);
        return 0;
    }
    const int rank = options.rank;
    const int world = options.worldSize;
    if (!TileXRMoonEp::MoonEpCombineV2RankSizeSupported(
            static_cast<uint32_t>(world))) {
        Abort(rank, "unsupported Combine V2 world size", world);
    }
    if (options.experts % world != 0) {
        Abort(rank, "expert count must be divisible by world size",
            options.experts);
    }
    for (const int64_t bs : options.batchSizes) {
        if (bs % world != 0 || bs >
            std::numeric_limits<int32_t>::max() / options.topK) {
            Abort(rank, "batch size must be divisible by world size", 1);
        }
    }

    const int device = options.device >= 0 ? options.device :
        rank % kDeviceCount;
    CheckAcl(rank, "aclInit", aclInit(nullptr));
    CheckAcl(rank, "aclrtSetDevice", aclrtSetDevice(device));

    aclrtStream stream = nullptr;
    CheckAcl(rank, "aclrtCreateStream", aclrtCreateStream(&stream));
    aclrtEvent startEvent = nullptr;
    aclrtEvent stopEvent = nullptr;
    CheckAcl(rank, "aclrtCreateEvent start", aclrtCreateEvent(&startEvent));
    CheckAcl(rank, "aclrtCreateEvent stop", aclrtCreateEvent(&stopEvent));

    TileXRCommPtr comm = nullptr;
    int ret = TileXRCommInitRankWithSharedQpDomain(
        options.commDomain, world, rank, &comm);
    if (ret != TileXR::TILEXR_SUCCESS) {
        Abort(rank, "TileXRCommInitRankWithSharedQpDomain", ret);
    }
    TileXRCommPtr weightMemoryComm = nullptr;
    if (options.fusedWeight) {
        ret = TileXRCommInitRankMemoryDomain(
            options.commDomain + 1, world, rank, &weightMemoryComm);
        if (ret != TileXR::TILEXR_SUCCESS) {
            Abort(rank, "TileXRCommInitRankMemoryDomain", ret);
        }
    }

    TileXR::CommArgs *commArgs = nullptr;
    uint32_t qpCount = 0;
    ret = TileXRGetCommArgsHost(comm, commArgs);
    if (ret != TileXR::TILEXR_SUCCESS || commArgs == nullptr ||
        (commArgs->extraFlag & TileXR::ExtraFlag::UDMA_SHARED_QP) == 0U) {
        Abort(rank, "shared-QP CommArgs validation", ret);
    }
    ret = TileXRUDMAGetQpCount(comm, &qpCount);
    if (ret != TileXR::TILEXR_SUCCESS || qpCount != kQpCount) {
        Abort(rank, "TileXRUDMAGetQpCount", ret);
    }

    uint64_t maxWorkspaceBytes = 0;
    int64_t maxBs = 0;
    for (const int64_t bs : options.batchSizes) {
        uint64_t workspaceBytes = 0;
        uint64_t outputOffsets[2] = {};
        ret = TileXRMoonEpCombineGetWorkspaceSizeV2(bs, options.hiddenSize, options.topK,
            bs * options.topK, TILEXR_MOONEP_DTYPE_BFLOAT16, &workspaceBytes,
            &outputOffsets[0], &outputOffsets[1]);
        if (ret != TILEXR_MOONEP_SUCCESS || workspaceBytes == 0) {
            Abort(rank, "TileXRMoonEpCombineGetWorkspaceSizeV2", ret);
        }
        maxWorkspaceBytes = std::max(maxWorkspaceBytes, workspaceBytes);
        maxBs = std::max(maxBs, bs);
    }

    void *workspace = nullptr;
    int32_t *dst = nullptr;
    float *routeWeightsNvs = nullptr;
    float *routeWeightsSk = nullptr;
    const std::size_t maxDstBytes = static_cast<std::size_t>(maxBs * options.topK) *
        sizeof(int32_t);
    const std::size_t maxWeightBytes = static_cast<std::size_t>(maxBs * options.topK) *
        sizeof(float);
    CheckAcl(rank, "aclrtMalloc workspace", aclrtMalloc(&workspace,
        static_cast<std::size_t>(maxWorkspaceBytes), ACL_MEM_MALLOC_HUGE_FIRST));
    CheckAcl(rank, "aclrtMalloc destinations", aclrtMalloc(
        reinterpret_cast<void **>(&dst), maxDstBytes,
        ACL_MEM_MALLOC_HUGE_FIRST));
    if (options.fusedWeight) {
        CheckAcl(rank, "aclrtMalloc route weights input", aclrtMalloc(
            reinterpret_cast<void **>(&routeWeightsNvs), maxWeightBytes,
            ACL_MEM_MALLOC_HUGE_FIRST));
        CheckAcl(rank, "aclrtMalloc route weights output", aclrtMalloc(
            reinterpret_cast<void **>(&routeWeightsSk), maxWeightBytes,
            ACL_MEM_MALLOC_HUGE_FIRST));
    }
    CheckAcl(rank, "aclrtMemset workspace", aclrtMemset(workspace,
        static_cast<std::size_t>(maxWorkspaceBytes), 0,
        static_cast<std::size_t>(maxWorkspaceBytes)));

    TileXRUDMAMemHandle handle = 0;
    ret = TileXRUDMARegister(comm, static_cast<GM_ADDR>(workspace),
        static_cast<std::size_t>(maxWorkspaceBytes), &handle);
    if (ret != TileXR::TILEXR_SUCCESS) {
        Abort(rank, "TileXRUDMARegister", ret);
    }
    if (!BarrierAll(rank, world, "workspace registration")) {
        Abort(rank, "workspace registration barrier", 1);
    }
    if (rank == 0) {
        std::cout << "COMBINE_V2_SETUP ranks=" << world
                  << " devices_per_host=" << kDeviceCount
                  << " experts=" << options.experts
                  << " k=" << options.topK
                  << " h=" << options.hiddenSize
                  << " dtype=bf16"
                  << " qp_count=" << qpCount
                  << " max_bs=" << maxBs
                  << " workspace_bytes=" << maxWorkspaceBytes
                  << " reduce=" << (options.reduceHidden ? "enabled" : "disabled")
                  << " fused_weight=" << (options.fusedWeight ? "enabled" : "disabled")
                  << std::endl;
    }

    for (const int64_t bs : options.batchSizes) {
        const int64_t slots = bs * options.topK;
        uint64_t caseWorkspaceBytes = 0;
        uint64_t outputOffsets[2] = {};
        ret = TileXRMoonEpCombineGetWorkspaceSizeV2(bs, options.hiddenSize,
            options.topK, slots, TILEXR_MOONEP_DTYPE_BFLOAT16, &caseWorkspaceBytes,
            &outputOffsets[0], &outputOffsets[1]);
        if (ret != TILEXR_MOONEP_SUCCESS || caseWorkspaceBytes == 0) {
            Abort(rank, "TileXRMoonEpCombineGetWorkspaceSizeV2", ret);
        }
        TileXRMoonEp::CombineV2Layout caseLayout {};
        ret = TileXRMoonEp::TileXRMoonEpBuildCombineV2Layout(
            bs, options.hiddenSize, options.topK, slots,
            TILEXR_MOONEP_DTYPE_BFLOAT16, &caseLayout);
        if (ret != TILEXR_MOONEP_SUCCESS) {
            Abort(rank, "TileXRMoonEpBuildCombineV2Layout", ret);
        }
        const std::size_t sourceElements = static_cast<std::size_t>(slots) *
            static_cast<std::size_t>(options.hiddenSize);
        const std::size_t sourceBytes = sourceElements * sizeof(uint16_t);
        const std::size_t dstBytes = static_cast<std::size_t>(slots) *
            sizeof(int32_t);
        std::vector<uint16_t> source(sourceElements, SourceValue(rank));
        std::vector<int32_t> destinations(static_cast<std::size_t>(slots));
        std::vector<float> routeWeights;
        if (options.fusedWeight) {
            routeWeights.resize(static_cast<std::size_t>(slots));
        }
        const int64_t slotsPerRank = slots / world;
        for (int64_t slot = 0; slot < slots; ++slot) {
            const int64_t targetRank = slot % world;
            const int64_t targetSlot = static_cast<int64_t>(rank) *
                slotsPerRank + slot / world;
            destinations[static_cast<std::size_t>(slot)] =
                static_cast<int32_t>(targetRank * slots + targetSlot);
            if (options.fusedWeight) {
                routeWeights[static_cast<std::size_t>(slot)] =
                    WeightValue(rank, slot, 0U);
            }
        }
        CheckAcl(rank, "input H2D copy", aclrtMemcpy(workspace,
            static_cast<std::size_t>(maxWorkspaceBytes), source.data(),
            sourceBytes, ACL_MEMCPY_HOST_TO_DEVICE));
        CheckAcl(rank, "destinations H2D copy", aclrtMemcpy(dst,
            maxDstBytes, destinations.data(), dstBytes,
            ACL_MEMCPY_HOST_TO_DEVICE));
        if (options.fusedWeight) {
            const std::size_t weightBytes = static_cast<std::size_t>(slots) *
                sizeof(float);
            CheckAcl(rank, "route weights H2D copy", aclrtMemcpy(
                routeWeightsNvs, maxWeightBytes, routeWeights.data(),
                weightBytes, ACL_MEMCPY_HOST_TO_DEVICE));
            CheckAcl(rank, "route weights output clear", aclrtMemset(
                routeWeightsSk, maxWeightBytes, 0, weightBytes));
        }
        source.clear();
        source.shrink_to_fit();
        destinations.clear();
        destinations.shrink_to_fit();
        routeWeights.clear();
        routeWeights.shrink_to_fit();
        if (!BarrierAll(rank, world, "case inputs ready")) {
            Abort(rank, "case input barrier", 1);
        }

        uint64_t activeOutputOffset = 0;
        LaunchCombine(rank, workspace, dst, comm, weightMemoryComm, bs,
            options.hiddenSize, options.topK, stream, options.reduceHidden,
            options.fusedWeight, routeWeightsNvs, routeWeightsSk,
            caseLayout.outputOffset, &activeOutputOffset);
        CheckAcl(rank, "correctness stream synchronization",
            aclrtSynchronizeStream(stream));
        const OutputCheckResult outputResult = CheckOutput(
            rank, world, bs, options.hiddenSize, options.topK, workspace,
            activeOutputOffset, options.reduceHidden);
        bool weightResult = !options.fusedWeight ||
            CheckWeights(rank, world, bs, options.topK, routeWeightsSk, 0U);
        if (outputResult != OutputCheckResult::Passed) {
            ReportFirstKernelFailure(rank, workspace, caseLayout);
        }
        if (!BarrierAll(rank, world, "correctness validation",
                outputResult == OutputCheckResult::Passed && weightResult)) {
            Abort(rank, "correctness validation barrier", 1);
        }

        if (options.fusedWeight) {
            const std::size_t weightBytes = static_cast<std::size_t>(slots) *
                sizeof(float);
            std::vector<float> continuousWeights(
                static_cast<std::size_t>(slots));
            for (int64_t slot = 0; slot < slots; ++slot) {
                continuousWeights[static_cast<std::size_t>(slot)] =
                    WeightValue(rank, slot, 1U);
            }
            CheckAcl(rank, "continuous route weights H2D copy", aclrtMemcpy(
                routeWeightsNvs, maxWeightBytes, continuousWeights.data(),
                weightBytes, ACL_MEMCPY_HOST_TO_DEVICE));
            CheckAcl(rank, "continuous route weights output clear", aclrtMemset(
                routeWeightsSk, maxWeightBytes, 0, weightBytes));
        }
        if (!BarrierAll(rank, world, "continuous inputs ready")) {
            Abort(rank, "continuous input barrier", 1);
        }

        for (int iteration = 0; iteration < options.warmup; ++iteration) {
            LaunchCombine(rank, workspace, dst, comm, weightMemoryComm, bs,
                options.hiddenSize, options.topK, stream, options.reduceHidden,
                options.fusedWeight, routeWeightsNvs, routeWeightsSk,
                caseLayout.outputOffset, &activeOutputOffset);
        }
        if (options.warmup > 0) {
            CheckAcl(rank, "warmup stream synchronization",
                aclrtSynchronizeStream(stream));
        }
        if (!BarrierAll(rank, world, "timed batch start")) {
            Abort(rank, "timed batch start barrier", 1);
        }

        CheckAcl(rank, "aclrtRecordEvent batch start",
            aclrtRecordEvent(startEvent, stream));
        for (int iteration = 0; iteration < options.iterations; ++iteration) {
            LaunchCombine(rank, workspace, dst, comm, weightMemoryComm, bs,
                options.hiddenSize, options.topK, stream, options.reduceHidden,
                options.fusedWeight, routeWeightsNvs, routeWeightsSk,
                caseLayout.outputOffset, &activeOutputOffset);
        }
        CheckAcl(rank, "aclrtRecordEvent batch stop",
            aclrtRecordEvent(stopEvent, stream));
        CheckAcl(rank, "aclrtSynchronizeEvent batch stop",
            aclrtSynchronizeEvent(stopEvent));
        float batchElapsedMs = 0.0F;
        CheckAcl(rank, "aclrtEventElapsedTime batch",
            aclrtEventElapsedTime(&batchElapsedMs, startEvent, stopEvent));
        const float averageMs = batchElapsedMs /
            static_cast<float>(options.iterations);
        if (options.fusedWeight) {
            weightResult = weightResult &&
                CheckWeights(rank, world, bs, options.topK, routeWeightsSk, 1U);
        }
        if (!BarrierAll(rank, world, "continuous weight validation",
                weightResult)) {
            Abort(rank, "continuous weight validation barrier", 1);
        }

        std::cout << std::fixed << std::setprecision(6)
                  << "COMBINE_V2_RANK_PERF bs=" << bs
                  << " rank=" << rank
                  << " iterations=" << options.iterations
                  << " total_ms=" << batchElapsedMs
                  << " avg_ms=" << averageMs
                  << " timing_source=batch_acl_event"
                  << " reduce=" << (options.reduceHidden ? "enabled" : "disabled")
                  << " fused_weight=" << (options.fusedWeight ? "enabled" : "disabled")
                  << " weight_correctness=" << (weightResult ? "passed" : "failed")
                  << " correctness=" << OutputCheckName(outputResult)
                  << std::endl;
    }

    const bool casesSynchronized = BarrierAll(rank, world,
        "all benchmark cases");
    const int unregisterRet = TileXRUDMAUnregister(comm, handle);
    const bool unregisterSynchronized = BarrierAll(rank, world,
        "workspace unregistration",
        unregisterRet == TileXR::TILEXR_SUCCESS);
    const int destroyWeightRet = weightMemoryComm == nullptr ?
        TileXR::TILEXR_SUCCESS : TileXRCommDestroy(weightMemoryComm);
    const int destroyRet = TileXRCommDestroy(comm);
    const aclError destroyStartRet = aclrtDestroyEvent(startEvent);
    const aclError destroyStopRet = aclrtDestroyEvent(stopEvent);
    const aclError freeDstRet = aclrtFree(dst);
    const aclError freeWeightInputRet = routeWeightsNvs == nullptr ?
        ACL_SUCCESS : aclrtFree(routeWeightsNvs);
    const aclError freeWeightOutputRet = routeWeightsSk == nullptr ?
        ACL_SUCCESS : aclrtFree(routeWeightsSk);
    const aclError freeWorkspaceRet = aclrtFree(workspace);
    const aclError destroyStreamRet = aclrtDestroyStream(stream);
    const aclError resetRet = aclrtResetDevice(device);
    const aclError finalizeRet = aclFinalize();
    const bool cleanupOk = unregisterRet == TileXR::TILEXR_SUCCESS &&
        destroyWeightRet == TileXR::TILEXR_SUCCESS &&
        destroyRet == TileXR::TILEXR_SUCCESS &&
        destroyStartRet == ACL_SUCCESS && destroyStopRet == ACL_SUCCESS &&
        freeDstRet == ACL_SUCCESS && freeWeightInputRet == ACL_SUCCESS &&
        freeWeightOutputRet == ACL_SUCCESS && freeWorkspaceRet == ACL_SUCCESS &&
        destroyStreamRet == ACL_SUCCESS && resetRet == ACL_SUCCESS &&
        finalizeRet == ACL_SUCCESS;
    return casesSynchronized && unregisterSynchronized && cleanupOk ? 0 : 1;
}
