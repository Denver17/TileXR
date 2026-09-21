#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "acl/acl_rt.h"
#include "ep_host.h"
#include "ep_kernel_launch.h"
#include "ep_memory_server_dedup_launch.h"
#include "ep_memory_server_dedup_combine_launch.h"
#include "server_dedup_combine_test_fixture.h"
#include "runtime/kernel.h"
#include "tilexr_api.h"
#include "tilexr_kernel_binary_loader.h"
#include "tilexr_types.h"
#include "../../../src/comm/tilexr_kernel_tiling_cache.h"
#include "../../../src/comm/tilexr_internal.h"

namespace {

struct LaunchCapture {
    std::string name;
    uint32_t blockDim = 0;
    rtStream_t stream = nullptr;
    std::vector<uint64_t> args;
    std::vector<uint8_t> tiling;
    uint32_t submittedBytes = 0U;
};

int gFailures = 0;
int gMagicCalls = 0;
uint32_t gProfilingCalls = 0U;
uint32_t gCombineProfilingCalls = 0U;
bool gProfilingExhausted = false;
constexpr int64_t kNextMagic = 0x123456781234LL;
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
int gTilingAllocations = 0;
int gTilingCopies = 0;
int gTilingFrees = 0;
aclError gTilingMallocResult = ACL_SUCCESS;
aclError gTilingCopyResult = ACL_SUCCESS;
aclError gTilingFreeResult = ACL_SUCCESS;
std::map<void *, size_t> gDeviceTilings;
std::map<TileXRCommPtr, std::unique_ptr<TileXR::KernelTilingCache>> gTilingCaches;


void Check(bool condition, const char *message)
{
    if (!condition) {
        std::cerr << "FAILED: " << message << std::endl;
        ++gFailures;
    }
}

void ResetCapture()
{
    gTilingFreeResult = ACL_SUCCESS;
    gTilingCaches.clear();
    Check(gDeviceTilings.empty(), "communicator cache teardown releases every GM allocation");
    gTilingAllocations = gTilingCopies = gTilingFrees = 0;
    gTilingMallocResult = gTilingCopyResult = ACL_SUCCESS;
    gMagicCalls = 0;
    gProfilingCalls = 0U;
    gCombineProfilingCalls = 0U;
    gProfilingExhausted = false;
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

void TestKernelTilingCache()
{
    ResetCapture();
    uint8_t first[32] = {1U};
    uint8_t second[32] = {2U};
    GM_ADDR firstDevice = nullptr;
    GM_ADDR secondDevice = nullptr;
    {
        TileXR::KernelTilingCache cache;
        Check(cache.Get(nullptr, 32, &firstDevice) == TileXR::TILEXR_ERROR_PARA_CHECK_FAIL &&
            cache.Get(first, 0, &firstDevice) == TileXR::TILEXR_ERROR_PARA_CHECK_FAIL &&
            cache.Get(first, 32, nullptr) == TileXR::TILEXR_ERROR_PARA_CHECK_FAIL,
            "invalid cache requests do not allocate");
        gTilingMallocResult = 1;
        Check(cache.Get(first, 32, &firstDevice) == TileXR::TILEXR_ERROR_INTERNAL && firstDevice == nullptr,
            "allocation failure publishes no device address");
        gTilingMallocResult = ACL_SUCCESS;
        gTilingCopyResult = 1;
        Check(cache.Get(first, 32, &firstDevice) == TileXR::TILEXR_ERROR_INTERNAL && firstDevice == nullptr,
            "copy failure publishes no device address");
        Check(gDeviceTilings.size() == 1U, "failed initialization remains owned for retry and cleanup");
        gTilingCopyResult = ACL_SUCCESS;
        Check(cache.Get(first, 32, &firstDevice) == TileXR::TILEXR_SUCCESS && firstDevice[0] == 1U,
            "failed initialization can retry without using partial data");
        const int allocations = gTilingAllocations;
        const int copies = gTilingCopies;
        GM_ADDR reused = nullptr;
        Check(cache.Get(first, 32, &reused) == TileXR::TILEXR_SUCCESS && reused == firstDevice &&
            gTilingAllocations == allocations && gTilingCopies == copies,
            "cache hit performs no allocation or H2D copy");
        Check(cache.Get(second, 32, &secondDevice) == TileXR::TILEXR_SUCCESS &&
            secondDevice != firstDevice && firstDevice[0] == 1U && secondDevice[0] == 2U,
            "changed configuration preserves previously submitted GM contents");
        first[0] = 7U;
        Check(firstDevice[0] == 1U, "Host source lifetime is independent after synchronous initialization");
        {
            TileXR::KernelTilingCache otherCommunicator;
            GM_ADDR otherDevice = nullptr;
            Check(otherCommunicator.Get(second, 32, &otherDevice) == TileXR::TILEXR_SUCCESS &&
                otherDevice != secondDevice, "separate communicators own separate device allocations");
        }
        Check(gDeviceTilings.size() == 2U, "destroying another cache preserves this communicator's data");
        gTilingFreeResult = 1;
        Check(cache.Clear() == TileXR::TILEXR_ERROR_INTERNAL && gDeviceTilings.size() == 2U,
            "failed teardown retains ownership for retry");
        gTilingFreeResult = ACL_SUCCESS;
        Check(cache.Clear() == TileXR::TILEXR_SUCCESS && gDeviceTilings.empty(),
            "teardown retry releases all cached configurations");
        gTilingCopyResult = 1;
        Check(cache.Get(first, 32, &firstDevice) == TileXR::TILEXR_ERROR_INTERNAL,
            "leave an unpublished failed copy for destructor cleanup");
        gTilingCopyResult = ACL_SUCCESS;
    }
    Check(gDeviceTilings.empty(), "destructor releases unpublished allocations too");
}

void TestCombineServerDedupLaunch()
{
    ResetCapture();
    auto params = CombineTest::ValidParams();
    TileXR::CommArgs comm {};
    CombineTest::InitComm(comm);
    const auto context = MakeContext(&comm);
    TileXREp::EpMemoryServerDedupCombineLayout layout {};
    auto launch = [&]() { return TileXREp::TileXREpLaunchCombineMemoryServerDedupKernel(params, context, layout); };
    gSocName = "Ascend910B3";
    Check(launch() == TileXR::TILEXR_ERROR_NOT_SUPPORT && gMagicCalls == 0 && gLoadedFiles.empty(),
        "Combine refuses unsupported SoC without side effects");
    gSocName = "Ascend950PR_9589";
    gDeviceResult = 1;
    Check(launch() == TileXR::TILEXR_ERROR_INTERNAL, "Combine device query failure");
    gDeviceResult = ACL_SUCCESS;
    for (int64_t cores : {int64_t(0), int64_t(UINT32_MAX) + 1}) {
        gVectorCores = cores;
        Check(launch() == TileXR::TILEXR_ERROR_INTERNAL, "Combine rejects invalid vector count");
    }
    gVectorCores = 63;
    Check(launch() == TileXR::TILEXR_ERROR_NOT_SUPPORT, "Combine needs 64 AIVs");
    gVectorCores = 64;
    gLoadResult = false;
    Check(launch() == TileXR::TILEXR_ERROR_NOT_INITIALIZED, "missing Combine binary is an error");
    gLoadResult = true;
    gBinaryRegisterResult = -1;
    Check(launch() == TileXR::TILEXR_ERROR_MKIRT, "Combine binary registration error");
    gBinaryRegisterResult = RT_ERROR_NONE;
    gFunctionRegisterResult = -1;
    Check(launch() == TileXR::TILEXR_ERROR_MKIRT && gBinaryUnregisterCalls == 1,
        "Combine failed function registration rolls back");
    Check(gMagicCalls == 0 && gTilingAllocations == 0 && gLaunches.empty(), "registration failures cannot consume magic");
    ResetCapture(); gVectorCores = 64;
    gTilingMallocResult = 1;
    Check(launch() == TileXR::TILEXR_ERROR_INTERNAL && gMagicCalls == 0, "tiling allocation fails before magic");
    Check(gLoadedFiles == std::vector<std::string>{"tilexr_ep_combine_memory_server_dedup_kernel.o"} &&
        gRegisteredEntries == std::vector<std::string>{"tilexr_ep_combine_memory_server_dedup_kernel"},
        "Combine registers its own artifact and signature independently from Dispatch");
    gTilingMallocResult = ACL_SUCCESS; gTilingCopyResult = 1;
    Check(launch() == TileXR::TILEXR_ERROR_INTERNAL && gMagicCalls == 0 && gLaunches.empty(),
        "tiling initialization fails before magic or submission");
    gTilingCopyResult = ACL_SUCCESS; gMagicResult = TileXR::TILEXR_ERROR_NOT_INITIALIZED;
    Check(launch() == TileXR::TILEXR_ERROR_NOT_INITIALIZED && gLaunches.empty(), "Combine magic failure blocks launch");
    ResetCapture(); gVectorCores = 96;
    Check(launch() == TileXR::TILEXR_SUCCESS && gLaunches.size() == 1, "Combine submits on larger A5 device");
    if (gLaunches.empty()) return;
    const std::vector<uint64_t> expected {reinterpret_cast<uint64_t>(context.devArgs), 0x100000, 0x200004,
        0x300004, 0x400004, 0x600004, 0x700004, 0x800004, 0x500002,
        gLaunches[0].args[9], static_cast<uint64_t>(kNextMagic)};
    std::vector<uint8_t> tiling(16, 0);
    tiling[0] = 4; tiling[4] = 13; tiling[8] = 192; tiling[10] = 6;
    tiling[11] = static_cast<uint8_t>(TileXR::TILEXR_DATA_TYPE_BFP16);
#if defined(TILEXR_EP_SERVER_DEDUP_ENABLE_PROFILING) && TILEXR_EP_SERVER_DEDUP_ENABLE_PROFILING
    tiling[13] = 1;
    Check(gCombineProfilingCalls == 1, "Combine reserves its independent latest region");
#endif
    Check(gLaunches[0].name == "tilexr_ep_combine_memory_server_dedup_kernel" &&
        gLaunches[0].blockDim == 64 && gLaunches[0].stream == params.combine.stream &&
        gLaunches[0].submittedBytes == 88 && gLaunches[0].args == expected && gLaunches[0].tiling == tiling,
        "Combine 88B ABI, full prefix pointer, 16B tiling and all reserved bytes are correct");
    Check(gMagicCalls == 1 && gBinaryRegisterCalls == 0 && gTilingAllocations == 1 && gTilingCopies == 1,
        "one magic, immutable tiling and cached independent registration");
    const auto firstAddress = gLaunches[0].args[9];
    for (int i = 0; i < 3; ++i) Check(launch() == TileXR::TILEXR_SUCCESS, "same-shape repeat launch");
    Check(gLaunches.back().args[9] == firstAddress && gTilingAllocations == 1 && gMagicCalls == 4,
        "same-shape Combine reuses immutable tiling and consumes one magic per submission");
    params.combine.dtype = TileXR::TILEXR_DATA_TYPE_FP16;
    params.combine.bs = 65536; params.combine.h = 131072; params.combine.topK = 32; params.combine.moeExpertNum = 1024;
    Check(launch() == TileXR::TILEXR_SUCCESS, "wide shapes and FP16 are not narrowed prematurely");
    std::vector<uint8_t> wide(16, 0);
    wide[2] = 1; wide[6] = 2; wide[9] = 4; wide[10] = 32; wide[11] = static_cast<uint8_t>(TileXR::TILEXR_DATA_TYPE_FP16);
    wide[13] = tiling[13];
    Check(gLaunches.back().tiling == wide && gLaunches.back().args[9] != firstAddress &&
        std::memcmp(reinterpret_cast<void *>(firstAddress), tiling.data(), tiling.size()) == 0,
        "new shape leaves earlier asynchronous Combine tiling unchanged");
    TileXREp::EpServerDedupTilingData dispatchTiling {};
    GM_ADDR dispatchAddress = nullptr;
    Check(TileXRCommGetKernelTiling(params.combine.comm, &dispatchTiling, sizeof(dispatchTiling), &dispatchAddress) ==
        TileXR::TILEXR_SUCCESS && reinterpret_cast<uint64_t>(dispatchAddress) != gLaunches.back().args[9],
        "same communicator separates Dispatch and Combine tiling snapshots");
    gLaunchResult = -1;
    const int magicCalls = gMagicCalls;
    Check(launch() == TileXR::TILEXR_ERROR_MKIRT && gMagicCalls == magicCalls + 1,
        "runtime submit failure is propagated after exactly one magic allocation");
    Check(gProfilingCalls == 0, "Combine never touches Dispatch profiling reservation");
#if defined(TILEXR_EP_SERVER_DEDUP_ENABLE_PROFILING) && TILEXR_EP_SERVER_DEDUP_ENABLE_PROFILING
    gLaunchResult = RT_ERROR_NONE;
    gProfilingExhausted = true;
    Check(launch() == TileXR::TILEXR_SUCCESS && gLaunches.back().tiling[13] == 0,
        "unavailable Combine capture disables writes without replacing earlier immutable tiling");
#else
    Check(gCombineProfilingCalls == 0, "profiling OFF never reserves Combine records");
#endif
}

void TestServerDedupCompileTimeLaunch()
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
    params.tokenTypeOut = reinterpret_cast<uint32_t *>(0x9000);
    params.destinationIndexOut = reinterpret_cast<uint32_t *>(0xe000);
    params.sourceMaskOut = reinterpret_cast<uint32_t *>(0xa000);
    params.relayReadIndexOut = reinterpret_cast<uint32_t *>(0xb000);
    params.expertScalesOut = reinterpret_cast<float *>(0xc000);
    p.dynamicScalesOut = reinterpret_cast<void *>(0xd000);
    p.quantMode = 4;
    p.expandXOutDtype = TileXR::TILEXR_DATA_TYPE_FP8E5M2;
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
    gTilingMallocResult = 1;
    Check(launch() == TileXR::TILEXR_ERROR_INTERNAL && gLaunches.empty(),
        "GM allocation failure prevents kernel submission");
    gTilingMallocResult = ACL_SUCCESS;
    gTilingCopyResult = 1;
    Check(launch() == TileXR::TILEXR_ERROR_INTERNAL && gLaunches.empty(),
        "GM initialization failure prevents kernel submission");
    gTilingCopyResult = ACL_SUCCESS;
    Check(launch() == TileXR::TILEXR_SUCCESS && gLaunches.size() == 1U,
        "launch can retry after GM initialization failure");

    ResetCapture();
    gVectorCores = 64;
    Check(launch() == TileXR::TILEXR_SUCCESS, "count server dedup launch succeeds");
    CheckLaunch(0, "tilexr_ep_dispatch_memory_server_dedup_kernel", 15);
#if defined(TILEXR_EP_SERVER_DEDUP_ENABLE_PROFILING)
    constexpr uint64_t firstProfilingSlot = 0U;
#else
    constexpr uint64_t firstProfilingSlot = UINT32_MAX;
#endif
    // Independent wire expectations: five-table addresses in contract order, real GM
    // tiling pointer, then magic. Read tiling separately from its device allocation.
    const std::vector<uint64_t> expected = {reinterpret_cast<uint64_t>(context.devArgs),
        0x1000, 0x2000, 0x3000, 0x5000, 0xd000, 0x6000, 0x7000, 0x9000, 0xe000, 0xb000, 0xa000, 0xc000,
        gLaunches[0].args[13], kNextMagic};
    std::vector<uint8_t> expectedTiling = {
        0x00, 0x01, 0x00, 0x00, 0x00, 0x1c, 0x00, 0x00, // bs=256, h=7168
        0, 0, 0, 0, // profiling slot
        0x80, 0x01, 6, 0, // experts=384, topK=6, reserved
        1, 4, 11, 16, 0, // count type, quant mode, BF16, E5M2, reserved
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    for (uint32_t byte = 0; byte < 4U; ++byte)
        expectedTiling[8U + byte] = static_cast<uint8_t>(firstProfilingSlot >> (8U * byte));
    Check(gLaunches[0].args == expected && gLaunches[0].tiling == expectedTiling,
        "120B kernel ABI and 32B GM tiling preserve all field values and reserved zeros");
    Check(gLaunches[0].submittedBytes == 120U, "runtime receives no inline tiling contents or padding");
    Check(gTilingAllocations == 1 && gTilingCopies == 1, "first configuration is initialized once before launch");
    Check(gLaunches[0].blockDim == 64 && gLaunches[0].stream == p.stream, "uses fixed 64 AIVs and caller stream");
    Check(gBinaryRegisterCalls == 0 && gMagicCalls == 1, "registered binary reused with exactly one magic per launch");

    ResetCapture();
    gVectorCores = 64;
    const int rounds = static_cast<int>(TileXR::PING_PONG_SIZE) + 3;
    for (int round = 0; round < rounds; ++round) {
        Check(launch() == TileXR::TILEXR_SUCCESS, "reused server dedup launch succeeds");
        auto roundArgs = expected;
        roundArgs[13] = gLaunches.back().args[13];
        roundArgs[14] = kNextMagic + round;
        auto roundTiling = expectedTiling;
        Check(gLaunches.size() == static_cast<std::size_t>(round + 1) &&
            gLaunches.back().args == roundArgs && gLaunches.back().tiling == roundTiling &&
            gLaunches.back().stream == p.stream,
            "each round forwards new magic and reuses all pointers and the stream");
    }
    Check(gLaunches.front().tiling == expectedTiling &&
        std::memcmp(reinterpret_cast<const void *>(gLaunches.front().args[13]), expectedTiling.data(), 32U) == 0,
        "later asynchronous submissions cannot overwrite the first GM tiling");
    Check(std::all_of(gLaunches.begin(), gLaunches.end(), [&](const LaunchCapture &capture) {
            return capture.args[13] == gLaunches[0].args[13];
        }) && gTilingAllocations == 1 && gTilingCopies == 1,
        "one compile-time mode reuses one immutable GM tiling entry with no per-round copy");
    Check(gMagicCalls == rounds && gBinaryRegisterCalls == 0 && gLoadedFiles.empty(),
        "multiple rounds consume exactly one magic each without reloading the artifact");

    // Maximum expert/topK encodings and an h that cannot fit in uint16_t.
    const auto originalParams = params;
    ResetCapture();
    gVectorCores = 64;
    p.bs = 1;
    p.h = 65537;
    p.moeExpertNum = 1024;
    p.topK = 32;
    p.expandXOutDtype = TileXR::TILEXR_DATA_TYPE_FP8E4M3;
    Check(launch() == TileXR::TILEXR_SUCCESS, "wide shape and compact enum tiling launch succeeds");
    auto wideTiling = expectedTiling;
    wideTiling[0] = 1; wideTiling[1] = 0;
    wideTiling[4] = 1; wideTiling[5] = 0; wideTiling[6] = 1;
    wideTiling[12] = 0; wideTiling[13] = 4; wideTiling[14] = 32; wideTiling[19] = 15;
    Check(gLaunches[0].tiling == wideTiling && gLaunches[0].args[14] == kNextMagic,
        "uint32 hidden, uint16 experts, uint8 topK/dtype and 64-bit magic survive packing");
    params = originalParams;

    ResetCapture();
    gVectorCores = 96;
    p.dtype = TileXR::TILEXR_DATA_TYPE_FP16;
    p.quantMode = 0;
    p.dynamicScalesOut = nullptr;
    p.expandXOutDtype = TileXR::TILEXR_DATA_TYPE_RESERVED;
    params.tokenTypeOut = reinterpret_cast<uint32_t *>(0xf000);
    Check(launch() == TileXR::TILEXR_SUCCESS && gLaunches[0].blockDim == 64 &&
        gLaunches[0].args[8] == 0xf000 && gLaunches[0].tiling[19] == static_cast<uint8_t>(p.dtype) &&
        gLaunches[0].tiling[17] == 0U && gLaunches[0].tiling[18] == 3U,
        "larger devices retain the selected split and explicit FP16 tiling");
    ResetCapture();
    gVectorCores = 64;
    gLaunchResult = -1;
    Check(launch() == TileXR::TILEXR_ERROR_MKIRT, "runtime submission failure is never PASS");
    Check(gMagicCalls == 1 && gLaunches.size() == 1, "failed submission consumes only its own magic");
    gLaunchResult = RT_ERROR_NONE;
    Check(launch() == TileXR::TILEXR_SUCCESS && gMagicCalls == 2 &&
        gLaunches.back().args[14] == static_cast<uint64_t>(kNextMagic + 1),
        "a later invocation cannot reuse a failed submission's magic");
#if defined(TILEXR_EP_SERVER_DEDUP_ENABLE_PROFILING)
    Check(gProfilingCalls == 2U && gLaunches.back().tiling[8] == 0U,
        "latest capture reuses its single slot independently of submission magic");
    gProfilingExhausted = true;
    Check(launch() == TileXR::TILEXR_SUCCESS && std::all_of(gLaunches.back().tiling.begin() + 8, gLaunches.back().tiling.begin() + 12,
            [](uint8_t byte) { return byte == 0xffU; }),
        "unavailable latest capture does not prevent communication launch");
#endif
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
        name != "tilexr_ep_combine_memory_server_dedup_kernel.o" &&
        name != "tilexr_ep_dispatch_memory_server_dedup_kernel.o") {
        return false;
    }
    gLoadedFiles.push_back(name);
    binary->data = kernelBinary;
    binary->size = sizeof(kernelBinary);
    return true;
}

} // namespace TileXR

int TileXRCommGetKernelTiling(TileXRCommPtr comm, const void *data, uint32_t bytes, GM_ADDR *device)
{
    if (comm == nullptr) return TileXR::TILEXR_ERROR_PARA_CHECK_FAIL;
    auto &cache = gTilingCaches[comm];
    if (!cache) cache.reset(new TileXR::KernelTilingCache());
    return cache->Get(data, bytes, device);
}

extern "C" {

int TileXRProfilingReserveLatestCombineLaunch(TileXRCommPtr, uint32_t recordsPerLaunch, uint32_t *slot)
{
    Check(recordsPerLaunch == 8U, "Combine has an independent eight-record latest budget");
    *slot = gProfilingExhausted ? UINT32_MAX : 0U;
    ++gCombineProfilingCalls;
    return TileXR::TILEXR_SUCCESS;
}

int TileXRProfilingReserveLatestLaunch(TileXRCommPtr, uint32_t recordsPerLaunch, uint32_t *slot)
{
    Check(recordsPerLaunch == 65536U, "Host and Kernel use the same per-launch capture budget");
    *slot = gProfilingExhausted ? UINT32_MAX : 0U;
    ++gProfilingCalls;
    return TileXR::TILEXR_SUCCESS;
}

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

aclError aclrtMalloc(void **device, size_t bytes, aclrtMemMallocPolicy policy)
{
    ++gTilingAllocations;
    Check(policy == ACL_MEM_MALLOC_NORMAL_ONLY, "small tiling uses ordinary device allocation");
    if (gTilingMallocResult != ACL_SUCCESS) return gTilingMallocResult;
    *device = std::malloc(bytes);
    if (*device == nullptr) return 1;
    std::memset(*device, 0xa5, bytes);
    gDeviceTilings[*device] = bytes;
    return ACL_SUCCESS;
}

aclError aclrtFree(void *device)
{
    ++gTilingFrees;
    if (gTilingFreeResult != ACL_SUCCESS) return gTilingFreeResult;
    Check(gDeviceTilings.erase(device) == 1U, "free matches a live tiling allocation");
    std::free(device);
    return ACL_SUCCESS;
}

aclError aclrtMemcpy(void *dst, std::size_t destMax, const void *src,
    std::size_t count, aclrtMemcpyKind kind)
{
    if (kind == ACL_MEMCPY_HOST_TO_DEVICE) {
        ++gTilingCopies;
        if (gTilingCopyResult != ACL_SUCCESS) return gTilingCopyResult;
        Check(gDeviceTilings.count(dst) == 1U && gDeviceTilings[dst] >= count && count <= destMax,
            "tiling initialization writes within its allocation");
        std::memcpy(dst, src, count);
    } else if (dst != nullptr && count <= destMax) {
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
        capture.submittedBytes = argsInfo->argsSize;
        std::size_t fieldCount = argsInfo->argsSize / sizeof(uint64_t);
        Check(argsInfo->hasTiling == 0U && argsInfo->hostInputInfoNum == 0U &&
            argsInfo->tilingAddrOffset == 0U && argsInfo->tilingDataOffset == 0U,
            "launch never requests inline tiling relocation");
        if (capture.name == "tilexr_ep_dispatch_memory_server_dedup_kernel") {
            Check(argsInfo->argsSize == 120U, "only the 120B kernel arguments are submitted");
            if (argsInfo->argsSize != 120U) return -1;
            const auto *fields = static_cast<const uint64_t *>(argsInfo->args);
            auto *deviceTiling = reinterpret_cast<uint8_t *>(fields[13]);
            Check(gDeviceTilings.count(deviceTiling) == 1U && gDeviceTilings[deviceTiling] == 32U,
                "tiling pointer resolves to a separately allocated 32B GM buffer");
            if (gDeviceTilings.count(deviceTiling) == 0U) return -1;
            capture.tiling.assign(deviceTiling, deviceTiling + 32U);
        }
        if (capture.name == "tilexr_ep_combine_memory_server_dedup_kernel") {
            Check(argsInfo->argsSize == 88U, "only 88B Combine arguments are submitted");
            if (argsInfo->argsSize != 88U) return -1;
            const auto *fields = static_cast<const uint64_t *>(argsInfo->args);
            auto *deviceTiling = reinterpret_cast<uint8_t *>(fields[9]);
            Check(gDeviceTilings.count(deviceTiling) == 1U && gDeviceTilings[deviceTiling] == 16U,
                "Combine has a separate 16B GM tiling allocation");
            if (gDeviceTilings.count(deviceTiling) == 0U) return -1;
            capture.tiling.assign(deviceTiling, deviceTiling + 16U);
        }
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
    TestKernelTilingCache();
    TestServerDedupCompileTimeLaunch();
    TestCombineServerDedupLaunch();
    ResetCapture();
#if defined(TILEXR_EP_TEST_COMBINE_C2)
    std::cout << "EP_SERVER_DEDUP_COMBINE_EMPTY_KERNEL_" << (gFailures == 0 ? "PASS" : "FAIL")
#else
    std::cout << "EP_SERVER_DEDUP_EMPTY_KERNEL_" << (gFailures == 0 ? "PASS" : "FAIL")
#endif
              << " test=launch" << std::endl;
    return gFailures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
