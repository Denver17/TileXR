#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>

#include "acl/acl_rt.h"
#include "dispatch_host.h"
#include "dispatch_launch.h"
#include "tilexr_types.h"
#include "tilexr_udma_fullmesh.h"
#include "tilexr_udma_reg.h"

namespace {

int failures = 0;
int hostArgsCalls = 0;
int devArgsCalls = 0;
int launchCalls = 0;
int memsetCalls = 0;
int synchronizeCalls = 0;
int launchReturn = TileXR::TILEXR_SUCCESS;
int fullmeshReturn = TileXR::TILEXR_SUCCESS;
aclError memsetReturn = ACL_SUCCESS;
aclError synchronizeReturn = ACL_SUCCESS;
uint32_t qpCount = 2U;
TileXR::CommArgs commArgs {};
TileXR::TileXRUDMARegistry registry {};
TileXR::TileXRUDMAFullmeshHostView fullmeshView {};
GM_ADDR devArgs = reinterpret_cast<GM_ADDR>(uintptr_t {0x9000});
TileXRMoonEp::DispatchUrmaLaunchParams launchedParams {};

void SetPeerMode(const char *value)
{
#if defined(_WIN32)
    _putenv_s("TILEXR_MOONEP_DISPATCH_PEER_MODE",
        value == nullptr ? "" : value);
#else
    if (value == nullptr) {
        unsetenv("TILEXR_MOONEP_DISPATCH_PEER_MODE");
    } else {
        setenv("TILEXR_MOONEP_DISPATCH_PEER_MODE", value, 1);
    }
#endif
}

void Check(bool condition, const std::string &message)
{
    if (!condition) {
        std::cerr << message << '\n';
        ++failures;
    }
}

void CheckStatus(const char *label, int actual, int expected)
{
    if (actual != expected) {
        std::cerr << label << ": expected " << expected << ", got " << actual << '\n';
        ++failures;
    }
}

void Reset()
{
    hostArgsCalls = devArgsCalls = launchCalls = 0;
    memsetCalls = synchronizeCalls = 0;
    launchReturn = TileXR::TILEXR_SUCCESS;
    fullmeshReturn = TileXR::TILEXR_SUCCESS;
    memsetReturn = synchronizeReturn = ACL_SUCCESS;
    qpCount = 2U;
    commArgs = TileXR::CommArgs {};
    commArgs.rank = 0;
    commArgs.localRank = 0;
    commArgs.rankSize = 1;
    commArgs.localRankSize = 1;
    registry = TileXR::TileXRUDMARegistry {};
    fullmeshView = TileXR::TileXRUDMAFullmeshHostView {};
    SetPeerMode(nullptr);
    devArgs = reinterpret_cast<GM_ADDR>(uintptr_t {0x9000});
    launchedParams = TileXRMoonEp::DispatchUrmaLaunchParams {};
}

void ConfigureGroupedTransport(void *workspace, uint64_t workspaceBytes)
{
    commArgs.rank = 0;
    commArgs.rankSize = 2;
    commArgs.localRank = 0;
    commArgs.localRankSize = 2;
    commArgs.extraFlag = TileXR::ExtraFlag::UDMA |
        TileXR::ExtraFlag::UDMA_FULLMESH;
    commArgs.udmaInfoPtr = reinterpret_cast<GM_ADDR>(uintptr_t {0xA000});
    commArgs.udmaRegistryPtr = reinterpret_cast<GM_ADDR>(uintptr_t {0xB000});
    commArgs.udmaFullmeshPtr = reinterpret_cast<GM_ADDR>(uintptr_t {0xC000});
    commArgs.udmaRegistrationGeneration = 7U;

    registry.rankSize = 2U;
    registry.regionCount = 1U;
    for (uint32_t rank = 0U; rank < registry.rankSize; ++rank) {
        registry.regions[rank].base = static_cast<GM_ADDR>(workspace);
        registry.regions[rank].bytes = workspaceBytes;
    }

    fullmeshView.slotCount = TileXR::TILEXR_UDMA_FULLMESH_SLOT_COUNT;
    fullmeshView.connectedCount = 1U;
    fullmeshView.localRank = 0U;
    fullmeshView.validPeerMask = TileXR::UDMAFullmeshExpectedPeerMask(0U, 2U);
    fullmeshView.registrationReady = 1U;
    fullmeshView.registrationGeneration =
        commArgs.udmaRegistrationGeneration;
    fullmeshView.infoDev = reinterpret_cast<GM_ADDR>(uintptr_t {0xD000});
    fullmeshView.viewDev = commArgs.udmaFullmeshPtr;
}

TileXRMoonEpPlanV1 ValidPlan()
{
    TileXRMoonEpPlanV1 plan {};
    plan.structSize = sizeof(plan);
    plan.abiVersion = TILEXR_MOONEP_ABI_VERSION_V1;
    plan.n = 128;
    plan.r = 1;
    plan.e = 8;
    plan.b = 2;
    plan.nvS = 128;
    plan.k = 2;
    plan.dst = reinterpret_cast<void *>(uintptr_t {0x3000});
    plan.expertsToCopy = reinterpret_cast<void *>(uintptr_t {0x3100});
    plan.zeroFillRanges = reinterpret_cast<void *>(uintptr_t {0x3200});
    plan.remoteStats = reinterpret_cast<void *>(uintptr_t {0x3300});
    plan.dupGroups = reinterpret_cast<void *>(uintptr_t {0x3400});
    plan.dupLoffs = reinterpret_cast<void *>(uintptr_t {0x3500});
    plan.dupCounts = reinterpret_cast<void *>(uintptr_t {0x3600});
    plan.status = reinterpret_cast<void *>(uintptr_t {0x3700});
    return plan;
}

TileXRMoonEpTensorV1 Tensor(void *data, uint32_t dtype, uint32_t rank,
    int64_t dim0, int64_t dim1, uint64_t elements)
{
    TileXRMoonEpTensorV1 tensor {};
    tensor.structSize = sizeof(tensor);
    tensor.abiVersion = TILEXR_MOONEP_ABI_VERSION_V1;
    tensor.data = data;
    tensor.elementCount = elements;
    tensor.dtype = dtype;
    tensor.rank = rank;
    tensor.shape[0] = dim0;
    tensor.shape[1] = dim1;
    return tensor;
}

TileXRMoonEpDispatchArgsV2 Args(const TileXRMoonEpPlanV1 *plan,
    const TileXRMoonEpTensorV1 *hiddenInput,
    const TileXRMoonEpTensorV1 *weightInput,
    TileXRMoonEpTensorV1 *hiddenOutput,
    TileXRMoonEpTensorV1 *weightOutput)
{
    TileXRMoonEpDispatchArgsV2 args {};
    args.structSize = sizeof(args);
    args.abiVersion = TILEXR_MOONEP_ABI_VERSION_V2;
    args.comm = reinterpret_cast<TileXRCommPtr>(uintptr_t {0x1000});
    args.plan = plan;
    args.hiddenSh = hiddenInput;
    args.routeWeightsSk = weightInput;
    args.hiddenNvsh = hiddenOutput;
    args.routeWeightsNvs = weightOutput;
    args.flags = TILEXR_MOONEP_FLAG_RESET_STATUS;
    args.registeredWorkspace = reinterpret_cast<void *>(uintptr_t {0x200000});
    args.registeredWorkspaceBytes = UINT64_C(2) * 1024U * 1024U;
    return args;
}

void TestPairedSingleLaunch()
{
    Reset();
    TileXRMoonEpPlanV1 plan = ValidPlan();
    TileXRMoonEpTensorV1 hiddenInput = Tensor(
        reinterpret_cast<void *>(uintptr_t {0x400000}),
        TILEXR_MOONEP_DTYPE_BFLOAT16, 2, 64, 17, 1088);
    TileXRMoonEpTensorV1 hiddenOutput = Tensor(
        reinterpret_cast<void *>(uintptr_t {0x500000}),
        TILEXR_MOONEP_DTYPE_BFLOAT16, 2, 128, 17, 2176);
    TileXRMoonEpTensorV1 weightInput = Tensor(
        reinterpret_cast<void *>(uintptr_t {0x600000}),
        TILEXR_MOONEP_DTYPE_FLOAT32, 2, 64, 2, 128);
    TileXRMoonEpTensorV1 weightOutput = Tensor(
        reinterpret_cast<void *>(uintptr_t {0x700000}),
        TILEXR_MOONEP_DTYPE_FLOAT32, 1, 128, 0, 128);
    TileXRMoonEpDispatchArgsV2 args = Args(&plan, &hiddenInput, &weightInput,
        &hiddenOutput, &weightOutput);
    aclrtStream stream = reinterpret_cast<aclrtStream>(uintptr_t {0x8000});

    CheckStatus("paired", TileXRMoonEp::TileXRMoonEpRunDispatchUrmaV2(
        &args, stream), TILEXR_MOONEP_SUCCESS);
    Check(hostArgsCalls == 1 && devArgsCalls == 1 && launchCalls == 1,
        "paired Dispatch must make exactly one internal launch");
    Check(memsetCalls == 1 && synchronizeCalls == 0,
        "paired success must enqueue one reset without synchronizing");
    Check(launchedParams.peerMode == TileXRMoonEp::DispatchPeerMode::GroupCredit,
        "unset peer mode must default to group_credit");
    Check(launchedParams.hiddenInput == hiddenInput.data &&
        launchedParams.hiddenOutput == hiddenOutput.data &&
        launchedParams.weightInput == weightInput.data &&
        launchedParams.weightOutput == weightOutput.data,
        "paired launch did not carry all four payload pointers");
    Check(launchedParams.layout.weight.sourceOffset >=
        launchedParams.layout.hidden.scratchOffset +
            launchedParams.layout.hidden.scratchBytes,
        "paired launch layout overlaps Hidden and Weight active regions");
}

void TestHiddenOnlyAndFailureBoundaries()
{
    Reset();
    TileXRMoonEpPlanV1 plan = ValidPlan();
    TileXRMoonEpTensorV1 hiddenInput = Tensor(
        reinterpret_cast<void *>(uintptr_t {0x400000}),
        TILEXR_MOONEP_DTYPE_FLOAT16, 2, 64, 16, 1024);
    TileXRMoonEpTensorV1 hiddenOutput = Tensor(
        reinterpret_cast<void *>(uintptr_t {0x500000}),
        TILEXR_MOONEP_DTYPE_FLOAT16, 2, 128, 16, 2048);
    TileXRMoonEpDispatchArgsV2 args = Args(
        &plan, &hiddenInput, nullptr, &hiddenOutput, nullptr);
    aclrtStream stream = reinterpret_cast<aclrtStream>(uintptr_t {0x8000});

    CheckStatus("hidden", TileXRMoonEp::TileXRMoonEpRunDispatchUrmaV2(
        &args, stream), TILEXR_MOONEP_SUCCESS);
    Check(launchCalls == 1 && launchedParams.weightInput == nullptr &&
        launchedParams.weightOutput == nullptr,
        "hidden-only Dispatch must launch once without Weight pointers");

    Reset();
    launchReturn = TileXR::TILEXR_ERROR_MKIRT;
    CheckStatus("launch failure", TileXRMoonEp::TileXRMoonEpRunDispatchUrmaV2(
        &args, stream), TILEXR_MOONEP_ERROR_INTERNAL);
    Check(launchCalls == 1 && synchronizeCalls == 1,
        "same-stream reset must be synchronized after launch failure");

    Reset();
    memsetReturn = 1;
    CheckStatus("reset failure", TileXRMoonEp::TileXRMoonEpRunDispatchUrmaV2(
        &args, stream), TILEXR_MOONEP_ERROR_INTERNAL);
    Check(launchCalls == 0, "reset failure must prevent launch");
}

void TestGroupedFullmeshCapability()
{
    Reset();
    TileXRMoonEpPlanV1 plan = ValidPlan();
    plan.r = 2;
    plan.n = 128;
    plan.nvS = 128;
    TileXRMoonEpTensorV1 hiddenInput = Tensor(
        reinterpret_cast<void *>(uintptr_t {0x4000}),
        TILEXR_MOONEP_DTYPE_FLOAT16, 2, 64, 16, 1024);
    TileXRMoonEpTensorV1 hiddenOutput = Tensor(
        reinterpret_cast<void *>(uintptr_t {0x5000}),
        TILEXR_MOONEP_DTYPE_FLOAT16, 2, 128, 16, 2048);
    TileXRMoonEpDispatchArgsV2 args = Args(
        &plan, &hiddenInput, nullptr, &hiddenOutput, nullptr);
    aclrtStream stream = reinterpret_cast<aclrtStream>(uintptr_t {0x8000});
    ConfigureGroupedTransport(
        args.registeredWorkspace, args.registeredWorkspaceBytes);

    CheckStatus("grouped fullmesh", TileXRMoonEp::TileXRMoonEpRunDispatchUrmaV2(
        &args, stream), TILEXR_MOONEP_SUCCESS);
    Check(launchCalls == 1, "valid grouped Fullmesh must launch Dispatch");
    Check(launchedParams.peerMode == TileXRMoonEp::DispatchPeerMode::GroupCredit,
        "unset grouped peer mode must default to group_credit");

    Reset();
    ConfigureGroupedTransport(
        args.registeredWorkspace, args.registeredWorkspaceBytes);
    SetPeerMode("");
    CheckStatus("empty grouped peer mode",
        TileXRMoonEp::TileXRMoonEpRunDispatchUrmaV2(&args, stream),
        TILEXR_MOONEP_SUCCESS);
    Check(launchedParams.peerMode == TileXRMoonEp::DispatchPeerMode::GroupCredit,
        "empty grouped peer mode must select group_credit");

    Reset();
    ConfigureGroupedTransport(
        args.registeredWorkspace, args.registeredWorkspaceBytes);
    SetPeerMode("group");
    CheckStatus("explicit group",
        TileXRMoonEp::TileXRMoonEpRunDispatchUrmaV2(&args, stream),
        TILEXR_MOONEP_SUCCESS);
    Check(launchedParams.peerMode == TileXRMoonEp::DispatchPeerMode::Group,
        "explicit group peer mode must select group");

    Reset();
    ConfigureGroupedTransport(
        args.registeredWorkspace, args.registeredWorkspaceBytes);
    SetPeerMode("legacy");
    CheckStatus("legacy peer mode",
        TileXRMoonEp::TileXRMoonEpRunDispatchUrmaV2(&args, stream),
        TILEXR_MOONEP_ERROR_INVALID_ARGUMENT);
    Check(launchCalls == 0, "legacy peer mode must prevent launch");

    Reset();
    ConfigureGroupedTransport(
        args.registeredWorkspace, args.registeredWorkspaceBytes);
    commArgs.localRankSize = 1;
    commArgs.extraFlag &= ~TileXR::ExtraFlag::UDMA_FULLMESH;
    commArgs.udmaFullmeshPtr = nullptr;
    fullmeshReturn = TileXR::TILEXR_ERROR_NOT_SUPPORT;
    CheckStatus("single local rank without fullmesh",
        TileXRMoonEp::TileXRMoonEpRunDispatchUrmaV2(&args, stream),
        TILEXR_MOONEP_SUCCESS);
    Check(launchCalls == 1,
        "single local rank grouped Dispatch must launch without Fullmesh");

    Reset();
    ConfigureGroupedTransport(
        args.registeredWorkspace, args.registeredWorkspaceBytes);
    commArgs.extraFlag &= ~TileXR::ExtraFlag::UDMA_FULLMESH;
    CheckStatus("missing fullmesh capability",
        TileXRMoonEp::TileXRMoonEpRunDispatchUrmaV2(&args, stream),
        TILEXR_MOONEP_ERROR_NOT_SUPPORTED);
    Check(launchCalls == 0, "missing grouped Fullmesh must prevent launch");

    Reset();
    ConfigureGroupedTransport(
        args.registeredWorkspace, args.registeredWorkspaceBytes);
    fullmeshReturn = TileXR::TILEXR_ERROR_NOT_SUPPORT;
    CheckStatus("fullmesh query failure",
        TileXRMoonEp::TileXRMoonEpRunDispatchUrmaV2(&args, stream),
        TILEXR_MOONEP_ERROR_NOT_SUPPORTED);

    Reset();
    ConfigureGroupedTransport(
        args.registeredWorkspace, args.registeredWorkspaceBytes);
    ++fullmeshView.registrationGeneration;
    CheckStatus("fullmesh generation mismatch",
        TileXRMoonEp::TileXRMoonEpRunDispatchUrmaV2(&args, stream),
        TILEXR_MOONEP_ERROR_NOT_SUPPORTED);

    SetPeerMode(nullptr);
}

} // namespace

extern "C" int TileXRGetCommArgsHost(TileXRCommPtr, TileXR::CommArgs *&args)
{
    ++hostArgsCalls;
    args = &commArgs;
    return TileXR::TILEXR_SUCCESS;
}

extern "C" int TileXRGetCommArgsDev(TileXRCommPtr, GM_ADDR &args)
{
    ++devArgsCalls;
    args = devArgs;
    return TileXR::TILEXR_SUCCESS;
}

extern "C" int TileXRUDMAGetQpCount(TileXRCommPtr, uint32_t *qpCount)
{
    if (qpCount != nullptr) {
        *qpCount = ::qpCount;
    }
    return TileXR::TILEXR_SUCCESS;
}

extern "C" int TileXRUDMAFullmeshQuery(
    TileXRCommPtr, TileXR::TileXRUDMAFullmeshHostView *view)
{
    if (view != nullptr) {
        *view = fullmeshReturn == TileXR::TILEXR_SUCCESS ?
            fullmeshView : TileXR::TileXRUDMAFullmeshHostView {};
    }
    return fullmeshReturn;
}

extern "C" int TileXRGetUDMARegistryHost(
    TileXRCommPtr, const TileXR::TileXRUDMARegistry **registry)
{
    if (registry != nullptr) {
        *registry = commArgs.rankSize > 1 ? &::registry : nullptr;
    }
    return TileXR::TILEXR_SUCCESS;
}

extern "C" aclError aclrtMemsetAsync(
    void *, size_t, int32_t, size_t, aclrtStream)
{
    ++memsetCalls;
    return memsetReturn;
}

extern "C" aclError aclrtSynchronizeStream(aclrtStream)
{
    ++synchronizeCalls;
    return synchronizeReturn;
}

extern "C" aclError aclrtGetDevice(int32_t *deviceId)
{
    if (deviceId != nullptr) {
        *deviceId = 0;
    }
    return ACL_SUCCESS;
}

extern "C" aclError aclrtGetDeviceInfo(
    uint32_t, aclrtDevAttr, int64_t *value)
{
    if (value != nullptr) {
        *value = TileXRMoonEp::kDispatchAivCoreCount;
    }
    return ACL_SUCCESS;
}

namespace TileXRMoonEp {
int TileXRMoonEpLaunchDispatchUrmaKernel(const DispatchUrmaLaunchParams &params)
{
    ++launchCalls;
    launchedParams = params;
    return launchReturn;
}
} // namespace TileXRMoonEp

int main()
{
    TestPairedSingleLaunch();
    TestHiddenOnlyAndFailureBoundaries();
    TestGroupedFullmeshCapability();
    SetPeerMode(nullptr);
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
