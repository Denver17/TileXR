/*
 * Copyright (c) 2026 TileXR Project
 * Licensed under the Apache License, Version 2.0
 */

#ifndef TILEXR_CCU_REPOSITORY_H
#define TILEXR_CCU_REPOSITORY_H

#include "ccu/tilexr_ccu_driver_adapter.h"
#include "ccu/tilexr_ccu_producer_plan.h"

#include <cstdint>
#include <string>
#include <vector>

namespace TileXR {

struct TileXRCcuRepositoryImage {
    uint8_t dieId = 0;
    uint16_t repositoryStartId = 0;
    uint16_t repositoryCount = 0;
    uint16_t missionStartId = 0;
    uint16_t missionCount = 0;
    uint16_t missionOffset = 0;
    uint16_t sqeLoadOffset = 0;
    uint16_t sqeLoadCount = 0;
    uint16_t syncOffset = 0;
    uint16_t syncCount = 0;
    std::vector<TileXRCcuInstr> instructions;
};

struct TileXRCcuRepositoryReport {
    uint16_t repositoryCount = 0;
    uint16_t installedInstructionCount = 0;
    uint16_t sqeLoadOffset = 0;
    uint16_t syncOffset = 0;
    bool repositoryUploaded = false;
    bool repositoryInstalled = false;
    std::string message;
};

using TileXRCcuDeviceAllocFn = int (*)(uint64_t bytes, void** devicePtr, void* userData);
using TileXRCcuCopyHostToDeviceFn = int (*)(
    void* deviceDst,
    uint64_t deviceDstBytes,
    const void* hostSrc,
    uint64_t bytes,
    void* userData);
using TileXRCcuCopyDeviceToHostFn = int (*)(
    void* hostDst,
    uint64_t hostDstBytes,
    const void* deviceSrc,
    uint64_t bytes,
    void* userData);
using TileXRCcuDeviceFreeFn = int (*)(void* devicePtr, void* userData);

struct TileXRCcuDeviceMemoryOps {
    TileXRCcuDeviceAllocFn alloc = nullptr;
    TileXRCcuCopyHostToDeviceFn copyHostToDevice = nullptr;
    TileXRCcuDeviceFreeFn free = nullptr;
    TileXRCcuCopyDeviceToHostFn copyDeviceToHost = nullptr;
};

enum class TileXRCcuRepositoryMemoryAllocMode : uint8_t {
    Acl = 0,
    AclModule3 = 1,
    RtHbm = 2,
};

struct TileXRCcuRepositoryInstallReceipt {
    uint8_t dieId = 0;
    uint16_t instructionStartId = 0;
    uint16_t instructionCount = 0;
    uint64_t instructionBytes = 0;
    uint64_t deviceInstructionAddr = 0;
    void* deviceInstructionPtr = nullptr;
    bool uploaded = false;
    bool installed = false;
};

enum class TileXRCcuRepositoryInstallWindow : uint8_t {
    Mission = 0,
    FullRepository = 1,
};

enum class TileXRCcuRepositoryInstallDataLenMode : uint8_t {
    InstructionBytes = 0,
    DescriptorBytes = 1,
};

struct TileXRCcuRepositoryInstallOptions {
    TileXRCcuRepositoryInstallWindow window = TileXRCcuRepositoryInstallWindow::Mission;
    TileXRCcuRepositoryInstallDataLenMode dataLenMode = TileXRCcuRepositoryInstallDataLenMode::InstructionBytes;
};

TileXRCcuDeviceMemoryOps TileXRCcuMakeAclDeviceMemoryOps();
TileXRCcuDeviceMemoryOps TileXRCcuMakeAclModule3DeviceMemoryOps();
TileXRCcuDeviceMemoryOps TileXRCcuMakeRtHbmDeviceMemoryOps();
TileXRCcuDeviceMemoryOps TileXRCcuMakeRepositoryDeviceMemoryOps(TileXRCcuRepositoryMemoryAllocMode mode);

int TileXRCcuBuildRepositoryImage(
    const TileXRCcuProducerPlan& plan,
    const TileXRCcuProgram& program,
    TileXRCcuRepositoryImage* image,
    TileXRCcuRepositoryReport* report);

int TileXRCcuInstallRepositoryImage(
    const TileXRCcuRepositoryImage& image,
    const TileXRCcuDeviceMemoryOps& memoryOps,
    void* memoryUserData,
    const TileXRCcuDriverAdapter& adapter,
    TileXRCcuRepositoryInstallReceipt* receipt,
    TileXRCcuRepositoryReport* report);

int TileXRCcuInstallRepositoryImageWithOptions(
    const TileXRCcuRepositoryImage& image,
    const TileXRCcuRepositoryInstallOptions& options,
    const TileXRCcuDeviceMemoryOps& memoryOps,
    void* memoryUserData,
    const TileXRCcuDriverAdapter& adapter,
    TileXRCcuRepositoryInstallReceipt* receipt,
    TileXRCcuRepositoryReport* report);

int TileXRCcuReleaseRepositoryInstallReceipt(
    TileXRCcuRepositoryInstallReceipt& receipt,
    const TileXRCcuDeviceMemoryOps& memoryOps,
    void* memoryUserData,
    TileXRCcuRepositoryReport* report);

} // namespace TileXR

#endif // TILEXR_CCU_REPOSITORY_H
