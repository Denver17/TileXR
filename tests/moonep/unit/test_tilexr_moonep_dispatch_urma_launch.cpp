/*
 * 验证 MoonEP 的布局、调用或诊断合同；直接覆盖对应 src/moonep 与 tools/moonep 接口，硬件数据面另行验收。
 */
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>

#include "acl/acl_rt.h"
#include "dispatch_launch.h"
#include "dispatch_layout.h"
#include "runtime/kernel.h"
#include "tilexr_kernel_binary_loader.h"
#include "tilexr_types.h"

namespace {

const unsigned char kDispatchUrmaKernelBinary[] = {
    0x7f, 'E', 'L', 'F'};

} // namespace

namespace TileXR {

bool LoadKernelBinary(const void *, const char *fileName,
    KernelBinaryView *binary)
{
    if (fileName == nullptr || binary == nullptr ||
        std::strcmp(fileName, "tilexr_moonep_dispatch_urma_kernel.o") != 0) {
        return false;
    }
    binary->data = kDispatchUrmaKernelBinary;
    binary->size = sizeof(kDispatchUrmaKernelBinary);
    return true;
}

} // namespace TileXR

namespace {

int failures = 0;
int magicCalls = 0;
int binaryRegisterCalls = 0;
int functionRegisterCalls = 0;
int launchCalls = 0;
int magicReturn = TileXR::TILEXR_SUCCESS;
int64_t nextMagic = 41;
rtError_t launchReturn = RT_ERROR_NONE;
uint32_t launchedBlockDim = 0U;
size_t launchedArgsSize = 0U;
uint32_t launchedLocalMemorySize = 0U;
TileXRMoonEp::DispatchKernelArgs launchedArgs {};

void Check(bool condition, const char *message)
{
    if (!condition) {
        std::cerr << message << std::endl;
        ++failures;
    }
}

TileXRMoonEp::DispatchUrmaLaunchParams Params(bool paired)
{
    TileXRMoonEp::DispatchUrmaLaunchParams params {};
    params.commArgs = reinterpret_cast<GM_ADDR>(uintptr_t {0x1000});
    params.hiddenInput = reinterpret_cast<void *>(uintptr_t {0x2000});
    params.weightInput = paired ?
        reinterpret_cast<void *>(uintptr_t {0x3000}) : nullptr;
    params.dst = reinterpret_cast<int32_t *>(uintptr_t {0x4000});
    params.zeroFillRanges = reinterpret_cast<int32_t *>(uintptr_t {0x5000});
    params.workspace = reinterpret_cast<void *>(uintptr_t {0x6000});
    params.hiddenOutput = reinterpret_cast<void *>(uintptr_t {0x7000});
    params.weightOutput = paired ?
        reinterpret_cast<void *>(uintptr_t {0x8000}) : nullptr;
    params.planStatus = reinterpret_cast<int32_t *>(uintptr_t {0x9000});
    params.comm = reinterpret_cast<TileXRCommPtr>(uintptr_t {0xa000});
    params.stream = reinterpret_cast<aclrtStream>(uintptr_t {0xb000});
    params.peerMode = TileXRMoonEp::DispatchPeerMode::Legacy;
    params.groupWidth = TileXRMoonEp::kDispatchDefaultGroupWidth;
    params.zeroFillRangeCount = 10;
    Check(TileXRMoonEp::TileXRMoonEpBuildDispatchUrmaLayout(
        4, 2, 2, 17, 4, &params.layout) == TileXR::TILEXR_SUCCESS,
        "failed to build launcher test layout");
    return params;
}

void TestPairedAndHiddenOnly()
{
    TileXRMoonEp::DispatchUrmaLaunchParams paired = Params(true);
    Check(TileXRMoonEp::TileXRMoonEpLaunchDispatchUrmaKernel(paired) ==
        TileXR::TILEXR_SUCCESS, "paired launcher failed");
    Check(magicCalls == 1 && launchCalls == 1,
        "paired launcher must take one magic and launch once");
    Check(binaryRegisterCalls == 1 && functionRegisterCalls == 1,
        "paired launcher did not register the external kernel once");
    Check(launchedArgsSize == sizeof(TileXRMoonEp::DispatchKernelArgs) &&
        launchedBlockDim == TileXRMoonEp::kDispatchAivCoreCount,
        "paired launch metadata mismatch");
    Check(launchedArgs.hiddenInput == paired.hiddenInput &&
        launchedArgs.weightInput == paired.weightInput &&
        launchedArgs.hiddenOutput == paired.hiddenOutput &&
        launchedArgs.weightOutput == paired.weightOutput,
        "paired Kernel ABI pointer order mismatch");
    Check(launchedArgs.hiddenSourceOffset == paired.layout.hidden.sourceOffset &&
        launchedArgs.hiddenScratchOffset == paired.layout.hidden.scratchOffset &&
        launchedArgs.hiddenRowBytes == paired.layout.hidden.rowBytes &&
        launchedArgs.weightSourceOffset == paired.layout.weight.sourceOffset &&
        launchedArgs.weightScratchOffset == paired.layout.weight.scratchOffset &&
        launchedArgs.weightRowBytes == paired.layout.weight.rowBytes,
        "paired Kernel ABI active layout order mismatch");
    Check(launchedArgs.hiddenDfxOffset == paired.layout.hiddenDfxOffset &&
        launchedArgs.weightDfxOffset == paired.layout.weightDfxOffset &&
        launchedArgs.hasWeight == 1U && launchedArgs.magic == nextMagic,
        "paired Kernel ABI diagnostics or epoch fields mismatch");
    Check(launchedLocalMemorySize >= 190U * 1024U,
        "paired launch did not configure the required dynamic UB");

    TileXRMoonEp::DispatchUrmaLaunchParams hidden = Params(false);
    nextMagic = 42;
    Check(TileXRMoonEp::TileXRMoonEpLaunchDispatchUrmaKernel(hidden) ==
        TileXR::TILEXR_SUCCESS, "hidden-only launcher failed");
    Check(magicCalls == 2 && launchCalls == 2 &&
        binaryRegisterCalls == 1 && functionRegisterCalls == 1,
        "hidden-only launch did not reuse one registered function and one epoch");
    Check(launchedArgs.weightInput == nullptr &&
        launchedArgs.weightOutput == nullptr && launchedArgs.hasWeight == 0U &&
        launchedArgs.magic == nextMagic,
        "hidden-only Kernel ABI optional Weight contract mismatch");
}

void TestFailureBoundaries()
{
    TileXRMoonEp::DispatchUrmaLaunchParams params = Params(true);
    params.groupWidth = 7U;
    Check(TileXRMoonEp::TileXRMoonEpLaunchDispatchUrmaKernel(params) ==
        TileXR::TILEXR_ERROR_PARA_CHECK_FAIL,
        "invalid group width was accepted");
    Check(magicCalls == 2 && launchCalls == 2,
        "Host-detectable configuration error consumed an epoch");

    params.groupWidth = TileXRMoonEp::kDispatchDefaultGroupWidth;
    magicReturn = -91;
    Check(TileXRMoonEp::TileXRMoonEpLaunchDispatchUrmaKernel(params) == -91,
        "magic failure was not propagated");
    Check(magicCalls == 3 && launchCalls == 2,
        "magic failure must stop before runtime launch");

    magicReturn = TileXR::TILEXR_SUCCESS;
    launchReturn = -92;
    Check(TileXRMoonEp::TileXRMoonEpLaunchDispatchUrmaKernel(params) ==
        TileXR::TILEXR_ERROR_MKIRT, "runtime launch failure was not mapped");
    Check(magicCalls == 4 && launchCalls == 3,
        "runtime launch failure call counts mismatch");

    params.weightOutput = nullptr;
    Check(TileXRMoonEp::TileXRMoonEpLaunchDispatchUrmaKernel(params) ==
        TileXR::TILEXR_ERROR_PARA_CHECK_FAIL,
        "unpaired Weight pointers were accepted");
    Check(magicCalls == 4 && launchCalls == 3,
        "pointer validation must happen before the epoch starts");
}

} // namespace

extern "C" int TileXRCommNextMagic(TileXRCommPtr, int64_t *magic)
{
    ++magicCalls;
    if (magicReturn == TileXR::TILEXR_SUCCESS && magic != nullptr) {
        *magic = nextMagic;
    }
    return magicReturn;
}

extern "C" aclError aclrtGetDevice(int32_t *deviceId)
{
    if (deviceId != nullptr) {
        *deviceId = 0;
    }
    return ACL_SUCCESS;
}

extern "C" aclError aclrtGetDeviceInfo(uint32_t, aclrtDevAttr attr, int64_t *value)
{
    if (value != nullptr) {
        *value = attr == ACL_DEV_ATTR_UBUF_PER_VECTOR_CORE ?
            192U * 1024U : TileXRMoonEp::kDispatchAivCoreCount;
    }
    return ACL_SUCCESS;
}

extern "C" rtError_t rtDevBinaryRegister(const rtDevBinary_t *, void **handle)
{
    ++binaryRegisterCalls;
    if (handle != nullptr) {
        *handle = reinterpret_cast<void *>(uintptr_t {0xc000});
    }
    return RT_ERROR_NONE;
}

extern "C" rtError_t rtDevBinaryUnRegister(void *)
{
    return RT_ERROR_NONE;
}

extern "C" rtError_t rtFunctionRegister(void *, const void *, const char_t *,
    const void *, uint32_t)
{
    ++functionRegisterCalls;
    return RT_ERROR_NONE;
}

extern "C" rtError_t rtKernelLaunchWithFlagV2(const void *, uint32_t blockDim,
    rtArgsEx_t *argsInfo, rtSmDesc_t *, rtStream_t, uint32_t,
    const rtTaskCfgInfo_t *cfgInfo)
{
    ++launchCalls;
    launchedBlockDim = blockDim;
    launchedArgsSize = argsInfo == nullptr ? 0U : argsInfo->argsSize;
    launchedLocalMemorySize = cfgInfo == nullptr ? 0U : cfgInfo->localMemorySize;
    if (argsInfo != nullptr && argsInfo->args != nullptr &&
        argsInfo->argsSize == sizeof(TileXRMoonEp::DispatchKernelArgs)) {
        launchedArgs = *static_cast<TileXRMoonEp::DispatchKernelArgs *>(
            argsInfo->args);
    }
    return launchReturn;
}

int main()
{
    TestPairedAndHiddenOnly();
    TestFailureBoundaries();
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
