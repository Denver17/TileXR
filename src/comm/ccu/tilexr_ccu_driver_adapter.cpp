/*
 * Copyright (c) 2026 TileXR Project
 * Licensed under the Apache License, Version 2.0
 */

#include "ccu/tilexr_ccu_driver_adapter.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <sstream>

namespace TileXR {
namespace {

void ResetReport(TileXRCcuDriverAdapterReport* report)
{
    if (report == nullptr) {
        return;
    }
    *report = TileXRCcuDriverAdapterReport{};
}

int Fail(TileXRCcuDriverAdapterReport* report, const std::string& message, int code = TILEXR_ERROR_PARA_CHECK_FAIL)
{
    if (report != nullptr) {
        report->message = message;
    }
    return code;
}

void InitRequest(uint8_t dieId, uint32_t opcode, TileXRCcuCustomChannelIn* in)
{
    std::memset(in, 0, sizeof(*in));
    in->op = opcode;
    in->offsetStartIdx = 0;
    in->data.dataInfo.udieIdx = dieId;
}

void FillCallReport(
    uint32_t devicePhyId,
    uint8_t dieId,
    uint32_t opcode,
    int driverRet,
    int opRet,
    const std::string& message,
    TileXRCcuDriverAdapterReport* report)
{
    if (report == nullptr) {
        return;
    }
    report->devicePhyId = devicePhyId;
    report->dieId = dieId;
    report->opcode = opcode;
    report->driverRet = driverRet;
    report->opRet = opRet;
    report->message = message;
}

std::string CcuCustomChannelFailureMessage(
    const char* prefix,
    uint32_t opcode,
    int driverRet,
    int opRet)
{
    std::ostringstream message;
    message << prefix
            << " op=" << opcode
            << " driverRet=" << driverRet
            << " opRet=" << opRet;
    return message.str();
}

template <typename Payload>
void CopyPayloadToSlot(const Payload& payload, TileXRCcuDataTypeUnion* slot)
{
    std::memcpy(slot, payload.raw, sizeof(payload.raw));
}

bool DirectTraceEnabled()
{
    const char* value = std::getenv("TILEXR_CCU_DIRECT_TRACE");
    return value != nullptr && value[0] != '\0' && value[0] != '0';
}

uint64_t LoadWord(const void* data, uint32_t offset, uint32_t bytes)
{
    uint64_t word = 0;
    if (offset >= bytes) {
        return word;
    }
    const uint32_t copyBytes = std::min<uint32_t>(sizeof(word), bytes - offset);
    std::memcpy(&word, static_cast<const uint8_t*>(data) + offset, copyBytes);
    return word;
}

void TraceWords(const char* label, const void* data, uint32_t bytes)
{
    const uint32_t wordCount = (bytes + 7U) / 8U;
    std::cerr << "TileXRDirectCcuTrace " << label << "Words=" << wordCount;
    for (uint32_t i = 0; i < wordCount; ++i) {
        std::cerr << " w" << i << "=" << std::hex << std::showbase
                  << LoadWord(data, i * 8U, bytes)
                  << std::dec << std::noshowbase;
    }
    std::cerr << "\n";
}

void TraceCustomChannelRequest(
    uint32_t devicePhyId,
    uint8_t dieId,
    uint32_t opcode,
    const TileXRCcuCustomChannelIn& in)
{
    if (!DirectTraceEnabled()) {
        return;
    }

    const uint32_t payloadBytes = std::min<uint32_t>(
        in.data.dataInfo.dataLen == 0 ? TILEXR_CCU_DATA_ARRAY_SLOT_BYTES : in.data.dataInfo.dataLen,
        sizeof(in.data.dataInfo.dataArray));
    std::cerr << "TileXRDirectCcuTrace customChannel"
              << " devicePhyId=" << devicePhyId
              << " op=" << opcode
              << " dieId=" << static_cast<uint32_t>(dieId)
              << " requestDieId=" << in.data.dataInfo.udieIdx
              << " offset=" << in.offsetStartIdx
              << " dataLen=" << in.data.dataInfo.dataLen
              << " arraySize=" << in.data.dataInfo.dataArraySize
              << " payloadWords=" << ((payloadBytes + 7U) / 8U)
              << "\n";
    TraceWords("customChannel.request", &in, std::min<uint32_t>(sizeof(in), 256U));
    TraceWords("customChannel.requestTrailer", &in.offsetStartIdx, sizeof(in.offsetStartIdx) + sizeof(in.op));
    TraceWords("customChannel.payload", in.data.dataInfo.dataArray, payloadBytes);
}

void TraceCustomChannelReturn(
    uint32_t devicePhyId,
    uint8_t dieId,
    uint32_t opcode,
    int driverRet,
    const TileXRCcuCustomChannelOut& out)
{
    if (!DirectTraceEnabled()) {
        return;
    }

    std::cerr << "TileXRDirectCcuTrace customChannel.return"
              << " devicePhyId=" << devicePhyId
              << " op=" << opcode
              << " dieId=" << static_cast<uint32_t>(dieId)
              << " driverRet=" << driverRet
              << " opRet=" << out.opRet
              << " offsetNext=" << out.offsetNextIdx
              << "\n";
    TraceWords("customChannel.response", &out, std::min<uint32_t>(sizeof(out), 256U));
    TraceWords("customChannel.responseTrailer", &out.offsetNextIdx, sizeof(out.offsetNextIdx) + sizeof(out.opRet));
}

} // namespace

int TileXRCcuDriverAdapter::Init(
    uint32_t devicePhyId,
    TileXRCcuCustomChannelFn customChannel,
    void* userData,
    TileXRCcuDriverAdapterReport* report)
{
    ResetReport(report);
    if (customChannel == nullptr) {
        initialized_ = false;
        return Fail(report, "missing CCU custom channel callback");
    }
    devicePhyId_ = devicePhyId;
    customChannel_ = customChannel;
    userData_ = userData;
    initialized_ = true;
    FillCallReport(devicePhyId_, 0, 0, 0, 0, "ok", report);
    return TILEXR_SUCCESS;
}

int TileXRCcuDriverAdapter::Call(
    uint8_t dieId,
    uint32_t opcode,
    TileXRCcuCustomChannelOut* out,
    TileXRCcuDriverAdapterReport* report) const
{
    TileXRCcuCustomChannelIn in;
    InitRequest(dieId, opcode, &in);
    return CallPrepared(dieId, opcode, in, out, report);
}

int TileXRCcuDriverAdapter::CallPrepared(
    uint8_t dieId,
    uint32_t opcode,
    const TileXRCcuCustomChannelIn& in,
    TileXRCcuCustomChannelOut* out,
    TileXRCcuDriverAdapterReport* report) const
{
    ResetReport(report);
    if (!initialized_ || customChannel_ == nullptr) {
        return Fail(report, "CCU driver adapter is not initialized");
    }
    if (out == nullptr) {
        return Fail(report, "missing CCU custom channel output");
    }

    std::memset(out, 0, sizeof(*out));
    TraceCustomChannelRequest(devicePhyId_, dieId, opcode, in);
    const int driverRet = customChannel_(devicePhyId_, in, out, userData_);
    TraceCustomChannelReturn(devicePhyId_, dieId, opcode, driverRet, *out);
    FillCallReport(devicePhyId_, dieId, opcode, driverRet, out->opRet, "ok", report);
    if (driverRet != 0) {
        return Fail(
            report,
            CcuCustomChannelFailureMessage("CCU custom channel call failed", opcode, driverRet, out->opRet),
            TILEXR_ERROR_MKIRT);
    }
    if (out->opRet != 0) {
        return Fail(
            report,
            CcuCustomChannelFailureMessage("CCU custom channel operation failed", opcode, driverRet, out->opRet),
            TILEXR_ERROR_MKIRT);
    }
    return TILEXR_SUCCESS;
}

int TileXRCcuDriverAdapter::GetBasicInfo(
    uint8_t dieId,
    TileXRCcuBasicInfo* basicInfo,
    TileXRCcuDriverAdapterReport* report) const
{
    if (basicInfo == nullptr) {
        ResetReport(report);
        return Fail(report, "missing output CCU basic info");
    }
    *basicInfo = TileXRCcuBasicInfo{};

    TileXRCcuCustomChannelOut out;
    const int ret = Call(dieId, TILEXR_CCU_U_OP_GET_BASIC_INFO, &out, report);
    if (ret != TILEXR_SUCCESS) {
        return ret;
    }

    const auto& raw = out.data.dataInfo.dataArray[0].baseinfo;
    basicInfo->dieId = dieId;
    basicInfo->msId = raw.msId;
    basicInfo->msidToken.tokenId = raw.tokenId;
    basicInfo->msidToken.tokenValue = raw.tokenValue;
    basicInfo->msidToken.valid = raw.tokenValid != 0;
    basicInfo->missionKey = raw.missionKey;
    basicInfo->resourceAddr = raw.resourceAddr;
    basicInfo->caps.cap0 = raw.caps.cap0;
    basicInfo->caps.cap1 = raw.caps.cap1;
    basicInfo->caps.cap2 = raw.caps.cap2;
    basicInfo->caps.cap3 = raw.caps.cap3;
    basicInfo->caps.cap4 = raw.caps.cap4;
    return TILEXR_SUCCESS;
}

int TileXRCcuDriverAdapter::GetDieEnabled(
    uint8_t dieId,
    bool* enabled,
    TileXRCcuDriverAdapterReport* report) const
{
    if (enabled == nullptr) {
        ResetReport(report);
        return Fail(report, "missing output CCU die enabled flag");
    }
    *enabled = false;

    TileXRCcuCustomChannelOut out;
    const int ret = Call(dieId, TILEXR_CCU_U_OP_GET_DIE_WORKING, &out, report);
    if (ret != TILEXR_SUCCESS) {
        return ret;
    }

    *enabled = out.data.dataInfo.dataArray[0].dieinfo.enableFlag == TILEXR_CCU_ENABLE_FLAG;
    return TILEXR_SUCCESS;
}

int TileXRCcuDriverAdapter::ReadInstructions(
    uint8_t dieId,
    uint16_t instructionStartId,
    void* instructions,
    uint32_t instructionCount,
    uint32_t instructionBytes,
    TileXRCcuDriverAdapterReport* report) const
{
    ResetReport(report);
    if (instructions == nullptr) {
        return Fail(report, "missing output CCU instruction readback buffer");
    }
    if (instructionCount == 0 || instructionCount > TILEXR_CCU_MAX_DATA_ARRAY_SIZE) {
        return Fail(report, "invalid CCU instruction readback count");
    }
    const uint32_t expectedBytes = instructionCount * TILEXR_CCU_INSTRUCTION_BYTES;
    if (instructionBytes != expectedBytes) {
        return Fail(report, "CCU instruction readback byte size mismatch");
    }

    TileXRCcuCustomChannelIn in;
    InitRequest(dieId, TILEXR_CCU_U_OP_GET_INSTRUCTION, &in);
    in.offsetStartIdx = instructionStartId;
    in.data.dataInfo.dataArraySize = instructionCount;
    in.data.dataInfo.dataLen = instructionBytes;

    TileXRCcuCustomChannelOut out;
    const int ret = CallPrepared(dieId, TILEXR_CCU_U_OP_GET_INSTRUCTION, in, &out, report);
    if (ret != TILEXR_SUCCESS) {
        return ret;
    }
    auto* dst = static_cast<uint8_t*>(instructions);
    for (uint32_t i = 0; i < instructionCount; ++i) {
        std::memcpy(
            dst + i * TILEXR_CCU_INSTRUCTION_BYTES,
            out.data.dataInfo.dataArray[i].byte32.raw,
            TILEXR_CCU_INSTRUCTION_BYTES);
    }
    return TILEXR_SUCCESS;
}

int TileXRCcuDriverAdapter::InstallInstructions(
    uint8_t dieId,
    uint16_t instructionStartId,
    uint16_t instructionCount,
    uint64_t deviceInstructionAddr,
    uint32_t instructionBytes,
    TileXRCcuDriverAdapterReport* report) const
{
    return InstallInstructionsWithDataLen(
        dieId,
        instructionStartId,
        instructionCount,
        deviceInstructionAddr,
        instructionBytes,
        instructionBytes,
        report);
}

int TileXRCcuDriverAdapter::InstallInstructionsWithDataLen(
    uint8_t dieId,
    uint16_t instructionStartId,
    uint16_t instructionCount,
    uint64_t deviceInstructionAddr,
    uint32_t instructionBytes,
    uint32_t customChannelDataLen,
    TileXRCcuDriverAdapterReport* report) const
{
    ResetReport(report);
    if (instructionCount == 0) {
        return Fail(report, "missing CCU instruction image");
    }
    if (deviceInstructionAddr == 0) {
        return Fail(report, "missing device CCU instruction image address");
    }
    const uint32_t expectedBytes = static_cast<uint32_t>(instructionCount) * TILEXR_CCU_INSTRUCTION_BYTES;
    if (instructionBytes == 0 || instructionBytes != expectedBytes) {
        return Fail(report, "CCU instruction image byte size mismatch");
    }
    if (customChannelDataLen == 0) {
        return Fail(report, "missing CCU instruction custom channel data length");
    }

    TileXRCcuCustomChannelIn in;
    InitRequest(dieId, TILEXR_CCU_U_OP_SET_INSTRUCTION, &in);
    in.offsetStartIdx = instructionStartId;
    in.data.dataInfo.dataArraySize = 1;
    in.data.dataInfo.dataLen = customChannelDataLen;
    in.data.dataInfo.dataArray[0].insinfo.resourceAddr = deviceInstructionAddr;

    TileXRCcuCustomChannelOut out;
    return CallPrepared(dieId, TILEXR_CCU_U_OP_SET_INSTRUCTION, in, &out, report);
}

int TileXRCcuDriverAdapter::InstallMsidToken(
    uint8_t dieId,
    uint32_t msId,
    uint32_t tokenId,
    uint32_t tokenValue,
    TileXRCcuDriverAdapterReport* report) const
{
    ResetReport(report);
    TileXRCcuCustomChannelIn in;
    InitRequest(dieId, TILEXR_CCU_U_OP_SET_MSID_TOKEN, &in);
    in.offsetStartIdx = 0;
    in.data.dataInfo.dataArray[0].baseinfo.msId = msId;
    in.data.dataInfo.dataArray[0].baseinfo.tokenId = tokenId;
    in.data.dataInfo.dataArray[0].baseinfo.tokenValue = tokenValue;

    TileXRCcuCustomChannelOut out;
    return CallPrepared(dieId, TILEXR_CCU_U_OP_SET_MSID_TOKEN, in, &out, report);
}

int TileXRCcuDriverAdapter::InstallPfeCtx(
    uint8_t dieId,
    uint32_t pfeOffset,
    const TileXRCcuPfeCtx& ctx,
    TileXRCcuDriverAdapterReport* report) const
{
    ResetReport(report);
    TileXRCcuCustomChannelIn in;
    InitRequest(dieId, TILEXR_CCU_U_OP_SET_PFE, &in);
    in.offsetStartIdx = pfeOffset;
    in.data.dataInfo.dataArraySize = 1;
    in.data.dataInfo.dataLen = TILEXR_CCU_PFE_CTX_BYTES;
    std::memcpy(&in.data.dataInfo.dataArray[0], ctx.raw, TILEXR_CCU_PFE_CTX_BYTES);

    TileXRCcuCustomChannelOut out;
    return CallPrepared(dieId, TILEXR_CCU_U_OP_SET_PFE, in, &out, report);
}

int TileXRCcuDriverAdapter::InstallJettyCtx(
    uint8_t dieId,
    uint16_t startJettyCtxId,
    const TileXRCcuLocalJettyCtxData* ctxs,
    uint32_t count,
    TileXRCcuDriverAdapterReport* report) const
{
    ResetReport(report);
    if (ctxs == nullptr) {
        return Fail(report, "missing CCU local jetty context payloads");
    }
    if (count == 0 || count > TILEXR_CCU_MAX_DATA_ARRAY_SIZE) {
        return Fail(report, "invalid CCU local jetty context count");
    }

    TileXRCcuCustomChannelIn in;
    InitRequest(dieId, TILEXR_CCU_U_OP_SET_JETTY_CTX, &in);
    in.offsetStartIdx = startJettyCtxId;
    in.data.dataInfo.dataArraySize = count;
    in.data.dataInfo.dataLen = count * TILEXR_CCU_LOCAL_JETTY_CTX_BYTES;
    for (uint32_t i = 0; i < count; ++i) {
        CopyPayloadToSlot(ctxs[i], &in.data.dataInfo.dataArray[i]);
    }

    TileXRCcuCustomChannelOut out;
    return CallPrepared(dieId, TILEXR_CCU_U_OP_SET_JETTY_CTX, in, &out, report);
}

int TileXRCcuDriverAdapter::InstallChannelCtxV1(
    uint8_t dieId,
    uint32_t channelId,
    const TileXRCcuChannelCtxDataV1& ctx,
    TileXRCcuDriverAdapterReport* report) const
{
    ResetReport(report);
    TileXRCcuCustomChannelIn in;
    InitRequest(dieId, TILEXR_CCU_U_OP_SET_CHANNEL, &in);
    in.offsetStartIdx = channelId;
    in.data.dataInfo.dataArraySize = 1;
    in.data.dataInfo.dataLen = TILEXR_CCU_CHANNEL_CTX_V1_BYTES;
    std::memcpy(&in.data.dataInfo.dataArray[0], ctx.raw, TILEXR_CCU_CHANNEL_CTX_V1_BYTES);

    TileXRCcuCustomChannelOut out;
    return CallPrepared(dieId, TILEXR_CCU_U_OP_SET_CHANNEL, in, &out, report);
}

int TileXRCcuDriverAdapter::ClearCkeRange(
    uint8_t dieId,
    uint32_t startCkeId,
    uint32_t count,
    TileXRCcuDriverAdapterReport* report) const
{
    ResetReport(report);
    if (count == 0) {
        return Fail(report, "missing CCU CKE range");
    }

    uint32_t remaining = count;
    uint32_t offset = startCkeId;
    while (remaining > 0) {
        const uint32_t batch = std::min(remaining, TILEXR_CCU_MAX_DATA_ARRAY_SIZE);
        TileXRCcuCustomChannelIn in;
        InitRequest(dieId, TILEXR_CCU_U_OP_SET_CKE, &in);
        in.offsetStartIdx = offset;
        in.data.dataInfo.dataArraySize = batch;
        in.data.dataInfo.dataLen = batch * TILEXR_CCU_CKE_SLOT_BYTES;

        TileXRCcuCustomChannelOut out;
        const int ret = CallPrepared(dieId, TILEXR_CCU_U_OP_SET_CKE, in, &out, report);
        if (ret != TILEXR_SUCCESS) {
            return ret;
        }
        remaining -= batch;
        offset += batch;
    }
    return TILEXR_SUCCESS;
}

int TileXRCcuDriverAdapter::InstallXnRange(
    uint8_t dieId,
    uint32_t startXnId,
    uint32_t count,
    TileXRCcuDriverAdapterReport* report) const
{
    ResetReport(report);
    if (count == 0) {
        return Fail(report, "missing CCU XN range");
    }

    uint32_t remaining = count;
    uint32_t offset = startXnId;
    while (remaining > 0) {
        const uint32_t batch = std::min(remaining, TILEXR_CCU_MAX_DATA_ARRAY_SIZE);
        TileXRCcuCustomChannelIn in;
        InitRequest(dieId, TILEXR_CCU_U_OP_SET_XN, &in);
        in.offsetStartIdx = offset;
        in.data.dataInfo.dataArraySize = batch;
        in.data.dataInfo.dataLen = batch * TILEXR_CCU_XN_SLOT_BYTES;

        TileXRCcuCustomChannelOut out;
        const int ret = CallPrepared(dieId, TILEXR_CCU_U_OP_SET_XN, in, &out, report);
        if (ret != TILEXR_SUCCESS) {
            return ret;
        }
        remaining -= batch;
        offset += batch;
    }
    return TILEXR_SUCCESS;
}

} // namespace TileXR
