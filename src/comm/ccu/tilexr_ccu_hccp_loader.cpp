/*
 * Copyright (c) 2026 TileXR Project
 * Licensed under the Apache License, Version 2.0
 */

#include "ccu/tilexr_ccu_hccp_loader.h"

#include "tilexr_types.h"

#include <cstdlib>
#include <dlfcn.h>
#include <map>
#include <mutex>
#include <sstream>
#include <utility>

namespace TileXR {
namespace {

void ResetReport(TileXRCcuHccpLoaderReport* report)
{
    if (report != nullptr) {
        *report = TileXRCcuHccpLoaderReport{};
    }
}

int Fail(TileXRCcuHccpLoaderReport* report, const std::string& message)
{
    if (report != nullptr) {
        report->message = message;
    }
    return TILEXR_ERROR_NOT_FOUND;
}

int FailWithCode(TileXRCcuHccpLoaderReport* report, const std::string& message, int code)
{
    if (report != nullptr) {
        report->message = message;
    }
    return code;
}

template <typename Fn>
bool LoadSymbol(void* handle, Fn& out, const char* primary, const char* fallback)
{
    out = reinterpret_cast<Fn>(dlsym(handle, primary));
    if (out == nullptr && fallback != nullptr) {
        out = reinterpret_cast<Fn>(dlsym(handle, fallback));
    }
    return out != nullptr;
}

template <typename Fn>
void LoadOptionalSymbol(void* handle, Fn& out, const char* primary, const char* fallback)
{
    if (handle == nullptr) {
        out = nullptr;
        return;
    }
    out = reinterpret_cast<Fn>(dlsym(handle, primary));
    if (out == nullptr && fallback != nullptr) {
        out = reinterpret_cast<Fn>(dlsym(handle, fallback));
    }
}

using RaHdcKey = std::pair<uint32_t, int>;

std::mutex g_raHdcMtx;
std::map<RaHdcKey, uint32_t> g_raHdcRefs;
uint32_t g_netServiceRefs = 0;
int g_netServiceHdcType = 0;

struct CcuTlvSession {
    void* handle = nullptr;
    uint32_t bufferSize = 0;
    uint32_t refs = 0;
};

std::mutex g_ccuTlvMtx;
std::map<uint32_t, CcuTlvSession> g_ccuTlvSessions;

bool EnvFlag(const char* name)
{
    const char* value = std::getenv(name);
    return value != nullptr && value[0] != '\0' && value[0] != '0';
}

std::string RaConfigText(const TileXRCcuRaInitConfig& config)
{
    std::ostringstream text;
    text << "phyId=" << config.phyId
         << " nicPosition=" << config.nicPosition
         << " hdcType=" << config.hdcType
         << " enableHdcAsync=" << (config.enableHdcAsync ? 1 : 0);
    return text.str();
}

} // namespace

TileXRCcuHccpLoader::~TileXRCcuHccpLoader()
{
    Unload();
}

int TileXRCcuHccpLoader::Load(TileXRCcuHccpLoaderReport* report)
{
    ResetReport(report);
    if (loaded_) {
        if (report != nullptr) {
            report->loaded = true;
            report->message = "ok";
        }
        return TILEXR_SUCCESS;
    }

    raHandle_ = dlopen("libra.so", RTLD_NOW);
    if (raHandle_ == nullptr) {
        return Fail(report, std::string("failed to load libra.so: ") + dlerror());
    }

    if (!LoadSymbol(raHandle_, RaCustomChannel, "RaCustomChannel", "ra_custom_channel")) {
        Unload();
        return Fail(report, "missing RaCustomChannel/ra_custom_channel in libra.so");
    }
    if (!LoadSymbol(raHandle_, RaInit, "RaInit", nullptr)) {
        Unload();
        return Fail(report, "missing RaInit in libra.so");
    }
    if (!LoadSymbol(raHandle_, RaDeinit, "RaDeinit", nullptr)) {
        Unload();
        return Fail(report, "missing RaDeinit in libra.so");
    }
    LoadOptionalSymbol(raHandle_, RaTlvInit, "RaTlvInit", nullptr);
    LoadOptionalSymbol(raHandle_, RaTlvRequest, "RaTlvRequest", nullptr);
    LoadOptionalSymbol(raHandle_, RaTlvDeinit, "RaTlvDeinit", nullptr);
    LoadOptionalSymbol(raHandle_, RaGetDevEidInfoNum, "RaGetDevEidInfoNum", "ra_get_dev_eid_info_num");
    LoadOptionalSymbol(raHandle_, RaGetDevEidInfoList, "RaGetDevEidInfoList", "ra_get_dev_eid_info_list");
    LoadOptionalSymbol(raHandle_, RaCtxInit, "RaCtxInit", "ra_ctx_init");
    LoadOptionalSymbol(raHandle_, RaCtxDeinit, "RaCtxDeinit", "ra_ctx_deinit");
    LoadOptionalSymbol(raHandle_, RaCtxTokenIdAlloc, "RaCtxTokenIdAlloc", "ra_ctx_token_id_alloc");
    LoadOptionalSymbol(raHandle_, RaCtxTokenIdFree, "RaCtxTokenIdFree", "ra_ctx_token_id_free");
    LoadOptionalSymbol(raHandle_, RaCtxLmemRegister, "RaCtxLmemRegister", "ra_ctx_lmem_register");
    LoadOptionalSymbol(raHandle_, RaCtxLmemUnregister, "RaCtxLmemUnregister", "ra_ctx_lmem_unregister");
    LoadOptionalSymbol(raHandle_, RaGetSecRandom, "RaGetSecRandom", "ra_get_sec_random");
    LoadOptionalSymbol(raHandle_, RaCtxChanCreate, "RaCtxChanCreate", "ra_ctx_chan_create");
    LoadOptionalSymbol(raHandle_, RaCtxChanDestroy, "RaCtxChanDestroy", "ra_ctx_chan_destroy");
    LoadOptionalSymbol(raHandle_, RaCtxCqCreate, "RaCtxCqCreate", "ra_ctx_cq_create");
    LoadOptionalSymbol(raHandle_, RaCtxCqDestroy, "RaCtxCqDestroy", "ra_ctx_cq_destroy");
    LoadOptionalSymbol(raHandle_, RaCtxQpCreate, "RaCtxQpCreate", "ra_ctx_qp_create");
    LoadOptionalSymbol(raHandle_, RaCtxQpDestroy, "RaCtxQpDestroy", "ra_ctx_qp_destroy");
    LoadOptionalSymbol(raHandle_, RaCtxQpImport, "RaCtxQpImport", "ra_ctx_qp_import");
    LoadOptionalSymbol(raHandle_, RaCtxQpUnimport, "RaCtxQpUnimport", "ra_ctx_qp_unimport");
    LoadOptionalSymbol(raHandle_, RaCtxQpBind, "RaCtxQpBind", "ra_ctx_qp_bind");
    LoadOptionalSymbol(raHandle_, RaCtxQpUnbind, "RaCtxQpUnbind", "ra_ctx_qp_unbind");
    LoadOptionalSymbol(raHandle_, RaGetTpInfoListAsync, "RaGetTpInfoListAsync", "ra_get_tp_info_list_async");
    LoadOptionalSymbol(raHandle_, RaGetAsyncReqResult, "RaGetAsyncReqResult", "ra_get_async_req_result");

    runtimeHandle_ = dlopen("libruntime.so", RTLD_NOW);
    LoadOptionalSymbol(runtimeHandle_, RtGetDevicePhyIdByIndex, "rtGetDevicePhyIdByIndex", nullptr);
    LoadOptionalSymbol(runtimeHandle_, RtOpenNetService, "rtOpenNetService", nullptr);
    LoadOptionalSymbol(runtimeHandle_, RtCloseNetService, "rtCloseNetService", nullptr);

    loaded_ = true;
    if (report != nullptr) {
        report->loaded = true;
        report->message = "ok";
    }
    return TILEXR_SUCCESS;
}

int TileXRCcuHccpLoader::LoadEndpointRouteProviderFromEnv(TileXRCcuHccpLoaderReport* report)
{
    ResetReport(report);
    if (CollectLocalEndpointRoute != nullptr) {
        if (report != nullptr) {
            report->endpointRouteProviderLoaded = true;
            report->message = "ok";
        }
        return TILEXR_SUCCESS;
    }

    const char* providerPath = std::getenv("TILEXR_CCU_ENDPOINT_ROUTE_PROVIDER");
    if (providerPath == nullptr || providerPath[0] == '\0') {
        return Fail(report, "direct CCU endpoint route provider is not configured");
    }
    if (report != nullptr) {
        report->endpointRouteProviderConfigured = true;
    }

    endpointRouteProviderHandle_ = dlopen(providerPath, RTLD_NOW);
    if (endpointRouteProviderHandle_ == nullptr) {
        return Fail(report, std::string("failed to load direct CCU endpoint route provider: ") + dlerror());
    }

    if (!LoadSymbol(
            endpointRouteProviderHandle_,
            CollectLocalEndpointRoute,
            "TileXRCcuCollectLocalEndpointRoute",
            "tilexr_ccu_collect_local_endpoint_route")) {
        dlclose(endpointRouteProviderHandle_);
        endpointRouteProviderHandle_ = nullptr;
        return Fail(report, "missing TileXRCcuCollectLocalEndpointRoute in direct CCU endpoint route provider");
    }

    if (report != nullptr) {
        report->endpointRouteProviderLoaded = true;
        report->message = "ok";
    }
    return TILEXR_SUCCESS;
}

void TileXRCcuHccpLoader::Unload()
{
    ReleaseCcuTlv();
    ReleaseRaHdc();
    RaCustomChannel = nullptr;
    RtGetDevicePhyIdByIndex = nullptr;
    RtOpenNetService = nullptr;
    RtCloseNetService = nullptr;
    RaInit = nullptr;
    RaDeinit = nullptr;
    RaTlvInit = nullptr;
    RaTlvRequest = nullptr;
    RaTlvDeinit = nullptr;
    RaGetDevEidInfoNum = nullptr;
    RaGetDevEidInfoList = nullptr;
    RaCtxInit = nullptr;
    RaCtxDeinit = nullptr;
    RaCtxTokenIdAlloc = nullptr;
    RaCtxTokenIdFree = nullptr;
    RaCtxLmemRegister = nullptr;
    RaCtxLmemUnregister = nullptr;
    RaGetSecRandom = nullptr;
    RaCtxChanCreate = nullptr;
    RaCtxChanDestroy = nullptr;
    RaCtxCqCreate = nullptr;
    RaCtxCqDestroy = nullptr;
    RaCtxQpCreate = nullptr;
    RaCtxQpDestroy = nullptr;
    RaCtxQpImport = nullptr;
    RaCtxQpUnimport = nullptr;
    RaCtxQpBind = nullptr;
    RaCtxQpUnbind = nullptr;
    RaGetTpInfoListAsync = nullptr;
    RaGetAsyncReqResult = nullptr;
    CollectLocalEndpointRoute = nullptr;
    loaded_ = false;
    if (endpointRouteProviderHandle_ != nullptr) {
        dlclose(endpointRouteProviderHandle_);
        endpointRouteProviderHandle_ = nullptr;
    }
    if (runtimeHandle_ != nullptr) {
        dlclose(runtimeHandle_);
        runtimeHandle_ = nullptr;
    }
    if (raHandle_ != nullptr) {
        dlclose(raHandle_);
        raHandle_ = nullptr;
    }
}

bool TileXRCcuHccpLoader::IsLoaded() const
{
    return loaded_;
}

int TileXRCcuHccpLoader::ResolveDevicePhyId(
    uint32_t logicDevId,
    uint32_t* phyId,
    TileXRCcuHccpLoaderReport* report) const
{
    if (report != nullptr) {
        report->logicDevId = logicDevId;
        report->runtimePhyIdRet = 0;
    }
    if (phyId == nullptr) {
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    if (!loaded_) {
        return TILEXR_ERROR_NOT_INITIALIZED;
    }
    if (RtGetDevicePhyIdByIndex == nullptr) {
        if (EnvFlag("TILEXR_CCU_DIRECT_ALLOW_LOGIC_PHY_FALLBACK")) {
            *phyId = logicDevId;
            if (report != nullptr) {
                report->devicePhyId = *phyId;
                report->message = "rtGetDevicePhyIdByIndex missing, using logic device id fallback";
            }
            return TILEXR_SUCCESS;
        }
        return FailWithCode(report, "missing rtGetDevicePhyIdByIndex in libruntime.so", TILEXR_ERROR_NOT_FOUND);
    }
    uint32_t resolvedPhyId = 0;
    const int ret = RtGetDevicePhyIdByIndex(logicDevId, &resolvedPhyId);
    if (report != nullptr) {
        report->runtimePhyIdRet = ret;
    }
    if (ret != 0) {
        return FailWithCode(report, "rtGetDevicePhyIdByIndex failed", TILEXR_ERROR_MKIRT);
    }
    *phyId = resolvedPhyId;
    if (report != nullptr) {
        report->devicePhyId = *phyId;
        report->message = "ok";
    }
    return TILEXR_SUCCESS;
}

int TileXRCcuHccpLoader::InitRaHdc(
    uint32_t devicePhyId,
    int hdcType,
    bool enableHdcAsync,
    TileXRCcuHccpLoaderReport* report)
{
    if (report != nullptr) {
        report->devicePhyId = devicePhyId;
        report->hdcType = hdcType;
    }
    if (!loaded_) {
        return FailWithCode(report, "CCU HCCP loader is not initialized for RA init", TILEXR_ERROR_NOT_INITIALIZED);
    }
    if (RaInit == nullptr || RaDeinit == nullptr) {
        return FailWithCode(report, "missing RaInit/RaDeinit in libra.so", TILEXR_ERROR_NOT_FOUND);
    }
    if (raHdcInitialized_) {
        std::lock_guard<std::mutex> lock(g_raHdcMtx);
        const auto it = g_raHdcRefs.find(RaHdcKey(raInitConfig_.phyId, raInitConfig_.hdcType));
        if (report != nullptr) {
            report->raInitialized = true;
            report->raInitRefCount = it == g_raHdcRefs.end() ? 0U : it->second;
            report->message = "ok";
        }
        return TILEXR_SUCCESS;
    }

    TileXRCcuRaInitConfig config {};
    config.phyId = devicePhyId;
    config.nicPosition = TILEXR_CCU_NETWORK_OFFLINE;
    config.hdcType = hdcType;
    config.enableHdcAsync = enableHdcAsync;

    const RaHdcKey key(config.phyId, config.hdcType);
    std::lock_guard<std::mutex> lock(g_raHdcMtx);
    int ret = AcquireNetServiceLocked(hdcType, report);
    if (ret != TILEXR_SUCCESS) {
        return ret;
    }
    uint32_t& refCount = g_raHdcRefs[key];
    if (refCount == 0) {
        ret = RaInit(&config);
        if (report != nullptr) {
            report->raInitRet = ret;
        }
        if (ret != 0) {
            g_raHdcRefs.erase(key);
            ReleaseNetServiceLocked(nullptr);
            std::ostringstream message;
            message << "RaInit failed ret=" << ret << ": " << RaConfigText(config);
            return FailWithCode(report, message.str(), TILEXR_ERROR_MKIRT);
        }
    }
    ++refCount;
    raInitConfig_ = config;
    raHdcInitialized_ = true;
    if (report != nullptr) {
        report->raInitialized = true;
        report->raInitRefCount = refCount;
        report->message = "ok";
    }
    return TILEXR_SUCCESS;
}

int TileXRCcuHccpLoader::InitCcuTlv(uint32_t devicePhyId, TileXRCcuHccpLoaderReport* report)
{
    if (report != nullptr) {
        report->devicePhyId = devicePhyId;
    }
    if (!loaded_) {
        return FailWithCode(report, "CCU HCCP loader is not initialized for TLV init",
            TILEXR_ERROR_NOT_INITIALIZED);
    }
    if (RaTlvInit == nullptr || RaTlvRequest == nullptr || RaTlvDeinit == nullptr) {
        return FailWithCode(report, "missing RaTlvInit/RaTlvRequest/RaTlvDeinit in libra.so",
            TILEXR_ERROR_NOT_FOUND);
    }
    if (ccuTlvInitialized_) {
        std::lock_guard<std::mutex> lock(g_ccuTlvMtx);
        const auto it = g_ccuTlvSessions.find(ccuTlvDevicePhyId_);
        if (report != nullptr) {
            report->ccuTlvInitialized = true;
            report->ccuTlvRefCount = it == g_ccuTlvSessions.end() ? 0U : it->second.refs;
            report->ccuTlvBufferSize = it == g_ccuTlvSessions.end() ? 0U : it->second.bufferSize;
            report->message = "ok";
        }
        return TILEXR_SUCCESS;
    }

    std::lock_guard<std::mutex> lock(g_ccuTlvMtx);
    CcuTlvSession& session = g_ccuTlvSessions[devicePhyId];
    if (session.refs == 0) {
        TileXRCcuTlvInitInfo initInfo {};
        initInfo.version = TILEXR_CCU_TLV_VERSION;
        initInfo.phyId = devicePhyId;
        initInfo.nicPosition = TILEXR_CCU_NETWORK_OFFLINE;
        uint32_t bufferSize = 0;
        void* tlvHandle = nullptr;
        int ret = RaTlvInit(&initInfo, &bufferSize, &tlvHandle);
        if (report != nullptr) {
            report->raTlvInitRet = ret;
            report->ccuTlvBufferSize = bufferSize;
        }
        if (ret != 0 || tlvHandle == nullptr) {
            g_ccuTlvSessions.erase(devicePhyId);
            std::ostringstream message;
            message << "RaTlvInit failed ret=" << ret
                    << ": phyId=" << initInfo.phyId
                    << " nicPosition=" << initInfo.nicPosition
                    << " version=" << initInfo.version;
            return FailWithCode(report, message.str(), TILEXR_ERROR_MKIRT);
        }

        TileXRCcuTlvMsg sendMsg {};
        TileXRCcuTlvMsg recvMsg {};
        sendMsg.type = TILEXR_CCU_TLV_MSG_TYPE_CCU_INIT;
        ret = RaTlvRequest(tlvHandle, TILEXR_CCU_TLV_MODULE_TYPE_CCU, &sendMsg, &recvMsg);
        if (report != nullptr) {
            report->raTlvRequestRet = ret;
        }
        if (ret != 0) {
            (void)RaTlvDeinit(tlvHandle);
            g_ccuTlvSessions.erase(devicePhyId);
            std::ostringstream message;
            message << "RaTlvRequest CCU_INIT failed ret=" << ret
                    << ": phyId=" << devicePhyId
                    << " moduleType=" << TILEXR_CCU_TLV_MODULE_TYPE_CCU
                    << " msgType=" << TILEXR_CCU_TLV_MSG_TYPE_CCU_INIT;
            return FailWithCode(report, message.str(), TILEXR_ERROR_MKIRT);
        }
        session.handle = tlvHandle;
        session.bufferSize = bufferSize;
    }

    ++session.refs;
    ccuTlvDevicePhyId_ = devicePhyId;
    ccuTlvInitialized_ = true;
    if (report != nullptr) {
        report->ccuTlvInitialized = true;
        report->ccuTlvRefCount = session.refs;
        report->ccuTlvBufferSize = session.bufferSize;
        report->message = "ok";
    }
    return TILEXR_SUCCESS;
}

int TileXRCcuHccpLoader::AcquireNetServiceLocked(int hdcType, TileXRCcuHccpLoaderReport* report)
{
    if (RtOpenNetService == nullptr || RtCloseNetService == nullptr) {
        return FailWithCode(report,
            "missing rtOpenNetService/rtCloseNetService in libruntime.so",
            TILEXR_ERROR_NOT_FOUND);
    }
    if (g_netServiceRefs > 0) {
        if (g_netServiceHdcType != hdcType) {
            std::ostringstream message;
            message << "runtime net service already opened for hdcType=" << g_netServiceHdcType
                    << ", requested hdcType=" << hdcType;
            return FailWithCode(report, message.str(), TILEXR_ERROR_PARA_CHECK_FAIL);
        }
        ++g_netServiceRefs;
        if (report != nullptr) {
            report->netServiceRefCount = g_netServiceRefs;
        }
        return TILEXR_SUCCESS;
    }

    std::string extParamText("--hdcType=" + std::to_string(hdcType));
    TileXRCcuRtProcExtParam extParam {};
    extParam.paramInfo = extParamText.c_str();
    extParam.paramLen = extParamText.size();
    TileXRCcuRtNetServiceOpenArgs openArgs {};
    openArgs.extParamList = &extParam;
    openArgs.extParamCnt = 1;
    const int ret = RtOpenNetService(&openArgs);
    if (report != nullptr) {
        report->rtOpenNetServiceRet = ret;
    }
    if (ret != 0) {
        std::ostringstream message;
        message << "rtOpenNetService failed ret=" << ret << ": " << extParamText;
        return FailWithCode(report, message.str(), TILEXR_ERROR_MKIRT);
    }

    g_netServiceHdcType = hdcType;
    g_netServiceRefs = 1;
    if (report != nullptr) {
        report->netServiceRefCount = g_netServiceRefs;
    }
    return TILEXR_SUCCESS;
}

void TileXRCcuHccpLoader::ReleaseNetServiceLocked(TileXRCcuHccpLoaderReport* report)
{
    if (g_netServiceRefs == 0) {
        return;
    }
    if (g_netServiceRefs > 1U) {
        --g_netServiceRefs;
        if (report != nullptr) {
            report->netServiceRefCount = g_netServiceRefs;
        }
        return;
    }

    int ret = 0;
    if (RtCloseNetService != nullptr) {
        ret = RtCloseNetService();
    }
    if (report != nullptr) {
        report->rtCloseNetServiceRet = ret;
        report->netServiceRefCount = 0;
    }
    g_netServiceRefs = 0;
    g_netServiceHdcType = 0;
}

void TileXRCcuHccpLoader::ReleaseCcuTlv()
{
    if (!ccuTlvInitialized_) {
        return;
    }
    std::lock_guard<std::mutex> lock(g_ccuTlvMtx);
    auto it = g_ccuTlvSessions.find(ccuTlvDevicePhyId_);
    if (it != g_ccuTlvSessions.end() && it->second.refs > 1U) {
        --it->second.refs;
        ccuTlvInitialized_ = false;
        ccuTlvDevicePhyId_ = 0;
        return;
    }
    if (it != g_ccuTlvSessions.end()) {
        if (RaTlvRequest != nullptr && it->second.handle != nullptr) {
            TileXRCcuTlvMsg sendMsg {};
            TileXRCcuTlvMsg recvMsg {};
            sendMsg.type = TILEXR_CCU_TLV_MSG_TYPE_CCU_UNINIT;
            (void)RaTlvRequest(it->second.handle, TILEXR_CCU_TLV_MODULE_TYPE_CCU, &sendMsg, &recvMsg);
        }
        if (RaTlvDeinit != nullptr && it->second.handle != nullptr) {
            (void)RaTlvDeinit(it->second.handle);
        }
        g_ccuTlvSessions.erase(it);
    }
    ccuTlvInitialized_ = false;
    ccuTlvDevicePhyId_ = 0;
}

void TileXRCcuHccpLoader::ReleaseRaHdc()
{
    if (!raHdcInitialized_) {
        return;
    }
    const RaHdcKey key(raInitConfig_.phyId, raInitConfig_.hdcType);
    std::lock_guard<std::mutex> lock(g_raHdcMtx);
    auto it = g_raHdcRefs.find(key);
    if (it != g_raHdcRefs.end() && it->second > 1U) {
        --it->second;
        ReleaseNetServiceLocked(nullptr);
        raHdcInitialized_ = false;
        raInitConfig_ = TileXRCcuRaInitConfig {};
        return;
    }
    if (RaDeinit != nullptr) {
        (void)RaDeinit(&raInitConfig_);
    }
    if (it != g_raHdcRefs.end()) {
        g_raHdcRefs.erase(it);
    }
    ReleaseNetServiceLocked(nullptr);
    raHdcInitialized_ = false;
    raInitConfig_ = TileXRCcuRaInitConfig {};
}

} // namespace TileXR
