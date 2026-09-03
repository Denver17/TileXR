/*
 * Copyright (c) 2026 TileXR Project
 * Licensed under the Apache License, Version 2.0
 */

#ifndef TILEXR_CCU_HCCP_LOADER_H
#define TILEXR_CCU_HCCP_LOADER_H

#include "ccu/tilexr_ccu_hccp_types.h"
#include "tilexr_types.h"

#include <string>

namespace TileXR {

struct TileXRCcuHccpLoaderReport {
    bool loaded = false;
    bool endpointRouteProviderConfigured = false;
    bool endpointRouteProviderLoaded = false;
    bool raInitialized = false;
    bool ccuTlvInitialized = false;
    uint32_t logicDevId = 0;
    uint32_t devicePhyId = 0;
    uint32_t raInitRefCount = 0;
    uint32_t netServiceRefCount = 0;
    uint32_t ccuTlvRefCount = 0;
    uint32_t ccuTlvBufferSize = 0;
    int hdcType = 0;
    int runtimePhyIdRet = 0;
    int rtOpenNetServiceRet = 0;
    int rtCloseNetServiceRet = 0;
    int raInitRet = 0;
    int raDeinitRet = 0;
    int raTlvInitRet = 0;
    int raTlvRequestRet = 0;
    int raTlvDeinitRet = 0;
    std::string message;
};

class TileXRCcuHccpLoader {
public:
    TileXRCcuHccpLoader() = default;
    ~TileXRCcuHccpLoader();
    TileXRCcuHccpLoader(const TileXRCcuHccpLoader&) = delete;
    TileXRCcuHccpLoader& operator=(const TileXRCcuHccpLoader&) = delete;

    int Load(TileXRCcuHccpLoaderReport* report);
    int LoadEndpointRouteProviderFromEnv(TileXRCcuHccpLoaderReport* report);
    int InitRaHdc(
        uint32_t devicePhyId,
        int hdcType,
        bool enableHdcAsync,
        TileXRCcuHccpLoaderReport* report);
    int InitCcuTlv(uint32_t devicePhyId, TileXRCcuHccpLoaderReport* report);
    void Unload();
    bool IsLoaded() const;
    int ResolveDevicePhyId(uint32_t logicDevId, uint32_t* phyId, TileXRCcuHccpLoaderReport* report = nullptr) const;

    TileXRCcuRaCustomChannelFunc RaCustomChannel = nullptr;
    TileXRCcuRtGetDevicePhyIdByIndexFunc RtGetDevicePhyIdByIndex = nullptr;
    TileXRCcuRtOpenNetServiceFunc RtOpenNetService = nullptr;
    TileXRCcuRtCloseNetServiceFunc RtCloseNetService = nullptr;
    TileXRCcuRaInitFunc RaInit = nullptr;
    TileXRCcuRaDeinitFunc RaDeinit = nullptr;
    TileXRCcuRaTlvInitFunc RaTlvInit = nullptr;
    TileXRCcuRaTlvRequestFunc RaTlvRequest = nullptr;
    TileXRCcuRaTlvDeinitFunc RaTlvDeinit = nullptr;
    TileXRCcuRaGetDevEidInfoNumFunc RaGetDevEidInfoNum = nullptr;
    TileXRCcuRaGetDevEidInfoListFunc RaGetDevEidInfoList = nullptr;
    TileXRCcuRaCtxInitFunc RaCtxInit = nullptr;
    TileXRCcuRaCtxDeinitFunc RaCtxDeinit = nullptr;
    TileXRCcuRaCtxTokenIdAllocFunc RaCtxTokenIdAlloc = nullptr;
    TileXRCcuRaCtxTokenIdFreeFunc RaCtxTokenIdFree = nullptr;
    TileXRCcuRaCtxLmemRegisterFunc RaCtxLmemRegister = nullptr;
    TileXRCcuRaCtxLmemUnregisterFunc RaCtxLmemUnregister = nullptr;
    TileXRCcuRaGetSecRandomFunc RaGetSecRandom = nullptr;
    TileXRCcuRaCtxChanCreateFunc RaCtxChanCreate = nullptr;
    TileXRCcuRaCtxChanDestroyFunc RaCtxChanDestroy = nullptr;
    TileXRCcuRaCtxCqCreateFunc RaCtxCqCreate = nullptr;
    TileXRCcuRaCtxCqDestroyFunc RaCtxCqDestroy = nullptr;
    TileXRCcuRaCtxQpCreateFunc RaCtxQpCreate = nullptr;
    TileXRCcuRaCtxQpDestroyFunc RaCtxQpDestroy = nullptr;
    TileXRCcuRaCtxQpImportFunc RaCtxQpImport = nullptr;
    TileXRCcuRaCtxQpUnimportFunc RaCtxQpUnimport = nullptr;
    TileXRCcuRaCtxQpBindFunc RaCtxQpBind = nullptr;
    TileXRCcuRaCtxQpUnbindFunc RaCtxQpUnbind = nullptr;
    TileXRCcuRaGetTpInfoListAsyncFunc RaGetTpInfoListAsync = nullptr;
    TileXRCcuRaGetAsyncReqResultFunc RaGetAsyncReqResult = nullptr;
    TileXRCcuEndpointRouteProviderFunc CollectLocalEndpointRoute = nullptr;

private:
    void ReleaseCcuTlv();
    void ReleaseRaHdc();
    int AcquireNetServiceLocked(int hdcType, TileXRCcuHccpLoaderReport* report);
    void ReleaseNetServiceLocked(TileXRCcuHccpLoaderReport* report = nullptr);

    void* raHandle_ = nullptr;
    void* runtimeHandle_ = nullptr;
    void* endpointRouteProviderHandle_ = nullptr;
    TileXRCcuRaInitConfig raInitConfig_ = {};
    uint32_t ccuTlvDevicePhyId_ = 0;
    bool raHdcInitialized_ = false;
    bool ccuTlvInitialized_ = false;
    bool loaded_ = false;
};

} // namespace TileXR

#endif // TILEXR_CCU_HCCP_LOADER_H
