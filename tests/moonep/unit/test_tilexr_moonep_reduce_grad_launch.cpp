#include <cstddef>
#include <cstdint>
#include <iostream>
#include <string>

#include "reduce_grad_common.h"
#include "reduce_grad_host.h"
#include "runtime/kernel.h"
#include "tilexr_types.h"

#ifndef UINTPTR_C
#define UINTPTR_C(value) static_cast<uintptr_t>(value)
#endif

extern "C" {
extern const unsigned char TileXRMoonEpReduceGradKernelBinaryData[] = {0x7f, 'E', 'L', 'F'};
extern const std::size_t TileXRMoonEpReduceGradKernelBinarySize =
    sizeof(TileXRMoonEpReduceGradKernelBinaryData);
}

namespace {

int g_failures = 0;
int g_magicReturn = TileXR::TILEXR_SUCCESS;
int g_magicCalls = 0;
int64_t g_magic = 77;
rtError_t g_binaryRegisterReturn = RT_ERROR_NONE;
rtError_t g_binaryUnregisterReturn = RT_ERROR_NONE;
rtError_t g_functionRegisterReturn = RT_ERROR_NONE;
rtError_t g_launchReturn = RT_ERROR_NONE;
int g_binaryRegisterCalls = 0;
int g_binaryUnregisterCalls = 0;
int g_functionRegisterCalls = 0;
int g_launchCalls = 0;
rtDevBinary_t g_binary {};
void *g_binaryHandle = reinterpret_cast<void *>(UINTPTR_C(0xa000));
void *g_unregisteredBinaryHandle = nullptr;
const void *g_functionStub = nullptr;
std::string g_functionStubName;
std::string g_functionKernelInfo;
const void *g_launchKernel = nullptr;
uint32_t g_blockDim = 0;
rtStream_t g_stream = nullptr;
uint32_t g_launchFlags = UINT32_MAX;
int g_schemMode = -1;
size_t g_argsSize = 0;
TileXRMoonEp::ReduceGradKernelArgs g_args {};

void Check(bool condition, const char *message)
{
    if (!condition) {
        std::cerr << message << std::endl;
        ++g_failures;
    }
}

TileXRMoonEpTensorV1 Tensor(void *data)
{
    TileXRMoonEpTensorV1 tensor {};
    tensor.data = data;
    return tensor;
}

void TestLaunch()
{
    TileXRMoonEpPlanV1 plan {};
    plan.expertsToCopy = reinterpret_cast<void *>(UINTPTR_C(0x2000));
    TileXRMoonEpTensorV1 gate = Tensor(reinterpret_cast<void *>(UINTPTR_C(0x3000)));
    TileXRMoonEpTensorV1 up = Tensor(reinterpret_cast<void *>(UINTPTR_C(0x4000)));
    TileXRMoonEpTensorV1 down = Tensor(reinterpret_cast<void *>(UINTPTR_C(0x5000)));
    TileXRMoonEpTensorV1 status = Tensor(reinterpret_cast<void *>(UINTPTR_C(0x6000)));

    TileXRMoonEp::ReduceGradLaunchParams params {};
    params.plan = &plan;
    params.gradients[0] = &gate;
    params.gradients[1] = &up;
    params.gradients[2] = &down;
    params.status = &status;
    params.waitIterations = 123;
    params.stream = reinterpret_cast<aclrtStream>(UINTPTR_C(0x8000));

    TileXRMoonEp::ReduceGradPreparedContext context {};
    context.comm = reinterpret_cast<TileXRCommPtr>(UINTPTR_C(0x1000));
    context.devArgs = reinterpret_cast<GM_ADDR>(UINTPTR_C(0x9000));
    context.profileView.infoDev = reinterpret_cast<GM_ADDR>(UINTPTR_C(0xa000));
    context.profileView.registryDev = reinterpret_cast<GM_ADDR>(UINTPTR_C(0xb000));
    context.expertsToCopy = plan.expertsToCopy;
    context.workspace = reinterpret_cast<void *>(UINTPTR_C(0x7000));
    context.workspaceBytes = 900;
    context.gradients[0] = gate;
    context.gradients[1] = up;
    context.gradients[2] = down;
    for (uint32_t projection = 0; projection < 3; ++projection) {
        context.sources[projection].data = reinterpret_cast<void *>(
            UINTPTR_C(0xc000) + projection * UINTPTR_C(0x1000));
        context.sources[projection].bytes = 100 + projection;
    }

    auto &layout = context.layout;
    layout.rank = 1;
    layout.rankSize = 4;
    layout.expertCount = 8;
    layout.expertsPerRank = 2;
    layout.prefetchSlots = 1;
    layout.blockDim = 64;
    layout.rowElements[0] = 10;
    layout.rowElements[1] = 20;
    layout.rowElements[2] = 30;
    layout.rowBytes[0] = 40;
    layout.rowBytes[1] = 80;
    layout.rowBytes[2] = 120;
    layout.chunkCounts[0] = 1;
    layout.chunkCounts[1] = 2;
    layout.chunkCounts[2] = 3;
    layout.projectionQpBase[0] = 0;
    layout.projectionQpBase[1] = 3;
    layout.projectionQpBase[2] = 6;
    layout.projectionQpCounts[0] = 3;
    layout.projectionQpCounts[1] = 3;
    layout.projectionQpCounts[2] = 2;
    layout.transportQpCount = 48;
    for (uint32_t lane = 0; lane < 8; ++lane) {
        layout.lanePhysicalQps[lane] = lane == 7 ? 16 : lane;
    }
    layout.qpCount = 8;
    layout.laneCount = 8;
    layout.barrierPhysicalQp = 47;
    layout.laneStateBytes = 32;
    layout.stagingOffset = 32;
    layout.bankStrideBytes = 64;
    layout.laneStrideBytes = 128;
    layout.chunkBytes = 16;
    layout.barrierOffset = 700;
    layout.barrierBytes = 200;
    layout.workspaceBytes = 900;

    g_binaryRegisterReturn = -10;
    Check(TileXRMoonEp::TileXRMoonEpLaunchReduceGradKernel(params, context) ==
        TileXR::TILEXR_ERROR_MKIRT,
        "ReduceGrad binary registration error was not translated");
    Check(g_binaryRegisterCalls == 1 && g_functionRegisterCalls == 0 &&
        g_launchCalls == 0 && g_magicCalls == 0,
        "binary registration failure did not stop launch");

    g_binaryRegisterReturn = RT_ERROR_NONE;
    g_functionRegisterReturn = -11;
    Check(TileXRMoonEp::TileXRMoonEpLaunchReduceGradKernel(params, context) ==
        TileXR::TILEXR_ERROR_MKIRT,
        "ReduceGrad function registration error was not translated");
    Check(g_binaryRegisterCalls == 2 && g_functionRegisterCalls == 1 &&
        g_binaryUnregisterCalls == 1 && g_launchCalls == 0 && g_magicCalls == 0,
        "function registration failure did not release the binary");
    Check(g_unregisteredBinaryHandle == g_binaryHandle,
        "function registration released the wrong binary handle");

    g_functionRegisterReturn = RT_ERROR_NONE;
    Check(TileXRMoonEp::TileXRMoonEpLaunchReduceGradKernel(params, context) ==
        TileXR::TILEXR_SUCCESS, "ReduceGrad runtime launch failed");
    Check(g_binaryRegisterCalls == 3 && g_functionRegisterCalls == 2 &&
        g_launchCalls == 1 && g_magicCalls == 1,
        "successful registration/launch counts mismatch");
    Check(g_binary.data == TileXRMoonEpReduceGradKernelBinaryData &&
        g_binary.length == TileXRMoonEpReduceGradKernelBinarySize &&
        g_binary.magic == RT_DEV_BINARY_MAGIC_ELF_AIVEC,
        "embedded pure-AIV binary descriptor mismatch");
    Check(g_functionStub != nullptr &&
        g_functionStubName == "tilexr_moonep_reduce_grad_kernel" &&
        g_functionKernelInfo == "tilexr_moonep_reduce_grad_kernel",
        "registered function signature mismatch");
    Check(g_launchKernel == g_functionStub && g_blockDim == 64 &&
        g_stream == params.stream && g_launchFlags == 0 && g_schemMode == 1,
        "runtime launch metadata mismatch");
    Check(g_argsSize == sizeof(TileXRMoonEp::ReduceGradKernelArgs),
        "kernel args size ABI mismatch");
    Check(g_args.commArgs == context.devArgs &&
        g_args.profileInfo == context.profileView.infoDev &&
        g_args.profileRegistry == context.profileView.registryDev &&
        g_args.expertsToCopy == plan.expertsToCopy &&
        g_args.workspace == context.workspace && g_args.status == status.data,
        "owner-pull pointer args mismatch");
    Check(g_args.gradients[0] == gate.data && g_args.gradients[1] == up.data &&
        g_args.gradients[2] == down.data &&
        g_args.sources[0] == context.sources[0].data &&
        g_args.sources[1] == context.sources[1].data &&
        g_args.sources[2] == context.sources[2].data,
        "gradient/source slice args mismatch");
    Check(g_args.rank == 1 && g_args.rankSize == 4 && g_args.expertCount == 8 &&
        g_args.expertsPerRank == 2 && g_args.prefetchSlots == 1,
        "rank args mismatch");
    Check(g_args.rowElements[0] == 10 && g_args.rowElements[1] == 20 &&
        g_args.rowElements[2] == 30 && g_args.rowBytes[0] == 40 &&
        g_args.rowBytes[1] == 80 && g_args.rowBytes[2] == 120 &&
        g_args.chunkCounts[2] == 3,
        "projection shape args mismatch");
    Check(g_args.projectionQpBase[1] == 3 &&
        g_args.projectionQpCounts[2] == 2 && g_args.qpCount == 8 &&
        g_args.laneCount == 8 && g_args.transportQpCount == 48 &&
        g_args.lanePhysicalQps[7] == 16 && g_args.barrierPhysicalQp == 47,
        "QP allocation args mismatch");
    Check(g_args.laneStateBytes == 32 && g_args.stagingOffset == 32 &&
        g_args.bankStrideBytes == 64 && g_args.laneStrideBytes == 128 &&
        g_args.chunkBytes == 16 && g_args.barrierOffset == 700 &&
        g_args.barrierBytes == 200 && g_args.workspaceBytes == 900 &&
        g_args.waitIterations == 123 && g_args.magic == g_magic,
        "workspace/timing args mismatch");

    g_launchReturn = -12;
    Check(TileXRMoonEp::TileXRMoonEpLaunchReduceGradKernel(params, context) ==
        TileXR::TILEXR_ERROR_MKIRT, "launch error was not translated");
    Check(g_binaryRegisterCalls == 3 && g_functionRegisterCalls == 2 &&
        g_launchCalls == 2 && g_magicCalls == 2,
        "successful kernel registration was not cached");

    g_launchReturn = RT_ERROR_NONE;
    g_magicReturn = -91;
    Check(TileXRMoonEp::TileXRMoonEpLaunchReduceGradKernel(params, context) == -91,
        "magic error was not propagated");
    Check(g_launchCalls == 2 && g_magicCalls == 3,
        "magic failure did not stop launch");
}

} // namespace

extern "C" int TileXRCommNextMagic(TileXRCommPtr, int64_t *magic)
{
    ++g_magicCalls;
    if (magic != nullptr) {
        *magic = g_magic;
    }
    return g_magicReturn;
}

extern "C" rtError_t rtDevBinaryRegister(const rtDevBinary_t *binary, void **handle)
{
    ++g_binaryRegisterCalls;
    if (binary != nullptr) {
        g_binary = *binary;
    }
    if (g_binaryRegisterReturn == RT_ERROR_NONE && handle != nullptr) {
        *handle = g_binaryHandle;
    }
    return g_binaryRegisterReturn;
}

extern "C" rtError_t rtDevBinaryUnRegister(void *handle)
{
    ++g_binaryUnregisterCalls;
    g_unregisteredBinaryHandle = handle;
    return g_binaryUnregisterReturn;
}

extern "C" rtError_t rtFunctionRegister(void *, const void *stubFunc,
    const char_t *stubName, const void *kernelInfoExt, uint32_t)
{
    ++g_functionRegisterCalls;
    g_functionStub = stubFunc;
    g_functionStubName = stubName == nullptr ? "" : stubName;
    g_functionKernelInfo = kernelInfoExt == nullptr ? "" :
        static_cast<const char *>(kernelInfoExt);
    return g_functionRegisterReturn;
}

extern "C" rtError_t rtKernelLaunchWithFlagV2(const void *kernel, uint32_t blockDim,
    rtArgsEx_t *argsInfo, rtSmDesc_t *, rtStream_t stream, uint32_t flags,
    const rtTaskCfgInfo_t *cfgInfo)
{
    ++g_launchCalls;
    g_launchKernel = kernel;
    g_blockDim = blockDim;
    g_stream = stream;
    g_launchFlags = flags;
    g_schemMode = cfgInfo == nullptr ? -1 : cfgInfo->schemMode;
    g_argsSize = argsInfo == nullptr ? 0 : argsInfo->argsSize;
    if (argsInfo != nullptr && argsInfo->args != nullptr &&
        argsInfo->argsSize == sizeof(TileXRMoonEp::ReduceGradKernelArgs)) {
        g_args = *static_cast<const TileXRMoonEp::ReduceGradKernelArgs *>(argsInfo->args);
    }
    return g_launchReturn;
}

int main()
{
    TestLaunch();
    return g_failures == 0 ? 0 : 1;
}
