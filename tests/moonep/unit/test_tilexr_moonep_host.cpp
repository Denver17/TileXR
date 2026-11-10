/*
 * 验证 MoonEP 的布局、调用或诊断合同；直接覆盖对应 src/moonep 与 tools/moonep 接口，硬件数据面另行验收。
 */
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string>

#include "acl/acl_rt.h"
#include "tilexr_api.h"
#include "tilexr_moonep.h"

namespace {

int failures = 0;
int commReturn = TILEXR_MOONEP_SUCCESS;
int queryReturn = TILEXR_MOONEP_SUCCESS;
int plannerReturn = TILEXR_MOONEP_SUCCESS;
int dispatchReturn = TILEXR_MOONEP_SUCCESS;
int dispatchUrmaReturn = TILEXR_MOONEP_SUCCESS;
int prefetchReturn = TILEXR_MOONEP_SUCCESS;
int queryCalls = 0;
int plannerCalls = 0;
int dispatchCalls = 0;
int dispatchUrmaCalls = 0;
int dispatchUrmaV2Calls = 0;
int dispatchWorkspaceQueryCalls = 0;
int prefetchCalls = 0;
int memsetCalls = 0;
uint64_t queryWorkspaceBytes = 512;
int64_t queryNvS = 12;
TileXR::CommArgs commArgs {};
const TileXRMoonEpDispatchArgsV1 *seenDispatch = nullptr;
const TileXRMoonEpDispatchArgsV1 *seenDispatchUrma = nullptr;
uint64_t seenDispatchUrmaFlags = UINT64_MAX;
aclrtStream seenDispatchStream = nullptr;

struct QueryCall {
    TileXRCommPtr comm = nullptr;
    int64_t s = 0;
    int64_t k = 0;
    int64_t e = 0;
    int64_t b = 0;
    int64_t tokenPadding = 0;
} queryCall;

struct PlannerCall {
    const int32_t *topk = nullptr;
    const int32_t *tokensPerExpert = nullptr;
    TileXRCommPtr comm = nullptr;
    int64_t s = 0;
    int64_t k = 0;
    int64_t e = 0;
    int64_t b = 0;
    int64_t tokenPadding = 0;
    void *workspace = nullptr;
    uint64_t workspaceBytes = 0;
    int32_t *dst = nullptr;
    int32_t *cuSeqlens = nullptr;
    int32_t *expertsToCopy = nullptr;
    int32_t *zeroFillRanges = nullptr;
    int32_t *remoteStats = nullptr;
    int32_t *dupCounts = nullptr;
    int32_t *status = nullptr;
    uint64_t waitIterations = 0;
    aclrtStream stream = nullptr;
} plannerCall;

void Reset()
{
    commReturn = queryReturn = plannerReturn = dispatchReturn = dispatchUrmaReturn =
        TILEXR_MOONEP_SUCCESS;
    prefetchReturn = TILEXR_MOONEP_SUCCESS;
    queryCalls = plannerCalls = dispatchCalls = dispatchUrmaCalls =
        dispatchUrmaV2Calls =
        dispatchWorkspaceQueryCalls = prefetchCalls = 0;
    memsetCalls = 0;
    queryWorkspaceBytes = 512;
    queryNvS = 12;
    commArgs = TileXR::CommArgs {};
    commArgs.rank = 0;
    commArgs.rankSize = 2;
    queryCall = QueryCall {};
    plannerCall = PlannerCall {};
    seenDispatch = nullptr;
    seenDispatchUrma = nullptr;
    seenDispatchUrmaFlags = UINT64_MAX;
    seenDispatchStream = nullptr;
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

TileXRMoonEpPlanV1 ValidPlan()
{
    TileXRMoonEpPlanV1 plan {};
    plan.structSize = sizeof(plan);
    plan.abiVersion = TILEXR_MOONEP_ABI_VERSION_V1;
    plan.n = 4;
    plan.r = 2;
    plan.e = 8;
    plan.b = 2;
    plan.nvS = 12;
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

TileXRMoonEpPlanningArgsV1 PlanningArgs(TileXRMoonEpPlanV1 *plan,
    const TileXRMoonEpTensorV1 *topk, const TileXRMoonEpTensorV1 *tpe,
    TileXRMoonEpTensorV1 *cu)
{
    TileXRMoonEpPlanningArgsV1 args {};
    args.structSize = sizeof(args);
    args.abiVersion = TILEXR_MOONEP_ABI_VERSION_V1;
    args.comm = reinterpret_cast<TileXRCommPtr>(uintptr_t {0x1000});
    args.topkExperts = topk;
    args.tokensPerExpert = tpe;
    args.workspace = reinterpret_cast<void *>(uintptr_t {0x2000});
    args.workspaceBytes = 512;
    args.cuSeqlens = cu;
    args.plan = plan;
    args.waitIterations = 1000;
    return args;
}

void TestAbiAndCapabilities()
{
    Reset();
    Check(TileXRMoonEpGetAbiVersion() == TILEXR_MOONEP_ABI_VERSION_V2,
        "ABI version mismatch");
    uint64_t nativeStages = 0;
    uint64_t stubStages = 0;
    CheckStatus("capabilities", TileXRMoonEpGetCapabilitiesV1(&nativeStages, &stubStages),
        TILEXR_MOONEP_SUCCESS);
    Check(nativeStages == (TILEXR_MOONEP_STAGE_PLANNING |
        TILEXR_MOONEP_STAGE_DISPATCH | TILEXR_MOONEP_STAGE_PREFETCH_WEIGHT),
        "native capability mask mismatch");
    Check(stubStages == TILEXR_MOONEP_STAGE_REDUCE_GRAD,
        "stub capability mask mismatch");
    CheckStatus("V2 capabilities", TileXRMoonEpGetCapabilitiesV2(
        &nativeStages, &stubStages), TILEXR_MOONEP_SUCCESS);
    Check(nativeStages == (TILEXR_MOONEP_STAGE_PLANNING |
        TILEXR_MOONEP_STAGE_DISPATCH | TILEXR_MOONEP_STAGE_PREFETCH_WEIGHT |
        TILEXR_MOONEP_STAGE_COMBINE | TILEXR_MOONEP_STAGE_REDUCE_GRAD),
        "V2 native capability mask mismatch");
    Check(stubStages == 0, "V2 stub capability mask mismatch");
}

void TestWorkspaceQuery()
{
    Reset();
    uint64_t workspaceBytes = 0;
    int64_t nvS = 0;
    TileXRCommPtr comm = reinterpret_cast<TileXRCommPtr>(uintptr_t {0x1000});
    CheckStatus("workspace query", TileXRMoonEpPlanningGetWorkspaceSizeV1(
        comm, 2, 2, 8, 2, 2, &workspaceBytes, &nvS), TILEXR_MOONEP_SUCCESS);
    Check(queryCalls == 1 && queryCall.comm == comm && queryCall.s == 2 &&
        queryCall.k == 2 && queryCall.e == 8 && queryCall.b == 2 &&
        queryCall.tokenPadding == 2, "workspace query arguments mismatch");
    Check(workspaceBytes == queryWorkspaceBytes && nvS == queryNvS,
        "workspace query outputs mismatch");
    CheckStatus("invalid workspace query", TileXRMoonEpPlanningGetWorkspaceSizeV1(
        comm, 2, 2, 8, 0, 2, &workspaceBytes, &nvS),
        TILEXR_MOONEP_ERROR_INVALID_ARGUMENT);

    uint64_t dispatchBytes = 0;
    uint64_t dispatchAlignment = 0;
    CheckStatus("dispatch workspace query", TileXRMoonEpDispatchGetWorkspaceSizeV1(
        comm, 2, 2, 64, TILEXR_MOONEP_DTYPE_BFLOAT16,
        &dispatchBytes, &dispatchAlignment), TILEXR_MOONEP_SUCCESS);
    Check(dispatchWorkspaceQueryCalls == 1 && dispatchBytes == 4096 &&
        dispatchAlignment == 2097152, "dispatch workspace query delegation mismatch");
    CheckStatus("dispatch V2 workspace query", TileXRMoonEpDispatchGetWorkspaceSizeV2(
        comm, 2, 2, 64, TILEXR_MOONEP_DTYPE_BFLOAT16,
        &dispatchBytes, &dispatchAlignment), TILEXR_MOONEP_SUCCESS);
    Check(dispatchWorkspaceQueryCalls == 2,
        "dispatch V2 workspace query delegation mismatch");
}

void TestPlanningDelegation()
{
    Reset();
    TileXRMoonEpPlanV1 plan = ValidPlan();
    TileXRMoonEpTensorV1 topk = Tensor(reinterpret_cast<void *>(uintptr_t {0x4000}),
        TILEXR_MOONEP_DTYPE_INT32, 2, 2, 2, 4);
    TileXRMoonEpTensorV1 tpe = Tensor(reinterpret_cast<void *>(uintptr_t {0x5000}),
        TILEXR_MOONEP_DTYPE_INT32, 1, 8, 0, 8);
    TileXRMoonEpTensorV1 cu = Tensor(reinterpret_cast<void *>(uintptr_t {0x6000}),
        TILEXR_MOONEP_DTYPE_INT32, 1, 10, 0, 10);
    TileXRMoonEpPlanningArgsV1 args = PlanningArgs(&plan, &topk, &tpe, &cu);
    aclrtStream stream = reinterpret_cast<aclrtStream>(uintptr_t {0x7000});

    CheckStatus("planning", TileXRMoonEpPlanningV1(&args, stream),
        TILEXR_MOONEP_SUCCESS);
    Check(queryCalls == 1 && plannerCalls == 1, "planning call counts mismatch");
    Check(plannerCall.topk == topk.data && plannerCall.tokensPerExpert == tpe.data &&
        plannerCall.comm == args.comm && plannerCall.s == 2 && plannerCall.k == 2 &&
        plannerCall.e == 8 && plannerCall.b == 2 && plannerCall.tokenPadding == 2 &&
        plannerCall.workspace == args.workspace &&
        plannerCall.workspaceBytes == args.workspaceBytes && plannerCall.dst == plan.dst &&
        plannerCall.cuSeqlens == cu.data &&
        plannerCall.expertsToCopy == plan.expertsToCopy &&
        plannerCall.zeroFillRanges == plan.zeroFillRanges &&
        plannerCall.remoteStats == plan.remoteStats && plannerCall.dupCounts == plan.dupCounts &&
        plannerCall.status == plan.status && plannerCall.waitIterations == args.waitIterations &&
        plannerCall.stream == stream, "Planner V3 delegation mismatch");

    plan.nvS = 13;
    CheckStatus("non-integral token padding", TileXRMoonEpPlanningV1(&args, stream),
        TILEXR_MOONEP_ERROR_INVALID_ARGUMENT);
    plan = ValidPlan();
    cu.shape[0] = 9;
    cu.elementCount = 9;
    CheckStatus("cu shape", TileXRMoonEpPlanningV1(&args, stream),
        TILEXR_MOONEP_ERROR_INVALID_ARGUMENT);
    cu.shape[0] = cu.elementCount = 10;
    args.workspaceBytes = 511;
    CheckStatus("short workspace", TileXRMoonEpPlanningV1(&args, stream),
        TILEXR_MOONEP_ERROR_INVALID_ARGUMENT);
    args.workspaceBytes = 512;
    commArgs.rankSize = 4;
    CheckStatus("rank size mismatch", TileXRMoonEpPlanningV1(&args, stream),
        TILEXR_MOONEP_ERROR_INVALID_ARGUMENT);

    commArgs.rankSize = 2;
    plan = ValidPlan();
    plan.e = std::numeric_limits<int64_t>::max() - 1;
    plan.b = 2;
    CheckStatus("expert plus slot overflow", TileXRMoonEpPlanningV1(&args, stream),
        TILEXR_MOONEP_ERROR_INVALID_ARGUMENT);
}

void TestStageDelegation()
{
    Reset();
    aclrtStream stream = reinterpret_cast<aclrtStream>(uintptr_t {0x7000});
    TileXRMoonEpDispatchArgsV1 dispatch {};
    dispatch.structSize = sizeof(dispatch);
    dispatch.abiVersion = TILEXR_MOONEP_ABI_VERSION_V1;
    CheckStatus("dispatch", TileXRMoonEpDispatchV1(&dispatch, stream), dispatchReturn);
    Check(dispatchCalls == 1 && seenDispatch == &dispatch && seenDispatchStream == stream,
        "dispatch delegation mismatch");

    dispatch.registeredWorkspace = reinterpret_cast<void *>(uintptr_t {0x800000});
    dispatch.registeredWorkspaceBytes = 2097152;
    CheckStatus("URMA dispatch", TileXRMoonEpDispatchV1(&dispatch, stream),
        dispatchUrmaReturn);
    Check(dispatchCalls == 1 && dispatchUrmaCalls == 1 &&
        seenDispatchUrma == &dispatch, "URMA dispatch selection mismatch");

    TileXRMoonEpPlanV1 plan = ValidPlan();
    dispatch.plan = &plan;
    dispatch.flags = TILEXR_MOONEP_FLAG_RESET_STATUS;
    CheckStatus("URMA dispatch reset routing", TileXRMoonEpDispatchV1(&dispatch, stream),
        dispatchUrmaReturn);
    Check(memsetCalls == 0 && dispatchUrmaCalls == 2 &&
        seenDispatchUrma == &dispatch &&
        seenDispatchUrmaFlags == TILEXR_MOONEP_FLAG_RESET_STATUS,
        "URMA dispatch reset flag must be delegated unchanged");
    dispatch.flags = TILEXR_MOONEP_FLAG_NONE;

    TileXRMoonEpDispatchArgsV2 dispatchV2 {};
    dispatchV2.structSize = sizeof(dispatchV2);
    dispatchV2.abiVersion = TILEXR_MOONEP_ABI_VERSION_V2;
    CheckStatus("V2 dispatch requires workspace",
        TileXRMoonEpDispatchV2(&dispatchV2, stream),
        TILEXR_MOONEP_ERROR_INVALID_ARGUMENT);
    Check(dispatchUrmaCalls == 2,
        "invalid V2 dispatch must not reach the URMA implementation");
    dispatchV2.registeredWorkspace =
        reinterpret_cast<void *>(uintptr_t {0x800000});
    dispatchV2.registeredWorkspaceBytes = 2097152;
    CheckStatus("V2 dispatch", TileXRMoonEpDispatchV2(&dispatchV2, stream),
        dispatchUrmaReturn);
    Check(dispatchUrmaCalls == 2 && dispatchUrmaV2Calls == 1 &&
        seenDispatchUrma == &dispatchV2,
        "V2 dispatch must select the URMA implementation");

    TileXRMoonEpPrefetchWeightArgsV1 prefetch {};
    prefetch.structSize = sizeof(prefetch);
    prefetch.abiVersion = TILEXR_MOONEP_ABI_VERSION_V1;
    prefetch.comm = reinterpret_cast<TileXRCommPtr>(uintptr_t {0x1000});
    prefetch.plan = &plan;
    CheckStatus("prefetch", TileXRMoonEpPrefetchWeightV1(&prefetch, stream), prefetchReturn);
    Check(prefetchCalls == 1, "prefetch delegation mismatch");

    TileXRMoonEpReduceGradArgsV1 reduce {};
    reduce.structSize = sizeof(reduce);
    reduce.abiVersion = TILEXR_MOONEP_ABI_VERSION_V1;
    reduce.comm = reinterpret_cast<TileXRCommPtr>(uintptr_t {0x1000});
    reduce.plan = &plan;
    TileXRMoonEpTensorV1 gradient = Tensor(
        reinterpret_cast<void *>(uintptr_t {0x8000}),
        TILEXR_MOONEP_DTYPE_FLOAT32, 2, 2, 2, 4);
    reduce.input = &gradient;
    reduce.output = &gradient;
    CheckStatus("reduce V1 stub", TileXRMoonEpReduceGradV1(&reduce, stream),
        TILEXR_MOONEP_SUCCESS);
}

} // namespace

extern "C" int TileXRGetCommArgsHost(TileXRCommPtr, TileXR::CommArgs *&args)
{
    args = commReturn == TILEXR_MOONEP_SUCCESS ? &commArgs : nullptr;
    return commReturn;
}

extern "C" int TileXRMoonEpPlannerGetWorkspaceSizeV3(TileXRCommPtr comm,
    int64_t s, int64_t k, int64_t e, int64_t b, int64_t tokenPadding,
    uint64_t *workspaceBytes, int64_t *nvS)
{
    ++queryCalls;
    queryCall = QueryCall {comm, s, k, e, b, tokenPadding};
    if (queryReturn == TILEXR_MOONEP_SUCCESS) {
        *workspaceBytes = queryWorkspaceBytes;
        *nvS = queryNvS;
    }
    return queryReturn;
}

extern "C" int TileXRMoonEpPlannerV3(const int32_t *topk, const int32_t *tpe,
    TileXRCommPtr comm, int64_t s, int64_t k, int64_t e, int64_t b,
    int64_t tokenPadding, void *workspace, uint64_t workspaceBytes, int32_t *dst,
    int32_t *cuSeqlens, int32_t *expertsToCopy, int32_t *zeroFillRanges,
    int32_t *remoteStats, int32_t *dupCounts, int32_t *status,
    uint64_t waitIterations, aclrtStream stream)
{
    ++plannerCalls;
    plannerCall = PlannerCall {topk, tpe, comm, s, k, e, b, tokenPadding,
        workspace, workspaceBytes, dst, cuSeqlens, expertsToCopy, zeroFillRanges,
        remoteStats, dupCounts, status, waitIterations, stream};
    return plannerReturn;
}

extern "C" aclError aclrtMemsetAsync(
    void *, size_t, int32_t, size_t, aclrtStream)
{
    ++memsetCalls;
    return ACL_SUCCESS;
}

extern "C" aclError aclrtMemcpyAsync(
    void *, size_t, const void *, size_t, aclrtMemcpyKind, aclrtStream)
{
    return ACL_SUCCESS;
}

namespace TileXRMoonEp {
int TileXRMoonEpRunDispatchV1(
    const TileXRMoonEpDispatchArgsV1 *args, aclrtStream stream)
{
    ++dispatchCalls;
    seenDispatch = args;
    seenDispatchStream = stream;
    return dispatchReturn;
}

int TileXRMoonEpQueryDispatchUrmaWorkspace(TileXRCommPtr, int64_t, int64_t,
    int64_t, uint32_t, uint64_t *workspaceBytes, uint64_t *workspaceAlignment)
{
    ++dispatchWorkspaceQueryCalls;
    *workspaceBytes = 4096;
    *workspaceAlignment = 2097152;
    return TILEXR_MOONEP_SUCCESS;
}

int TileXRMoonEpRunDispatchUrmaV1(
    const TileXRMoonEpDispatchArgsV1 *args, aclrtStream)
{
    ++dispatchUrmaCalls;
    seenDispatchUrma = args;
    seenDispatchUrmaFlags = args->flags;
    return dispatchUrmaReturn;
}

int TileXRMoonEpRunDispatchUrmaV2(
    const TileXRMoonEpDispatchArgsV2 *args, aclrtStream)
{
    ++dispatchUrmaV2Calls;
    seenDispatchUrma = args;
    return dispatchUrmaReturn;
}

int TileXRMoonEpRunPrefetchWeightV1(
    const TileXRMoonEpPrefetchWeightArgsV1 *, aclrtStream)
{
    ++prefetchCalls;
    return prefetchReturn;
}

} // namespace TileXRMoonEp

int main()
{
    TestAbiAndCapabilities();
    TestWorkspaceQuery();
    TestPlanningDelegation();
    TestStageDelegation();
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
