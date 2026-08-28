#include <cstdint>
#include <iostream>
#include <limits>
#include <string>

#include "acl/acl_rt.h"
#include "comm_args.h"
#include "reduce_grad_host.h"
#include "tilexr_api.h"
#include "tilexr_moonep.h"
#include "tilexr_types.h"
#include "tilexr_udma_reg.h"

#ifndef UINTPTR_C
#define UINTPTR_C(value) static_cast<uintptr_t>(value)
#endif

namespace {

int g_failures = 0;
int g_launchCalls = 0;
int g_memsetCalls = 0;
int g_syncCalls = 0;
int g_profileRegisterCalls = 0;
int g_profileQueryCalls = 0;
int g_profileUnregisterCalls = 0;
int g_qpCountCalls = 0;
int g_profileRegisterReturn = TileXR::TILEXR_SUCCESS;
int g_profileQueryReturn = TileXR::TILEXR_SUCCESS;
int g_profileUnregisterReturn = TileXR::TILEXR_SUCCESS;
int g_qpCountReturn = TileXR::TILEXR_SUCCESS;
int g_launchReturn = TileXR::TILEXR_SUCCESS;
aclError g_syncReturn = ACL_SUCCESS;
uint32_t g_qpCount = 8;
TileXR::CommArgs g_commArgs {};
TileXR::TileXRUDMAProfileDesc g_profileDesc {};
TileXR::TileXRUDMAProfileRegistry g_profileRegistry {};
TileXR::TileXRUDMAProfileView g_profileView {};
GM_ADDR g_commDev = reinterpret_cast<GM_ADDR>(UINTPTR_C(0x800000));

void Check(bool condition, const std::string &message)
{
    if (!condition) {
        std::cerr << message << std::endl;
        ++g_failures;
    }
}

void CheckStatus(const std::string &label, int actual, int expected)
{
    if (actual != expected) {
        std::cerr << label << " returned " << actual << ", expected " << expected << std::endl;
        ++g_failures;
    }
}

TileXRMoonEpPlanV1 Plan(int64_t rankSize = 8, int64_t expertsPerRank = 2,
    int64_t prefetchSlots = 2)
{
    TileXRMoonEpPlanV1 plan {};
    plan.structSize = sizeof(plan);
    plan.abiVersion = TILEXR_MOONEP_ABI_VERSION_V1;
    plan.n = 4;
    plan.k = 2;
    plan.e = rankSize * expertsPerRank;
    plan.b = prefetchSlots;
    plan.r = rankSize;
    plan.nvS = 4;
    plan.expertsToCopy = reinterpret_cast<void *>(UINTPTR_C(0x13000));
    return plan;
}

TileXRMoonEpTensorV1 Gradient(void *data, int64_t rows, uint64_t rowElements)
{
    TileXRMoonEpTensorV1 tensor {};
    tensor.structSize = sizeof(tensor);
    tensor.abiVersion = TILEXR_MOONEP_ABI_VERSION_V1;
    tensor.data = data;
    tensor.elementCount = static_cast<uint64_t>(rows) * rowElements;
    tensor.dtype = TILEXR_MOONEP_DTYPE_FLOAT32;
    tensor.rank = 2;
    tensor.shape[0] = rows;
    tensor.shape[1] = static_cast<int64_t>(rowElements);
    return tensor;
}

TileXRMoonEpTensorV1 Status()
{
    TileXRMoonEpTensorV1 tensor {};
    tensor.structSize = sizeof(tensor);
    tensor.abiVersion = TILEXR_MOONEP_ABI_VERSION_V1;
    tensor.data = reinterpret_cast<void *>(UINTPTR_C(0x16000));
    tensor.elementCount = 1;
    tensor.dtype = TILEXR_MOONEP_DTYPE_INT32;
    tensor.rank = 1;
    tensor.shape[0] = 1;
    return tensor;
}

void Reset(int64_t rankSize = 8)
{
    g_launchCalls = 0;
    g_memsetCalls = 0;
    g_syncCalls = 0;
    g_profileRegisterCalls = 0;
    g_profileQueryCalls = 0;
    g_profileUnregisterCalls = 0;
    g_qpCountCalls = 0;
    g_profileRegisterReturn = TileXR::TILEXR_SUCCESS;
    g_profileQueryReturn = TileXR::TILEXR_SUCCESS;
    g_profileUnregisterReturn = TileXR::TILEXR_SUCCESS;
    g_qpCountReturn = TileXR::TILEXR_SUCCESS;
    g_launchReturn = TileXR::TILEXR_SUCCESS;
    g_syncReturn = ACL_SUCCESS;
    g_qpCount = 8;
    g_commArgs = TileXR::CommArgs {};
    g_commArgs.rank = 0;
    g_commArgs.localRank = 0;
    g_commArgs.rankSize = static_cast<int>(rankSize);
    g_commArgs.localRankSize = static_cast<int>(rankSize);
    g_commArgs.extraFlag = TileXR::ExtraFlag::TOPO_910A5;
    g_commArgs.extraFlag |= TileXR::ExtraFlag::UDMA;
    g_profileDesc = TileXR::TileXRUDMAProfileDesc {};
    g_profileRegistry = TileXR::TileXRUDMAProfileRegistry {};
    g_profileView = TileXR::TileXRUDMAProfileView {};
}

TileXRMoonEpReduceGradWorkspaceInfoV2 Query(TileXRMoonEpPlanV1 *plan,
    TileXRMoonEpTensorV1 *gate, TileXRMoonEpTensorV1 *up,
    TileXRMoonEpTensorV1 *down, int expected)
{
    TileXRMoonEpReduceGradWorkspaceQueryV2 query {};
    query.structSize = sizeof(query);
    query.abiVersion = TILEXR_MOONEP_ABI_VERSION_V2;
    query.comm = reinterpret_cast<TileXRCommPtr>(UINTPTR_C(0x1000));
    query.plan = plan;
    query.gate = gate;
    query.up = up;
    query.down = down;
    TileXRMoonEpReduceGradWorkspaceInfoV2 info {};
    info.structSize = sizeof(info);
    info.abiVersion = TILEXR_MOONEP_ABI_VERSION_V2;
    CheckStatus("workspace query", TileXRMoonEpReduceGradGetWorkspaceSizeV2(
        &query, &info), expected);
    return info;
}

TileXRMoonEpReduceGradPrepareArgsV2 PrepareArgs(TileXRMoonEpPlanV1 *plan,
    TileXRMoonEpTensorV1 *gate, TileXRMoonEpTensorV1 *up,
    TileXRMoonEpTensorV1 *down,
    const TileXRMoonEpReduceGradWorkspaceInfoV2 &info)
{
    TileXRMoonEpReduceGradPrepareArgsV2 args {};
    args.structSize = sizeof(args);
    args.abiVersion = TILEXR_MOONEP_ABI_VERSION_V2;
    args.comm = reinterpret_cast<TileXRCommPtr>(UINTPTR_C(0x1000));
    args.plan = plan;
    args.gate = gate;
    args.up = up;
    args.down = down;
    args.workspace = info.workspaceBytes == 0 ? nullptr :
        reinterpret_cast<void *>(UINTPTR_C(0x20000000));
    args.workspaceBytes = info.workspaceBytes;
    TileXRMoonEpTensorV1 *gradients[] = {gate, up, down};
    for (uint32_t projection = 0; projection < 3; ++projection) {
        args.sources[projection].registrationBase = reinterpret_cast<void *>(
            UINTPTR_C(0x30000000) + projection * UINTPTR_C(0x10000000));
        args.sources[projection].data = static_cast<uint8_t *>(
            args.sources[projection].registrationBase) + 0x1000;
        args.sources[projection].bytes = static_cast<uint64_t>(plan->b) *
            info.rowBytes[projection];
        args.sources[projection].registrationBytes =
            args.sources[projection].bytes + 0x2000;
        Check(args.sources[projection].bytes == static_cast<uint64_t>(plan->b) *
            static_cast<uint64_t>(gradients[projection]->shape[1]) * sizeof(float),
            "test source byte construction mismatch");
    }
    return args;
}

TileXRMoonEpReduceGradArgsV2 LaunchArgs(TileXRMoonEpReduceGradPreparedV2 prepared,
    const TileXRMoonEpReduceGradPrepareArgsV2 &prepare,
    TileXRMoonEpTensorV1 *status)
{
    TileXRMoonEpReduceGradArgsV2 args {};
    args.structSize = sizeof(args);
    args.abiVersion = TILEXR_MOONEP_ABI_VERSION_V2;
    args.prepared = prepared;
    args.plan = prepare.plan;
    args.gate = prepare.gate;
    args.up = prepare.up;
    args.down = prepare.down;
    for (uint32_t projection = 0; projection < 3; ++projection) {
        args.sources[projection] = prepare.sources[projection];
    }
    args.status = status;
    args.waitIterations = 1000;
    return args;
}

void TestPreparedLifecycleAndHotLaunch()
{
    Reset();
    TileXRMoonEpPlanV1 plan = Plan();
    TileXRMoonEpTensorV1 gate = Gradient(
        reinterpret_cast<void *>(UINTPTR_C(0x400000)), 18, 1024);
    TileXRMoonEpTensorV1 up = Gradient(
        reinterpret_cast<void *>(UINTPTR_C(0x500000)), 18, 2048);
    TileXRMoonEpTensorV1 down = Gradient(
        reinterpret_cast<void *>(UINTPTR_C(0x600000)), 18, 4096);
    const auto info = Query(&plan, &gate, &up, &down, TileXR::TILEXR_SUCCESS);
    Check(info.workspaceBytes > 0 && info.qpCount == 8 &&
        info.projectionQpCounts[0] == 2 && info.projectionQpCounts[1] == 2 &&
        info.projectionQpCounts[2] == 4,
        "workspace query did not expose weighted owner-pull layout");
    auto prepare = PrepareArgs(&plan, &gate, &up, &down, info);
    TileXRMoonEpReduceGradPreparedV2 prepared = nullptr;
    CheckStatus("prepare", TileXRMoonEpReduceGradPrepareV2(&prepare, &prepared),
        TileXR::TILEXR_SUCCESS);
    Check(prepared != nullptr && g_profileRegisterCalls == 1 &&
        g_profileQueryCalls == 1,
        "prepare must register and query exactly one persistent profile");
    Check(g_profileDesc.regionCount == 4 && g_profileDesc.qpBindingCount == 8,
        "profile must contain staging plus three source regions");
    Check(g_profileDesc.regions[1].base == prepare.sources[0].data &&
        g_profileDesc.regions[1].bytes == prepare.sources[0].bytes &&
        g_profileDesc.regions[1].registrationBase ==
            prepare.sources[0].registrationBase &&
        g_profileDesc.regions[1].registrationBytes ==
            prepare.sources[0].registrationBytes,
        "profile must keep the logical source view separate from its backing MR");
    for (uint32_t qp = 0; qp < 8; ++qp) {
        Check(g_profileDesc.qpBindings[qp].localRegion == 0,
            "every QP must write into the staging MR");
    }

    TileXRMoonEpTensorV1 status = Status();
    auto launch = LaunchArgs(prepared, prepare, &status);
    aclrtStream stream = reinterpret_cast<aclrtStream>(UINTPTR_C(0x700000));
    CheckStatus("hot launch", TileXRMoonEpReduceGradV2(&launch, stream),
        TileXR::TILEXR_SUCCESS);
    Check(g_profileRegisterCalls == 1 && g_profileQueryCalls == 2 &&
        g_memsetCalls == 1 && g_launchCalls == 1,
        "hot launch must only validate the persistent profile and enqueue work");

    launch.sources[0].data = reinterpret_cast<void *>(UINTPTR_C(0xdead0000));
    CheckStatus("source identity mismatch", TileXRMoonEpReduceGradV2(&launch, stream),
        TileXR::TILEXR_ERROR_PARA_CHECK_FAIL);
    launch.sources[0] = prepare.sources[0];
    launch.sources[0].registrationBase =
        reinterpret_cast<void *>(UINTPTR_C(0xdead0000));
    CheckStatus("source registration identity mismatch",
        TileXRMoonEpReduceGradV2(&launch, stream),
        TileXR::TILEXR_ERROR_PARA_CHECK_FAIL);
    launch.sources[0] = prepare.sources[0];
    gate.data = reinterpret_cast<void *>(UINTPTR_C(0xbeef0000));
    CheckStatus("gradient identity mismatch", TileXRMoonEpReduceGradV2(&launch, stream),
        TileXR::TILEXR_ERROR_PARA_CHECK_FAIL);
    gate.data = reinterpret_cast<void *>(UINTPTR_C(0x400000));
    Check(g_memsetCalls == 1 && g_launchCalls == 1,
        "pointer mismatches must fail before enqueuing work");

    g_launchReturn = -91;
    CheckStatus("launch failure", TileXRMoonEpReduceGradV2(&launch, stream), -91);
    Check(g_memsetCalls == 2 && g_launchCalls == 2 && g_syncCalls == 1,
        "launch failure must drain the enqueued status memset");
    g_syncReturn = 1;
    CheckStatus("launch failure drain failure",
        TileXRMoonEpReduceGradV2(&launch, stream), TileXR::TILEXR_ERROR_MKIRT);
    Check(g_memsetCalls == 3 && g_launchCalls == 3 && g_syncCalls == 2,
        "failed launch drain must be attempted exactly once");
    g_launchReturn = TileXR::TILEXR_SUCCESS;
    g_syncReturn = ACL_SUCCESS;

    g_profileView.infoDev = reinterpret_cast<GM_ADDR>(UINTPTR_C(0xdeadbeef));
    CheckStatus("profile mismatch", TileXRMoonEpReduceGradV2(&launch, stream),
        TileXR::TILEXR_ERROR_NOT_INITIALIZED);
    g_profileView.infoDev = reinterpret_cast<GM_ADDR>(UINTPTR_C(0x900000));

    CheckStatus("destroy", TileXRMoonEpReduceGradDestroyPreparedV2(prepared),
        TileXR::TILEXR_SUCCESS);
    Check(g_profileUnregisterCalls == 1,
        "destroy must unregister the persistent profile exactly once");
}

void TestPreparationValidationAndCapability()
{
    Reset();
    TileXRMoonEpPlanV1 plan = Plan();
    TileXRMoonEpTensorV1 gate = Gradient(
        reinterpret_cast<void *>(UINTPTR_C(0x400000)), 18, 1024);
    TileXRMoonEpTensorV1 up = Gradient(
        reinterpret_cast<void *>(UINTPTR_C(0x500000)), 18, 1024);
    TileXRMoonEpTensorV1 down = Gradient(
        reinterpret_cast<void *>(UINTPTR_C(0x600000)), 18, 1024);
    auto info = Query(&plan, &gate, &up, &down, TileXR::TILEXR_SUCCESS);
    auto prepare = PrepareArgs(&plan, &gate, &up, &down, info);
    TileXRMoonEpReduceGradPreparedV2 prepared = nullptr;

    prepare.sources[1].bytes -= sizeof(float);
    CheckStatus("source size", TileXRMoonEpReduceGradPrepareV2(&prepare, &prepared),
        TileXR::TILEXR_ERROR_PARA_CHECK_FAIL);
    prepare = PrepareArgs(&plan, &gate, &up, &down, info);
    prepare.sources[1].registrationBase = prepare.sources[1].data;
    prepare.sources[1].registrationBytes = prepare.sources[1].bytes - 1;
    CheckStatus("source outside registration",
        TileXRMoonEpReduceGradPrepareV2(&prepare, &prepared),
        TileXR::TILEXR_ERROR_PARA_CHECK_FAIL);
    prepare = PrepareArgs(&plan, &gate, &up, &down, info);
    prepare.workspace = reinterpret_cast<void *>(UINTPTR_C(0x20000200));
    CheckStatus("workspace alignment", TileXRMoonEpReduceGradPrepareV2(&prepare, &prepared),
        TileXR::TILEXR_ERROR_PARA_CHECK_FAIL);

    g_commArgs.extraFlag &= ~TileXR::ExtraFlag::UDMA;
    (void)Query(&plan, &gate, &up, &down, TileXR::TILEXR_ERROR_NOT_SUPPORT);
    g_commArgs.extraFlag |= TileXR::ExtraFlag::UDMA;
    g_qpCount = 2;
    (void)Query(&plan, &gate, &up, &down, TileXR::TILEXR_ERROR_NOT_SUPPORT);
    g_qpCount = 48;
    (void)Query(&plan, &gate, &up, &down, TileXR::TILEXR_ERROR_NOT_SUPPORT);
    g_commArgs.extraFlag |= TileXR::ExtraFlag::UDMA_SHARED_QP;
    info = Query(&plan, &gate, &up, &down, TileXR::TILEXR_SUCCESS);
    Check(info.qpCount == 3,
        "shared-domain query must report only the three active ReduceGrad lanes");
    prepare = PrepareArgs(&plan, &gate, &up, &down, info);
    CheckStatus("shared-domain prepare",
        TileXRMoonEpReduceGradPrepareV2(&prepare, &prepared),
        TileXR::TILEXR_SUCCESS);
    Check(g_profileDesc.qpBindingCount == 48,
        "persistent profile must bind every transport QP in the shared domain");
    Check(g_profileDesc.qpBindings[0].remoteRegion == 1 &&
        g_profileDesc.qpBindings[1].remoteRegion == 2 &&
        g_profileDesc.qpBindings[16].remoteRegion == 3,
        "active shared-domain QPs must bind gate/up/down to physical QPs 0/1/16");
    CheckStatus("shared-domain destroy",
        TileXRMoonEpReduceGradDestroyPreparedV2(prepared), TileXR::TILEXR_SUCCESS);
    prepared = nullptr;
    g_qpCount = 49;
    (void)Query(&plan, &gate, &up, &down, TileXR::TILEXR_ERROR_NOT_SUPPORT);
    Check(g_qpCountCalls == 9,
        "UDMA-capable multi-rank queries must validate the current hardware QP count");

    Reset();
    plan = Plan();
    gate = Gradient(reinterpret_cast<void *>(UINTPTR_C(0x400000)), 18, 1024);
    up = Gradient(reinterpret_cast<void *>(UINTPTR_C(0x500000)), 18, 1024);
    down = Gradient(reinterpret_cast<void *>(UINTPTR_C(0x600000)), 18, 1024);
    info = Query(&plan, &gate, &up, &down, TileXR::TILEXR_SUCCESS);
    prepare = PrepareArgs(&plan, &gate, &up, &down, info);
    g_profileRegisterReturn = -91;
    CheckStatus("profile registration failure",
        TileXRMoonEpReduceGradPrepareV2(&prepare, &prepared), -91);
    Check(prepared == nullptr && g_profileQueryCalls == 0,
        "failed registration must not expose a prepared handle");
}

void TestRanksBelowFourAreUnsupported()
{
    Reset(3);
    TileXRMoonEpPlanV1 plan = Plan(3);
    TileXRMoonEpTensorV1 gate = Gradient(
        reinterpret_cast<void *>(UINTPTR_C(0x400000)), 16, 1024);
    TileXRMoonEpTensorV1 up = Gradient(
        reinterpret_cast<void *>(UINTPTR_C(0x500000)), 16, 1024);
    TileXRMoonEpTensorV1 down = Gradient(
        reinterpret_cast<void *>(UINTPTR_C(0x600000)), 16, 1024);
    (void)Query(&plan, &gate, &up, &down, TileXR::TILEXR_ERROR_NOT_SUPPORT);
    Check(g_qpCountCalls == 0 && g_profileRegisterCalls == 0,
        "rank counts below four must fail before UDMA profile work");

    Reset(4);
    plan = Plan(4);
    gate = Gradient(reinterpret_cast<void *>(UINTPTR_C(0x400000)), 10, 1024);
    up = Gradient(reinterpret_cast<void *>(UINTPTR_C(0x500000)), 10, 1024);
    down = Gradient(reinterpret_cast<void *>(UINTPTR_C(0x600000)), 10, 1024);
    (void)Query(&plan, &gate, &up, &down, TileXR::TILEXR_SUCCESS);
    Check(g_qpCountCalls == 1,
        "four ranks must reach UDMA capability validation");
}

void TestCrossNodeBarrierProfileBinding()
{
    Reset(16);
    g_commArgs.localRankSize = 8;
    g_qpCount = 48;
    g_commArgs.extraFlag |= TileXR::ExtraFlag::UDMA_SHARED_QP;
    TileXRMoonEpPlanV1 plan = Plan(16);
    TileXRMoonEpTensorV1 gate = Gradient(
        reinterpret_cast<void *>(UINTPTR_C(0x400000)), 34, 1024);
    TileXRMoonEpTensorV1 up = Gradient(
        reinterpret_cast<void *>(UINTPTR_C(0x500000)), 34, 1024);
    TileXRMoonEpTensorV1 down = Gradient(
        reinterpret_cast<void *>(UINTPTR_C(0x600000)), 34, 1024);
    const auto info = Query(&plan, &gate, &up, &down, TileXR::TILEXR_SUCCESS);
    Check(info.qpCount == 3,
        "48-QP cross-node query must retain one lane per projection");
    auto prepare = PrepareArgs(&plan, &gate, &up, &down, info);
    TileXRMoonEpReduceGradPreparedV2 prepared = nullptr;
    CheckStatus("cross-node prepare",
        TileXRMoonEpReduceGradPrepareV2(&prepare, &prepared),
        TileXR::TILEXR_SUCCESS);
    Check(prepared != nullptr && g_profileDesc.qpBindingCount == 48 &&
        g_profileDesc.qpBindings[47].localRegion == 0 &&
        g_profileDesc.qpBindings[47].remoteRegion == 0,
        "cross-node barrier QP must bind staging to staging");
    Check(g_profileDesc.qpBindings[0].remoteRegion == 1 &&
        g_profileDesc.qpBindings[1].remoteRegion == 2 &&
        g_profileDesc.qpBindings[16].remoteRegion == 3,
        "cross-node projection QPs must retain the 0/1/16 mapping");
    CheckStatus("cross-node destroy",
        TileXRMoonEpReduceGradDestroyPreparedV2(prepared),
        TileXR::TILEXR_SUCCESS);

    Reset(16);
    g_commArgs.localRankSize = 8;
    g_qpCount = 3;
    plan = Plan(16);
    gate = Gradient(reinterpret_cast<void *>(UINTPTR_C(0x400000)), 34, 1024);
    up = Gradient(reinterpret_cast<void *>(UINTPTR_C(0x500000)), 34, 1024);
    down = Gradient(reinterpret_cast<void *>(UINTPTR_C(0x600000)), 34, 1024);
    (void)Query(&plan, &gate, &up, &down, TileXR::TILEXR_ERROR_NOT_SUPPORT);
}

void TestSlotsMayExceedExpertsPerRank()
{
    Reset();
    TileXRMoonEpPlanV1 plan = Plan(8, 8, 14);
    TileXRMoonEpTensorV1 gate = Gradient(
        reinterpret_cast<void *>(UINTPTR_C(0x400000)), 78, 1024);
    TileXRMoonEpTensorV1 up = Gradient(
        reinterpret_cast<void *>(UINTPTR_C(0x500000)), 78, 1024);
    TileXRMoonEpTensorV1 down = Gradient(
        reinterpret_cast<void *>(UINTPTR_C(0x600000)), 78, 1024);
    const auto info = Query(&plan, &gate, &up, &down, TileXR::TILEXR_SUCCESS);
    Check(info.workspaceBytes > 0,
        "workspace query must accept native dedicated-suite B greater than E/R");

    plan.b = std::numeric_limits<int32_t>::max() / plan.r + 1;
    (void)Query(&plan, &gate, &up, &down, TileXR::TILEXR_ERROR_PARA_CHECK_FAIL);
}

} // namespace

extern "C" int TileXRGetCommArgsHost(TileXRCommPtr, TileXR::CommArgs *&commArgs)
{
    commArgs = &g_commArgs;
    return TileXR::TILEXR_SUCCESS;
}

extern "C" int TileXRGetCommArgsDev(TileXRCommPtr, GM_ADDR &commArgs)
{
    commArgs = g_commDev;
    return TileXR::TILEXR_SUCCESS;
}

extern "C" int TileXRUDMAGetQpCount(TileXRCommPtr, uint32_t *qpCount)
{
    ++g_qpCountCalls;
    if (qpCount != nullptr) {
        *qpCount = g_qpCount;
    }
    return g_qpCountReturn;
}

extern "C" int TileXRUDMAProfileRegister(TileXRCommPtr,
    const TileXR::TileXRUDMAProfileDesc *desc, TileXRUDMAProfileHandle *handle)
{
    ++g_profileRegisterCalls;
    if (g_profileRegisterReturn != TileXR::TILEXR_SUCCESS) {
        return g_profileRegisterReturn;
    }
    g_profileDesc = *desc;
    *handle = 7;
    g_profileRegistry = TileXR::TileXRUDMAProfileRegistry {};
    g_profileRegistry.rankSize = static_cast<uint32_t>(g_commArgs.rankSize);
    g_profileRegistry.regionCount = desc->regionCount;
    g_profileRegistry.qpCount = desc->qpBindingCount;
    for (uint32_t qp = 0; qp < desc->qpBindingCount; ++qp) {
        g_profileRegistry.qpBindings[qp] = desc->qpBindings[qp];
    }
    for (int rank = 0; rank < g_commArgs.rankSize; ++rank) {
        for (uint32_t region = 0; region < desc->regionCount; ++region) {
            g_profileRegistry.regions[static_cast<size_t>(rank) *
                TileXR::TILEXR_UDMA_PROFILE_MAX_REGIONS + region] = desc->regions[region];
        }
    }
    g_profileView = TileXR::TileXRUDMAProfileView {};
    g_profileView.rankSize = static_cast<uint32_t>(g_commArgs.rankSize);
    g_profileView.regionCount = desc->regionCount;
    g_profileView.qpCount = desc->qpBindingCount;
    g_profileView.infoDev = reinterpret_cast<GM_ADDR>(UINTPTR_C(0x900000));
    g_profileView.registryDev = reinterpret_cast<GM_ADDR>(UINTPTR_C(0xa00000));
    g_profileView.registryHost = &g_profileRegistry;
    return TileXR::TILEXR_SUCCESS;
}

extern "C" int TileXRUDMAProfileQuery(TileXRCommPtr, TileXRUDMAProfileHandle,
    TileXR::TileXRUDMAProfileView *view)
{
    ++g_profileQueryCalls;
    if (g_profileQueryReturn == TileXR::TILEXR_SUCCESS && view != nullptr) {
        *view = g_profileView;
    }
    return g_profileQueryReturn;
}

extern "C" int TileXRUDMAProfileUnregister(TileXRCommPtr, TileXRUDMAProfileHandle)
{
    ++g_profileUnregisterCalls;
    return g_profileUnregisterReturn;
}

extern "C" aclError aclrtMemsetAsync(void *, size_t, int32_t, size_t, aclrtStream)
{
    ++g_memsetCalls;
    return ACL_SUCCESS;
}

extern "C" aclError aclrtMemcpyAsync(void *, size_t, const void *, size_t,
    aclrtMemcpyKind, aclrtStream)
{
    return ACL_SUCCESS;
}

extern "C" aclError aclrtSynchronizeStream(aclrtStream)
{
    ++g_syncCalls;
    return g_syncReturn;
}

namespace TileXRMoonEp {

int TileXRMoonEpLaunchReduceGradKernel(const ReduceGradLaunchParams &,
    const ReduceGradPreparedContext &)
{
    ++g_launchCalls;
    return g_launchReturn;
}

} // namespace TileXRMoonEp

int main()
{
    TestPreparedLifecycleAndHotLaunch();
    TestPreparationValidationAndCapability();
    TestRanksBelowFourAreUnsupported();
    TestCrossNodeBarrierProfileBinding();
    TestSlotsMayExceedExpertsPerRank();
    return g_failures == 0 ? 0 : 1;
}
