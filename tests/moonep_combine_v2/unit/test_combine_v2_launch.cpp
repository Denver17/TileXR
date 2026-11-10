/*
 * 拦截 Runtime launch，验证 Combine V2 参数 ABI、动态 UB 配置及错误拒绝。
 */

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>

#include "acl/acl_rt.h"
#include "combine_v2_launch.h"
#include "moonep_kernel_registration.h"
#include "runtime/kernel.h"
#include "tilexr_types.h"

namespace {

constexpr int64_t kA5UbBytes = 248 * 1024;
constexpr int64_t kMinUbBytes = 216 * 1024;

int failures = 0;
aclError getDeviceReturn = ACL_SUCCESS;
aclError getDeviceInfoReturn = ACL_SUCCESS;
int64_t dynamicUbBytes = kA5UbBytes;
int registrationReturn = TileXR::TILEXR_SUCCESS;
rtError_t launchReturn = RT_ERROR_NONE;
int registrationCalls = 0;
int launchCalls = 0;
uint32_t capturedBlockDim = 0;
std::size_t capturedArgsSize = 0;
constexpr std::size_t kKernelArgCount = 28U;
uint64_t capturedArgs[kKernelArgCount] = {};
rtTaskCfgInfo_t capturedCfg {};

void Check(bool condition, const char *message)
{
    if (!condition) {
        std::cerr << message << '\n';
        ++failures;
    }
}

void CheckStatus(int actual, int expected, const char *message)
{
    if (actual != expected) {
        std::cerr << message << ": expected " << expected
                  << ", got " << actual << '\n';
        ++failures;
    }
}

void Reset()
{
    getDeviceReturn = ACL_SUCCESS;
    getDeviceInfoReturn = ACL_SUCCESS;
    dynamicUbBytes = kA5UbBytes;
    registrationReturn = TileXR::TILEXR_SUCCESS;
    launchReturn = RT_ERROR_NONE;
    registrationCalls = 0;
    launchCalls = 0;
    capturedBlockDim = 0;
    capturedArgsSize = 0;
    for (uint64_t &value : capturedArgs) {
        value = 0;
    }
    capturedCfg = rtTaskCfgInfo_t {};
}

TileXRMoonEp::CombineV2Params ValidParams(uint64_t *activeOutputOffset)
{
    TileXRMoonEp::CombineV2Params params {};
    params.registeredWorkspace = reinterpret_cast<void *>(uintptr_t {0x1000});
    params.dstLocal = reinterpret_cast<const int32_t *>(uintptr_t {0x2000});
    params.bs = 8;
    params.h = 3584;
    params.topK = 16;
    params.nvS = 128;
    params.aivCoreNum = TileXRMoonEp::kMoonEpCombineV2CoreCount;
    params.activeOutputOffset = activeOutputOffset;
    params.dtype = TILEXR_MOONEP_DTYPE_BFLOAT16;
    params.reduceHidden = true;
    params.stream = reinterpret_cast<aclrtStream>(uintptr_t {0x3000});
    return params;
}

TileXRMoonEp::CombineV2LaunchContext ValidContext()
{
    TileXRMoonEp::CombineV2LaunchContext context {};
    context.devArgs = reinterpret_cast<GM_ADDR>(uintptr_t {0x4000});
    context.layout.scratchOffset[0] = 4096;
    context.layout.scratchOffset[1] = 8192;
    context.layout.collectiveStatusOffset = 20480;
    context.layout.outputOffset = 24576;
    context.layout.rowBytes = 7168;
    context.magic = 1;
    return context;
}

void TestConfiguresDynamicUb()
{
    Reset();
    uint64_t activeOutputOffset = 0;
    const TileXRMoonEp::CombineV2Params params =
        ValidParams(&activeOutputOffset);
    const TileXRMoonEp::CombineV2LaunchContext context = ValidContext();

    CheckStatus(TileXRMoonEp::TileXRMoonEpLaunchCombineV2Kernel(params, context),
        TileXR::TILEXR_SUCCESS, "configured launch");
    Check(registrationCalls == 1 && launchCalls == 1,
        "configured launch did not reach Runtime");
    Check(capturedBlockDim == TileXRMoonEp::kMoonEpCombineV2CoreCount,
        "configured launch did not use the fixed 16-block dimension");
    Check(capturedCfg.schemMode == 1,
        "configured launch did not preserve batch scheduling mode");
    Check(capturedCfg.localMemorySize == static_cast<uint32_t>(kA5UbBytes),
        "configured launch did not pass the device UB size");
    Check(capturedArgsSize == sizeof(capturedArgs),
        "configured launch used the wrong kernel ABI size");
    Check(capturedArgs[6] == 0U && capturedArgs[9] == 0U &&
        capturedArgs[10] == 0U &&
        capturedArgs[11] == context.layout.collectiveStatusOffset &&
        capturedArgs[12] == context.layout.outputOffset &&
        capturedArgs[17] == context.layout.rowBytes &&
        capturedArgs[18] == 1U &&
        capturedArgs[19] == static_cast<uint64_t>(context.magic) &&
        capturedArgs[20] == 0U && capturedArgs[21] == 0U &&
        capturedArgs[22] == 0U && capturedArgs[27] == 0U,
        "configured launch did not pass kernel arguments");
    Check(activeOutputOffset == context.layout.scratchOffset[1],
        "configured launch did not publish the active epoch");
}

void TestRejectsUnavailableDevice()
{
    Reset();
    getDeviceReturn = 1;
    uint64_t activeOutputOffset = 0;
    CheckStatus(TileXRMoonEp::TileXRMoonEpLaunchCombineV2Kernel(
        ValidParams(&activeOutputOffset), ValidContext()),
        TileXR::TILEXR_ERROR_INTERNAL, "device query failure");
    Check(registrationCalls == 0 && launchCalls == 0,
        "device query failure still launched the kernel");
}

void TestRejectsInvalidDynamicUb()
{
    Reset();
    getDeviceInfoReturn = 1;
    uint64_t activeOutputOffset = 0;
    CheckStatus(TileXRMoonEp::TileXRMoonEpLaunchCombineV2Kernel(
        ValidParams(&activeOutputOffset), ValidContext()),
        TileXR::TILEXR_ERROR_PARA_CHECK_FAIL, "UB query failure");

    Reset();
    dynamicUbBytes = kMinUbBytes - 1;
    CheckStatus(TileXRMoonEp::TileXRMoonEpLaunchCombineV2Kernel(
        ValidParams(&activeOutputOffset), ValidContext()),
        TileXR::TILEXR_ERROR_PARA_CHECK_FAIL, "insufficient dynamic UB");
    Check(registrationCalls == 0 && launchCalls == 0,
        "invalid dynamic UB still launched the kernel");
}

} // namespace

extern "C" {
aclError aclrtGetDevice(int32_t *deviceId)
{
    if (getDeviceReturn == ACL_SUCCESS && deviceId != nullptr) {
        *deviceId = 0;
    }
    return getDeviceReturn;
}

aclError aclrtGetDeviceInfo(uint32_t, aclrtDevAttr attr, int64_t *value)
{
    if (attr != ACL_DEV_ATTR_UBUF_PER_VECTOR_CORE) {
        return 1;
    }
    if (getDeviceInfoReturn == ACL_SUCCESS && value != nullptr) {
        *value = dynamicUbBytes;
    }
    return getDeviceInfoReturn;
}

rtError_t rtKernelLaunchWithFlagV2(const void *, uint32_t blockDim,
    rtArgsEx_t *argsInfo,
    void *, rtStream_t, uint32_t, const rtTaskCfgInfo_t *cfgInfo)
{
    ++launchCalls;
    capturedBlockDim = blockDim;
    if (argsInfo != nullptr) {
        capturedArgsSize = argsInfo->argsSize;
        if (argsInfo->args != nullptr && argsInfo->argsSize ==
                sizeof(capturedArgs)) {
            const uint64_t *args = static_cast<const uint64_t *>(argsInfo->args);
            for (std::size_t index = 0; index < kKernelArgCount; ++index) {
                capturedArgs[index] = args[index];
            }
        }
    }
    if (cfgInfo != nullptr) {
        capturedCfg = *cfgInfo;
    }
    return launchReturn;
}
} // extern "C"

namespace TileXRMoonEp {

int EnsureMoonEpKernelRegistered(KernelRegistrationState &,
    const char *, const void *, const char *)
{
    ++registrationCalls;
    return registrationReturn;
}

} // namespace TileXRMoonEp

int main()
{
    TestConfiguresDynamicUb();
    TestRejectsUnavailableDevice();
    TestRejectsInvalidDynamicUb();
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
