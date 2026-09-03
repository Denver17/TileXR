/*
 * Copyright (c) 2024-2026 TileXR Project
 * This file is a part of the CANN Open Software.
 * Licensed under CANN Open Software License Agreement Version 1.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#include "collective_kernel.h"

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <string>

#include "collective_utils.h"
#include "runtime/kernel.h"

extern "C" {
extern const unsigned char TileXRCollectivesKernelBinaryData[];
extern const std::size_t TileXRCollectivesKernelBinarySize;
}

namespace TileXRCollectives {
namespace Host {
namespace {

constexpr uint32_t LCCL_RT_DEV_BINARY_MAGIC_ELF_AIVEC = 0x41415246;
constexpr uint64_t FUNSIG_OFFSET_BITS = 16;
constexpr uint64_t FUNSIG_SKEW = 0x1000;

std::mutex g_registrationMutex;
bool g_registered = false;
int g_registrationStatus = TileXR::TILEXR_ERROR_NOT_INITIALIZED;
void *g_binaryHandle = nullptr;

struct DataTypeRegistration {
    TileXR::TileXRDataType dataType;
    const char *kernelTypeName;
};

const DataTypeRegistration kDataTypes[] = {
    { TileXR::TILEXR_DATA_TYPE_INT8, "int8_t" },
    { TileXR::TILEXR_DATA_TYPE_INT16, "int16_t" },
    { TileXR::TILEXR_DATA_TYPE_INT32, "int" },
    { TileXR::TILEXR_DATA_TYPE_INT64, "int64_t" },
    { TileXR::TILEXR_DATA_TYPE_FP16, "float16_t" },
    { TileXR::TILEXR_DATA_TYPE_FP32, "float" },
    { TileXR::TILEXR_DATA_TYPE_BFP16, "bfloat16_t" },
};

const TileXR::TileXRType kRegisteredCollectiveTypes[] = {
    TileXR::TileXRType::ALL_GATHER,
    TileXR::TileXRType::ALL2ALL,
    TileXR::TileXRType::ALL_REDUCE,
    TileXR::TileXRType::REDUCE_SCATTER,
    TileXR::TileXRType::BROADCAST,
};

bool IsStandaloneCollectiveType(TileXR::TileXRType type)
{
    return type == TileXR::TileXRType::ALL_GATHER ||
        type == TileXR::TileXRType::ALL2ALL ||
        type == TileXR::TileXRType::ALL_REDUCE ||
        type == TileXR::TileXRType::REDUCE_SCATTER ||
        type == TileXR::TileXRType::BROADCAST;
}

int8_t *GetFunSig(TileXR::TileXRType type, TileXR::TileXRDataType dataType)
{
    const uint64_t sig = (static_cast<uint64_t>(type) << FUNSIG_OFFSET_BITS << FUNSIG_OFFSET_BITS) +
        (static_cast<uint64_t>(dataType) << FUNSIG_OFFSET_BITS) + FUNSIG_SKEW;
    return reinterpret_cast<int8_t *>(sig);
}

std::string KernelName(TileXR::TileXRType type, const DataTypeRegistration &dataType)
{
    if (type == TileXR::TileXRType::BROADCAST) {
        return TileXR::TILEXR_TYPE2NAME.at(type);
    }
    return TileXR::TILEXR_TYPE2NAME.at(type) + "_" + dataType.kernelTypeName;
}

int RegisterCollectivesKernelsLocked()
{
    if (g_registered) {
        return TileXR::TILEXR_SUCCESS;
    }
    if (TileXRCollectivesKernelBinaryData == nullptr || TileXRCollectivesKernelBinarySize == 0) {
        g_registrationStatus = TileXR::TILEXR_ERROR_NOT_INITIALIZED;
        return g_registrationStatus;
    }

    rtDevBinary_t binary {};
    binary.data = TileXRCollectivesKernelBinaryData;
    binary.length = static_cast<uint32_t>(TileXRCollectivesKernelBinarySize);
    binary.magic = LCCL_RT_DEV_BINARY_MAGIC_ELF_AIVEC;
    binary.version = 0;

    void *binHandle = nullptr;
    rtError_t rtRet = rtDevBinaryRegister(&binary, &binHandle);
    if (rtRet != RT_ERROR_NONE) {
        std::cerr << "TileXR collectives rtDevBinaryRegister failed, ret=" << rtRet
                  << ", binarySize=" << TileXRCollectivesKernelBinarySize << std::endl;
        g_registrationStatus = TileXR::TILEXR_ERROR_MKIRT;
        return g_registrationStatus;
    }

    for (const auto type : kRegisteredCollectiveTypes) {
        for (const auto &dataType : kDataTypes) {
            const std::string name = KernelName(type, dataType);
            rtRet = rtFunctionRegister(binHandle, GetFunSig(type, dataType.dataType),
                name.c_str(), name.c_str(), 0);
            if (rtRet != RT_ERROR_NONE) {
                std::cerr << "TileXR collectives rtFunctionRegister failed, ret=" << rtRet
                          << ", kernel=" << name
                          << ", type=" << static_cast<int>(type)
                          << ", dataType=" << static_cast<int>(dataType.dataType) << std::endl;
                g_registrationStatus = TileXR::TILEXR_ERROR_MKIRT;
                return g_registrationStatus;
            }
        }
    }

    g_binaryHandle = binHandle;
    g_registered = true;
    g_registrationStatus = TileXR::TILEXR_SUCCESS;
    return TileXR::TILEXR_SUCCESS;
}

int EnsureCollectivesKernelsRegistered()
{
    std::lock_guard<std::mutex> guard(g_registrationMutex);
    return RegisterCollectivesKernelsLocked();
}

} // namespace

int LaunchCollectiveKernel(TileXRCommPtr comm, TileXR::TileXRType type, const HostLaunchContext &context,
                           void *sendBuf, void *recvBuf, int64_t kernelCount,
                           TileXR::TileXRDataType dataType, uint32_t blockDim,
                           aclrtStream stream,
                           CollectiveLaunchAttrs attrs)
{
    if (!IsStandaloneCollectiveType(type) ||
        comm == nullptr || context.hostArgs == nullptr || context.devArgs == nullptr || sendBuf == nullptr || recvBuf == nullptr ||
        kernelCount <= 0 || blockDim == 0 || !IsSupportedDataType(dataType)) {
        return TileXR::TILEXR_ERROR_PARA_CHECK_FAIL;
    }

    const int registerRet = EnsureCollectivesKernelsRegistered();
    if (registerRet != TileXR::TILEXR_SUCCESS) {
        return registerRet;
    }

    int64_t magic = 0;
    const int magicRet = TileXRCommNextMagic(comm, &magic);
    if (magicRet != TileXR::TILEXR_SUCCESS) {
        return magicRet;
    }

    AscendCCLKernelArgs args {};
    args.input = sendBuf;
    args.output = recvBuf;
    args.commArgsPtr = context.devArgs;
    args.count = kernelCount;
    args.magic = magic;
    args.op = attrs.op;
    args.root = attrs.root;

    rtTaskCfgInfo_t cfgInfo {};
    cfgInfo.schemMode = 1;

    rtArgsEx_t argsInfo {};
    argsInfo.args = &args;
    argsInfo.argsSize = sizeof(args);

    const rtError_t ret = rtKernelLaunchWithFlagV2(GetFunSig(type, dataType), blockDim, &argsInfo, nullptr,
        static_cast<rtStream_t>(stream), 0, &cfgInfo);
    if (ret != RT_ERROR_NONE) {
        std::cerr << "TileXR collectives rtKernelLaunchWithFlagV2 failed, ret=" << ret
                  << ", type=" << static_cast<int>(type)
                  << ", dataType=" << static_cast<int>(dataType)
                  << ", blockDim=" << blockDim
                  << ", kernelCount=" << kernelCount
                  << ", stream=" << stream << std::endl;
    }
    return ret == RT_ERROR_NONE ? TileXR::TILEXR_SUCCESS : TileXR::TILEXR_ERROR_MKIRT;
}

} // namespace Host
} // namespace TileXRCollectives
