/*
 * 将已校验的 MoonEP Host 参数打包为外置 kernel ABI，通过注册后的 runtime 入口提交。
 */
#include "dispatch_launch.h"

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <mutex>

#include "acl/acl_rt.h"
#include "../common/dispatch_credit.h"
#include "../common/dispatch_schedule.h"
#include "runtime/kernel.h"
#include "tilexr_kernel_binary_loader.h"
#include "tilexr_types.h"

#ifndef TILEXR_MOONEP_DISPATCH_SYSTEM_CYCLES_PER_SECOND
#define TILEXR_MOONEP_DISPATCH_SYSTEM_CYCLES_PER_SECOND UINT64_C(1000000000)
#endif

namespace TileXRMoonEp {
namespace {

constexpr uint32_t kAivBinaryMagic = 0x41415246U;
constexpr char kDispatchKernelName[] = "tilexr_moonep_dispatch_urma_kernel";
constexpr char kDispatchAivCoreCountEnv[] =
    "TILEXR_MOONEP_DISPATCH_AIV_CORE_COUNT";
constexpr uint32_t kDispatchSimtDCacheBytes = 32U * 1024U;
constexpr uint32_t kDispatchMinDynamicUbBytes = 190U * 1024U;

std::mutex gDispatchRegistrationMutex;
bool gDispatchRegistered = false;
int gDispatchRegistrationStatus = TileXR::TILEXR_ERROR_NOT_INITIALIZED;
void *gDispatchBinaryHandle = nullptr;
uint8_t gDispatchKernelStub = 0;

int EnsureDispatchKernelRegistered()
{
    std::lock_guard<std::mutex> guard(gDispatchRegistrationMutex);
    if (gDispatchRegistered) {
        return gDispatchRegistrationStatus;
    }
    TileXR::KernelBinaryView kernelBinary {};
    if (!TileXR::LoadKernelBinary(
            &gDispatchRegistrationMutex,
            "tilexr_moonep_dispatch_urma_kernel.o", &kernelBinary)) {
        return TileXR::TILEXR_ERROR_NOT_INITIALIZED;
    }
    rtDevBinary_t binary {};
    binary.data = kernelBinary.data;
    binary.length = static_cast<uint32_t>(kernelBinary.size);
    binary.magic = kAivBinaryMagic;
    binary.version = 0;
    rtError_t rtRet = rtDevBinaryRegister(&binary, &gDispatchBinaryHandle);
    if (rtRet == RT_ERROR_NONE) {
        rtRet = rtFunctionRegister(gDispatchBinaryHandle, &gDispatchKernelStub,
            kDispatchKernelName, kDispatchKernelName, 0);
    }
    if (rtRet != RT_ERROR_NONE) {
        std::cerr << "TileXR MoonEP Dispatch kernel registration failed, ret="
                  << rtRet << std::endl;
        gDispatchRegistrationStatus = TileXR::TILEXR_ERROR_MKIRT;
        return gDispatchRegistrationStatus;
    }
    gDispatchRegistered = true;
    gDispatchRegistrationStatus = TileXR::TILEXR_SUCCESS;
    return gDispatchRegistrationStatus;
}

uint32_t ResolveDispatchAivCoreCount()
{
    const char *value = std::getenv(kDispatchAivCoreCountEnv);
    if (value == nullptr || value[0] == '\0') {
        return kDispatchAivCoreCount;
    }
    errno = 0;
    char *end = nullptr;
    const unsigned long parsed = std::strtoul(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0' || parsed == 0UL ||
        parsed > kDispatchAivCoreCount) {
        std::cerr << kDispatchAivCoreCountEnv << " must be an integer in [1, "
                  << kDispatchAivCoreCount << "], got " << value << std::endl;
        return 0U;
    }
    return static_cast<uint32_t>(parsed);
}

int ConfigureDispatchSimtMemory(rtTaskCfgInfo_t &cfgInfo)
{
    int32_t deviceId = 0;
    if (aclrtGetDevice(&deviceId) != ACL_SUCCESS) {
        return TileXR::TILEXR_ERROR_INTERNAL;
    }

    int64_t dynamicUbBytes = 0;
    if (aclrtGetDeviceInfo(static_cast<uint32_t>(deviceId),
            ACL_DEV_ATTR_UBUF_PER_VECTOR_CORE, &dynamicUbBytes) != ACL_SUCCESS ||
        dynamicUbBytes < static_cast<int64_t>(kDispatchMinDynamicUbBytes) ||
        dynamicUbBytes > static_cast<int64_t>(UINT32_MAX - kDispatchSimtDCacheBytes)) {
        return TileXR::TILEXR_ERROR_PARA_CHECK_FAIL;
    }

    cfgInfo.localMemorySize = static_cast<uint32_t>(dynamicUbBytes);
    return TileXR::TILEXR_SUCCESS;
}

} // namespace

uint64_t TileXRMoonEpDispatchCompletionTimeoutTicks()
{
    constexpr uint64_t cyclesPerSecond =
        TILEXR_MOONEP_DISPATCH_SYSTEM_CYCLES_PER_SECOND;
    static_assert(cyclesPerSecond > 0, "Dispatch system-cycle frequency must be positive");
    static_assert(cyclesPerSecond <= std::numeric_limits<uint64_t>::max() /
        kDispatchProductionTimeoutSeconds, "Dispatch timeout tick count overflows");
    return cyclesPerSecond * kDispatchProductionTimeoutSeconds;
}

int TileXRMoonEpLaunchDispatchUrmaKernel(const DispatchUrmaLaunchParams &params)
{
    const bool hasWeight = params.weightInput != nullptr;
    if (params.hiddenInput == nullptr || params.hiddenOutput == nullptr ||
        (hasWeight != (params.weightOutput != nullptr))) {
        return TileXR::TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    const uint32_t aivCoreCount = ResolveDispatchAivCoreCount();
    if (aivCoreCount == 0U) {
        return TileXR::TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    if (!DispatchPeerModeValid(static_cast<uint32_t>(params.peerMode)) ||
        !DispatchGroupWidthValid(params.groupWidth)) {
        return TileXR::TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    int64_t magic = 0;
    int ret = TileXRCommNextMagic(params.comm, &magic);
    if (ret != TileXR::TILEXR_SUCCESS) {
        return ret;
    }
    if (DispatchPeerModeUsesCredit(static_cast<uint32_t>(params.peerMode))) {
        const uint32_t groupCount = DispatchGroupedGroupCount(
            params.layout.rankSize, params.groupWidth);
        uint64_t creditToken = 0U;
        if (groupCount != 0U && !DispatchCreditToken(
                magic, groupCount - 1U, creditToken)) {
            return TileXR::TILEXR_ERROR_PARA_CHECK_FAIL;
        }
    }
    ret = EnsureDispatchKernelRegistered();
    if (ret != TileXR::TILEXR_SUCCESS) {
        return ret;
    }

    DispatchKernelArgs args {
        params.commArgs,
        reinterpret_cast<GM_ADDR>(const_cast<void *>(params.hiddenInput)),
        reinterpret_cast<GM_ADDR>(const_cast<void *>(params.weightInput)),
        reinterpret_cast<GM_ADDR>(const_cast<int32_t *>(params.dst)),
        reinterpret_cast<GM_ADDR>(const_cast<int32_t *>(params.zeroFillRanges)),
        static_cast<GM_ADDR>(params.workspace),
        static_cast<GM_ADDR>(params.hiddenOutput),
        static_cast<GM_ADDR>(params.weightOutput),
        reinterpret_cast<GM_ADDR>(params.planStatus),
        params.layout.hidden.sourceOffset,
        params.layout.hidden.scratchOffset,
        params.layout.hidden.rowBytes,
        params.layout.weight.sourceOffset,
        params.layout.weight.scratchOffset,
        params.layout.weight.rowBytes,
        params.layout.completionFlagsOffset,
        params.layout.creditOffset,
        params.layout.creditSourceOffset,
        params.layout.signalOffset,
        params.layout.hiddenDfxOffset,
        params.layout.weightDfxOffset,
        params.layout.kernelStatusOffset,
        params.layout.s,
        params.layout.k,
        params.layout.h,
        params.layout.routeCount,
        params.layout.destinationCapacity,
        params.zeroFillRangeCount,
        hasWeight ? 1U : 0U,
        magic,
        TileXRMoonEpDispatchCompletionTimeoutTicks(),
        static_cast<uint64_t>(params.peerMode),
        params.groupWidth,
    };

    rtArgsEx_t argsInfo {};
    argsInfo.args = &args;
    argsInfo.argsSize = sizeof(args);
    rtTaskCfgInfo_t cfgInfo {};
    const int memoryRet = ConfigureDispatchSimtMemory(cfgInfo);
    if (memoryRet != TileXR::TILEXR_SUCCESS) {
        return memoryRet;
    }
    const rtError_t rtRet = rtKernelLaunchWithFlagV2(&gDispatchKernelStub,
        aivCoreCount, &argsInfo, nullptr,
        static_cast<rtStream_t>(params.stream), 0, &cfgInfo);
    if (rtRet != RT_ERROR_NONE) {
        std::cerr << "TileXR MoonEP Dispatch kernel launch failed, ret="
                  << rtRet << std::endl;
        return TileXR::TILEXR_ERROR_MKIRT;
    }
    return TileXR::TILEXR_SUCCESS;
}

} // namespace TileXRMoonEp
