#include "reduce_grad_host.h"

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <mutex>

#include "reduce_grad_common.h"
#include "runtime/kernel.h"
#include "tilexr_kernel_binary_loader.h"
#include "tilexr_types.h"

namespace TileXRMoonEp {
namespace {

const char kReduceGradKernelName[] = "tilexr_moonep_reduce_grad_kernel";
std::mutex g_reduceGradRegistrationMutex;
bool g_reduceGradRegistered = false;
void *g_reduceGradBinaryHandle = nullptr;

int EnsureReduceGradKernelRegistered()
{
    std::lock_guard<std::mutex> guard(g_reduceGradRegistrationMutex);
    if (g_reduceGradRegistered) {
        return TileXR::TILEXR_SUCCESS;
    }
    TileXR::KernelBinaryView kernelBinary {};
    if (!TileXR::LoadKernelBinary(&g_reduceGradRegistrationMutex,
            "tilexr_moonep_reduce_grad_kernel.o", &kernelBinary)) {
        return TileXR::TILEXR_ERROR_NOT_INITIALIZED;
    }

    rtDevBinary_t binary {};
    binary.data = kernelBinary.data;
    binary.length = static_cast<uint64_t>(kernelBinary.size);
    binary.magic = RT_DEV_BINARY_MAGIC_ELF_AIVEC;
    binary.version = 0;

    void *binaryHandle = nullptr;
    rtError_t rtRet = rtDevBinaryRegister(&binary, &binaryHandle);
    if (rtRet != RT_ERROR_NONE) {
        std::cerr << "MoonEP ReduceGrad rtDevBinaryRegister failed, ret=" << rtRet << std::endl;
        return TileXR::TILEXR_ERROR_MKIRT;
    }
    rtRet = rtFunctionRegister(binaryHandle, kReduceGradKernelName,
        kReduceGradKernelName, kReduceGradKernelName, 0);
    if (rtRet != RT_ERROR_NONE) {
        std::cerr << "MoonEP ReduceGrad rtFunctionRegister failed, ret=" << rtRet << std::endl;
        const rtError_t unregisterRet = rtDevBinaryUnRegister(binaryHandle);
        if (unregisterRet != RT_ERROR_NONE) {
            std::cerr << "MoonEP ReduceGrad rtDevBinaryUnRegister failed, ret="
                      << unregisterRet << std::endl;
        }
        return TileXR::TILEXR_ERROR_MKIRT;
    }
    g_reduceGradBinaryHandle = binaryHandle;
    g_reduceGradRegistered = true;
    return TileXR::TILEXR_SUCCESS;
}

} // namespace

int TileXRMoonEpLaunchReduceGradKernel(const ReduceGradLaunchParams &params,
    const ReduceGradPreparedContext &context)
{
    const int registerRet = EnsureReduceGradKernelRegistered();
    if (registerRet != TileXR::TILEXR_SUCCESS) {
        return registerRet;
    }

    int64_t magic = 0;
    const int ret = TileXRCommNextMagic(context.comm, &magic);
    if (ret != TileXR::TILEXR_SUCCESS) {
        return ret;
    }
    const ReduceGradLayout &layout = context.layout;
    static_assert(sizeof(ReduceGradKernelArgs) <= std::numeric_limits<uint32_t>::max(),
        "ReduceGrad kernel argument block exceeds Runtime V2 argsSize");
    ReduceGradKernelArgs args {};
    args.commArgs = context.devArgs;
    args.profileInfo = context.profileView.infoDev;
    args.profileRegistry = context.profileView.registryDev;
    args.expertsToCopy = reinterpret_cast<GM_ADDR>(context.expertsToCopy);
    args.workspace = reinterpret_cast<GM_ADDR>(context.workspace);
    args.status = reinterpret_cast<GM_ADDR>(params.status->data);
    args.rank = layout.rank;
    args.rankSize = layout.rankSize;
    args.expertCount = layout.expertCount;
    args.expertsPerRank = layout.expertsPerRank;
    args.prefetchSlots = layout.prefetchSlots;
    args.transportQpCount = layout.transportQpCount;
    args.qpCount = layout.qpCount;
    args.laneCount = layout.laneCount;
    args.barrierPhysicalQp = layout.barrierPhysicalQp;
    args.laneStateBytes = layout.laneStateBytes;
    args.stagingOffset = layout.stagingOffset;
    args.bankStrideBytes = layout.bankStrideBytes;
    args.laneStrideBytes = layout.laneStrideBytes;
    args.chunkBytes = layout.chunkBytes;
    args.barrierOffset = layout.barrierOffset;
    args.barrierBytes = layout.barrierBytes;
    args.workspaceBytes = layout.workspaceBytes;
    args.waitIterations = params.waitIterations;
    args.magic = magic;
    for (uint32_t projection = 0; projection < kReduceGradProjectionCount; ++projection) {
        args.gradients[projection] = reinterpret_cast<GM_ADDR>(
            context.gradients[projection].data);
        args.sources[projection] = reinterpret_cast<GM_ADDR>(
            context.sources[projection].data);
        args.rowElements[projection] = layout.rowElements[projection];
        args.rowBytes[projection] = layout.rowBytes[projection];
        args.chunkCounts[projection] = layout.chunkCounts[projection];
        args.projectionQpBase[projection] = layout.projectionQpBase[projection];
        args.projectionQpCounts[projection] = layout.projectionQpCounts[projection];
    }
    for (uint32_t lane = 0; lane < layout.laneCount; ++lane) {
        args.lanePhysicalQps[lane] = layout.lanePhysicalQps[lane];
    }

    rtArgsEx_t argsInfo {};
    argsInfo.args = &args;
    argsInfo.argsSize = static_cast<uint32_t>(sizeof(args));
    rtTaskCfgInfo_t cfgInfo {};
    cfgInfo.schemMode = 1;
    const rtError_t launchRet = rtKernelLaunchWithFlagV2(kReduceGradKernelName,
        static_cast<uint32_t>(layout.blockDim), &argsInfo, nullptr,
        static_cast<rtStream_t>(params.stream), 0, &cfgInfo);
    return launchRet == RT_ERROR_NONE ? TileXR::TILEXR_SUCCESS : TileXR::TILEXR_ERROR_MKIRT;
}

} // namespace TileXRMoonEp
