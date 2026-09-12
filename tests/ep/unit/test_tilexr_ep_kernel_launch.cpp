#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <map>
#include <string>
#include <vector>

#include "acl/acl_rt.h"
#include "ep_host.h"
#include "ep_kernel_launch.h"
#include "ep_memory_server_dedup_launch.h"
#include "runtime/kernel.h"
#include "tilexr_api.h"
#include "tilexr_kernel_binary_loader.h"
#include "tilexr_types.h"

namespace {

struct LaunchCapture {
    std::string name;
    uint32_t blockDim = 0;
    rtStream_t stream = nullptr;
    std::vector<uint64_t> args;
};

int gFailures = 0;
int gMagicCalls = 0;
constexpr int64_t kNextMagic = 0x1234;
std::string gSocName = "Ascend950PR_9589";
int gBinaryRegisterCalls = 0;
int gBinaryUnregisterCalls = 0;
rtError_t gBinaryRegisterResult = RT_ERROR_NONE;
rtError_t gFunctionRegisterResult = RT_ERROR_NONE;
rtError_t gLaunchResult = RT_ERROR_NONE;
bool gLoadResult = true;
aclError gDeviceResult = ACL_SUCCESS;
int64_t gVectorCores = 48;
int gMagicResult = TileXR::TILEXR_SUCCESS;
std::vector<std::string> gLoadedFiles;
std::vector<std::string> gRegisteredEntries;
std::map<const void *, std::string> gSignatureNames;
std::vector<LaunchCapture> gLaunches;

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
    gBinaryUnregisterCalls = 0;
    gBinaryRegisterResult = RT_ERROR_NONE;
    gFunctionRegisterResult = RT_ERROR_NONE;
    gLaunchResult = RT_ERROR_NONE;
    gLoadResult = true;
    gDeviceResult = ACL_SUCCESS;
    gVectorCores = 48;
    gMagicResult = TileXR::TILEXR_SUCCESS;
    gLoadedFiles.clear();
    gRegisteredEntries.clear();
    gLaunches.clear();
}

TileXREp::EpDispatchParams MakeDispatchParams()
{
    TileXREp::EpDispatchParams params;
    params.x = reinterpret_cast<void *>(0x1000);
    params.expertIds = reinterpret_cast<int32_t *>(0x2000);
    params.scales = reinterpret_cast<void *>(0x2100);
    params.xActiveMask = reinterpret_cast<bool *>(0x2200);
    params.comm = reinterpret_cast<TileXRCommPtr>(0x3000);
    params.bs = 256;
    params.h = 7168;
    params.topK = 6;
    params.moeExpertNum = 16;
    params.epWorldSize = 8;
    params.tpWorldSize = 2;
    params.tpRankId = 1;
    params.sharedExpertNum = 2;
    params.sharedExpertRankNum = 1;
    params.quantMode = 4;
    params.expertTokenNumsType = 1;
    params.expandXOut = reinterpret_cast<void *>(0x4000);
    params.dynamicScalesOut = reinterpret_cast<void *>(0x5000);
    params.expertTokenNumsOut = reinterpret_cast<int64_t *>(0x6000);
    params.epRecvCountsOut = reinterpret_cast<int32_t *>(0x7000);
    params.tpRecvCountsOut = reinterpret_cast<int32_t *>(0x7100);
    params.assistInfoForCombineOut = reinterpret_cast<int32_t *>(0x8000);
    params.workspace = reinterpret_cast<void *>(0x8800);
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
    params.workspace = reinterpret_cast<void *>(0x15000);
    params.dtype = TileXR::TILEXR_DATA_TYPE_BFP16;
    params.stream = reinterpret_cast<aclrtStream>(0x9000);
    return params;
}

TileXR::CommArgs MakeCommArgs(bool crossNode = false)
{
    TileXR::CommArgs args {};
    args.rankSize = 8;
    args.rank = 0;
    args.localRankSize = crossNode ? 4 : 8;
    args.peerMemBytes = TileXREp::kEpMemoryRequiredPeerBytes;
    for (int rank = 0; rank < args.rankSize; ++rank) {
        args.peerMems[rank] = reinterpret_cast<GM_ADDR>(
            static_cast<uintptr_t>(0x100000 + rank * 0x1000));
    }
    return args;
}

TileXREp::EpHostLaunchContext MakeContext(TileXR::CommArgs *commArgs)
{
    TileXREp::EpHostLaunchContext context {commArgs,
        reinterpret_cast<GM_ADDR>(0xa000), {}};
    context.window.dtypeBytes = 2;
    context.window.maxRoutesPerSrc = 192;
    context.window.rowBytes = 14336;
    context.window.payloadRowBytes = 14336;
    context.window.payloadBytesPerSlot = 0x2000;
    context.window.assistBytesPerSlot = 0x1000;
    context.window.slotBytes = 0x4000;
    context.window.totalBytes = 0x20000;
    context.window.rankSize = commArgs->rankSize;
    return context;
}

void CheckLaunch(std::size_t index, const char *name, std::size_t fieldCount)
{
    Check(index < gLaunches.size(), "expected kernel launch exists");
    if (index >= gLaunches.size()) {
        return;
    }
    Check(gLaunches[index].name == name, "runtime launches the registered signature");
    Check(gLaunches[index].args.size() == fieldCount, "runtime argument field count is exact");
}

void TestUnsupportedSocRejectsAllKernels()
{
    TileXR::CommArgs commArgs = MakeCommArgs();
    const TileXREp::EpHostLaunchContext context = MakeContext(&commArgs);
    const TileXREp::EpDispatchParams dispatch = MakeDispatchParams();
    const TileXREp::EpCombineParams combine = MakeCombineParams();

    ResetCapture();
    gSocName = "Ascend910B3";
    Check(TileXREp::TileXREpLaunchDispatchKernel(dispatch, context) ==
        TileXR::TILEXR_ERROR_NOT_SUPPORT, "ordinary dispatch rejects 910B");
    Check(TileXREp::TileXREpLaunchCombineKernel(combine, context) ==
        TileXR::TILEXR_ERROR_NOT_SUPPORT, "ordinary combine rejects 910B");
    Check(TileXREp::TileXREpLaunchDispatchMemoryKernel(dispatch, context) ==
        TileXR::TILEXR_ERROR_NOT_SUPPORT, "memory dispatch rejects 910B");
    Check(TileXREp::TileXREpLaunchCombineMemoryKernel(combine, context) ==
        TileXR::TILEXR_ERROR_NOT_SUPPORT, "memory combine rejects 910B");
    Check(gMagicCalls == 0 && gLoadedFiles.empty() && gBinaryRegisterCalls == 0 && gLaunches.empty(),
        "unsupported SoC is rejected before state mutation or binary loading");
    gSocName = "Ascend950PR_9589";
}

void TestServerDedupRuntimeLaunch()
{
    TileXR::CommArgs commArgs = MakeCommArgs();
    commArgs.rankSize = 64;
    const auto context = MakeContext(&commArgs);
    TileXREp::EpMemoryServerDedupParams params {};
    auto &p = params.dispatch;
    p.x = reinterpret_cast<void *>(0x1000);
    p.expertIds = reinterpret_cast<int32_t *>(0x2000);
    p.expertScales = reinterpret_cast<void *>(0x3000);
    p.comm = reinterpret_cast<TileXRCommPtr>(0x4000);
    p.expandXOut = reinterpret_cast<void *>(0x5000);
    p.expertTokenNumsOut = reinterpret_cast<int64_t *>(0x6000);
    p.epRecvCountsOut = reinterpret_cast<int32_t *>(0x7000);
    p.bs = 256; p.h = 7168; p.topK = 6; p.moeExpertNum = 384;
    p.expertTokenNumsType = 1;
    p.dtype = TileXR::TILEXR_DATA_TYPE_BFP16;
    p.expandXOutDtype = TileXR::TILEXR_DATA_TYPE_RESERVED;
    p.stream = reinterpret_cast<aclrtStream>(0x8000);
    params.assistExOut = reinterpret_cast<uint32_t *>(0x9000);
    params.sourceMaskOut = reinterpret_cast<uint32_t *>(0xa000);
    params.groupOut = reinterpret_cast<uint32_t *>(0xb000);
    TileXREp::EpMemoryServerDedupLayout layout {};
    layout.globalBs = 64 * 256;
    auto launch = [&]() { return TileXREp::TileXREpLaunchDispatchMemoryServerDedupKernel(params, context, layout); };

    ResetCapture();
    gVectorCores = 64;
    gSocName = "Ascend910B3";
    Check(launch() == TileXR::TILEXR_ERROR_NOT_SUPPORT, "server dedup rejects non-A5");
    Check(TileXREp::TileXREpRegisterDispatchMemoryServerDedupKernel() == TileXR::TILEXR_ERROR_NOT_SUPPORT,
        "registration-only entry rejects non-A5");
    Check(gMagicCalls == 0 && gLoadedFiles.empty() && gLaunches.empty(), "non-A5 causes no side effects");
    gSocName = "Ascend950PR_9589";

    ResetCapture();
    gVectorCores = 64;
    gDeviceResult = 1;
    Check(launch() == TileXR::TILEXR_ERROR_INTERNAL, "device query failure propagates");
    gDeviceResult = ACL_SUCCESS;
    gVectorCores = 0;
    Check(launch() == TileXR::TILEXR_ERROR_INTERNAL, "zero vector cores rejected");
    gVectorCores = static_cast<int64_t>(UINT32_MAX) + 1;
    Check(launch() == TileXR::TILEXR_ERROR_INTERNAL, "core count narrowing overflow rejected");
    gVectorCores = 48;
    Check(launch() == TileXR::TILEXR_ERROR_NOT_SUPPORT, "fewer than 64 AIVs rejected");
    gVectorCores = 64;
    Check(gMagicCalls == 0 && gLoadedFiles.empty() && gLaunches.empty(), "bad launch settings consume no magic");

    ResetCapture();
    gVectorCores = 64;
    gLoadResult = false;
    Check(launch() == TileXR::TILEXR_ERROR_NOT_INITIALIZED, "missing artifact is an error");
    Check(gMagicCalls == 0 && gBinaryRegisterCalls == 0 && gLaunches.empty(), "missing artifact does not register or launch");
    ResetCapture();
    gVectorCores = 64;
    gBinaryRegisterResult = -1;
    Check(launch() == TileXR::TILEXR_ERROR_MKIRT, "binary registration failure propagates");
    Check(gMagicCalls == 0 && gLaunches.empty(), "binary registration failure consumes no magic");
    ResetCapture();
    gVectorCores = 64;
    gFunctionRegisterResult = -1;
    Check(launch() == TileXR::TILEXR_ERROR_MKIRT, "function registration failure propagates");
    Check(gBinaryUnregisterCalls == 1 && gMagicCalls == 0 && gLaunches.empty(), "failed function registration rolls back binary");

    ResetCapture();
    gVectorCores = 64;
    gMagicResult = TileXR::TILEXR_ERROR_NOT_INITIALIZED;
    Check(launch() == TileXR::TILEXR_ERROR_NOT_INITIALIZED, "magic allocation failure propagates");
    Check(gMagicCalls == 1 && gLaunches.empty(), "failed magic allocation prevents launch");
    Check(gLoadedFiles == std::vector<std::string>{"tilexr_ep_dispatch_memory_server_dedup_kernel.o"},
        "dedup loads its own artifact");
    Check(gRegisteredEntries == std::vector<std::string>{"tilexr_ep_dispatch_memory_server_dedup_kernel"},
        "dedup registers its own signature");

    ResetCapture();
    gVectorCores = 64;
    Check(launch() == TileXR::TILEXR_SUCCESS, "count server dedup launch succeeds");
    CheckLaunch(0, "tilexr_ep_dispatch_memory_server_dedup_kernel", 20);
    const std::vector<uint64_t> expected = {reinterpret_cast<uint64_t>(context.devArgs),
        0x1000, 0x2000, 0x3000, 0x5000, 0x6000, 0x7000, 0, 0x9000, 0xa000, 0xb000,
        256, 7168, 6, 384, 64 * 256, 1, static_cast<uint64_t>(p.dtype), static_cast<uint64_t>(p.dtype), kNextMagic};
    Check(gLaunches[0].args == expected, "all twenty kernel arguments preserve order, widths and values");
    Check(gLaunches[0].blockDim == 64 && gLaunches[0].stream == p.stream, "uses fixed 64 AIVs and caller stream");
    Check(gBinaryRegisterCalls == 0 && gMagicCalls == 1, "registered binary reused with exactly one magic per launch");

    ResetCapture();
    gVectorCores = 64;
    const int rounds = static_cast<int>(TileXR::PING_PONG_SIZE) + 3;
    for (int round = 0; round < rounds; ++round) {
        Check(launch() == TileXR::TILEXR_SUCCESS, "reused server dedup launch succeeds");
        auto roundArgs = expected;
        roundArgs.back() = kNextMagic + round;
        Check(gLaunches.size() == static_cast<std::size_t>(round + 1) &&
            gLaunches.back().args == roundArgs && gLaunches.back().stream == p.stream,
            "each round forwards its new magic and reuses all pointers and the stream");
    }
    Check(gMagicCalls == rounds && gBinaryRegisterCalls == 0 && gLoadedFiles.empty(),
        "multiple rounds consume exactly one magic each without reloading the artifact");

    ResetCapture();
    gVectorCores = 96;
    p.dtype = TileXR::TILEXR_DATA_TYPE_FP16;
    p.expandXOutDtype = p.dtype;
    p.assistInfoForCombineOut = reinterpret_cast<int32_t *>(0xc000);
    Check(launch() == TileXR::TILEXR_SUCCESS && gLaunches[0].blockDim == 64 &&
        gLaunches[0].args[7] == 0xc000 && gLaunches[0].args[18] == static_cast<uint64_t>(p.dtype),
        "larger devices retain fixed 48/16 partition and explicit FP16 ABI");
    ResetCapture();
    gVectorCores = 64;
    gLaunchResult = -1;
    Check(launch() == TileXR::TILEXR_ERROR_MKIRT, "runtime submission failure is never PASS");
    Check(gMagicCalls == 1 && gLaunches.size() == 1, "failed submission consumes only its own magic");
    gLaunchResult = RT_ERROR_NONE;
    Check(launch() == TileXR::TILEXR_SUCCESS && gMagicCalls == 2 &&
        gLaunches.back().args.back() == static_cast<uint64_t>(kNextMagic + 1),
        "a later invocation cannot reuse a failed submission's magic");
}

void TestOrdinaryDispatchRuntimeLaunch()
{
    TileXR::CommArgs localArgs = MakeCommArgs();
    TileXREp::EpHostLaunchContext localContext = MakeContext(&localArgs);
    const TileXREp::EpDispatchParams params = MakeDispatchParams();

    ResetCapture();
    gBinaryRegisterResult = -1;
    Check(TileXREp::TileXREpLaunchDispatchKernel(params, localContext) ==
        TileXR::TILEXR_ERROR_MKIRT, "ordinary dispatch registration failure is returned");
    Check(gMagicCalls == 1 && gBinaryRegisterCalls == 1 && gLaunches.empty(),
        "ordinary dispatch registration failure does not launch");

    ResetCapture();
    Check(TileXREp::TileXREpLaunchDispatchKernel(params, localContext) ==
        TileXR::TILEXR_SUCCESS, "ordinary dispatch runtime kernel launches");
    Check(gLoadedFiles.size() == 1 && gLoadedFiles[0] == "tilexr_ep_dispatch_kernel.o",
        "ordinary dispatch loads the A5 object");
    Check(gRegisteredEntries == std::vector<std::string>({
        "tilexr_ep_dispatch_kernel", "tilexr_ep_dispatch_cross_node_kernel"}),
        "ordinary dispatch registers both entry points");
    CheckLaunch(0, "tilexr_ep_dispatch_kernel", 31);
    Check(gLaunches[0].blockDim == 1, "ordinary dispatch keeps MVP block dimension");
    Check(gLaunches[0].args[0] == reinterpret_cast<uint64_t>(localContext.devArgs) &&
        gLaunches[0].args[1] == reinterpret_cast<uint64_t>(params.x) &&
        gLaunches[0].args[11] == reinterpret_cast<uint64_t>(params.workspace) &&
        gLaunches[0].args[30] == static_cast<uint64_t>(kNextMagic),
        "ordinary dispatch argument order preserves pointer and magic fields");

    TileXR::CommArgs crossArgs = MakeCommArgs(true);
    TileXREp::EpHostLaunchContext crossContext = MakeContext(&crossArgs);
    ResetCapture();
    Check(TileXREp::TileXREpLaunchDispatchKernel(params, crossContext) ==
        TileXR::TILEXR_SUCCESS, "cross-node dispatch runtime kernel launches");
    Check(gBinaryRegisterCalls == 0, "ordinary dispatch registration is process-wide");
    CheckLaunch(0, "tilexr_ep_dispatch_cross_node_kernel", 31);
}

void TestOrdinaryCombineRuntimeLaunch()
{
    TileXR::CommArgs localArgs = MakeCommArgs();
    TileXREp::EpHostLaunchContext localContext = MakeContext(&localArgs);
    const TileXREp::EpCombineParams params = MakeCombineParams();

    ResetCapture();
    gBinaryRegisterResult = -1;
    Check(TileXREp::TileXREpLaunchCombineKernel(params, localContext) ==
        TileXR::TILEXR_ERROR_MKIRT, "ordinary combine registration failure is returned");
    Check(gMagicCalls == 1 && gBinaryRegisterCalls == 1 && gLaunches.empty(),
        "ordinary combine registration failure does not launch");

    ResetCapture();
    Check(TileXREp::TileXREpLaunchCombineKernel(params, localContext) ==
        TileXR::TILEXR_SUCCESS, "ordinary combine runtime kernel launches");
    Check(gLoadedFiles.size() == 1 && gLoadedFiles[0] == "tilexr_ep_combine_kernel.o",
        "ordinary combine loads the A5 object");
    Check(gRegisteredEntries == std::vector<std::string>({
        "tilexr_ep_combine_kernel", "tilexr_ep_combine_cross_node_kernel",
        "tilexr_ep_combine_cross_node_drain_kernel"}),
        "ordinary combine registers all three entry points");
    CheckLaunch(0, "tilexr_ep_combine_kernel", 18);
    Check(gLaunches[0].args[0] == reinterpret_cast<uint64_t>(localContext.devArgs) &&
        gLaunches[0].args[4] == reinterpret_cast<uint64_t>(params.yOut) &&
        gLaunches[0].args[17] == static_cast<uint64_t>(kNextMagic),
        "ordinary combine argument order preserves pointer and magic fields");

    TileXR::CommArgs crossArgs = MakeCommArgs(true);
    TileXREp::EpHostLaunchContext crossContext = MakeContext(&crossArgs);
    ResetCapture();
    Check(TileXREp::TileXREpLaunchCombineKernel(params, crossContext) ==
        TileXR::TILEXR_SUCCESS, "cross-node combine and drain kernels launch");
    Check(gBinaryRegisterCalls == 0, "ordinary combine registration is process-wide");
    Check(gLaunches.size() == 2, "cross-node combine launches transfer then drain");
    CheckLaunch(0, "tilexr_ep_combine_cross_node_kernel", 19);
    CheckLaunch(1, "tilexr_ep_combine_cross_node_drain_kernel", 15);
}

void TestDispatchMemoryRuntimeLaunch()
{
    TileXR::CommArgs commArgs = MakeCommArgs();
    const TileXREp::EpHostLaunchContext context = MakeContext(&commArgs);
    TileXREp::EpDispatchParams params = MakeDispatchParams();
    params.scales = nullptr;
    params.xActiveMask = nullptr;
    params.tpWorldSize = 0;
    params.tpRankId = 0;
    params.tpRecvCountsOut = nullptr;
    params.sharedExpertNum = 0;
    params.sharedExpertRankNum = 0;
    params.workspace = nullptr;

    ResetCapture();
    TileXREp::EpDispatchParams invalid = params;
    invalid.h = 0;
    Check(TileXREp::TileXREpLaunchDispatchMemoryKernel(invalid, context) ==
        TileXR::TILEXR_ERROR_PARA_CHECK_FAIL, "invalid memory dispatch configuration is rejected");
    Check(gMagicCalls == 0 && gBinaryRegisterCalls == 0 && gLaunches.empty(),
        "invalid memory dispatch does not consume launch state");

    ResetCapture();
    Check(TileXREp::TileXREpLaunchDispatchMemoryKernel(params, context) ==
        TileXR::TILEXR_SUCCESS, "memory dispatch runtime kernel launches");
    Check(gLoadedFiles.size() == 1 && gLoadedFiles[0] == "tilexr_ep_dispatch_memory_kernel.o",
        "memory dispatch loads its A5 object");
    Check(gRegisteredEntries == std::vector<std::string>({"tilexr_ep_dispatch_memory_kernel"}),
        "memory dispatch registers its exact entry point");
    CheckLaunch(0, "tilexr_ep_dispatch_memory_kernel", 22);
    Check(gLaunches[0].blockDim == 48, "memory dispatch uses runtime vector core count");
}

void TestCombineMemoryRuntimeLaunch()
{
    TileXR::CommArgs commArgs = MakeCommArgs();
    const TileXREp::EpHostLaunchContext context = MakeContext(&commArgs);
    const TileXREp::EpCombineParams params = MakeCombineParams();

    ResetCapture();
    TileXREp::EpCombineParams invalid = params;
    invalid.h = 0;
    Check(TileXREp::TileXREpLaunchCombineMemoryKernel(invalid, context) ==
        TileXR::TILEXR_ERROR_PARA_CHECK_FAIL, "invalid memory combine configuration is rejected");
    Check(gMagicCalls == 0 && gBinaryRegisterCalls == 0 && gLaunches.empty(),
        "invalid memory combine does not consume launch state");

    ResetCapture();
    Check(TileXREp::TileXREpLaunchCombineMemoryKernel(params, context) ==
        TileXR::TILEXR_SUCCESS, "memory combine runtime kernel launches");
    Check(gLoadedFiles.size() == 1 && gLoadedFiles[0] == "tilexr_ep_combine_memory_kernel.o",
        "memory combine loads its A5 object");
    Check(gRegisteredEntries == std::vector<std::string>({"tilexr_ep_combine_memory_kernel"}),
        "memory combine registers its exact entry point");
    CheckLaunch(0, "tilexr_ep_combine_memory_kernel", 19);
    Check(gLaunches[0].blockDim == 48, "memory combine uses runtime vector core count");
}

} // namespace

namespace TileXR {

bool LoadKernelBinary(const void *, const char *fileName, KernelBinaryView *binary)
{
    static const unsigned char kernelBinary[] = {0x01};
    if (!gLoadResult || fileName == nullptr || binary == nullptr) {
        return false;
    }
    const std::string name(fileName);
    if (name != "tilexr_ep_dispatch_kernel.o" && name != "tilexr_ep_combine_kernel.o" &&
        name != "tilexr_ep_dispatch_memory_kernel.o" &&
        name != "tilexr_ep_combine_memory_kernel.o" &&
        name != "tilexr_ep_dispatch_memory_server_dedup_kernel.o") {
        return false;
    }
    gLoadedFiles.push_back(name);
    binary->data = kernelBinary;
    binary->size = sizeof(kernelBinary);
    return true;
}

} // namespace TileXR

extern "C" {

int TileXRCommNextMagic(TileXRCommPtr, int64_t *magic)
{
    ++gMagicCalls;
    if (magic != nullptr) {
        *magic = kNextMagic + gMagicCalls - 1;
    }
    return gMagicResult;
}

int TileXRGetCommArgsHost(TileXRCommPtr, TileXR::CommArgs *&)
{
    return TileXR::TILEXR_ERROR_NOT_SUPPORT;
}

int TileXRGetCommArgsDev(TileXRCommPtr, GM_ADDR &)
{
    return TileXR::TILEXR_ERROR_NOT_SUPPORT;
}

const char *aclrtGetSocName()
{
    return gSocName.c_str();
}

aclError aclrtGetDevice(int32_t *deviceId)
{
    if (deviceId != nullptr) {
        *deviceId = 0;
    }
    return gDeviceResult;
}

aclError aclrtGetDeviceInfo(uint32_t, aclrtDevAttr attr, int64_t *value)
{
    if (value != nullptr) {
        *value = attr == ACL_DEV_ATTR_VECTOR_CORE_NUM ? gVectorCores : 192 * 1024;
    }
    return ACL_SUCCESS;
}

aclError aclrtSynchronizeStream(aclrtStream)
{
    return ACL_SUCCESS;
}

aclError aclrtMemcpy(void *dst, std::size_t destMax, const void *,
    std::size_t count, aclrtMemcpyKind)
{
    if (dst != nullptr && count <= destMax) {
        std::memset(dst, 0, count);
    }
    return ACL_SUCCESS;
}

rtError_t rtDevBinaryRegister(const rtDevBinary_t *, void **handle)
{
    ++gBinaryRegisterCalls;
    if (gBinaryRegisterResult == RT_ERROR_NONE && handle != nullptr) {
        *handle = reinterpret_cast<void *>(static_cast<uintptr_t>(0xb000 + gBinaryRegisterCalls));
    }
    return gBinaryRegisterResult;
}

rtError_t rtDevBinaryUnRegister(void *)
{
    ++gBinaryUnregisterCalls;
    return RT_ERROR_NONE;
}

rtError_t rtFunctionRegister(void *, const void *signature, const char_t *name,
    const void *, uint32_t)
{
    const std::string entry = name == nullptr ? "" : name;
    gRegisteredEntries.push_back(entry);
    gSignatureNames[signature] = entry;
    return gFunctionRegisterResult;
}

rtError_t rtKernelLaunchWithFlagV2(const void *signature, uint32_t blockDim,
    rtArgsEx_t *argsInfo, rtSmDesc_t *, rtStream_t stream, uint32_t,
    const rtTaskCfgInfo_t *)
{
    LaunchCapture capture;
    const auto nameIt = gSignatureNames.find(signature);
    capture.name = nameIt == gSignatureNames.end() ? "" : nameIt->second;
    capture.blockDim = blockDim;
    capture.stream = stream;
    if (argsInfo != nullptr && argsInfo->args != nullptr) {
        const std::size_t fieldCount = argsInfo->argsSize / sizeof(uint64_t);
        const uint64_t *fields = static_cast<const uint64_t *>(argsInfo->args);
        capture.args.assign(fields, fields + fieldCount);
    }
    gLaunches.push_back(capture);
    return gLaunchResult;
}

} // extern "C"

int main()
{
    TestUnsupportedSocRejectsAllKernels();
    TestOrdinaryDispatchRuntimeLaunch();
    TestOrdinaryCombineRuntimeLaunch();
    TestDispatchMemoryRuntimeLaunch();
    TestCombineMemoryRuntimeLaunch();
    TestServerDedupRuntimeLaunch();
    std::cout << "EP_SERVER_DEDUP_EMPTY_KERNEL_" << (gFailures == 0 ? "PASS" : "FAIL")
              << " test=launch" << std::endl;
    return gFailures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
