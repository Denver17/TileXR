#ifndef TILEXR_MOONEP_KERNEL_LAUNCH_H
#define TILEXR_MOONEP_KERNEL_LAUNCH_H

#include <cstddef>
#include <cstdint>
#include <iostream>

#include "moonep_kernel_registration.h"
#include "runtime/kernel.h"
#include "tilexr_types.h"

namespace TileXRMoonEp {

inline int LaunchRegisteredMoonEpKernel(KernelRegistrationState &state,
    const char *binaryFileName, const void *signature,
    const char *kernelName, const char *stageName,
    uint32_t blockDim, void *args, std::size_t argsSize, rtStream_t stream,
    const rtTaskCfgInfo_t *taskCfgInfo = nullptr)
{
    const int registerRet = EnsureMoonEpKernelRegistered(
        state, binaryFileName, signature, kernelName);
    if (registerRet != TileXR::TILEXR_SUCCESS) {
        return registerRet;
    }

    rtArgsEx_t argsInfo {};
    argsInfo.args = args;
    argsInfo.argsSize = argsSize;
    rtTaskCfgInfo_t cfgInfo {};
    if (taskCfgInfo != nullptr) {
        cfgInfo = *taskCfgInfo;
    }
    cfgInfo.schemMode = 1;

    const rtError_t ret = rtKernelLaunchWithFlagV2(
        signature, blockDim, &argsInfo, nullptr, stream, 0, &cfgInfo);
    if (ret != RT_ERROR_NONE) {
        std::cerr << "TileXR MoonEP " << stageName
                  << " rtKernelLaunchWithFlagV2 failed, ret=" << ret << std::endl;
        return TileXR::TILEXR_ERROR_MKIRT;
    }
    return TileXR::TILEXR_SUCCESS;
}

} // namespace TileXRMoonEp

#endif // TILEXR_MOONEP_KERNEL_LAUNCH_H
