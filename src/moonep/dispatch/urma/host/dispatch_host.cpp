#include "dispatch_host.h"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>

#include "acl/acl_rt.h"
#include "comm_args.h"
#include "dispatch_launch.h"
#include "dispatch_layout.h"
#include "../common/dispatch_schedule.h"
#include "../common/dispatch_trace.h"
#include "../common/dispatch_wqe_batch.h"
#include "tilexr_api.h"
#include "tilexr_types.h"
#include "tilexr_udma_fullmesh.h"
#include "tilexr_udma_reg.h"
#include "tilexr_udma_types.h"

namespace TileXRMoonEp {
namespace {

constexpr char kDispatchPeerModeEnv[] =
    "TILEXR_MOONEP_DISPATCH_PEER_MODE";
constexpr char kDispatchGroupWidthEnv[] =
    "TILEXR_MOONEP_DISPATCH_GROUP_WIDTH";

struct DispatchPeerConfig {
    DispatchPeerMode mode = DispatchPeerMode::GroupCredit;
    uint32_t groupWidth = kDispatchDefaultGroupWidth;
};

bool ResolveDispatchPeerConfig(DispatchPeerConfig &config)
{
    const char *mode = std::getenv(kDispatchPeerModeEnv);
    if (mode == nullptr || mode[0] == '\0' ||
        std::strcmp(mode, "group_credit") == 0) {
        config.mode = DispatchPeerMode::GroupCredit;
    } else if (std::strcmp(mode, "group") == 0) {
        config.mode = DispatchPeerMode::Group;
    } else {
        return false;
    }

    const char *width = std::getenv(kDispatchGroupWidthEnv);
    if (width == nullptr || width[0] == '\0' || std::strcmp(width, "16") == 0) {
        config.groupWidth = kDispatchDefaultGroupWidth;
    } else if (std::strcmp(width, "8") == 0) {
        config.groupWidth = kDispatchValidationGroupWidth;
    } else {
        return false;
    }
    return true;
}

bool DispatchVectorBatchShapeSupported(uint64_t routeCount,
    uint64_t destinationCapacity, bool hasWeight)
{
    constexpr uint64_t vectorCompareMinElements = 256U / sizeof(int32_t);
    return routeCount >= vectorCompareMinElements &&
        routeCount <= UINT32_MAX &&
        destinationCapacity >= routeCount &&
        destinationCapacity <= UINT32_MAX &&
        DispatchPeerWqesStreamable(routeCount,
            TileXR::TILEXR_UDMA_SQ_BB_COUNT,
            kDispatchSqPollReserve, hasWeight);
}

int ValidateDispatchPeerConfig(const DispatchPeerConfig &config,
    const TileXR::CommArgs &commArgs, uint64_t routeCount,
    uint64_t destinationCapacity, bool hasWeight)
{
    if (!DispatchPeerModeUsesGroups(static_cast<uint32_t>(config.mode))) {
        return TILEXR_MOONEP_SUCCESS;
    }
    if (!DispatchVectorBatchShapeSupported(
            routeCount, destinationCapacity, hasWeight)) {
        return TILEXR_MOONEP_ERROR_NOT_SUPPORTED;
    }
    if (commArgs.rankSize > 1 &&
        DispatchGroupedGroupCount(commArgs.rankSize, config.groupWidth) == 0U) {
        return TILEXR_MOONEP_ERROR_NOT_SUPPORTED;
    }
    return TILEXR_MOONEP_SUCCESS;
}

bool CheckedMul(uint64_t lhs, uint64_t rhs, uint64_t *out)
{
    if (out == nullptr || (lhs != 0 && rhs > std::numeric_limits<uint64_t>::max() / lhs)) {
        return false;
    }
    *out = lhs * rhs;
    return true;
}

bool TensorDescriptorValid(const TileXRMoonEpTensorV1 *tensor)
{
    if (tensor == nullptr || tensor->structSize < sizeof(*tensor) ||
        tensor->abiVersion != TILEXR_MOONEP_ABI_VERSION_V1 || tensor->data == nullptr ||
        tensor->elementCount == 0 || tensor->rank == 0 ||
        tensor->rank > TILEXR_MOONEP_MAX_TENSOR_RANK) {
        return false;
    }
    uint64_t elements = 1;
    for (uint32_t i = 0; i < TILEXR_MOONEP_MAX_TENSOR_RANK; ++i) {
        if (i < tensor->rank) {
            if (tensor->shape[i] <= 0 || !CheckedMul(elements,
                    static_cast<uint64_t>(tensor->shape[i]), &elements)) {
                return false;
            }
        } else if (tensor->shape[i] != 0) {
            return false;
        }
    }
    return elements == tensor->elementCount;
}

bool PlanValid(const TileXRMoonEpPlanV1 *plan)
{
    uint64_t encodedCapacity = 0;
    return plan != nullptr && plan->structSize >= sizeof(*plan) &&
        plan->abiVersion == TILEXR_MOONEP_ABI_VERSION_V1 && plan->n > 0 &&
        plan->r > 0 && plan->e > 0 && plan->b > 0 && plan->nvS >= plan->n &&
        plan->k > 0 && plan->k <= 32 && plan->n % plan->k == 0 &&
        plan->e % plan->r == 0 && plan->b <= plan->e / plan->r &&
        plan->e <= std::numeric_limits<int64_t>::max() - plan->b &&
        plan->dst != nullptr && plan->expertsToCopy != nullptr &&
        plan->zeroFillRanges != nullptr && plan->remoteStats != nullptr &&
        plan->dupGroups != nullptr && plan->dupLoffs != nullptr &&
        plan->dupCounts != nullptr && plan->status != nullptr &&
        CheckedMul(static_cast<uint64_t>(plan->nvS),
            static_cast<uint64_t>(plan->r), &encodedCapacity) &&
        encodedCapacity <= static_cast<uint64_t>(INT32_MAX) + 1ULL;
}

bool TensorBytes(const TileXRMoonEpTensorV1 &tensor, uint64_t *bytes)
{
    uint64_t elementBytes = 0;
    switch (tensor.dtype) {
        case TILEXR_MOONEP_DTYPE_FLOAT16:
        case TILEXR_MOONEP_DTYPE_BFLOAT16:
            elementBytes = 2;
            break;
        case TILEXR_MOONEP_DTYPE_FLOAT32:
            elementBytes = 4;
            break;
        default:
            return false;
    }
    return CheckedMul(tensor.elementCount, elementBytes, bytes);
}

bool RangesOverlap(const void *lhs, uint64_t lhsBytes, const void *rhs, uint64_t rhsBytes)
{
    if (lhs == nullptr || rhs == nullptr || lhsBytes == 0 || rhsBytes == 0) {
        return false;
    }
    const uintptr_t lhsBegin = reinterpret_cast<uintptr_t>(lhs);
    const uintptr_t rhsBegin = reinterpret_cast<uintptr_t>(rhs);
    if (lhsBytes > static_cast<uint64_t>(UINTPTR_MAX - lhsBegin) ||
        rhsBytes > static_cast<uint64_t>(UINTPTR_MAX - rhsBegin)) {
        return true;
    }
    return lhsBegin < rhsBegin + static_cast<uintptr_t>(rhsBytes) &&
        rhsBegin < lhsBegin + static_cast<uintptr_t>(lhsBytes);
}

int ResolveDispatchTrace(const TileXRMoonEpDispatchArgsV1 &args,
    const TileXRMoonEpDispatchTraceV1 *&trace, uint64_t &requiredBytes)
{
    trace = nullptr;
    requiredBytes = 0U;
    const std::size_t traceFieldEnd = offsetof(TileXRMoonEpDispatchArgsV1, trace) +
        sizeof(args.trace);
    if (args.structSize < traceFieldEnd || args.trace == nullptr) {
        return TILEXR_MOONEP_SUCCESS;
    }
    trace = args.trace;
    uint64_t coreRecordOffset = 0U;
    uint64_t eventOffset = 0U;
    if (trace->structSize < sizeof(*trace) ||
        trace->abiVersion != TILEXR_MOONEP_ABI_VERSION_V1 ||
        trace->buffer == nullptr || trace->bufferBytes == 0U ||
        trace->iteration >= trace->iterationCount || trace->reserved != 0U ||
        reinterpret_cast<uintptr_t>(trace->buffer) % alignof(DispatchTraceEvent) != 0U ||
        !DispatchTraceLayout(trace->iterationCount, trace->eventCapacity,
            coreRecordOffset, eventOffset, requiredBytes) ||
        trace->bufferBytes < requiredBytes) {
        return TILEXR_MOONEP_ERROR_INVALID_ARGUMENT;
    }
    return TILEXR_MOONEP_SUCCESS;
}

int SelectPayload(const TileXRMoonEpPlanV1 &plan,
    const TileXRMoonEpTensorV1 &input, const TileXRMoonEpTensorV1 &output,
    DispatchPayloadMode *mode, int64_t *h)
{
    if (mode == nullptr || h == nullptr || input.dtype != output.dtype) {
        return TILEXR_MOONEP_ERROR_INVALID_ARGUMENT;
    }
    if (input.dtype == TILEXR_MOONEP_DTYPE_FLOAT16 ||
        input.dtype == TILEXR_MOONEP_DTYPE_BFLOAT16) {
        if (input.rank != 2 || output.rank != 2 ||
            input.shape[0] != plan.n / plan.k || input.shape[1] <= 0 ||
            output.shape[0] != plan.nvS ||
            output.shape[1] != input.shape[1]) {
            return TILEXR_MOONEP_ERROR_INVALID_ARGUMENT;
        }
        *mode = DispatchPayloadMode::Hidden;
        *h = input.shape[1];
        return TILEXR_MOONEP_SUCCESS;
    }
    if (input.dtype == TILEXR_MOONEP_DTYPE_FLOAT32 && input.rank == 2 &&
        output.rank == 1 && input.shape[0] == plan.n / plan.k &&
        input.shape[1] == plan.k && output.shape[0] == plan.nvS) {
        *mode = DispatchPayloadMode::RouteWeight;
        *h = 1;
        return TILEXR_MOONEP_SUCCESS;
    }
    return TILEXR_MOONEP_ERROR_INVALID_ARGUMENT;
}

int ValidateRegisteredWorkspace(TileXRCommPtr comm, const TileXR::CommArgs &commArgs,
    void *workspace, uint64_t workspaceBytes)
{
    if (workspace == nullptr || workspaceBytes == 0 ||
        reinterpret_cast<uintptr_t>(workspace) % kDispatchRegistrationAlignmentBytes != 0 ||
        workspaceBytes % kDispatchRegistrationAlignmentBytes != 0) {
        return TILEXR_MOONEP_ERROR_INVALID_ARGUMENT;
    }
    if (commArgs.rankSize == 1) {
        return TILEXR_MOONEP_SUCCESS;
    }
    if ((commArgs.extraFlag & TileXR::ExtraFlag::UDMA) == 0 ||
        commArgs.udmaInfoPtr == nullptr || commArgs.udmaRegistryPtr == nullptr) {
        return TILEXR_MOONEP_ERROR_NOT_SUPPORTED;
    }
    uint32_t qpCount = 0U;
    const bool sharedQp =
        (commArgs.extraFlag & TileXR::ExtraFlag::UDMA_SHARED_QP) != 0U;
    if (TileXRUDMAGetQpCount(comm, &qpCount) != TileXR::TILEXR_SUCCESS ||
        !DispatchQpCountSupported(qpCount, sharedQp)) {
        return TILEXR_MOONEP_ERROR_NOT_SUPPORTED;
    }
    const TileXR::TileXRUDMARegistry *registry = nullptr;
    const int registryRet = TileXRGetUDMARegistryHost(comm, &registry);
    if (registryRet != TileXR::TILEXR_SUCCESS ||
        !TileXR::UDMARegistryValid(registry, commArgs.rankSize) ||
        registry->regions[commArgs.rank].base != static_cast<GM_ADDR>(workspace)) {
        return TILEXR_MOONEP_ERROR_INVALID_ARGUMENT;
    }
    for (int rank = 0; rank < commArgs.rankSize; ++rank) {
        if (registry->regions[rank].bytes < workspaceBytes ||
            !TileXR::UDMARegionContains(registry, rank, 0, workspaceBytes)) {
            return TILEXR_MOONEP_ERROR_INVALID_ARGUMENT;
        }
    }
    return TILEXR_MOONEP_SUCCESS;
}

int ValidateFullmeshCapability(TileXRCommPtr comm,
    const TileXR::CommArgs &commArgs)
{
    if (commArgs.rankSize == 1 || commArgs.localRankSize == 1) {
        return TILEXR_MOONEP_SUCCESS;
    }
    if (commArgs.localRankSize <= 0 ||
        commArgs.localRank < 0 ||
        commArgs.localRank >= commArgs.localRankSize ||
        commArgs.rank % commArgs.localRankSize != commArgs.localRank ||
        (commArgs.extraFlag & TileXR::ExtraFlag::UDMA_FULLMESH) == 0U ||
        commArgs.udmaFullmeshPtr == nullptr ||
        commArgs.udmaRegistrationGeneration == 0U) {
        return TILEXR_MOONEP_ERROR_NOT_SUPPORTED;
    }
    TileXR::TileXRUDMAFullmeshHostView view {};
    const int ret = TileXRUDMAFullmeshQuery(comm, &view);
    if (ret != TileXR::TILEXR_SUCCESS ||
        !TileXR::UDMAFullmeshHostViewValid(view,
            static_cast<uint32_t>(commArgs.localRank),
            static_cast<uint32_t>(commArgs.localRankSize),
            commArgs.udmaRegistrationGeneration) ||
        view.viewDev != commArgs.udmaFullmeshPtr) {
        return TILEXR_MOONEP_ERROR_NOT_SUPPORTED;
    }
    return TILEXR_MOONEP_SUCCESS;
}

int ValidateAivCoreCount()
{
    int32_t device = 0;
    int64_t vectorCores = 0;
    if (aclrtGetDevice(&device) != ACL_SUCCESS ||
        aclrtGetDeviceInfo(static_cast<uint32_t>(device), ACL_DEV_ATTR_VECTOR_CORE_NUM,
            &vectorCores) != ACL_SUCCESS) {
        return TILEXR_MOONEP_ERROR_INTERNAL;
    }
    if (vectorCores < static_cast<int64_t>(kDispatchAivCoreCount)) {
        return TILEXR_MOONEP_ERROR_NOT_SUPPORTED;
    }
    return TILEXR_MOONEP_SUCCESS;
}

int MapLaunchStatus(int status)
{
    if (status == TileXR::TILEXR_SUCCESS) {
        return TILEXR_MOONEP_SUCCESS;
    }
    if (status == TileXR::TILEXR_ERROR_PARA_CHECK_FAIL) {
        return TILEXR_MOONEP_ERROR_INVALID_ARGUMENT;
    }
    if (status == TileXR::TILEXR_ERROR_NOT_SUPPORT) {
        return TILEXR_MOONEP_ERROR_NOT_SUPPORTED;
    }
    return TILEXR_MOONEP_ERROR_INTERNAL;
}

} // namespace

int TileXRMoonEpQueryDispatchUrmaWorkspace(TileXRCommPtr comm, int64_t s,
    int64_t k, int64_t h, uint32_t hiddenDtype, uint64_t *workspaceBytes,
    uint64_t *workspaceAlignment)
{
    if (comm == nullptr || workspaceBytes == nullptr || workspaceAlignment == nullptr ||
        s <= 0 || k <= 0 || k > 32 || h <= 0 ||
        (hiddenDtype != TILEXR_MOONEP_DTYPE_FLOAT16 &&
            hiddenDtype != TILEXR_MOONEP_DTYPE_BFLOAT16)) {
        return TILEXR_MOONEP_ERROR_INVALID_ARGUMENT;
    }
    TileXR::CommArgs *commArgs = nullptr;
    const int commRet = TileXRGetCommArgsHost(comm, commArgs);
    if (commRet != TileXR::TILEXR_SUCCESS || commArgs == nullptr) {
        return TILEXR_MOONEP_ERROR_INTERNAL;
    }
    uint64_t routeCount = 0;
    if (!CheckedMul(static_cast<uint64_t>(s), static_cast<uint64_t>(k),
            &routeCount) || routeCount > static_cast<uint64_t>(INT64_MAX)) {
        return TILEXR_MOONEP_ERROR_INVALID_ARGUMENT;
    }
    MoonEpDispatchUrmaLayout layout {};
    if (TileXRMoonEpBuildDispatchUrmaLayout(commArgs->rankSize, s, k, h,
            static_cast<int64_t>(routeCount), &layout) !=
        TileXR::TILEXR_SUCCESS) {
        return TILEXR_MOONEP_ERROR_INVALID_ARGUMENT;
    }
    *workspaceBytes = layout.totalBytes;
    *workspaceAlignment = kDispatchRegistrationAlignmentBytes;
    return TILEXR_MOONEP_SUCCESS;
}

static int RunDispatchUrma(const TileXRMoonEpDispatchArgsV1 *args,
    aclrtStream stream, bool resetStatus)
{
    const std::size_t baseArgsBytes = offsetof(TileXRMoonEpDispatchArgsV1, trace);
    if (args == nullptr || args->structSize < baseArgsBytes ||
        (args->abiVersion != TILEXR_MOONEP_ABI_VERSION_V1 &&
            args->abiVersion != TILEXR_MOONEP_ABI_VERSION_V2) ||
        stream == nullptr ||
        args->comm == nullptr || args->plan == nullptr || args->hiddenSh == nullptr ||
        args->hiddenNvsh == nullptr ||
        (args->flags & ~TILEXR_MOONEP_FLAG_RESET_STATUS) != 0 ||
        !PlanValid(args->plan) || !TensorDescriptorValid(args->hiddenSh) ||
        !TensorDescriptorValid(args->hiddenNvsh) ||
        (args->routeWeightsSk == nullptr) != (args->routeWeightsNvs == nullptr) ||
        (args->routeWeightsSk != nullptr &&
            (!TensorDescriptorValid(args->routeWeightsSk) ||
                !TensorDescriptorValid(args->routeWeightsNvs)))) {
        return TILEXR_MOONEP_ERROR_INVALID_ARGUMENT;
    }

    const TileXRMoonEpDispatchTraceV1 *trace = nullptr;
    uint64_t traceRequiredBytes = 0U;
    int ret = ResolveDispatchTrace(*args, trace, traceRequiredBytes);
    if (ret != TILEXR_MOONEP_SUCCESS) {
        return ret;
    }

    DispatchPayloadMode hiddenMode = DispatchPayloadMode::Hidden;
    int64_t h = 0;
    ret = SelectPayload(*args->plan, *args->hiddenSh, *args->hiddenNvsh,
        &hiddenMode, &h);
    if (ret != TILEXR_MOONEP_SUCCESS || hiddenMode != DispatchPayloadMode::Hidden) {
        return TILEXR_MOONEP_ERROR_INVALID_ARGUMENT;
    }
    if (args->routeWeightsSk != nullptr) {
        DispatchPayloadMode weightMode = DispatchPayloadMode::Hidden;
        int64_t weightH = 0;
        ret = SelectPayload(*args->plan, *args->routeWeightsSk,
            *args->routeWeightsNvs, &weightMode, &weightH);
        if (ret != TILEXR_MOONEP_SUCCESS ||
            weightMode != DispatchPayloadMode::RouteWeight) {
            return TILEXR_MOONEP_ERROR_INVALID_ARGUMENT;
        }
    }

    TileXR::CommArgs *commArgs = nullptr;
    GM_ADDR devArgs = nullptr;
    if (TileXRGetCommArgsHost(args->comm, commArgs) != TileXR::TILEXR_SUCCESS ||
        commArgs == nullptr || TileXRGetCommArgsDev(args->comm, devArgs) !=
            TileXR::TILEXR_SUCCESS || devArgs == nullptr) {
        return TILEXR_MOONEP_ERROR_INTERNAL;
    }
    if (args->plan->r != commArgs->rankSize) {
        return TILEXR_MOONEP_ERROR_INVALID_ARGUMENT;
    }

    MoonEpDispatchUrmaLayout layout {};
    if (TileXRMoonEpBuildDispatchUrmaLayout(commArgs->rankSize,
            args->plan->n / args->plan->k, args->plan->k, h,
            args->plan->nvS, &layout) != TileXR::TILEXR_SUCCESS ||
        TileXRMoonEpBindDispatchUrmaWorkspace(
            args->registeredWorkspaceBytes, &layout) != TileXR::TILEXR_SUCCESS) {
        return TILEXR_MOONEP_ERROR_INVALID_ARGUMENT;
    }
    ret = ValidateRegisteredWorkspace(args->comm, *commArgs,
        args->registeredWorkspace, args->registeredWorkspaceBytes);
    if (ret != TILEXR_MOONEP_SUCCESS) {
        return ret;
    }
    ret = ValidateAivCoreCount();
    if (ret != TILEXR_MOONEP_SUCCESS) {
        return ret;
    }
    DispatchPeerConfig peerConfig {};
    if (!ResolveDispatchPeerConfig(peerConfig)) {
        return TILEXR_MOONEP_ERROR_INVALID_ARGUMENT;
    }
    ret = ValidateDispatchPeerConfig(peerConfig, *commArgs,
        static_cast<uint64_t>(layout.routeCount),
        static_cast<uint64_t>(layout.destinationCapacity),
        args->routeWeightsSk != nullptr);
    if (ret != TILEXR_MOONEP_SUCCESS) {
        return ret;
    }
    if (DispatchPeerModeUsesGroups(static_cast<uint32_t>(peerConfig.mode))) {
        ret = ValidateFullmeshCapability(args->comm, *commArgs);
        if (ret != TILEXR_MOONEP_SUCCESS) {
            return ret;
        }
    }

    struct BufferRange {
        const void *data;
        uint64_t bytes;
    };
    BufferRange ranges[4] {};
    uint32_t rangeCount = 0;
    const TileXRMoonEpTensorV1 *tensors[4] = {
        args->hiddenSh, args->hiddenNvsh,
        args->routeWeightsSk, args->routeWeightsNvs};
    for (uint32_t index = 0; index < 4U; ++index) {
        if (tensors[index] == nullptr) {
            continue;
        }
        uint64_t bytes = 0;
        if (!TensorBytes(*tensors[index], &bytes)) {
            return TILEXR_MOONEP_ERROR_INVALID_ARGUMENT;
        }
        ranges[rangeCount++] = BufferRange {tensors[index]->data, bytes};
    }
    uint64_t dstBytes = 0;
    const uint64_t zeroFillRangeCount = static_cast<uint64_t>(
        args->plan->e + args->plan->b);
    uint64_t zeroFillBytes = 0;
    if (zeroFillRangeCount > UINT32_MAX ||
        !CheckedMul(static_cast<uint64_t>(layout.routeCount),
            sizeof(int32_t), &dstBytes) ||
        !CheckedMul(zeroFillRangeCount, 2U * sizeof(int32_t),
            &zeroFillBytes) ||
        RangesOverlap(args->plan->dst, dstBytes, args->registeredWorkspace,
            args->registeredWorkspaceBytes) ||
        RangesOverlap(args->plan->zeroFillRanges, zeroFillBytes,
            args->registeredWorkspace, args->registeredWorkspaceBytes) ||
        RangesOverlap(args->plan->dst, dstBytes,
            args->plan->zeroFillRanges, zeroFillBytes) ||
        RangesOverlap(args->plan->status, sizeof(int32_t),
            args->registeredWorkspace, args->registeredWorkspaceBytes) ||
        RangesOverlap(args->plan->status, sizeof(int32_t),
            args->plan->zeroFillRanges, zeroFillBytes)) {
        return TILEXR_MOONEP_ERROR_INVALID_ARGUMENT;
    }
    if (trace != nullptr &&
        (RangesOverlap(trace->buffer, traceRequiredBytes,
             args->registeredWorkspace, args->registeredWorkspaceBytes) ||
         RangesOverlap(trace->buffer, traceRequiredBytes,
              args->plan->dst, dstBytes) ||
         RangesOverlap(trace->buffer, traceRequiredBytes,
              args->plan->zeroFillRanges, zeroFillBytes) ||
         RangesOverlap(trace->buffer, traceRequiredBytes,
              args->plan->status, sizeof(int32_t)))) {
        return TILEXR_MOONEP_ERROR_INVALID_ARGUMENT;
    }
    for (uint32_t lhs = 0; lhs < rangeCount; ++lhs) {
        if (RangesOverlap(ranges[lhs].data, ranges[lhs].bytes,
                args->registeredWorkspace, args->registeredWorkspaceBytes) ||
            RangesOverlap(ranges[lhs].data, ranges[lhs].bytes,
                args->plan->dst, dstBytes) ||
            RangesOverlap(ranges[lhs].data, ranges[lhs].bytes,
                args->plan->zeroFillRanges, zeroFillBytes) ||
            RangesOverlap(ranges[lhs].data, ranges[lhs].bytes,
                args->plan->status, sizeof(int32_t))) {
            return TILEXR_MOONEP_ERROR_INVALID_ARGUMENT;
        }
        if (trace != nullptr && RangesOverlap(ranges[lhs].data,
                ranges[lhs].bytes, trace->buffer, traceRequiredBytes)) {
            return TILEXR_MOONEP_ERROR_INVALID_ARGUMENT;
        }
        for (uint32_t rhs = lhs + 1U; rhs < rangeCount; ++rhs) {
            if (RangesOverlap(ranges[lhs].data, ranges[lhs].bytes,
                    ranges[rhs].data, ranges[rhs].bytes)) {
                return TILEXR_MOONEP_ERROR_INVALID_ARGUMENT;
            }
        }
    }

    DispatchUrmaLaunchParams params {};
    params.commArgs = devArgs;
    params.dst = static_cast<const int32_t *>(args->plan->dst);
    params.zeroFillRanges = static_cast<const int32_t *>(args->plan->zeroFillRanges);
    params.workspace = args->registeredWorkspace;
    params.planStatus = static_cast<int32_t *>(args->plan->status);
    params.comm = args->comm;
    params.stream = stream;
    params.peerMode = peerConfig.mode;
    params.groupWidth = peerConfig.groupWidth;
    params.zeroFillRangeCount = args->plan->e + args->plan->b;
    if (trace != nullptr) {
        params.trace = trace->buffer;
        params.traceBytes = trace->bufferBytes;
        params.traceIteration = trace->iteration;
        params.traceIterationCount = trace->iterationCount;
        params.traceEventCapacity = trace->eventCapacity;
    }
    params.layout = layout;
    params.hiddenInput = args->hiddenSh->data;
    params.hiddenOutput = args->hiddenNvsh->data;
    params.weightInput = args->routeWeightsSk == nullptr ? nullptr :
        args->routeWeightsSk->data;
    params.weightOutput = args->routeWeightsNvs == nullptr ? nullptr :
        args->routeWeightsNvs->data;

    const bool statusResetEnqueued = resetStatus ||
        (args->flags & TILEXR_MOONEP_FLAG_RESET_STATUS) != 0;
    if (statusResetEnqueued && aclrtMemsetAsync(args->plan->status,
            sizeof(int32_t), 0, sizeof(int32_t), stream) != ACL_SUCCESS) {
        return TILEXR_MOONEP_ERROR_INTERNAL;
    }

    ret = MapLaunchStatus(TileXRMoonEpLaunchDispatchUrmaKernel(params));
    if (ret != TILEXR_MOONEP_SUCCESS && statusResetEnqueued &&
        aclrtSynchronizeStream(stream) != ACL_SUCCESS) {
        return TILEXR_MOONEP_ERROR_INTERNAL;
    }
    return ret;
}

int TileXRMoonEpRunDispatchUrmaV1(const TileXRMoonEpDispatchArgsV1 *args,
    aclrtStream stream)
{
    return RunDispatchUrma(args, stream, false);
}

int TileXRMoonEpRunDispatchUrmaV2(const TileXRMoonEpDispatchArgsV2 *args,
    aclrtStream stream)
{
    return RunDispatchUrma(args, stream, true);
}

} // namespace TileXRMoonEp
