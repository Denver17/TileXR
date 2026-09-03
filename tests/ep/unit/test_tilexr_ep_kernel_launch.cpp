#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>

#include "acl/acl_rt.h"
#include "ep_host.h"
#include "ep_kernel_launch.h"
#include "runtime/kernel.h"
#include "tilexr_api.h"
#include "tilexr_types.h"

namespace {

int gFailures = 0;
int gMagicCalls = 0;
constexpr int64_t kNextMagic = 0x1234;
int gBinaryRegisterCalls = 0;
int gFunctionRegisterCalls = 0;
int gLaunchCalls = 0;
rtError_t gBinaryRegisterResult = RT_ERROR_NONE;
rtError_t gFunctionRegisterResult = RT_ERROR_NONE;
rtError_t gLaunchResult = RT_ERROR_NONE;
uint32_t gLaunchBlockDim = 0;
std::string gRegisteredEntry;
std::array<uint64_t, 22> gLaunchArgs {};
std::size_t gLaunchArgsSize = 0;

void Check(bool condition, const char *message)
{
    if (!condition) {
        std::cerr << "FAILED: " << message << std::endl;
        ++gFailures;
    }
}

void ResetCapture()
{
    gMagicCalls = 0;
    gBinaryRegisterCalls = 0;
    gFunctionRegisterCalls = 0;
    gLaunchCalls = 0;
    gBinaryRegisterResult = RT_ERROR_NONE;
    gFunctionRegisterResult = RT_ERROR_NONE;
    gLaunchResult = RT_ERROR_NONE;
    gLaunchBlockDim = 0;
    gRegisteredEntry.clear();
    gLaunchArgs.fill(0);
    gLaunchArgsSize = 0;
}

TileXREp::EpDispatchParams MakeDispatchParams()
{
    TileXREp::EpDispatchParams params;
    params.x = reinterpret_cast<void *>(0x1000);
    params.expertIds = reinterpret_cast<int32_t *>(0x2000);
    params.comm = reinterpret_cast<TileXRCommPtr>(0x3000);
    params.bs = 256;
    params.h = 7168;
    params.topK = 6;
    params.moeExpertNum = 16;
    params.epWorldSize = 8;
    params.quantMode = 4;
    params.expertTokenNumsType = 1;
    params.expandXOut = reinterpret_cast<void *>(0x4000);
    params.dynamicScalesOut = reinterpret_cast<void *>(0x5000);
    params.expertTokenNumsOut = reinterpret_cast<int64_t *>(0x6000);
    params.epRecvCountsOut = reinterpret_cast<int32_t *>(0x7000);
    params.assistInfoForCombineOut = reinterpret_cast<int32_t *>(0x8000);
    params.dtype = TileXR::TILEXR_DATA_TYPE_BFP16;
    params.expandXOutDtype = TileXR::TILEXR_DATA_TYPE_FP8E4M3;
    params.stream = reinterpret_cast<aclrtStream>(0x9000);
    return params;
}

TileXREp::EpCombineParams MakeCombineParams()
{
    TileXREp::EpCombineParams params;
    params.expertOut = reinterpret_cast<void *>(0x11000);
    params.assistInfoForCombine = reinterpret_cast<int32_t *>(0x12000);
    params.epRecvCounts = reinterpret_cast<int32_t *>(0x13000);
    params.comm = reinterpret_cast<TileXRCommPtr>(0x3000);
    params.bs = 256;
    params.h = 7168;
    params.topK = 2;
    params.moeExpertNum = 16;
    params.epWorldSize = 8;
    params.yOut = reinterpret_cast<void *>(0x14000);
    params.dtype = TileXR::TILEXR_DATA_TYPE_BFP16;
    params.stream = reinterpret_cast<aclrtStream>(0x9000);
    return params;
}

TileXR::CommArgs MakeCommArgs()
{
    TileXR::CommArgs args {};
    args.rankSize = 8;
    args.rank = 0;
    args.localRankSize = 8;
    args.peerMemBytes = TileXREp::kEpMemoryRequiredPeerBytes;
    for (int rank = 0; rank < args.rankSize; ++rank) {
        args.peerMems[rank] = reinterpret_cast<GM_ADDR>(
            static_cast<uintptr_t>(0x100000 + rank * 0x1000));
    }
    return args;
}

void CheckDispatchArgs(const TileXREp::EpDispatchParams &params,
    const TileXREp::EpHostLaunchContext &context)
{
    const std::array<uint64_t, 22> expected {{
        reinterpret_cast<uint64_t>(context.devArgs), reinterpret_cast<uint64_t>(params.x),
        reinterpret_cast<uint64_t>(params.expertIds), 0,
        reinterpret_cast<uint64_t>(params.expandXOut),
        reinterpret_cast<uint64_t>(params.dynamicScalesOut),
        reinterpret_cast<uint64_t>(params.expertTokenNumsOut),
        reinterpret_cast<uint64_t>(params.epRecvCountsOut),
        reinterpret_cast<uint64_t>(params.assistInfoForCombineOut),
        static_cast<uint64_t>(params.bs), static_cast<uint64_t>(params.h),
        static_cast<uint64_t>(params.topK), static_cast<uint64_t>(params.moeExpertNum),
        0, 0, static_cast<uint64_t>(params.bs * context.hostArgs->rankSize),
        1, 0, 4, static_cast<uint64_t>(params.dtype),
        static_cast<uint64_t>(params.expandXOutDtype), static_cast<uint64_t>(kNextMagic)
    }};
    Check(gLaunchArgsSize == expected.size() * sizeof(uint64_t),
        "dispatch argument ABI is 22 fields");
    Check(gLaunchArgs == expected, "dispatch runtime arguments preserve order");
}

void CheckCombineArgs(const TileXREp::EpCombineParams &params,
    const TileXREp::EpHostLaunchContext &context)
{
    const std::array<uint64_t, 19> expected {{
        reinterpret_cast<uint64_t>(context.devArgs), reinterpret_cast<uint64_t>(params.expertOut),
        reinterpret_cast<uint64_t>(params.assistInfoForCombine),
        reinterpret_cast<uint64_t>(params.epRecvCounts), 0, 0, 0,
        reinterpret_cast<uint64_t>(params.yOut), static_cast<uint64_t>(params.bs),
        static_cast<uint64_t>(params.h), static_cast<uint64_t>(params.topK),
        static_cast<uint64_t>(params.moeExpertNum), 0, 0,
        static_cast<uint64_t>(params.bs * context.hostArgs->rankSize), 0, 0,
        static_cast<uint64_t>(params.dtype), static_cast<uint64_t>(kNextMagic)
    }};
    Check(gLaunchArgsSize == expected.size() * sizeof(uint64_t),
        "combine argument ABI is 19 fields");
    Check(std::equal(expected.begin(), expected.end(), gLaunchArgs.begin()),
        "combine runtime arguments preserve order");
}

void TestDispatchRuntimeLaunch()
{
    TileXR::CommArgs commArgs = MakeCommArgs();
    TileXREp::EpHostLaunchContext context {&commArgs,
        reinterpret_cast<GM_ADDR>(0xa000), {}};
    const TileXREp::EpDispatchParams params = MakeDispatchParams();

    ResetCapture();
    TileXREp::EpDispatchParams invalid = params;
    invalid.h = 0;
    Check(TileXREp::TileXREpLaunchDispatchMemoryKernel(invalid, context) ==
        TileXR::TILEXR_ERROR_PARA_CHECK_FAIL, "invalid dispatch configuration is rejected");
    Check(gMagicCalls == 0 && gBinaryRegisterCalls == 0 && gLaunchCalls == 0,
        "invalid dispatch does not consume launch state");

    ResetCapture();
    gBinaryRegisterResult = -1;
    Check(TileXREp::TileXREpLaunchDispatchMemoryKernel(params, context) ==
        TileXR::TILEXR_ERROR_MKIRT, "dispatch registration failure is returned");
    Check(gMagicCalls == 1 && gBinaryRegisterCalls == 1 && gLaunchCalls == 0,
        "dispatch registration failure does not launch");

    ResetCapture();
    Check(TileXREp::TileXREpLaunchDispatchMemoryKernel(params, context) ==
        TileXR::TILEXR_SUCCESS, "dispatch runtime kernel launches");
    Check(gMagicCalls == 1 && gBinaryRegisterCalls == 1 &&
        gFunctionRegisterCalls == 1 && gLaunchCalls == 1,
        "dispatch embedded kernel registers and launches once");
    Check(gRegisteredEntry == "tilexr_ep_dispatch_memory_kernel",
        "dispatch runtime entry is registered");
    Check(gLaunchBlockDim == 48, "dispatch uses resolved runtime block dimension");
    CheckDispatchArgs(params, context);

    Check(TileXREp::TileXREpLaunchDispatchMemoryKernel(params, context) ==
        TileXR::TILEXR_SUCCESS, "repeated dispatch runtime launch succeeds");
    Check(gMagicCalls == 2 && gBinaryRegisterCalls == 1 &&
        gFunctionRegisterCalls == 1 && gLaunchCalls == 2,
        "dispatch embedded kernel registration is process-wide");
}

void TestCombineRuntimeLaunch()
{
    TileXR::CommArgs commArgs = MakeCommArgs();
    TileXREp::EpHostLaunchContext context {&commArgs,
        reinterpret_cast<GM_ADDR>(0xa000), {}};
    const TileXREp::EpCombineParams params = MakeCombineParams();

    ResetCapture();
    TileXREp::EpCombineParams invalid = params;
    invalid.h = 0;
    Check(TileXREp::TileXREpLaunchCombineMemoryKernel(invalid, context) ==
        TileXR::TILEXR_ERROR_PARA_CHECK_FAIL, "invalid combine configuration is rejected");
    Check(gMagicCalls == 0 && gBinaryRegisterCalls == 0 && gLaunchCalls == 0,
        "invalid combine does not consume launch state");

    ResetCapture();
    gBinaryRegisterResult = -1;
    Check(TileXREp::TileXREpLaunchCombineMemoryKernel(params, context) ==
        TileXR::TILEXR_ERROR_MKIRT, "combine registration failure is returned");
    Check(gMagicCalls == 1 && gBinaryRegisterCalls == 1 && gLaunchCalls == 0,
        "combine registration failure does not launch");

    ResetCapture();
    Check(TileXREp::TileXREpLaunchCombineMemoryKernel(params, context) ==
        TileXR::TILEXR_SUCCESS, "combine runtime kernel launches");
    Check(gMagicCalls == 1 && gBinaryRegisterCalls == 1 &&
        gFunctionRegisterCalls == 1 && gLaunchCalls == 1,
        "combine embedded kernel registers and launches once");
    Check(gRegisteredEntry == "tilexr_ep_combine_memory_kernel",
        "combine runtime entry is registered");
    Check(gLaunchBlockDim == 48, "combine uses resolved runtime block dimension");
    CheckCombineArgs(params, context);
}

} // namespace

extern "C" {

extern const unsigned char TileXREpDispatchMemoryKernelBinaryData[] = {0x01};
extern const std::size_t TileXREpDispatchMemoryKernelBinarySize = 1;
extern const unsigned char TileXREpCombineMemoryKernelBinaryData[] = {0x02};
extern const std::size_t TileXREpCombineMemoryKernelBinarySize = 1;

int TileXRCommNextMagic(TileXRCommPtr, int64_t *magic)
{
    ++gMagicCalls;
    if (magic != nullptr) {
        *magic = kNextMagic;
    }
    return TileXR::TILEXR_SUCCESS;
}

int TileXRGetCommArgsHost(TileXRCommPtr, TileXR::CommArgs *&)
{
    return TileXR::TILEXR_ERROR_NOT_SUPPORT;
}

int TileXRGetCommArgsDev(TileXRCommPtr, GM_ADDR &)
{
    return TileXR::TILEXR_ERROR_NOT_SUPPORT;
}

aclError aclrtGetDevice(int32_t *deviceId)
{
    if (deviceId != nullptr) {
        *deviceId = 0;
    }
    return ACL_SUCCESS;
}

aclError aclrtGetDeviceInfo(uint32_t, aclrtDevAttr attr, int64_t *value)
{
    if (value != nullptr) {
        *value = attr == ACL_DEV_ATTR_VECTOR_CORE_NUM ? 48 : 192 * 1024;
    }
    return ACL_SUCCESS;
}

aclError aclrtSynchronizeStream(aclrtStream)
{
    return ACL_SUCCESS;
}

aclError aclrtMemcpy(void *dst, std::size_t destMax, const void *src,
    std::size_t count, aclrtMemcpyKind)
{
    if (dst != nullptr && src != nullptr && count <= destMax) {
        std::memcpy(dst, src, count);
    }
    return ACL_SUCCESS;
}

rtError_t rtDevBinaryRegister(const rtDevBinary_t *, void **handle)
{
    ++gBinaryRegisterCalls;
    if (gBinaryRegisterResult == RT_ERROR_NONE && handle != nullptr) {
        *handle = reinterpret_cast<void *>(0xb000);
    }
    return gBinaryRegisterResult;
}

rtError_t rtFunctionRegister(void *, const void *, const char_t *name,
    const void *, uint32_t)
{
    ++gFunctionRegisterCalls;
    gRegisteredEntry = name == nullptr ? "" : name;
    return gFunctionRegisterResult;
}

rtError_t rtKernelLaunchWithFlagV2(const void *, uint32_t blockDim,
    rtArgsEx_t *argsInfo, rtSmDesc_t *, rtStream_t, uint32_t,
    const rtTaskCfgInfo_t *)
{
    ++gLaunchCalls;
    gLaunchBlockDim = blockDim;
    gLaunchArgsSize = argsInfo == nullptr ? 0 : argsInfo->argsSize;
    gLaunchArgs.fill(0);
    if (argsInfo != nullptr && argsInfo->args != nullptr) {
        const std::size_t copyBytes = gLaunchArgsSize < sizeof(gLaunchArgs) ?
            gLaunchArgsSize : sizeof(gLaunchArgs);
        std::memcpy(gLaunchArgs.data(), argsInfo->args, copyBytes);
    }
    return gLaunchResult;
}

} // extern "C"

void launch_tilexr_ep_dispatch_kernel(uint32_t, void *, GM_ADDR, GM_ADDR,
    GM_ADDR, GM_ADDR, GM_ADDR, GM_ADDR, GM_ADDR, GM_ADDR, GM_ADDR, GM_ADDR,
    GM_ADDR, GM_ADDR, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t,
    int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t,
    int64_t, int64_t, int64_t, int64_t, int64_t)
{}

void launch_tilexr_ep_dispatch_cross_node_kernel(uint32_t, void *, GM_ADDR,
    GM_ADDR, GM_ADDR, GM_ADDR, GM_ADDR, GM_ADDR, GM_ADDR, GM_ADDR, GM_ADDR,
    GM_ADDR, GM_ADDR, GM_ADDR, int64_t, int64_t, int64_t, int64_t, int64_t,
    int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t,
    int64_t, int64_t, int64_t, int64_t, int64_t, int64_t)
{}

void launch_tilexr_ep_combine_kernel(uint32_t, void *, GM_ADDR, GM_ADDR, GM_ADDR,
    GM_ADDR, GM_ADDR, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t,
    int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t)
{}

void launch_tilexr_ep_combine_cross_node_kernel(uint32_t, void *, GM_ADDR,
    GM_ADDR, GM_ADDR, GM_ADDR, GM_ADDR, GM_ADDR, int64_t, int64_t, int64_t,
    int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t,
    int64_t, int64_t)
{}

void launch_tilexr_ep_combine_cross_node_drain_kernel(uint32_t, void *, GM_ADDR,
    GM_ADDR, GM_ADDR, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t,
    int64_t, int64_t, int64_t, int64_t, int64_t, int64_t)
{}

int main()
{
    TestDispatchRuntimeLaunch();
    TestCombineRuntimeLaunch();
    return gFailures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
