#include "ep_kernel_launch.h"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <mutex>

#include "acl/acl_rt.h"
#include "ep_memory_layout.h"
#include "ep_window.h"
#include "tilexr_api.h"
#include "tilexr_types.h"
#include "runtime/kernel.h"

extern "C" {
extern const unsigned char TileXREpDispatchMemoryKernelBinaryData[];
extern const std::size_t TileXREpDispatchMemoryKernelBinarySize;
extern const unsigned char TileXREpCombineMemoryKernelBinaryData[];
extern const std::size_t TileXREpCombineMemoryKernelBinarySize;
}

extern void launch_tilexr_ep_dispatch_kernel(uint32_t blockDim, void *stream, GM_ADDR commArgs, GM_ADDR x,
    GM_ADDR expertIds, GM_ADDR scales, GM_ADDR xActiveMask, GM_ADDR expandXOut, GM_ADDR dynamicScalesOut,
    GM_ADDR expertTokenNumsOut,
    GM_ADDR epRecvCountsOut, GM_ADDR tpRecvCountsOut, GM_ADDR assistInfoForCombineOut, GM_ADDR workspace, int64_t bs, int64_t h,
    int64_t topK, int64_t moeExpertNum, int64_t dtypeBytes, int64_t maxRoutesPerSrc, int64_t rowBytes,
    int64_t payloadRowBytes,
    int64_t payloadBytesPerSlot, int64_t assistBytesPerSlot, int64_t slotBytes, int64_t totalBytes,
    int64_t expertTokenNumsType, int64_t sharedExpertNum, int64_t sharedExpertRankNum, int64_t quantMode,
    int64_t tpWorldSize, int64_t tpRankId, int64_t magic);

extern void launch_tilexr_ep_dispatch_cross_node_kernel(uint32_t blockDim, void *stream, GM_ADDR commArgs, GM_ADDR x,
    GM_ADDR expertIds, GM_ADDR scales, GM_ADDR xActiveMask, GM_ADDR expandXOut, GM_ADDR dynamicScalesOut,
    GM_ADDR expertTokenNumsOut,
    GM_ADDR epRecvCountsOut, GM_ADDR tpRecvCountsOut, GM_ADDR assistInfoForCombineOut, GM_ADDR workspace, int64_t bs, int64_t h,
    int64_t topK, int64_t moeExpertNum, int64_t dtypeBytes, int64_t maxRoutesPerSrc, int64_t rowBytes,
    int64_t payloadRowBytes,
    int64_t payloadBytesPerSlot, int64_t assistBytesPerSlot, int64_t slotBytes, int64_t totalBytes,
    int64_t expertTokenNumsType, int64_t sharedExpertNum, int64_t sharedExpertRankNum, int64_t quantMode,
    int64_t tpWorldSize, int64_t tpRankId, int64_t magic);

extern void launch_tilexr_ep_combine_kernel(uint32_t blockDim, void *stream, GM_ADDR commArgs, GM_ADDR expertOut,
    GM_ADDR assistInfoForCombine, GM_ADDR epRecvCounts, GM_ADDR yOut, int64_t bs, int64_t h, int64_t topK,
    int64_t moeExpertNum, int64_t dtype, int64_t dtypeBytes, int64_t maxRoutesPerSrc, int64_t rowBytes,
    int64_t payloadBytesPerSlot, int64_t assistBytesPerSlot, int64_t slotBytes, int64_t totalBytes, int64_t magic);

extern void launch_tilexr_ep_combine_cross_node_kernel(uint32_t blockDim, void *stream, GM_ADDR commArgs,
    GM_ADDR expertOut, GM_ADDR assistInfoForCombine, GM_ADDR epRecvCounts, GM_ADDR yOut, GM_ADDR workspace,
    int64_t bs, int64_t h, int64_t topK, int64_t moeExpertNum, int64_t dtype, int64_t dtypeBytes,
    int64_t maxRoutesPerSrc, int64_t rowBytes, int64_t payloadBytesPerSlot, int64_t assistBytesPerSlot,
    int64_t slotBytes, int64_t totalBytes, int64_t magic);

extern void launch_tilexr_ep_combine_cross_node_drain_kernel(uint32_t blockDim, void *stream, GM_ADDR commArgs,
    GM_ADDR yOut, GM_ADDR workspace, int64_t bs, int64_t h, int64_t topK, int64_t moeExpertNum, int64_t dtype,
    int64_t dtypeBytes, int64_t maxRoutesPerSrc, int64_t rowBytes, int64_t payloadBytesPerSlot,
    int64_t assistBytesPerSlot, int64_t slotBytes, int64_t totalBytes);

namespace TileXREp {

namespace {

constexpr uint32_t kAivBinaryMagic = 0x41415246U;
constexpr char kDispatchMemoryKernelName[] = "tilexr_ep_dispatch_memory_kernel";
constexpr char kCombineMemoryKernelName[] = "tilexr_ep_combine_memory_kernel";

std::mutex gDispatchMemoryRegistrationMutex;
bool gDispatchMemoryRegistered = false;
int gDispatchMemoryRegistrationStatus = TileXR::TILEXR_ERROR_NOT_INITIALIZED;
void *gDispatchMemoryBinaryHandle = nullptr;
uint8_t gDispatchMemoryKernelStub = 0;

std::mutex gCombineMemoryRegistrationMutex;
bool gCombineMemoryRegistered = false;
int gCombineMemoryRegistrationStatus = TileXR::TILEXR_ERROR_NOT_INITIALIZED;
void *gCombineMemoryBinaryHandle = nullptr;
uint8_t gCombineMemoryKernelStub = 0;

struct DispatchMemoryKernelArgs {
    GM_ADDR commArgs;
    GM_ADDR x;
    GM_ADDR expertIds;
    GM_ADDR xActiveMask;
    GM_ADDR expandXOut;
    GM_ADDR dynamicScalesOut;
    GM_ADDR expertTokenNumsOut;
    GM_ADDR sendCountsOut;
    GM_ADDR assistInfoForCombineOut;
    int64_t bs;
    int64_t h;
    int64_t topK;
    int64_t moeExpertNum;
    int64_t sharedExpertNum;
    int64_t sharedExpertRankNum;
    int64_t globalBs;
    int64_t expertTokenNumsType;
    int64_t activeMaskType;
    int64_t quantMode;
    int64_t dtype;
    int64_t expandXOutDtype;
    int64_t magic;
};

static_assert(sizeof(DispatchMemoryKernelArgs) == 22U * sizeof(uint64_t),
    "dispatch memory kernel argument ABI changed");

struct CombineMemoryKernelArgs {
    GM_ADDR commArgs;
    GM_ADDR expertOut;
    GM_ADDR assistInfoForCombine;
    GM_ADDR sendCounts;
    GM_ADDR expertScales;
    GM_ADDR xActiveMask;
    GM_ADDR sharedExpertX;
    GM_ADDR yOut;
    int64_t bs;
    int64_t h;
    int64_t topK;
    int64_t moeExpertNum;
    int64_t sharedExpertNum;
    int64_t sharedExpertRankNum;
    int64_t globalBs;
    int64_t activeMaskType;
    int64_t quantMode;
    int64_t dtype;
    int64_t magic;
};

static_assert(sizeof(CombineMemoryKernelArgs) == 19U * sizeof(uint64_t),
    "combine memory kernel argument ABI changed");

int EnsureDispatchMemoryKernelRegistered()
{
    std::lock_guard<std::mutex> guard(gDispatchMemoryRegistrationMutex);
    if (gDispatchMemoryRegistered) {
        return gDispatchMemoryRegistrationStatus;
    }
    if (TileXREpDispatchMemoryKernelBinarySize == 0) {
        return TileXR::TILEXR_ERROR_NOT_INITIALIZED;
    }

    rtDevBinary_t binary {};
    binary.data = TileXREpDispatchMemoryKernelBinaryData;
    binary.length = static_cast<uint32_t>(TileXREpDispatchMemoryKernelBinarySize);
    binary.magic = kAivBinaryMagic;
    binary.version = 0;

    rtError_t rtRet = rtDevBinaryRegister(&binary, &gDispatchMemoryBinaryHandle);
    if (rtRet == RT_ERROR_NONE) {
        rtRet = rtFunctionRegister(gDispatchMemoryBinaryHandle, &gDispatchMemoryKernelStub,
            kDispatchMemoryKernelName, kDispatchMemoryKernelName, 0);
    }
    if (rtRet != RT_ERROR_NONE) {
        std::cerr << "TileXR EP dispatch memory kernel registration failed, ret=" << rtRet << std::endl;
        gDispatchMemoryRegistrationStatus = TileXR::TILEXR_ERROR_MKIRT;
        return gDispatchMemoryRegistrationStatus;
    }

    gDispatchMemoryRegistered = true;
    gDispatchMemoryRegistrationStatus = TileXR::TILEXR_SUCCESS;
    return gDispatchMemoryRegistrationStatus;
}

int LaunchDispatchMemoryKernel(uint32_t blockDim, aclrtStream stream, DispatchMemoryKernelArgs *args)
{
    const int registerRet = EnsureDispatchMemoryKernelRegistered();
    if (registerRet != TileXR::TILEXR_SUCCESS) {
        return registerRet;
    }

    rtArgsEx_t argsInfo {};
    argsInfo.args = args;
    argsInfo.argsSize = sizeof(*args);
    rtTaskCfgInfo_t cfgInfo {};
    const rtError_t rtRet = rtKernelLaunchWithFlagV2(&gDispatchMemoryKernelStub, blockDim, &argsInfo, nullptr,
        static_cast<rtStream_t>(stream), 0, &cfgInfo);
    if (rtRet != RT_ERROR_NONE) {
        std::cerr << "TileXR EP dispatch memory kernel launch failed, ret=" << rtRet << std::endl;
        return TileXR::TILEXR_ERROR_MKIRT;
    }
    return TileXR::TILEXR_SUCCESS;
}

int EnsureCombineMemoryKernelRegistered()
{
    std::lock_guard<std::mutex> guard(gCombineMemoryRegistrationMutex);
    if (gCombineMemoryRegistered) {
        return gCombineMemoryRegistrationStatus;
    }
    if (TileXREpCombineMemoryKernelBinarySize == 0) {
        return TileXR::TILEXR_ERROR_NOT_INITIALIZED;
    }

    rtDevBinary_t binary {};
    binary.data = TileXREpCombineMemoryKernelBinaryData;
    binary.length = static_cast<uint32_t>(TileXREpCombineMemoryKernelBinarySize);
    binary.magic = kAivBinaryMagic;
    binary.version = 0;

    rtError_t rtRet = rtDevBinaryRegister(&binary, &gCombineMemoryBinaryHandle);
    if (rtRet == RT_ERROR_NONE) {
        rtRet = rtFunctionRegister(gCombineMemoryBinaryHandle, &gCombineMemoryKernelStub,
            kCombineMemoryKernelName, kCombineMemoryKernelName, 0);
    }
    if (rtRet != RT_ERROR_NONE) {
        std::cerr << "TileXR EP combine memory kernel registration failed, ret=" << rtRet << std::endl;
        gCombineMemoryRegistrationStatus = TileXR::TILEXR_ERROR_MKIRT;
        return gCombineMemoryRegistrationStatus;
    }

    gCombineMemoryRegistered = true;
    gCombineMemoryRegistrationStatus = TileXR::TILEXR_SUCCESS;
    return gCombineMemoryRegistrationStatus;
}

int LaunchCombineMemoryKernel(uint32_t blockDim, aclrtStream stream, CombineMemoryKernelArgs *args)
{
    const int registerRet = EnsureCombineMemoryKernelRegistered();
    if (registerRet != TileXR::TILEXR_SUCCESS) {
        return registerRet;
    }

    rtArgsEx_t argsInfo {};
    argsInfo.args = args;
    argsInfo.argsSize = sizeof(*args);
    rtTaskCfgInfo_t cfgInfo {};
    const rtError_t rtRet = rtKernelLaunchWithFlagV2(&gCombineMemoryKernelStub, blockDim, &argsInfo, nullptr,
        static_cast<rtStream_t>(stream), 0, &cfgInfo);
    if (rtRet != RT_ERROR_NONE) {
        std::cerr << "TileXR EP combine memory kernel launch failed, ret=" << rtRet << std::endl;
        return TileXR::TILEXR_ERROR_MKIRT;
    }
    return TileXR::TILEXR_SUCCESS;
}

bool TileXREpUsesCrossNodeKernel(const EpHostLaunchContext &context)
{
    return context.hostArgs != nullptr && context.hostArgs->localRankSize > 0 &&
        context.hostArgs->localRankSize < context.hostArgs->rankSize;
}

int64_t TileXREpUdmaStatusOffset(int64_t totalBytes, int64_t rankSize, int64_t slotBytes)
{
    const int64_t operationBytes = TileXREpUdmaOperationBytes(totalBytes, rankSize, slotBytes);
    if (operationBytes == TileXR::TILEXR_INVALID_VALUE || operationBytes > INT64_MAX / 2) {
        return TileXR::TILEXR_INVALID_VALUE;
    }
    return operationBytes * 2;
}

} // namespace

int TileXREpLaunchDispatchKernel(const EpDispatchParams &params, const EpHostLaunchContext &context)
{
    int64_t magic = 0;
    const int magicRet = TileXRCommNextMagic(params.comm, &magic);
    if (magicRet != TileXR::TILEXR_SUCCESS) {
        return magicRet;
    }

    constexpr uint32_t kMvpBlockDim = 1;
    if (TileXREpUsesCrossNodeKernel(context)) {
        launch_tilexr_ep_dispatch_cross_node_kernel(kMvpBlockDim, params.stream, context.devArgs,
            static_cast<GM_ADDR>(params.x), reinterpret_cast<GM_ADDR>(params.expertIds),
            static_cast<GM_ADDR>(params.scales), reinterpret_cast<GM_ADDR>(params.xActiveMask),
            static_cast<GM_ADDR>(params.expandXOut), static_cast<GM_ADDR>(params.dynamicScalesOut),
            reinterpret_cast<GM_ADDR>(params.expertTokenNumsOut), reinterpret_cast<GM_ADDR>(params.epRecvCountsOut),
            reinterpret_cast<GM_ADDR>(params.tpRecvCountsOut),
            reinterpret_cast<GM_ADDR>(params.assistInfoForCombineOut), static_cast<GM_ADDR>(params.workspace),
            params.bs, params.h, params.topK, params.moeExpertNum, context.window.dtypeBytes,
            context.window.maxRoutesPerSrc, context.window.rowBytes, context.window.payloadRowBytes,
            context.window.payloadBytesPerSlot,
            context.window.assistBytesPerSlot, context.window.slotBytes, context.window.totalBytes,
            params.expertTokenNumsType, params.sharedExpertNum, params.sharedExpertRankNum, params.quantMode,
            params.tpWorldSize, params.tpRankId, magic);
    } else {
        launch_tilexr_ep_dispatch_kernel(kMvpBlockDim, params.stream, context.devArgs, static_cast<GM_ADDR>(params.x),
            reinterpret_cast<GM_ADDR>(params.expertIds), static_cast<GM_ADDR>(params.scales),
            reinterpret_cast<GM_ADDR>(params.xActiveMask), static_cast<GM_ADDR>(params.expandXOut),
            static_cast<GM_ADDR>(params.dynamicScalesOut), reinterpret_cast<GM_ADDR>(params.expertTokenNumsOut),
            reinterpret_cast<GM_ADDR>(params.epRecvCountsOut),
            reinterpret_cast<GM_ADDR>(params.tpRecvCountsOut), reinterpret_cast<GM_ADDR>(params.assistInfoForCombineOut),
            static_cast<GM_ADDR>(params.workspace), params.bs, params.h, params.topK, params.moeExpertNum, context.window.dtypeBytes,
            context.window.maxRoutesPerSrc, context.window.rowBytes, context.window.payloadRowBytes,
            context.window.payloadBytesPerSlot, context.window.assistBytesPerSlot, context.window.slotBytes,
            context.window.totalBytes, params.expertTokenNumsType, params.sharedExpertNum, params.sharedExpertRankNum,
            params.quantMode, params.tpWorldSize, params.tpRankId, magic);
    }
    return TileXR::TILEXR_SUCCESS;
}

int TileXREpLaunchDispatchMemoryKernel(const EpDispatchParams &params, const EpHostLaunchContext &context)
{
    int32_t deviceId = 0;
    if (aclrtGetDevice(&deviceId) != ACL_SUCCESS) {
        return TileXR::TILEXR_ERROR_INTERNAL;
    }
    int64_t vectorCoreNum = 0;
    if (aclrtGetDeviceInfo(static_cast<uint32_t>(deviceId), ACL_DEV_ATTR_VECTOR_CORE_NUM, &vectorCoreNum) !=
            ACL_SUCCESS ||
        vectorCoreNum < 2 || vectorCoreNum > static_cast<int64_t>(UINT32_MAX)) {
        return TileXR::TILEXR_ERROR_INTERNAL;
    }

    const char *blockDimOverride = std::getenv("TILEXR_EP_DISPATCH_MEMORY_BLOCK_DIM");
    uint32_t blockDim = 0U;
    const int blockDimRet = TileXREpResolveMemoryBlockDim(
        static_cast<uint32_t>(vectorCoreNum), 2U, blockDimOverride, &blockDim);
    if (blockDimRet != TileXR::TILEXR_SUCCESS) {
        std::cerr << "Invalid TILEXR_EP_DISPATCH_MEMORY_BLOCK_DIM="
                  << (blockDimOverride == nullptr ? "<unset>" : blockDimOverride)
                  << ", device vector cores=" << vectorCoreNum << std::endl;
        return blockDimRet;
    }

    EpMemoryDispatchReferenceConfig memoryConfig {};
    const int configRet = TileXREpValidateDispatchMemoryConfig(
        params, *context.hostArgs, blockDim, &memoryConfig);
    if (configRet != TileXR::TILEXR_SUCCESS) {
        return configRet;
    }

    const int64_t globalBs = params.globalBs == 0 ? params.bs * context.hostArgs->rankSize : params.globalBs;
    int64_t magic = 0;
    const int magicRet = TileXRCommNextMagic(params.comm, &magic);
    if (magicRet != TileXR::TILEXR_SUCCESS) {
        return magicRet;
    }

    DispatchMemoryKernelArgs args { context.devArgs, static_cast<GM_ADDR>(params.x),
        reinterpret_cast<GM_ADDR>(params.expertIds), reinterpret_cast<GM_ADDR>(params.xActiveMask),
        static_cast<GM_ADDR>(params.expandXOut), static_cast<GM_ADDR>(params.dynamicScalesOut),
        reinterpret_cast<GM_ADDR>(params.expertTokenNumsOut), reinterpret_cast<GM_ADDR>(params.epRecvCountsOut),
        reinterpret_cast<GM_ADDR>(params.assistInfoForCombineOut), params.bs, params.h, params.topK,
        params.moeExpertNum, params.sharedExpertNum, params.sharedExpertRankNum, globalBs,
        params.expertTokenNumsType, params.activeMaskType, params.quantMode, static_cast<int64_t>(params.dtype),
        static_cast<int64_t>(params.expandXOutDtype == TileXR::TILEXR_DATA_TYPE_RESERVED ?
            params.dtype : params.expandXOutDtype), magic };
    return LaunchDispatchMemoryKernel(blockDim, params.stream, &args);
}

int TileXREpLaunchCombineMemoryKernel(const EpCombineParams &params, const EpHostLaunchContext &context)
{
    int32_t deviceId = 0;
    if (aclrtGetDevice(&deviceId) != ACL_SUCCESS) {
        return TileXR::TILEXR_ERROR_INTERNAL;
    }
    int64_t vectorCoreNum = 0;
    if (aclrtGetDeviceInfo(static_cast<uint32_t>(deviceId), ACL_DEV_ATTR_VECTOR_CORE_NUM, &vectorCoreNum) !=
            ACL_SUCCESS ||
        vectorCoreNum <= 0 || vectorCoreNum > static_cast<int64_t>(UINT32_MAX)) {
        return TileXR::TILEXR_ERROR_INTERNAL;
    }
    const char *blockDimOverride = std::getenv("TILEXR_EP_COMBINE_MEMORY_BLOCK_DIM");
    uint32_t blockDim = 0U;
    const int blockDimRet = TileXREpResolveMemoryBlockDim(
        static_cast<uint32_t>(vectorCoreNum), 1U, blockDimOverride, &blockDim);
    if (blockDimRet != TileXR::TILEXR_SUCCESS) {
        std::cerr << "Invalid TILEXR_EP_COMBINE_MEMORY_BLOCK_DIM="
                  << (blockDimOverride == nullptr ? "<unset>" : blockDimOverride)
                  << ", device vector cores=" << vectorCoreNum << std::endl;
        return blockDimRet;
    }

    EpMemoryCombineReferenceConfig memoryConfig {};
    const int configRet = TileXREpValidateCombineMemoryConfig(
        params, *context.hostArgs, blockDim, &memoryConfig);
    if (configRet != TileXR::TILEXR_SUCCESS) {
        return configRet;
    }

    const int64_t globalBs = params.globalBs == 0 ? params.bs * context.hostArgs->rankSize : params.globalBs;
    int64_t magic = 0;
    const int magicRet = TileXRCommNextMagic(params.comm, &magic);
    if (magicRet != TileXR::TILEXR_SUCCESS) {
        return magicRet;
    }
    CombineMemoryKernelArgs args { context.devArgs,
        static_cast<GM_ADDR>(params.expertOut), reinterpret_cast<GM_ADDR>(params.assistInfoForCombine),
        reinterpret_cast<GM_ADDR>(params.epRecvCounts), reinterpret_cast<GM_ADDR>(params.expertScales),
        reinterpret_cast<GM_ADDR>(params.xActiveMask), static_cast<GM_ADDR>(params.sharedExpertX),
        static_cast<GM_ADDR>(params.yOut), params.bs, params.h, params.topK, params.moeExpertNum,
        params.sharedExpertNum, params.sharedExpertRankNum, globalBs, params.activeMaskType,
        params.quantMode, static_cast<int64_t>(params.dtype), magic };
    return LaunchCombineMemoryKernel(blockDim, params.stream, &args);
}

int TileXREpLaunchCombineKernel(const EpCombineParams &params, const EpHostLaunchContext &context)
{
    int64_t magic = 0;
    const int magicRet = TileXRCommNextMagic(params.comm, &magic);
    if (magicRet != TileXR::TILEXR_SUCCESS) {
        return magicRet;
    }

    constexpr uint32_t kMvpBlockDim = 1;
    if (TileXREpUsesCrossNodeKernel(context)) {
        launch_tilexr_ep_combine_cross_node_kernel(kMvpBlockDim, params.stream, context.devArgs,
            static_cast<GM_ADDR>(params.expertOut), reinterpret_cast<GM_ADDR>(params.assistInfoForCombine),
            reinterpret_cast<GM_ADDR>(params.epRecvCounts), static_cast<GM_ADDR>(params.yOut),
            static_cast<GM_ADDR>(params.workspace), params.bs, params.h, params.topK, params.moeExpertNum,
            static_cast<int64_t>(params.dtype), context.window.dtypeBytes, context.window.maxRoutesPerSrc,
            context.window.rowBytes, context.window.payloadBytesPerSlot, context.window.assistBytesPerSlot,
            context.window.slotBytes, context.window.totalBytes, magic);
        aclError aclRet = aclrtSynchronizeStream(params.stream);
        if (aclRet != ACL_SUCCESS) {
            return TileXR::TILEXR_ERROR_INTERNAL;
        }
        uint64_t status = TileXREp::kEpStatusOk;
        const int64_t statusOffset = TileXREpUdmaStatusOffset(
            context.window.totalBytes, context.window.rankSize, context.window.slotBytes);
        if (statusOffset == TileXR::TILEXR_INVALID_VALUE) {
            return TileXR::TILEXR_ERROR_PARA_CHECK_FAIL;
        }
        aclRet = aclrtMemcpy(&status, sizeof(status), static_cast<GM_ADDR>(params.workspace) + statusOffset,
            sizeof(status), ACL_MEMCPY_DEVICE_TO_HOST);
        if (aclRet != ACL_SUCCESS) {
            return TileXR::TILEXR_ERROR_INTERNAL;
        }
        if (status != TileXREp::kEpStatusOk) {
            return TileXR::TILEXR_ERROR_TIMEOUT;
        }
        launch_tilexr_ep_combine_cross_node_drain_kernel(kMvpBlockDim, params.stream, context.devArgs,
            static_cast<GM_ADDR>(params.yOut), static_cast<GM_ADDR>(params.workspace), params.bs, params.h,
            params.topK, params.moeExpertNum, static_cast<int64_t>(params.dtype), context.window.dtypeBytes,
            context.window.maxRoutesPerSrc, context.window.rowBytes, context.window.payloadBytesPerSlot,
            context.window.assistBytesPerSlot, context.window.slotBytes, context.window.totalBytes);
    } else {
        launch_tilexr_ep_combine_kernel(kMvpBlockDim, params.stream, context.devArgs,
            static_cast<GM_ADDR>(params.expertOut), reinterpret_cast<GM_ADDR>(params.assistInfoForCombine),
            reinterpret_cast<GM_ADDR>(params.epRecvCounts), static_cast<GM_ADDR>(params.yOut),
            params.bs, params.h, params.topK, params.moeExpertNum, static_cast<int64_t>(params.dtype),
            context.window.dtypeBytes, context.window.maxRoutesPerSrc, context.window.rowBytes,
            context.window.payloadBytesPerSlot, context.window.assistBytesPerSlot, context.window.slotBytes,
            context.window.totalBytes, magic);
    }
    return TileXR::TILEXR_SUCCESS;
}

} // namespace TileXREp
