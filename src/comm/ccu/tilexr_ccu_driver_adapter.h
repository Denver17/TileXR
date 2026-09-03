/*
 * Copyright (c) 2026 TileXR Project
 * Licensed under the Apache License, Version 2.0
 */

#ifndef TILEXR_CCU_DRIVER_ADAPTER_H
#define TILEXR_CCU_DRIVER_ADAPTER_H

#include "ccu/tilexr_ccu_hccp_types.h"
#include "ccu/tilexr_ccu_specs.h"

#include <cstdint>
#include <string>

namespace TileXR {

constexpr uint32_t TILEXR_CCU_U_OP_GET_BASIC_INFO = 11;
constexpr uint32_t TILEXR_CCU_U_OP_GET_DIE_WORKING = 15;
constexpr uint32_t TILEXR_CCU_U_OP_GET_INSTRUCTION = 201;
constexpr uint32_t TILEXR_CCU_U_OP_SET_MSID_TOKEN = 53;
constexpr uint32_t TILEXR_CCU_U_OP_SET_INSTRUCTION = 251;
constexpr uint32_t TILEXR_CCU_U_OP_SET_XN = 253;
constexpr uint32_t TILEXR_CCU_U_OP_SET_CKE = 254;
constexpr uint32_t TILEXR_CCU_U_OP_SET_PFE = 255;
constexpr uint32_t TILEXR_CCU_U_OP_SET_CHANNEL = 256;
constexpr uint32_t TILEXR_CCU_U_OP_SET_JETTY_CTX = 257;
constexpr uint32_t TILEXR_CCU_ENABLE_FLAG = 1;
constexpr uint32_t TILEXR_CCU_INSTRUCTION_BYTES = 32;
constexpr uint32_t TILEXR_CCU_DATA_ARRAY_SLOT_BYTES = 64;
constexpr uint32_t TILEXR_CCU_XN_SLOT_BYTES = 8;
constexpr uint32_t TILEXR_CCU_CKE_SLOT_BYTES = 8;
constexpr uint32_t TILEXR_CCU_PFE_CTX_BYTES = 8;
constexpr uint32_t TILEXR_CCU_LOCAL_JETTY_CTX_BYTES = 32;
constexpr uint32_t TILEXR_CCU_CHANNEL_CTX_V1_BYTES = 64;
constexpr uint32_t TILEXR_CCU_MAX_DATA_ARRAY_SIZE = 8;

struct TileXRCcuPfeCtx {
    uint8_t raw[TILEXR_CCU_PFE_CTX_BYTES];
};

struct TileXRCcuLocalJettyCtxData {
    uint8_t raw[TILEXR_CCU_LOCAL_JETTY_CTX_BYTES];
};

struct TileXRCcuChannelCtxDataV1 {
    uint8_t raw[TILEXR_CCU_CHANNEL_CTX_V1_BYTES];
};

struct TileXRCcuDriverAdapterReport {
    uint32_t devicePhyId = 0;
    uint8_t dieId = 0;
    uint32_t opcode = 0;
    int driverRet = 0;
    int opRet = 0;
    std::string message;
};

using TileXRCcuCustomChannelFn = int (*)(
    uint32_t devicePhyId,
    const TileXRCcuCustomChannelIn& in,
    TileXRCcuCustomChannelOut* out,
    void* userData);

class TileXRCcuDriverAdapter {
public:
    int Init(
        uint32_t devicePhyId,
        TileXRCcuCustomChannelFn customChannel,
        void* userData,
        TileXRCcuDriverAdapterReport* report);

    int GetBasicInfo(uint8_t dieId, TileXRCcuBasicInfo* basicInfo, TileXRCcuDriverAdapterReport* report) const;
    int GetDieEnabled(uint8_t dieId, bool* enabled, TileXRCcuDriverAdapterReport* report) const;
    int ReadInstructions(
        uint8_t dieId,
        uint16_t instructionStartId,
        void* instructions,
        uint32_t instructionCount,
        uint32_t instructionBytes,
        TileXRCcuDriverAdapterReport* report) const;
    int InstallInstructions(
        uint8_t dieId,
        uint16_t instructionStartId,
        uint16_t instructionCount,
        uint64_t deviceInstructionAddr,
        uint32_t instructionBytes,
        TileXRCcuDriverAdapterReport* report) const;
    int InstallInstructionsWithDataLen(
        uint8_t dieId,
        uint16_t instructionStartId,
        uint16_t instructionCount,
        uint64_t deviceInstructionAddr,
        uint32_t instructionBytes,
        uint32_t customChannelDataLen,
        TileXRCcuDriverAdapterReport* report) const;
    int InstallMsidToken(
        uint8_t dieId,
        uint32_t msId,
        uint32_t tokenId,
        uint32_t tokenValue,
        TileXRCcuDriverAdapterReport* report) const;
    int InstallPfeCtx(
        uint8_t dieId,
        uint32_t pfeOffset,
        const TileXRCcuPfeCtx& ctx,
        TileXRCcuDriverAdapterReport* report) const;
    int InstallJettyCtx(
        uint8_t dieId,
        uint16_t startJettyCtxId,
        const TileXRCcuLocalJettyCtxData* ctxs,
        uint32_t count,
        TileXRCcuDriverAdapterReport* report) const;
    int InstallChannelCtxV1(
        uint8_t dieId,
        uint32_t channelId,
        const TileXRCcuChannelCtxDataV1& ctx,
        TileXRCcuDriverAdapterReport* report) const;
    int ClearCkeRange(
        uint8_t dieId,
        uint32_t startCkeId,
        uint32_t count,
        TileXRCcuDriverAdapterReport* report) const;
    int InstallXnRange(
        uint8_t dieId,
        uint32_t startXnId,
        uint32_t count,
        TileXRCcuDriverAdapterReport* report) const;

private:
    int Call(uint8_t dieId, uint32_t opcode, TileXRCcuCustomChannelOut* out, TileXRCcuDriverAdapterReport* report)
        const;
    int CallPrepared(
        uint8_t dieId,
        uint32_t opcode,
        const TileXRCcuCustomChannelIn& in,
        TileXRCcuCustomChannelOut* out,
        TileXRCcuDriverAdapterReport* report) const;

    uint32_t devicePhyId_ = 0;
    TileXRCcuCustomChannelFn customChannel_ = nullptr;
    void* userData_ = nullptr;
    bool initialized_ = false;
};

} // namespace TileXR

#endif // TILEXR_CCU_DRIVER_ADAPTER_H
