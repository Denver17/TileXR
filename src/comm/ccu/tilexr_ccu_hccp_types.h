/*
 * Copyright (c) 2026 TileXR Project
 * Licensed under the Apache License, Version 2.0
 */

#ifndef TILEXR_CCU_HCCP_TYPES_H
#define TILEXR_CCU_HCCP_TYPES_H

#include "ccu/tilexr_ccu_abi_constants.h"

#include <cstdint>

namespace TileXR {

constexpr int TILEXR_CCU_NETWORK_OFFLINE = 1;
constexpr int TILEXR_CCU_HDC_SERVICE_TYPE_RDMA = 6;
constexpr int TILEXR_CCU_HDC_SERVICE_TYPE_RDMA_V2 = 18;
constexpr uint32_t TILEXR_CCU_CUSTOM_CHAN_DATA_MAX_SIZE = 2048;
constexpr int TILEXR_CCU_TLV_VERSION = 1;
constexpr uint32_t TILEXR_CCU_TLV_MODULE_TYPE_CCU = 1;
constexpr uint32_t TILEXR_CCU_TLV_MSG_TYPE_CCU_INIT = 0;
constexpr uint32_t TILEXR_CCU_TLV_MSG_TYPE_CCU_UNINIT = 1;
constexpr uint32_t TILEXR_CCU_HCCP_DEV_EID_INFO_NAME_BYTES = 64;
constexpr uint32_t TILEXR_CCU_HCCP_MEM_KEY_BYTES = 128;
constexpr uint32_t TILEXR_CCU_HCCP_TOKEN_POLICY_PLAIN_TEXT = 1;
constexpr uint32_t TILEXR_CCU_HCCP_MEM_SEG_ACCESS_READ = 1U << 1U;
constexpr uint32_t TILEXR_CCU_HCCP_MEM_SEG_ACCESS_WRITE = 1U << 2U;
constexpr uint32_t TILEXR_CCU_HCCP_MEM_SEG_ACCESS_ATOMIC = 1U << 3U;
constexpr uint32_t TILEXR_CCU_HCCP_MEM_SEG_ACCESS_DEFAULT =
    TILEXR_CCU_HCCP_MEM_SEG_ACCESS_READ |
    TILEXR_CCU_HCCP_MEM_SEG_ACCESS_WRITE |
    TILEXR_CCU_HCCP_MEM_SEG_ACCESS_ATOMIC;
constexpr uint32_t TILEXR_CCU_HCCP_QP_KEY_BYTES = 64;
constexpr uint32_t TILEXR_CCU_HCCP_CQ_DEPTH_DEFAULT = 16384;
constexpr uint32_t TILEXR_CCU_HCCP_RQ_DEPTH_DEFAULT = 256;
constexpr uint32_t TILEXR_CCU_HCCP_JETTY_MODE_CCU = 2;
constexpr uint32_t TILEXR_CCU_HCCP_TRANSPORT_MODE_RM = 1;
constexpr uint32_t TILEXR_CCU_HCCP_JETTY_IMPORT_MODE_EXP = 1;
constexpr uint32_t TILEXR_CCU_HCCP_JETTY_GRP_POLICY_RR = 0;
constexpr uint32_t TILEXR_CCU_HCCP_TARGET_TYPE_JETTY = 1;
constexpr uint32_t TILEXR_CCU_HCCP_TP_TYPE_RTP = 0;
constexpr uint32_t TILEXR_CCU_HCCP_TP_TYPE_CTP = 1;
constexpr uint8_t TILEXR_CCU_HCCP_RNR_RETRY_DEFAULT = 7;

struct TileXRCcuDataByte8 {
    char raw[8];
};

struct TileXRCcuDataByte32 {
    char raw[32];
};

struct TileXRCcuDataByte64 {
    char raw[64];
};

struct TileXRCcuCustomChannelCaps {
    uint32_t cap0;
    uint32_t cap1;
    uint32_t cap2;
    uint32_t cap3;
    uint32_t cap4;
};

struct TileXRCcuInstrInfo {
    uint64_t resourceAddr;
};

struct TileXRCcuDieInfo {
    uint32_t enableFlag;
};

struct TileXRCcuBaseInfoData {
    uint32_t msId;
    uint32_t tokenId;
    uint32_t tokenValue;
    uint32_t tokenValid;
    uint32_t missionKey;
    uint64_t resourceAddr;
    TileXRCcuCustomChannelCaps caps;
};

union TileXRCcuDataTypeUnion {
    TileXRCcuDataByte8 byte8;
    TileXRCcuDataByte32 byte32;
    TileXRCcuDataByte64 byte64;
    TileXRCcuBaseInfoData baseinfo;
    TileXRCcuInstrInfo insinfo;
    TileXRCcuDieInfo dieinfo;
};

struct TileXRCcuData {
    uint32_t udieIdx;
    uint32_t dataLen;
    uint32_t dataArraySize;
    TileXRCcuDataTypeUnion dataArray[8];
};

union TileXRCcuDataUnion {
    char raw[TILEXR_CCU_CUSTOM_CHAN_DATA_MAX_SIZE];
    TileXRCcuData dataInfo;
};

struct TileXRCcuCustomChannelIn {
    TileXRCcuDataUnion data;
    uint32_t offsetStartIdx;
    uint32_t op;
};

struct TileXRCcuCustomChannelOut {
    TileXRCcuDataUnion data;
    uint32_t offsetNextIdx;
    int opRet;
};

struct TileXRCcuRaInfo {
    int mode;
    uint32_t phyId;
};

union TileXRCcuHccpEid {
    uint8_t raw[TILEXR_CCU_EID_BYTES];
    struct {
        uint64_t reserved;
        uint32_t prefix;
        uint32_t addr;
    } in4;
    struct {
        uint64_t subnetPrefix;
        uint64_t interfaceId;
    } in6;
};

struct TileXRCcuHccpDevEidInfo {
    char name[TILEXR_CCU_HCCP_DEV_EID_INFO_NAME_BYTES];
    uint32_t type;
    uint32_t eidIndex;
    TileXRCcuHccpEid eid;
    uint32_t dieId;
    uint32_t chipId;
    uint32_t funcId;
    uint32_t resv;
};

struct TileXRCcuHccpCtxInitCfg {
    int mode;
    union {
        struct {
            bool disabledLiteThread;
        } rdma;
    };
};

struct TileXRCcuHccpCtxInitAttr {
    uint32_t phyId;
    union {
        uint8_t rdmaPad[24];
        struct {
            uint32_t eidIndex;
            TileXRCcuHccpEid eid;
        } ub;
    };
    uint32_t resv[16];
};

struct TileXRCcuHccpTokenId {
    uint32_t tokenId;
};

struct TileXRCcuHccpMemKey {
    uint8_t value[TILEXR_CCU_HCCP_MEM_KEY_BYTES];
    uint8_t size;
};

struct TileXRCcuHccpMemInfo {
    uint64_t addr;
    uint64_t size;
};

union TileXRCcuHccpRegSegFlag {
    struct {
        uint32_t tokenPolicy : 3;
        uint32_t cacheable : 1;
        uint32_t dsva : 1;
        uint32_t access : 6;
        uint32_t nonPin : 1;
        uint32_t userIova : 1;
        uint32_t tokenIdValid : 1;
        uint32_t reserved : 18;
    } bs;
    uint32_t value;
};

struct TileXRCcuHccpMemRegAttr {
    TileXRCcuHccpMemInfo mem;
    union {
        struct {
            int access;
        } rdma;
        struct {
            TileXRCcuHccpRegSegFlag flags;
            uint32_t tokenValue;
            void* tokenIdHandle;
        } ub;
    };
    uint32_t resv[8];
};

struct TileXRCcuHccpMemRegInfo {
    TileXRCcuHccpMemKey key;
    union {
        struct {
            uint32_t lkey;
        } rdma;
        struct {
            uint32_t tokenId;
            uint64_t targetSegHandle;
        } ub;
    };
    uint32_t resv[8];
};

struct TileXRCcuHccpMrRegInfo {
    TileXRCcuHccpMemRegAttr in;
    TileXRCcuHccpMemRegInfo out;
};

union TileXRCcuHccpDataPlaneCstmFlag {
    struct {
        uint32_t pollCqCstm : 1;
        uint32_t reserved : 31;
    } bs;
    uint32_t value;
};

struct TileXRCcuHccpChanInfo {
    struct {
        TileXRCcuHccpDataPlaneCstmFlag dataPlaneFlag;
    } in;
    struct {
        int fd;
    } out;
};

union TileXRCcuHccpJfcFlag {
    struct {
        uint32_t lockFree : 1;
        uint32_t jfcInline : 1;
        uint32_t reserved : 30;
    } bs;
    uint32_t value;
};

struct TileXRCcuHccpCqInfo {
    struct {
        void* chanHandle;
        uint32_t depth;
        union {
            struct {
                uint64_t cqContext;
                uint32_t mode;
                uint32_t compVector;
            } rdma;
            struct {
                uint64_t userCtx;
                int mode;
                uint32_t ceqn;
                TileXRCcuHccpJfcFlag flag;
                struct {
                    bool valid;
                    uint32_t cqeFlag;
                } ccuExCfg;
            } ub;
        };
    } in;
    struct {
        uint64_t va;
        uint32_t id;
        uint32_t cqeSize;
        uint64_t bufAddr;
        uint64_t swdbAddr;
    } out;
};

union TileXRCcuHccpJettyFlag {
    struct {
        uint32_t shareJfr : 1;
        uint32_t reserved : 31;
    } bs;
    uint32_t value;
};

union TileXRCcuHccpJfsFlag {
    struct {
        uint32_t lockFree : 1;
        uint32_t errorSuspend : 1;
        uint32_t outorderComp : 1;
        uint32_t orderType : 8;
        uint32_t multiPath : 1;
        uint32_t reserved : 20;
    } bs;
    uint32_t value;
};

union TileXRCcuHccpCstmJfsFlag {
    struct {
        uint32_t sqCstm : 1;
        uint32_t dbCstm : 1;
        uint32_t dbCtlCstm : 1;
        uint32_t reserved : 29;
    } bs;
    uint32_t value;
};

struct TileXRCcuHccpJettyQueCfgEx {
    uint32_t buffSize;
    uint64_t buffVa;
};

struct TileXRCcuHccpQpCreateAttr {
    void* scqHandle;
    void* rcqHandle;
    void* srqHandle;
    uint32_t sqDepth;
    uint32_t rqDepth;
    int transportMode;
    union {
        struct {
            uint32_t mode;
            uint32_t udpSport;
            uint8_t trafficClass;
            uint8_t sl;
            uint8_t timeout;
            uint8_t rnrRetry;
            uint8_t retryCnt;
        } rdma;
        struct {
            int mode;
            uint32_t jettyId;
            TileXRCcuHccpJettyFlag flag;
            TileXRCcuHccpJfsFlag jfsFlag;
            void* tokenIdHandle;
            uint32_t tokenValue;
            uint8_t priority;
            uint8_t rnrRetry;
            uint8_t errTimeout;
            union {
                struct {
                    TileXRCcuHccpJettyQueCfgEx sq;
                    bool piType;
                    TileXRCcuHccpCstmJfsFlag cstmFlag;
                    uint32_t sqebbNum;
                } extMode;
                struct {
                    bool lockFlag;
                    uint32_t sqeBufIdx;
                } taCacheMode;
            };
        } ub;
    };
    uint32_t resv[16];
};

struct TileXRCcuHccpQpKey {
    uint8_t value[TILEXR_CCU_HCCP_QP_KEY_BYTES];
    uint8_t size;
};

struct TileXRCcuHccpQpCreateInfo {
    TileXRCcuHccpQpKey key;
    union {
        struct {
            uint32_t qpn;
        } rdma;
        struct {
            uint32_t uasid;
            uint32_t id;
            uint64_t sqBuffVa;
            uint64_t wqebbSize;
            uint64_t dbAddr;
            uint32_t dbTokenId;
            uint64_t ciAddr;
        } ub;
    };
    uint64_t va;
    uint32_t resv[16];
};

union TileXRCcuHccpImportJettyFlag {
    struct {
        uint32_t tokenPolicy : 3;
        uint32_t orderType : 8;
        uint32_t shareTp : 1;
        uint32_t reserved : 20;
    } bs;
    uint32_t value;
};

struct TileXRCcuHccpJettyImportExpCfg {
    uint64_t tpHandle;
    uint64_t peerTpHandle;
    uint64_t tag;
    uint32_t txPsn;
    uint32_t rxPsn;
    uint32_t rsv[16];
};

struct TileXRCcuHccpQpImportInfo {
    struct {
        TileXRCcuHccpQpKey key;
        union {
            struct {
                int mode;
                uint32_t tokenValue;
                int policy;
                int type;
                TileXRCcuHccpImportJettyFlag flag;
                TileXRCcuHccpJettyImportExpCfg expImportCfg;
                uint32_t tpType;
            } ub;
        };
        uint32_t resv[7];
    } in;
    struct {
        union {
            struct {
                uint64_t tjettyHandle;
                uint32_t tpn;
            } ub;
        };
        uint32_t resv[8];
    } out;
};

union TileXRCcuHccpGetTpCfgFlag {
    struct {
        uint32_t ctp : 1;
        uint32_t rtp : 1;
        uint32_t utp : 1;
        uint32_t uboe : 1;
        uint32_t preDefined : 1;
        uint32_t dynamicDefined : 1;
        uint32_t reserved : 26;
    } bs;
    uint32_t value;
};

struct TileXRCcuHccpGetTpCfg {
    TileXRCcuHccpGetTpCfgFlag flag;
    int transMode;
    TileXRCcuHccpEid localEid;
    TileXRCcuHccpEid peerEid;
};

struct TileXRCcuHccpTpInfo {
    uint64_t tpHandle;
    uint32_t resv;
};

struct TileXRCcuRaInitConfig {
    uint32_t phyId;
    uint32_t nicPosition;
    int hdcType;
    bool enableHdcAsync;
};

struct TileXRCcuRtProcExtParam {
    const char* paramInfo;
    uint64_t paramLen;
};

struct TileXRCcuRtNetServiceOpenArgs {
    TileXRCcuRtProcExtParam* extParamList;
    uint64_t extParamCnt;
};

struct TileXRCcuTlvInitInfo {
    int version;
    uint32_t phyId;
    uint32_t nicPosition;
    uint32_t reserved[16];
};

struct TileXRCcuTlvMsg {
    uint32_t type;
    uint32_t length;
    char* data;
};

struct TileXRCcuEndpointRouteProviderResourceWindow {
    uint64_t addr = 0;
    uint64_t bytes = 0;
    uint32_t tokenId = 0;
    uint32_t rawTokenId = 0;
    uint32_t tokenValue = 0;
};

struct TileXRCcuEndpointRouteProviderRoute {
    uint8_t remoteEid[TILEXR_CCU_EID_BYTES] = {};
    uint32_t tpn = 0;
    uint64_t doorbellVa = 0;
    uint32_t doorbellTokenId = 0;
    uint32_t doorbellTokenValue = 0;
    uint32_t sqDepth = 0;
    bool endpointRouteVerified = false;
};

using TileXRCcuRaCustomChannelFunc = int (*)(
    TileXRCcuRaInfo info,
    TileXRCcuCustomChannelIn* in,
    TileXRCcuCustomChannelOut* out);

using TileXRCcuRtGetDevicePhyIdByIndexFunc = int (*)(uint32_t logicDevId, uint32_t* phyId);
using TileXRCcuRtOpenNetServiceFunc = int (*)(const TileXRCcuRtNetServiceOpenArgs* args);
using TileXRCcuRtCloseNetServiceFunc = int (*)();

using TileXRCcuRaInitFunc = int (*)(TileXRCcuRaInitConfig* config);
using TileXRCcuRaDeinitFunc = int (*)(TileXRCcuRaInitConfig* config);
using TileXRCcuRaTlvInitFunc = int (*)(TileXRCcuTlvInitInfo* initInfo, uint32_t* bufferSize, void** tlvHandle);
using TileXRCcuRaTlvRequestFunc = int (*)(
    void* tlvHandle,
    uint32_t moduleType,
    TileXRCcuTlvMsg* sendMsg,
    TileXRCcuTlvMsg* recvMsg);
using TileXRCcuRaTlvDeinitFunc = int (*)(void* tlvHandle);
using TileXRCcuRaGetDevEidInfoNumFunc = int (*)(TileXRCcuRaInfo info, uint32_t* num);
using TileXRCcuRaGetDevEidInfoListFunc = int (*)(
    TileXRCcuRaInfo info,
    TileXRCcuHccpDevEidInfo list[],
    uint32_t* num);
using TileXRCcuRaCtxInitFunc = int (*)(
    TileXRCcuHccpCtxInitCfg* cfg,
    TileXRCcuHccpCtxInitAttr* attr,
    void** ctx);
using TileXRCcuRaCtxDeinitFunc = int (*)(void* ctx);
using TileXRCcuRaCtxTokenIdAllocFunc = int (*)(
    void* ctx,
    TileXRCcuHccpTokenId* token,
    void** tokenHandle);
using TileXRCcuRaCtxTokenIdFreeFunc = int (*)(void* ctx, void* tokenHandle);
using TileXRCcuRaCtxLmemRegisterFunc = int (*)(
    void* ctx,
    TileXRCcuHccpMrRegInfo* mr,
    void** lmemHandle);
using TileXRCcuRaCtxLmemUnregisterFunc = int (*)(void* ctx, void* lmemHandle);
using TileXRCcuRaGetSecRandomFunc = int (*)(TileXRCcuRaInfo* info, uint32_t* value);
using TileXRCcuRaCtxChanCreateFunc = int (*)(void* ctx, TileXRCcuHccpChanInfo* info, void** chanHandle);
using TileXRCcuRaCtxChanDestroyFunc = int (*)(void* ctx, void* chanHandle);
using TileXRCcuRaCtxCqCreateFunc = int (*)(void* ctx, TileXRCcuHccpCqInfo* info, void** cqHandle);
using TileXRCcuRaCtxCqDestroyFunc = int (*)(void* ctx, void* cqHandle);
using TileXRCcuRaCtxQpCreateFunc = int (*)(
    void* ctx,
    TileXRCcuHccpQpCreateAttr* attr,
    TileXRCcuHccpQpCreateInfo* info,
    void** qpHandle);
using TileXRCcuRaCtxQpDestroyFunc = int (*)(void* qpHandle);
using TileXRCcuRaCtxQpImportFunc = int (*)(
    void* ctx,
    TileXRCcuHccpQpImportInfo* info,
    void** remoteQpHandle);
using TileXRCcuRaCtxQpUnimportFunc = int (*)(void* ctx, void* remoteQpHandle);
using TileXRCcuRaCtxQpBindFunc = int (*)(void* qpHandle, void* remoteQpHandle);
using TileXRCcuRaCtxQpUnbindFunc = int (*)(void* qpHandle);
using TileXRCcuRaGetTpInfoListAsyncFunc = int (*)(
    void* ctx,
    TileXRCcuHccpGetTpCfg* cfg,
    TileXRCcuHccpTpInfo infoList[],
    uint32_t* num,
    void** reqHandle);
using TileXRCcuRaGetAsyncReqResultFunc = int (*)(void* reqHandle, int* reqResult);

using TileXRCcuEndpointRouteProviderFunc = int (*)(
    uint32_t devicePhyId,
    const TileXRCcuEndpointRouteProviderResourceWindow* localResourceWindow,
    TileXRCcuEndpointRouteProviderRoute* route);

static_assert(sizeof(TileXRCcuCustomChannelIn::data.raw) == TILEXR_CCU_CUSTOM_CHAN_DATA_MAX_SIZE,
    "CCU custom channel input must match HCCP custom_chan_info_in data size");
static_assert(sizeof(TileXRCcuCustomChannelOut::data.raw) == TILEXR_CCU_CUSTOM_CHAN_DATA_MAX_SIZE,
    "CCU custom channel output must match HCCP custom_chan_info_out data size");

} // namespace TileXR

#endif // TILEXR_CCU_HCCP_TYPES_H
