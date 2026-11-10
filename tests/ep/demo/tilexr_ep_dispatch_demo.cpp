/*
 * 创建各 rank 的 EP 输入并校验 Dispatch/Combine 输出，支持 Memory 与 UDMA backend。
 * perf 模式保留暖机、测量前后 barrier 和 stream 完成等待，耗时由外部 profiler 采集。
 * main 持有 communicator、stream 和 device buffers，并在退出路径调用 Cleanup。
 */
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <limits>
#include <string>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include "acl/acl.h"
#include "ep_memory_window.h"
#include "mxfp8_golden.h"
#include "tilexr_ep_expert_ids.h"
#include "tilexr_api.h"
#include "tilexr_ep.h"
#include "tilexr_types.h"

namespace {

constexpr int64_t kDefaultBs = 4;
constexpr int64_t kDefaultH = 8;
constexpr int64_t kDefaultTopK = 2;
constexpr int64_t kAssistInts = 4;
constexpr uint16_t kFp16One = 0x3c00;
constexpr uint16_t kBf16One = 0x3f80;
constexpr std::size_t kUdmaCacheLineBytes = 64;
constexpr std::size_t kUdmaRegistrationAlignment = 2 * 1024 * 1024;

TileXRUDMAMemHandle g_workspaceHandle = 0;
bool g_workspaceRegistered = false;

constexpr std::size_t AssistInts(bool useMemory)
{
    return useMemory ? static_cast<std::size_t>(TileXREp::kEpMemoryAssistInfoWidth) :
        static_cast<std::size_t>(kAssistInts);
}

enum class ExpertListMode {
    UNIFORM,
    RANDOM,
    EXPLICIT,
};

struct DemoConfig {
    int64_t bs = kDefaultBs;
    int64_t h = kDefaultH;
    int64_t topK = kDefaultTopK;
    int64_t moeExpertNum = 8;
    int64_t sharedExpertNum = 0;
    int64_t sharedExpertRankNum = 0;
    int64_t tpWorldSize = 0;
    int64_t tpRankId = 0;
    int64_t maxRoutesPerRank() const
    {
        return bs * (topK + sharedExpertNum);
    }

    int64_t effectiveTpWorldSize() const
    {
        return tpWorldSize == 0 ? 1 : tpWorldSize;
    }

    int64_t expandedElements() const
    {
        return maxRoutesPerRank() * effectiveTpWorldSize() * h;
    }

    std::vector<int32_t> expertIds;
    std::vector<std::vector<int32_t>> expertIdsByRank;
};

enum class DemoBackend {
    UDMA,
    MEMORY,
};

enum class DemoRunMode {
    DISPATCH,
    COMBINE,
    DISPATCH_COMBINE,
};

struct HostPort {
    std::string host;
    int port;
};

bool EnvEnabled(const char *name)
{
    const char *value = std::getenv(name);
    return value != nullptr && value[0] != '\0' && std::string(value) != "0";
}

int GetEnvInt(const char *name, int fallback)
{
    const char *value = std::getenv(name);
    if (value == nullptr || value[0] == '\0') {
        return fallback;
    }
    return std::atoi(value);
}

bool GetEnvUint32(const char *name, uint32_t fallback, uint32_t *value)
{
    if (value == nullptr) {
        return false;
    }
    const char *text = std::getenv(name);
    if (text == nullptr || text[0] == '\0') {
        *value = fallback;
        return true;
    }
    errno = 0;
    char *end = nullptr;
    const unsigned long parsed = std::strtoul(text, &end, 10);
    if (end == text || *end != '\0' || errno != 0 || parsed > std::numeric_limits<uint32_t>::max()) {
        return false;
    }
    *value = static_cast<uint32_t>(parsed);
    return true;
}

bool ParseInt32List(const char *text, std::vector<int32_t> *values)
{
    if (text == nullptr || values == nullptr) {
        return false;
    }
    values->clear();
    const char *cursor = text;
    while (*cursor != '\0') {
        while (*cursor == ',' || *cursor == ';' || *cursor == ' ' || *cursor == '\t' || *cursor == '\n') {
            ++cursor;
        }
        if (*cursor == '\0') {
            break;
        }
        errno = 0;
        char *end = nullptr;
        const long parsed = std::strtol(cursor, &end, 10);
        if (end == cursor || errno != 0 || parsed < std::numeric_limits<int32_t>::min() ||
            parsed > std::numeric_limits<int32_t>::max()) {
            return false;
        }
        values->push_back(static_cast<int32_t>(parsed));
        cursor = end;
        if (*cursor != '\0' && *cursor != ',' && *cursor != ';' && *cursor != ' ' && *cursor != '\t' &&
            *cursor != '\n') {
            return false;
        }
    }
    return !values->empty();
}

bool ParseHostPort(const std::string &text, HostPort *out)
{
    const std::size_t pos = text.rfind(':');
    if (out == nullptr || pos == std::string::npos || pos == 0 || pos + 1 >= text.size()) {
        return false;
    }
    const int port = std::atoi(text.substr(pos + 1).c_str());
    if (port <= 0 || port > 65535) {
        return false;
    }
    out->host = text.substr(0, pos);
    out->port = port;
    return true;
}

HostPort GetBarrierEndpoint()
{
    HostPort endpoint {"127.0.0.1", 10174};
    const char *barrier = std::getenv("TILEXR_DEMO_BARRIER_ADDR");
    if (barrier != nullptr && barrier[0] != '\0' && ParseHostPort(barrier, &endpoint)) {
        return endpoint;
    }

    const char *commId = std::getenv("TILEXR_COMM_ID");
    HostPort commEndpoint {};
    if (commId != nullptr && ParseHostPort(commId, &commEndpoint)) {
        endpoint.host = commEndpoint.host;
        endpoint.port = commEndpoint.port + 97;
        if (endpoint.port > 65535) {
            endpoint.port = commEndpoint.port - 97;
        }
    }
    return endpoint;
}

bool SendAll(int fd, const void *data, std::size_t bytes)
{
    const char *ptr = static_cast<const char *>(data);
    std::size_t sent = 0;
    while (sent < bytes) {
        const ssize_t ret = send(fd, ptr + sent, bytes - sent, 0);
        if (ret <= 0) {
            return false;
        }
        sent += static_cast<std::size_t>(ret);
    }
    return true;
}

bool RecvAll(int fd, void *data, std::size_t bytes)
{
    char *ptr = static_cast<char *>(data);
    std::size_t received = 0;
    while (received < bytes) {
        const ssize_t ret = recv(fd, ptr + received, bytes - received, MSG_WAITALL);
        if (ret <= 0) {
            return false;
        }
        received += static_cast<std::size_t>(ret);
    }
    return true;
}

bool DemoBarrierServer(int rankSize, const HostPort &endpoint)
{
    const int listenFd = socket(AF_INET, SOCK_STREAM, 0);
    if (listenFd < 0) {
        std::cerr << "demo barrier server socket failed: " << std::strerror(errno) << std::endl;
        return false;
    }

    int reuse = 1;
    (void)setsockopt(listenFd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    sockaddr_in addr {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(static_cast<uint16_t>(endpoint.port));
    if (bind(listenFd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0 ||
        listen(listenFd, rankSize) != 0) {
        std::cerr << "demo barrier server listen failed on port " << endpoint.port << ": "
                  << std::strerror(errno) << std::endl;
        close(listenFd);
        return false;
    }

    std::vector<int> clients;
    for (int peer = 1; peer < rankSize; ++peer) {
        const int fd = accept(listenFd, nullptr, nullptr);
        if (fd < 0) {
            std::cerr << "demo barrier accept failed: " << std::strerror(errno) << std::endl;
            close(listenFd);
            return false;
        }
        char byte = 0;
        if (!RecvAll(fd, &byte, 1)) {
            std::cerr << "demo barrier recv failed" << std::endl;
            close(fd);
            close(listenFd);
            return false;
        }
        clients.push_back(fd);
    }

    const char release = 1;
    for (int fd : clients) {
        if (!SendAll(fd, &release, 1)) {
            std::cerr << "demo barrier release failed" << std::endl;
            close(fd);
            close(listenFd);
            return false;
        }
        close(fd);
    }
    close(listenFd);
    return true;
}

bool DemoBarrierClient(const HostPort &endpoint)
{
    sockaddr_in addr {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(endpoint.port));
    if (inet_pton(AF_INET, endpoint.host.c_str(), &addr.sin_addr) != 1) {
        std::cerr << "demo barrier invalid host: " << endpoint.host << std::endl;
        return false;
    }

    int fd = -1;
    for (int attempt = 0; attempt < 600; ++attempt) {
        fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) {
            return false;
        }
        if (connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == 0) {
            break;
        }
        close(fd);
        fd = -1;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    if (fd < 0) {
        std::cerr << "demo barrier connect failed to " << endpoint.host << ":" << endpoint.port << std::endl;
        return false;
    }

    const char arrived = 1;
    char release = 0;
    const bool ok = SendAll(fd, &arrived, 1) && RecvAll(fd, &release, 1);
    close(fd);
    if (!ok) {
        std::cerr << "demo barrier client exchange failed" << std::endl;
    }
    return ok;
}

bool DemoBarrierAll(int rank, int rankSize, const std::string &step)
{
    if (rankSize <= 1) {
        return true;
    }
    const HostPort endpoint = GetBarrierEndpoint();
    const bool ok = rank == 0 ? DemoBarrierServer(rankSize, endpoint) : DemoBarrierClient(endpoint);
    if (!ok) {
        std::cerr << "rank " << rank << " demo barrier failed after " << step << std::endl;
    }
    return ok;
}

std::vector<int> ParseDeviceList(const char *value)
{
    std::vector<int> devices;
    if (value == nullptr || value[0] == '\0') {
        return devices;
    }

    std::string text(value);
    std::size_t begin = 0;
    while (begin <= text.size()) {
        const std::size_t end = text.find(',', begin);
        const std::string part = text.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
        if (!part.empty()) {
            devices.push_back(std::atoi(part.c_str()));
        }
        if (end == std::string::npos) {
            break;
        }
        begin = end + 1;
    }
    return devices;
}

int GetDeviceIdFromEnv(int rank, int npuCount, int firstNpu)
{
    const std::vector<int> devices = ParseDeviceList(std::getenv("TILEXR_DEMO_DEVICES"));
    if (!devices.empty()) {
        return devices[rank % static_cast<int>(devices.size())];
    }
    return firstNpu + rank % std::max(npuCount, 1);
}

bool CheckAcl(aclError ret, const std::string &what)
{
    if (ret != ACL_SUCCESS) {
        std::cerr << what << " failed, acl ret=" << ret << std::endl;
        return false;
    }
    return true;
}

bool CheckTileXR(int ret, const std::string &what)
{
    if (ret != TileXR::TILEXR_SUCCESS) {
        std::cerr << what << " failed, TileXR ret=" << ret << std::endl;
        return false;
    }
    return true;
}

std::uintptr_t AlignAddress(std::uintptr_t value, std::size_t alignment)
{
    const std::uintptr_t mask = static_cast<std::uintptr_t>(alignment - 1);
    return (value + mask) & ~mask;
}

std::size_t AlignSize(std::size_t value, std::size_t alignment)
{
    const std::size_t remainder = value % alignment;
    return remainder == 0 ? value : value + alignment - remainder;
}

std::size_t EpSlotBytes(
    const DemoConfig &config, std::size_t payloadRowBytes, std::size_t payloadScaleBytesPerRow)
{
    const std::size_t payloadScaleBytes =
        static_cast<std::size_t>(config.maxRoutesPerRank()) * payloadScaleBytesPerRow;
    const std::size_t payloadBytes = AlignSize(
        static_cast<std::size_t>(config.maxRoutesPerRank()) * payloadRowBytes + payloadScaleBytes, 32);
    const std::size_t assistWindowBytes = AlignSize(
        static_cast<std::size_t>(config.maxRoutesPerRank()) * kAssistInts * sizeof(int32_t), 32);
    return AlignSize(64 + payloadBytes + assistWindowBytes, 32);
}

std::size_t EpWindowBytes(int rankSize, const DemoConfig &config, std::size_t payloadRowBytes,
    std::size_t payloadScaleBytesPerRow)
{
    return AlignSize(64 + static_cast<std::size_t>(rankSize) *
        EpSlotBytes(config, payloadRowBytes, payloadScaleBytesPerRow), 32);
}

std::size_t EpOperationBytes(int rankSize, const DemoConfig &config, std::size_t payloadRowBytes,
    std::size_t payloadScaleBytesPerRow)
{
    const std::size_t windowBytes = EpWindowBytes(rankSize, config, payloadRowBytes, payloadScaleBytesPerRow);
    const std::size_t readyOffset = windowBytes * 2 + static_cast<std::size_t>(rankSize) * sizeof(uint64_t);
    const std::size_t relayOffset = AlignSize(readyOffset, kUdmaCacheLineBytes);
    const std::size_t relayBytes = static_cast<std::size_t>(rankSize) * static_cast<std::size_t>(rankSize) *
        EpSlotBytes(config, payloadRowBytes, payloadScaleBytesPerRow);
    const std::size_t relayReadyOffset = AlignSize(relayOffset + relayBytes, kUdmaCacheLineBytes);
    const std::size_t relayReadyBytes = static_cast<std::size_t>(rankSize) * sizeof(uint64_t);
    return AlignSize(relayReadyOffset + relayReadyBytes, kUdmaCacheLineBytes);
}

std::size_t EpRequiredWorkspaceBytes(int rankSize, const DemoConfig &config, std::size_t payloadRowBytes,
    std::size_t payloadScaleBytesPerRow)
{
    const std::size_t operationBytes = EpOperationBytes(rankSize, config, payloadRowBytes, payloadScaleBytesPerRow);
    return AlignSize(operationBytes * 2 + sizeof(uint64_t), kUdmaCacheLineBytes);
}

uint16_t XValue(int rank, int64_t token, int64_t h)
{
    return static_cast<uint16_t>(
        0x3c00 + rank * 0x0400 + token * 0x0100 + (h % 32) * 0x0010);
}

uint16_t InputValue(TileXR::TileXRDataType dtype, int rank, int64_t token, int64_t h)
{
    if (dtype == TileXR::TILEXR_DATA_TYPE_BFP16) {
        return static_cast<uint16_t>(
            0x3f80 + rank * 0x0080 + token * 0x0010 + (h % 32));
    }
    return XValue(rank, token, h);
}

uint16_t Mxfp8InputValue(TileXR::TileXRDataType dtype, int rank, int64_t token, int64_t h)
{
    static constexpr uint16_t kFp16Values[] = {
        0xc500, 0xc400, 0xc200, 0xc000, 0xbc00, 0x0000,
        0x3c00, 0x4000, 0x4200, 0x4400, 0x4500,
    };
    static constexpr uint16_t kBf16Values[] = {
        0xc0a0, 0xc080, 0xc040, 0xc000, 0xbf80, 0x0000,
        0x3f80, 0x4000, 0x4040, 0x4080, 0x40a0,
    };
    constexpr std::size_t kValueCount = sizeof(kFp16Values) / sizeof(kFp16Values[0]);
    const std::size_t index = (static_cast<std::size_t>(rank) * 7U +
        static_cast<std::size_t>(token) * 5U + static_cast<std::size_t>(h)) % kValueCount;
    return dtype == TileXR::TILEXR_DATA_TYPE_BFP16 ? kBf16Values[index] : kFp16Values[index];
}

float HalfBitsToFloat(uint16_t bits)
{
    const int sign = (bits & 0x8000U) != 0 ? -1 : 1;
    const int exponent = static_cast<int>((bits >> 10) & 0x1fU);
    const int mantissa = static_cast<int>(bits & 0x03ffU);
    if (exponent == 0 && mantissa == 0) {
        return sign < 0 ? -0.0f : 0.0f;
    }
    if (exponent == 0) {
        float value = static_cast<float>(mantissa) / 1024.0f;
        for (int shift = 0; shift < 14; ++shift) {
            value *= 0.5f;
        }
        return static_cast<float>(sign) * value;
    }
    float value = 1.0f + static_cast<float>(mantissa) / 1024.0f;
    const int power = exponent - 15;
    if (power >= 0) {
        for (int shift = 0; shift < power; ++shift) {
            value *= 2.0f;
        }
    } else {
        for (int shift = 0; shift < -power; ++shift) {
            value *= 0.5f;
        }
    }
    return static_cast<float>(sign) * value;
}

float DataBitsToFloat(uint16_t bits, TileXR::TileXRDataType dtype)
{
    if (dtype == TileXR::TILEXR_DATA_TYPE_BFP16) {
        const uint32_t fp32Bits = static_cast<uint32_t>(bits) << 16U;
        float value = 0.0f;
        std::memcpy(&value, &fp32Bits, sizeof(value));
        return value;
    }
    return HalfBitsToFloat(bits);
}

int8_t QuantizedXValue(int rank, int64_t token, int64_t h, float scale)
{
    const float value = HalfBitsToFloat(XValue(rank, token, h)) * scale;
    int rounded = static_cast<int>(value >= 0.0f ? value + 0.5f : value - 0.5f);
    if (rounded > 127) {
        rounded = 127;
    } else if (rounded < -128) {
        rounded = -128;
    }
    return static_cast<int8_t>(rounded);
}

float DynamicScaleForXValue(int rank, int64_t token, int64_t hSize)
{
    float maxAbs = 0.0f;
    for (int64_t h = 0; h < hSize; ++h) {
        const float value = std::fabs(HalfBitsToFloat(XValue(rank, token, h)));
        maxAbs = std::max(maxAbs, value);
    }
    return maxAbs > 0.0f ? maxAbs / 127.0f : 1.0f;
}

int8_t DynamicQuantizedXValue(int rank, int64_t token, int64_t h, int64_t hSize)
{
    const float scale = DynamicScaleForXValue(rank, token, hSize);
    return QuantizedXValue(rank, token, h, scale > 0.0f ? 1.0f / scale : 1.0f);
}

float ExpertScaleValue(int64_t token, int64_t topKId)
{
    return 0.5f + static_cast<float>(token) * 0.0625f + static_cast<float>(topKId) * 0.125f;
}

std::vector<float> BuildExpertScales(const DemoConfig &config)
{
    std::vector<float> scales(static_cast<std::size_t>(config.bs * config.topK));
    for (int64_t token = 0; token < config.bs; ++token) {
        for (int64_t topKId = 0; topKId < config.topK; ++topKId) {
            scales[static_cast<std::size_t>(token * config.topK + topKId)] =
                ExpertScaleValue(token, topKId);
        }
    }
    return scales;
}

std::vector<int32_t> ExpertIds(const DemoConfig &config)
{
    return config.expertIds;
}

const std::vector<int32_t> &ExpertIdsForRank(const DemoConfig &config, int rank)
{
    return config.expertIdsByRank[static_cast<std::size_t>(rank)];
}

bool BuildExpertIds(DemoConfig *config, int rank, int rankSize, ExpertListMode mode, uint32_t seed)
{
    if (config == nullptr || config->bs <= 0 || config->topK <= 0 || config->moeExpertNum <= 0 ||
        config->topK > config->moeExpertNum || rankSize <= 0 || rank < 0 || rank >= rankSize) {
        return false;
    }
    const std::size_t routeCount = static_cast<std::size_t>(config->bs * config->topK);
    if (mode == ExpertListMode::EXPLICIT) {
        if (config->expertIds.size() != routeCount) {
            return false;
        }
        if (!std::all_of(config->expertIds.begin(), config->expertIds.end(),
            [config](int32_t expertId) { return expertId >= 0 && expertId < config->moeExpertNum; })) {
            return false;
        }
        config->expertIdsByRank.assign(static_cast<std::size_t>(rankSize), config->expertIds);
        return true;
    }

    const int64_t epWorldSize = rankSize;
    const int64_t moeRankNum = epWorldSize - config->sharedExpertRankNum;
    if (moeRankNum <= 0) {
        return false;
    }
    config->expertIdsByRank.resize(static_cast<std::size_t>(rankSize));
    for (int currentRank = 0; currentRank < rankSize; ++currentRank) {
        TileXREpDemo::ExpertIdsConfig idsConfig {};
        idsConfig.bs = config->bs;
        idsConfig.topK = config->topK;
        idsConfig.moeExpertNum = config->moeExpertNum;
        idsConfig.moeRankNum = moeRankNum;
        idsConfig.epWorldSize = epWorldSize;
        idsConfig.epRankId = currentRank;
        idsConfig.seed = seed;
        std::vector<int32_t> ids = mode == ExpertListMode::UNIFORM ?
            TileXREpDemo::BuildBalancedExpertIds(idsConfig) :
            TileXREpDemo::BuildRandomExpertIds(idsConfig);
        if (ids.size() != routeCount) {
            return false;
        }
        config->expertIdsByRank[static_cast<std::size_t>(currentRank)].swap(ids);
    }
    config->expertIds = config->expertIdsByRank[static_cast<std::size_t>(rank)];
    return true;
}

std::vector<uint8_t> ActiveMask(int64_t activeMaskType, const DemoConfig &config)
{
    if (activeMaskType == TileXREp::TILEXR_EP_ACTIVE_MASK_NONE) {
        return {};
    }
    const int64_t elementCount = activeMaskType == TileXREp::TILEXR_EP_ACTIVE_MASK_EXPERT ?
        config.bs * config.topK : config.bs;
    std::vector<uint8_t> mask(static_cast<std::size_t>(elementCount), 1);
    if (!mask.empty()) {
        mask.back() = 0;
    }
    return mask;
}

bool IsRouteActive(const std::vector<uint8_t> &activeMask, const DemoConfig &config,
    int64_t token, int64_t topKId)
{
    if (activeMask.empty()) {
        return true;
    }
    if (activeMask.size() == static_cast<std::size_t>(config.bs)) {
        return activeMask[static_cast<std::size_t>(token)] != 0;
    }
    return activeMask[static_cast<std::size_t>(token * config.topK + topKId)] != 0;
}

bool IsTokenActive(const std::vector<uint8_t> &activeMask, const DemoConfig &config, int64_t token)
{
    for (int64_t topKId = 0; topKId < config.topK; ++topKId) {
        if (IsRouteActive(activeMask, config, token, topKId)) {
            return true;
        }
    }
    return false;
}

int64_t LocalExpertNum(int rankSize, const DemoConfig &config)
{
    const int64_t expertRankSize = static_cast<int64_t>(rankSize) / config.effectiveTpWorldSize();
    const int64_t moeRankNum = expertRankSize - config.sharedExpertRankNum;
    if (moeRankNum <= 0) {
        return 0;
    }
    return config.moeExpertNum / moeRankNum;
}

int64_t OutputLocalExpertNum(int rank, int rankSize, const DemoConfig &config)
{
    return rank < config.sharedExpertRankNum ? 1 : LocalExpertNum(rankSize, config);
}

std::size_t MemorySendCountsCount(int rank, int rankSize, const DemoConfig &config)
{
    return static_cast<std::size_t>(OutputLocalExpertNum(rank, rankSize, config) * rankSize);
}

int ExpertRankForRank(int rank, const DemoConfig &config)
{
    return static_cast<int>(rank / config.effectiveTpWorldSize());
}

int64_t DstRankForExpert(int32_t globalExpertId, int srcRank, int rankSize, const DemoConfig &config)
{
    const int64_t localExpertNum = LocalExpertNum(rankSize, config);
    if (globalExpertId < 0 || localExpertNum <= 0) {
        return -1;
    }
    const int64_t expertRankSize = static_cast<int64_t>(rankSize) / config.effectiveTpWorldSize();
    if (globalExpertId < config.sharedExpertNum) {
        const int64_t rankNumPerSharedExpert = config.sharedExpertRankNum / config.sharedExpertNum;
        return srcRank % rankNumPerSharedExpert + globalExpertId * rankNumPerSharedExpert;
    }
    const int64_t moeExpertId = static_cast<int64_t>(globalExpertId) - config.sharedExpertNum;
    const int64_t dstRank = config.sharedExpertRankNum + moeExpertId / localExpertNum;
    return dstRank < expertRankSize ? dstRank : -1;
}

int64_t LocalExpertForExpert(int32_t globalExpertId, int rankSize, const DemoConfig &config)
{
    const int64_t localExpertNum = LocalExpertNum(rankSize, config);
    if (globalExpertId < 0 || localExpertNum <= 0) {
        return -1;
    }
    if (globalExpertId < config.sharedExpertNum) {
        return 0;
    }
    return (static_cast<int64_t>(globalExpertId) - config.sharedExpertNum) % localExpertNum;
}

bool RouteBelongsToRank(int32_t globalExpertId, int srcRank, int rank, int rankSize, const DemoConfig &config)
{
    return DstRankForExpert(globalExpertId, srcRank, rankSize, config) == ExpertRankForRank(rank, config);
}

struct ExpectedRoute {
    int32_t srcRank;
    int32_t tokenId;
    int32_t topKId;
    int32_t expertId;
};

std::vector<ExpectedRoute> BuildExpectedRoutes(
    int rank, int rankSize, const DemoConfig &config, const std::vector<uint8_t> &activeMask)
{
    std::vector<ExpectedRoute> expected;
    const int64_t effectiveTpWorldSize = config.effectiveTpWorldSize();
    const int targetTpRankId = static_cast<int>(rank % effectiveTpWorldSize);
    for (int srcRank = 0; srcRank < rankSize; ++srcRank) {
        if (effectiveTpWorldSize > 1 && srcRank % effectiveTpWorldSize != targetTpRankId) {
            continue;
        }
        const std::vector<int32_t> &expertIds = ExpertIdsForRank(config, srcRank);
        for (int64_t token = 0; token < config.bs; ++token) {
            if (!IsTokenActive(activeMask, config, token)) {
                continue;
            }
            for (int64_t sharedExpertId = 0; sharedExpertId < config.sharedExpertNum; ++sharedExpertId) {
                if (RouteBelongsToRank(
                        static_cast<int32_t>(sharedExpertId), srcRank, rank, rankSize, config)) {
                    expected.push_back(ExpectedRoute {srcRank, static_cast<int32_t>(token),
                        static_cast<int32_t>(config.topK + sharedExpertId), static_cast<int32_t>(sharedExpertId)});
                }
            }
            for (int64_t topKId = 0; topKId < config.topK; ++topKId) {
                if (!IsRouteActive(activeMask, config, token, topKId)) {
                    continue;
                }
                const int64_t route = token * config.topK + topKId;
                const int32_t expertId = static_cast<int32_t>(config.sharedExpertNum) + expertIds[route];
                if (RouteBelongsToRank(expertId, srcRank, rank, rankSize, config)) {
                    expected.push_back(ExpectedRoute {srcRank, static_cast<int32_t>(token),
                        static_cast<int32_t>(topKId), expertId});
                }
            }
        }
    }
    return expected;
}

std::vector<ExpectedRoute> BuildExpectedTpRoutes(
    int rank, int rankSize, const DemoConfig &config, const std::vector<uint8_t> &activeMask)
{
    std::vector<ExpectedRoute> expected = BuildExpectedRoutes(rank, rankSize, config, activeMask);
    const int64_t effectiveTpWorldSize = config.effectiveTpWorldSize();
    if (effectiveTpWorldSize <= 1) {
        return expected;
    }
    const int tpGroupStartRank = ExpertRankForRank(rank, config) * static_cast<int>(effectiveTpWorldSize);
    for (int64_t tpLane = 0; tpLane < effectiveTpWorldSize; ++tpLane) {
        const int tpPeerRank = tpGroupStartRank + static_cast<int>(tpLane);
        if (tpPeerRank == rank || tpPeerRank < 0 || tpPeerRank >= rankSize) {
            continue;
        }
        const std::vector<ExpectedRoute> peerExpected = BuildExpectedRoutes(tpPeerRank, rankSize, config, activeMask);
        expected.insert(expected.end(), peerExpected.begin(), peerExpected.end());
    }
    return expected;
}

std::vector<ExpectedRoute> BuildExpectedMemoryRoutes(
    int rank, int rankSize, const DemoConfig &config, const std::vector<uint8_t> &activeMask)
{
    const std::vector<ExpectedRoute> sourceMajor = BuildExpectedRoutes(rank, rankSize, config, activeMask);
    const int64_t localExpertNum = OutputLocalExpertNum(rank, rankSize, config);
    std::vector<ExpectedRoute> expected;
    expected.reserve(sourceMajor.size());
    for (int64_t localExpert = 0; localExpert < localExpertNum; ++localExpert) {
        for (int srcRank = 0; srcRank < rankSize; ++srcRank) {
            for (const ExpectedRoute &route : sourceMajor) {
                if (route.srcRank == srcRank &&
                    LocalExpertForExpert(route.expertId, rankSize, config) == localExpert) {
                    expected.push_back(route);
                }
            }
        }
    }
    return expected;
}

std::vector<int32_t> BuildExpectedRecvCounts(
    int rank, int rankSize, const DemoConfig &config, const std::vector<uint8_t> &activeMask, bool useMemory)
{
    const std::vector<ExpectedRoute> localExpected = BuildExpectedRoutes(rank, rankSize, config, activeMask);
    std::vector<int32_t> expected(useMemory ? MemorySendCountsCount(rank, rankSize, config) :
        static_cast<std::size_t>(rankSize), 0);
    if (!useMemory) {
        for (const ExpectedRoute &route : localExpected) {
            ++expected[static_cast<std::size_t>(route.srcRank)];
        }
        return expected;
    }

    int32_t running = 0;
    const int64_t localExpertNum = OutputLocalExpertNum(rank, rankSize, config);
    for (int64_t localExpert = 0; localExpert < localExpertNum; ++localExpert) {
        for (int srcRank = 0; srcRank < rankSize; ++srcRank) {
            for (const ExpectedRoute &route : localExpected) {
                if (route.srcRank == srcRank &&
                    LocalExpertForExpert(route.expertId, rankSize, config) == localExpert) {
                    ++running;
                }
            }
            expected[static_cast<std::size_t>(localExpert * rankSize + srcRank)] = running;
        }
    }
    return expected;
}

struct StandaloneCombineInputs {
    std::vector<uint16_t> expertOut;
    std::vector<int32_t> assist;
    std::vector<int32_t> recvCounts;
};

bool BuildStandaloneCombineInputs(int rank, int rankSize, const DemoConfig &config,
    const std::vector<uint8_t> &activeMask, bool useMemory, TileXR::TileXRDataType dtype,
    bool useCombineMxfp8, std::size_t expandedRows, StandaloneCombineInputs *inputs)
{
    if (inputs == nullptr) {
        return false;
    }
    const std::vector<ExpectedRoute> routes = useMemory ?
        BuildExpectedMemoryRoutes(rank, rankSize, config, activeMask) :
        BuildExpectedTpRoutes(rank, rankSize, config, activeMask);
    if (routes.size() > expandedRows) {
        return false;
    }

    const uint16_t one = dtype == TileXR::TILEXR_DATA_TYPE_BFP16 ? kBf16One : kFp16One;
    const std::size_t assistInts = AssistInts(useMemory);
    inputs->expertOut.assign(expandedRows * static_cast<std::size_t>(config.h), one);
    inputs->assist.assign(expandedRows * assistInts, 0);
    inputs->recvCounts = BuildExpectedRecvCounts(rank, rankSize, config, activeMask, useMemory);
    for (std::size_t row = 0; row < routes.size(); ++row) {
        const ExpectedRoute &route = routes[row];
        if (useCombineMxfp8) {
            for (int64_t h = 0; h < config.h; ++h) {
                inputs->expertOut[row * static_cast<std::size_t>(config.h) + static_cast<std::size_t>(h)] =
                    Mxfp8InputValue(dtype, route.srcRank, route.tokenId, h);
            }
        }
        const std::size_t offset = row * assistInts;
        inputs->assist[offset] = route.srcRank;
        inputs->assist[offset + 1] = route.tokenId;
        inputs->assist[offset + 2] = route.topKId;
        if (!useMemory) {
            inputs->assist[offset + 3] = route.expertId;
        }
    }
    return true;
}

TileXREpDemo::Mxfp8Tensor BuildExpectedMxfp8Dispatch(const std::vector<ExpectedRoute> &routes,
    const DemoConfig &config, TileXR::TileXRDataType dtype, std::size_t expandedRows,
    TileXREpDemo::Mxfp8Format format)
{
    std::vector<float> routedInput(expandedRows * static_cast<std::size_t>(config.h), 0.0f);
    for (std::size_t row = 0; row < routes.size(); ++row) {
        for (int64_t h = 0; h < config.h; ++h) {
            routedInput[row * static_cast<std::size_t>(config.h) + static_cast<std::size_t>(h)] =
                DataBitsToFloat(Mxfp8InputValue(dtype, routes[row].srcRank, routes[row].tokenId, h), dtype);
        }
    }
    return TileXREpDemo::QuantizeMxfp8(
        routedInput, expandedRows, static_cast<std::size_t>(config.h), format);
}

bool ValidateOutputs(int rank, int rankSize, const DemoConfig &config, const std::vector<uint8_t> &expandX,
    const std::vector<int64_t> &expertTokenNums, const std::vector<int32_t> &recvCounts,
    const std::vector<int32_t> &assist, const std::vector<uint8_t> &dynamicScalesOut,
    const std::vector<uint8_t> &activeMask, int expertTokenNumsType, bool useStaticQuant,
    bool usePerTokenDynamicQuant, bool useMxfp8, float staticQuantScale, bool useMemoryDispatch,
    TileXR::TileXRDataType dtype, const TileXREpDemo::Mxfp8Tensor &expectedMxfp8)
{
    const int64_t localExpertNum = OutputLocalExpertNum(rank, rankSize, config);
    const std::vector<ExpectedRoute> expected = useMemoryDispatch ?
        BuildExpectedMemoryRoutes(rank, rankSize, config, activeMask) :
        BuildExpectedTpRoutes(rank, rankSize, config, activeMask);
    const std::vector<int32_t> expectedRecv =
        BuildExpectedRecvCounts(rank, rankSize, config, activeMask, useMemoryDispatch);
    std::vector<int64_t> expectedExpertCounts(localExpertNum, 0);
    for (const ExpectedRoute &route : expected) {
        const int64_t localExpert = LocalExpertForExpert(route.expertId, rankSize, config);
        if (localExpert >= 0 && localExpert < localExpertNum) {
            ++expectedExpertCounts[localExpert];
        }
    }
    if (expertTokenNumsType == 0) {
        int64_t running = 0;
        for (int64_t localExpert = 0; localExpert < localExpertNum; ++localExpert) {
            running += expectedExpertCounts[localExpert];
            expectedExpertCounts[localExpert] = running;
        }
    }

    for (std::size_t index = 0; index < expectedRecv.size(); ++index) {
        if (recvCounts[index] != expectedRecv[index]) {
            std::cerr << "rank " << rank << (useMemoryDispatch ? " sendCounts[" : " recvCounts[")
                      << index << "] expected " << expectedRecv[index] << " got " << recvCounts[index]
                      << std::endl;
            if (useMemoryDispatch) {
                std::cerr << "rank " << rank << " sendCounts expected/got:";
                for (std::size_t countIndex = 0; countIndex < expectedRecv.size(); ++countIndex) {
                    std::cerr << " " << expectedRecv[countIndex] << "/" << recvCounts[countIndex];
                }
                std::cerr << std::endl;
            }
            return false;
        }
    }

    for (int64_t localExpert = 0; localExpert < localExpertNum; ++localExpert) {
        if (expertTokenNums[localExpert] != expectedExpertCounts[localExpert]) {
            std::cerr << "rank " << rank << " expertTokenNums[" << localExpert << "] expected "
                      << expectedExpertCounts[localExpert] << " got " << expertTokenNums[localExpert] << std::endl;
            return false;
        }
    }

    for (std::size_t row = 0; row < expected.size(); ++row) {
        const ExpectedRoute &route = expected[row];
        const std::size_t assistInts = AssistInts(useMemoryDispatch);
        const std::size_t assistOffset = row * assistInts;
        bool assistMismatch = assist[assistOffset] != route.srcRank ||
            assist[assistOffset + 1] != route.tokenId || assist[assistOffset + 2] != route.topKId;
        if (!useMemoryDispatch) {
            assistMismatch = assistMismatch || assist[assistOffset + 3] != route.expertId;
        }
        if (assistMismatch) {
            std::cerr << "rank " << rank << " assist row " << row << " mismatch: got {"
                      << assist[assistOffset] << ", " << assist[assistOffset + 1] << ", "
                      << assist[assistOffset + 2];
            if (!useMemoryDispatch) {
                std::cerr << ", " << assist[assistOffset + 3];
            }
            std::cerr << "}" << std::endl;
            return false;
        }

        if (usePerTokenDynamicQuant) {
            const float expectedScale = DynamicScaleForXValue(route.srcRank, route.tokenId, config.h);
            float actualScale = 0.0f;
            std::memcpy(&actualScale, dynamicScalesOut.data() + row * sizeof(float), sizeof(float));
            if (std::fabs(actualScale - expectedScale) > 1.0e-5f) {
                std::cerr << "rank " << rank << " dynamicScalesOut[" << row << "] expected "
                          << expectedScale << " got " << actualScale << std::endl;
                return false;
            }
        }

        for (int64_t h = 0; h < config.h; ++h) {
            if (useMxfp8) {
                const std::size_t offset = row * static_cast<std::size_t>(config.h) +
                    static_cast<std::size_t>(h);
                if (expandX[offset] != expectedMxfp8.elements[offset]) {
                    std::cerr << "rank " << rank << " MXFP8 expandX[" << row << "][" << h
                              << "] expected 0x" << std::hex << static_cast<int>(expectedMxfp8.elements[offset])
                              << " got 0x" << static_cast<int>(expandX[offset]) << std::dec << std::endl;
                    return false;
                }
                continue;
            }
            const bool useInt8Output = useStaticQuant || usePerTokenDynamicQuant;
            const std::size_t byteOffset = row * config.h * (useInt8Output ? sizeof(int8_t) : sizeof(uint16_t)) +
                h * (useInt8Output ? sizeof(int8_t) : sizeof(uint16_t));
            const int expectedValue = useStaticQuant ?
                static_cast<int>(QuantizedXValue(route.srcRank, route.tokenId, h, staticQuantScale)) :
                (usePerTokenDynamicQuant ?
                    static_cast<int>(DynamicQuantizedXValue(route.srcRank, route.tokenId, h, config.h)) :
                    static_cast<int>(InputValue(dtype, route.srcRank, route.tokenId, h)));
            const int actualValue = useInt8Output ?
                static_cast<int>(*reinterpret_cast<const int8_t *>(&expandX[byteOffset])) :
                static_cast<int>(*reinterpret_cast<const uint16_t *>(&expandX[byteOffset]));
            if (actualValue != expectedValue) {
                std::cerr << "rank " << rank << " expandX[" << row << "][" << h << "] expected 0x"
                          << std::hex << expectedValue << " got 0x" << actualValue << std::dec << std::endl;
                return false;
            }
        }
        if (useMxfp8) {
            for (std::size_t scale = 0; scale < expectedMxfp8.scaleCountPerRow; ++scale) {
                const std::size_t offset = row * expectedMxfp8.scaleCountPerRow + scale;
                if (dynamicScalesOut[offset] != expectedMxfp8.scales[offset]) {
                    std::cerr << "rank " << rank << " MXFP8 dynamicScalesOut[" << row << "][" << scale
                              << "] expected 0x" << std::hex << static_cast<int>(expectedMxfp8.scales[offset])
                              << " got 0x" << static_cast<int>(dynamicScalesOut[offset]) << std::dec << std::endl;
                    return false;
                }
            }
        }
    }

    return true;
}

int64_t ExpectedCombineRouteCount(
    const DemoConfig &config, const std::vector<uint8_t> &activeMask, int64_t token)
{
    if (!IsTokenActive(activeMask, config, token)) {
        return 0;
    }
    int64_t count = config.sharedExpertNum;
    const std::vector<int32_t> expertIds = ExpertIds(config);
    for (int64_t topKId = 0; topKId < config.topK; ++topKId) {
        const int32_t expertId = expertIds[static_cast<std::size_t>(token * config.topK + topKId)];
        if (IsRouteActive(activeMask, config, token, topKId) && expertId >= 0 && expertId < config.moeExpertNum) {
            ++count;
        }
    }
    return count;
}

bool ValidateCombineOutputs(int rank, const DemoConfig &config, const std::vector<uint16_t> &yOut,
    const std::vector<uint8_t> &activeMask, TileXR::TileXRDataType dtype, bool usesDispatchOutput,
    int commQuantMode, const std::vector<float> &expertScales)
{
    const bool useCombineMxfp8 = commQuantMode == 3 || commQuantMode == 4;
    if (useCombineMxfp8 && expertScales.size() != static_cast<std::size_t>(config.bs * config.topK)) {
        std::cerr << "rank " << rank << " invalid combine expertScales size" << std::endl;
        return false;
    }
    const TileXREpDemo::Mxfp8Format format = commQuantMode == 3 ?
        TileXREpDemo::Mxfp8Format::E5M2 : TileXREpDemo::Mxfp8Format::E4M3;
    const std::vector<int32_t> expertIds = ExpertIds(config);
    for (int64_t token = 0; token < config.bs; ++token) {
        const int64_t routeCount = ExpectedCombineRouteCount(config, activeMask, token);
        std::vector<float> roundTrip;
        if (useCombineMxfp8) {
            std::vector<float> row(static_cast<std::size_t>(config.h));
            for (int64_t h = 0; h < config.h; ++h) {
                row[static_cast<std::size_t>(h)] =
                    DataBitsToFloat(Mxfp8InputValue(dtype, rank, token, h), dtype);
            }
            roundTrip = TileXREpDemo::RoundTripMxfp8(
                row, 1, static_cast<std::size_t>(config.h), format);
        }
        for (int64_t h = 0; h < config.h; ++h) {
            float expectedValue = 0.0f;
            if (useCombineMxfp8) {
                if (IsTokenActive(activeMask, config, token)) {
                    for (int64_t topKId = 0; topKId < config.topK; ++topKId) {
                        const std::size_t route = static_cast<std::size_t>(token * config.topK + topKId);
                        if (IsRouteActive(activeMask, config, token, topKId) && expertIds[route] >= 0 &&
                            expertIds[route] < config.moeExpertNum) {
                            expectedValue += roundTrip[static_cast<std::size_t>(h)] * expertScales[route];
                        }
                    }
                    for (int64_t sharedExpertId = 0; sharedExpertId < config.sharedExpertNum; ++sharedExpertId) {
                        expectedValue += roundTrip[static_cast<std::size_t>(h)];
                    }
                }
            } else {
                const float inputValue = usesDispatchOutput ?
                    DataBitsToFloat(InputValue(dtype, rank, token, h), dtype) : 1.0f;
                expectedValue = static_cast<float>(routeCount) * inputValue;
            }
            if (dtype == TileXR::TILEXR_DATA_TYPE_FP16) {
                constexpr float kFp16MaxFinite = 65504.0f;
                expectedValue = std::max(-kFp16MaxFinite, std::min(kFp16MaxFinite, expectedValue));
            }
            const uint16_t actualBits = yOut[token * config.h + h];
            const float actualValue = DataBitsToFloat(actualBits, dtype);
            const float tolerance = (usesDispatchOutput || useCombineMxfp8) ?
                std::max(1.0e-3f, std::fabs(expectedValue) * 0.01f) : 0.0f;
            if (std::fabs(actualValue - expectedValue) > tolerance) {
                std::cerr << "rank " << rank << " yOut[" << token << "][" << h << "] expected "
                          << expectedValue << " got " << actualValue << " (bits 0x" << std::hex
                          << actualBits << std::dec << ")" << std::endl;
                return false;
            }
        }
    }
    return true;
}

bool ValidateTpRecvCounts(int rank, int rankSize, const DemoConfig &config, const std::vector<uint8_t> &activeMask,
    const std::vector<int32_t> &recvCounts, const std::vector<int32_t> &tpRecvCounts)
{
    if (config.effectiveTpWorldSize() == 1) {
        if (recvCounts.size() != tpRecvCounts.size()) {
            std::cerr << "rank " << rank << " tpRecvCounts size mismatch" << std::endl;
            return false;
        }
        for (std::size_t index = 0; index < recvCounts.size(); ++index) {
            if (tpRecvCounts[index] != recvCounts[index]) {
                std::cerr << "rank " << rank << " tpRecvCounts[" << index << "] expected "
                          << recvCounts[index] << " got " << tpRecvCounts[index] << std::endl;
                return false;
            }
        }
        return true;
    }
    const int64_t effectiveTpWorldSize = config.effectiveTpWorldSize();
    if (effectiveTpWorldSize <= 1 || tpRecvCounts.size() < static_cast<std::size_t>(effectiveTpWorldSize)) {
        std::cerr << "rank " << rank << " tpRecvCounts size mismatch" << std::endl;
        return false;
    }
    const int tpGroupStartRank = ExpertRankForRank(rank, config) * static_cast<int>(effectiveTpWorldSize);
    for (int64_t tpLane = 0; tpLane < effectiveTpWorldSize; ++tpLane) {
        const int tpPeerRank = tpGroupStartRank + static_cast<int>(tpLane);
        const int64_t expectedCount = tpPeerRank >= 0 && tpPeerRank < rankSize ?
            static_cast<int64_t>(BuildExpectedRoutes(tpPeerRank, rankSize, config, activeMask).size()) : 0;
        if (tpRecvCounts[tpLane] != expectedCount) {
            std::cerr << "rank " << rank << " tpRecvCounts[" << tpLane << "] expected "
                      << expectedCount << " got " << tpRecvCounts[tpLane] << std::endl;
            return false;
        }
    }
    return true;
}

void Cleanup(TileXRCommPtr comm, aclrtStream stream, int deviceId, bool deviceSet, bool aclReady,
    std::vector<void *> &buffers)
{
    if (comm != nullptr && g_workspaceRegistered) {
        (void)TileXRUDMAUnregister(comm, g_workspaceHandle);
        g_workspaceRegistered = false;
        g_workspaceHandle = 0;
    }
    for (void *buffer : buffers) {
        if (buffer != nullptr) {
            (void)aclrtFree(buffer);
        }
    }
    if (comm != nullptr) {
        (void)TileXRCommDestroy(comm);
    }
    if (stream != nullptr) {
        (void)aclrtDestroyStream(stream);
    }
    if (deviceSet) {
        (void)aclrtResetDevice(deviceId);
    }
    if (aclReady) {
        (void)aclFinalize();
    }
}

} // namespace

int main(int argc, char **argv)
{
    const int rankSize = argc > 1 ? std::atoi(argv[1]) : GetEnvInt("RANK_SIZE", 2);
    const int rank = argc > 2 ? std::atoi(argv[2]) : GetEnvInt("RANK", 0);
    const int npuCount = argc > 3 ? std::atoi(argv[3]) : GetEnvInt("TILEXR_DEMO_NPUS", rankSize);
    const int firstNpu = argc > 4 ? std::atoi(argv[4]) : GetEnvInt("TILEXR_DEMO_FIRST_NPU", 0);
    const int loopCount = GetEnvInt("TILEXR_EP_DEMO_LOOP", 150);
    const bool perfMode = EnvEnabled("TILEXR_EP_DEMO_PERF");
    const int warmupCount = GetEnvInt("TILEXR_EP_DEMO_WARMUP", 50);
    const char *backendText = std::getenv("TILEXR_EP_DEMO_IMPL");
    bool validBackend = true;
    DemoBackend backend = DemoBackend::UDMA;
    if (backendText != nullptr && backendText[0] != '\0') {
        if (std::strcmp(backendText, "memory") == 0) {
            backend = DemoBackend::MEMORY;
        } else if (std::strcmp(backendText, "udma") != 0) {
            validBackend = false;
        }
    }
    const bool useMemory = backend == DemoBackend::MEMORY;
    const char *runModeText = std::getenv("TILEXR_EP_DEMO_RUN_MODE");
    bool validRunMode = true;
    DemoRunMode runMode = DemoRunMode::DISPATCH_COMBINE;
    if (runModeText != nullptr && runModeText[0] != '\0') {
        if (std::strcmp(runModeText, "dispatch") == 0) {
            runMode = DemoRunMode::DISPATCH;
        } else if (std::strcmp(runModeText, "combine") == 0) {
            runMode = DemoRunMode::COMBINE;
        } else if (std::strcmp(runModeText, "dispatch_combine") != 0) {
            validRunMode = false;
        }
    }
    const bool runDispatch = runMode != DemoRunMode::COMBINE;
    const bool runCombine = runMode != DemoRunMode::DISPATCH;
    const char *activeMaskTypeText = std::getenv("TILEXR_EP_DEMO_ACTIVE_MASK_TYPE");
    bool validActiveMaskType = true;
    int64_t activeMaskType = TileXREp::TILEXR_EP_ACTIVE_MASK_NONE;
    if (activeMaskTypeText == nullptr || activeMaskTypeText[0] == '\0') {
        activeMaskType = EnvEnabled("TILEXR_EP_DEMO_ACTIVE_MASK") ?
            TileXREp::TILEXR_EP_ACTIVE_MASK_TOKEN : TileXREp::TILEXR_EP_ACTIVE_MASK_NONE;
    } else if (std::strcmp(activeMaskTypeText, "token") == 0) {
        activeMaskType = TileXREp::TILEXR_EP_ACTIVE_MASK_TOKEN;
    } else if (std::strcmp(activeMaskTypeText, "expert") == 0) {
        activeMaskType = TileXREp::TILEXR_EP_ACTIVE_MASK_EXPERT;
    } else if (std::strcmp(activeMaskTypeText, "none") != 0) {
        validActiveMaskType = false;
    }
    const bool useActiveMask = activeMaskType != TileXREp::TILEXR_EP_ACTIVE_MASK_NONE;
    const char *dtypeText = std::getenv("TILEXR_EP_DEMO_DTYPE");
    bool validDtype = true;
    TileXR::TileXRDataType dtype = TileXR::TILEXR_DATA_TYPE_FP16;
    if (dtypeText != nullptr && dtypeText[0] != '\0') {
        if (std::strcmp(dtypeText, "bf16") == 0) {
            dtype = TileXR::TILEXR_DATA_TYPE_BFP16;
        } else if (std::strcmp(dtypeText, "fp16") != 0) {
            validDtype = false;
        }
    }
    const bool requestedTpRecvCounts = EnvEnabled("TILEXR_EP_DEMO_TP_RECV_COUNTS");
    const int expertTokenNumsType = GetEnvInt("TILEXR_EP_DEMO_EXPERT_TOKEN_NUMS_TYPE", 1);
    const int quantMode = GetEnvInt("TILEXR_EP_DEMO_QUANT_MODE", 0);
    const bool useStaticQuant = quantMode == 1;
    const bool usePerTokenDynamicQuant = quantMode == 2;
    const bool useMxfp8 = quantMode == 4;
    const int commQuantMode = GetEnvInt("TILEXR_EP_DEMO_COMM_QUANT_MODE", 0);
    const bool useCombineMxfp8 = commQuantMode == 3 || commQuantMode == 4;
    const char *mxfp8FormatText = std::getenv("TILEXR_EP_DEMO_MXFP8_FORMAT");
    bool validMxfp8Format = true;
    TileXREpDemo::Mxfp8Format mxfp8Format = TileXREpDemo::Mxfp8Format::E4M3;
    if (mxfp8FormatText != nullptr && mxfp8FormatText[0] != '\0') {
        if (std::strcmp(mxfp8FormatText, "e5m2") == 0 ||
            std::strcmp(mxfp8FormatText, "fp8_e5m2") == 0) {
            mxfp8Format = TileXREpDemo::Mxfp8Format::E5M2;
        } else if (std::strcmp(mxfp8FormatText, "e4m3") != 0 &&
            std::strcmp(mxfp8FormatText, "fp8_e4m3fn") != 0) {
            validMxfp8Format = false;
        }
    }
    const TileXR::TileXRDataType expandXOutDtype = useMxfp8 ?
        (mxfp8Format == TileXREpDemo::Mxfp8Format::E4M3 ? TileXR::TILEXR_DATA_TYPE_FP8E4M3 :
            TileXR::TILEXR_DATA_TYPE_FP8E5M2) : dtype;
    const float staticQuantScale = static_cast<float>(GetEnvInt("TILEXR_EP_DEMO_STATIC_QUANT_SCALE", 1));
    DemoConfig config {};
    config.bs = GetEnvInt("TILEXR_EP_DEMO_BS", static_cast<int>(config.bs));
    config.h = GetEnvInt("TILEXR_EP_DEMO_H", static_cast<int>(config.h));
    config.topK = GetEnvInt("TILEXR_EP_DEMO_TOPK", static_cast<int>(config.topK));
    config.moeExpertNum = GetEnvInt("TILEXR_EP_DEMO_MOE_EXPERT_NUM", static_cast<int>(config.moeExpertNum));
    config.sharedExpertNum = GetEnvInt("TILEXR_EP_DEMO_SHARED_EXPERT_NUM", 0);
    config.sharedExpertRankNum = GetEnvInt("TILEXR_EP_DEMO_SHARED_EXPERT_RANK_NUM", 0);
    config.tpWorldSize = GetEnvInt("TILEXR_EP_DEMO_TP_WORLD_SIZE", 0);
    config.tpRankId = GetEnvInt("TILEXR_EP_DEMO_TP_RANK_ID",
        config.effectiveTpWorldSize() > 1 ? rank % config.effectiveTpWorldSize() : 0);
    const char *expertIdsText = std::getenv("TILEXR_EP_DEMO_EXPERT_IDS");
    const bool hasConfiguredExpertIds = expertIdsText != nullptr && expertIdsText[0] != '\0';
    const bool validExpertIds = !hasConfiguredExpertIds || ParseInt32List(expertIdsText, &config.expertIds);
    const char *expertModeText = std::getenv("TILEXR_EP_DEMO_EXPERT_MODE");
    bool validExpertMode = true;
    ExpertListMode expertMode = hasConfiguredExpertIds ? ExpertListMode::EXPLICIT : ExpertListMode::UNIFORM;
    if (expertModeText != nullptr && expertModeText[0] != '\0') {
        if (std::strcmp(expertModeText, "random") == 0) {
            expertMode = ExpertListMode::RANDOM;
        } else if (std::strcmp(expertModeText, "explicit") == 0) {
            expertMode = ExpertListMode::EXPLICIT;
        } else if (std::strcmp(expertModeText, "uniform") != 0) {
            validExpertMode = false;
        }
    }
    uint32_t expertSeed = 1;
    const bool validExpertSeed = GetEnvUint32("TILEXR_EP_DEMO_EXPERT_SEED", 1, &expertSeed);
    const bool unambiguousExpertConfig =
        (expertMode == ExpertListMode::EXPLICIT) == hasConfiguredExpertIds;
    const bool validExpertConfiguration = validExpertIds && validExpertMode && validExpertSeed &&
        unambiguousExpertConfig && BuildExpertIds(&config, rank, rankSize, expertMode, expertSeed);
    const bool useTpRecvCounts = requestedTpRecvCounts || config.effectiveTpWorldSize() != 1;

    const int64_t expertRankSize = static_cast<int64_t>(rankSize) / config.effectiveTpWorldSize();
    const int64_t moeRankNum = expertRankSize - config.sharedExpertRankNum;
    if (rankSize <= 0 || rank < 0 || rank >= rankSize || loopCount <= 0 || warmupCount < 0 ||
        config.effectiveTpWorldSize() <= 0 ||
        rankSize % config.effectiveTpWorldSize() != 0 || config.bs <= 0 || config.h <= 0 || config.topK <= 0 ||
        moeRankNum <= 0 ||
        config.moeExpertNum <= 0 || config.moeExpertNum % moeRankNum != 0 ||
        config.sharedExpertNum < 0 || config.sharedExpertRankNum < 0 ||
        ((config.sharedExpertNum == 0) != (config.sharedExpertRankNum == 0)) ||
        (config.sharedExpertNum > 0 && config.sharedExpertRankNum % config.sharedExpertNum != 0) ||
        (config.effectiveTpWorldSize() > 1 && config.tpRankId != rank % config.effectiveTpWorldSize()) ||
        !validBackend || !validRunMode || !validActiveMaskType || !validDtype || !validMxfp8Format ||
        !validExpertConfiguration ||
        (expertTokenNumsType != 0 && expertTokenNumsType != 1) ||
        (quantMode != 0 && quantMode != 1 && quantMode != 2 && quantMode != 4) ||
        (commQuantMode != 0 && commQuantMode != 3 && commQuantMode != 4) ||
        activeMaskType == TileXREp::TILEXR_EP_ACTIVE_MASK_EXPERT ||
        useStaticQuant || usePerTokenDynamicQuant || (useMxfp8 && (runCombine || !useMemory)) ||
        (useCombineMxfp8 && (!runCombine || !useMemory)) ||
        (runCombine && config.effectiveTpWorldSize() != 1) ||
        (perfMode && runMode == DemoRunMode::DISPATCH_COMBINE)) {
        std::cerr << "This demo expects a valid rank and moeExpertNum divisible by MoE rank num, got moeExpertNum="
                  << config.moeExpertNum << " rankSize=" << rankSize
                  << " bs=" << config.bs
                  << " h=" << config.h
                  << " topK=" << config.topK
                  << " sharedExpertNum=" << config.sharedExpertNum
                  << " sharedExpertRankNum=" << config.sharedExpertRankNum
                  << " tpWorldSize=" << config.tpWorldSize
                  << " tpRankId=" << config.tpRankId
                  << ", and expertTokenNumsType 0 or 1, got rankSize=" << rankSize << " rank=" << rank
                   << " expertTokenNumsType=" << expertTokenNumsType
                   << " quantMode=" << quantMode << " activeMaskType=" << activeMaskType
                   << " commQuantMode=" << commQuantMode
                  << " backend=" << (useMemory ? "memory" : "udma")
                  << " runMode=" << (runMode == DemoRunMode::DISPATCH ? "dispatch" :
                      (runMode == DemoRunMode::COMBINE ? "combine" : "dispatch_combine"))
                  << " expertMode=" << (expertMode == ExpertListMode::UNIFORM ? "uniform" :
                      (expertMode == ExpertListMode::RANDOM ? "random" : "explicit"))
                  << " expertSeed=" << expertSeed
                  << " mxfp8Format=" << (mxfp8Format == TileXREpDemo::Mxfp8Format::E4M3 ? "e4m3" : "e5m2")
                  << " expertIds=" << config.expertIds.size()
                  << " dtype=" << static_cast<int64_t>(dtype)
                  << " loopCount=" << loopCount << std::endl;
        return 2;
    }

    const int64_t localExpertNum = OutputLocalExpertNum(rank, rankSize, config);
    const int deviceId = GetDeviceIdFromEnv(rank, npuCount, firstNpu);
    bool aclReady = false;
    bool deviceSet = false;
    aclrtStream stream = nullptr;
    TileXRCommPtr comm = nullptr;
    std::vector<void *> buffers;

    if (!CheckAcl(aclInit(nullptr), "aclInit")) {
        Cleanup(comm, stream, deviceId, deviceSet, aclReady, buffers);
        return 1;
    }
    aclReady = true;
    if (!CheckAcl(aclrtSetDevice(deviceId), "aclrtSetDevice")) {
        Cleanup(comm, stream, deviceId, deviceSet, aclReady, buffers);
        return 1;
    }
    deviceSet = true;
    if (!CheckAcl(aclrtCreateStream(&stream), "aclrtCreateStream")) {
        Cleanup(comm, stream, deviceId, deviceSet, aclReady, buffers);
        return 1;
    }
    if (!CheckTileXR(TileXRCommInitRankLocal(rankSize, rank, &comm), "TileXRCommInitRankLocal")) {
        Cleanup(comm, stream, deviceId, deviceSet, aclReady, buffers);
        return 1;
    }

    std::vector<uint16_t> hostX(static_cast<std::size_t>(config.bs * config.h));
    for (int64_t token = 0; token < config.bs; ++token) {
        for (int64_t h = 0; h < config.h; ++h) {
            hostX[token * config.h + h] = (useMxfp8 || useCombineMxfp8) ?
                Mxfp8InputValue(dtype, rank, token, h) : InputValue(dtype, rank, token, h);
        }
    }
    const std::vector<int32_t> hostExpertIds = ExpertIds(config);
    const std::vector<uint8_t> hostActiveMask = ActiveMask(activeMaskType, config);
    const std::vector<ExpectedRoute> expectedRoutes = useMemory ?
        BuildExpectedMemoryRoutes(rank, rankSize, config, hostActiveMask) :
        BuildExpectedTpRoutes(rank, rankSize, config, hostActiveMask);
    const std::size_t expectedRouteCount = expectedRoutes.size();

    void *xDev = nullptr;
    void *expertIdsDev = nullptr;
    void *scalesDev = nullptr;
    void *xActiveMaskDev = nullptr;
    void *expandXDev = nullptr;
    void *dynamicScalesDev = nullptr;
    void *expertTokenNumsDev = nullptr;
    void *recvCountsDev = nullptr;
    void *tpRecvCountsDev = nullptr;
    void *assistDev = nullptr;
    void *expertOutDev = nullptr;
    void *expertScalesDev = nullptr;
    void *yOutDev = nullptr;
    void *workspaceDev = nullptr;
    void *rawWorkspaceDev = nullptr;
    TileXRUDMAMemHandle workspaceHandle = 0;

    const std::size_t xBytes = hostX.size() * sizeof(uint16_t);
    const std::size_t scaleBytes = sizeof(float);
    const std::size_t expertIdsBytes = hostExpertIds.size() * sizeof(int32_t);
    const std::size_t xActiveMaskBytes = hostActiveMask.size() * sizeof(uint8_t);
    const std::size_t expandedElements = std::max(static_cast<std::size_t>(config.expandedElements()),
        expectedRouteCount * static_cast<std::size_t>(config.h));
    const std::size_t maxRoutesPerRank = static_cast<std::size_t>(config.maxRoutesPerRank());
    const std::size_t expandElementBytes =
        (useStaticQuant || usePerTokenDynamicQuant || useMxfp8) ? sizeof(int8_t) : sizeof(uint16_t);
    const std::size_t expandXBytes = expandedElements * expandElementBytes;
    const std::size_t expertOutBytes = expandedElements * sizeof(uint16_t);
    const std::size_t expandedRows = expandedElements / config.h;
    const std::size_t payloadScaleBytesPerRow = usePerTokenDynamicQuant ? sizeof(float) :
        (useMxfp8 ? TileXREpDemo::Mxfp8ScaleCountPerRow(static_cast<std::size_t>(config.h)) : 0U);
    const std::size_t dynamicScalesBytes = expandedRows * payloadScaleBytesPerRow;
    const std::size_t expertTokenNumsBytes = localExpertNum * sizeof(int64_t);
    const std::size_t recvCountsElements = useMemory ?
        MemorySendCountsCount(rank, rankSize, config) : static_cast<std::size_t>(rankSize);
    const std::size_t recvCountsBytes = recvCountsElements * sizeof(int32_t);
    const std::size_t tpRecvCountsBytes = rankSize * sizeof(int32_t);
    const std::size_t assistInts = AssistInts(useMemory);
    const std::size_t assistBytes = expandedRows * assistInts * sizeof(int32_t);
    const std::size_t yOutBytes = static_cast<std::size_t>(config.bs * config.h) * sizeof(uint16_t);
    const std::vector<float> hostExpertScales = useCombineMxfp8 ?
        BuildExpertScales(config) : std::vector<float> {};
    const std::size_t expertScalesBytes = hostExpertScales.size() * sizeof(float);
    const std::size_t payloadRowBytes = config.h * expandElementBytes;
    const std::size_t dispatchWindowBytes =
        EpWindowBytes(rankSize, config, payloadRowBytes, payloadScaleBytesPerRow);
    const std::size_t dispatchPayloadBytes = AlignSize(dispatchWindowBytes, 32) *
        static_cast<std::size_t>(config.effectiveTpWorldSize() + 2);
    const std::size_t workspacePayloadBytes = std::max(dispatchPayloadBytes,
        EpRequiredWorkspaceBytes(rankSize, config, payloadRowBytes, payloadScaleBytesPerRow));
    const std::size_t workspaceBytes = ((workspacePayloadBytes + kUdmaRegistrationAlignment - 1) /
        kUdmaRegistrationAlignment) * kUdmaRegistrationAlignment;
    const TileXREpDemo::Mxfp8Tensor expectedMxfp8 = useMxfp8 ?
        BuildExpectedMxfp8Dispatch(expectedRoutes, config, dtype, expandedRows, mxfp8Format) :
        TileXREpDemo::Mxfp8Tensor {};

    StandaloneCombineInputs standaloneCombineInputs;
    if (runMode == DemoRunMode::COMBINE &&
        !BuildStandaloneCombineInputs(rank, rankSize, config, hostActiveMask, useMemory, dtype,
            useCombineMxfp8, expandedRows, &standaloneCombineInputs)) {
        std::cerr << "rank " << rank << " failed to construct standalone combine inputs" << std::endl;
        Cleanup(comm, stream, deviceId, deviceSet, aclReady, buffers);
        return 1;
    }

    if (!CheckAcl(aclrtMalloc(&xDev, xBytes, ACL_MEM_MALLOC_HUGE_FIRST), "aclrtMalloc x") ||
        !CheckAcl(aclrtMalloc(&expertIdsDev, expertIdsBytes, ACL_MEM_MALLOC_HUGE_FIRST), "aclrtMalloc expertIds") ||
        (useStaticQuant && !CheckAcl(aclrtMalloc(&scalesDev, scaleBytes, ACL_MEM_MALLOC_HUGE_FIRST),
            "aclrtMalloc scales")) ||
        (useActiveMask && !CheckAcl(aclrtMalloc(&xActiveMaskDev, xActiveMaskBytes, ACL_MEM_MALLOC_HUGE_FIRST),
            "aclrtMalloc xActiveMask")) ||
        !CheckAcl(aclrtMalloc(&expandXDev, expandXBytes, ACL_MEM_MALLOC_HUGE_FIRST), "aclrtMalloc expandX") ||
        ((usePerTokenDynamicQuant || useMxfp8) && !CheckAcl(aclrtMalloc(&dynamicScalesDev, dynamicScalesBytes,
            ACL_MEM_MALLOC_HUGE_FIRST), "aclrtMalloc dynamicScales")) ||
        !CheckAcl(aclrtMalloc(&expertTokenNumsDev, expertTokenNumsBytes, ACL_MEM_MALLOC_HUGE_FIRST),
            "aclrtMalloc expertTokenNums") ||
        !CheckAcl(aclrtMalloc(&recvCountsDev, recvCountsBytes, ACL_MEM_MALLOC_HUGE_FIRST), "aclrtMalloc recvCounts") ||
        (useTpRecvCounts && !CheckAcl(aclrtMalloc(&tpRecvCountsDev, tpRecvCountsBytes, ACL_MEM_MALLOC_HUGE_FIRST),
            "aclrtMalloc tpRecvCounts")) ||
        !CheckAcl(aclrtMalloc(&assistDev, assistBytes, ACL_MEM_MALLOC_HUGE_FIRST), "aclrtMalloc assist") ||
        !CheckAcl(aclrtMalloc(&expertOutDev, expertOutBytes, ACL_MEM_MALLOC_HUGE_FIRST), "aclrtMalloc expertOut") ||
        (useCombineMxfp8 && !CheckAcl(aclrtMalloc(&expertScalesDev, expertScalesBytes,
            ACL_MEM_MALLOC_HUGE_FIRST), "aclrtMalloc expertScales")) ||
        !CheckAcl(aclrtMalloc(&yOutDev, yOutBytes, ACL_MEM_MALLOC_HUGE_FIRST), "aclrtMalloc yOut") ||
        (!useMemory && !CheckAcl(aclrtMalloc(&rawWorkspaceDev,
            workspaceBytes + kUdmaRegistrationAlignment - 1,
            ACL_MEM_MALLOC_HUGE_FIRST), "aclrtMalloc workspace"))) {
        workspaceDev = rawWorkspaceDev;
        buffers = {xDev, expertIdsDev, scalesDev, xActiveMaskDev, expandXDev, dynamicScalesDev,
            expertTokenNumsDev, recvCountsDev, tpRecvCountsDev, assistDev, expertOutDev, expertScalesDev,
            yOutDev, workspaceDev};
        Cleanup(comm, stream, deviceId, deviceSet, aclReady, buffers);
        return 1;
    }
    if (!useMemory) {
        workspaceDev = reinterpret_cast<void *>(AlignAddress(
            reinterpret_cast<std::uintptr_t>(rawWorkspaceDev), kUdmaRegistrationAlignment));
    }
    buffers = {xDev, expertIdsDev, scalesDev, xActiveMaskDev, expandXDev, dynamicScalesDev, expertTokenNumsDev,
        recvCountsDev, tpRecvCountsDev, assistDev, expertOutDev, expertScalesDev, yOutDev, rawWorkspaceDev};

    if (!CheckAcl(aclrtMemcpy(xDev, xBytes, hostX.data(), xBytes, ACL_MEMCPY_HOST_TO_DEVICE), "copy x") ||
        !CheckAcl(aclrtMemcpy(expertIdsDev, expertIdsBytes, hostExpertIds.data(), expertIdsBytes,
            ACL_MEMCPY_HOST_TO_DEVICE), "copy expertIds") ||
        (useStaticQuant && !CheckAcl(aclrtMemcpy(scalesDev, scaleBytes, &staticQuantScale, scaleBytes,
            ACL_MEMCPY_HOST_TO_DEVICE), "copy scales")) ||
        (useActiveMask && !CheckAcl(aclrtMemcpy(xActiveMaskDev, xActiveMaskBytes, hostActiveMask.data(),
            xActiveMaskBytes, ACL_MEMCPY_HOST_TO_DEVICE), "copy xActiveMask")) ||
        !CheckAcl(aclrtMemset(expandXDev, expandXBytes, 0, expandXBytes), "memset expandX") ||
        ((usePerTokenDynamicQuant || useMxfp8) && !CheckAcl(aclrtMemset(dynamicScalesDev, dynamicScalesBytes, 0,
            dynamicScalesBytes), "memset dynamicScales")) ||
        !CheckAcl(aclrtMemset(expertTokenNumsDev, expertTokenNumsBytes, 0, expertTokenNumsBytes),
            "memset expertTokenNums") ||
        (runMode == DemoRunMode::COMBINE ?
            !CheckAcl(aclrtMemcpy(recvCountsDev, recvCountsBytes, standaloneCombineInputs.recvCounts.data(),
                recvCountsBytes, ACL_MEMCPY_HOST_TO_DEVICE), "copy standalone recvCounts") :
            !CheckAcl(aclrtMemset(recvCountsDev, recvCountsBytes, 0, recvCountsBytes), "memset recvCounts")) ||
        (useTpRecvCounts && !CheckAcl(aclrtMemset(tpRecvCountsDev, tpRecvCountsBytes, 0, tpRecvCountsBytes),
            "memset tpRecvCounts")) ||
        (runMode == DemoRunMode::COMBINE ?
            !CheckAcl(aclrtMemcpy(assistDev, assistBytes, standaloneCombineInputs.assist.data(), assistBytes,
                ACL_MEMCPY_HOST_TO_DEVICE), "copy standalone assist") :
            !CheckAcl(aclrtMemset(assistDev, assistBytes, 0, assistBytes), "memset assist")) ||
        (useCombineMxfp8 && !CheckAcl(aclrtMemcpy(expertScalesDev, expertScalesBytes,
            hostExpertScales.data(), expertScalesBytes, ACL_MEMCPY_HOST_TO_DEVICE), "copy expertScales")) ||
        !CheckAcl(aclrtMemset(yOutDev, yOutBytes, 0, yOutBytes), "memset yOut") ||
        (!useMemory && !CheckAcl(aclrtMemset(workspaceDev, workspaceBytes, 0, workspaceBytes),
            "memset workspace"))) {
        Cleanup(comm, stream, deviceId, deviceSet, aclReady, buffers);
        return 1;
    }

    if (!useMemory && !CheckTileXR(TileXRUDMARegister(comm, static_cast<GM_ADDR>(workspaceDev), workspaceBytes,
            &workspaceHandle), "TileXRUDMARegister workspace")) {
        Cleanup(comm, stream, deviceId, deviceSet, aclReady, buffers);
        return 1;
    }
    if (!useMemory) {
        g_workspaceHandle = workspaceHandle;
        g_workspaceRegistered = true;
    }

    const uint16_t expertOutOne = dtype == TileXR::TILEXR_DATA_TYPE_BFP16 ? kBf16One : kFp16One;
    const std::vector<uint16_t> hostExpertOut = runMode == DemoRunMode::COMBINE ?
        standaloneCombineInputs.expertOut : std::vector<uint16_t>(expandedElements, expertOutOne);
    if (!CheckAcl(aclrtMemcpy(expertOutDev, expertOutBytes, hostExpertOut.data(), expertOutBytes,
            ACL_MEMCPY_HOST_TO_DEVICE), "copy expertOut")) {
        Cleanup(comm, stream, deviceId, deviceSet, aclReady, buffers);
        return 1;
    }

    const auto DispatchOnce = [&]() -> int {
        if (useMemory) {
            return TileXRMoeEpDispatchMemory(xDev, static_cast<int32_t *>(expertIdsDev), scalesDev,
                static_cast<bool *>(xActiveMaskDev), activeMaskType, nullptr, comm, config.bs, config.h, config.topK,
                config.moeExpertNum,
                expertRankSize, ExpertRankForRank(rank, config), config.tpWorldSize, config.tpRankId, 0,
                config.sharedExpertNum, config.sharedExpertRankNum, quantMode, config.bs * rankSize,
                expertTokenNumsType, expandXDev, dynamicScalesDev, static_cast<int32_t *>(assistDev),
                static_cast<int64_t *>(expertTokenNumsDev), static_cast<int32_t *>(recvCountsDev),
                static_cast<int32_t *>(tpRecvCountsDev), nullptr, dtype, expandXOutDtype, stream);
        }
        return TileXRMoeEpDispatch(xDev, static_cast<int32_t *>(expertIdsDev), scalesDev,
            static_cast<bool *>(xActiveMaskDev), nullptr, comm, config.bs, config.h, config.topK, config.moeExpertNum,
            expertRankSize, ExpertRankForRank(rank, config), config.tpWorldSize, config.tpRankId, 0,
            config.sharedExpertNum, config.sharedExpertRankNum, quantMode, config.bs * rankSize,
            expertTokenNumsType, expandXDev, dynamicScalesDev, static_cast<int32_t *>(assistDev),
            static_cast<int64_t *>(expertTokenNumsDev), static_cast<int32_t *>(recvCountsDev),
            static_cast<int32_t *>(tpRecvCountsDev), nullptr, workspaceDev, dtype, stream);
    };

    const auto CombineOnce = [&]() -> int {
        void *combineExpertOutDev = runMode == DemoRunMode::DISPATCH_COMBINE ? expandXDev : expertOutDev;
        if (useMemory) {
            return TileXRMoeEpCombineMemory(combineExpertOutDev, static_cast<int32_t *>(assistDev),
                static_cast<int32_t *>(recvCountsDev), static_cast<float *>(expertScalesDev),
                static_cast<bool *>(xActiveMaskDev), activeMaskType, nullptr, comm, config.bs, config.h,
                config.topK, config.moeExpertNum, rankSize, rank,
                config.tpWorldSize, config.tpRankId, 0, config.sharedExpertNum, config.sharedExpertRankNum,
                commQuantMode, config.bs * rankSize, yOutDev, dtype, stream);
        }
        return TileXRMoeEpCombine(combineExpertOutDev, static_cast<int32_t *>(assistDev),
            static_cast<int32_t *>(recvCountsDev), comm, config.bs, config.h, config.topK, config.moeExpertNum,
            yOutDev, workspaceDev, dtype, stream);
    };

    if (runMode == DemoRunMode::COMBINE &&
        !DemoBarrierAll(rank, rankSize, "standalone combine inputs ready")) {
        Cleanup(comm, stream, deviceId, deviceSet, aclReady, buffers);
        return 1;
    }

    if (runMode == DemoRunMode::DISPATCH_COMBINE && !perfMode) {
        for (int loop = 0; loop < loopCount; ++loop) {
            if (!CheckTileXR(DispatchOnce(), useMemory ? "memory dispatch" : "udma dispatch") ||
                !CheckTileXR(CombineOnce(), useMemory ? "memory combine" : "udma combine")) {
                std::cerr << "rank " << rank << " dispatch/combine loop " << loop << " failed" << std::endl;
                Cleanup(comm, stream, deviceId, deviceSet, aclReady, buffers);
                return 1;
            }
        }
        if (!CheckAcl(aclrtSynchronizeStream(stream), "aclrtSynchronizeStream dispatch/combine") ||
            !DemoBarrierAll(rank, rankSize, "dispatch/combine synchronized")) {
            Cleanup(comm, stream, deviceId, deviceSet, aclReady, buffers);
            return 1;
        }
    }

    if (runDispatch && perfMode) {
        for (int loop = 0; loop < warmupCount; ++loop) {
            if (!CheckTileXR(DispatchOnce(), useMemory ? "memory dispatch warmup" : "udma dispatch warmup")) {
                std::cerr << "rank " << rank << " dispatch warmup " << loop << " failed" << std::endl;
                Cleanup(comm, stream, deviceId, deviceSet, aclReady, buffers);
                return 1;
            }
        }
        if (!CheckAcl(aclrtSynchronizeStream(stream), "aclrtSynchronizeStream dispatch warmup") ||
            !DemoBarrierAll(rank, rankSize, "dispatch measured batch start")) {
            Cleanup(comm, stream, deviceId, deviceSet, aclReady, buffers);
            return 1;
        }
        for (int loop = 0; loop < loopCount; ++loop) {
            if (!CheckTileXR(DispatchOnce(), useMemory ? "memory dispatch" : "udma dispatch")) {
                std::cerr << "rank " << rank << " dispatch perf loop " << loop << " failed" << std::endl;
                Cleanup(comm, stream, deviceId, deviceSet, aclReady, buffers);
                return 1;
            }
        }
        if (!CheckAcl(aclrtSynchronizeStream(stream), "aclrtSynchronizeStream dispatch measured batch")) {
            Cleanup(comm, stream, deviceId, deviceSet, aclReady, buffers);
            return 1;
        }
        if (!DemoBarrierAll(rank, rankSize, "dispatch perf synchronized")) {
            Cleanup(comm, stream, deviceId, deviceSet, aclReady, buffers);
            return 1;
        }
        std::cout << "rank " << rank << " dispatch perf warmup " << warmupCount << " loops " << loopCount
                  << " completed timing_source external_profiler" << std::endl;
    } else if (runDispatch && runMode != DemoRunMode::DISPATCH_COMBINE) {
        for (int loop = 0; loop < loopCount; ++loop) {
            const int dispatchRet = DispatchOnce();
            if (!CheckTileXR(dispatchRet, useMemory ? "memory dispatch" : "udma dispatch")) {
                std::cerr << "rank " << rank << " dispatch loop " << loop << " failed" << std::endl;
                Cleanup(comm, stream, deviceId, deviceSet, aclReady, buffers);
                return 1;
            }
            if (!CheckAcl(aclrtSynchronizeStream(stream), "aclrtSynchronizeStream dispatch") ||
                !DemoBarrierAll(rank, rankSize, "dispatch synchronized")) {
                std::cerr << "rank " << rank << " dispatch completion loop " << loop << " failed" << std::endl;
                Cleanup(comm, stream, deviceId, deviceSet, aclReady, buffers);
                return 1;
            }
        }
    }
    if (runCombine && perfMode) {
        for (int loop = 0; loop < warmupCount; ++loop) {
            if (!CheckTileXR(CombineOnce(), useMemory ? "memory combine warmup" : "udma combine warmup")) {
                std::cerr << "rank " << rank << " combine warmup " << loop << " failed" << std::endl;
                Cleanup(comm, stream, deviceId, deviceSet, aclReady, buffers);
                return 1;
            }
        }
        if (!CheckAcl(aclrtSynchronizeStream(stream), "aclrtSynchronizeStream combine warmup") ||
            !DemoBarrierAll(rank, rankSize, "combine measured batch start")) {
            Cleanup(comm, stream, deviceId, deviceSet, aclReady, buffers);
            return 1;
        }
        for (int loop = 0; loop < loopCount; ++loop) {
            if (!CheckTileXR(CombineOnce(), useMemory ? "memory combine" : "udma combine")) {
                std::cerr << "rank " << rank << " combine perf loop " << loop << " failed" << std::endl;
                Cleanup(comm, stream, deviceId, deviceSet, aclReady, buffers);
                return 1;
            }
        }
        if (!CheckAcl(aclrtSynchronizeStream(stream), "aclrtSynchronizeStream combine measured batch")) {
            Cleanup(comm, stream, deviceId, deviceSet, aclReady, buffers);
            return 1;
        }
        if (!DemoBarrierAll(rank, rankSize, "combine perf synchronized")) {
            Cleanup(comm, stream, deviceId, deviceSet, aclReady, buffers);
            return 1;
        }
        std::cout << "rank " << rank << " combine perf warmup " << warmupCount << " loops " << loopCount
                  << " completed timing_source external_profiler" << std::endl;
    } else if (runMode == DemoRunMode::COMBINE) {
        for (int loop = 0; loop < loopCount; ++loop) {
            const int combineRet = CombineOnce();
            if (!CheckTileXR(combineRet, useMemory ? "memory combine" : "udma combine") ||
                !CheckAcl(aclrtSynchronizeStream(stream), "aclrtSynchronizeStream combine") ||
                !DemoBarrierAll(rank, rankSize, "combine synchronized")) {
                std::cerr << "rank " << rank << " combine loop " << loop << " failed" << std::endl;
                Cleanup(comm, stream, deviceId, deviceSet, aclReady, buffers);
                return 1;
            }
        }
    }

    std::vector<uint8_t> hostExpandX(expandXBytes);
    std::vector<int64_t> hostExpertTokenNums(localExpertNum);
    std::vector<int32_t> hostRecvCounts(recvCountsElements);
    std::vector<int32_t> hostTpRecvCounts(rankSize);
    std::vector<int32_t> hostAssist(expandedRows * assistInts);
    std::vector<uint8_t> hostDynamicScales(dynamicScalesBytes);
    std::vector<uint16_t> hostYOut(static_cast<std::size_t>(config.bs * config.h));

    if (runDispatch && (!CheckAcl(aclrtMemcpy(hostExpandX.data(), expandXBytes, expandXDev, expandXBytes,
            ACL_MEMCPY_DEVICE_TO_HOST), "copy expandX") ||
        !CheckAcl(aclrtMemcpy(hostExpertTokenNums.data(), expertTokenNumsBytes, expertTokenNumsDev,
            expertTokenNumsBytes, ACL_MEMCPY_DEVICE_TO_HOST), "copy expertTokenNums") ||
        !CheckAcl(aclrtMemcpy(hostRecvCounts.data(), recvCountsBytes, recvCountsDev, recvCountsBytes,
            ACL_MEMCPY_DEVICE_TO_HOST), "copy recvCounts") ||
        !CheckAcl(aclrtMemcpy(hostAssist.data(), assistBytes, assistDev, assistBytes,
            ACL_MEMCPY_DEVICE_TO_HOST), "copy assist"))) {
        Cleanup(comm, stream, deviceId, deviceSet, aclReady, buffers);
        return 1;
    }
    if (runCombine && !CheckAcl(aclrtMemcpy(hostYOut.data(), yOutBytes, yOutDev, yOutBytes,
            ACL_MEMCPY_DEVICE_TO_HOST), "copy yOut")) {
        Cleanup(comm, stream, deviceId, deviceSet, aclReady, buffers);
        return 1;
    }
    if (runDispatch && useTpRecvCounts &&
        !CheckAcl(aclrtMemcpy(hostTpRecvCounts.data(), tpRecvCountsBytes, tpRecvCountsDev,
            tpRecvCountsBytes, ACL_MEMCPY_DEVICE_TO_HOST), "copy tpRecvCounts")) {
        Cleanup(comm, stream, deviceId, deviceSet, aclReady, buffers);
        return 1;
    }
    if (runDispatch && (usePerTokenDynamicQuant || useMxfp8) &&
        !CheckAcl(aclrtMemcpy(hostDynamicScales.data(), dynamicScalesBytes,
            dynamicScalesDev, dynamicScalesBytes, ACL_MEMCPY_DEVICE_TO_HOST), "copy dynamicScales")) {
        Cleanup(comm, stream, deviceId, deviceSet, aclReady, buffers);
        return 1;
    }
    const bool dispatchOk = !runDispatch ||
        (ValidateOutputs(rank, rankSize, config, hostExpandX, hostExpertTokenNums, hostRecvCounts,
            hostAssist, hostDynamicScales, hostActiveMask, expertTokenNumsType, useStaticQuant,
            usePerTokenDynamicQuant, useMxfp8, staticQuantScale, useMemory, dtype, expectedMxfp8) &&
        (!useTpRecvCounts || ValidateTpRecvCounts(rank, rankSize, config, hostActiveMask, hostRecvCounts,
            hostTpRecvCounts)));
    if (runDispatch) {
        std::cout << "rank " << rank << " dispatch validation " << (dispatchOk ? "PASS" : "FAIL") << std::endl;
    }
    const bool combineOk = !runCombine || ValidateCombineOutputs(rank, config, hostYOut, hostActiveMask, dtype,
        runMode == DemoRunMode::DISPATCH_COMBINE, commQuantMode, hostExpertScales);
    if (runCombine) {
        std::cout << "rank " << rank << " combine validation " << (combineOk ? "PASS" : "FAIL") << std::endl;
    }
    Cleanup(comm, stream, deviceId, deviceSet, aclReady, buffers);
    return dispatchOk && combineOk ? 0 : 1;
}
