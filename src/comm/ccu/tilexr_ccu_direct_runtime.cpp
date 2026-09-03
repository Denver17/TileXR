/*
 * Copyright (c) 2026 TileXR Project
 * Licensed under the Apache License, Version 2.0
 */

#include "ccu/tilexr_ccu_direct_runtime.h"

#include "ccu/tilexr_ccu_topology.h"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <limits>
#include <thread>
#include <sstream>
#include <chrono>

namespace TileXR {
namespace {

constexpr uint32_t TILEXR_CCU_DEFAULT_DIRECT_SQ_DEPTH = 8;
constexpr uint32_t TILEXR_CCU_DIRECT_CCUM_SQE_BYTES = 64;
constexpr uint32_t TILEXR_CCU_DIRECT_SQ_EBB_WORDS = 4;
constexpr uint32_t TILEXR_CCU_DIRECT_LOOP_JETTY_ID = 1024;
constexpr uint32_t TILEXR_CCU_DIRECT_LOOP_JETTY_CTX_ID = 0;
constexpr uint32_t TILEXR_CCU_HCOMM_INNER_FE_JETTY_NUM = 23;
constexpr uint32_t TILEXR_CCU_HCOMM_OUTER_FE_START_JETTY_CTX_ID = 92;
constexpr uint32_t TILEXR_CCU_HCOMM_OUTER_FE_JETTY_NUM = 36;
constexpr uint32_t TILEXR_CCU_HCOMM_MAX_INNER_FE_ID = 7;
constexpr uint64_t TILEXR_CCU_V1_WQE_BASIC_BLOCK_OFFSET = TILEXR_CCU_V1_CCUM_OFFSET + 0x800000ULL;
constexpr uint64_t TILEXR_CCU_DIRECT_SQ_BUFFER_BYTES = 256ULL * 1024ULL;
constexpr uint32_t TILEXR_CCU_DIRECT_CCU_POLL_CQ_DEPTH = 64;
constexpr uint32_t TILEXR_CCU_DIRECT_ENDPOINT_ROUTE_ASYNC_MAX_POLLS = 1000;
constexpr uint32_t TILEXR_CCU_DIRECT_ENDPOINT_ROUTE_ASYNC_SLEEP_US = 1000;
constexpr uint32_t TILEXR_CCU_DIRECT_ENDPOINT_ROUTE_MAX_ATTEMPTS = 8;
constexpr uint8_t TILEXR_CCU_DIRECT_ENDPOINT_ERR_TIMEOUT = 16;
constexpr uint8_t TILEXR_CCU_DIRECT_CTP_ENDPOINT_ERR_TIMEOUT = 8;
constexpr int TILEXR_CCU_DIRECT_MAX_RANK_SIZE = 128;
constexpr int TILEXR_CCU_HCCP_JFC_MODE_CCU_POLL = 2;
constexpr int TILEXR_CCU_HCCP_ASYNC_EAGAIN = 128301;
constexpr const char* TILEXR_CCU_DIRECT_HDC_TYPE_ENV = "TILEXR_CCU_DIRECT_HDC_TYPE";
constexpr const char* TILEXR_CCU_DIRECT_RESOURCE_WINDOW_TOKEN_ID_ENV =
    "TILEXR_CCU_DIRECT_RESOURCE_WINDOW_TOKEN_ID";
constexpr const char* TILEXR_CCU_DIRECT_RESOURCE_WINDOW_RAW_TOKEN_ID_ENV =
    "TILEXR_CCU_DIRECT_RESOURCE_WINDOW_RAW_TOKEN_ID";
constexpr const char* TILEXR_CCU_DIRECT_RESOURCE_WINDOW_TOKEN_VALUE_ENV =
    "TILEXR_CCU_DIRECT_RESOURCE_WINDOW_TOKEN_VALUE";
constexpr const char* TILEXR_CCU_DIRECT_RESOURCE_WINDOW_REGISTRATION_MODE_ENV =
    "TILEXR_CCU_DIRECT_RESOURCE_WINDOW_REGISTRATION_MODE";
constexpr const char* TILEXR_CCU_DIRECT_RESOURCE_WINDOW_EID_INDEX_ENV =
    "TILEXR_CCU_DIRECT_RESOURCE_WINDOW_EID_INDEX";
constexpr const char* TILEXR_CCU_DIRECT_RESOURCE_WINDOW_EID_ENV =
    "TILEXR_CCU_DIRECT_RESOURCE_WINDOW_EID";
constexpr const char* TILEXR_CCU_DIRECT_ENDPOINT_ROUTE_COLLECTION_MODE_ENV =
    "TILEXR_CCU_DIRECT_ENDPOINT_ROUTE_COLLECTION_MODE";
constexpr const char* TILEXR_CCU_DIRECT_TRACE_ENDPOINT_ROUTE_ENV =
    "TILEXR_CCU_DIRECT_TRACE_ENDPOINT_ROUTE";
constexpr const char* TILEXR_CCU_DIRECT_TRUST_SYNTHETIC_ENDPOINT_ROUTE_ENV =
    "TILEXR_CCU_DIRECT_TRUST_SYNTHETIC_ENDPOINT_ROUTE";
constexpr const char* TILEXR_CCU_DIRECT_ENDPOINT_ROUTE_EXCHANGE_MODE_ENV =
    "TILEXR_CCU_DIRECT_ENDPOINT_ROUTE_EXCHANGE_MODE";
constexpr const char* TILEXR_CCU_HCCL_ROOT_INFO_PATH = "/etc/hccl_rootinfo.json";
constexpr const char* TILEXR_CCU_DIRECT_REMOTE_CCU_VA_OFFSET_ENV =
    "TILEXR_CCU_DIRECT_REMOTE_CCU_VA_OFFSET";
constexpr const char* TILEXR_CCU_DIRECT_RECOVER_TASK_KILL_STATE_ENV =
    "TILEXR_CCU_DIRECT_RECOVER_TASK_KILL_STATE";
constexpr uint8_t TILEXR_CCU_DIRECT_DEFAULT_DIE_ID = 0;
constexpr uint64_t TILEXR_CCU_UB_MEM_PAGE_SIZE = 4096ULL;
constexpr uint32_t TILEXR_CCU_URMA_TOKEN_ID_RIGHT_SHIFT = 8;
constexpr uint32_t TILEXR_CCU_TP_HANDLE_REQUEST_NUM = 8;
constexpr uint32_t TILEXR_CCU_TP_ATTR_BITMAP_SL = 1U << 10U;
constexpr uint32_t TILEXR_CCU_TP_ATTR_BITMAP_SL_AVAILABLE = 1U << 17U;
constexpr uint32_t TILEXR_CCU_DEFAULT_HCCL_QOS = 4;
constexpr uint32_t TILEXR_CCU_UBOE_DEV_FLAG_RIGHT_SHIFT = 19U;

struct TileXRCcuEndpointTpHandleExchange {
    uint64_t tpHandles[TILEXR_CCU_DIRECT_MAX_RANK_SIZE] = {};
    uint32_t psn = 0;
};

void ResetReport(TileXRCcuDirectRuntimeReport* report)
{
    if (report != nullptr) {
        *report = TileXRCcuDirectRuntimeReport{};
    }
}

int Fail(TileXRCcuDirectRuntimeReport* report, const std::string& message, int code = TILEXR_ERROR_NOT_FOUND)
{
    if (report != nullptr) {
        report->message = message;
    }
    return code;
}

bool IsEmptyEid(const std::array<uint8_t, TILEXR_CCU_EID_BYTES>& eid)
{
    return std::all_of(eid.begin(), eid.end(), [](uint8_t value) { return value == 0; });
}

std::array<uint8_t, TILEXR_CCU_EID_BYTES> ReverseEndpointEid(
    const std::array<uint8_t, TILEXR_CCU_EID_BYTES>& eid)
{
    std::array<uint8_t, TILEXR_CCU_EID_BYTES> reversed {};
    std::reverse_copy(eid.begin(), eid.end(), reversed.begin());
    return reversed;
}

std::string FormatEndpointEid(const std::array<uint8_t, TILEXR_CCU_EID_BYTES>& eid)
{
    std::ostringstream text;
    text << std::hex << std::setfill('0');
    for (uint8_t byte : eid) {
        text << std::setw(2) << static_cast<uint32_t>(byte);
    }
    return text.str();
}

bool HasCompleteEndpointRoute(const TileXRCcuLowerLayerTransportRoute& route)
{
    return !IsEmptyEid(route.remoteEid) &&
        route.doorbellVa != 0 &&
        route.doorbellTokenId != 0 &&
        route.sqDepth != 0;
}

struct TileXRCcuPeerEndpointOffer {
    uint64_t resourceAddr = 0;
    uint32_t resourceTokenId = 0;
    uint32_t resourceRawTokenId = 0;
    uint32_t resourceTokenValue = 0;
    uint32_t jettyTokenValue = 0;
    std::array<uint8_t, TILEXR_CCU_EID_BYTES> eid {};
    TileXRCcuHccpQpKey qpKey {};
    uint32_t psn = 0;
    uint32_t funcId = 0;
    bool funcIdValid = false;
    bool valid = false;
};

bool SameEid(
    const std::array<uint8_t, TILEXR_CCU_EID_BYTES>& expected,
    const TileXRCcuHccpEid& actual)
{
    return std::memcmp(expected.data(), actual.raw, expected.size()) == 0;
}

bool UseImportedPeerEndpointRoute()
{
    const char* mode = std::getenv(TILEXR_CCU_DIRECT_ENDPOINT_ROUTE_EXCHANGE_MODE_ENV);
    return mode == nullptr || mode[0] == '\0' || std::strcmp(mode, "imported_peer") == 0;
}

uint8_t SelectDirectCcuCleanupDieId()
{
    const char* value = std::getenv("TILEXR_CCU_DIRECT_INSTALL_DIE_ID");
    if (value == nullptr || value[0] == '\0') {
        return TILEXR_CCU_DIRECT_DEFAULT_DIE_ID;
    }
    char* end = nullptr;
    const unsigned long parsed = std::strtoul(value, &end, 10);
    return end != value && *end == '\0' && parsed <= UINT8_MAX ?
        static_cast<uint8_t>(parsed) :
        TILEXR_CCU_DIRECT_DEFAULT_DIE_ID;
}

bool RecoverTaskKillState()
{
    const char* value = std::getenv(TILEXR_CCU_DIRECT_RECOVER_TASK_KILL_STATE_ENV);
    return value != nullptr && value[0] != '\0' && value[0] != '0';
}

uint64_t SelectResourceWindowBytes(const TileXRCcuBasicInfo& basicInfo)
{
    (void)basicInfo;
    return TILEXR_CCU_RESOURCE_WINDOW_BYTES;
}

std::string RankEnvName(const char* base, int rank)
{
    std::ostringstream name;
    name << base << "_RANK" << rank;
    return name.str();
}

const char* SelectRankedEnv(const char* base, int rank)
{
    const std::string rankedName = RankEnvName(base, rank);
    const char* ranked = std::getenv(rankedName.c_str());
    if (ranked != nullptr && ranked[0] != '\0') {
        return ranked;
    }
    const char* value = std::getenv(base);
    return value != nullptr && value[0] != '\0' ? value : nullptr;
}

bool ParseUnsignedEnv(const char* value, uint64_t* out)
{
    if (value == nullptr || value[0] == '\0' || out == nullptr) {
        return false;
    }
    errno = 0;
    char* end = nullptr;
    const unsigned long long parsed = std::strtoull(value, &end, 0);
    if (errno != 0 || end == value || *end != '\0') {
        return false;
    }
    *out = static_cast<uint64_t>(parsed);
    return true;
}

uint64_t SelectRemoteCcuVaOffset()
{
    uint64_t value = 0;
    return ParseUnsignedEnv(std::getenv(TILEXR_CCU_DIRECT_REMOTE_CCU_VA_OFFSET_ENV), &value) ? value : 0;
}

bool ParseU32RankedEnv(const char* base, int rank, uint32_t* out)
{
    uint64_t value = 0;
    if (!ParseUnsignedEnv(SelectRankedEnv(base, rank), &value) || value > 0xffffffffULL || out == nullptr) {
        return false;
    }
    *out = static_cast<uint32_t>(value);
    return true;
}

bool HasRankedEnv(const char* base, int rank)
{
    return SelectRankedEnv(base, rank) != nullptr;
}

std::array<uint8_t, TILEXR_CCU_EID_BYTES> CopyRawEid(const TileXRCcuHccpEid& eid)
{
    std::array<uint8_t, TILEXR_CCU_EID_BYTES> copied {};
    for (uint32_t i = 0; i < TILEXR_CCU_EID_BYTES; ++i) {
        copied[i] = eid.raw[i];
    }
    return copied;
}

bool IsRaCtxResourceWindowRegistrationMode()
{
    const char* value = std::getenv(TILEXR_CCU_DIRECT_RESOURCE_WINDOW_REGISTRATION_MODE_ENV);
    if (value == nullptr || value[0] == '\0') {
        return false;
    }
    const std::string mode(value);
    return mode == "ra_ctx" || mode == "ractx" || mode == "public_ra_ctx" || mode == "1";
}

bool TrustSyntheticEndpointRouteForDiagnostics()
{
    const char* value = std::getenv(TILEXR_CCU_DIRECT_TRUST_SYNTHETIC_ENDPOINT_ROUTE_ENV);
    return value != nullptr && value[0] != '\0' && value[0] != '0';
}

bool TraceEndpointRoute()
{
    const char* value = std::getenv(TILEXR_CCU_DIRECT_TRACE_ENDPOINT_ROUTE_ENV);
    return value != nullptr && value[0] != '\0' && value[0] != '0';
}

void TraceEndpointRouteStep(const std::string& message)
{
    if (TraceEndpointRoute()) {
        std::cerr << "TileXRDirectCcuTrace endpointRoute " << message << std::endl;
    }
}

void TraceTaskKillStep(
    const char* step,
    uint8_t dieId,
    int ret,
    const TileXRCcuDriverAdapterReport& report)
{
    if (!TraceEndpointRoute()) {
        return;
    }
    std::cerr << "TileXRDirectCcuTrace taskKill" << step
              << " dieId=" << static_cast<uint32_t>(dieId)
              << " ret=" << ret
              << " opcode=" << report.opcode
              << " driverRet=" << report.driverRet
              << " opRet=" << report.opRet
              << " message=\"" << report.message << "\""
              << std::endl;
}

void TraceRaCtxEidInfos(const std::vector<TileXRCcuHccpDevEidInfo>& eidInfos)
{
    if (!TraceEndpointRoute()) {
        return;
    }
    for (size_t i = 0; i < eidInfos.size(); ++i) {
        std::cerr << "TileXRDirectCcuTrace endpointRoute raCtxEidInfo"
                  << " ordinal=" << i
                  << " eidIndex=" << eidInfos[i].eidIndex
                  << " dieId=" << eidInfos[i].dieId
                  << " funcId=" << eidInfos[i].funcId
                  << " devFeature=0x" << std::hex << eidInfos[i].resv << std::dec
                  << " eid=" << FormatEndpointEid(CopyRawEid(eidInfos[i].eid))
                  << std::endl;
    }
}

bool ParseEndpointEid(const char* value, std::array<uint8_t, TILEXR_CCU_EID_BYTES>* eid);

bool BuildRaCtxResourceWindowEidCandidates(
    int rank,
    uint8_t dieId,
    const std::vector<TileXRCcuHccpDevEidInfo>& eidInfos,
    std::vector<TileXRCcuHccpDevEidInfo>* candidates)
{
    if (eidInfos.empty() || candidates == nullptr) {
        return false;
    }
    candidates->clear();
    TraceRaCtxEidInfos(eidInfos);
    const char* configuredEid = SelectRankedEnv(TILEXR_CCU_DIRECT_RESOURCE_WINDOW_EID_ENV, rank);
    if (configuredEid != nullptr) {
        std::array<uint8_t, TILEXR_CCU_EID_BYTES> expected {};
        if (!ParseEndpointEid(configuredEid, &expected)) {
            return false;
        }
        for (const auto& eidInfo : eidInfos) {
            if (CopyRawEid(eidInfo.eid) == expected) {
                candidates->push_back(eidInfo);
                return true;
            }
        }
        return false;
    }
    const char* configured = SelectRankedEnv(TILEXR_CCU_DIRECT_RESOURCE_WINDOW_EID_INDEX_ENV, rank);
    if (configured != nullptr) {
        uint64_t configuredIndex = 0;
        if (!ParseUnsignedEnv(configured, &configuredIndex) || configuredIndex > 0xffffffffULL) {
            return false;
        }
        for (const auto& eidInfo : eidInfos) {
            if (eidInfo.eidIndex == static_cast<uint32_t>(configuredIndex)) {
                candidates->push_back(eidInfo);
                return true;
            }
        }
        return false;
    }
    for (auto it = eidInfos.rbegin(); it != eidInfos.rend(); ++it) {
        const bool uboeOnly =
            ((it->resv >> TILEXR_CCU_UBOE_DEV_FLAG_RIGHT_SHIFT) & 1U) != 0U;
        if (it->dieId == dieId && !uboeOnly) {
            candidates->push_back(*it);
        }
    }
    return !candidates->empty();
}

bool IsRaCtxLoopEndpointRouteCollectionMode()
{
    const char* value = std::getenv(TILEXR_CCU_DIRECT_ENDPOINT_ROUTE_COLLECTION_MODE_ENV);
    if (value == nullptr || value[0] == '\0') {
        return false;
    }
    const std::string mode(value);
    return mode == "ra_ctx_loop" || mode == "ractx_loop" || mode == "public_ra_ctx_loop";
}

uint64_t AlignResourceWindowAddr(uint64_t addr)
{
    return addr & ~(TILEXR_CCU_UB_MEM_PAGE_SIZE - 1ULL);
}

bool HasRaCtxResourceWindowSymbols(const TileXRCcuHccpLoader& loader)
{
    return loader.RaGetDevEidInfoNum != nullptr &&
        loader.RaGetDevEidInfoList != nullptr &&
        loader.RaCtxInit != nullptr &&
        loader.RaCtxDeinit != nullptr &&
        loader.RaCtxTokenIdAlloc != nullptr &&
        loader.RaCtxTokenIdFree != nullptr &&
        loader.RaGetSecRandom != nullptr &&
        loader.RaCtxLmemRegister != nullptr &&
        loader.RaCtxLmemUnregister != nullptr;
}

bool HasRaCtxRemoteMemoryImportSymbols(const TileXRCcuHccpLoader& loader)
{
    return loader.RaCtxRmemImport != nullptr &&
        loader.RaCtxRmemUnimport != nullptr;
}

bool HasRaCtxEndpointRouteSymbols(const TileXRCcuHccpLoader& loader)
{
    return loader.RaCtxCqCreate != nullptr &&
        loader.RaCtxCqDestroy != nullptr &&
        loader.RaCtxQpCreate != nullptr &&
        loader.RaCtxQpDestroy != nullptr &&
        loader.RaCtxQpImport != nullptr &&
        loader.RaCtxQpUnimport != nullptr &&
        loader.RaGetTpInfoListAsync != nullptr &&
        loader.RaGetAsyncReqResult != nullptr;
}

uint32_t CountAvailableSl(uint16_t mask)
{
    uint32_t count = 0;
    for (uint32_t bit = 0; bit < 16U; ++bit) {
        count += (mask & (1U << bit)) != 0U ? 1U : 0U;
    }
    return count;
}

uint8_t SlAtRank(uint16_t mask, uint32_t rank)
{
    uint32_t seen = 0;
    for (uint8_t bit = 0; bit < 16U; ++bit) {
        if ((mask & (1U << bit)) != 0U && seen++ == rank) {
            return bit;
        }
    }
    return 0;
}

bool MapQosToTpAndSl(
    uint32_t qos,
    uint32_t tpCount,
    uint16_t slMask,
    uint32_t* tpIndex,
    uint8_t* mappedSl)
{
    if (tpIndex == nullptr || mappedSl == nullptr || tpCount == 0) {
        return false;
    }
    const uint32_t slCount = CountAvailableSl(slMask);
    const uint32_t k = std::min(tpCount, slCount);
    if (k == 0) {
        return false;
    }
    const uint32_t groupCount = std::min(8U, k);
    const uint32_t q = qos & 7U;
    const uint32_t group = k == 3U ? (q < 3U ? 0U : (q < 5U ? 1U : 2U)) :
        (q * groupCount) / 8U;
    const uint32_t slot = (group * k) / groupCount;
    if (slot >= k || slot >= tpCount) {
        return false;
    }
    const uint32_t slRank = (slCount - 1U) - slot;
    *tpIndex = (k - 1U) - slot;
    *mappedSl = SlAtRank(slMask, slRank);
    return true;
}

uint32_t SelectEndpointRouteSqDepth()
{
    return TILEXR_CCU_DEFAULT_DIRECT_SQ_DEPTH;
}

uint32_t SelectEndpointRouteSqBytes(uint32_t sqDepth)
{
    return sqDepth * TILEXR_CCU_DIRECT_SQ_EBB_WORDS * TILEXR_CCU_DIRECT_CCUM_SQE_BYTES;
}

bool SelectEndpointRouteJettyCtxId(uint32_t pfeId, uint32_t peerOrdinal, uint16_t* jettyCtxId)
{
    if (jettyCtxId == nullptr) {
        return false;
    }
    const uint32_t start = pfeId > TILEXR_CCU_HCOMM_MAX_INNER_FE_ID ?
        TILEXR_CCU_HCOMM_OUTER_FE_START_JETTY_CTX_ID :
        pfeId * TILEXR_CCU_HCOMM_INNER_FE_JETTY_NUM;
    const uint32_t count = pfeId > TILEXR_CCU_HCOMM_MAX_INNER_FE_ID ?
        TILEXR_CCU_HCOMM_OUTER_FE_JETTY_NUM :
        TILEXR_CCU_HCOMM_INNER_FE_JETTY_NUM;
    if (peerOrdinal >= count || start + peerOrdinal >= 128U) {
        return false;
    }
    *jettyCtxId = static_cast<uint16_t>(start + peerOrdinal);
    return true;
}

uint64_t SelectEndpointRouteSqVa(
    const TileXRCcuLocalResourceWindowInfo& localResourceWindow,
    uint16_t jettyCtxId)
{
    return localResourceWindow.addr + TILEXR_CCU_V1_WQE_BASIC_BLOCK_OFFSET +
        static_cast<uint64_t>(jettyCtxId) * TILEXR_CCU_DIRECT_SQ_BUFFER_BYTES;
}

int WaitRaCtxAsyncRequest(TileXRCcuHccpLoader& loader, void* reqHandle)
{
    if (loader.RaGetAsyncReqResult == nullptr || reqHandle == nullptr) {
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    for (uint32_t poll = 0; poll < TILEXR_CCU_DIRECT_ENDPOINT_ROUTE_ASYNC_MAX_POLLS; ++poll) {
        int reqResult = 0;
        const int ret = loader.RaGetAsyncReqResult(reqHandle, &reqResult);
        if (TraceEndpointRoute()) {
            std::cerr << "TileXRDirectCcuTrace endpointRoute asyncPoll poll=" << poll
                      << " ret=" << ret
                      << " reqResult=" << reqResult << std::endl;
        }
        if (ret == TILEXR_CCU_HCCP_ASYNC_EAGAIN) {
            std::this_thread::sleep_for(
                std::chrono::microseconds(TILEXR_CCU_DIRECT_ENDPOINT_ROUTE_ASYNC_SLEEP_US));
            continue;
        }
        if (ret != 0 || reqResult != 0) {
            return TILEXR_ERROR_MKIRT;
        }
        return TILEXR_SUCCESS;
    }
    return TILEXR_ERROR_TIMEOUT;
}

bool ApplyResourceWindowTokenOverride(
    int rank,
    TileXRCcuLocalResourceWindowInfo* window)
{
    if (window == nullptr || !HasRankedEnv(TILEXR_CCU_DIRECT_RESOURCE_WINDOW_TOKEN_ID_ENV, rank)) {
        return false;
    }
    uint32_t tokenId = 0;
    if (!ParseU32RankedEnv(TILEXR_CCU_DIRECT_RESOURCE_WINDOW_TOKEN_ID_ENV, rank, &tokenId) || tokenId == 0) {
        return false;
    }

    uint32_t rawTokenId = tokenId;
    if (HasRankedEnv(TILEXR_CCU_DIRECT_RESOURCE_WINDOW_RAW_TOKEN_ID_ENV, rank) &&
        !ParseU32RankedEnv(TILEXR_CCU_DIRECT_RESOURCE_WINDOW_RAW_TOKEN_ID_ENV, rank, &rawTokenId)) {
        return false;
    }

    uint32_t tokenValue = 0;
    if (HasRankedEnv(TILEXR_CCU_DIRECT_RESOURCE_WINDOW_TOKEN_VALUE_ENV, rank) &&
        !ParseU32RankedEnv(TILEXR_CCU_DIRECT_RESOURCE_WINDOW_TOKEN_VALUE_ENV, rank, &tokenValue)) {
        return false;
    } else if (!HasRankedEnv(TILEXR_CCU_DIRECT_RESOURCE_WINDOW_TOKEN_VALUE_ENV, rank)) {
        tokenValue = window->tokenValue;
    }

    window->tokenId = tokenId;
    window->rawTokenId = rawTokenId;
    window->tokenValue = tokenValue;
    return true;
}

int SelectDirectCcuHdcType()
{
    uint64_t value = 0;
    if (!ParseUnsignedEnv(std::getenv(TILEXR_CCU_DIRECT_HDC_TYPE_ENV), &value)) {
        return TILEXR_CCU_HDC_SERVICE_TYPE_RDMA_V2;
    }
    if (value == TILEXR_CCU_HDC_SERVICE_TYPE_RDMA || value == TILEXR_CCU_HDC_SERVICE_TYPE_RDMA_V2) {
        return static_cast<int>(value);
    }
    return TILEXR_CCU_HDC_SERVICE_TYPE_RDMA_V2;
}

bool ParseHexBytePair(char high, char low, uint8_t* out)
{
    auto hexValue = [](char c) -> int {
        if (c >= '0' && c <= '9') {
            return c - '0';
        }
        if (c >= 'a' && c <= 'f') {
            return c - 'a' + 10;
        }
        if (c >= 'A' && c <= 'F') {
            return c - 'A' + 10;
        }
        return -1;
    };
    const int highValue = hexValue(high);
    const int lowValue = hexValue(low);
    if (highValue < 0 || lowValue < 0 || out == nullptr) {
        return false;
    }
    *out = static_cast<uint8_t>((highValue << 4U) | lowValue);
    return true;
}

bool ParseEndpointEid(const char* value, std::array<uint8_t, TILEXR_CCU_EID_BYTES>* eid)
{
    if (value == nullptr || eid == nullptr) {
        return false;
    }
    std::string text(value);
    if (text.size() >= 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
        text.erase(0, 2);
    }
    text.erase(std::remove_if(text.begin(), text.end(), [](char c) {
        return c == ':' || c == '-' || c == '_' || c == ' ';
    }), text.end());
    if (text.size() != TILEXR_CCU_EID_BYTES * 2U) {
        return false;
    }
    std::array<uint8_t, TILEXR_CCU_EID_BYTES> parsed {};
    for (uint32_t i = 0; i < TILEXR_CCU_EID_BYTES; ++i) {
        if (!ParseHexBytePair(text[i * 2U], text[i * 2U + 1U], &parsed[i])) {
            return false;
        }
    }
    *eid = parsed;
    return true;
}

int CollectLocalEndpointRouteFromEnv(
    int rank,
    TileXRCcuLowerLayerTransportRoute* route,
    TileXRCcuDirectRuntimeReport* report)
{
    if (route == nullptr) {
        return Fail(report, "missing output direct CCU local endpoint route", TILEXR_ERROR_PARA_CHECK_FAIL);
    }

    const char* eidEnv = SelectRankedEnv("TILEXR_CCU_DIRECT_LOCAL_ENDPOINT_EID", rank);
    const char* tpnEnv = SelectRankedEnv("TILEXR_CCU_DIRECT_LOCAL_ENDPOINT_TPN", rank);
    const char* doorbellVaEnv = SelectRankedEnv("TILEXR_CCU_DIRECT_LOCAL_ENDPOINT_DOORBELL_VA", rank);
    const char* tokenIdEnv = SelectRankedEnv("TILEXR_CCU_DIRECT_LOCAL_ENDPOINT_DOORBELL_TOKEN_ID", rank);
    const char* tokenValueEnv = SelectRankedEnv("TILEXR_CCU_DIRECT_LOCAL_ENDPOINT_DOORBELL_TOKEN_VALUE", rank);
    const char* sqDepthEnv = SelectRankedEnv("TILEXR_CCU_DIRECT_LOCAL_ENDPOINT_SQ_DEPTH", rank);

    if (eidEnv == nullptr && tpnEnv == nullptr && doorbellVaEnv == nullptr &&
        tokenIdEnv == nullptr && tokenValueEnv == nullptr && sqDepthEnv == nullptr) {
        return Fail(report, "direct CCU local endpoint route collector is not configured", TILEXR_ERROR_NOT_FOUND);
    }
    if (eidEnv == nullptr || tpnEnv == nullptr || doorbellVaEnv == nullptr ||
        tokenIdEnv == nullptr || sqDepthEnv == nullptr) {
        return Fail(report, "direct CCU local endpoint route env is incomplete", TILEXR_ERROR_PARA_CHECK_FAIL);
    }

    TileXRCcuLowerLayerTransportRoute parsed;
    if (!ParseEndpointEid(eidEnv, &parsed.remoteEid)) {
        return Fail(report, "invalid direct CCU local endpoint EID env", TILEXR_ERROR_PARA_CHECK_FAIL);
    }
    uint64_t value = 0;
    if (!ParseUnsignedEnv(tpnEnv, &value) || value == 0 || value > 0xffffffffULL) {
        return Fail(report, "invalid direct CCU local endpoint TPN env", TILEXR_ERROR_PARA_CHECK_FAIL);
    }
    parsed.tpn = static_cast<uint32_t>(value);
    if (!ParseUnsignedEnv(doorbellVaEnv, &parsed.doorbellVa)) {
        return Fail(report, "invalid direct CCU local endpoint doorbell VA env", TILEXR_ERROR_PARA_CHECK_FAIL);
    }
    if (!ParseUnsignedEnv(tokenIdEnv, &value) || value == 0 || value > 0xffffffffULL) {
        return Fail(report, "invalid direct CCU local endpoint doorbell token id env", TILEXR_ERROR_PARA_CHECK_FAIL);
    }
    parsed.doorbellTokenId = static_cast<uint32_t>(value);
    if (tokenValueEnv != nullptr) {
        if (!ParseUnsignedEnv(tokenValueEnv, &value) || value > 0xffffffffULL) {
            return Fail(report, "invalid direct CCU local endpoint doorbell token value env",
                TILEXR_ERROR_PARA_CHECK_FAIL);
        }
        parsed.doorbellTokenValue = static_cast<uint32_t>(value);
    }
    if (!ParseUnsignedEnv(sqDepthEnv, &value) || value == 0 || value > 0xffffffffULL) {
        return Fail(report, "invalid direct CCU local endpoint SQ depth env", TILEXR_ERROR_PARA_CHECK_FAIL);
    }
    parsed.sqDepth = static_cast<uint32_t>(value);
    parsed.endpointRouteVerified = true;
    *route = parsed;
    return TILEXR_SUCCESS;
}

void FillProviderResourceWindow(
    const TileXRCcuLocalResourceWindowInfo& localResourceWindow,
    TileXRCcuEndpointRouteProviderResourceWindow* providerWindow)
{
    if (providerWindow == nullptr) {
        return;
    }
    providerWindow->addr = localResourceWindow.addr;
    providerWindow->bytes = localResourceWindow.bytes;
    providerWindow->tokenId = localResourceWindow.tokenId;
    providerWindow->rawTokenId = localResourceWindow.rawTokenId;
    providerWindow->tokenValue = localResourceWindow.tokenValue;
}

void CopyProviderRoute(
    const TileXRCcuEndpointRouteProviderRoute& providerRoute,
    TileXRCcuLowerLayerTransportRoute* route)
{
    if (route == nullptr) {
        return;
    }
    TileXRCcuLowerLayerTransportRoute copied;
    for (uint32_t i = 0; i < TILEXR_CCU_EID_BYTES; ++i) {
        copied.remoteEid[i] = providerRoute.remoteEid[i];
    }
    copied.tpn = providerRoute.tpn;
    copied.doorbellVa = providerRoute.doorbellVa;
    copied.doorbellTokenId = providerRoute.doorbellTokenId;
    copied.doorbellTokenValue = providerRoute.doorbellTokenValue;
    copied.sqDepth = providerRoute.sqDepth;
    copied.endpointRouteVerified = providerRoute.endpointRouteVerified;
    *route = copied;
}

} // namespace

int TileXRCcuDirectRuntime::Init(
    const TileXRCcuDirectRuntimeOptions& options,
    TileXRCcuDirectRuntimeReport* report)
{
    ResetReport(report);
    Shutdown();
    options_ = options;

    TileXRCcuHccpLoaderReport loaderReport;
    int ret = loader_.Load(&loaderReport);
    if (ret != TILEXR_SUCCESS) {
        return Fail(report, loaderReport.message);
    }

    ret = ResolveDevicePhyId(&devicePhyId_, report);
    if (ret != TILEXR_SUCCESS) {
        Shutdown();
        return ret;
    }
    const int hdcType = SelectDirectCcuHdcType();
    if (report != nullptr) {
        report->hdcType = hdcType;
    }
    TileXRCcuHccpLoaderReport raReport;
    ret = loader_.InitRaHdc(devicePhyId_, hdcType, true, &raReport);
    if (ret != TILEXR_SUCCESS) {
        Shutdown();
        return Fail(report, raReport.message, ret);
    }
    if (report != nullptr) {
        report->raInitialized = true;
    }

    TileXRCcuHccpLoaderReport tlvReport;
    ret = loader_.InitCcuTlv(devicePhyId_, &tlvReport);
    if (ret != TILEXR_SUCCESS) {
        Shutdown();
        return Fail(report, tlvReport.message, ret);
    }

    initialized_ = true;
    TileXRCcuDriverAdapter adapter;
    TileXRCcuDriverAdapterReport adapterReport;
    const uint8_t cleanupDieId = SelectDirectCcuCleanupDieId();
    int cleanupRet = CreateDriverAdapter(&adapter, &adapterReport);
    if (cleanupRet == TILEXR_SUCCESS && RecoverTaskKillState()) {
        cleanupRet = adapter.SetTaskKill(cleanupDieId, &adapterReport);
        TraceTaskKillStep("Set", cleanupDieId, cleanupRet, adapterReport);
        // hcomm treats SET_TASKKILL as a best-effort trigger and gates recovery
        // on the following CLEAN_TASKKILL_STATE result.
        cleanupRet = TILEXR_SUCCESS;
    }
    if (cleanupRet == TILEXR_SUCCESS) {
        cleanupRet = adapter.CleanTaskKillState(cleanupDieId, &adapterReport);
    }
    TraceTaskKillStep("Cleanup", cleanupDieId, cleanupRet, adapterReport);
    if (RecoverTaskKillState() && cleanupRet != TILEXR_SUCCESS) {
        const std::string message = "failed to clean direct CCU task-kill state after explicit recovery: " +
            adapterReport.message;
        Shutdown();
        return Fail(report, message, cleanupRet);
    }
    if (report != nullptr) {
        report->initialized = true;
        report->raInitialized = true;
        report->ccuTlvInitialized = true;
        report->logicDevId = static_cast<uint32_t>(options_.devId);
        report->devicePhyId = devicePhyId_;
        report->hdcType = hdcType;
        report->message = "ok";
    }
    return TILEXR_SUCCESS;
}

void TileXRCcuDirectRuntime::Shutdown()
{
    ReleaseLocalEndpointRoute();
    ReleaseRegisteredResourceWindow();
    loader_.Unload();
    cachedBasicInfo_ = TileXRCcuBasicInfo{};
    localResourceWindow_ = TileXRCcuLocalResourceWindowInfo{};
    localVerifiedEndpointRoute_ = TileXRCcuLowerLayerTransportRoute{};
    cachedBasicInfoValid_ = false;
    resourceWindowRegistered_ = false;
    localVerifiedEndpointRouteValid_ = false;
    endpointChanHandle_ = nullptr;
    endpointCqHandle_ = nullptr;
    endpointQpHandle_ = nullptr;
    endpointRemoteQpHandle_ = nullptr;
    endpointPeerRemoteQpHandles_.clear();
    importedRemoteMemoryBuffers_.clear();
    endpointQpKey_ = TileXRCcuHccpQpKey{};
    endpointQpKeyValid_ = false;
    endpointRouteBound_ = false;
    endpointPsn_ = 1;
    devicePhyId_ = 0;
    initialized_ = false;
}

bool TileXRCcuDirectRuntime::IsAvailable() const
{
    return initialized_ && loader_.IsLoaded() && loader_.RaCustomChannel != nullptr;
}

int TileXRCcuDirectRuntime::ResolveDevicePhyId(uint32_t* devicePhyId, TileXRCcuDirectRuntimeReport* report) const
{
    if (devicePhyId == nullptr) {
        return Fail(report, "missing output CCU physical device id", TILEXR_ERROR_PARA_CHECK_FAIL);
    }
    TileXRCcuHccpLoaderReport loaderReport;
    const int ret = loader_.ResolveDevicePhyId(
        static_cast<uint32_t>(options_.devId),
        devicePhyId,
        &loaderReport);
    if (ret != TILEXR_SUCCESS) {
        return Fail(report, loaderReport.message.empty() ?
            "failed to resolve CCU physical device id" : loaderReport.message, ret);
    }
    if (report != nullptr) {
        report->logicDevId = static_cast<uint32_t>(options_.devId);
        report->devicePhyId = *devicePhyId;
    }
    return TILEXR_SUCCESS;
}

int TileXRCcuDirectRuntime::CreateDriverAdapter(
    TileXRCcuDriverAdapter* adapter,
    TileXRCcuDriverAdapterReport* report)
{
    if (!IsAvailable()) {
        if (report != nullptr) {
            *report = TileXRCcuDriverAdapterReport{};
            report->message = "direct CCU runtime is unavailable";
        }
        return TILEXR_ERROR_NOT_FOUND;
    }

    TileXRCcuRaCustomChannelProviderReport providerReport;
    int ret = raCustomChannelProvider_.Init(devicePhyId_, loader_.RaCustomChannel, &providerReport);
    if (ret != TILEXR_SUCCESS) {
        if (report != nullptr) {
            *report = TileXRCcuDriverAdapterReport{};
            report->message = providerReport.message;
        }
        return ret;
    }
    return raCustomChannelProvider_.CreateAdapter(adapter, report);
}

int TileXRCcuDirectRuntime::QueryBasicInfo(
    uint8_t dieId,
    TileXRCcuBasicInfo* basicInfo,
    TileXRCcuDriverAdapterReport* report)
{
    if (basicInfo == nullptr) {
        if (report != nullptr) {
            *report = TileXRCcuDriverAdapterReport{};
            report->message = "missing output CCU basic info";
        }
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }

    TileXRCcuDriverAdapter adapter;
    int ret = CreateDriverAdapter(&adapter, report);
    if (ret != TILEXR_SUCCESS) {
        return ret;
    }
    bool enabled = false;
    ret = adapter.GetDieEnabled(dieId, &enabled, report);
    if (ret != TILEXR_SUCCESS) {
        return ret;
    }
    if (!enabled) {
        if (report != nullptr) {
            report->message = "direct CCU die is not enabled";
        }
        return TILEXR_ERROR_NOT_FOUND;
    }
    ret = adapter.GetBasicInfo(dieId, basicInfo, report);
    if (ret == TILEXR_SUCCESS) {
        cachedBasicInfo_ = *basicInfo;
        cachedBasicInfoValid_ = true;
    }
    return ret;
}

int TileXRCcuDirectRuntime::RegisterCcuResourceRmaBuffer(uint64_t resourceAddr)
{
    if (!IsAvailable()) {
        return TILEXR_ERROR_NOT_INITIALIZED;
    }
    if (resourceWindowRegistered_ &&
        localResourceWindow_.addr == resourceAddr &&
        localResourceWindow_.raCtxRegistered == IsRaCtxResourceWindowRegistrationMode()) {
        return TILEXR_SUCCESS;
    }
    ReleaseRegisteredResourceWindow();
    if (!cachedBasicInfoValid_ || resourceAddr == 0) {
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    if (cachedBasicInfo_.resourceAddr != 0 && cachedBasicInfo_.resourceAddr != resourceAddr) {
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }

    const uint64_t resourceBytes = SelectResourceWindowBytes(cachedBasicInfo_);
    if (IsRaCtxResourceWindowRegistrationMode()) {
        return RegisterCcuResourceRmaBufferWithRaCtx(resourceAddr, resourceBytes);
    }

    localResourceWindow_ = TileXRCcuLocalResourceWindowInfo{};
    localResourceWindow_.addr = resourceAddr;
    localResourceWindow_.bytes = resourceBytes;
    localResourceWindow_.tokenId = cachedBasicInfo_.msidToken.tokenId;
    localResourceWindow_.rawTokenId = cachedBasicInfo_.msidToken.tokenId;
    localResourceWindow_.tokenValue = cachedBasicInfo_.msidToken.tokenValue;
    if (HasRankedEnv(TILEXR_CCU_DIRECT_RESOURCE_WINDOW_TOKEN_ID_ENV, options_.rank) &&
        !ApplyResourceWindowTokenOverride(options_.rank, &localResourceWindow_)) {
        localResourceWindow_ = TileXRCcuLocalResourceWindowInfo{};
        resourceWindowRegistered_ = false;
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    resourceWindowRegistered_ = true;
    return TILEXR_SUCCESS;
}

int TileXRCcuDirectRuntime::RegisterMemoryBuffer(
    uint64_t addr,
    uint64_t bytes,
    TileXRCcuRegisteredMemoryBufferInfo* info)
{
    if (info == nullptr) {
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    *info = TileXRCcuRegisteredMemoryBufferInfo {};
    if (!IsAvailable() || !resourceWindowRegistered_ || localResourceWindow_.raCtxHandle == nullptr) {
        return TILEXR_ERROR_NOT_INITIALIZED;
    }
    if (addr == 0 || bytes == 0 || !HasRaCtxResourceWindowSymbols(loader_)) {
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }

    void* tokenIdHandle = nullptr;
    void* lmemHandle = nullptr;
    TileXRCcuHccpTokenId allocatedToken {};
    int ret = loader_.RaCtxTokenIdAlloc(localResourceWindow_.raCtxHandle, &allocatedToken, &tokenIdHandle);
    if (ret != 0 || tokenIdHandle == nullptr) {
        return TILEXR_ERROR_MKIRT;
    }

    uint32_t tokenValue = 0;
    TileXRCcuRaInfo randomInfo {};
    randomInfo.mode = TILEXR_CCU_NETWORK_OFFLINE;
    randomInfo.phyId = devicePhyId_;
    ret = loader_.RaGetSecRandom(&randomInfo, &tokenValue);
    if (ret != 0) {
        if (loader_.RaCtxTokenIdFree != nullptr) {
            (void)loader_.RaCtxTokenIdFree(localResourceWindow_.raCtxHandle, tokenIdHandle);
        }
        return TILEXR_ERROR_MKIRT;
    }

    const uint64_t alignedAddr = AlignResourceWindowAddr(addr);
    const uint64_t alignedBytes = bytes + (addr - alignedAddr);
    TileXRCcuHccpMrRegInfo mr {};
    mr.in.mem.addr = alignedAddr;
    mr.in.mem.size = alignedBytes;
    mr.in.ub.flags.value = 0;
    mr.in.ub.flags.bs.tokenPolicy = TILEXR_CCU_HCCP_TOKEN_POLICY_PLAIN_TEXT;
    mr.in.ub.flags.bs.tokenIdValid = 1;
    mr.in.ub.flags.bs.access = TILEXR_CCU_HCCP_MEM_SEG_ACCESS_DEFAULT;
    mr.in.ub.flags.bs.nonPin = 1;
    mr.in.ub.tokenValue = tokenValue;
    mr.in.ub.tokenIdHandle = tokenIdHandle;

    ret = loader_.RaCtxLmemRegister(localResourceWindow_.raCtxHandle, &mr, &lmemHandle);
    if (ret != 0 || lmemHandle == nullptr) {
        if (lmemHandle != nullptr && loader_.RaCtxLmemUnregister != nullptr) {
            (void)loader_.RaCtxLmemUnregister(localResourceWindow_.raCtxHandle, lmemHandle);
        }
        if (loader_.RaCtxTokenIdFree != nullptr) {
            (void)loader_.RaCtxTokenIdFree(localResourceWindow_.raCtxHandle, tokenIdHandle);
        }
        return TILEXR_ERROR_MKIRT;
    }

    TileXRCcuRegisteredMemoryBufferInfo registeredInfo {};
    registeredInfo.addr = addr;
    registeredInfo.bytes = bytes;
    registeredInfo.alignedAddr = alignedAddr;
    registeredInfo.alignedBytes = alignedBytes;
    registeredInfo.targetSegVa = mr.out.ub.targetSegHandle + (addr - alignedAddr);
    registeredInfo.rawTokenId = mr.out.ub.tokenId != 0 ? mr.out.ub.tokenId : allocatedToken.tokenId;
    registeredInfo.tokenId = registeredInfo.rawTokenId >> TILEXR_CCU_URMA_TOKEN_ID_RIGHT_SHIFT;
    registeredInfo.tokenValue = tokenValue;
    registeredInfo.key = mr.out.key;
    registeredInfo.tokenIdHandle = tokenIdHandle;
    registeredInfo.lmemHandle = lmemHandle;
    registeredInfo.valid = true;

    registeredMemoryBuffers_.push_back(registeredInfo);
    *info = registeredInfo;
    return TILEXR_SUCCESS;
}

int TileXRCcuDirectRuntime::ImportRemoteMemoryBuffer(
    const TileXRCcuRemoteMemoryBufferImportRequest& request,
    TileXRCcuImportedRemoteMemoryBufferInfo* info)
{
    if (info == nullptr) {
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    *info = TileXRCcuImportedRemoteMemoryBufferInfo {};
    if (!IsAvailable() || !resourceWindowRegistered_ || localResourceWindow_.raCtxHandle == nullptr) {
        return TILEXR_ERROR_NOT_INITIALIZED;
    }
    if (!request.valid || request.key.size == 0 || !HasRaCtxRemoteMemoryImportSymbols(loader_)) {
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }

    TileXRCcuHccpMrImportInfo mr {};
    mr.in.key = request.key;
    mr.in.ub.tokenValue = request.tokenValue;
    mr.in.ub.mappingAddr = 0;
    mr.in.ub.flags.value = 0;
    mr.in.ub.flags.bs.access = TILEXR_CCU_HCCP_MEM_SEG_ACCESS_DEFAULT;

    void* rmemHandle = nullptr;
    const int ret = loader_.RaCtxRmemImport(localResourceWindow_.raCtxHandle, &mr, &rmemHandle);
    if (ret != 0 || rmemHandle == nullptr || mr.out.ub.targetSegHandle == 0) {
        if (rmemHandle != nullptr && loader_.RaCtxRmemUnimport != nullptr) {
            (void)loader_.RaCtxRmemUnimport(localResourceWindow_.raCtxHandle, rmemHandle);
        }
        return TILEXR_ERROR_MKIRT;
    }

    TileXRCcuImportedRemoteMemoryBufferInfo imported {};
    imported.addr = request.addr;
    imported.bytes = request.bytes;
    imported.targetSegVa = mr.out.ub.targetSegHandle + request.offset;
    imported.rmemHandle = rmemHandle;
    imported.valid = true;
    importedRemoteMemoryBuffers_.push_back(imported);
    *info = imported;
    return TILEXR_SUCCESS;
}

int TileXRCcuDirectRuntime::RegisterCcuResourceRmaBufferWithRaCtx(
    uint64_t resourceAddr,
    uint64_t resourceBytes)
{
    if (!HasRaCtxResourceWindowSymbols(loader_)) {
        return TILEXR_ERROR_NOT_FOUND;
    }

    TileXRCcuRaInfo raInfo {};
    raInfo.mode = TILEXR_CCU_NETWORK_OFFLINE;
    raInfo.phyId = devicePhyId_;

    uint32_t eidNum = 0;
    int ret = loader_.RaGetDevEidInfoNum(raInfo, &eidNum);
    if (ret != 0 || eidNum == 0) {
        return ret == 0 ? TILEXR_ERROR_NOT_FOUND : TILEXR_ERROR_MKIRT;
    }

    std::vector<TileXRCcuHccpDevEidInfo> eidInfos(eidNum);
    uint32_t queriedEidNum = eidNum;
    ret = loader_.RaGetDevEidInfoList(raInfo, eidInfos.data(), &queriedEidNum);
    if (ret != 0 || queriedEidNum == 0) {
        return ret == 0 ? TILEXR_ERROR_NOT_FOUND : TILEXR_ERROR_MKIRT;
    }
    eidInfos.resize(queriedEidNum);

    void* ctxHandle = nullptr;
    void* tokenIdHandle = nullptr;
    void* lmemHandle = nullptr;

    std::vector<TileXRCcuHccpDevEidInfo> eidCandidates;
    if (!BuildRaCtxResourceWindowEidCandidates(
            options_.rank,
            SelectDirectCcuCleanupDieId(),
            eidInfos,
            &eidCandidates)) {
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }

    TileXRCcuHccpCtxInitCfg ctxCfg {};
    ctxCfg.mode = TILEXR_CCU_NETWORK_OFFLINE;
    ctxCfg.rdma.disabledLiteThread = false;
    TileXRCcuHccpDevEidInfo selectedEid {};
    const bool explicitEid =
        HasRankedEnv(TILEXR_CCU_DIRECT_RESOURCE_WINDOW_EID_ENV, options_.rank) ||
        HasRankedEnv(TILEXR_CCU_DIRECT_RESOURCE_WINDOW_EID_INDEX_ENV, options_.rank);
    const bool canProbeLoopTp = !explicitEid &&
        loader_.RaGetTpInfoListAsync != nullptr && loader_.RaGetAsyncReqResult != nullptr;
    for (const auto& candidate : eidCandidates) {
        TileXRCcuHccpCtxInitAttr ctxAttr {};
        ctxAttr.phyId = devicePhyId_;
        ctxAttr.ub.eidIndex = candidate.eidIndex;
        ctxAttr.ub.eid = candidate.eid;
        ret = loader_.RaCtxInit(&ctxCfg, &ctxAttr, &ctxHandle);
        if (ret != 0 || ctxHandle == nullptr) {
            ctxHandle = nullptr;
            continue;
        }
        uint64_t loopTpHandle = 0;
        const auto candidateEid = CopyRawEid(candidate.eid);
        const int probeRet = canProbeLoopTp ?
            QueryTpHandleForPeer(ctxHandle, candidateEid, candidateEid, &loopTpHandle) :
            TILEXR_SUCCESS;
        if (TraceEndpointRoute()) {
            std::cerr << "TileXRDirectCcuTrace endpointRoute loopEidCandidate"
                      << " eidIndex=" << candidate.eidIndex
                      << " dieId=" << candidate.dieId
                      << " funcId=" << candidate.funcId
                      << " eid=" << FormatEndpointEid(candidateEid)
                      << " probeRet=" << probeRet
                      << " tpHandle=0x" << std::hex << loopTpHandle << std::dec
                      << std::endl;
        }
        if (probeRet == TILEXR_SUCCESS) {
            selectedEid = candidate;
            break;
        }
        (void)loader_.RaCtxDeinit(ctxHandle);
        ctxHandle = nullptr;
    }
    if (ctxHandle == nullptr) {
        return TILEXR_ERROR_NOT_FOUND;
    }

    TileXRCcuHccpTokenId allocatedToken {};
    ret = loader_.RaCtxTokenIdAlloc(ctxHandle, &allocatedToken, &tokenIdHandle);
    if (ret != 0 || tokenIdHandle == nullptr) {
        if (loader_.RaCtxDeinit != nullptr) {
            (void)loader_.RaCtxDeinit(ctxHandle);
        }
        return TILEXR_ERROR_MKIRT;
    }

    uint32_t tokenValue = 0;
    TileXRCcuRaInfo randomInfo {};
    randomInfo.mode = TILEXR_CCU_NETWORK_OFFLINE;
    randomInfo.phyId = devicePhyId_;
    ret = loader_.RaGetSecRandom(&randomInfo, &tokenValue);
    if (ret != 0) {
        if (loader_.RaCtxTokenIdFree != nullptr) {
            (void)loader_.RaCtxTokenIdFree(ctxHandle, tokenIdHandle);
        }
        if (loader_.RaCtxDeinit != nullptr) {
            (void)loader_.RaCtxDeinit(ctxHandle);
        }
        return TILEXR_ERROR_MKIRT;
    }

    const uint64_t alignedAddr = AlignResourceWindowAddr(resourceAddr);
    const uint64_t alignedBytes = resourceBytes + (resourceAddr - alignedAddr);
    TileXRCcuHccpMrRegInfo mr {};
    mr.in.mem.addr = alignedAddr;
    mr.in.mem.size = alignedBytes;
    mr.in.ub.flags.value = 0;
    mr.in.ub.flags.bs.tokenPolicy = TILEXR_CCU_HCCP_TOKEN_POLICY_PLAIN_TEXT;
    mr.in.ub.flags.bs.tokenIdValid = 1;
    mr.in.ub.flags.bs.access = TILEXR_CCU_HCCP_MEM_SEG_ACCESS_DEFAULT;
    mr.in.ub.flags.bs.nonPin = 1;
    mr.in.ub.tokenValue = tokenValue;
    mr.in.ub.tokenIdHandle = tokenIdHandle;

    ret = loader_.RaCtxLmemRegister(ctxHandle, &mr, &lmemHandle);
    if (ret != 0 || lmemHandle == nullptr) {
        if (lmemHandle != nullptr && loader_.RaCtxLmemUnregister != nullptr) {
            (void)loader_.RaCtxLmemUnregister(ctxHandle, lmemHandle);
        }
        if (loader_.RaCtxTokenIdFree != nullptr) {
            (void)loader_.RaCtxTokenIdFree(ctxHandle, tokenIdHandle);
        }
        if (loader_.RaCtxDeinit != nullptr) {
            (void)loader_.RaCtxDeinit(ctxHandle);
        }
        return TILEXR_ERROR_MKIRT;
    }

    const uint32_t rawTokenId = mr.out.ub.tokenId != 0 ? mr.out.ub.tokenId : allocatedToken.tokenId;
    localResourceWindow_ = TileXRCcuLocalResourceWindowInfo{};
    localResourceWindow_.addr = resourceAddr;
    localResourceWindow_.bytes = resourceBytes;
    localResourceWindow_.rawTokenId = rawTokenId;
    localResourceWindow_.tokenId = rawTokenId >> TILEXR_CCU_URMA_TOKEN_ID_RIGHT_SHIFT;
    localResourceWindow_.tokenValue = tokenValue;
    localResourceWindow_.targetSegHandle = mr.out.ub.targetSegHandle;
    localResourceWindow_.raCtxHandle = ctxHandle;
    localResourceWindow_.tokenIdHandle = tokenIdHandle;
    localResourceWindow_.lmemHandle = lmemHandle;
    for (uint32_t i = 0; i < TILEXR_CCU_EID_BYTES; ++i) {
        localResourceWindow_.eid[i] = selectedEid.eid.raw[i];
    }
    localResourceWindow_.eidIndex = selectedEid.eidIndex;
    localResourceWindow_.funcId = selectedEid.funcId;
    localResourceWindow_.funcIdValid = true;
    localResourceWindow_.raCtxRegistered = true;
    resourceWindowRegistered_ = true;
    return TILEXR_SUCCESS;
}

void TileXRCcuDirectRuntime::ReleaseRegisteredResourceWindow()
{
    ReleaseImportedRemoteMemoryBuffers();
    ReleaseRegisteredMemoryBuffers();
    ReleaseLocalEndpointRoute();
    if (localResourceWindow_.raCtxRegistered) {
        if (localResourceWindow_.lmemHandle != nullptr &&
            localResourceWindow_.raCtxHandle != nullptr &&
            loader_.RaCtxLmemUnregister != nullptr) {
            (void)loader_.RaCtxLmemUnregister(localResourceWindow_.raCtxHandle, localResourceWindow_.lmemHandle);
        }
        if (localResourceWindow_.tokenIdHandle != nullptr &&
            localResourceWindow_.raCtxHandle != nullptr &&
            loader_.RaCtxTokenIdFree != nullptr) {
            (void)loader_.RaCtxTokenIdFree(localResourceWindow_.raCtxHandle, localResourceWindow_.tokenIdHandle);
        }
        if (localResourceWindow_.raCtxHandle != nullptr && loader_.RaCtxDeinit != nullptr) {
            (void)loader_.RaCtxDeinit(localResourceWindow_.raCtxHandle);
        }
    }
    localResourceWindow_ = TileXRCcuLocalResourceWindowInfo{};
    localVerifiedEndpointRoute_ = TileXRCcuLowerLayerTransportRoute{};
    resourceWindowRegistered_ = false;
    localVerifiedEndpointRouteValid_ = false;
}

void TileXRCcuDirectRuntime::ReleaseImportedRemoteMemoryBuffers()
{
    if (localResourceWindow_.raCtxHandle != nullptr && loader_.RaCtxRmemUnimport != nullptr) {
        for (auto it = importedRemoteMemoryBuffers_.rbegin(); it != importedRemoteMemoryBuffers_.rend(); ++it) {
            if (it->rmemHandle != nullptr) {
                (void)loader_.RaCtxRmemUnimport(localResourceWindow_.raCtxHandle, it->rmemHandle);
            }
        }
    }
    importedRemoteMemoryBuffers_.clear();
}

void TileXRCcuDirectRuntime::ReleaseRegisteredMemoryBuffers()
{
    if (localResourceWindow_.raCtxHandle != nullptr) {
        for (auto it = registeredMemoryBuffers_.rbegin(); it != registeredMemoryBuffers_.rend(); ++it) {
            if (it->lmemHandle != nullptr && loader_.RaCtxLmemUnregister != nullptr) {
                (void)loader_.RaCtxLmemUnregister(localResourceWindow_.raCtxHandle, it->lmemHandle);
            }
            if (it->tokenIdHandle != nullptr && loader_.RaCtxTokenIdFree != nullptr) {
                (void)loader_.RaCtxTokenIdFree(localResourceWindow_.raCtxHandle, it->tokenIdHandle);
            }
        }
    }
    registeredMemoryBuffers_.clear();
}

void TileXRCcuDirectRuntime::ReleasePeerEndpointImports()
{
    if (localResourceWindow_.raCtxHandle != nullptr && loader_.RaCtxQpUnimport != nullptr) {
        for (void* handle : endpointPeerRemoteQpHandles_) {
            if (handle != nullptr) {
                (void)loader_.RaCtxQpUnimport(localResourceWindow_.raCtxHandle, handle);
            }
        }
    }
    endpointPeerRemoteQpHandles_.clear();
}

void TileXRCcuDirectRuntime::ReleasePeerEndpointState(TileXRCcuPeerEndpointState* state)
{
    if (state == nullptr) {
        return;
    }
    if (state->remoteQpHandle != nullptr && state->resourceWindow.raCtxHandle != nullptr &&
        loader_.RaCtxQpUnimport != nullptr) {
        (void)loader_.RaCtxQpUnimport(state->resourceWindow.raCtxHandle, state->remoteQpHandle);
    }
    if (state->qpHandle != nullptr && loader_.RaCtxQpDestroy != nullptr) {
        (void)loader_.RaCtxQpDestroy(state->qpHandle);
    }
    if (state->cqHandle != nullptr && state->resourceWindow.raCtxHandle != nullptr &&
        loader_.RaCtxCqDestroy != nullptr) {
        (void)loader_.RaCtxCqDestroy(state->resourceWindow.raCtxHandle, state->cqHandle);
    }
    if (state->resourceWindow.lmemHandle != nullptr && state->resourceWindow.raCtxHandle != nullptr &&
        loader_.RaCtxLmemUnregister != nullptr) {
        (void)loader_.RaCtxLmemUnregister(
            state->resourceWindow.raCtxHandle,
            state->resourceWindow.lmemHandle);
    }
    if (state->resourceWindow.tokenIdHandle != nullptr && state->resourceWindow.raCtxHandle != nullptr &&
        loader_.RaCtxTokenIdFree != nullptr) {
        (void)loader_.RaCtxTokenIdFree(
            state->resourceWindow.raCtxHandle,
            state->resourceWindow.tokenIdHandle);
    }
    if (state->resourceWindow.raCtxHandle != nullptr && loader_.RaCtxDeinit != nullptr) {
        (void)loader_.RaCtxDeinit(state->resourceWindow.raCtxHandle);
    }
    *state = TileXRCcuPeerEndpointState {};
}

void TileXRCcuDirectRuntime::ReleasePeerEndpointRoutes()
{
    for (auto it = peerEndpointStates_.rbegin(); it != peerEndpointStates_.rend(); ++it) {
        ReleasePeerEndpointState(&*it);
    }
    peerEndpointStates_.clear();
}

void TileXRCcuDirectRuntime::ReleaseLocalEndpointRoute()
{
    ReleasePeerEndpointRoutes();
    ReleasePeerEndpointImports();
    if (endpointRouteBound_ && endpointQpHandle_ != nullptr && loader_.RaCtxQpUnbind != nullptr) {
        (void)loader_.RaCtxQpUnbind(endpointQpHandle_);
    }
    if (endpointRemoteQpHandle_ != nullptr &&
        localResourceWindow_.raCtxHandle != nullptr &&
        loader_.RaCtxQpUnimport != nullptr) {
        (void)loader_.RaCtxQpUnimport(localResourceWindow_.raCtxHandle, endpointRemoteQpHandle_);
    }
    if (endpointQpHandle_ != nullptr && loader_.RaCtxQpDestroy != nullptr) {
        (void)loader_.RaCtxQpDestroy(endpointQpHandle_);
    }
    if (endpointCqHandle_ != nullptr &&
        localResourceWindow_.raCtxHandle != nullptr &&
        loader_.RaCtxCqDestroy != nullptr) {
        (void)loader_.RaCtxCqDestroy(localResourceWindow_.raCtxHandle, endpointCqHandle_);
    }
    if (endpointChanHandle_ != nullptr &&
        localResourceWindow_.raCtxHandle != nullptr &&
        loader_.RaCtxChanDestroy != nullptr) {
        (void)loader_.RaCtxChanDestroy(localResourceWindow_.raCtxHandle, endpointChanHandle_);
    }
    endpointChanHandle_ = nullptr;
    endpointCqHandle_ = nullptr;
    endpointQpHandle_ = nullptr;
    endpointRemoteQpHandle_ = nullptr;
    endpointQpKey_ = TileXRCcuHccpQpKey{};
    endpointQpKeyValid_ = false;
    endpointRouteBound_ = false;
    localVerifiedEndpointRoute_ = TileXRCcuLowerLayerTransportRoute{};
    localVerifiedEndpointRouteValid_ = false;
}

int TileXRCcuDirectRuntime::CollectLocalEndpointRouteWithRaCtx(TileXRCcuLowerLayerTransportRoute* route)
{
    if (route == nullptr) {
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    *route = TileXRCcuLowerLayerTransportRoute{};
    int lastRet = TILEXR_ERROR_NOT_FOUND;
    for (uint32_t attempt = 0; attempt < TILEXR_CCU_DIRECT_ENDPOINT_ROUTE_MAX_ATTEMPTS; ++attempt) {
        TileXRCcuLowerLayerTransportRoute attemptRoute;
        bool asyncWaitFailed = false;
        const int ret = CollectLocalEndpointRouteWithRaCtxOnce(&attemptRoute, &asyncWaitFailed);
        if (ret == TILEXR_SUCCESS) {
            *route = attemptRoute;
            return TILEXR_SUCCESS;
        }
        lastRet = ret;
        if (!asyncWaitFailed || attempt + 1 >= TILEXR_CCU_DIRECT_ENDPOINT_ROUTE_MAX_ATTEMPTS) {
            return ret;
        }
        if (TraceEndpointRoute()) {
            std::cerr << "TileXRDirectCcuTrace endpointRoute retryAfterAsyncFailure"
                      << " attempt=" << (attempt + 1)
                      << " ret=" << ret << std::endl;
        }
    }
    return lastRet;
}

int TileXRCcuDirectRuntime::CollectLocalEndpointRouteWithRaCtxOnce(
    TileXRCcuLowerLayerTransportRoute* route,
    bool* asyncWaitFailed)
{
    if (route == nullptr) {
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    if (asyncWaitFailed != nullptr) {
        *asyncWaitFailed = false;
    }
    *route = TileXRCcuLowerLayerTransportRoute{};
    if (!localResourceWindow_.raCtxRegistered ||
        localResourceWindow_.raCtxHandle == nullptr ||
        localResourceWindow_.tokenIdHandle == nullptr ||
        IsEmptyEid(localResourceWindow_.eid) ||
        !HasRaCtxEndpointRouteSymbols(loader_)) {
        TraceEndpointRouteStep("raCtxLoop precheck failed");
        return TILEXR_ERROR_NOT_FOUND;
    }

    ReleaseLocalEndpointRoute();
    const uint32_t sqDepth = SelectEndpointRouteSqDepth();
    const uint64_t sqVa = SelectEndpointRouteSqVa(
        localResourceWindow_,
        TILEXR_CCU_DIRECT_LOOP_JETTY_CTX_ID);
    const uint32_t sqBytes = SelectEndpointRouteSqBytes(sqDepth);
    if (TraceEndpointRoute()) {
        std::cerr << "TileXRDirectCcuTrace endpointRoute begin"
                  << " ctx=" << localResourceWindow_.raCtxHandle
                  << " sqDepth=" << sqDepth
                  << " sqVa=0x" << std::hex << sqVa
                  << " sqBytes=0x" << sqBytes
                  << std::dec << std::endl;
    }

    TileXRCcuHccpCqInfo cqInfo {};
    cqInfo.in.chanHandle = nullptr;
    cqInfo.in.depth = TILEXR_CCU_DIRECT_CCU_POLL_CQ_DEPTH;
    cqInfo.in.ub.userCtx = 0;
    cqInfo.in.ub.mode = TILEXR_CCU_HCCP_JFC_MODE_CCU_POLL;
    cqInfo.in.ub.ceqn = 0;
    cqInfo.in.ub.flag.value = 0;
    int ret = loader_.RaCtxCqCreate(localResourceWindow_.raCtxHandle, &cqInfo, &endpointCqHandle_);
    if (TraceEndpointRoute()) {
        std::cerr << "TileXRDirectCcuTrace endpointRoute cqCreate ret=" << ret
                  << " cq=" << endpointCqHandle_ << std::endl;
    }
    if (ret != 0 || endpointCqHandle_ == nullptr) {
        ReleaseLocalEndpointRoute();
        return TILEXR_ERROR_MKIRT;
    }

    TileXRCcuHccpQpCreateAttr qpAttr {};
    qpAttr.scqHandle = endpointCqHandle_;
    qpAttr.rcqHandle = endpointCqHandle_;
    qpAttr.srqHandle = endpointCqHandle_;
    qpAttr.sqDepth = sqDepth;
    qpAttr.rqDepth = TILEXR_CCU_HCCP_RQ_DEPTH_DEFAULT;
    qpAttr.transportMode = TILEXR_CCU_HCCP_TRANSPORT_MODE_RM;
    qpAttr.ub.mode = static_cast<int>(TILEXR_CCU_HCCP_JETTY_MODE_CCU);
    qpAttr.ub.jettyId = TILEXR_CCU_DIRECT_LOOP_JETTY_ID;
    qpAttr.ub.tokenIdHandle = localResourceWindow_.tokenIdHandle;
    qpAttr.ub.tokenValue = localResourceWindow_.tokenValue;
    qpAttr.ub.flag.value = 0;
    qpAttr.ub.flag.bs.shareJfr = 1;
    qpAttr.ub.jfsFlag.bs.errorSuspend = 1;
    qpAttr.ub.priority = 2;
    qpAttr.ub.rnrRetry = TILEXR_CCU_HCCP_RNR_RETRY_DEFAULT;
    qpAttr.ub.errTimeout = TILEXR_CCU_DIRECT_ENDPOINT_ERR_TIMEOUT;
    qpAttr.ub.extMode.cstmFlag.value = 0;
    qpAttr.ub.extMode.cstmFlag.bs.sqCstm = 1;
    qpAttr.ub.extMode.sq.buffVa = sqVa;
    qpAttr.ub.extMode.sq.buffSize = sqBytes;
    qpAttr.ub.extMode.sqebbNum = sqDepth;

    TileXRCcuHccpQpCreateInfo qpInfo {};
    ret = loader_.RaCtxQpCreate(localResourceWindow_.raCtxHandle, &qpAttr, &qpInfo, &endpointQpHandle_);
    if (TraceEndpointRoute()) {
        std::cerr << "TileXRDirectCcuTrace endpointRoute qpCreate ret=" << ret
                  << " qp=" << endpointQpHandle_
                  << " keySize=" << static_cast<uint32_t>(qpInfo.key.size)
                  << " id=" << qpInfo.ub.id
                  << " dbAddr=0x" << std::hex << qpInfo.ub.dbAddr
                  << " dbTokenId=0x" << qpInfo.ub.dbTokenId
                  << std::dec << std::endl;
    }
    if (ret != 0 || endpointQpHandle_ == nullptr) {
        ReleaseLocalEndpointRoute();
        return TILEXR_ERROR_MKIRT;
    }
    endpointQpKey_ = qpInfo.key;
    endpointQpKeyValid_ = qpInfo.key.size != 0;

    TileXRCcuHccpGetTpCfg tpCfg {};
    tpCfg.flag.value = 0;
    tpCfg.flag.bs.rtp = 1;
    tpCfg.transMode = TILEXR_CCU_HCCP_TRANSPORT_MODE_RM;
    for (uint32_t i = 0; i < TILEXR_CCU_EID_BYTES; ++i) {
        tpCfg.localEid.raw[i] = localResourceWindow_.eid[i];
        tpCfg.peerEid.raw[i] = localResourceWindow_.eid[i];
    }
    TileXRCcuHccpTpInfo tpInfo {};
    uint32_t tpInfoNum = 1;
    void* reqHandle = nullptr;
    ret = loader_.RaGetTpInfoListAsync(
        localResourceWindow_.raCtxHandle,
        &tpCfg,
        &tpInfo,
        &tpInfoNum,
        &reqHandle);
    if (TraceEndpointRoute()) {
        std::cerr << "TileXRDirectCcuTrace endpointRoute getTpInfoAsync ret=" << ret
                  << " req=" << reqHandle
                  << " num=" << tpInfoNum
                  << " tpHandle=0x" << std::hex << tpInfo.tpHandle
                  << std::dec << std::endl;
    }
    if (ret != 0 || reqHandle == nullptr || tpInfoNum == 0) {
        ReleaseLocalEndpointRoute();
        return TILEXR_ERROR_MKIRT;
    }
    ret = WaitRaCtxAsyncRequest(loader_, reqHandle);
    if (ret != TILEXR_SUCCESS) {
        ReleaseLocalEndpointRoute();
        if (asyncWaitFailed != nullptr) {
            *asyncWaitFailed = true;
        }
        return ret;
    }
    if (TraceEndpointRoute()) {
        std::cerr << "TileXRDirectCcuTrace endpointRoute getTpInfoDone"
                  << " tpHandle=0x" << std::hex << tpInfo.tpHandle
                  << std::dec << std::endl;
    }
    if (tpInfo.tpHandle == 0) {
        ReleaseLocalEndpointRoute();
        return TILEXR_ERROR_NOT_FOUND;
    }

    TileXRCcuHccpQpImportInfo importInfo {};
    importInfo.in.key = qpInfo.key;
    importInfo.in.ub.mode = TILEXR_CCU_HCCP_JETTY_IMPORT_MODE_EXP;
    importInfo.in.ub.tokenValue = localResourceWindow_.tokenValue;
    importInfo.in.ub.policy = TILEXR_CCU_HCCP_JETTY_GRP_POLICY_RR;
    importInfo.in.ub.type = TILEXR_CCU_HCCP_TARGET_TYPE_JETTY;
    importInfo.in.ub.flag.value = 0;
    importInfo.in.ub.flag.bs.tokenPolicy = TILEXR_CCU_HCCP_TOKEN_POLICY_PLAIN_TEXT;
    importInfo.in.ub.expImportCfg.tpHandle = tpInfo.tpHandle;
    importInfo.in.ub.expImportCfg.peerTpHandle = tpInfo.tpHandle;
    importInfo.in.ub.expImportCfg.txPsn = endpointPsn_;
    importInfo.in.ub.expImportCfg.rxPsn = endpointPsn_;
    importInfo.in.ub.tpType = TILEXR_CCU_HCCP_TP_TYPE_RTP;

    ret = loader_.RaCtxQpImport(localResourceWindow_.raCtxHandle, &importInfo, &endpointRemoteQpHandle_);
    if (TraceEndpointRoute()) {
        std::cerr << "TileXRDirectCcuTrace endpointRoute qpImport ret=" << ret
                  << " remoteQp=" << endpointRemoteQpHandle_
                  << " tpn=0x" << std::hex << importInfo.out.ub.tpn
                  << std::dec << std::endl;
    }
    if (ret != 0 || endpointRemoteQpHandle_ == nullptr) {
        ReleaseLocalEndpointRoute();
        return TILEXR_ERROR_MKIRT;
    }
    ++endpointPsn_;

    TileXRCcuLowerLayerTransportRoute collected;
    collected.remoteEid = localResourceWindow_.eid;
    collected.tpn = importInfo.out.ub.tpn;
    collected.doorbellVa = qpInfo.ub.dbAddr;
    collected.doorbellTokenId = qpInfo.ub.dbTokenId >> TILEXR_CCU_URMA_TOKEN_ID_RIGHT_SHIFT;
    collected.doorbellTokenValue = localResourceWindow_.tokenValue;
    collected.sqDepth = sqDepth;
    collected.startJettyId = static_cast<uint16_t>(qpInfo.ub.id);
    collected.endpointRouteVerified = true;
    if (!HasCompleteEndpointRoute(collected)) {
        TraceEndpointRouteStep("collected route incomplete");
        ReleaseLocalEndpointRoute();
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    if (TraceEndpointRoute()) {
        std::cerr << "TileXRDirectCcuTrace endpointRoute verified"
                  << " tpn=0x" << std::hex << collected.tpn
                  << " doorbellVa=0x" << collected.doorbellVa
                  << " doorbellTokenId=0x" << collected.doorbellTokenId
                  << std::dec
                  << " startJettyId=" << collected.startJettyId
                  << " sqDepth=" << collected.sqDepth << std::endl;
    }
    *route = collected;
    return TILEXR_SUCCESS;
}

int TileXRCcuDirectRuntime::RefreshLocalVerifiedEndpointRoute(TileXRCcuDirectRuntimeReport* report)
{
    ResetReport(report);
    localVerifiedEndpointRoute_ = TileXRCcuLowerLayerTransportRoute{};
    localVerifiedEndpointRouteValid_ = false;
    if (!resourceWindowRegistered_) {
        return Fail(report, "direct CCU resource window is not registered for endpoint route collection",
            TILEXR_ERROR_NOT_INITIALIZED);
    }
    if (options_.rankSize > 2) {
        return PreparePeerEndpointRoutes(report);
    }

    TileXRCcuLowerLayerTransportRoute route;
    int ret = TILEXR_ERROR_NOT_FOUND;
    if (options_.localEndpointRouteCollector != nullptr) {
        ret = options_.localEndpointRouteCollector(
            devicePhyId_,
            localResourceWindow_,
            &route,
            options_.localEndpointRouteCollectorUserData);
        if (ret != TILEXR_SUCCESS) {
            return Fail(report, "direct CCU local endpoint route collector failed", ret);
        }
    } else if (IsRaCtxLoopEndpointRouteCollectionMode()) {
        ret = CollectLocalEndpointRouteWithRaCtx(&route);
        if (ret != TILEXR_SUCCESS) {
            return Fail(report, "direct CCU RA ctx loop endpoint route collection failed", ret);
        }
    } else {
        TileXRCcuHccpLoaderReport providerReport;
        ret = loader_.LoadEndpointRouteProviderFromEnv(&providerReport);
        if (ret == TILEXR_SUCCESS && loader_.CollectLocalEndpointRoute != nullptr) {
            TileXRCcuEndpointRouteProviderResourceWindow providerWindow;
            FillProviderResourceWindow(localResourceWindow_, &providerWindow);
            TileXRCcuEndpointRouteProviderRoute providerRoute;
            ret = loader_.CollectLocalEndpointRoute(devicePhyId_, &providerWindow, &providerRoute);
            if (ret != TILEXR_SUCCESS) {
                return Fail(report, "direct CCU endpoint route provider failed", ret);
            }
            CopyProviderRoute(providerRoute, &route);
        } else if (ret == TILEXR_ERROR_NOT_FOUND && !providerReport.endpointRouteProviderConfigured) {
            const bool canCollectRaCtxRoute =
                IsRaCtxResourceWindowRegistrationMode() &&
                localResourceWindow_.raCtxRegistered &&
                HasRaCtxEndpointRouteSymbols(loader_);
            if (canCollectRaCtxRoute) {
                ret = CollectLocalEndpointRouteWithRaCtx(&route);
                if (ret != TILEXR_SUCCESS && ret != TILEXR_ERROR_NOT_FOUND) {
                    return Fail(report, "direct CCU RA ctx loop endpoint route collection failed", ret);
                }
            }
            if (ret != TILEXR_SUCCESS) {
                ret = CollectLocalEndpointRouteFromEnv(options_.rank, &route, report);
                if (ret != TILEXR_SUCCESS) {
                    return ret;
                }
            }
        } else {
            return Fail(report, providerReport.message.empty() ?
                "direct CCU endpoint route provider failed to load" : providerReport.message, ret);
        }
    }

    ret = ConfigureLocalVerifiedEndpointRoute(route);
    if (ret != TILEXR_SUCCESS) {
        return Fail(report, "direct CCU local endpoint route collector returned incomplete route", ret);
    }
    if (report != nullptr) {
        report->initialized = initialized_;
        report->logicDevId = static_cast<uint32_t>(options_.devId);
        report->devicePhyId = devicePhyId_;
        report->message = "direct CCU local endpoint route collected";
    }
    return TILEXR_SUCCESS;
}

int TileXRCcuDirectRuntime::ConfigureLocalVerifiedEndpointRoute(
    const TileXRCcuLowerLayerTransportRoute& route)
{
    if (!route.endpointRouteVerified || !HasCompleteEndpointRoute(route)) {
        localVerifiedEndpointRoute_ = TileXRCcuLowerLayerTransportRoute{};
        localVerifiedEndpointRouteValid_ = false;
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    localVerifiedEndpointRoute_ = route;
    localVerifiedEndpointRouteValid_ = true;
    return TILEXR_SUCCESS;
}

int TileXRCcuDirectRuntime::ExportLocalCcuRmaBuffer(TileXRCcuLocalResourceWindowInfo* info) const
{
    if (info == nullptr) {
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    *info = TileXRCcuLocalResourceWindowInfo{};
    if (!resourceWindowRegistered_) {
        return TILEXR_ERROR_NOT_INITIALIZED;
    }
    *info = localResourceWindow_;
    return TILEXR_SUCCESS;
}

int TileXRCcuDirectRuntime::CreatePeerEndpointState(
    uint32_t peerRank,
    uint32_t peerDevicePhyId,
    const std::array<uint8_t, TILEXR_CCU_EID_BYTES>& localEid,
    const std::array<uint8_t, TILEXR_CCU_EID_BYTES>& peerEid,
    uint32_t tpType,
    uint32_t peerOrdinal,
    TileXRCcuPeerEndpointState* state)
{
    if (state == nullptr || !resourceWindowRegistered_ || localResourceWindow_.addr == 0 ||
        !HasRaCtxResourceWindowSymbols(loader_) || !HasRaCtxEndpointRouteSymbols(loader_)) {
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    *state = TileXRCcuPeerEndpointState {};
    state->peerRank = peerRank;
    state->peerDevicePhyId = peerDevicePhyId;
    state->tpType = tpType;
    if (tpType != TILEXR_CCU_HCCP_TP_TYPE_RTP && tpType != TILEXR_CCU_HCCP_TP_TYPE_CTP) {
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }

    TileXRCcuRaInfo raInfo {};
    raInfo.mode = TILEXR_CCU_NETWORK_OFFLINE;
    raInfo.phyId = devicePhyId_;
    uint32_t eidNum = 0;
    int ret = loader_.RaGetDevEidInfoNum(raInfo, &eidNum);
    if (ret != 0 || eidNum == 0) {
        return TILEXR_ERROR_MKIRT;
    }
    std::vector<TileXRCcuHccpDevEidInfo> eidInfos(eidNum);
    uint32_t queriedEidNum = eidNum;
    ret = loader_.RaGetDevEidInfoList(raInfo, eidInfos.data(), &queriedEidNum);
    if (ret != 0 || queriedEidNum == 0) {
        return TILEXR_ERROR_MKIRT;
    }
    const auto eidIt = std::find_if(
        eidInfos.begin(),
        eidInfos.begin() + queriedEidNum,
        [&localEid](const TileXRCcuHccpDevEidInfo& info) {
            return SameEid(localEid, info.eid);
        });
    if (eidIt == eidInfos.begin() + queriedEidNum) {
        return TILEXR_ERROR_NOT_FOUND;
    }
    state->eidInfo = *eidIt;

    TileXRCcuHccpCtxInitCfg ctxCfg {};
    ctxCfg.mode = TILEXR_CCU_NETWORK_OFFLINE;
    ctxCfg.rdma.disabledLiteThread = false;
    TileXRCcuHccpCtxInitAttr ctxAttr {};
    ctxAttr.phyId = devicePhyId_;
    ctxAttr.ub.eidIndex = state->eidInfo.eidIndex;
    ctxAttr.ub.eid = state->eidInfo.eid;
    ret = loader_.RaCtxInit(&ctxCfg, &ctxAttr, &state->resourceWindow.raCtxHandle);
    if (ret != 0 || state->resourceWindow.raCtxHandle == nullptr) {
        ReleasePeerEndpointState(state);
        return TILEXR_ERROR_MKIRT;
    }

    TileXRCcuHccpTokenId allocatedToken {};
    ret = loader_.RaCtxTokenIdAlloc(
        state->resourceWindow.raCtxHandle,
        &allocatedToken,
        &state->resourceWindow.tokenIdHandle);
    if (ret != 0 || state->resourceWindow.tokenIdHandle == nullptr) {
        ReleasePeerEndpointState(state);
        return TILEXR_ERROR_MKIRT;
    }
    TileXRCcuRaInfo randomInfo {};
    randomInfo.mode = TILEXR_CCU_NETWORK_OFFLINE;
    randomInfo.phyId = devicePhyId_;
    ret = loader_.RaGetSecRandom(&randomInfo, &state->jettyTokenValue);
    if (ret != 0) {
        ReleasePeerEndpointState(state);
        return TILEXR_ERROR_MKIRT;
    }
    state->resourceWindow.addr = localResourceWindow_.addr;
    state->resourceWindow.bytes = localResourceWindow_.bytes;
    state->resourceWindow.rawTokenId = localResourceWindow_.rawTokenId;
    state->resourceWindow.tokenId = localResourceWindow_.tokenId;
    state->resourceWindow.tokenValue = localResourceWindow_.tokenValue;
    state->resourceWindow.targetSegHandle = localResourceWindow_.targetSegHandle;
    state->resourceWindow.eid = localEid;
    state->resourceWindow.eidIndex = state->eidInfo.eidIndex;
    state->resourceWindow.funcId = state->eidInfo.funcId;
    state->resourceWindow.funcIdValid = true;
    state->resourceWindow.raCtxRegistered = false;

    uint16_t jettyCtxId = 0;
    if (!SelectEndpointRouteJettyCtxId(state->eidInfo.funcId, peerOrdinal, &jettyCtxId)) {
        ReleasePeerEndpointState(state);
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }

    ret = SelectTpRouteForPeer(
        state->resourceWindow.raCtxHandle,
        localEid,
        peerEid,
        state->tpType,
        &state->localTpHandle,
        &state->mappedJettyPriority);
    if (ret != TILEXR_SUCCESS) {
        ReleasePeerEndpointState(state);
        return ret;
    }

    TileXRCcuHccpCqInfo cqInfo {};
    cqInfo.in.chanHandle = nullptr;
    cqInfo.in.depth = TILEXR_CCU_DIRECT_CCU_POLL_CQ_DEPTH;
    cqInfo.in.ub.userCtx = 0;
    cqInfo.in.ub.mode = TILEXR_CCU_HCCP_JFC_MODE_CCU_POLL;
    cqInfo.in.ub.ceqn = 0;
    cqInfo.in.ub.flag.value = 0;
    ret = loader_.RaCtxCqCreate(state->resourceWindow.raCtxHandle, &cqInfo, &state->cqHandle);
    if (ret != 0 || state->cqHandle == nullptr) {
        ReleasePeerEndpointState(state);
        return TILEXR_ERROR_MKIRT;
    }

    const uint32_t sqDepth = SelectEndpointRouteSqDepth();
    TileXRCcuHccpQpCreateAttr qpAttr {};
    qpAttr.scqHandle = state->cqHandle;
    qpAttr.rcqHandle = state->cqHandle;
    qpAttr.srqHandle = state->cqHandle;
    qpAttr.sqDepth = sqDepth;
    qpAttr.rqDepth = TILEXR_CCU_HCCP_RQ_DEPTH_DEFAULT;
    qpAttr.transportMode = TILEXR_CCU_HCCP_TRANSPORT_MODE_RM;
    qpAttr.ub.mode = static_cast<int>(TILEXR_CCU_HCCP_JETTY_MODE_CCU);
    qpAttr.ub.jettyId = static_cast<uint16_t>(TILEXR_CCU_DIRECT_LOOP_JETTY_ID + jettyCtxId);
    qpAttr.ub.tokenIdHandle = state->resourceWindow.tokenIdHandle;
    qpAttr.ub.tokenValue = state->jettyTokenValue;
    qpAttr.ub.flag.value = 0;
    qpAttr.ub.flag.bs.shareJfr = 1;
    qpAttr.ub.jfsFlag.bs.errorSuspend = 1;
    qpAttr.ub.priority = state->mappedJettyPriority;
    qpAttr.ub.rnrRetry = TILEXR_CCU_HCCP_RNR_RETRY_DEFAULT;
    qpAttr.ub.errTimeout = state->tpType == TILEXR_CCU_HCCP_TP_TYPE_CTP ?
        TILEXR_CCU_DIRECT_CTP_ENDPOINT_ERR_TIMEOUT : TILEXR_CCU_DIRECT_ENDPOINT_ERR_TIMEOUT;
    qpAttr.ub.extMode.cstmFlag.value = 0;
    qpAttr.ub.extMode.cstmFlag.bs.sqCstm = 1;
    qpAttr.ub.extMode.sq.buffVa = SelectEndpointRouteSqVa(localResourceWindow_, jettyCtxId);
    qpAttr.ub.extMode.sq.buffSize = SelectEndpointRouteSqBytes(sqDepth);
    qpAttr.ub.extMode.sqebbNum = sqDepth;
    ret = loader_.RaCtxQpCreate(
        state->resourceWindow.raCtxHandle,
        &qpAttr,
        &state->qpInfo,
        &state->qpHandle);
    if (ret != 0 || state->qpHandle == nullptr || state->qpInfo.key.size == 0) {
        ReleasePeerEndpointState(state);
        return TILEXR_ERROR_MKIRT;
    }
    state->psn = endpointPsn_++;
    if (TraceEndpointRoute()) {
        std::cerr << "TileXRDirectCcuTrace peerEndpoint created"
                  << " rank=" << options_.rank
                  << " peerRank=" << peerRank
                  << " peerDevice=" << peerDevicePhyId
                  << " localEid=" << FormatEndpointEid(localEid)
                  << " eidIndex=" << state->eidInfo.eidIndex
                  << " funcId=" << state->eidInfo.funcId
                  << " tpHandle=0x" << std::hex << state->localTpHandle << std::dec
                  << " priority=" << static_cast<uint32_t>(state->mappedJettyPriority)
                  << " tpType=" << state->tpType
                  << " jettyCtxId=" << jettyCtxId
                  << " sqVa=0x" << std::hex << qpAttr.ub.extMode.sq.buffVa << std::dec
                  << " qpId=" << state->qpInfo.ub.id
                  << " psn=" << state->psn
                  << std::endl;
    }
    return TILEXR_SUCCESS;
}

int TileXRCcuDirectRuntime::PreparePeerEndpointRoutes(TileXRCcuDirectRuntimeReport* report)
{
    ReleasePeerEndpointRoutes();
    std::vector<uint32_t> allDevicePhyIds(static_cast<size_t>(options_.rankSize), 0);
    int ret = options_.allGather(
        &devicePhyId_,
        sizeof(devicePhyId_),
        allDevicePhyIds.data(),
        options_.allGatherUserData);
    if (ret != TILEXR_SUCCESS) {
        return Fail(report, "failed to exchange physical device ids for CCU endpoint routes", ret);
    }
    std::vector<uint32_t> peerDevicePhyIds;
    std::vector<uint32_t> peerRanks;
    for (int peer = 0; peer < options_.rankSize; ++peer) {
        if (peer != options_.rank) {
            peerRanks.push_back(static_cast<uint32_t>(peer));
            peerDevicePhyIds.push_back(allDevicePhyIds[static_cast<size_t>(peer)]);
        }
    }
    std::vector<TileXRCcuPeerEidRoute> topologyRoutes;
    std::string topologyMessage;
    ret = TileXRCcuResolvePeerEidRoutes(
        TILEXR_CCU_HCCL_ROOT_INFO_PATH,
        devicePhyId_,
        peerDevicePhyIds,
        &topologyRoutes,
        &topologyMessage);
    if (ret != TILEXR_SUCCESS || topologyRoutes.size() != peerRanks.size()) {
        return Fail(report, topologyMessage.empty() ?
            "failed to resolve peer-specific CCU EIDs" : topologyMessage, ret);
    }

    std::vector<std::array<uint8_t, TILEXR_CCU_EID_BYTES>> localEidsByPeer(
        static_cast<size_t>(options_.rankSize));
    for (uint32_t ordinal = 0; ordinal < peerRanks.size(); ++ordinal) {
        localEidsByPeer[peerRanks[ordinal]] = topologyRoutes[ordinal].localEid;
    }
    std::vector<std::array<uint8_t, TILEXR_CCU_EID_BYTES>> allLocalEidsByPeer(
        static_cast<size_t>(options_.rankSize) * static_cast<size_t>(options_.rankSize));
    ret = options_.allGather(
        localEidsByPeer.data(),
        localEidsByPeer.size() * sizeof(localEidsByPeer.front()),
        allLocalEidsByPeer.data(),
        options_.allGatherUserData);
    if (ret != TILEXR_SUCCESS) {
        return Fail(report, "failed to exchange peer-specific CCU topology EIDs", ret);
    }

    peerEndpointStates_.reserve(peerRanks.size());
    for (uint32_t ordinal = 0; ordinal < peerRanks.size(); ++ordinal) {
        const auto& peerEid = allLocalEidsByPeer[
            static_cast<size_t>(peerRanks[ordinal]) * static_cast<size_t>(options_.rankSize) +
            static_cast<size_t>(options_.rank)];
        if (IsEmptyEid(peerEid)) {
            ReleasePeerEndpointRoutes();
            return Fail(report, "missing reciprocal peer-specific CCU topology EID", TILEXR_ERROR_NOT_FOUND);
        }
        TileXRCcuPeerEndpointState state;
        ret = CreatePeerEndpointState(
            peerRanks[ordinal],
            peerDevicePhyIds[ordinal],
            topologyRoutes[ordinal].localEid,
            peerEid,
            topologyRoutes[ordinal].tpType,
            ordinal,
            &state);
        if (ret != TILEXR_SUCCESS) {
            ReleasePeerEndpointRoutes();
            return Fail(report, "failed to create peer-specific CCU endpoint", ret);
        }
        peerEndpointStates_.push_back(state);
    }

    std::vector<TileXRCcuPeerEndpointOffer> localOffers(static_cast<size_t>(options_.rankSize));
    for (const auto& state : peerEndpointStates_) {
        auto& offer = localOffers[state.peerRank];
        offer.resourceAddr = state.resourceWindow.addr;
        offer.resourceTokenId = state.resourceWindow.tokenId;
        offer.resourceRawTokenId = state.resourceWindow.rawTokenId;
        offer.resourceTokenValue = state.resourceWindow.tokenValue;
        offer.jettyTokenValue = state.jettyTokenValue;
        offer.eid = state.resourceWindow.eid;
        offer.qpKey = state.qpInfo.key;
        if (offer.qpKey.size == 0 || offer.qpKey.size > TILEXR_CCU_HCCP_QP_KEY_BYTES) {
            ReleasePeerEndpointRoutes();
            return Fail(report, "peer-specific CCU QP key has an invalid size", TILEXR_ERROR_MKIRT);
        }
        offer.psn = state.psn;
        offer.funcId = state.resourceWindow.funcId;
        offer.funcIdValid = state.resourceWindow.funcIdValid;
        offer.valid = true;
    }
    std::vector<TileXRCcuPeerEndpointOffer> allOffers(
        static_cast<size_t>(options_.rankSize) * static_cast<size_t>(options_.rankSize));
    ret = options_.allGather(
        localOffers.data(),
        localOffers.size() * sizeof(TileXRCcuPeerEndpointOffer),
        allOffers.data(),
        options_.allGatherUserData);
    if (ret != TILEXR_SUCCESS) {
        ReleasePeerEndpointRoutes();
        return Fail(report, "failed to exchange peer-specific CCU endpoint offers", ret);
    }

    std::vector<uint64_t> localTpHandles(static_cast<size_t>(options_.rankSize), 0);
    for (auto& state : peerEndpointStates_) {
        const auto& peerOffer = allOffers[
            static_cast<size_t>(state.peerRank) * static_cast<size_t>(options_.rankSize) +
            static_cast<size_t>(options_.rank)];
        if (!peerOffer.valid || peerOffer.qpKey.size == 0) {
            ReleasePeerEndpointRoutes();
            return Fail(report, "missing reciprocal peer-specific CCU endpoint offer", TILEXR_ERROR_NOT_FOUND);
        }
        localTpHandles[state.peerRank] = state.localTpHandle;
    }
    std::vector<uint64_t> allTpHandles(
        static_cast<size_t>(options_.rankSize) * static_cast<size_t>(options_.rankSize), 0);
    ret = options_.allGather(
        localTpHandles.data(),
        localTpHandles.size() * sizeof(uint64_t),
        allTpHandles.data(),
        options_.allGatherUserData);
    if (ret != TILEXR_SUCCESS) {
        ReleasePeerEndpointRoutes();
        return Fail(report, "failed to exchange peer-specific CCU TP handles", ret);
    }

    for (uint32_t ordinal = 0; ordinal < peerEndpointStates_.size(); ++ordinal) {
        auto& state = peerEndpointStates_[ordinal];
        const auto& peerOffer = allOffers[
            static_cast<size_t>(state.peerRank) * static_cast<size_t>(options_.rankSize) +
            static_cast<size_t>(options_.rank)];
        const uint64_t localTpHandle = localTpHandles[state.peerRank];
        const uint64_t peerTpHandle = allTpHandles[
            static_cast<size_t>(state.peerRank) * static_cast<size_t>(options_.rankSize) +
            static_cast<size_t>(options_.rank)];
        TileXRCcuHccpQpImportInfo importInfo {};
        importInfo.in.key = peerOffer.qpKey;
        if (importInfo.in.key.size == 0 || importInfo.in.key.size > TILEXR_CCU_HCCP_QP_KEY_BYTES) {
            ReleasePeerEndpointRoutes();
            return Fail(report, "peer-specific remote CCU QP key has an invalid size", TILEXR_ERROR_MKIRT);
        }
        importInfo.in.ub.mode = TILEXR_CCU_HCCP_JETTY_IMPORT_MODE_EXP;
        importInfo.in.ub.tokenValue = peerOffer.jettyTokenValue;
        importInfo.in.ub.policy = TILEXR_CCU_HCCP_JETTY_GRP_POLICY_RR;
        importInfo.in.ub.type = TILEXR_CCU_HCCP_TARGET_TYPE_JETTY;
        importInfo.in.ub.flag.value = 0;
        importInfo.in.ub.flag.bs.tokenPolicy = TILEXR_CCU_HCCP_TOKEN_POLICY_PLAIN_TEXT;
        importInfo.in.ub.expImportCfg.tpHandle = localTpHandle;
        importInfo.in.ub.expImportCfg.peerTpHandle = peerTpHandle;
        importInfo.in.ub.expImportCfg.txPsn = state.psn;
        importInfo.in.ub.expImportCfg.rxPsn = peerOffer.psn;
        importInfo.in.ub.tpType = state.tpType;
        ret = loader_.RaCtxQpImport(
            state.resourceWindow.raCtxHandle,
            &importInfo,
            &state.remoteQpHandle);
        if (ret != 0 || state.remoteQpHandle == nullptr) {
            ReleasePeerEndpointRoutes();
            return Fail(report, "failed to import peer-specific CCU QP", TILEXR_ERROR_MKIRT);
        }
        state.route.remoteEid = ReverseEndpointEid(peerOffer.eid);
        state.route.tpn = importInfo.out.ub.tpn;
        state.route.doorbellVa = state.qpInfo.ub.dbAddr;
        state.route.doorbellTokenId =
            state.qpInfo.ub.dbTokenId >> TILEXR_CCU_URMA_TOKEN_ID_RIGHT_SHIFT;
        state.route.doorbellTokenValue = state.jettyTokenValue;
        state.route.sqDepth = SelectEndpointRouteSqDepth();
        state.route.startJettyId = static_cast<uint16_t>(state.qpInfo.ub.id);
        state.route.remoteCcuVa = peerOffer.resourceAddr;
        state.route.memoryTokenId = peerOffer.resourceTokenId;
        state.route.memoryTokenValue = peerOffer.resourceTokenValue;
        state.route.endpointRouteVerified = true;
        if (TraceEndpointRoute()) {
            std::cerr << "TileXRDirectCcuTrace peerEndpoint imported"
                      << " rank=" << options_.rank
                      << " peerRank=" << state.peerRank
                      << " localEid=" << FormatEndpointEid(state.resourceWindow.eid)
                      << " peerEid=" << FormatEndpointEid(peerOffer.eid)
                      << " localTpHandle=0x" << std::hex << localTpHandle
                      << " peerTpHandle=0x" << peerTpHandle
                      << std::dec
                      << " localMemoryTokenId=0x" << std::hex << state.resourceWindow.tokenId
                      << " peerMemoryTokenId=0x" << peerOffer.resourceTokenId
                      << " localCcuResourceTokenId=0x" << state.resourceWindow.tokenId
                      << std::dec
                      << " localPsn=" << state.psn
                      << " peerPsn=" << peerOffer.psn
                      << " tpn=0x" << std::hex << state.route.tpn
                      << " doorbellVa=0x" << state.route.doorbellVa
                      << " remoteCcuVa=0x" << state.route.remoteCcuVa
                      << std::dec
                      << " taJettyId=" << state.route.startJettyId
                      << std::endl;
        }
    }
    localVerifiedEndpointRoute_ = peerEndpointStates_.front().route;
    localVerifiedEndpointRouteValid_ = true;
    if (report != nullptr) {
        report->initialized = initialized_;
        report->message = "peer-specific direct CCU endpoint routes prepared";
    }
    return TILEXR_SUCCESS;
}

int TileXRCcuDirectRuntime::QueryTpHandleForPeer(
    const std::array<uint8_t, TILEXR_CCU_EID_BYTES>& peerEid,
    uint64_t* tpHandle)
{
    return QueryTpHandleForPeer(
        localResourceWindow_.raCtxHandle,
        localResourceWindow_.eid,
        peerEid,
        tpHandle);
}

int TileXRCcuDirectRuntime::SelectTpRouteForPeer(
    void* ctxHandle,
    const std::array<uint8_t, TILEXR_CCU_EID_BYTES>& localEid,
    const std::array<uint8_t, TILEXR_CCU_EID_BYTES>& peerEid,
    uint32_t tpType,
    uint64_t* tpHandle,
    uint8_t* mappedJettyPriority)
{
    if (ctxHandle == nullptr || tpHandle == nullptr || mappedJettyPriority == nullptr ||
        IsEmptyEid(localEid) || IsEmptyEid(peerEid) ||
        loader_.RaGetTpInfoListAsync == nullptr || loader_.RaGetTpAttrAsync == nullptr ||
        loader_.RaSetTpAttrAsync == nullptr || loader_.RaGetAsyncReqResult == nullptr) {
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    *tpHandle = 0;
    *mappedJettyPriority = 0;

    TileXRCcuHccpGetTpCfg tpCfg {};
    tpCfg.flag.bs.rtp = tpType == TILEXR_CCU_HCCP_TP_TYPE_RTP ? 1 : 0;
    tpCfg.flag.bs.ctp = tpType == TILEXR_CCU_HCCP_TP_TYPE_CTP ? 1 : 0;
    tpCfg.transMode = TILEXR_CCU_HCCP_TRANSPORT_MODE_RM;
    std::copy(localEid.begin(), localEid.end(), tpCfg.localEid.raw);
    std::copy(peerEid.begin(), peerEid.end(), tpCfg.peerEid.raw);

    std::array<TileXRCcuHccpTpInfo, TILEXR_CCU_TP_HANDLE_REQUEST_NUM> tpInfos {};
    uint32_t tpInfoNum = static_cast<uint32_t>(tpInfos.size());
    void* reqHandle = nullptr;
    int ret = loader_.RaGetTpInfoListAsync(
        ctxHandle, &tpCfg, tpInfos.data(), &tpInfoNum, &reqHandle);
    if (ret != 0 || reqHandle == nullptr) {
        return TILEXR_ERROR_MKIRT;
    }
    ret = WaitRaCtxAsyncRequest(loader_, reqHandle);
    if (ret != TILEXR_SUCCESS || tpInfoNum == 0 || tpInfoNum > tpInfos.size()) {
        return ret == TILEXR_SUCCESS ? TILEXR_ERROR_NOT_FOUND : ret;
    }

    TileXRCcuHccpTpAttr attr {};
    uint32_t attrBitmap = TILEXR_CCU_TP_ATTR_BITMAP_SL_AVAILABLE |
        TILEXR_CCU_TP_ATTR_BITMAP_SL;
    reqHandle = nullptr;
    ret = loader_.RaGetTpAttrAsync(
        ctxHandle, tpInfos[0].tpHandle, &attrBitmap, &attr, &reqHandle);
    if (ret != 0 || reqHandle == nullptr) {
        return TILEXR_ERROR_MKIRT;
    }
    ret = WaitRaCtxAsyncRequest(loader_, reqHandle);
    if (ret != TILEXR_SUCCESS) {
        return ret;
    }

    uint32_t tpIndex = 0;
    uint8_t mappedSl = 0;
    if (!MapQosToTpAndSl(
            TILEXR_CCU_DEFAULT_HCCL_QOS, tpInfoNum, attr.slBitmap, &tpIndex, &mappedSl) ||
        tpInfos[tpIndex].tpHandle == 0) {
        return TILEXR_ERROR_NOT_FOUND;
    }

    if (tpType == TILEXR_CCU_HCCP_TP_TYPE_RTP) {
        TileXRCcuHccpTpAttr setAttr {};
        setAttr.sl = mappedSl;
        reqHandle = nullptr;
        ret = loader_.RaSetTpAttrAsync(
            ctxHandle,
            tpInfos[tpIndex].tpHandle,
            TILEXR_CCU_TP_ATTR_BITMAP_SL,
            &setAttr,
            &reqHandle);
        if (ret != 0 || reqHandle == nullptr) {
            return TILEXR_ERROR_MKIRT;
        }
        ret = WaitRaCtxAsyncRequest(loader_, reqHandle);
        if (ret != TILEXR_SUCCESS) {
            return ret;
        }
    }

    if (TraceEndpointRoute()) {
        std::cerr << "TileXRDirectCcuTrace endpointRoute selectedTp"
                  << " localEid=" << FormatEndpointEid(localEid)
                  << " peerEid=" << FormatEndpointEid(peerEid)
                  << " tpCount=" << tpInfoNum
                  << " slBitmap=0x" << std::hex << attr.slBitmap
                  << " tpIndex=" << std::dec << tpIndex
                  << " tpHandle=0x" << std::hex << tpInfos[tpIndex].tpHandle
                  << std::dec << " mappedSl=" << static_cast<uint32_t>(mappedSl)
                  << std::endl;
    }
    *tpHandle = tpInfos[tpIndex].tpHandle;
    *mappedJettyPriority = mappedSl;
    return TILEXR_SUCCESS;
}

int TileXRCcuDirectRuntime::QueryTpHandleForPeer(
    void* ctxHandle,
    const std::array<uint8_t, TILEXR_CCU_EID_BYTES>& localEid,
    const std::array<uint8_t, TILEXR_CCU_EID_BYTES>& peerEid,
    uint64_t* tpHandle)
{
    if (tpHandle == nullptr || IsEmptyEid(localEid) || IsEmptyEid(peerEid) ||
        ctxHandle == nullptr || loader_.RaGetTpInfoListAsync == nullptr ||
        loader_.RaGetAsyncReqResult == nullptr) {
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    *tpHandle = 0;

    TileXRCcuHccpGetTpCfg tpCfg {};
    tpCfg.flag.value = 0;
    tpCfg.flag.bs.rtp = 1;
    tpCfg.transMode = TILEXR_CCU_HCCP_TRANSPORT_MODE_RM;
    for (uint32_t i = 0; i < TILEXR_CCU_EID_BYTES; ++i) {
        tpCfg.localEid.raw[i] = localEid[i];
        tpCfg.peerEid.raw[i] = peerEid[i];
    }

    TileXRCcuHccpTpInfo tpInfo {};
    uint32_t tpInfoNum = 1;
    void* reqHandle = nullptr;
    const int ret = loader_.RaGetTpInfoListAsync(
        ctxHandle,
        &tpCfg,
        &tpInfo,
        &tpInfoNum,
        &reqHandle);
    if (TraceEndpointRoute()) {
        std::cerr << "TileXRDirectCcuTrace endpointRoute peerTpInfoAsync"
                  << " ret=" << ret
                  << " req=" << reqHandle
                  << " num=" << tpInfoNum
                  << " tpHandle=0x" << std::hex << tpInfo.tpHandle
                  << std::dec << std::endl;
    }
    if (ret != 0 || reqHandle == nullptr || tpInfoNum == 0) {
        return TILEXR_ERROR_MKIRT;
    }
    const int waitRet = WaitRaCtxAsyncRequest(loader_, reqHandle);
    if (waitRet != TILEXR_SUCCESS) {
        return waitRet;
    }
    if (tpInfo.tpHandle == 0) {
        return TILEXR_ERROR_NOT_FOUND;
    }
    if (TraceEndpointRoute()) {
        std::cerr << "TileXRDirectCcuTrace endpointRoute peerTpInfoReady"
                  << " localEid=" << FormatEndpointEid(localEid)
                  << " peerEid=" << FormatEndpointEid(peerEid)
                  << " tpHandle=0x" << std::hex << tpInfo.tpHandle
                  << std::dec << std::endl;
    }
    *tpHandle = tpInfo.tpHandle;
    return TILEXR_SUCCESS;
}

int TileXRCcuDirectRuntime::ImportPeerEndpointRoute(
    const TileXRCcuResourceWindowExchange& peerWindow,
    uint64_t localTpHandle,
    uint64_t peerTpHandle,
    uint32_t localPsn,
    uint32_t peerPsn,
    TileXRCcuLowerLayerTransportRoute* importedRoute)
{
    if (importedRoute == nullptr || peerWindow.qpKey.size == 0 || localTpHandle == 0 || peerTpHandle == 0 ||
        localResourceWindow_.raCtxHandle == nullptr || loader_.RaCtxQpImport == nullptr ||
        !localVerifiedEndpointRouteValid_ || !HasCompleteEndpointRoute(localVerifiedEndpointRoute_)) {
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    *importedRoute = TileXRCcuLowerLayerTransportRoute{};

    TileXRCcuHccpQpImportInfo importInfo {};
    importInfo.in.key = peerWindow.qpKey;
    importInfo.in.ub.mode = TILEXR_CCU_HCCP_JETTY_IMPORT_MODE_EXP;
    importInfo.in.ub.tokenValue = peerWindow.tokenValue;
    importInfo.in.ub.policy = TILEXR_CCU_HCCP_JETTY_GRP_POLICY_RR;
    importInfo.in.ub.type = TILEXR_CCU_HCCP_TARGET_TYPE_JETTY;
    importInfo.in.ub.flag.value = 0;
    importInfo.in.ub.flag.bs.tokenPolicy = TILEXR_CCU_HCCP_TOKEN_POLICY_PLAIN_TEXT;
    importInfo.in.ub.expImportCfg.tpHandle = localTpHandle;
    importInfo.in.ub.expImportCfg.peerTpHandle = peerTpHandle;
    importInfo.in.ub.expImportCfg.txPsn = localPsn;
    importInfo.in.ub.expImportCfg.rxPsn = peerPsn;
    importInfo.in.ub.tpType = TILEXR_CCU_HCCP_TP_TYPE_RTP;

    void* remoteQpHandle = nullptr;
    const int ret = loader_.RaCtxQpImport(localResourceWindow_.raCtxHandle, &importInfo, &remoteQpHandle);
    if (TraceEndpointRoute()) {
        std::cerr << "TileXRDirectCcuTrace endpointRoute peerQpImport"
                  << " ret=" << ret
                  << " remoteQp=" << remoteQpHandle
                  << " localTpHandle=0x" << std::hex << localTpHandle
                  << " peerTpHandle=0x" << peerTpHandle
                  << " tpn=0x" << importInfo.out.ub.tpn
                  << std::dec
                  << " localPsn=" << localPsn
                  << " peerPsn=" << peerPsn
                  << " peerEid=" << FormatEndpointEid(peerWindow.remoteEid)
                  << " channelEid=" << FormatEndpointEid(ReverseEndpointEid(peerWindow.remoteEid))
                  << std::endl;
    }
    if (ret != 0 || remoteQpHandle == nullptr) {
        return TILEXR_ERROR_MKIRT;
    }
    endpointPeerRemoteQpHandles_.push_back(remoteQpHandle);

    importedRoute->remoteEid = ReverseEndpointEid(peerWindow.remoteEid);
    importedRoute->tpn = importInfo.out.ub.tpn;
    importedRoute->doorbellVa = localVerifiedEndpointRoute_.doorbellVa;
    importedRoute->doorbellTokenId = localVerifiedEndpointRoute_.doorbellTokenId;
    importedRoute->doorbellTokenValue = localVerifiedEndpointRoute_.doorbellTokenValue;
    importedRoute->sqDepth = localVerifiedEndpointRoute_.sqDepth;
    importedRoute->startJettyId = localVerifiedEndpointRoute_.startJettyId;
    importedRoute->endpointRouteVerified = true;
    return HasCompleteEndpointRoute(*importedRoute) ? TILEXR_SUCCESS : TILEXR_ERROR_PARA_CHECK_FAIL;
}

int TileXRCcuDirectRuntime::ExportRemoteCcuRmaBuffers(std::vector<TileXRCcuRemoteCcuBufferInfo>* buffers)
{
    if (buffers == nullptr) {
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    buffers->clear();
    if (!resourceWindowRegistered_) {
        return TILEXR_ERROR_NOT_INITIALIZED;
    }
    if (!peerEndpointStates_.empty()) {
        buffers->reserve(peerEndpointStates_.size());
        for (const auto& state : peerEndpointStates_) {
            TileXRCcuRemoteCcuBufferInfo remote;
            remote.remoteCcuVa = state.route.remoteCcuVa;
            remote.peerRank = state.peerRank;
            remote.memoryTokenId = state.route.memoryTokenId;
            remote.rawMemoryTokenId = state.route.memoryTokenId << TILEXR_CCU_URMA_TOKEN_ID_RIGHT_SHIFT;
            remote.memoryTokenValue = state.route.memoryTokenValue;
            remote.localPfeId = state.resourceWindow.funcId;
            remote.localPfeIdValid = state.resourceWindow.funcIdValid;
            remote.remoteEid = state.route.remoteEid;
            remote.tpn = state.route.tpn;
            remote.doorbellVa = state.route.doorbellVa;
            remote.doorbellTokenId = state.route.doorbellTokenId;
            remote.doorbellTokenValue = state.route.doorbellTokenValue;
            remote.sqDepth = state.route.sqDepth;
            remote.startJettyId = state.route.startJettyId;
            remote.localDoorbellVa = state.route.doorbellVa;
            remote.localDoorbellTokenId = state.route.doorbellTokenId;
            remote.localDoorbellTokenValue = state.route.doorbellTokenValue;
            remote.localSqDepth = state.route.sqDepth;
            remote.endpointRouteVerified = true;
            buffers->push_back(remote);
        }
        return TILEXR_SUCCESS;
    }
    if (options_.rankSize <= 1) {
        return TILEXR_SUCCESS;
    }
    if (options_.rank < 0 || options_.rank >= options_.rankSize ||
        options_.rankSize > TILEXR_CCU_DIRECT_MAX_RANK_SIZE ||
        options_.allGather == nullptr) {
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    ReleasePeerEndpointImports();

    TileXRCcuResourceWindowExchange local {
        localResourceWindow_.addr,
        localResourceWindow_.bytes,
        localResourceWindow_.tokenId,
        localResourceWindow_.rawTokenId,
        localResourceWindow_.tokenValue,
    };
    local.funcId = localResourceWindow_.funcId;
    local.funcIdValid = localResourceWindow_.funcIdValid;
    if (localVerifiedEndpointRouteValid_) {
        local.remoteEid = localVerifiedEndpointRoute_.remoteEid;
        local.tpn = localVerifiedEndpointRoute_.tpn;
        local.doorbellVa = localVerifiedEndpointRoute_.doorbellVa;
        local.doorbellTokenId = localVerifiedEndpointRoute_.doorbellTokenId;
        local.doorbellTokenValue = localVerifiedEndpointRoute_.doorbellTokenValue;
        local.sqDepth = localVerifiedEndpointRoute_.sqDepth;
        local.startJettyId = localVerifiedEndpointRoute_.startJettyId;
        local.qpKey = endpointQpKey_;
        local.psn = endpointPsn_;
        local.endpointRouteVerified = true;
        local.channelResourceOwnerVerified = localVerifiedEndpointRoute_.channelResourceOwnerVerified;
        local.transportResourceExchangeVerified = localVerifiedEndpointRoute_.transportResourceExchangeVerified;
    }
    std::vector<TileXRCcuResourceWindowExchange> all(static_cast<size_t>(options_.rankSize));
    const int exportRet = options_.allGather(
        &local,
        sizeof(local),
        all.data(),
        options_.allGatherUserData);
    if (exportRet != TILEXR_SUCCESS) {
        return exportRet;
    }

    const bool useImportedPeerRoute = UseImportedPeerEndpointRoute();
    const bool canImportPeerRoutes =
        useImportedPeerRoute &&
        endpointQpKeyValid_ &&
        localVerifiedEndpointRouteValid_ &&
        HasCompleteEndpointRoute(localVerifiedEndpointRoute_) &&
        localResourceWindow_.raCtxHandle != nullptr &&
        loader_.RaCtxQpImport != nullptr &&
        loader_.RaGetTpInfoListAsync != nullptr &&
        loader_.RaGetAsyncReqResult != nullptr;
    TileXRCcuEndpointTpHandleExchange localTpHandles {};
    std::vector<TileXRCcuEndpointTpHandleExchange> allTpHandles;
    bool peerTpHandlesReady = false;
    if (canImportPeerRoutes) {
        localTpHandles.psn = endpointPsn_;
        for (int peer = 0; peer < options_.rankSize; ++peer) {
            if (peer == options_.rank) {
                continue;
            }
            const auto& peerWindow = all[peer];
            if (peerWindow.addr == 0 || IsEmptyEid(peerWindow.remoteEid)) {
                continue;
            }
            uint64_t tpHandle = 0;
            if (QueryTpHandleForPeer(peerWindow.remoteEid, &tpHandle) == TILEXR_SUCCESS) {
                localTpHandles.tpHandles[peer] = tpHandle;
            }
        }
        allTpHandles.resize(static_cast<size_t>(options_.rankSize));
        const int tpExchangeRet = options_.allGather(
            &localTpHandles,
            sizeof(localTpHandles),
            allTpHandles.data(),
            options_.allGatherUserData);
        peerTpHandlesReady = tpExchangeRet == TILEXR_SUCCESS;
    }

    const uint64_t remoteCcuVaOffset = SelectRemoteCcuVaOffset();
    buffers->reserve(static_cast<size_t>(options_.rankSize - 1));
    for (int peer = 0; peer < options_.rankSize; ++peer) {
        if (peer == options_.rank) {
            continue;
        }
        const auto& peerWindow = all[peer];
        if (peerWindow.addr == 0) {
            buffers->clear();
            return TILEXR_ERROR_NOT_FOUND;
        }
        TileXRCcuRemoteCcuBufferInfo remote;
        if (remoteCcuVaOffset > std::numeric_limits<uint64_t>::max() - peerWindow.addr) {
            buffers->clear();
            return TILEXR_ERROR_PARA_CHECK_FAIL;
        }
        remote.remoteCcuVa = peerWindow.addr + remoteCcuVaOffset;
        remote.peerRank = static_cast<uint32_t>(peer);
        remote.memoryTokenId = peerWindow.tokenId;
        remote.rawMemoryTokenId = peerWindow.rawTokenId;
        remote.memoryTokenValue = peerWindow.tokenValue;
        remote.localPfeId = localResourceWindow_.funcId;
        remote.localPfeIdValid = localResourceWindow_.funcIdValid;
        remote.remoteEid = ReverseEndpointEid(peerWindow.remoteEid);
        TileXRCcuLowerLayerTransportRoute importedRoute;
        const uint64_t localTpForPeer = peerTpHandlesReady ?
            localTpHandles.tpHandles[peer] : 0;
        const uint64_t peerTpForLocal = peerTpHandlesReady ?
            allTpHandles[static_cast<size_t>(peer)].tpHandles[options_.rank] : 0;
        const bool importedPeerRoute =
            peerTpHandlesReady &&
            peerTpForLocal != 0 &&
            localTpForPeer != 0 &&
            ImportPeerEndpointRoute(
                peerWindow,
                localTpForPeer,
                peerTpForLocal,
                localTpHandles.psn,
                allTpHandles[static_cast<size_t>(peer)].psn,
                &importedRoute) == TILEXR_SUCCESS;
        if (importedPeerRoute) {
            remote.remoteEid = importedRoute.remoteEid;
            remote.tpn = importedRoute.tpn;
            remote.doorbellVa = importedRoute.doorbellVa;
            remote.doorbellTokenId = importedRoute.doorbellTokenId;
            remote.doorbellTokenValue = importedRoute.doorbellTokenValue;
            remote.sqDepth = importedRoute.sqDepth;
            remote.startJettyId = importedRoute.startJettyId;
        } else {
            remote.tpn = peerWindow.tpn;
            remote.doorbellVa = peerWindow.doorbellVa;
            remote.doorbellTokenId = peerWindow.doorbellTokenId;
            remote.doorbellTokenValue = peerWindow.doorbellTokenValue;
            remote.sqDepth = peerWindow.sqDepth;
            remote.startJettyId = peerWindow.startJettyId;
            if (!useImportedPeerRoute && TraceEndpointRoute() && peerWindow.endpointRouteVerified) {
                std::cerr << "TileXRDirectCcuTrace endpointRoute usePeerExportedRoute"
                          << " peerRank=" << peer
                          << " tpn=0x" << std::hex << peerWindow.tpn
                          << " peerEid=" << FormatEndpointEid(peerWindow.remoteEid)
                          << " channelEid=" << FormatEndpointEid(remote.remoteEid)
                          << " remoteCcuVa=0x" << remote.remoteCcuVa
                          << " remoteCcuVaBase=0x" << peerWindow.addr
                          << " remoteCcuVaOffset=0x" << remoteCcuVaOffset
                          << std::dec << std::endl;
            }
        }
        if (localVerifiedEndpointRouteValid_) {
            remote.localDoorbellVa = localVerifiedEndpointRoute_.doorbellVa;
            remote.localDoorbellTokenId = localVerifiedEndpointRoute_.doorbellTokenId;
            remote.localDoorbellTokenValue = localVerifiedEndpointRoute_.doorbellTokenValue;
            remote.localSqDepth = localVerifiedEndpointRoute_.sqDepth;
        }
        remote.endpointRouteVerified = peerWindow.endpointRouteVerified;
        if (importedPeerRoute) {
            remote.endpointRouteVerified = true;
        }
        remote.channelResourceOwnerVerified = peerWindow.channelResourceOwnerVerified;
        remote.transportResourceExchangeVerified = peerWindow.transportResourceExchangeVerified;
        if (TraceEndpointRoute()) {
            std::cerr << "TileXRDirectCcuTrace endpointRoute channelRoute"
                      << " peerRank=" << peer
                      << " importedPeerRoute=" << (importedPeerRoute ? 1 : 0)
                      << " peerTpHandlesReady=" << (peerTpHandlesReady ? 1 : 0)
                      << " localEid=" << FormatEndpointEid(localResourceWindow_.eid)
                      << " peerEid=" << FormatEndpointEid(peerWindow.remoteEid)
                      << " selectedRemoteEid=" << FormatEndpointEid(remote.remoteEid)
                      << " localTpHandle=0x" << std::hex << localTpForPeer
                      << " peerTpHandle=0x" << peerTpForLocal
                      << " tpn=0x" << remote.tpn
                      << " remoteCcuVa=0x" << remote.remoteCcuVa
                      << " remoteCcuVaBase=0x" << peerWindow.addr
                      << " remoteCcuVaOffset=0x" << remoteCcuVaOffset
                      << " doorbellVa=0x" << remote.doorbellVa
                      << " localDoorbellVa=0x" << remote.localDoorbellVa
                      << " startJettyId=0x" << remote.startJettyId
                      << " memoryTokenId=0x" << remote.memoryTokenId
                      << " rawMemoryTokenId=0x" << remote.rawMemoryTokenId
                      << " memoryTokenValue=0x" << remote.memoryTokenValue
                      << std::dec
                      << " localPfeId=" << remote.localPfeId
                      << " localPfeIdValid=" << (remote.localPfeIdValid ? 1 : 0)
                      << " endpointRouteVerified=" << (remote.endpointRouteVerified ? 1 : 0)
                      << " channelResourceOwnerVerified=" << (remote.channelResourceOwnerVerified ? 1 : 0)
                      << " transportResourceExchangeVerified="
                      << (remote.transportResourceExchangeVerified ? 1 : 0)
                      << std::endl;
        }
        buffers->push_back(remote);
    }
    if (peerTpHandlesReady) {
        ++endpointPsn_;
    }
    return TILEXR_SUCCESS;
}

int TileXRCcuDirectRuntime::ExportLowerLayerTransportSnapshot(
    const TileXRCcuLowerLayerTransportSnapshot& templateSnapshot,
    TileXRCcuLowerLayerTransportSnapshot* snapshot) const
{
    if (snapshot == nullptr) {
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    *snapshot = TileXRCcuLowerLayerTransportSnapshot{};
    if (!resourceWindowRegistered_) {
        return TILEXR_ERROR_NOT_INITIALIZED;
    }

    *snapshot = templateSnapshot;
    for (uint32_t i = 0; i < snapshot->routes.size(); ++i) {
        auto& route = snapshot->routes[i];
        const bool preserveVerifiedRoute = route.endpointRouteVerified && HasCompleteEndpointRoute(route);
        bool filledSyntheticEndpointField = false;
        if (IsEmptyEid(route.remoteEid)) {
            route.remoteEid[0] = static_cast<uint8_t>(devicePhyId_ & 0xffU);
            route.remoteEid[1] = static_cast<uint8_t>((devicePhyId_ >> 8U) & 0xffU);
            route.remoteEid[2] = static_cast<uint8_t>(route.peerRank & 0xffU);
            route.remoteEid[3] = static_cast<uint8_t>((route.peerRank >> 8U) & 0xffU);
            route.remoteEid[4] = static_cast<uint8_t>(options_.rank & 0xff);
            route.remoteEid[5] = static_cast<uint8_t>(snapshot->dieId);
            route.remoteEid[6] = static_cast<uint8_t>(route.channelId & 0xffU);
            route.remoteEid[7] = static_cast<uint8_t>((route.channelId >> 8U) & 0xffU);
            filledSyntheticEndpointField = true;
        }
        if (!preserveVerifiedRoute && route.tpn == 0) {
            route.tpn = route.channelId + 1U;
            filledSyntheticEndpointField = true;
        }
        if (route.doorbellVa == 0) {
            route.doorbellVa = localResourceWindow_.addr + TILEXR_CCU_V1_XN_RESOURCE_OFFSET +
                static_cast<uint64_t>(snapshot->xnStartId + i) * TILEXR_CCU_XN_SLOT_BYTES;
            filledSyntheticEndpointField = true;
        }
        if (route.doorbellTokenId == 0) {
            route.doorbellTokenId = localResourceWindow_.tokenId;
            filledSyntheticEndpointField = true;
        }
        if (!preserveVerifiedRoute && route.doorbellTokenValue == 0) {
            route.doorbellTokenValue = localResourceWindow_.tokenValue;
        }
        if (route.sqDepth == 0) {
            route.sqDepth = TILEXR_CCU_DEFAULT_DIRECT_SQ_DEPTH;
            filledSyntheticEndpointField = true;
        }
        route.endpointRouteVerified = (preserveVerifiedRoute && !filledSyntheticEndpointField) ||
            (filledSyntheticEndpointField && TrustSyntheticEndpointRouteForDiagnostics() &&
             HasCompleteEndpointRoute(route));
    }
    return TILEXR_SUCCESS;
}

} // namespace TileXR
