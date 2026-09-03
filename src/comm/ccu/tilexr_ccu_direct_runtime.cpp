/*
 * Copyright (c) 2026 TileXR Project
 * Licensed under the Apache License, Version 2.0
 */

#include "ccu/tilexr_ccu_direct_runtime.h"

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
constexpr uint64_t TILEXR_CCU_V1_WQE_BASIC_BLOCK_OFFSET = TILEXR_CCU_V1_CCUM_OFFSET + 0x800000ULL;
constexpr uint64_t TILEXR_CCU_DIRECT_SQ_BUFFER_BYTES = 256ULL * 1024ULL;
constexpr uint32_t TILEXR_CCU_DIRECT_CCU_POLL_CQ_DEPTH = 64;
constexpr uint32_t TILEXR_CCU_DIRECT_ENDPOINT_ROUTE_ASYNC_MAX_POLLS = 1000;
constexpr uint32_t TILEXR_CCU_DIRECT_ENDPOINT_ROUTE_ASYNC_SLEEP_US = 1000;
constexpr uint32_t TILEXR_CCU_DIRECT_ENDPOINT_ROUTE_MAX_ATTEMPTS = 8;
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
constexpr const char* TILEXR_CCU_DIRECT_ENDPOINT_ROUTE_COLLECTION_MODE_ENV =
    "TILEXR_CCU_DIRECT_ENDPOINT_ROUTE_COLLECTION_MODE";
constexpr const char* TILEXR_CCU_DIRECT_TRACE_ENDPOINT_ROUTE_ENV =
    "TILEXR_CCU_DIRECT_TRACE_ENDPOINT_ROUTE";
constexpr const char* TILEXR_CCU_DIRECT_TRUST_SYNTHETIC_ENDPOINT_ROUTE_ENV =
    "TILEXR_CCU_DIRECT_TRUST_SYNTHETIC_ENDPOINT_ROUTE";
constexpr const char* TILEXR_CCU_DIRECT_ENDPOINT_ROUTE_EXCHANGE_MODE_ENV =
    "TILEXR_CCU_DIRECT_ENDPOINT_ROUTE_EXCHANGE_MODE";
constexpr const char* TILEXR_CCU_DIRECT_REMOTE_CCU_VA_OFFSET_ENV =
    "TILEXR_CCU_DIRECT_REMOTE_CCU_VA_OFFSET";
constexpr uint64_t TILEXR_CCU_UB_MEM_PAGE_SIZE = 4096ULL;
constexpr uint32_t TILEXR_CCU_URMA_TOKEN_ID_RIGHT_SHIFT = 8;

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

bool UseImportedPeerEndpointRoute()
{
    const char* mode = std::getenv(TILEXR_CCU_DIRECT_ENDPOINT_ROUTE_EXCHANGE_MODE_ENV);
    return mode == nullptr || mode[0] == '\0' || std::strcmp(mode, "imported_peer") == 0;
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

uint32_t SelectEndpointRouteSqDepth()
{
    return TILEXR_CCU_DEFAULT_DIRECT_SQ_DEPTH;
}

uint32_t SelectEndpointRouteSqBytes(uint32_t sqDepth)
{
    return sqDepth * TILEXR_CCU_DIRECT_SQ_EBB_WORDS * TILEXR_CCU_DIRECT_CCUM_SQE_BYTES;
}

uint64_t SelectEndpointRouteSqVa(const TileXRCcuLocalResourceWindowInfo& localResourceWindow)
{
    return localResourceWindow.addr + TILEXR_CCU_V1_WQE_BASIC_BLOCK_OFFSET +
        static_cast<uint64_t>(TILEXR_CCU_DIRECT_LOOP_JETTY_CTX_ID) * TILEXR_CCU_DIRECT_SQ_BUFFER_BYTES;
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

    void* ctxHandle = nullptr;
    void* tokenIdHandle = nullptr;
    void* lmemHandle = nullptr;

    TileXRCcuHccpCtxInitCfg ctxCfg {};
    ctxCfg.mode = TILEXR_CCU_NETWORK_OFFLINE;
    ctxCfg.rdma.disabledLiteThread = false;
    TileXRCcuHccpCtxInitAttr ctxAttr {};
    ctxAttr.phyId = devicePhyId_;
    ctxAttr.ub.eidIndex = eidInfos[0].eidIndex;
    ctxAttr.ub.eid = eidInfos[0].eid;

    ret = loader_.RaCtxInit(&ctxCfg, &ctxAttr, &ctxHandle);
    if (ret != 0 || ctxHandle == nullptr) {
        return TILEXR_ERROR_MKIRT;
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
        localResourceWindow_.eid[i] = eidInfos[0].eid.raw[i];
    }
    localResourceWindow_.eidIndex = eidInfos[0].eidIndex;
    localResourceWindow_.raCtxRegistered = true;
    resourceWindowRegistered_ = true;
    return TILEXR_SUCCESS;
}

void TileXRCcuDirectRuntime::ReleaseRegisteredResourceWindow()
{
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

void TileXRCcuDirectRuntime::ReleaseLocalEndpointRoute()
{
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
    const uint64_t sqVa = SelectEndpointRouteSqVa(localResourceWindow_);
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

int TileXRCcuDirectRuntime::QueryTpHandleForPeer(
    const std::array<uint8_t, TILEXR_CCU_EID_BYTES>& peerEid,
    uint64_t* tpHandle)
{
    if (tpHandle == nullptr || IsEmptyEid(localResourceWindow_.eid) || IsEmptyEid(peerEid) ||
        localResourceWindow_.raCtxHandle == nullptr || loader_.RaGetTpInfoListAsync == nullptr ||
        loader_.RaGetAsyncReqResult == nullptr) {
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    *tpHandle = 0;

    TileXRCcuHccpGetTpCfg tpCfg {};
    tpCfg.flag.value = 0;
    tpCfg.flag.bs.rtp = 1;
    tpCfg.transMode = TILEXR_CCU_HCCP_TRANSPORT_MODE_RM;
    for (uint32_t i = 0; i < TILEXR_CCU_EID_BYTES; ++i) {
        tpCfg.localEid.raw[i] = localResourceWindow_.eid[i];
        tpCfg.peerEid.raw[i] = peerEid[i];
    }

    TileXRCcuHccpTpInfo tpInfo {};
    uint32_t tpInfoNum = 1;
    void* reqHandle = nullptr;
    const int ret = loader_.RaGetTpInfoListAsync(
        localResourceWindow_.raCtxHandle,
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
    if (localVerifiedEndpointRouteValid_) {
        local.remoteEid = localVerifiedEndpointRoute_.remoteEid;
        local.tpn = localVerifiedEndpointRoute_.tpn;
        local.doorbellVa = localVerifiedEndpointRoute_.doorbellVa;
        local.doorbellTokenId = localVerifiedEndpointRoute_.doorbellTokenId;
        local.doorbellTokenValue = localVerifiedEndpointRoute_.doorbellTokenValue;
        local.sqDepth = localVerifiedEndpointRoute_.sqDepth;
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
        } else {
            remote.tpn = peerWindow.tpn;
            remote.doorbellVa = peerWindow.doorbellVa;
            remote.doorbellTokenId = peerWindow.doorbellTokenId;
            remote.doorbellTokenValue = peerWindow.doorbellTokenValue;
            remote.sqDepth = peerWindow.sqDepth;
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
                      << " memoryTokenId=0x" << remote.memoryTokenId
                      << " rawMemoryTokenId=0x" << remote.rawMemoryTokenId
                      << " memoryTokenValue=0x" << remote.memoryTokenValue
                      << std::dec
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
