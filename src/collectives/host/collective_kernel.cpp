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

#include "acl/acl_rt.h"
#include "collective_utils.h"
#include "runtime/kernel.h"
#include "tilexr_kernel_binary_loader.h"

namespace TileXRCollectives {
namespace Host {
namespace {

int ConfigureSimtMemory(rtTaskCfgInfo_t &cfg)
{
    int32_t device = 0;
    int64_t totalUbBytes = 0;
    if (aclrtGetDevice(&device) != ACL_SUCCESS ||
        aclrtGetDeviceInfo(static_cast<uint32_t>(device),
            ACL_DEV_ATTR_UBUF_PER_VECTOR_CORE, &totalUbBytes) != ACL_SUCCESS ||
        !ComputeA5SimtLocalMemorySize(totalUbBytes,
            &cfg.localMemorySize)) {
        return TileXR::TILEXR_ERROR_PARA_CHECK_FAIL;
    }

    cfg.schemMode = RT_SCHEM_MODE_NORMAL;
    return TileXR::TILEXR_SUCCESS;
}

constexpr uint32_t LCCL_RT_DEV_BINARY_MAGIC_ELF_AIVEC = 0x41415246;
constexpr uint64_t FUNSIG_OFFSET_BITS = 16;
constexpr uint64_t FUNSIG_SKEW = 0x1000;

std::mutex g_registrationMutex;
bool g_registered = false;
int g_registrationStatus = TileXR::TILEXR_ERROR_NOT_INITIALIZED;
void *g_binaryHandle = nullptr;
void *g_a5BinaryHandle = nullptr;
void *g_a5Binary128Handle = nullptr;
uint8_t g_a5KernelStub = 0U;
uint8_t g_a5Kernel128Stub = 0U;
bool g_a5Registered = false;

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
    TileXR::KernelBinaryView kernelBinary {};
    if (!TileXR::LoadKernelBinary(
            reinterpret_cast<const void *>(&RegisterCollectivesKernelsLocked),
            "tilexr_collectives_op.o", &kernelBinary)) {
        g_registrationStatus = TileXR::TILEXR_ERROR_NOT_INITIALIZED;
        return g_registrationStatus;
    }

    rtDevBinary_t binary {};
    binary.data = kernelBinary.data;
    binary.length = static_cast<uint32_t>(kernelBinary.size);
    binary.magic = LCCL_RT_DEV_BINARY_MAGIC_ELF_AIVEC;
    binary.version = 0;

    void *binHandle = nullptr;
    rtError_t rtRet = rtDevBinaryRegister(&binary, &binHandle);
    if (rtRet != RT_ERROR_NONE) {
        std::cerr << "TileXR collectives rtDevBinaryRegister failed, ret=" << rtRet
                  << ", binarySize=" << kernelBinary.size << std::endl;
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

int RegisterA5Binary(const char *fileName, const char *name,
    void **handle, uint8_t *stub)
{
    TileXR::KernelBinaryView kernelBinary {};
    if (!TileXR::LoadKernelBinary(
            reinterpret_cast<const void *>(&RegisterA5Binary), fileName,
            &kernelBinary)) {
        return TileXR::TILEXR_ERROR_NOT_INITIALIZED;
    }
    rtDevBinary_t binary {};
    binary.data = kernelBinary.data;
    binary.length = static_cast<uint32_t>(kernelBinary.size);
    binary.magic = LCCL_RT_DEV_BINARY_MAGIC_ELF_AIVEC;
    void *binaryHandle = nullptr;
    rtError_t ret = rtDevBinaryRegister(&binary, &binaryHandle);
    if (ret == RT_ERROR_NONE) {
        ret = rtFunctionRegister(binaryHandle, stub, name, name, 0U);
    }
    if (ret != RT_ERROR_NONE) {
        if (binaryHandle != nullptr) {
            (void)rtDevBinaryUnRegister(binaryHandle);
        }
        return TileXR::TILEXR_ERROR_MKIRT;
    }
    *handle = binaryHandle;
    return TileXR::TILEXR_SUCCESS;
}

int EnsureA5KernelsRegistered()
{
    std::lock_guard<std::mutex> guard(g_registrationMutex);
    if (g_a5Registered) {
        return TileXR::TILEXR_SUCCESS;
    }
    int ret = RegisterA5Binary("tilexr_alltoall_a5_kernel.o",
        "tilexr_alltoall_a5_kernel",
        &g_a5BinaryHandle, &g_a5KernelStub);
    if (ret != TileXR::TILEXR_SUCCESS) {
        return ret;
    }
    ret = RegisterA5Binary("tilexr_alltoall_a5_kernel_128.o",
        "tilexr_alltoall_a5_kernel_128",
        &g_a5Binary128Handle, &g_a5Kernel128Stub);
    if (ret != TileXR::TILEXR_SUCCESS) {
        (void)rtDevBinaryUnRegister(g_a5BinaryHandle);
        g_a5BinaryHandle = nullptr;
        return ret;
    }
    g_a5Registered = true;
    return TileXR::TILEXR_SUCCESS;
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

int LaunchA5AllToAllKernel(uint32_t rankSize,
    const TileXRGrouped::KernelArgs &args, aclrtStream stream)
{
    if (!TileXRGrouped::ValidRankSize(rankSize) || stream == nullptr) {
        return TileXR::TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    const int registerRet = EnsureA5KernelsRegistered();
    if (registerRet != TileXR::TILEXR_SUCCESS) {
        return registerRet;
    }
    rtArgsEx_t argsInfo {};
    argsInfo.args = const_cast<TileXRGrouped::KernelArgs *>(&args);
    argsInfo.argsSize = static_cast<uint32_t>(sizeof(args));
    rtTaskCfgInfo_t cfg {};
    const int configRet = ConfigureSimtMemory(cfg);
    if (configRet != TileXR::TILEXR_SUCCESS) {
        return configRet;
    }
    const void *stub = TileXRGrouped::SelectKernelVariant(rankSize) ==
        TileXRGrouped::KernelVariant::Rank128 ?
        static_cast<const void *>(&g_a5Kernel128Stub) :
        static_cast<const void *>(&g_a5KernelStub);
    const rtError_t ret = rtKernelLaunchWithFlagV2(stub,
        TileXRGrouped::kBlockDim, &argsInfo, nullptr,
        static_cast<rtStream_t>(stream), 0U, &cfg);
    return ret == RT_ERROR_NONE ?
        TileXR::TILEXR_SUCCESS : TileXR::TILEXR_ERROR_MKIRT;
}

} // namespace Host
} // namespace TileXRCollectives
