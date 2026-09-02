/*
 * Copyright (c) 2026 TileXR Project
 * Licensed under the Apache License, Version 2.0
 */

#include "ccu/tilexr_ccu_specs.h"

#include <limits>

namespace TileXR {
namespace {

constexpr uint32_t TILEXR_CCU_MOVE_16_BITS = 16;
constexpr uint32_t TILEXR_CCU_MOVE_24_BITS = 24;

void ResetReport(TileXRCcuSpecsReport* report)
{
    if (report == nullptr) {
        return;
    }
    *report = TileXRCcuSpecsReport{};
}

int Fail(TileXRCcuSpecsReport* report, const std::string& message)
{
    if (report != nullptr) {
        report->message = message;
    }
    return TILEXR_ERROR_PARA_CHECK_FAIL;
}

uint32_t Low16PlusOne(uint32_t value)
{
    return (value & 0x0000FFFFU) + 1U;
}

uint32_t High16PlusOne(uint32_t value)
{
    return ((value >> TILEXR_CCU_MOVE_16_BITS) & 0x0000FFFFU) + 1U;
}

bool FitsU16(uint32_t value)
{
    return value <= std::numeric_limits<uint16_t>::max();
}

bool WindowOverflows(uint16_t start, uint32_t count)
{
    return count == 0 || count > std::numeric_limits<uint16_t>::max() ||
        static_cast<uint32_t>(start) + count >
            static_cast<uint32_t>(std::numeric_limits<uint16_t>::max()) + 1U;
}

void FillReport(const TileXRCcuSpecInfo& info, TileXRCcuSpecsReport* report)
{
    if (report == nullptr) {
        return;
    }
    report->instructionNum = info.instructionNum;
    report->xnNum = info.xnNum;
    report->ckeNum = info.ckeNum;
    report->channelNum = info.channelNum;
    report->missionNum = info.missionNum;
    report->message = "ok";
}

} // namespace

int TileXRCcuDecodeBasicInfo(
    const TileXRCcuBasicInfo& basicInfo,
    TileXRCcuSpecInfo* specInfo,
    TileXRCcuSpecsReport* report)
{
    ResetReport(report);
    if (specInfo == nullptr) {
        return Fail(report, "missing output CCU spec info");
    }
    *specInfo = TileXRCcuSpecInfo{};

    if (basicInfo.missionKey == 0) {
        return Fail(report, "missing CCU mission key in basic info");
    }
    if (basicInfo.resourceAddr == 0) {
        return Fail(report, "missing CCU resource address in basic info");
    }
    if (basicInfo.resourceAddr > std::numeric_limits<uint64_t>::max() - TILEXR_CCU_V1_XN_RESOURCE_OFFSET) {
        return Fail(report, "CCU XN base address overflows");
    }

    TileXRCcuSpecInfo decoded;
    decoded.dieId = basicInfo.dieId;
    decoded.msId = basicInfo.msId;
    decoded.missionKey = basicInfo.missionKey;
    decoded.resourceAddr = basicInfo.resourceAddr;
    decoded.xnBaseAddr = basicInfo.resourceAddr + TILEXR_CCU_V1_XN_RESOURCE_OFFSET;

    decoded.instructionNum = Low16PlusOne(basicInfo.caps.cap0);
    decoded.xnNum = High16PlusOne(basicInfo.caps.cap1);
    decoded.gsaNum = Low16PlusOne(basicInfo.caps.cap1);
    decoded.msNum = High16PlusOne(basicInfo.caps.cap2);
    decoded.ckeNum = Low16PlusOne(basicInfo.caps.cap2);
    decoded.jettyNum = High16PlusOne(basicInfo.caps.cap3);
    decoded.channelNum = Low16PlusOne(basicInfo.caps.cap3);
    decoded.pfeNum = (basicInfo.caps.cap4 & 0x000000FFU) + 1U;
    decoded.missionNum = ((basicInfo.caps.cap0 >> TILEXR_CCU_MOVE_16_BITS) & 0x000000FFU) + 1U;
    decoded.loopEngineNum = ((basicInfo.caps.cap0 >> TILEXR_CCU_MOVE_24_BITS) & 0x000000FFU) + 1U;

    *specInfo = decoded;
    FillReport(decoded, report);
    return TILEXR_SUCCESS;
}

int TileXRCcuBuildResourceSpec(
    const TileXRCcuSpecInfo& specInfo,
    uint16_t missionStartId,
    uint16_t instructionStartId,
    uint16_t xnStartId,
    uint16_t ckeStartId,
    uint16_t channelStartId,
    TileXRCcuResourceSpec* resourceSpec,
    TileXRCcuSpecsReport* report,
    uint16_t gsaStartId)
{
    ResetReport(report);
    if (resourceSpec == nullptr) {
        return Fail(report, "missing output CCU resource spec");
    }
    *resourceSpec = TileXRCcuResourceSpec{};

    if (specInfo.missionKey == 0) {
        return Fail(report, "missing CCU mission key in spec info");
    }
    if (!FitsU16(specInfo.missionNum) || !FitsU16(specInfo.instructionNum) || !FitsU16(specInfo.xnNum) ||
        !FitsU16(specInfo.gsaNum) ||
        !FitsU16(specInfo.ckeNum) || !FitsU16(specInfo.channelNum)) {
        return Fail(report, "CCU resource count exceeds TileXR resource window capacity");
    }
    if (WindowOverflows(missionStartId, specInfo.missionNum)) {
        return Fail(report, "mission resource window overflows");
    }
    if (WindowOverflows(instructionStartId, specInfo.instructionNum)) {
        return Fail(report, "instruction resource window overflows");
    }
    if (WindowOverflows(xnStartId, specInfo.xnNum)) {
        return Fail(report, "XN resource window overflows");
    }
    if (gsaStartId != 0 && WindowOverflows(gsaStartId, specInfo.gsaNum)) {
        return Fail(report, "GSA resource window overflows");
    }
    if (WindowOverflows(ckeStartId, specInfo.ckeNum)) {
        return Fail(report, "CKE resource window overflows");
    }
    if (WindowOverflows(channelStartId, specInfo.channelNum)) {
        return Fail(report, "channel resource window overflows");
    }

    TileXRCcuResourceSpec result;
    result.dieId = specInfo.dieId;
    result.missionKey = specInfo.missionKey;
    result.missionStartId = missionStartId;
    result.missionCount = static_cast<uint16_t>(specInfo.missionNum);
    result.instructionStartId = instructionStartId;
    result.instructionCount = static_cast<uint16_t>(specInfo.instructionNum);
    result.xnStartId = xnStartId;
    result.xnCount = static_cast<uint16_t>(specInfo.xnNum);
    result.gsaStartId = gsaStartId;
    result.gsaCount = gsaStartId == 0 ? 0 : static_cast<uint16_t>(specInfo.gsaNum);
    result.ckeStartId = ckeStartId;
    result.ckeCount = static_cast<uint16_t>(specInfo.ckeNum);
    result.channelStartId = channelStartId;
    result.channelCount = static_cast<uint16_t>(specInfo.channelNum);

    *resourceSpec = result;
    FillReport(specInfo, report);
    return TILEXR_SUCCESS;
}

} // namespace TileXR
