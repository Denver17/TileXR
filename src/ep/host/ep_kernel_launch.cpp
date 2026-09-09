#include "ep_kernel_launch.h"
#include "ep_memory_server_dedup_launch.h"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <mutex>

#include "acl/acl_rt.h"
#include "ep_memory_layout.h"
#include "ep_window.h"
#include "runtime/kernel.h"
#include "tilexr_api.h"
#include "tilexr_kernel_binary_loader.h"
#include "tilexr_types.h"

namespace TileXREp {
namespace {

constexpr uint32_t kAivBinaryMagic = 0x41415246U;
constexpr uint32_t kMvpBlockDim = 1U;

constexpr char kDispatchKernelFile[] = "tilexr_ep_dispatch_kernel.o";
constexpr char kDispatchKernelName[] = "tilexr_ep_dispatch_kernel";
constexpr char kDispatchCrossNodeKernelName[] = "tilexr_ep_dispatch_cross_node_kernel";
constexpr char kCombineKernelFile[] = "tilexr_ep_combine_kernel.o";
constexpr char kCombineKernelName[] = "tilexr_ep_combine_kernel";
constexpr char kCombineCrossNodeKernelName[] = "tilexr_ep_combine_cross_node_kernel";
constexpr char kCombineCrossNodeDrainKernelName[] = "tilexr_ep_combine_cross_node_drain_kernel";
constexpr char kDispatchMemoryKernelFile[] = "tilexr_ep_dispatch_memory_kernel.o";
constexpr char kDispatchMemoryKernelName[] = "tilexr_ep_dispatch_memory_kernel";
constexpr char kCombineMemoryKernelFile[] = "tilexr_ep_combine_memory_kernel.o";
constexpr char kCombineMemoryKernelName[] = "tilexr_ep_combine_memory_kernel";
constexpr char kServerDedupKernelFile[] = "tilexr_ep_dispatch_memory_server_dedup_kernel.o";
constexpr char kServerDedupKernelName[] = "tilexr_ep_dispatch_memory_server_dedup_kernel";

struct KernelRegistrationState {
    std::mutex mutex;
    bool registered = false;
    void *binaryHandle = nullptr;
};

struct KernelEntry {
    const char *name;
    uint8_t *stub;
};

KernelRegistrationState gDispatchRegistration;
uint8_t gDispatchKernelStub = 0;
uint8_t gDispatchCrossNodeKernelStub = 0;

KernelRegistrationState gCombineRegistration;
uint8_t gCombineKernelStub = 0;
uint8_t gCombineCrossNodeKernelStub = 0;
uint8_t gCombineCrossNodeDrainKernelStub = 0;

KernelRegistrationState gDispatchMemoryRegistration;
uint8_t gDispatchMemoryKernelStub = 0;

KernelRegistrationState gCombineMemoryRegistration;
uint8_t gCombineMemoryKernelStub = 0;
KernelRegistrationState gServerDedupRegistration;
uint8_t gServerDedupKernelStub = 0;

bool HasPrefix(const char *value, const char *prefix)
{
    if (value == nullptr || prefix == nullptr) {
        return false;
    }
    return std::strncmp(value, prefix, std::strlen(prefix)) == 0;
}

int EnsureKernelRegistered(KernelRegistrationState &state, const char *binaryFileName,
    const KernelEntry *entries, std::size_t entryCount, const void *anchor)
{
    std::lock_guard<std::mutex> guard(state.mutex);
    if (state.registered) {
        return TileXR::TILEXR_SUCCESS;
    }
    if (binaryFileName == nullptr || entries == nullptr || entryCount == 0 || anchor == nullptr) {
        return TileXR::TILEXR_ERROR_NOT_INITIALIZED;
    }

    TileXR::KernelBinaryView kernelBinary {};
    if (!TileXR::LoadKernelBinary(anchor, binaryFileName, &kernelBinary)) {
        return TileXR::TILEXR_ERROR_NOT_INITIALIZED;
    }

    rtDevBinary_t binary {};
    binary.magic = kAivBinaryMagic;
    binary.version = 0;
    binary.data = kernelBinary.data;
    binary.length = static_cast<uint64_t>(kernelBinary.size);

    void *binaryHandle = nullptr;
    rtError_t rtRet = rtDevBinaryRegister(&binary, &binaryHandle);
    if (rtRet == RT_ERROR_NONE) {
        for (std::size_t index = 0; index < entryCount; ++index) {
            rtRet = rtFunctionRegister(binaryHandle, entries[index].stub,
                entries[index].name, entries[index].name, 0U);
            if (rtRet != RT_ERROR_NONE) {
                break;
            }
        }
    }
    if (rtRet != RT_ERROR_NONE) {
        std::cerr << "TileXR EP kernel registration failed, ret=" << rtRet
                  << ", binary=" << binaryFileName << std::endl;
        if (binaryHandle != nullptr) {
            (void)rtDevBinaryUnRegister(binaryHandle);
        }
        return TileXR::TILEXR_ERROR_MKIRT;
    }

    state.binaryHandle = binaryHandle;
    state.registered = true;
    return TileXR::TILEXR_SUCCESS;
}

int EnsureDispatchKernelsRegistered()
{
    const KernelEntry entries[] = {
        {kDispatchKernelName, &gDispatchKernelStub},
        {kDispatchCrossNodeKernelName, &gDispatchCrossNodeKernelStub},
    };
    return EnsureKernelRegistered(gDispatchRegistration, kDispatchKernelFile,
        entries, sizeof(entries) / sizeof(entries[0]),
        reinterpret_cast<const void *>(&EnsureDispatchKernelsRegistered));
}

int EnsureCombineKernelsRegistered()
{
    const KernelEntry entries[] = {
        {kCombineKernelName, &gCombineKernelStub},
        {kCombineCrossNodeKernelName, &gCombineCrossNodeKernelStub},
        {kCombineCrossNodeDrainKernelName, &gCombineCrossNodeDrainKernelStub},
    };
    return EnsureKernelRegistered(gCombineRegistration, kCombineKernelFile,
        entries, sizeof(entries) / sizeof(entries[0]),
        reinterpret_cast<const void *>(&EnsureCombineKernelsRegistered));
}

int EnsureDispatchMemoryKernelRegistered()
{
    const KernelEntry entry {kDispatchMemoryKernelName, &gDispatchMemoryKernelStub};
    return EnsureKernelRegistered(gDispatchMemoryRegistration, kDispatchMemoryKernelFile,
        &entry, 1U, reinterpret_cast<const void *>(&EnsureDispatchMemoryKernelRegistered));
}

int EnsureCombineMemoryKernelRegistered()
{
    const KernelEntry entry {kCombineMemoryKernelName, &gCombineMemoryKernelStub};
    return EnsureKernelRegistered(gCombineMemoryRegistration, kCombineMemoryKernelFile,
        &entry, 1U, reinterpret_cast<const void *>(&EnsureCombineMemoryKernelRegistered));
}

int LaunchRegisteredKernel(const void *stub, uint32_t blockDim, aclrtStream stream,
    void *args, std::size_t argsSize, const char *kernelName)
{
    rtArgsEx_t argsInfo {};
    argsInfo.args = args;
    argsInfo.argsSize = argsSize;
    rtTaskCfgInfo_t cfgInfo {};
    const rtError_t rtRet = rtKernelLaunchWithFlagV2(stub, blockDim, &argsInfo, nullptr,
        static_cast<rtStream_t>(stream), 0U, &cfgInfo);
    if (rtRet != RT_ERROR_NONE) {
        std::cerr << "TileXR EP kernel launch failed, ret=" << rtRet
                  << ", kernel=" << kernelName << std::endl;
        return TileXR::TILEXR_ERROR_MKIRT;
    }
    return TileXR::TILEXR_SUCCESS;
}

struct DispatchKernelArgs {
    GM_ADDR commArgs;
    GM_ADDR x;
    GM_ADDR expertIds;
    GM_ADDR scales;
    GM_ADDR xActiveMask;
    GM_ADDR expandXOut;
    GM_ADDR dynamicScalesOut;
    GM_ADDR expertTokenNumsOut;
    GM_ADDR epRecvCountsOut;
    GM_ADDR tpRecvCountsOut;
    GM_ADDR assistInfoForCombineOut;
    GM_ADDR workspace;
    int64_t bs;
    int64_t h;
    int64_t topK;
    int64_t moeExpertNum;
    int64_t dtypeBytes;
    int64_t maxRoutesPerSrc;
    int64_t rowBytes;
    int64_t payloadRowBytes;
    int64_t payloadBytesPerSlot;
    int64_t assistBytesPerSlot;
    int64_t slotBytes;
    int64_t totalBytes;
    int64_t expertTokenNumsType;
    int64_t sharedExpertNum;
    int64_t sharedExpertRankNum;
    int64_t quantMode;
    int64_t tpWorldSize;
    int64_t tpRankId;
    int64_t magic;
};

static_assert(sizeof(DispatchKernelArgs) == 31U * sizeof(uint64_t),
    "dispatch kernel argument ABI changed");

struct CombineKernelArgs {
    GM_ADDR commArgs;
    GM_ADDR expertOut;
    GM_ADDR assistInfoForCombine;
    GM_ADDR epRecvCounts;
    GM_ADDR yOut;
    int64_t bs;
    int64_t h;
    int64_t topK;
    int64_t moeExpertNum;
    int64_t dtype;
    int64_t dtypeBytes;
    int64_t maxRoutesPerSrc;
    int64_t rowBytes;
    int64_t payloadBytesPerSlot;
    int64_t assistBytesPerSlot;
    int64_t slotBytes;
    int64_t totalBytes;
    int64_t magic;
};

static_assert(sizeof(CombineKernelArgs) == 18U * sizeof(uint64_t),
    "combine kernel argument ABI changed");

struct CombineCrossNodeKernelArgs {
    GM_ADDR commArgs;
    GM_ADDR expertOut;
    GM_ADDR assistInfoForCombine;
    GM_ADDR epRecvCounts;
    GM_ADDR yOut;
    GM_ADDR workspace;
    int64_t bs;
    int64_t h;
    int64_t topK;
    int64_t moeExpertNum;
    int64_t dtype;
    int64_t dtypeBytes;
    int64_t maxRoutesPerSrc;
    int64_t rowBytes;
    int64_t payloadBytesPerSlot;
    int64_t assistBytesPerSlot;
    int64_t slotBytes;
    int64_t totalBytes;
    int64_t magic;
};

static_assert(sizeof(CombineCrossNodeKernelArgs) == 19U * sizeof(uint64_t),
    "cross-node combine kernel argument ABI changed");

struct CombineCrossNodeDrainKernelArgs {
    GM_ADDR commArgs;
    GM_ADDR yOut;
    GM_ADDR workspace;
    int64_t bs;
    int64_t h;
    int64_t topK;
    int64_t moeExpertNum;
    int64_t dtype;
    int64_t dtypeBytes;
    int64_t maxRoutesPerSrc;
    int64_t rowBytes;
    int64_t payloadBytesPerSlot;
    int64_t assistBytesPerSlot;
    int64_t slotBytes;
    int64_t totalBytes;
};

static_assert(sizeof(CombineCrossNodeDrainKernelArgs) == 15U * sizeof(uint64_t),
    "cross-node combine drain kernel argument ABI changed");

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

bool TileXREpIsA5Soc()
{
    const char *socName = aclrtGetSocName();
    return HasPrefix(socName, "Ascend950") || HasPrefix(socName, "Ascend910_9") ||
        HasPrefix(socName, "A5");
}

int TileXREpLaunchDispatchKernel(const EpDispatchParams &params, const EpHostLaunchContext &context)
{
    if (!TileXREpIsA5Soc()) {
        return TileXR::TILEXR_ERROR_NOT_SUPPORT;
    }

    int64_t magic = 0;
    const int magicRet = TileXRCommNextMagic(params.comm, &magic);
    if (magicRet != TileXR::TILEXR_SUCCESS) {
        return magicRet;
    }

    const int registerRet = EnsureDispatchKernelsRegistered();
    if (registerRet != TileXR::TILEXR_SUCCESS) {
        return registerRet;
    }

    DispatchKernelArgs args { context.devArgs, static_cast<GM_ADDR>(params.x),
        reinterpret_cast<GM_ADDR>(params.expertIds), static_cast<GM_ADDR>(params.scales),
        reinterpret_cast<GM_ADDR>(params.xActiveMask), static_cast<GM_ADDR>(params.expandXOut),
        static_cast<GM_ADDR>(params.dynamicScalesOut), reinterpret_cast<GM_ADDR>(params.expertTokenNumsOut),
        reinterpret_cast<GM_ADDR>(params.epRecvCountsOut), reinterpret_cast<GM_ADDR>(params.tpRecvCountsOut),
        reinterpret_cast<GM_ADDR>(params.assistInfoForCombineOut), static_cast<GM_ADDR>(params.workspace),
        params.bs, params.h, params.topK, params.moeExpertNum, context.window.dtypeBytes,
        context.window.maxRoutesPerSrc, context.window.rowBytes, context.window.payloadRowBytes,
        context.window.payloadBytesPerSlot, context.window.assistBytesPerSlot, context.window.slotBytes,
        context.window.totalBytes, params.expertTokenNumsType, params.sharedExpertNum,
        params.sharedExpertRankNum, params.quantMode, params.tpWorldSize, params.tpRankId, magic };

    const bool crossNode = TileXREpUsesCrossNodeKernel(context);
    return LaunchRegisteredKernel(crossNode ? &gDispatchCrossNodeKernelStub : &gDispatchKernelStub,
        kMvpBlockDim, params.stream, &args, sizeof(args),
        crossNode ? kDispatchCrossNodeKernelName : kDispatchKernelName);
}

int TileXREpRegisterDispatchMemoryServerDedupKernel()
{
    if (!TileXREpIsA5Soc()) {
        return TileXR::TILEXR_ERROR_NOT_SUPPORT;
    }
    const KernelEntry entry {kServerDedupKernelName, &gServerDedupKernelStub};
    return EnsureKernelRegistered(gServerDedupRegistration, kServerDedupKernelFile,
        &entry, 1U, &gServerDedupKernelStub);
}

int TileXREpSubmitDispatchMemoryServerDedupKernel(EpServerDedupKernelArgs &args,
    uint32_t blockDim, aclrtStream stream)
{
    return LaunchRegisteredKernel(&gServerDedupKernelStub, blockDim, stream,
        &args, sizeof(args), kServerDedupKernelName);
}

int TileXREpLaunchDispatchMemoryKernel(const EpDispatchParams &params, const EpHostLaunchContext &context)
{
    if (!TileXREpIsA5Soc()) {
        return TileXR::TILEXR_ERROR_NOT_SUPPORT;
    }

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
    const int registerRet = EnsureDispatchMemoryKernelRegistered();
    if (registerRet != TileXR::TILEXR_SUCCESS) {
        return registerRet;
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
    return LaunchRegisteredKernel(&gDispatchMemoryKernelStub, blockDim, params.stream,
        &args, sizeof(args), kDispatchMemoryKernelName);
}

int TileXREpLaunchCombineMemoryKernel(const EpCombineParams &params, const EpHostLaunchContext &context)
{
    if (!TileXREpIsA5Soc()) {
        return TileXR::TILEXR_ERROR_NOT_SUPPORT;
    }

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
    const int registerRet = EnsureCombineMemoryKernelRegistered();
    if (registerRet != TileXR::TILEXR_SUCCESS) {
        return registerRet;
    }

    CombineMemoryKernelArgs args { context.devArgs,
        static_cast<GM_ADDR>(params.expertOut), reinterpret_cast<GM_ADDR>(params.assistInfoForCombine),
        reinterpret_cast<GM_ADDR>(params.epRecvCounts), reinterpret_cast<GM_ADDR>(params.expertScales),
        reinterpret_cast<GM_ADDR>(params.xActiveMask), static_cast<GM_ADDR>(params.sharedExpertX),
        static_cast<GM_ADDR>(params.yOut), params.bs, params.h, params.topK, params.moeExpertNum,
        params.sharedExpertNum, params.sharedExpertRankNum, globalBs, params.activeMaskType,
        params.quantMode, static_cast<int64_t>(params.dtype), magic };
    return LaunchRegisteredKernel(&gCombineMemoryKernelStub, blockDim, params.stream,
        &args, sizeof(args), kCombineMemoryKernelName);
}

int TileXREpLaunchCombineKernel(const EpCombineParams &params, const EpHostLaunchContext &context)
{
    if (!TileXREpIsA5Soc()) {
        return TileXR::TILEXR_ERROR_NOT_SUPPORT;
    }

    int64_t magic = 0;
    const int magicRet = TileXRCommNextMagic(params.comm, &magic);
    if (magicRet != TileXR::TILEXR_SUCCESS) {
        return magicRet;
    }
    const int registerRet = EnsureCombineKernelsRegistered();
    if (registerRet != TileXR::TILEXR_SUCCESS) {
        return registerRet;
    }

    if (TileXREpUsesCrossNodeKernel(context)) {
        CombineCrossNodeKernelArgs args { context.devArgs, static_cast<GM_ADDR>(params.expertOut),
            reinterpret_cast<GM_ADDR>(params.assistInfoForCombine), reinterpret_cast<GM_ADDR>(params.epRecvCounts),
            static_cast<GM_ADDR>(params.yOut), static_cast<GM_ADDR>(params.workspace), params.bs, params.h,
            params.topK, params.moeExpertNum, static_cast<int64_t>(params.dtype), context.window.dtypeBytes,
            context.window.maxRoutesPerSrc, context.window.rowBytes, context.window.payloadBytesPerSlot,
            context.window.assistBytesPerSlot, context.window.slotBytes, context.window.totalBytes, magic };
        int ret = LaunchRegisteredKernel(&gCombineCrossNodeKernelStub, kMvpBlockDim, params.stream,
            &args, sizeof(args), kCombineCrossNodeKernelName);
        if (ret != TileXR::TILEXR_SUCCESS) {
            return ret;
        }

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

        CombineCrossNodeDrainKernelArgs drainArgs { context.devArgs,
            static_cast<GM_ADDR>(params.yOut), static_cast<GM_ADDR>(params.workspace), params.bs, params.h,
            params.topK, params.moeExpertNum, static_cast<int64_t>(params.dtype), context.window.dtypeBytes,
            context.window.maxRoutesPerSrc, context.window.rowBytes, context.window.payloadBytesPerSlot,
            context.window.assistBytesPerSlot, context.window.slotBytes, context.window.totalBytes };
        return LaunchRegisteredKernel(&gCombineCrossNodeDrainKernelStub, kMvpBlockDim, params.stream,
            &drainArgs, sizeof(drainArgs), kCombineCrossNodeDrainKernelName);
    }

    CombineKernelArgs args { context.devArgs, static_cast<GM_ADDR>(params.expertOut),
        reinterpret_cast<GM_ADDR>(params.assistInfoForCombine), reinterpret_cast<GM_ADDR>(params.epRecvCounts),
        static_cast<GM_ADDR>(params.yOut), params.bs, params.h, params.topK, params.moeExpertNum,
        static_cast<int64_t>(params.dtype), context.window.dtypeBytes, context.window.maxRoutesPerSrc,
        context.window.rowBytes, context.window.payloadBytesPerSlot, context.window.assistBytesPerSlot,
        context.window.slotBytes, context.window.totalBytes, magic };
    return LaunchRegisteredKernel(&gCombineKernelStub, kMvpBlockDim, params.stream,
        &args, sizeof(args), kCombineKernelName);
}

} // namespace TileXREp
