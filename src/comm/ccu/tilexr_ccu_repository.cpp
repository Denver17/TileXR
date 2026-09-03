/*
 * Copyright (c) 2026 TileXR Project
 * Licensed under the Apache License, Version 2.0
 */

#include "ccu/tilexr_ccu_repository.h"

#if defined(__has_include)
#if __has_include(<acl/acl_rt.h>)
#define TILEXR_CCU_HAVE_ACL_RT_HEADER 1
#include <acl/acl_rt.h>
#else
#define TILEXR_CCU_HAVE_ACL_RT_HEADER 0
#endif
#else
#define TILEXR_CCU_HAVE_ACL_RT_HEADER 1
#include <acl/acl_rt.h>
#endif

#if defined(__has_include)
#if __has_include(<runtime/mem.h>)
#define TILEXR_CCU_HAVE_RUNTIME_MEM_HEADER 1
#include <runtime/mem.h>
#else
#define TILEXR_CCU_HAVE_RUNTIME_MEM_HEADER 0
#endif
#else
#define TILEXR_CCU_HAVE_RUNTIME_MEM_HEADER 1
#include <runtime/mem.h>
#endif

#include <algorithm>
#include <iomanip>
#include <limits>
#include <sstream>
#include <vector>

namespace TileXR {
namespace {

constexpr uint16_t TILEXR_CCU_ACL_MODULE3_ID = 3U;
constexpr uint16_t TILEXR_CCU_RT_HBM_MODULE_ID = 0U;

struct TileXRCcuUploadReadbackDiagnostic {
    bool attempted = false;
    bool ok = false;
    int ret = 0;
    uint64_t bytes = 0;
    uint64_t fnv1a64 = 0;
    uint32_t mismatchCount = 0;
    std::string firstInstructionWords;
    std::string lastInstructionWords;
};

void ResetReport(TileXRCcuRepositoryReport* report)
{
    if (report == nullptr) {
        return;
    }
    report->repositoryCount = 0;
    report->installedInstructionCount = 0;
    report->sqeLoadOffset = 0;
    report->syncOffset = 0;
    report->repositoryUploaded = false;
    report->repositoryInstalled = false;
    report->message.clear();
}

int Fail(
    TileXRCcuRepositoryReport* report,
    const std::string& message,
    int code = TILEXR_ERROR_PARA_CHECK_FAIL)
{
    if (report != nullptr) {
        report->message = message;
    }
    return code;
}

bool ContainsWindow(uint16_t outerStart, uint16_t outerCount, uint16_t innerStart, uint16_t innerCount)
{
    if (outerCount == 0 || innerCount == 0) {
        return false;
    }
    const uint32_t outerBegin = outerStart;
    const uint32_t outerEnd = outerBegin + outerCount;
    const uint32_t innerBegin = innerStart;
    const uint32_t innerEnd = innerBegin + innerCount;
    return innerBegin >= outerBegin && innerEnd <= outerEnd;
}

uint16_t OffsetFrom(uint16_t base, uint16_t id)
{
    return static_cast<uint16_t>(static_cast<uint32_t>(id) - base);
}

int ValidateProgramShape(
    const TileXRCcuProducerPlan& plan,
    const TileXRCcuProgram& program,
    TileXRCcuRepositoryReport* report)
{
    if (program.sync.empty()) {
        return Fail(report, "missing sync microcode for repository image");
    }
    if (program.sqeLoad.empty()) {
        if (plan.taskWindows.size() != 1) {
            return Fail(report, "pure sync repository image requires exactly one task window");
        }
        const auto& syncTask = plan.taskWindows[0];
        if (syncTask.instCnt < program.sync.size()) {
            return Fail(report, "sync task window is too small for generated sync microcode");
        }
        return TILEXR_SUCCESS;
    }
    if (plan.taskWindows.size() < 2) {
        return Fail(report, "repository image requires SQE and sync task windows");
    }

    const auto& sqeTask = plan.taskWindows[0];
    const auto& syncTask = plan.taskWindows[1];
    if (sqeTask.instCnt != program.sqeLoad.size()) {
        return Fail(report, "SQE task window does not match generated SQE microcode");
    }
    if (syncTask.instStartId != static_cast<uint32_t>(sqeTask.instStartId) + sqeTask.instCnt) {
        return Fail(report, "sync task window must start immediately after SQE microcode");
    }
    if (syncTask.instCnt < program.sync.size()) {
        return Fail(report, "sync task window is too small for generated sync microcode");
    }
    return TILEXR_SUCCESS;
}

int ValidateRepositoryWindow(const TileXRCcuProducerPlan& plan, TileXRCcuRepositoryReport* report)
{
    const auto& window = plan.instructionWindow;
    if (!ContainsWindow(
            window.repositoryStartId, window.repositoryCount, window.missionStartId, window.missionCount)) {
        return Fail(report, "mission instruction window is outside the repository image");
    }
    for (const auto& task : plan.taskWindows) {
        if (task.dieId != window.dieId) {
            return Fail(report, "task die does not match repository image die");
        }
        if (!ContainsWindow(window.missionStartId, window.missionCount, task.instStartId, task.instCnt)) {
            return Fail(report, "task instruction window is outside the mission image");
        }
        if (!ContainsWindow(window.repositoryStartId, window.repositoryCount, task.instStartId, task.instCnt)) {
            return Fail(report, "task instruction window is outside the repository image");
        }
    }
    return TILEXR_SUCCESS;
}

void FillImageHeader(const TileXRCcuInstructionWindow& window, TileXRCcuRepositoryImage* image)
{
    image->dieId = window.dieId;
    image->repositoryStartId = window.repositoryStartId;
    image->repositoryCount = window.repositoryCount;
    image->missionStartId = window.missionStartId;
    image->missionCount = window.missionCount;
    image->missionOffset = OffsetFrom(window.repositoryStartId, window.missionStartId);
}

void FillReport(const TileXRCcuRepositoryImage& image, TileXRCcuRepositoryReport* report)
{
    if (report == nullptr) {
        return;
    }
    report->repositoryCount = image.repositoryCount;
    report->installedInstructionCount = static_cast<uint16_t>(image.sqeLoadCount + image.syncCount);
    report->sqeLoadOffset = image.sqeLoadOffset;
    report->syncOffset = image.syncOffset;
    report->repositoryUploaded = false;
    report->repositoryInstalled = false;
    report->message = "ok";
}

int BuildRepositoryPaddingInstruction(const TileXRCcuProducerPlan& plan, TileXRCcuInstr* instr,
    TileXRCcuRepositoryReport* report)
{
    if (plan.kernelLocalXn.startId == 0 ||
        TileXRCcuEncodeLoadImdToXn(plan.kernelLocalXn.startId, 0, 0, instr) != TILEXR_SUCCESS) {
        return Fail(report, "failed to build valid repository padding instruction");
    }
    return TILEXR_SUCCESS;
}

int ValidateMemoryOps(const TileXRCcuDeviceMemoryOps& memoryOps, TileXRCcuRepositoryReport* report)
{
    if (memoryOps.alloc == nullptr) {
        return Fail(report, "missing CCU repository device allocation hook");
    }
    if (memoryOps.copyHostToDevice == nullptr) {
        return Fail(report, "missing CCU repository host-to-device copy hook");
    }
    if (memoryOps.free == nullptr) {
        return Fail(report, "missing CCU repository device free hook");
    }
    return TILEXR_SUCCESS;
}

int ValidateInstallImage(const TileXRCcuRepositoryImage& image, TileXRCcuRepositoryReport* report)
{
    if (image.instructions.empty()) {
        return Fail(report, "missing CCU repository instruction image");
    }
    if (image.missionCount == 0) {
        return Fail(report, "missing CCU repository mission instruction window");
    }
    if (image.missionOffset >= image.instructions.size() ||
        static_cast<uint32_t>(image.missionOffset) + image.missionCount > image.instructions.size()) {
        return Fail(report, "CCU repository mission window is outside instruction image");
    }
    if (image.missionStartId == 0) {
        return Fail(report, "missing CCU repository mission start instruction id");
    }
    return TILEXR_SUCCESS;
}

void FillInstallReport(const TileXRCcuRepositoryInstallReceipt& receipt, TileXRCcuRepositoryReport* report)
{
    if (report == nullptr) {
        return;
    }
    report->repositoryCount = receipt.instructionCount;
    report->installedInstructionCount = receipt.instructionCount;
    report->sqeLoadOffset = 0;
    report->syncOffset = 0;
    report->repositoryUploaded = receipt.uploaded;
    report->repositoryInstalled = receipt.installed;
    report->message = "ok";
}

bool FitsSizeT(uint64_t bytes)
{
    return bytes <= static_cast<uint64_t>(std::numeric_limits<size_t>::max());
}

uint16_t InstallStartId(const TileXRCcuRepositoryImage& image, const TileXRCcuRepositoryInstallOptions& options)
{
    return options.window == TileXRCcuRepositoryInstallWindow::FullRepository ?
        image.repositoryStartId :
        image.missionStartId;
}

uint16_t InstallInstructionCount(
    const TileXRCcuRepositoryImage& image,
    const TileXRCcuRepositoryInstallOptions& options)
{
    return options.window == TileXRCcuRepositoryInstallWindow::FullRepository ?
        image.repositoryCount :
        image.missionCount;
}

uint16_t InstallImageOffset(const TileXRCcuRepositoryImage& image, const TileXRCcuRepositoryInstallOptions& options)
{
    return options.window == TileXRCcuRepositoryInstallWindow::FullRepository ? 0 : image.missionOffset;
}

uint32_t InstallCustomChannelDataLen(
    uint64_t instructionBytes,
    const TileXRCcuRepositoryInstallOptions& options)
{
    return options.dataLenMode == TileXRCcuRepositoryInstallDataLenMode::DescriptorBytes ?
        static_cast<uint32_t>(sizeof(TileXRCcuInstrInfo)) :
        static_cast<uint32_t>(instructionBytes);
}

const char* InstallWindowText(TileXRCcuRepositoryInstallWindow window)
{
    return window == TileXRCcuRepositoryInstallWindow::FullRepository ? "full_repository" : "mission";
}

const char* InstallDataLenModeText(TileXRCcuRepositoryInstallDataLenMode mode)
{
    return mode == TileXRCcuRepositoryInstallDataLenMode::DescriptorBytes ?
        "descriptor_bytes" :
        "instruction_bytes";
}

uint64_t MixFnv1aByte(uint64_t hash, uint8_t value)
{
    constexpr uint64_t prime = 1099511628211ULL;
    hash ^= value;
    hash *= prime;
    return hash;
}

uint64_t BuildInstructionFnv1a64(const TileXRCcuInstr* instructions, uint16_t count)
{
    uint64_t hash = 1469598103934665603ULL;
    for (uint16_t i = 0; i < count; ++i) {
        for (uint64_t word : instructions[i].words) {
            for (uint32_t byte = 0; byte < 8U; ++byte) {
                hash = MixFnv1aByte(hash, static_cast<uint8_t>((word >> (byte * 8U)) & 0xffU));
            }
        }
    }
    return hash;
}

std::string FormatInstructionWords(const TileXRCcuInstr& instr)
{
    std::ostringstream out;
    out << std::hex << std::setfill('0');
    for (uint32_t i = 0; i < 4U; ++i) {
        if (i != 0) {
            out << ",";
        }
        out << "0x" << std::setw(16) << instr.words[i];
    }
    return out.str();
}

uint32_t CountInstructionMismatches(
    const TileXRCcuInstr* expected,
    const TileXRCcuInstr* actual,
    uint16_t count)
{
    uint32_t mismatches = 0;
    for (uint16_t i = 0; i < count; ++i) {
        if (expected[i].words[0] != actual[i].words[0] ||
            expected[i].words[1] != actual[i].words[1] ||
            expected[i].words[2] != actual[i].words[2] ||
            expected[i].words[3] != actual[i].words[3]) {
            ++mismatches;
        }
    }
    return mismatches;
}

const char* UploadReadbackStatusText(const TileXRCcuUploadReadbackDiagnostic& diagnostic)
{
    if (!diagnostic.attempted) {
        return "skipped";
    }
    if (diagnostic.ret != 0) {
        return "failed";
    }
    if (!diagnostic.ok) {
        return "mismatch";
    }
    return "ok";
}

void FillSuccessfulReadbackDiagnostic(
    const std::vector<TileXRCcuInstr>& readback,
    const TileXRCcuInstr* expectedInstructions,
    uint16_t instructionCount,
    TileXRCcuUploadReadbackDiagnostic* diagnostic)
{
    if (diagnostic == nullptr || readback.empty() || expectedInstructions == nullptr || instructionCount == 0) {
        return;
    }
    diagnostic->fnv1a64 = BuildInstructionFnv1a64(readback.data(), instructionCount);
    diagnostic->firstInstructionWords = FormatInstructionWords(readback.front());
    diagnostic->lastInstructionWords = FormatInstructionWords(readback.back());
    diagnostic->mismatchCount = CountInstructionMismatches(expectedInstructions, readback.data(), instructionCount);
    diagnostic->ok = diagnostic->mismatchCount == 0;
}

void BuildUploadReadbackDiagnostic(
    const TileXRCcuDeviceMemoryOps& memoryOps,
    void* devicePtr,
    uint64_t instructionBytes,
    const TileXRCcuInstr* expectedInstructions,
    uint16_t instructionCount,
    void* memoryUserData,
    TileXRCcuUploadReadbackDiagnostic* diagnostic)
{
    if (diagnostic == nullptr) {
        return;
    }
    *diagnostic = TileXRCcuUploadReadbackDiagnostic {};
    if (memoryOps.copyDeviceToHost == nullptr) {
        return;
    }
    diagnostic->attempted = true;
    diagnostic->bytes = instructionBytes;
    if (devicePtr == nullptr || expectedInstructions == nullptr || instructionCount == 0 ||
        !FitsSizeT(instructionBytes)) {
        diagnostic->ret = TILEXR_ERROR_PARA_CHECK_FAIL;
        return;
    }

    std::vector<TileXRCcuInstr> readback(instructionCount);
    diagnostic->ret = memoryOps.copyDeviceToHost(
        readback.data(),
        instructionBytes,
        devicePtr,
        instructionBytes,
        memoryUserData);
    if (diagnostic->ret != 0) {
        return;
    }

    FillSuccessfulReadbackDiagnostic(readback, expectedInstructions, instructionCount, diagnostic);
}

void BuildDriverInstructionReadbackDiagnostic(
    const TileXRCcuDriverAdapter& adapter,
    uint8_t dieId,
    uint16_t instructionStartId,
    uint16_t instructionCount,
    uint64_t instructionBytes,
    const TileXRCcuInstr* expectedInstructions,
    TileXRCcuUploadReadbackDiagnostic* diagnostic)
{
    if (diagnostic == nullptr) {
        return;
    }
    *diagnostic = TileXRCcuUploadReadbackDiagnostic {};
    diagnostic->attempted = true;
    diagnostic->bytes = instructionBytes;
    if (expectedInstructions == nullptr || instructionCount == 0 || !FitsSizeT(instructionBytes)) {
        diagnostic->ret = TILEXR_ERROR_PARA_CHECK_FAIL;
        return;
    }

    std::vector<TileXRCcuInstr> readback(instructionCount);
    uint16_t readOffset = 0;
    while (readOffset < instructionCount) {
        const uint32_t batch = std::min<uint32_t>(
            TILEXR_CCU_MAX_DATA_ARRAY_SIZE,
            static_cast<uint32_t>(instructionCount - readOffset));
        TileXRCcuDriverAdapterReport driverReport;
        diagnostic->ret = adapter.ReadInstructions(
            dieId,
            static_cast<uint16_t>(instructionStartId + readOffset),
            readback.data() + readOffset,
            batch,
            batch * TILEXR_CCU_INSTRUCTION_BYTES,
            &driverReport);
        if (diagnostic->ret != TILEXR_SUCCESS) {
            return;
        }
        readOffset = static_cast<uint16_t>(readOffset + batch);
    }

    FillSuccessfulReadbackDiagnostic(readback, expectedInstructions, instructionCount, diagnostic);
}

std::string BuildInstallFailureDiagnostic(
    const std::string& driverMessage,
    const TileXRCcuRepositoryImage& image,
    const TileXRCcuRepositoryInstallOptions& options,
    uint16_t installOffset,
    uint16_t installStartId,
    uint16_t installCount,
    uint64_t instructionBytes,
    uint32_t customChannelDataLen,
    uint64_t deviceInstructionAddr,
    const TileXRCcuUploadReadbackDiagnostic& uploadReadback,
    const TileXRCcuUploadReadbackDiagnostic& driverReadback)
{
    const TileXRCcuInstr* firstInstruction = image.instructions.data() + installOffset;
    const TileXRCcuInstr* lastInstruction = firstInstruction + installCount - 1U;
    const uint64_t hash = BuildInstructionFnv1a64(firstInstruction, installCount);

    std::ostringstream message;
    message << "failed to install CCU repository instruction image: " << driverMessage
            << " dieId=" << static_cast<uint32_t>(image.dieId)
            << " installStartId=" << installStartId
            << " installCount=" << installCount
            << " instructionBytes=" << instructionBytes
            << " customChannelDataLen=" << customChannelDataLen
            << " deviceInstructionAddr=0x" << std::hex << deviceInstructionAddr << std::dec
            << " window=" << InstallWindowText(options.window)
            << " dataLenMode=" << InstallDataLenModeText(options.dataLenMode)
            << " firstInstructionWords=" << FormatInstructionWords(*firstInstruction)
            << " lastInstructionWords=" << FormatInstructionWords(*lastInstruction)
            << " instructionFnv1a64=0x" << std::hex << hash << std::dec
            << " uploadReadback=" << UploadReadbackStatusText(uploadReadback);
    if (uploadReadback.attempted) {
        message << " uploadReadbackRet=" << uploadReadback.ret
                << " uploadReadbackBytes=" << uploadReadback.bytes;
        if (uploadReadback.ret == 0) {
            message << " uploadReadbackFnv1a64=0x" << std::hex << uploadReadback.fnv1a64 << std::dec
                    << " uploadReadbackFirstInstructionWords=" << uploadReadback.firstInstructionWords
                    << " uploadReadbackLastInstructionWords=" << uploadReadback.lastInstructionWords
                    << " uploadReadbackMismatchCount=" << uploadReadback.mismatchCount;
        }
    }
    message << " driverReadback=" << UploadReadbackStatusText(driverReadback);
    if (driverReadback.attempted) {
        message << " driverReadbackRet=" << driverReadback.ret
                << " driverReadbackBytes=" << driverReadback.bytes;
        if (driverReadback.ret == 0) {
            message << " driverReadbackFnv1a64=0x" << std::hex << driverReadback.fnv1a64 << std::dec
                    << " driverReadbackFirstInstructionWords=" << driverReadback.firstInstructionWords
                    << " driverReadbackLastInstructionWords=" << driverReadback.lastInstructionWords
                    << " driverReadbackMismatchCount=" << driverReadback.mismatchCount;
        }
    }
    return message.str();
}

int AclDeviceAlloc(uint64_t bytes, void** devicePtr, void*)
{
    if (devicePtr == nullptr || bytes == 0 || !FitsSizeT(bytes)) {
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }
#if TILEXR_CCU_HAVE_ACL_RT_HEADER
    return aclrtMalloc(devicePtr, static_cast<size_t>(bytes), ACL_MEM_MALLOC_HUGE_FIRST);
#else
    *devicePtr = nullptr;
    return TILEXR_ERROR_MKIRT;
#endif
}

int AclModule3DeviceAlloc(uint64_t bytes, void** devicePtr, void*)
{
    if (devicePtr == nullptr || bytes == 0 || !FitsSizeT(bytes)) {
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }
#if TILEXR_CCU_HAVE_ACL_RT_HEADER
    aclrtMallocAttrValue moduleIdValue {};
    moduleIdValue.moduleId = TILEXR_CCU_ACL_MODULE3_ID;
    aclrtMallocAttribute attrs {ACL_RT_MEM_ATTR_MODULE_ID, moduleIdValue};
    aclrtMallocConfig cfg {&attrs, 1};
    return aclrtMallocWithCfg(
        devicePtr,
        static_cast<size_t>(bytes),
        static_cast<aclrtMemMallocPolicy>(ACL_MEM_TYPE_HIGH_BAND_WIDTH | ACL_MEM_MALLOC_HUGE_FIRST),
        &cfg);
#else
    *devicePtr = nullptr;
    return TILEXR_ERROR_MKIRT;
#endif
}

int AclCopyHostToDevice(void* deviceDst, uint64_t deviceDstBytes, const void* hostSrc, uint64_t bytes, void*)
{
    if (deviceDst == nullptr || hostSrc == nullptr || bytes == 0 || bytes > deviceDstBytes ||
        !FitsSizeT(deviceDstBytes) || !FitsSizeT(bytes)) {
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }
#if TILEXR_CCU_HAVE_ACL_RT_HEADER
    return aclrtMemcpy(
        deviceDst,
        static_cast<size_t>(deviceDstBytes),
        hostSrc,
        static_cast<size_t>(bytes),
        ACL_MEMCPY_HOST_TO_DEVICE);
#else
    return TILEXR_ERROR_MKIRT;
#endif
}

int AclCopyDeviceToHost(void* hostDst, uint64_t hostDstBytes, const void* deviceSrc, uint64_t bytes, void*)
{
    if (hostDst == nullptr || deviceSrc == nullptr || bytes == 0 || bytes > hostDstBytes ||
        !FitsSizeT(hostDstBytes) || !FitsSizeT(bytes)) {
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }
#if TILEXR_CCU_HAVE_ACL_RT_HEADER
    return aclrtMemcpy(
        hostDst,
        static_cast<size_t>(hostDstBytes),
        deviceSrc,
        static_cast<size_t>(bytes),
        ACL_MEMCPY_DEVICE_TO_HOST);
#else
    return TILEXR_ERROR_MKIRT;
#endif
}

int AclDeviceFree(void* devicePtr, void*)
{
    if (devicePtr == nullptr) {
        return TILEXR_SUCCESS;
    }
#if TILEXR_CCU_HAVE_ACL_RT_HEADER
    return aclrtFree(devicePtr);
#else
    return TILEXR_ERROR_MKIRT;
#endif
}

int RtHbmDeviceAlloc(uint64_t bytes, void** devicePtr, void*)
{
    if (devicePtr == nullptr || bytes == 0) {
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }
#if TILEXR_CCU_HAVE_RUNTIME_MEM_HEADER
    return rtMalloc(devicePtr, bytes, RT_MEMORY_HBM, TILEXR_CCU_RT_HBM_MODULE_ID);
#else
    *devicePtr = nullptr;
    return TILEXR_ERROR_MKIRT;
#endif
}

int RtHbmCopyHostToDevice(void* deviceDst, uint64_t deviceDstBytes, const void* hostSrc, uint64_t bytes, void*)
{
    if (deviceDst == nullptr || hostSrc == nullptr || bytes == 0 || bytes > deviceDstBytes) {
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }
#if TILEXR_CCU_HAVE_RUNTIME_MEM_HEADER
    return rtMemcpy(deviceDst, deviceDstBytes, hostSrc, bytes, RT_MEMCPY_HOST_TO_DEVICE);
#else
    return TILEXR_ERROR_MKIRT;
#endif
}

int RtHbmCopyDeviceToHost(void* hostDst, uint64_t hostDstBytes, const void* deviceSrc, uint64_t bytes, void*)
{
    if (hostDst == nullptr || deviceSrc == nullptr || bytes == 0 || bytes > hostDstBytes) {
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }
#if TILEXR_CCU_HAVE_RUNTIME_MEM_HEADER
    return rtMemcpy(hostDst, hostDstBytes, deviceSrc, bytes, RT_MEMCPY_DEVICE_TO_HOST);
#else
    return TILEXR_ERROR_MKIRT;
#endif
}

int RtHbmDeviceFree(void* devicePtr, void*)
{
    if (devicePtr == nullptr) {
        return TILEXR_SUCCESS;
    }
#if TILEXR_CCU_HAVE_RUNTIME_MEM_HEADER
    return rtFree(devicePtr);
#else
    return TILEXR_ERROR_MKIRT;
#endif
}

} // namespace

TileXRCcuDeviceMemoryOps TileXRCcuMakeAclDeviceMemoryOps()
{
    TileXRCcuDeviceMemoryOps ops;
#if TILEXR_CCU_HAVE_ACL_RT_HEADER
    ops.alloc = AclDeviceAlloc;
    ops.copyHostToDevice = AclCopyHostToDevice;
    ops.free = AclDeviceFree;
    ops.copyDeviceToHost = AclCopyDeviceToHost;
#endif
    return ops;
}

TileXRCcuDeviceMemoryOps TileXRCcuMakeAclModule3DeviceMemoryOps()
{
    TileXRCcuDeviceMemoryOps ops;
#if TILEXR_CCU_HAVE_ACL_RT_HEADER
    ops.alloc = AclModule3DeviceAlloc;
    ops.copyHostToDevice = AclCopyHostToDevice;
    ops.free = AclDeviceFree;
    ops.copyDeviceToHost = AclCopyDeviceToHost;
#endif
    return ops;
}

TileXRCcuDeviceMemoryOps TileXRCcuMakeRtHbmDeviceMemoryOps()
{
    TileXRCcuDeviceMemoryOps ops;
#if TILEXR_CCU_HAVE_RUNTIME_MEM_HEADER
    ops.alloc = RtHbmDeviceAlloc;
    ops.copyHostToDevice = RtHbmCopyHostToDevice;
    ops.free = RtHbmDeviceFree;
    ops.copyDeviceToHost = RtHbmCopyDeviceToHost;
#endif
    return ops;
}

TileXRCcuDeviceMemoryOps TileXRCcuMakeRepositoryDeviceMemoryOps(TileXRCcuRepositoryMemoryAllocMode mode)
{
    if (mode == TileXRCcuRepositoryMemoryAllocMode::AclModule3) {
        return TileXRCcuMakeAclModule3DeviceMemoryOps();
    }
    if (mode == TileXRCcuRepositoryMemoryAllocMode::RtHbm) {
        return TileXRCcuMakeRtHbmDeviceMemoryOps();
    }
    return TileXRCcuMakeAclDeviceMemoryOps();
}

int TileXRCcuBuildRepositoryImage(
    const TileXRCcuProducerPlan& plan,
    const TileXRCcuProgram& program,
    TileXRCcuRepositoryImage* image,
    TileXRCcuRepositoryReport* report)
{
    ResetReport(report);
    if (image == nullptr) {
        return Fail(report, "missing output repository image");
    }
    image->instructions.clear();

    TileXRCcuProducerPlanReport planReport;
    if (TileXRCcuValidateProducerPlan(plan, &planReport) != TILEXR_SUCCESS) {
        return Fail(report, planReport.message);
    }
    if (ValidateProgramShape(plan, program, report) != TILEXR_SUCCESS) {
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    if (ValidateRepositoryWindow(plan, report) != TILEXR_SUCCESS) {
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }

    FillImageHeader(plan.instructionWindow, image);
    const size_t syncTaskIndex = program.sqeLoad.empty() ? 0U : 1U;
    image->sqeLoadOffset = program.sqeLoad.empty() ?
        0 :
        OffsetFrom(plan.instructionWindow.repositoryStartId, plan.taskWindows[0].instStartId);
    image->sqeLoadCount = static_cast<uint16_t>(program.sqeLoad.size());
    image->syncOffset = OffsetFrom(plan.instructionWindow.repositoryStartId, plan.taskWindows[syncTaskIndex].instStartId);
    image->syncCount = static_cast<uint16_t>(program.sync.size());

    const uint32_t sqeEnd = static_cast<uint32_t>(image->sqeLoadOffset) + image->sqeLoadCount;
    const uint32_t syncEnd = static_cast<uint32_t>(image->syncOffset) + image->syncCount;
    if (sqeEnd > image->repositoryCount || syncEnd > image->repositoryCount) {
        image->instructions.clear();
        return Fail(report, "generated microcode does not fit in repository image");
    }

    TileXRCcuInstr paddingInstr;
    if (BuildRepositoryPaddingInstruction(plan, &paddingInstr, report) != TILEXR_SUCCESS) {
        image->instructions.clear();
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    image->instructions.assign(image->repositoryCount, paddingInstr);
    std::copy(program.sqeLoad.begin(), program.sqeLoad.end(), image->instructions.begin() + image->sqeLoadOffset);
    std::copy(program.sync.begin(), program.sync.end(), image->instructions.begin() + image->syncOffset);

    FillReport(*image, report);
    return TILEXR_SUCCESS;
}

int TileXRCcuInstallRepositoryImage(
    const TileXRCcuRepositoryImage& image,
    const TileXRCcuDeviceMemoryOps& memoryOps,
    void* memoryUserData,
    const TileXRCcuDriverAdapter& adapter,
    TileXRCcuRepositoryInstallReceipt* receipt,
    TileXRCcuRepositoryReport* report)
{
    TileXRCcuRepositoryInstallOptions options;
    return TileXRCcuInstallRepositoryImageWithOptions(
        image,
        options,
        memoryOps,
        memoryUserData,
        adapter,
        receipt,
        report);
}

int TileXRCcuInstallRepositoryImageWithOptions(
    const TileXRCcuRepositoryImage& image,
    const TileXRCcuRepositoryInstallOptions& options,
    const TileXRCcuDeviceMemoryOps& memoryOps,
    void* memoryUserData,
    const TileXRCcuDriverAdapter& adapter,
    TileXRCcuRepositoryInstallReceipt* receipt,
    TileXRCcuRepositoryReport* report)
{
    ResetReport(report);
    if (receipt == nullptr) {
        return Fail(report, "missing output CCU repository install receipt");
    }
    *receipt = TileXRCcuRepositoryInstallReceipt{};

    if (ValidateMemoryOps(memoryOps, report) != TILEXR_SUCCESS ||
        ValidateInstallImage(image, report) != TILEXR_SUCCESS) {
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }

    const uint16_t installOffset = InstallImageOffset(image, options);
    const uint16_t installStartId = InstallStartId(image, options);
    const uint16_t installCount = InstallInstructionCount(image, options);
    if (installCount == 0 ||
        installOffset >= image.instructions.size() ||
        static_cast<uint32_t>(installOffset) + installCount > image.instructions.size()) {
        return Fail(report, "CCU repository selected install window is outside instruction image");
    }

    const uint64_t instructionBytes = static_cast<uint64_t>(installCount) * sizeof(TileXRCcuInstr);
    void* devicePtr = nullptr;
    const int allocRet = memoryOps.alloc(instructionBytes, &devicePtr, memoryUserData);
    if (allocRet != 0 || devicePtr == nullptr) {
        return Fail(report, "failed to allocate CCU repository device instruction image", TILEXR_ERROR_MKIRT);
    }

    const auto* firstInstruction = image.instructions.data() + installOffset;
    const int copyRet = memoryOps.copyHostToDevice(
        devicePtr,
        instructionBytes,
        firstInstruction,
        instructionBytes,
        memoryUserData);
    if (copyRet != 0) {
        (void)memoryOps.free(devicePtr, memoryUserData);
        return Fail(report, "failed to copy CCU repository instruction image to device", TILEXR_ERROR_MKIRT);
    }

    TileXRCcuRepositoryInstallReceipt result;
    result.dieId = image.dieId;
    result.instructionStartId = installStartId;
    result.instructionCount = installCount;
    result.instructionBytes = instructionBytes;
    result.deviceInstructionAddr = reinterpret_cast<uint64_t>(devicePtr);
    result.deviceInstructionPtr = devicePtr;
    result.uploaded = true;

    TileXRCcuUploadReadbackDiagnostic uploadReadback;
    BuildUploadReadbackDiagnostic(
        memoryOps,
        devicePtr,
        instructionBytes,
        firstInstruction,
        installCount,
        memoryUserData,
        &uploadReadback);

    const uint32_t customChannelDataLen = InstallCustomChannelDataLen(instructionBytes, options);
    TileXRCcuDriverAdapterReport driverReport;
    const int installRet = adapter.InstallInstructionsWithDataLen(
        image.dieId,
        installStartId,
        installCount,
        result.deviceInstructionAddr,
        static_cast<uint32_t>(instructionBytes),
        customChannelDataLen,
        &driverReport);
    if (installRet != TILEXR_SUCCESS) {
        TileXRCcuUploadReadbackDiagnostic driverReadback;
        BuildDriverInstructionReadbackDiagnostic(
            adapter,
            image.dieId,
            installStartId,
            installCount,
            instructionBytes,
            firstInstruction,
            &driverReadback);
        (void)memoryOps.free(devicePtr, memoryUserData);
        return Fail(
            report,
            BuildInstallFailureDiagnostic(
                driverReport.message,
                image,
                options,
                installOffset,
                installStartId,
                installCount,
                instructionBytes,
                customChannelDataLen,
                result.deviceInstructionAddr,
                uploadReadback,
                driverReadback),
            TILEXR_ERROR_MKIRT);
    }

    result.installed = true;
    *receipt = result;
    FillInstallReport(*receipt, report);
    if (report != nullptr &&
        (options.window != TileXRCcuRepositoryInstallWindow::Mission ||
         options.dataLenMode != TileXRCcuRepositoryInstallDataLenMode::InstructionBytes)) {
        report->message = std::string("ok window=") + InstallWindowText(options.window) +
            " dataLenMode=" + InstallDataLenModeText(options.dataLenMode);
    }
    return TILEXR_SUCCESS;
}

int TileXRCcuReleaseRepositoryInstallReceipt(
    TileXRCcuRepositoryInstallReceipt& receipt,
    const TileXRCcuDeviceMemoryOps& memoryOps,
    void* memoryUserData,
    TileXRCcuRepositoryReport* report)
{
    ResetReport(report);
    if (memoryOps.free == nullptr) {
        return Fail(report, "missing CCU repository device free hook");
    }
    if (receipt.deviceInstructionPtr == nullptr) {
        receipt = TileXRCcuRepositoryInstallReceipt{};
        if (report != nullptr) {
            report->message = "ok";
        }
        return TILEXR_SUCCESS;
    }
    const int freeRet = memoryOps.free(receipt.deviceInstructionPtr, memoryUserData);
    if (freeRet != 0) {
        return Fail(report, "failed to release CCU repository device instruction image", TILEXR_ERROR_MKIRT);
    }
    receipt = TileXRCcuRepositoryInstallReceipt{};
    if (report != nullptr) {
        report->message = "ok";
    }
    return TILEXR_SUCCESS;
}

} // namespace TileXR
