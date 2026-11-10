/*
 * 校验 MoonEP 公共 stage 参数，转发到 Planner、Dispatch、Prefetch、Combine 和 ReduceGrad Host 实现；不持有调用者的 tensor buffer。
 */
#include "tilexr_moonep.h"

#include <cstddef>
#include <cstdint>
#include <limits>

#include "acl/acl_rt.h"
#include "tilexr_api.h"
#include "tilexr_moonep_planner.h"

namespace TileXRMoonEp {

int TileXRMoonEpRunDispatchV1(
    const TileXRMoonEpDispatchArgsV1 *args, aclrtStream stream);
int TileXRMoonEpQueryDispatchUrmaWorkspace(TileXRCommPtr comm, int64_t s,
    int64_t k, int64_t h, uint32_t hiddenDtype, uint64_t *workspaceBytes,
    uint64_t *workspaceAlignment);
int TileXRMoonEpRunDispatchUrmaV1(
    const TileXRMoonEpDispatchArgsV1 *args, aclrtStream stream);
int TileXRMoonEpRunDispatchUrmaV2(
    const TileXRMoonEpDispatchArgsV2 *args, aclrtStream stream);
int TileXRMoonEpRunPrefetchWeightV1(
    const TileXRMoonEpPrefetchWeightArgsV1 *args, aclrtStream stream);
} // namespace TileXRMoonEp

namespace {

const uint64_t kNativeStagesV1 = static_cast<uint64_t>(TILEXR_MOONEP_STAGE_PLANNING) |
    static_cast<uint64_t>(TILEXR_MOONEP_STAGE_DISPATCH) |
    static_cast<uint64_t>(TILEXR_MOONEP_STAGE_PREFETCH_WEIGHT);
const uint64_t kStubStagesV1 = static_cast<uint64_t>(TILEXR_MOONEP_STAGE_REDUCE_GRAD);
const uint64_t kNativeStagesV2 = kNativeStagesV1 |
    static_cast<uint64_t>(TILEXR_MOONEP_STAGE_COMBINE) |
    static_cast<uint64_t>(TILEXR_MOONEP_STAGE_REDUCE_GRAD);
const uint64_t kStubStagesV2 = 0;

bool CheckedMultiply(uint64_t lhs, uint64_t rhs, uint64_t *result)
{
    if (result == nullptr || (lhs != 0 && rhs > std::numeric_limits<uint64_t>::max() / lhs)) {
        return false;
    }
    *result = lhs * rhs;
    return true;
}

bool TensorHeaderValid(const TileXRMoonEpTensorV1 *tensor)
{
    if (tensor == nullptr || tensor->structSize < sizeof(*tensor) ||
        tensor->abiVersion != TILEXR_MOONEP_ABI_VERSION_V1 || tensor->data == nullptr ||
        tensor->elementCount == 0 || tensor->rank == 0 ||
        tensor->rank > TILEXR_MOONEP_MAX_TENSOR_RANK) {
        return false;
    }
    uint64_t elements = 1;
    for (uint32_t dim = 0; dim < TILEXR_MOONEP_MAX_TENSOR_RANK; ++dim) {
        if (dim < tensor->rank) {
            if (tensor->shape[dim] <= 0 ||
                !CheckedMultiply(elements, static_cast<uint64_t>(tensor->shape[dim]), &elements)) {
                return false;
            }
        } else if (tensor->shape[dim] != 0) {
            return false;
        }
    }
    return tensor->elementCount == elements;
}

bool Int32Tensor1D(const TileXRMoonEpTensorV1 *tensor, int64_t dim0)
{
    return TensorHeaderValid(tensor) && tensor->dtype == TILEXR_MOONEP_DTYPE_INT32 &&
        tensor->rank == 1 && tensor->shape[0] == dim0;
}

bool Int32Tensor2D(const TileXRMoonEpTensorV1 *tensor, int64_t dim0, int64_t dim1)
{
    return TensorHeaderValid(tensor) && tensor->dtype == TILEXR_MOONEP_DTYPE_INT32 &&
        tensor->rank == 2 && tensor->shape[0] == dim0 && tensor->shape[1] == dim1;
}

bool TensorBytes(const TileXRMoonEpTensorV1 *tensor, size_t *bytes)
{
    if (bytes == nullptr || !TensorHeaderValid(tensor)) {
        return false;
    }
    uint64_t elementBytes = 0;
    switch (tensor->dtype) {
        case TILEXR_MOONEP_DTYPE_INT32:
        case TILEXR_MOONEP_DTYPE_FLOAT32:
            elementBytes = 4;
            break;
        case TILEXR_MOONEP_DTYPE_FLOAT16:
        case TILEXR_MOONEP_DTYPE_BFLOAT16:
            elementBytes = 2;
            break;
        default:
            return false;
    }
    uint64_t total = 0;
    if (!CheckedMultiply(tensor->elementCount, elementBytes, &total) ||
        total > static_cast<uint64_t>(std::numeric_limits<size_t>::max())) {
        return false;
    }
    *bytes = static_cast<size_t>(total);
    return true;
}

bool TensorDescriptorsEqual(const TileXRMoonEpTensorV1 &lhs,
    const TileXRMoonEpTensorV1 &rhs)
{
    if (lhs.elementCount != rhs.elementCount || lhs.dtype != rhs.dtype ||
        lhs.rank != rhs.rank) {
        return false;
    }
    for (uint32_t dim = 0; dim < TILEXR_MOONEP_MAX_TENSOR_RANK; ++dim) {
        if (lhs.shape[dim] != rhs.shape[dim]) {
            return false;
        }
    }
    return true;
}

bool PlanValid(const TileXRMoonEpPlanV1 *plan, int64_t *s, int64_t *tokenPadding)
{
    if (s == nullptr || tokenPadding == nullptr || plan == nullptr ||
        plan->structSize < sizeof(*plan) ||
        plan->abiVersion != TILEXR_MOONEP_ABI_VERSION_V1 || plan->n <= 0 ||
        plan->r <= 0 || plan->r > 128 || plan->e <= 0 || plan->b <= 0 ||
        plan->nvS < plan->n || plan->k <= 0 || plan->k > 32 ||
        plan->n % plan->k != 0 || plan->e % plan->r != 0 ||
        plan->b > plan->e / plan->r || plan->dst == nullptr ||
        plan->e > std::numeric_limits<int64_t>::max() - plan->b ||
        plan->expertsToCopy == nullptr || plan->zeroFillRanges == nullptr ||
        plan->remoteStats == nullptr || plan->dupGroups == nullptr ||
        plan->dupLoffs == nullptr || plan->dupCounts == nullptr || plan->status == nullptr) {
        return false;
    }
    uint64_t encodedCapacity = 0;
    if (!CheckedMultiply(static_cast<uint64_t>(plan->r),
            static_cast<uint64_t>(plan->nvS), &encodedCapacity) ||
        encodedCapacity > static_cast<uint64_t>(std::numeric_limits<int32_t>::max()) + 1U) {
        return false;
    }
    const int64_t expertsPerRank = plan->e / plan->r;
    uint64_t denominator = 0;
    if (!CheckedMultiply(UINT64_C(2), static_cast<uint64_t>(expertsPerRank),
            &denominator)) {
        return false;
    }
    const uint64_t paddingExtra = static_cast<uint64_t>(plan->nvS - plan->n);
    if (denominator == 0 || paddingExtra % denominator != 0) {
        return false;
    }
    *s = plan->n / plan->k;
    *tokenPadding = static_cast<int64_t>(paddingExtra / denominator + 1);
    return *s > 0 && *tokenPadding > 0;
}

int ValidateCommPlan(TileXRCommPtr comm, const TileXRMoonEpPlanV1 *plan,
    int64_t *s, int64_t *tokenPadding)
{
    if (comm == nullptr || !PlanValid(plan, s, tokenPadding)) {
        return TILEXR_MOONEP_ERROR_INVALID_ARGUMENT;
    }
    TileXR::CommArgs *commArgs = nullptr;
    const int ret = TileXRGetCommArgsHost(comm, commArgs);
    if (ret != TILEXR_MOONEP_SUCCESS) {
        return ret;
    }
    if (commArgs == nullptr || plan->r != static_cast<int64_t>(commArgs->rankSize)) {
        return TILEXR_MOONEP_ERROR_INVALID_ARGUMENT;
    }
    return TILEXR_MOONEP_SUCCESS;
}

int RunReduceGradV1Stub(const TileXRMoonEpReduceGradArgsV1 *args,
    aclrtStream stream)
{
    if (args == nullptr || args->structSize < sizeof(*args) ||
        args->abiVersion != TILEXR_MOONEP_ABI_VERSION_V1 || args->comm == nullptr ||
        args->flags != TILEXR_MOONEP_FLAG_NONE || stream == nullptr ||
        args->plan == nullptr || args->input == nullptr || args->output == nullptr) {
        return TILEXR_MOONEP_ERROR_INVALID_ARGUMENT;
    }

    int64_t s = 0;
    int64_t tokenPadding = 0;
    const int commRet = ValidateCommPlan(args->comm, args->plan, &s, &tokenPadding);
    if (commRet != TILEXR_MOONEP_SUCCESS) {
        return commRet;
    }

    size_t inputBytes = 0;
    size_t outputBytes = 0;
    if (!TensorBytes(args->input, &inputBytes) ||
        !TensorBytes(args->output, &outputBytes) ||
        args->input->dtype != args->output->dtype ||
        args->input->elementCount != args->output->elementCount) {
        return TILEXR_MOONEP_ERROR_INVALID_ARGUMENT;
    }
    if (args->input->data == args->output->data) {
        return TensorDescriptorsEqual(*args->input, *args->output) ?
            TILEXR_MOONEP_SUCCESS : TILEXR_MOONEP_ERROR_INVALID_ARGUMENT;
    }

    aclError ret = aclrtMemsetAsync(
        args->output->data, outputBytes, 0, outputBytes, stream);
    if (ret != ACL_SUCCESS) {
        return TILEXR_MOONEP_ERROR_INTERNAL;
    }
    ret = aclrtMemcpyAsync(args->output->data, outputBytes, args->input->data,
        inputBytes, ACL_MEMCPY_DEVICE_TO_DEVICE, stream);
    return ret == ACL_SUCCESS ? TILEXR_MOONEP_SUCCESS : TILEXR_MOONEP_ERROR_INTERNAL;
}

} // namespace

extern "C" uint32_t TileXRMoonEpGetAbiVersion(void)
{
    return TILEXR_MOONEP_ABI_VERSION_V2;
}

extern "C" int TileXRMoonEpGetCapabilitiesV2(uint64_t *nativeStages,
    uint64_t *stubStages)
{
    if (nativeStages == nullptr || stubStages == nullptr) {
        return TILEXR_MOONEP_ERROR_INVALID_ARGUMENT;
    }
    *nativeStages = kNativeStagesV2;
    *stubStages = kStubStagesV2;
    return TILEXR_MOONEP_SUCCESS;
}

extern "C" int TileXRMoonEpGetCapabilitiesV1(uint64_t *nativeStages, uint64_t *stubStages)
{
    if (nativeStages == nullptr || stubStages == nullptr) {
        return TILEXR_MOONEP_ERROR_INVALID_ARGUMENT;
    }
    *nativeStages = kNativeStagesV1;
    *stubStages = kStubStagesV1;
    return TILEXR_MOONEP_SUCCESS;
}

extern "C" int TileXRMoonEpPlanningGetWorkspaceSizeV1(TileXRCommPtr comm, int64_t s,
    int64_t k, int64_t e, int64_t b, int64_t tokenPadding,
    uint64_t *workspaceBytes, int64_t *nvS)
{
    if (comm == nullptr || s <= 0 || k <= 0 || e <= 0 || b <= 0 || tokenPadding <= 0 ||
        workspaceBytes == nullptr || nvS == nullptr) {
        return TILEXR_MOONEP_ERROR_INVALID_ARGUMENT;
    }
    return TileXRMoonEpPlannerGetWorkspaceSizeV3(
        comm, s, k, e, b, tokenPadding, workspaceBytes, nvS);
}

extern "C" int TileXRMoonEpPlanningV1(const TileXRMoonEpPlanningArgsV1 *args,
    aclrtStream stream)
{
    if (args == nullptr || args->structSize < sizeof(*args) ||
        args->abiVersion != TILEXR_MOONEP_ABI_VERSION_V1 || args->comm == nullptr ||
        args->topkExperts == nullptr || args->tokensPerExpert == nullptr ||
        args->workspace == nullptr || args->workspaceBytes == 0 ||
        args->cuSeqlens == nullptr || args->plan == nullptr || args->waitIterations == 0 ||
        args->flags != TILEXR_MOONEP_FLAG_NONE || stream == nullptr) {
        return TILEXR_MOONEP_ERROR_INVALID_ARGUMENT;
    }

    int64_t s = 0;
    int64_t tokenPadding = 0;
    const int commRet = ValidateCommPlan(args->comm, args->plan, &s, &tokenPadding);
    if (commRet != TILEXR_MOONEP_SUCCESS) {
        return commRet;
    }
    if (!Int32Tensor2D(args->topkExperts, s, args->plan->k) ||
        !Int32Tensor1D(args->tokensPerExpert, args->plan->e) ||
        !Int32Tensor1D(args->cuSeqlens, args->plan->e + args->plan->b)) {
        return TILEXR_MOONEP_ERROR_INVALID_ARGUMENT;
    }

    uint64_t requiredWorkspaceBytes = 0;
    int64_t expectedNvS = 0;
    int ret = TileXRMoonEpPlanningGetWorkspaceSizeV1(args->comm, s, args->plan->k,
        args->plan->e, args->plan->b, tokenPadding,
        &requiredWorkspaceBytes, &expectedNvS);
    if (ret != TILEXR_MOONEP_SUCCESS) {
        return ret;
    }
    if (requiredWorkspaceBytes == 0 || args->workspaceBytes < requiredWorkspaceBytes ||
        expectedNvS != args->plan->nvS) {
        return TILEXR_MOONEP_ERROR_INVALID_ARGUMENT;
    }

    return TileXRMoonEpPlannerV3(
        static_cast<const int32_t *>(args->topkExperts->data),
        static_cast<const int32_t *>(args->tokensPerExpert->data), args->comm,
        s, args->plan->k, args->plan->e, args->plan->b, tokenPadding,
        args->workspace, args->workspaceBytes,
        static_cast<int32_t *>(args->plan->dst),
        static_cast<int32_t *>(args->cuSeqlens->data),
        static_cast<int32_t *>(args->plan->expertsToCopy),
        static_cast<int32_t *>(args->plan->zeroFillRanges),
        static_cast<int32_t *>(args->plan->remoteStats),
        static_cast<int32_t *>(args->plan->dupCounts),
        static_cast<int32_t *>(args->plan->status),
        args->waitIterations, stream);
}

extern "C" int TileXRMoonEpDispatchV1(const TileXRMoonEpDispatchArgsV1 *args,
    aclrtStream stream)
{
    const std::size_t legacyArgsBytes = offsetof(
        TileXRMoonEpDispatchArgsV1, registeredWorkspace);
    const std::size_t urmaArgsBytes = sizeof(TileXRMoonEpDispatchArgsV1);
    if (args != nullptr && args->structSize >= urmaArgsBytes &&
        (args->registeredWorkspace != nullptr ||
            args->registeredWorkspaceBytes != 0)) {
        return TileXRMoonEp::TileXRMoonEpRunDispatchUrmaV1(args, stream);
    }
    if (args != nullptr && args->structSize < legacyArgsBytes) {
        return TILEXR_MOONEP_ERROR_INVALID_ARGUMENT;
    }
    return TileXRMoonEp::TileXRMoonEpRunDispatchV1(args, stream);
}

extern "C" int TileXRMoonEpDispatchGetWorkspaceSizeV1(TileXRCommPtr comm,
    int64_t s, int64_t k, int64_t h, uint32_t hiddenDtype,
    uint64_t *workspaceBytes, uint64_t *workspaceAlignment)
{
    return TileXRMoonEp::TileXRMoonEpQueryDispatchUrmaWorkspace(
        comm, s, k, h, hiddenDtype, workspaceBytes, workspaceAlignment);
}

extern "C" int TileXRMoonEpDispatchGetWorkspaceSizeV2(TileXRCommPtr comm,
    int64_t s, int64_t k, int64_t h, uint32_t hiddenDtype,
    uint64_t *workspaceBytes, uint64_t *workspaceAlignment)
{
    return TileXRMoonEp::TileXRMoonEpQueryDispatchUrmaWorkspace(
        comm, s, k, h, hiddenDtype, workspaceBytes, workspaceAlignment);
}

extern "C" int TileXRMoonEpDispatchV2(const TileXRMoonEpDispatchArgsV2 *args,
    aclrtStream stream)
{
    if (args == nullptr || args->structSize < sizeof(*args) ||
        args->abiVersion != TILEXR_MOONEP_ABI_VERSION_V2 ||
        args->registeredWorkspace == nullptr ||
        args->registeredWorkspaceBytes == 0) {
        return TILEXR_MOONEP_ERROR_INVALID_ARGUMENT;
    }
    return TileXRMoonEp::TileXRMoonEpRunDispatchUrmaV2(args, stream);
}

extern "C" int TileXRMoonEpPrefetchWeightV1(
    const TileXRMoonEpPrefetchWeightArgsV1 *args, aclrtStream stream)
{
    return TileXRMoonEp::TileXRMoonEpRunPrefetchWeightV1(args, stream);
}

extern "C" int TileXRMoonEpReduceGradV1(const TileXRMoonEpReduceGradArgsV1 *args,
    aclrtStream stream)
{
    return RunReduceGradV1Stub(args, stream);
}
