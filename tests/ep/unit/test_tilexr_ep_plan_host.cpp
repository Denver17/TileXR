#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>

#include "acl/acl_rt.h"
#include "runtime/kernel.h"

#include "ep_plan_host.h"
#include "ep_plan_peer_mailbox.h"
#include "moonep_kernel_registration.h"
#include "planner_launch.h"
#include "tilexr_types.h"

namespace {
TileXR::CommArgs *g_hostCommArgs = nullptr;
GM_ADDR g_deviceCommArgs = nullptr;
int g_hostCommRet = TileXR::TILEXR_SUCCESS;
int g_deviceCommRet = TileXR::TILEXR_SUCCESS;
int g_nextMagicRet = TileXR::TILEXR_SUCCESS;
int64_t g_nextMagic = 0;
rtError_t g_launchRet = RT_ERROR_NONE;
int g_failures = 0;

struct PlanLaunchCapture {
    int calls = 0;
    uint32_t blockDim = 0;
    void *stream = nullptr;
    GM_ADDR commArgs = nullptr;
    GM_ADDR topkExperts = nullptr;
    GM_ADDR tokensPerExpert = nullptr;
    GM_ADDR globalRankIds = nullptr;
    GM_ADDR dst = nullptr;
    GM_ADDR cuSeqlens = nullptr;
    GM_ADDR expertsToCopy = nullptr;
    GM_ADDR remoteExperts = nullptr;
    GM_ADDR expertTargets = nullptr;
    GM_ADDR remoteStats = nullptr;
    GM_ADDR status = nullptr;
    GM_ADDR localWorkspace = nullptr;
    GM_ADDR metaWorkspace = nullptr;
    int64_t rank = -1;
    int64_t rankSize = -1;
    int64_t s = -1;
    int64_t topK = -1;
    int64_t expertNum = -1;
    uint64_t epoch = 0;
    uint64_t waitIterations = 0;
    int64_t magic = 0;
};
PlanLaunchCapture g_launchCapture {};

struct PlanKernelArgs {
    GM_ADDR commArgs;
    GM_ADDR topkExperts;
    GM_ADDR tokensPerExpert;
    GM_ADDR globalRankIds;
    GM_ADDR dst;
    GM_ADDR cuSeqlens;
    GM_ADDR expertsToCopy;
    GM_ADDR remoteExperts;
    GM_ADDR expertTargets;
    GM_ADDR remoteStats;
    GM_ADDR status;
    GM_ADDR localWorkspace;
    GM_ADDR metaWorkspace;
    int64_t rank;
    int64_t rankSize;
    int64_t s;
    int64_t topK;
    int64_t expertNum;
    int64_t prefetchSlots;
    int64_t rankTokenCapacity;
    int64_t nvS;
    int64_t tokenPadding;
    int64_t tokenRouteLimitPerPair;
    int32_t cardsPerServer;
    int32_t cardsPerCabinet;
    int32_t crossCandidateCount;
    uint64_t epoch;
    uint64_t waitIterations;
    int64_t magic;
};

void Check(bool condition, const char *message)
{
    if (!condition) {
        std::cerr << "FAIL: " << message << std::endl;
        ++g_failures;
    }
}

void CheckInt(const char *message, int actual, int expected)
{
    if (actual != expected) {
        std::cerr << "FAIL: " << message << " actual=" << actual
                  << " expected=" << expected << std::endl;
        ++g_failures;
    }
}
} // namespace

extern "C" int TileXRGetCommArgsHost(TileXRCommPtr, TileXR::CommArgs *&commArgs)
{
    commArgs = g_hostCommArgs;
    return g_hostCommRet;
}

extern "C" int TileXRGetCommArgsDev(TileXRCommPtr, GM_ADDR &commArgs)
{
    commArgs = g_deviceCommArgs;
    return g_deviceCommRet;
}

extern "C" int TileXRCommNextMagic(TileXRCommPtr, int64_t *magic)
{
    if (g_nextMagicRet == TileXR::TILEXR_SUCCESS && magic != nullptr) *magic = g_nextMagic;
    return g_nextMagicRet;
}

extern "C" aclError aclrtMemcpyAsync(void *dst, size_t destMax, const void *src,
    size_t count, aclrtMemcpyKind, aclrtStream)
{
    if (dst == nullptr || src == nullptr || count > destMax) return 1;
    std::memcpy(dst, src, count);
    return 0;
}

extern "C" aclError aclrtMemcpy(void *dst, size_t destMax, const void *src,
    size_t count, aclrtMemcpyKind)
{
    if (dst == nullptr || src == nullptr || count > destMax) return 1;
    std::memcpy(dst, src, count);
    return 0;
}

namespace TileXRMoonEp {
int EnsureMoonEpKernelRegistered(KernelRegistrationState &, const char *,
    const void *, const char *)
{
    return TileXR::TILEXR_SUCCESS;
}
} // namespace TileXRMoonEp

rtError_t rtKernelLaunchWithFlagV2(const void *func, uint32_t blockDim,
    rtArgsEx_t *argsInfo, rtSmDesc_t *, rtStream_t stream, uint32_t,
    const rtTaskCfgInfo_t *)
{
    if (func != TileXRMoonEp::KernelSignature(TileXRMoonEp::kPlannerV2KernelSignature) ||
        argsInfo == nullptr || argsInfo->args == nullptr ||
        argsInfo->argsSize != sizeof(PlanKernelArgs)) {
        return static_cast<rtError_t>(1);
    }
    const auto &args = *static_cast<const PlanKernelArgs *>(argsInfo->args);
    ++g_launchCapture.calls;
    g_launchCapture.blockDim = blockDim;
    g_launchCapture.stream = stream;
    g_launchCapture.commArgs = args.commArgs;
    g_launchCapture.topkExperts = args.topkExperts;
    g_launchCapture.tokensPerExpert = args.tokensPerExpert;
    g_launchCapture.globalRankIds = args.globalRankIds;
    g_launchCapture.dst = args.dst;
    g_launchCapture.cuSeqlens = args.cuSeqlens;
    g_launchCapture.expertsToCopy = args.expertsToCopy;
    g_launchCapture.remoteExperts = args.remoteExperts;
    g_launchCapture.expertTargets = args.expertTargets;
    g_launchCapture.remoteStats = args.remoteStats;
    g_launchCapture.status = args.status;
    g_launchCapture.localWorkspace = args.localWorkspace;
    g_launchCapture.metaWorkspace = args.metaWorkspace;
    g_launchCapture.rank = args.rank;
    g_launchCapture.rankSize = args.rankSize;
    g_launchCapture.s = args.s;
    g_launchCapture.topK = args.topK;
    g_launchCapture.expertNum = args.expertNum;
    g_launchCapture.epoch = args.epoch;
    g_launchCapture.waitIterations = args.waitIterations;
    g_launchCapture.magic = args.magic;
    return g_launchRet;
}

namespace {
TileXRMoonEPPlanConfig ValidConfig()
{
    TileXRMoonEPPlanConfig config {};
    config.prefetchSlots = 4;
    config.rankTokenCapacity = 16;
    config.nvS = 32;
    config.tokenPadding = 4;
    config.tokenRouteLimitPerPair = 8;
    config.cardsPerServer = 8;
    config.cardsPerCabinet = 64;
    config.crossCandidateCount = 3;
    return config;
}

TileXRMoonEPPlanDesc ValidPlan(const TileXRMoonEPPlanConfig &config)
{
    TileXRMoonEPPlanDesc plan {};
    plan.dst = reinterpret_cast<int32_t *>(0x1000);
    plan.cuSeqlens = reinterpret_cast<int32_t *>(0x2000);
    plan.expertsToCopy = reinterpret_cast<int32_t *>(0x3000);
    plan.remoteStats = reinterpret_cast<int32_t *>(0x4000);
    plan.dupGroups = reinterpret_cast<int32_t *>(0x5000);
    plan.dupLoffs = reinterpret_cast<int32_t *>(0x6000);
    plan.dupCounts = reinterpret_cast<int32_t *>(0x7000);
    plan.status = reinterpret_cast<int32_t *>(0x8000);
    plan.s = 8;
    plan.k = 2;
    plan.r = 8;
    plan.e = 16;
    plan.b = config.prefetchSlots;
    plan.cap = config.rankTokenCapacity;
    plan.nvS = config.nvS;
    plan.tokenPadding = config.tokenPadding;
    plan.epoch = 7;
    return plan;
}

TileXRMoonEPPlanMetadataV2 ValidMetadata(const TileXRMoonEPPlanConfig &config,
    const TileXRMoonEPPlanDesc &plan)
{
    TileXRMoonEPPlanMetadataV2 metadata {};
    metadata.structSize = sizeof(metadata);
    metadata.abiVersion = TILEXR_MOONEP_PLAN_METADATA_V2_ABI_VERSION;
    metadata.dst = plan.dst;
    metadata.dstCount = static_cast<uint64_t>(plan.s * plan.k);
    metadata.cuSeqlens = plan.cuSeqlens;
    metadata.cuSeqlensCount = static_cast<uint64_t>(plan.e + plan.b);
    metadata.remoteExperts = reinterpret_cast<int32_t *>(0x9000);
    metadata.remoteExpertsCount = static_cast<uint64_t>(plan.r * plan.b);
    metadata.expertTargets = reinterpret_cast<uint64_t *>(0xA000);
    metadata.expertTargetsCount = static_cast<uint64_t>((plan.e / plan.r) * ((plan.r + 63) / 64));
    metadata.remoteStats = plan.remoteStats;
    metadata.remoteStatsCount = 2;
    metadata.status = plan.status;
    metadata.statusCount = TileXREp::Plan::kPlanStatusWords;
    metadata.dupGroups = plan.dupGroups;
    metadata.dupGroupsCount = static_cast<uint64_t>(config.nvS * 3);
    metadata.dupLoffs = plan.dupLoffs;
    metadata.dupLoffsCount = static_cast<uint64_t>(config.nvS);
    metadata.dupCounts = plan.dupCounts;
    metadata.dupCountsCount = 2;
    metadata.s = plan.s;
    metadata.k = plan.k;
    metadata.r = plan.r;
    metadata.e = plan.e;
    metadata.b = plan.b;
    metadata.nvS = plan.nvS;
    metadata.epoch = plan.epoch;
    return metadata;
}

struct Fixture {
    TileXRMoonEPPlanConfig config = ValidConfig();
    TileXRMoonEPPlanDesc plan = ValidPlan(config);
    TileXR::CommArgs commArgs {};
    TileXREp::Plan::PlanWorkspaceLayout layout {};
    TileXREp::Plan::PlanHostArguments arguments {};
    TileXREp::Plan::PlanRuntimeMetadata runtime {};

    Fixture() { Reset(); }

    void Reset()
    {
        config = ValidConfig();
        plan = ValidPlan(config);
        commArgs = TileXR::CommArgs {};
        layout = TileXREp::Plan::PlanWorkspaceLayout {};
        arguments = TileXREp::Plan::PlanHostArguments {};
        runtime = TileXREp::Plan::PlanRuntimeMetadata {};

        CheckInt("fixture layout", TileXREp::Plan::BuildPlanWorkspaceLayout(
            plan.r, plan.s, plan.k, plan.e, config, &layout), TileXR::TILEXR_SUCCESS);

        commArgs.rank = 3;
        commArgs.rankSize = 8;
        commArgs.localRank = 3;
        commArgs.localRankSize = 8;
        commArgs.extraFlag = TileXR::ExtraFlag::TOPO_910A5;
        for (int rank = 0; rank < commArgs.rankSize; ++rank) {
            commArgs.peerMems[rank] = reinterpret_cast<GM_ADDR>(
                static_cast<uintptr_t>(0x100000 + rank * 0x10000));
        }

        arguments.topkExperts = reinterpret_cast<const int32_t *>(0x10000);
        arguments.tokensPerExpert = reinterpret_cast<const int32_t *>(0x11000);
        arguments.globalRankIds = reinterpret_cast<const int32_t *>(0x12000);
        arguments.s = plan.s;
        arguments.topK = plan.k;
        arguments.expertNum = plan.e;
        arguments.config = &config;
        arguments.plan = &plan;
        arguments.localWorkspace = reinterpret_cast<void *>(0x200000);
        arguments.localWorkspaceBytes = layout.local.totalBytes;
        arguments.registeredMetaWorkspace = reinterpret_cast<void *>(0x300000);
        arguments.registeredMetaBytes = layout.registeredMeta.totalBytes;
        arguments.waitIterations = 4096;
        arguments.stream = reinterpret_cast<aclrtStream>(0x400000);

        runtime.hostCommArgs = &commArgs;
        runtime.deviceCommArgs = reinterpret_cast<GM_ADDR>(0x500000);
    }
};

int Validate(Fixture &fixture, TileXREp::Plan::PlanHostContext *context = nullptr)
{
    TileXREp::Plan::PlanHostContext local {};
    return TileXREp::Plan::ValidatePlanHostArguments(
        fixture.arguments, fixture.runtime, context == nullptr ? &local : context);
}

void InstallRuntimeGetters(Fixture &fixture)
{
    g_hostCommArgs = &fixture.commArgs;
    g_deviceCommArgs = fixture.runtime.deviceCommArgs;
    g_hostCommRet = TileXR::TILEXR_SUCCESS;
    g_deviceCommRet = TileXR::TILEXR_SUCCESS;
}

void TestValidContextAndHeader()
{
    Fixture fixture;
    TileXREp::Plan::PlanHostContext context {};
    CheckInt("valid host context", Validate(fixture, &context), TileXR::TILEXR_SUCCESS);
    Check(context.layout.local.totalBytes == fixture.layout.local.totalBytes, "local layout copied");
    Check(context.layout.registeredMeta.totalBytes == fixture.layout.registeredMeta.totalBytes,
        "metadata layout copied");
    Check(context.callHeader.abiVersion == TileXREp::Plan::kPlanAbiVersion, "ABI version in header");
    Check(context.callHeader.rankSize == fixture.commArgs.rankSize, "rank size in header");
    Check(context.callHeader.epoch == fixture.plan.epoch, "epoch in header");
    Check(context.rank == fixture.commArgs.rank, "communicator rank forwarded");
    Check(context.deviceCommArgs == fixture.runtime.deviceCommArgs, "device comm args forwarded");

    TileXREp::Plan::PlanCallHeader same = context.callHeader;
    Check(TileXREp::Plan::PlanCallHeadersMatch(context.callHeader, same), "identical headers match");
#define CHECK_HEADER_MISMATCH(FIELD)     do {         same = context.callHeader;         ++same.FIELD;         Check(!TileXREp::Plan::PlanCallHeadersMatch(context.callHeader, same),             #FIELD " mismatch detected");     } while (0)
    CHECK_HEADER_MISMATCH(abiVersion);
    CHECK_HEADER_MISMATCH(headerBytes);
    CHECK_HEADER_MISMATCH(rankSize);
    CHECK_HEADER_MISMATCH(s);
    CHECK_HEADER_MISMATCH(k);
    CHECK_HEADER_MISMATCH(expertNum);
    CHECK_HEADER_MISMATCH(prefetchSlots);
    CHECK_HEADER_MISMATCH(rankTokenCapacity);
    CHECK_HEADER_MISMATCH(nvS);
    CHECK_HEADER_MISMATCH(tokenPadding);
    CHECK_HEADER_MISMATCH(tokenRouteLimitPerPair);
    CHECK_HEADER_MISMATCH(cardsPerServer);
    CHECK_HEADER_MISMATCH(cardsPerCabinet);
    CHECK_HEADER_MISMATCH(crossCandidateCount);
    CHECK_HEADER_MISMATCH(epoch);
    CHECK_HEADER_MISMATCH(topologyHash);
#undef CHECK_HEADER_MISMATCH
}

void TestPointersAlignmentWorkspaceAndWaitBudget()
{
    Fixture fixture;
    fixture.arguments.topkExperts = nullptr;
    CheckInt("null input", Validate(fixture), TileXR::TILEXR_ERROR_PARA_CHECK_FAIL);

    fixture.Reset();
    fixture.plan.status = nullptr;
    CheckInt("null output", Validate(fixture), TileXR::TILEXR_ERROR_PARA_CHECK_FAIL);

    fixture.Reset();
    fixture.plan.dst = reinterpret_cast<int32_t *>(0x1002);
    CheckInt("misaligned output", Validate(fixture), TileXR::TILEXR_ERROR_PARA_CHECK_FAIL);

    fixture.Reset();
    fixture.arguments.remoteExperts = reinterpret_cast<int32_t *>(0x9000);
    CheckInt("unpaired remoteExperts", Validate(fixture), TileXR::TILEXR_ERROR_PARA_CHECK_FAIL);

    fixture.Reset();
    fixture.arguments.expertTargets = reinterpret_cast<uint64_t *>(0xA000);
    CheckInt("unpaired expertTargets", Validate(fixture), TileXR::TILEXR_ERROR_PARA_CHECK_FAIL);

    fixture.Reset();
    fixture.arguments.remoteExperts = reinterpret_cast<int32_t *>(0x9002);
    fixture.arguments.expertTargets = reinterpret_cast<uint64_t *>(0xA000);
    CheckInt("misaligned remoteExperts", Validate(fixture), TileXR::TILEXR_ERROR_PARA_CHECK_FAIL);

    fixture.Reset();
    fixture.arguments.remoteExperts = reinterpret_cast<int32_t *>(0x9000);
    fixture.arguments.expertTargets = reinterpret_cast<uint64_t *>(0xA004);
    CheckInt("misaligned expertTargets", Validate(fixture), TileXR::TILEXR_ERROR_PARA_CHECK_FAIL);

    fixture.Reset();
    fixture.arguments.localWorkspace = reinterpret_cast<void *>(0x200020);
    CheckInt("misaligned local workspace", Validate(fixture), TileXR::TILEXR_ERROR_PARA_CHECK_FAIL);

    fixture.Reset();
    --fixture.arguments.localWorkspaceBytes;
    CheckInt("small local workspace", Validate(fixture), TileXR::TILEXR_ERROR_PARA_CHECK_FAIL);

    fixture.Reset();
    --fixture.arguments.registeredMetaBytes;
    CheckInt("small metadata workspace", Validate(fixture), TileXR::TILEXR_ERROR_PARA_CHECK_FAIL);

    fixture.Reset();
    fixture.arguments.waitIterations = 0;
    CheckInt("zero wait budget", Validate(fixture), TileXR::TILEXR_ERROR_PARA_CHECK_FAIL);

    fixture.Reset();
    fixture.arguments.stream = nullptr;
    CheckInt("null stream", Validate(fixture), TileXR::TILEXR_ERROR_PARA_CHECK_FAIL);
}

void TestDescriptorAndConfig()
{
    Fixture fixture;
    --fixture.plan.cap;
    CheckInt("descriptor CAP mismatch", Validate(fixture), TileXR::TILEXR_ERROR_PARA_CHECK_FAIL);

    fixture.Reset();
    --fixture.plan.r;
    CheckInt("descriptor rank size mismatch", Validate(fixture), TileXR::TILEXR_ERROR_PARA_CHECK_FAIL);

    fixture.Reset();
    fixture.plan.epoch = 0;
    CheckInt("zero epoch", Validate(fixture), TileXR::TILEXR_ERROR_PARA_CHECK_FAIL);

    fixture.Reset();
    fixture.config.cardsPerServer = 4;
    CheckInt("unsupported topology", Validate(fixture), TileXR::TILEXR_ERROR_PARA_CHECK_FAIL);
}

void TestCommunicatorAndPeerWindows()
{
    Fixture fixture;
    fixture.commArgs.rank = fixture.commArgs.rankSize;
    CheckInt("invalid communicator rank", Validate(fixture), TileXR::TILEXR_ERROR_PARA_CHECK_FAIL);

    fixture.Reset();
    fixture.commArgs.rankSize = 0;
    CheckInt("invalid communicator rank size", Validate(fixture), TileXR::TILEXR_ERROR_PARA_CHECK_FAIL);

    fixture.Reset();
    fixture.commArgs.extraFlag = 0;
    CheckInt("non-A5 communicator", Validate(fixture), TileXR::TILEXR_ERROR_NOT_SUPPORT);

    fixture.Reset();
    fixture.runtime.deviceCommArgs = nullptr;
    CheckInt("missing device comm args", Validate(fixture), TileXR::TILEXR_ERROR_NOT_INITIALIZED);

    fixture.Reset();
    fixture.commArgs.peerMems[5] = nullptr;
    CheckInt("missing peer memory window", Validate(fixture), TileXR::TILEXR_ERROR_NOT_INITIALIZED);

    fixture.Reset();
    const TileXREp::Plan::PlanPeerMailboxLayout mailbox =
        TileXREp::Plan::BuildPlanPeerMailboxLayout(fixture.commArgs.rankSize, fixture.plan.e);
    const int64_t expertCapacity = static_cast<int64_t>(
        (mailbox.rowBytes - mailbox.tpe) / sizeof(int32_t));
    const int64_t oversizedExpertNum =
        ((expertCapacity / fixture.commArgs.rankSize) + 1) * fixture.commArgs.rankSize;
    fixture.plan.e = oversizedExpertNum;
    fixture.arguments.expertNum = oversizedExpertNum;
    CheckInt("oversized peer mailbox payload layout", TileXREp::Plan::BuildPlanWorkspaceLayout(
        fixture.commArgs.rankSize, fixture.plan.s, fixture.plan.k, oversizedExpertNum,
        fixture.config, &fixture.layout), TileXR::TILEXR_SUCCESS);
    fixture.arguments.localWorkspaceBytes = fixture.layout.local.totalBytes;
    fixture.arguments.registeredMetaBytes = fixture.layout.registeredMeta.totalBytes;
    CheckInt("oversized peer mailbox payload", Validate(fixture),
        TileXR::TILEXR_ERROR_PARA_CHECK_FAIL);
}

void TestCommittedPlanReuseContract()
{
    Fixture fixture;
    TileXREp::Plan::PlanCallHeader committedHeader {};
    CheckInt("build committed header", TileXREp::Plan::BuildPlanCallHeader(
        fixture.commArgs.rankSize, fixture.arguments.s, fixture.arguments.topK,
        fixture.arguments.expertNum, fixture.config, fixture.plan.epoch, 0x1234,
        &committedHeader), TileXR::TILEXR_SUCCESS);
    TileXREp::Plan::PlanCallHeader requestedHeader = committedHeader;
    TileXREp::Plan::PlanEpochState epochState {};
    epochState.requestedEpoch = fixture.plan.epoch;
    epochState.committedEpoch = fixture.plan.epoch;
    epochState.topologyHash = committedHeader.topologyHash;
    epochState.reserved = TileXREp::Plan::kPlanAffinityCacheValid;
    Check(TileXREp::Plan::IsCommittedPlanReusable(committedHeader, requestedHeader, epochState),
        "matching committed identity must be reusable");

    ++requestedHeader.topologyHash;
    Check(!TileXREp::Plan::IsCommittedPlanReusable(committedHeader, requestedHeader, epochState),
        "topology mismatch must reject reuse");
    requestedHeader = committedHeader;
    epochState.committedEpoch = 0;
    Check(!TileXREp::Plan::IsCommittedPlanReusable(committedHeader, requestedHeader, epochState),
        "uncommitted plan must not be reusable");
}

void TestPrepareLaunchContext()
{
    Fixture fixture;
    InstallRuntimeGetters(fixture);
    TileXREp::Plan::PlanHostContext context {};
    const TileXRCommPtr comm = reinterpret_cast<TileXRCommPtr>(0x600000);
    CheckInt("prepare launch context", TileXREp::Plan::PreparePlanLaunchContext(
        comm, fixture.arguments, &context), TileXR::TILEXR_SUCCESS);
    Check(context.deviceCommArgs == g_deviceCommArgs, "prepare forwards device comm args");

    g_deviceCommRet = TileXR::TILEXR_ERROR_INTERNAL;
    CheckInt("device comm getter error", TileXREp::Plan::PreparePlanLaunchContext(
        comm, fixture.arguments, &context), TileXR::TILEXR_ERROR_INTERNAL);
    g_deviceCommRet = TileXR::TILEXR_SUCCESS;

    g_hostCommRet = TileXR::TILEXR_ERROR_INTERNAL;
    CheckInt("host comm getter error", TileXREp::Plan::PreparePlanLaunchContext(
        comm, fixture.arguments, &context), TileXR::TILEXR_ERROR_INTERNAL);
    g_hostCommRet = TileXR::TILEXR_SUCCESS;

    CheckInt("null communicator", TileXREp::Plan::PreparePlanLaunchContext(
        nullptr, fixture.arguments, &context), TileXR::TILEXR_ERROR_PARA_CHECK_FAIL);
}

void TestLaunchPlanKernel()
{
    Fixture fixture;
    TileXREp::Plan::PlanHostContext context {};
    CheckInt("launch fixture validates", Validate(fixture, &context), TileXR::TILEXR_SUCCESS);
    const TileXRCommPtr comm = reinterpret_cast<TileXRCommPtr>(0x600000);

    g_launchCapture = PlanLaunchCapture {};
    g_launchRet = RT_ERROR_NONE;
    g_nextMagicRet = TileXR::TILEXR_SUCCESS;
    g_nextMagic = 12345;
    CheckInt("launch plan kernel", TileXREp::Plan::LaunchPlanKernel(
        comm, fixture.arguments, context), TileXR::TILEXR_SUCCESS);
    Check(g_launchCapture.calls == 1, "exactly one plan kernel launch");
    Check(g_launchCapture.blockDim == 1, "plan kernel uses one block");
    Check(g_launchCapture.stream == fixture.arguments.stream, "launch forwards stream");
    Check(g_launchCapture.commArgs == context.deviceCommArgs, "launch forwards device comm args");
    Check(g_launchCapture.rank == fixture.commArgs.rank, "launch forwards communicator rank");
    Check(g_launchCapture.rankSize == fixture.commArgs.rankSize, "launch forwards rank size");
    Check(g_launchCapture.topkExperts == reinterpret_cast<GM_ADDR>(
        const_cast<int32_t *>(fixture.arguments.topkExperts)), "launch forwards topkExperts");
    Check(g_launchCapture.tokensPerExpert == reinterpret_cast<GM_ADDR>(
        const_cast<int32_t *>(fixture.arguments.tokensPerExpert)), "launch forwards tokensPerExpert");
    Check(g_launchCapture.globalRankIds == reinterpret_cast<GM_ADDR>(
        const_cast<int32_t *>(fixture.arguments.globalRankIds)), "launch forwards globalRankIds");
    Check(g_launchCapture.dst == reinterpret_cast<GM_ADDR>(fixture.plan.dst), "launch forwards dst");
    Check(g_launchCapture.cuSeqlens == reinterpret_cast<GM_ADDR>(fixture.plan.cuSeqlens),
        "launch forwards cuSeqlens");
    Check(g_launchCapture.expertsToCopy == reinterpret_cast<GM_ADDR>(fixture.plan.expertsToCopy),
        "launch forwards expertsToCopy");
    Check(g_launchCapture.remoteExperts == reinterpret_cast<GM_ADDR>(fixture.arguments.remoteExperts),
        "launch forwards remoteExperts");
    Check(g_launchCapture.expertTargets == reinterpret_cast<GM_ADDR>(fixture.arguments.expertTargets),
        "launch forwards expertTargets");
    Check(g_launchCapture.remoteStats == reinterpret_cast<GM_ADDR>(fixture.plan.remoteStats),
        "launch forwards remoteStats");
    Check(g_launchCapture.status == reinterpret_cast<GM_ADDR>(fixture.plan.status),
        "launch forwards status");
    Check(g_launchCapture.localWorkspace == static_cast<GM_ADDR>(fixture.arguments.localWorkspace),
        "launch forwards local workspace");
    Check(g_launchCapture.metaWorkspace == static_cast<GM_ADDR>(fixture.arguments.registeredMetaWorkspace),
        "launch forwards metadata workspace");
    Check(g_launchCapture.epoch == fixture.plan.epoch, "launch forwards epoch");
    Check(g_launchCapture.waitIterations == fixture.arguments.waitIterations,
        "launch forwards wait budget");
    Check(g_launchCapture.magic == g_nextMagic, "launch forwards allocated magic");

    g_launchRet = static_cast<rtError_t>(1);
    CheckInt("runtime launch failure", TileXREp::Plan::LaunchPlanKernel(
        comm, fixture.arguments, context), TileXR::TILEXR_ERROR_MKIRT);
    g_launchRet = RT_ERROR_NONE;

    g_launchCapture = PlanLaunchCapture {};
    g_nextMagicRet = TileXR::TILEXR_ERROR_INTERNAL;
    CheckInt("magic allocation error", TileXREp::Plan::LaunchPlanKernel(
        comm, fixture.arguments, context), TileXR::TILEXR_ERROR_INTERNAL);
    Check(g_launchCapture.calls == 0, "magic failure prevents launch");
    g_nextMagicRet = TileXR::TILEXR_SUCCESS;
}

void TestPublicPlanLaunch()
{
    Fixture fixture;
    InstallRuntimeGetters(fixture);
    g_launchCapture = PlanLaunchCapture {};
    g_launchRet = RT_ERROR_NONE;
    g_nextMagicRet = TileXR::TILEXR_SUCCESS;
    g_nextMagic = 6789;
    const TileXRMoonEPPlanDesc originalPlan = fixture.plan;
    const TileXRCommPtr comm = reinterpret_cast<TileXRCommPtr>(0x600000);
    CheckInt("valid public plan launches", TileXRMoeEpPlanV2(
        fixture.arguments.topkExperts, fixture.arguments.tokensPerExpert,
        fixture.arguments.globalRankIds, comm, fixture.arguments.s, fixture.arguments.topK,
        fixture.arguments.expertNum, &fixture.config, &fixture.plan,
        fixture.arguments.localWorkspace, fixture.arguments.localWorkspaceBytes,
        fixture.arguments.registeredMetaWorkspace, fixture.arguments.registeredMetaBytes,
        fixture.arguments.stream), TileXR::TILEXR_SUCCESS);
    Check(g_launchCapture.calls == 1, "public plan launches once");
    Check(g_launchCapture.waitIterations == (1ULL << 24),
        "optimized public API uses bounded default wait budget");
    Check(fixture.plan.dst == originalPlan.dst && fixture.plan.epoch == originalPlan.epoch,
        "host launch does not mutate plan descriptor");

    g_launchCapture = PlanLaunchCapture {};
    CheckInt("invalid public arguments do not launch", TileXRMoeEpPlanV2(
        nullptr, fixture.arguments.tokensPerExpert, fixture.arguments.globalRankIds, comm,
        fixture.arguments.s, fixture.arguments.topK, fixture.arguments.expertNum, &fixture.config,
        &fixture.plan, fixture.arguments.localWorkspace, fixture.arguments.localWorkspaceBytes,
        fixture.arguments.registeredMetaWorkspace, fixture.arguments.registeredMetaBytes,
        fixture.arguments.stream), TileXR::TILEXR_ERROR_PARA_CHECK_FAIL);
    Check(g_launchCapture.calls == 0, "invalid public arguments prevent launch");

    alignas(TileXRMoonEPPlanConfig) unsigned char configStorage[sizeof(TileXRMoonEPPlanConfig) + 1] = {};
    const TileXRMoonEPPlanConfig *misalignedConfig =
        reinterpret_cast<const TileXRMoonEPPlanConfig *>(configStorage + 1);
    g_launchCapture = PlanLaunchCapture {};
    CheckInt("misaligned public config is rejected", TileXRMoeEpPlanV2(
        fixture.arguments.topkExperts, fixture.arguments.tokensPerExpert,
        fixture.arguments.globalRankIds, comm, fixture.arguments.s, fixture.arguments.topK,
        fixture.arguments.expertNum, misalignedConfig, &fixture.plan,
        fixture.arguments.localWorkspace, fixture.arguments.localWorkspaceBytes,
        fixture.arguments.registeredMetaWorkspace, fixture.arguments.registeredMetaBytes,
        fixture.arguments.stream), TileXR::TILEXR_ERROR_PARA_CHECK_FAIL);
    Check(g_launchCapture.calls == 0, "misaligned public config prevents launch");

    alignas(TileXRMoonEPPlanDesc) unsigned char planStorage[sizeof(TileXRMoonEPPlanDesc) + 1] = {};
    TileXRMoonEPPlanDesc *misalignedPlan = reinterpret_cast<TileXRMoonEPPlanDesc *>(planStorage + 1);
    g_launchCapture = PlanLaunchCapture {};
    CheckInt("misaligned public plan is rejected", TileXRMoeEpPlanV2(
        fixture.arguments.topkExperts, fixture.arguments.tokensPerExpert,
        fixture.arguments.globalRankIds, comm, fixture.arguments.s, fixture.arguments.topK,
        fixture.arguments.expertNum, &fixture.config, misalignedPlan,
        fixture.arguments.localWorkspace, fixture.arguments.localWorkspaceBytes,
        fixture.arguments.registeredMetaWorkspace, fixture.arguments.registeredMetaBytes,
        fixture.arguments.stream), TileXR::TILEXR_ERROR_PARA_CHECK_FAIL);
    Check(g_launchCapture.calls == 0, "misaligned public plan prevents launch");
}

void TestPublicMetadataPlanLaunch()
{
    Fixture fixture;
    InstallRuntimeGetters(fixture);
    const TileXRCommPtr comm = reinterpret_cast<TileXRCommPtr>(0x600000);
    TileXRMoonEPPlanMetadataV2 metadata = ValidMetadata(fixture.config, fixture.plan);

    g_launchCapture = PlanLaunchCapture {};
    CheckInt("valid metadata plan launches", TileXRMoeEpPlanV2WithMetadata(
        fixture.arguments.topkExperts, fixture.arguments.tokensPerExpert,
        fixture.arguments.globalRankIds, comm, fixture.arguments.s, fixture.arguments.topK,
        fixture.arguments.expertNum, &fixture.config, &metadata, fixture.arguments.localWorkspace,
        fixture.arguments.localWorkspaceBytes, fixture.arguments.registeredMetaWorkspace,
        fixture.arguments.registeredMetaBytes, fixture.arguments.stream), TileXR::TILEXR_SUCCESS);
    Check(g_launchCapture.calls == 1, "metadata plan launches once");
    Check(g_launchCapture.remoteExperts == reinterpret_cast<GM_ADDR>(metadata.remoteExperts),
        "metadata API forwards remoteExperts");
    Check(g_launchCapture.expertTargets == reinterpret_cast<GM_ADDR>(metadata.expertTargets),
        "metadata API forwards expertTargets");

    alignas(TileXRMoonEPPlanMetadataV2) unsigned char metadataStorage[sizeof(TileXRMoonEPPlanMetadataV2) + 1] = {};
    TileXRMoonEPPlanMetadataV2 *misalignedMetadata =
        reinterpret_cast<TileXRMoonEPPlanMetadataV2 *>(metadataStorage + 1);
    g_launchCapture = PlanLaunchCapture {};
    CheckInt("misaligned metadata descriptor is rejected", TileXRMoeEpPlanV2WithMetadata(
        fixture.arguments.topkExperts, fixture.arguments.tokensPerExpert,
        fixture.arguments.globalRankIds, comm, fixture.arguments.s, fixture.arguments.topK,
        fixture.arguments.expertNum, &fixture.config, misalignedMetadata,
        fixture.arguments.localWorkspace, fixture.arguments.localWorkspaceBytes,
        fixture.arguments.registeredMetaWorkspace, fixture.arguments.registeredMetaBytes,
        fixture.arguments.stream), TileXR::TILEXR_ERROR_PARA_CHECK_FAIL);
    Check(g_launchCapture.calls == 0, "misaligned metadata descriptor prevents launch");

    auto ExpectRejected = [&](const char *name, const TileXRMoonEPPlanMetadataV2 &invalid,
                              int64_t callS = -1, int64_t callTopK = -1) {
        TileXRMoonEPPlanMetadataV2 copy = invalid;
        g_launchCapture = PlanLaunchCapture {};
        const int64_t actualS = callS < 0 ? fixture.arguments.s : callS;
        const int64_t actualTopK = callTopK < 0 ? fixture.arguments.topK : callTopK;
        CheckInt(name, TileXRMoeEpPlanV2WithMetadata(
            fixture.arguments.topkExperts, fixture.arguments.tokensPerExpert,
            fixture.arguments.globalRankIds, comm, actualS, actualTopK, fixture.arguments.expertNum,
            &fixture.config, &copy, fixture.arguments.localWorkspace,
            fixture.arguments.localWorkspaceBytes, fixture.arguments.registeredMetaWorkspace,
            fixture.arguments.registeredMetaBytes, fixture.arguments.stream),
            TileXR::TILEXR_ERROR_PARA_CHECK_FAIL);
        Check(g_launchCapture.calls == 0, "invalid metadata prevents launch");
    };

    TileXRMoonEPPlanMetadataV2 invalid = metadata;
    invalid.structSize = sizeof(invalid) - 1;
    ExpectRejected("metadata struct size", invalid);
    invalid = metadata;
    ++invalid.abiVersion;
    ExpectRejected("metadata ABI version", invalid);
    struct MetadataCountCase {
        const char *name;
        uint64_t TileXRMoonEPPlanMetadataV2::*member;
    };
    const MetadataCountCase countCases[] = {
        {"metadata dst count", &TileXRMoonEPPlanMetadataV2::dstCount},
        {"metadata cuSeqlens count", &TileXRMoonEPPlanMetadataV2::cuSeqlensCount},
        {"metadata remoteExperts count", &TileXRMoonEPPlanMetadataV2::remoteExpertsCount},
        {"metadata expertTargets count", &TileXRMoonEPPlanMetadataV2::expertTargetsCount},
        {"metadata remoteStats count", &TileXRMoonEPPlanMetadataV2::remoteStatsCount},
        {"metadata status count", &TileXRMoonEPPlanMetadataV2::statusCount},
        {"metadata dupGroups count", &TileXRMoonEPPlanMetadataV2::dupGroupsCount},
        {"metadata dupLoffs count", &TileXRMoonEPPlanMetadataV2::dupLoffsCount},
        {"metadata dupCounts count", &TileXRMoonEPPlanMetadataV2::dupCountsCount},
    };
    for (const MetadataCountCase &countCase : countCases) {
        invalid = metadata;
        --(invalid.*(countCase.member));
        ExpectRejected(countCase.name, invalid);
    }
    invalid = metadata;
    invalid.remoteExperts = nullptr;
    ExpectRejected("metadata remoteExperts pointer", invalid);
    invalid = metadata;
    invalid.expertTargets = nullptr;
    ExpectRejected("metadata expertTargets pointer", invalid);
    invalid = metadata;
    invalid.r = fixture.commArgs.rankSize / 2;
    invalid.remoteExpertsCount = static_cast<uint64_t>(invalid.r * invalid.b);
    invalid.expertTargetsCount = static_cast<uint64_t>((invalid.e / invalid.r) * ((invalid.r + 63) / 64));
    ExpectRejected("metadata communicator rank count", invalid);
    invalid = metadata;
    invalid.dstCount = UINT64_MAX;
    invalid.s = INT64_MAX;
    ExpectRejected("metadata dst count overflow", invalid, INT64_MAX, 2);

    g_launchCapture = PlanLaunchCapture {};
    CheckInt("legacy API remains valid", TileXRMoeEpPlanV2(
        fixture.arguments.topkExperts, fixture.arguments.tokensPerExpert,
        fixture.arguments.globalRankIds, comm, fixture.arguments.s, fixture.arguments.topK,
        fixture.arguments.expertNum, &fixture.config, &fixture.plan,
        fixture.arguments.localWorkspace, fixture.arguments.localWorkspaceBytes,
        fixture.arguments.registeredMetaWorkspace, fixture.arguments.registeredMetaBytes,
        fixture.arguments.stream), TileXR::TILEXR_SUCCESS);
    Check(g_launchCapture.remoteExperts == nullptr && g_launchCapture.expertTargets == nullptr,
        "legacy API keeps metadata outputs disabled");
}
} // namespace

int main()
{
    TestValidContextAndHeader();
    TestPointersAlignmentWorkspaceAndWaitBudget();
    TestDescriptorAndConfig();
    TestCommunicatorAndPeerWindows();
    TestCommittedPlanReuseContract();
    TestPrepareLaunchContext();
    TestLaunchPlanKernel();
    TestPublicPlanLaunch();
    TestPublicMetadataPlanLaunch();
    return g_failures == 0 ? 0 : 1;
}
