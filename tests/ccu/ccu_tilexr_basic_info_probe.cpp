/*
 * Copyright (c) 2026 TileXR Project
 * Licensed under the Apache License, Version 2.0
 *
 * Probe TileXR-owned direct CCU runtime basic-info path.
 * This does not install repository state and does not submit CCU tasks.
 */

#include "ccu/tilexr_ccu_direct_runtime.h"
#include "ccu/tilexr_ccu_specs.h"

#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>

namespace {

uint32_t ParseUintArg(const char* value, uint32_t fallback)
{
    if (value == nullptr) {
        return fallback;
    }
    char* end = nullptr;
    const unsigned long parsed = std::strtoul(value, &end, 0);
    if (end == value || *end != '\0') {
        return fallback;
    }
    return static_cast<uint32_t>(parsed);
}

void PrintHex32(const char* label, uint32_t value)
{
    std::cout << label << "=0x" << std::hex << std::setw(8) << std::setfill('0') << value << std::dec;
}

void PrintHex64(const char* label, uint64_t value)
{
    std::cout << label << "=0x" << std::hex << std::setw(16) << std::setfill('0') << value << std::dec;
}

void PrintBasicInfo(
    uint32_t deviceLogicId,
    uint8_t dieId,
    const TileXR::TileXRCcuBasicInfo& basicInfo,
    const TileXR::TileXRCcuDriverAdapterReport& adapterReport,
    const TileXR::TileXRCcuSpecInfo& specInfo)
{
    std::cout << "tilexr_ccu_basic_info result"
              << " deviceLogicId=" << deviceLogicId
              << " devicePhyId=" << adapterReport.devicePhyId
              << " dieId=" << static_cast<uint32_t>(dieId)
              << " driverRet=" << adapterReport.driverRet
              << " opRet=" << adapterReport.opRet
              << " msId=" << basicInfo.msId
              << " tokenId=" << basicInfo.msidToken.tokenId
              << " tokenValue=" << basicInfo.msidToken.tokenValue
              << " tokenValid=" << (basicInfo.msidToken.valid ? 1 : 0)
              << " ";
    PrintHex32("missionKey", basicInfo.missionKey);
    std::cout << " ";
    PrintHex64("resourceAddr", basicInfo.resourceAddr);
    std::cout << " instructionNum=" << specInfo.instructionNum
              << " xnNum=" << specInfo.xnNum
              << " ckeNum=" << specInfo.ckeNum
              << " channelNum=" << specInfo.channelNum
              << " missionNum=" << specInfo.missionNum
              << std::endl;
}

} // namespace

int main(int argc, char** argv)
{
    const uint32_t deviceLogicId = ParseUintArg(argc > 1 ? argv[1] : nullptr, 0);
    const uint8_t dieId = static_cast<uint8_t>(ParseUintArg(argc > 2 ? argv[2] : nullptr, 0));

    TileXR::TileXRCcuDirectRuntime runtime;
    TileXR::TileXRCcuDirectRuntimeOptions options;
    options.devId = static_cast<int>(deviceLogicId);
    options.rank = 0;
    options.rankSize = 1;

    TileXR::TileXRCcuDirectRuntimeReport runtimeReport;
    int ret = runtime.Init(options, &runtimeReport);
    if (ret != TileXR::TILEXR_SUCCESS) {
        std::cerr << "failed to initialize TileXR CCU direct runtime: "
                  << runtimeReport.message
                  << " deviceLogicId=" << runtimeReport.logicDevId
                  << " devicePhyId=" << runtimeReport.devicePhyId
                  << " hdcType=" << runtimeReport.hdcType
                  << " raInitialized=" << (runtimeReport.raInitialized ? 1 : 0)
                  << std::endl;
        return 2;
    }
    std::cout << "tilexr_ccu_basic_info runtime"
              << " deviceLogicId=" << runtimeReport.logicDevId
              << " devicePhyId=" << runtimeReport.devicePhyId
              << " hdcType=" << runtimeReport.hdcType
              << " raInitialized=" << (runtimeReport.raInitialized ? 1 : 0)
              << " message=\"" << runtimeReport.message << "\""
              << std::endl;

    TileXR::TileXRCcuBasicInfo basicInfo;
    TileXR::TileXRCcuDriverAdapterReport adapterReport;
    ret = runtime.QueryBasicInfo(dieId, &basicInfo, &adapterReport);
    if (ret != TileXR::TILEXR_SUCCESS) {
        std::cerr << "failed to query CCU basic info: " << adapterReport.message
                  << " driverRet=" << adapterReport.driverRet
                  << " opRet=" << adapterReport.opRet << std::endl;
        return 3;
    }

    TileXR::TileXRCcuSpecInfo specInfo;
    TileXR::TileXRCcuSpecsReport specsReport;
    ret = TileXR::TileXRCcuDecodeBasicInfo(basicInfo, &specInfo, &specsReport);
    if (ret != TileXR::TILEXR_SUCCESS) {
        std::cerr << "failed to decode CCU basic info: " << specsReport.message << std::endl;
        return 4;
    }

    PrintBasicInfo(deviceLogicId, dieId, basicInfo, adapterReport, specInfo);
    return 0;
}
