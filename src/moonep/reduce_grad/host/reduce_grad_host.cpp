#include "reduce_grad_host.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <mutex>
#include <new>

#include "acl/acl_rt.h"
#include "comm_args.h"
#include "tilexr_types.h"

namespace TileXRMoonEp {
namespace {

std::mutex g_reduceGradEnqueueMutex;

bool CheckedMul(uint64_t lhs, uint64_t rhs, uint64_t *out)
{
    if (out == nullptr || (lhs != 0 && rhs > std::numeric_limits<uint64_t>::max() / lhs)) {
        return false;
    }
    *out = lhs * rhs;
    return true;
}

bool CheckedAdd(uint64_t lhs, uint64_t rhs, uint64_t *out)
{
    if (out == nullptr || rhs > std::numeric_limits<uint64_t>::max() - lhs) {
        return false;
    }
    *out = lhs + rhs;
    return true;
}

bool AddressRangeValid(const void *base, uint64_t bytes)
{
    if (base == nullptr || bytes == 0 ||
        bytes > static_cast<uint64_t>(std::numeric_limits<uintptr_t>::max())) {
        return false;
    }
    const uintptr_t address = reinterpret_cast<uintptr_t>(base);
    return static_cast<uintptr_t>(bytes) <= std::numeric_limits<uintptr_t>::max() - address;
}

bool AddressRangeContains(const void *outerBase, uint64_t outerBytes,
    const void *innerBase, uint64_t innerBytes)
{
    if (!AddressRangeValid(outerBase, outerBytes) ||
        !AddressRangeValid(innerBase, innerBytes)) {
        return false;
    }
    const uintptr_t outer = reinterpret_cast<uintptr_t>(outerBase);
    const uintptr_t inner = reinterpret_cast<uintptr_t>(innerBase);
    if (inner < outer) {
        return false;
    }
    const uint64_t offset = static_cast<uint64_t>(inner - outer);
    return offset <= outerBytes && innerBytes <= outerBytes - offset;
}

void ResolveSourceRegistration(const TileXRMoonEpReduceGradSourceSliceV2 &source,
    void **base, uint64_t *bytes)
{
    if (source.registrationBase == nullptr && source.registrationBytes == 0) {
        *base = source.data;
        *bytes = source.bytes;
        return;
    }
    *base = source.registrationBase;
    *bytes = source.registrationBytes;
}

bool ValidatePlan(const TileXRMoonEpPlanV1 *plan, const TileXR::CommArgs &commArgs)
{
    if (plan == nullptr || plan->structSize < sizeof(*plan) ||
        plan->abiVersion != TILEXR_MOONEP_ABI_VERSION_V1 || plan->n <= 0 ||
        plan->k <= 0 || plan->n % plan->k != 0 || plan->e <= 0 || plan->b <= 0 ||
        plan->r <= 0 || plan->r > TileXR::TILEXR_MAX_RANK_SIZE ||
        plan->r != commArgs.rankSize || plan->e % plan->r != 0 ||
        plan->e > std::numeric_limits<int32_t>::max() ||
        plan->b > std::numeric_limits<int32_t>::max() ||
        plan->b > std::numeric_limits<int32_t>::max() / plan->r ||
        plan->expertsToCopy == nullptr) {
        return false;
    }
    uint64_t capacity = 0;
    return CheckedMul(static_cast<uint64_t>(plan->n / plan->k),
        static_cast<uint64_t>(plan->k), &capacity) &&
        capacity == static_cast<uint64_t>(plan->n);
}

bool ValidateGradient(const TileXRMoonEpTensorV1 *tensor, int64_t rowCount,
    uint64_t *rowElements)
{
    if (tensor == nullptr || rowElements == nullptr || tensor->structSize < sizeof(*tensor) ||
        tensor->abiVersion != TILEXR_MOONEP_ABI_VERSION_V1 || tensor->data == nullptr ||
        tensor->dtype != TILEXR_MOONEP_DTYPE_FLOAT32 || tensor->rank < 2 ||
        tensor->rank > TILEXR_MOONEP_MAX_TENSOR_RANK || tensor->shape[0] != rowCount) {
        return false;
    }

    uint64_t elements = 1;
    for (uint32_t dim = 0; dim < TILEXR_MOONEP_MAX_TENSOR_RANK; ++dim) {
        if (dim < tensor->rank) {
            if (tensor->shape[dim] <= 0 ||
                !CheckedMul(elements, static_cast<uint64_t>(tensor->shape[dim]), &elements)) {
                return false;
            }
        } else if (tensor->shape[dim] != 0) {
            return false;
        }
    }
    if (elements != tensor->elementCount || rowCount <= 0 ||
        elements % static_cast<uint64_t>(rowCount) != 0) {
        return false;
    }
    *rowElements = elements / static_cast<uint64_t>(rowCount);
    return *rowElements != 0;
}

bool ValidateStatus(const TileXRMoonEpTensorV1 *status)
{
    return status != nullptr && status->structSize >= sizeof(*status) &&
        status->abiVersion == TILEXR_MOONEP_ABI_VERSION_V1 && status->data != nullptr &&
        status->elementCount == 1 && status->dtype == TILEXR_MOONEP_DTYPE_INT32 &&
        status->rank == 1 && status->shape[0] == 1 && status->shape[1] == 0 &&
        status->shape[2] == 0 && status->shape[3] == 0;
}

bool TensorIdentityEqual(const TileXRMoonEpTensorV1 &expected,
    const TileXRMoonEpTensorV1 *actual)
{
    if (actual == nullptr || actual->structSize < sizeof(*actual) ||
        actual->abiVersion != expected.abiVersion || actual->data != expected.data ||
        actual->elementCount != expected.elementCount || actual->dtype != expected.dtype ||
        actual->rank != expected.rank) {
        return false;
    }
    for (uint32_t dim = 0; dim < TILEXR_MOONEP_MAX_TENSOR_RANK; ++dim) {
        if (actual->shape[dim] != expected.shape[dim]) {
            return false;
        }
    }
    return true;
}

bool SourceIdentityEqual(const TileXRMoonEpReduceGradSourceSliceV2 &expected,
    const TileXRMoonEpReduceGradSourceSliceV2 &actual)
{
    return actual.data == expected.data && actual.bytes == expected.bytes &&
        actual.registrationBase == expected.registrationBase &&
        actual.registrationBytes == expected.registrationBytes;
}

bool IsA5(const TileXR::CommArgs &commArgs)
{
    return (commArgs.extraFlag & TileXR::ExtraFlag::TOPO_910A5) != 0;
}

bool UsesSharedQps(const TileXR::CommArgs &commArgs)
{
    return (commArgs.extraFlag & TileXR::ExtraFlag::UDMA_SHARED_QP) != 0;
}

bool LocalityValid(const TileXR::CommArgs &commArgs)
{
    return commArgs.rankSize > 0 && commArgs.rankSize <= TileXR::TILEXR_MAX_RANK_SIZE &&
        commArgs.rank >= 0 && commArgs.rank < commArgs.rankSize &&
        commArgs.localRankSize > 0 && commArgs.localRankSize <= commArgs.rankSize &&
        commArgs.rankSize % commArgs.localRankSize == 0 &&
        commArgs.localRank >= 0 && commArgs.localRank < commArgs.localRankSize;
}

bool ProfileViewMatches(const ReduceGradPreparedContext &context,
    const TileXR::TileXRUDMAProfileView &view)
{
    return view.version == TileXR::TILEXR_UDMA_PROFILE_VERSION &&
        view.rankSize == static_cast<uint32_t>(context.layout.rankSize) &&
        view.regionCount == kReduceGradProfileRegionCount &&
        view.qpCount == context.layout.transportQpCount &&
        view.infoDev == context.profileView.infoDev &&
        view.registryDev == context.profileView.registryDev &&
        view.registryHost == context.profileView.registryHost &&
        TileXR::UDMAProfileRegistryValid(view.registryHost, context.layout.rankSize,
            kReduceGradProfileRegionCount, context.layout.transportQpCount);
}

bool PreparedRegistryMatches(const ReduceGradPreparedContext &context)
{
    const auto *registry = context.profileView.registryHost;
    if (!TileXR::UDMAProfileRegistryValid(registry, context.layout.rankSize,
            kReduceGradProfileRegionCount, context.layout.transportQpCount)) {
        return false;
    }
    const int rank = static_cast<int>(context.layout.rank);
    const auto *staging = TileXR::UDMAProfileRegion(registry, rank, kReduceGradStagingRegion);
    if (staging == nullptr || staging->base != static_cast<GM_ADDR>(context.workspace) ||
        staging->bytes != context.workspaceBytes) {
        return false;
    }
    for (uint32_t projection = 0; projection < kReduceGradProjectionCount; ++projection) {
        const auto *source = TileXR::UDMAProfileRegion(registry, rank, projection + 1);
        if (source == nullptr || source->base != static_cast<GM_ADDR>(
                context.sources[projection].data) ||
            source->bytes != context.sources[projection].bytes) {
            return false;
        }
    }
    uint32_t qpRegions[kReduceGradMaxTransportQpCount] = {};
    for (uint32_t qp = 0; qp < context.layout.transportQpCount; ++qp) {
        qpRegions[qp] = kReduceGradGate + 1U;
    }
    for (uint32_t lane = 0; lane < context.layout.laneCount; ++lane) {
        qpRegions[context.layout.lanePhysicalQps[lane]] =
            context.layout.qpProjection[lane] + 1U;
    }
    if (context.layout.barrierPhysicalQp != kReduceGradInvalidPhysicalQp) {
        qpRegions[context.layout.barrierPhysicalQp] = kReduceGradStagingRegion;
    }
    for (uint32_t qp = 0; qp < context.layout.transportQpCount; ++qp) {
        if (registry->qpBindings[qp].localRegion != kReduceGradStagingRegion ||
            registry->qpBindings[qp].remoteRegion != qpRegions[qp]) {
            return false;
        }
    }
    return true;
}

void FillWorkspaceInfo(const ReduceGradLayout &layout,
    TileXRMoonEpReduceGradWorkspaceInfoV2 *info)
{
    info->workspaceBytes = layout.workspaceBytes;
    info->workspaceAlignment = kReduceGradWorkspaceAlignment;
    info->udmaChunkBytes = layout.chunkBytes;
    info->laneStateBytes = layout.laneStateBytes;
    info->laneStateStrideBytes = kReduceGradLaneStateStrideBytes;
    info->bankStrideBytes = layout.bankStrideBytes;
    info->laneStrideBytes = layout.laneStrideBytes;
    info->qpCount = layout.qpCount;
    info->blockDim = static_cast<uint32_t>(layout.blockDim);
    info->reserved = 0;
    for (uint32_t projection = 0; projection < kReduceGradProjectionCount; ++projection) {
        info->rowBytes[projection] = layout.rowBytes[projection];
        info->chunkCounts[projection] = layout.chunkCounts[projection];
        info->projectionQpCounts[projection] = layout.projectionQpCounts[projection];
    }
}

} // namespace

int TileXRMoonEpPrepareReduceGradLayout(TileXRCommPtr comm,
    const TileXRMoonEpPlanV1 *plan,
    const TileXRMoonEpTensorV1 *const gradients[kReduceGradProjectionCount],
    uint64_t requestedChunkBytes, ReduceGradLayout *layout)
{
    if (comm == nullptr || gradients == nullptr || layout == nullptr) {
        return TileXR::TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    TileXR::CommArgs *commArgs = nullptr;
    int ret = TileXRGetCommArgsHost(comm, commArgs);
    if (ret != TileXR::TILEXR_SUCCESS) {
        return ret;
    }
    if (commArgs == nullptr) {
        return TileXR::TILEXR_ERROR_NOT_INITIALIZED;
    }
    if (!ValidatePlan(plan, *commArgs)) {
        return TileXR::TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    if (!IsA5(*commArgs)) {
        return TileXR::TILEXR_ERROR_NOT_SUPPORT;
    }
    if (!LocalityValid(*commArgs)) {
        return TileXR::TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    if (commArgs->rankSize < kReduceGradMinRankCount) {
        return TileXR::TILEXR_ERROR_NOT_SUPPORT;
    }

    uint64_t rowElements[kReduceGradProjectionCount] = {};
    uint64_t rowCountValue = 0;
    if (!CheckedAdd(static_cast<uint64_t>(plan->e), static_cast<uint64_t>(plan->b),
            &rowCountValue) || rowCountValue > static_cast<uint64_t>(INT64_MAX)) {
        return TileXR::TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    const int64_t rowCount = static_cast<int64_t>(rowCountValue);
    for (uint32_t projection = 0; projection < kReduceGradProjectionCount; ++projection) {
        if (!ValidateGradient(gradients[projection], rowCount, &rowElements[projection])) {
            return TileXR::TILEXR_ERROR_PARA_CHECK_FAIL;
        }
    }

    uint32_t transportQpCount = 0;
    if ((commArgs->extraFlag & TileXR::ExtraFlag::UDMA) == 0) {
        return TileXR::TILEXR_ERROR_NOT_SUPPORT;
    }
    ret = TileXRUDMAGetQpCount(comm, &transportQpCount);
    if (ret != TileXR::TILEXR_SUCCESS) {
        return ret;
    }
    if (transportQpCount < kReduceGradMinMultiRankQpCount ||
        transportQpCount > kReduceGradMaxTransportQpCount ||
        (transportQpCount > kReduceGradMaxUdmaQpCount &&
            (transportQpCount != kReduceGradMaxTransportQpCount ||
                !UsesSharedQps(*commArgs)))) {
        return TileXR::TILEXR_ERROR_NOT_SUPPORT;
    }
    return TileXRMoonEpBuildReduceGradLayout(commArgs->rank, commArgs->rankSize,
        commArgs->localRankSize, plan->e, plan->b, rowElements,
        transportQpCount, requestedChunkBytes, layout);
}

int TileXRMoonEpCreateReduceGradPreparedContext(
    const ReduceGradPrepareParams &params, ReduceGradPreparedContext **context)
{
    if (context == nullptr) {
        return TileXR::TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    *context = nullptr;
    const TileXRMoonEpTensorV1 *gradients[kReduceGradProjectionCount] = {
        params.gradients[kReduceGradGate], params.gradients[kReduceGradUp],
        params.gradients[kReduceGradDown]};
    ReduceGradLayout layout {};
    int ret = TileXRMoonEpPrepareReduceGradLayout(params.comm, params.plan,
        gradients, params.requestedChunkBytes, &layout);
    if (ret != TileXR::TILEXR_SUCCESS) {
        return ret;
    }

    if (!AddressRangeValid(params.workspace, params.workspaceBytes) ||
        params.workspaceBytes < layout.workspaceBytes ||
        reinterpret_cast<uintptr_t>(params.workspace) %
            kReduceGradWorkspaceAlignment != 0) {
        return TileXR::TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    for (uint32_t projection = 0; projection < kReduceGradProjectionCount; ++projection) {
        uint64_t requiredBytes = 0;
        void *registrationBase = nullptr;
        uint64_t registrationBytes = 0;
        ResolveSourceRegistration(params.sources[projection],
            &registrationBase, &registrationBytes);
        if (!CheckedMul(static_cast<uint64_t>(layout.prefetchSlots),
                layout.rowBytes[projection], &requiredBytes) ||
            params.sources[projection].bytes != requiredBytes ||
            !AddressRangeValid(params.sources[projection].data,
                params.sources[projection].bytes) ||
            !AddressRangeContains(registrationBase, registrationBytes,
                params.sources[projection].data, params.sources[projection].bytes)) {
            return TileXR::TILEXR_ERROR_PARA_CHECK_FAIL;
        }
    }

    ReduceGradPreparedContext *next = new (std::nothrow) ReduceGradPreparedContext();
    if (next == nullptr) {
        return TileXR::TILEXR_ERROR_INTERNAL;
    }
    next->comm = params.comm;
    next->layout = layout;
    next->planN = params.plan->n;
    next->planK = params.plan->k;
    next->expertsToCopy = params.plan->expertsToCopy;
    next->workspace = params.workspace;
    next->workspaceBytes = params.workspaceBytes;
    for (uint32_t projection = 0; projection < kReduceGradProjectionCount; ++projection) {
        next->gradients[projection] = *params.gradients[projection];
        next->sources[projection] = params.sources[projection];
    }

    ret = TileXRGetCommArgsHost(params.comm, next->hostArgs);
    if (ret == TileXR::TILEXR_SUCCESS && next->hostArgs == nullptr) {
        ret = TileXR::TILEXR_ERROR_NOT_INITIALIZED;
    }
    if (ret == TileXR::TILEXR_SUCCESS) {
        ret = TileXRGetCommArgsDev(params.comm, next->devArgs);
        if (ret == TileXR::TILEXR_SUCCESS && next->devArgs == nullptr) {
            ret = TileXR::TILEXR_ERROR_NOT_INITIALIZED;
        }
    }
    if (ret != TileXR::TILEXR_SUCCESS) {
        delete next;
        return ret;
    }

    TileXR::TileXRUDMAProfileDesc desc {};
    desc.regionCount = kReduceGradProfileRegionCount;
    desc.qpBindingCount = layout.transportQpCount;
    desc.regions[kReduceGradStagingRegion].base = static_cast<GM_ADDR>(params.workspace);
    desc.regions[kReduceGradStagingRegion].bytes = params.workspaceBytes;
    for (uint32_t projection = 0; projection < kReduceGradProjectionCount; ++projection) {
        desc.regions[projection + 1].base = static_cast<GM_ADDR>(
            params.sources[projection].data);
        desc.regions[projection + 1].bytes = params.sources[projection].bytes;
        void *registrationBase = nullptr;
        uint64_t registrationBytes = 0;
        ResolveSourceRegistration(params.sources[projection],
            &registrationBase, &registrationBytes);
        desc.regions[projection + 1].registrationBase =
            static_cast<GM_ADDR>(registrationBase);
        desc.regions[projection + 1].registrationBytes = registrationBytes;
    }
    for (uint32_t qp = 0; qp < layout.transportQpCount; ++qp) {
        desc.qpBindings[qp].localRegion = kReduceGradStagingRegion;
        desc.qpBindings[qp].remoteRegion = kReduceGradGate + 1U;
    }
    for (uint32_t lane = 0; lane < layout.laneCount; ++lane) {
        desc.qpBindings[layout.lanePhysicalQps[lane]].remoteRegion =
            layout.qpProjection[lane] + 1U;
    }
    if (layout.barrierPhysicalQp != kReduceGradInvalidPhysicalQp) {
        desc.qpBindings[layout.barrierPhysicalQp].remoteRegion =
            kReduceGradStagingRegion;
    }
    ret = TileXRUDMAProfileRegister(params.comm, &desc, &next->profileHandle);
    if (ret == TileXR::TILEXR_SUCCESS) {
        ret = TileXRUDMAProfileQuery(params.comm, next->profileHandle,
            &next->profileView);
    }
    if (ret == TileXR::TILEXR_SUCCESS &&
        (!ProfileViewMatches(*next, next->profileView) ||
            !PreparedRegistryMatches(*next))) {
        ret = TileXR::TILEXR_ERROR_NOT_INITIALIZED;
    }
    if (ret != TileXR::TILEXR_SUCCESS) {
        if (next->profileHandle != 0) {
            const int cleanupRet = TileXRUDMAProfileUnregister(
                params.comm, next->profileHandle);
            if (cleanupRet != TileXR::TILEXR_SUCCESS) {
                ret = cleanupRet;
            }
        }
        delete next;
        return ret;
    }

    *context = next;
    return TileXR::TILEXR_SUCCESS;
}

int TileXRMoonEpDestroyReduceGradPreparedContext(ReduceGradPreparedContext *context)
{
    if (context == nullptr) {
        return TileXR::TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    if (context->profileHandle != 0) {
        const int ret = TileXRUDMAProfileUnregister(context->comm, context->profileHandle);
        if (ret != TileXR::TILEXR_SUCCESS) {
            return ret;
        }
    }
    delete context;
    return TileXR::TILEXR_SUCCESS;
}

int TileXRMoonEpValidateReduceGradLaunch(const ReduceGradLaunchParams &params,
    const ReduceGradPreparedContext &context)
{
    if (params.stream == nullptr || params.waitIterations == 0 ||
        !ValidateStatus(params.status) || params.plan == nullptr ||
        params.plan->structSize < sizeof(*params.plan) ||
        params.plan->abiVersion != TILEXR_MOONEP_ABI_VERSION_V1 ||
        params.plan->n != context.planN || params.plan->k != context.planK ||
        params.plan->r != context.layout.rankSize ||
        params.plan->e != context.layout.expertCount ||
        params.plan->b != context.layout.prefetchSlots ||
        params.plan->expertsToCopy != context.expertsToCopy) {
        return TileXR::TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    for (uint32_t projection = 0; projection < kReduceGradProjectionCount; ++projection) {
        if (!TensorIdentityEqual(context.gradients[projection],
                params.gradients[projection]) ||
            !SourceIdentityEqual(context.sources[projection], params.sources[projection])) {
            return TileXR::TILEXR_ERROR_PARA_CHECK_FAIL;
        }
    }
    if (context.layout.rankSize > 1) {
        TileXR::TileXRUDMAProfileView current {};
        const int ret = TileXRUDMAProfileQuery(
            context.comm, context.profileHandle, &current);
        if (ret != TileXR::TILEXR_SUCCESS) {
            return ret;
        }
        if (!ProfileViewMatches(context, current) || !PreparedRegistryMatches(context)) {
            return TileXR::TILEXR_ERROR_NOT_INITIALIZED;
        }
    }
    return TileXR::TILEXR_SUCCESS;
}

} // namespace TileXRMoonEp

extern "C" int TileXRMoonEpReduceGradGetWorkspaceSizeV2(
    const TileXRMoonEpReduceGradWorkspaceQueryV2 *query,
    TileXRMoonEpReduceGradWorkspaceInfoV2 *info)
{
    if (query == nullptr || info == nullptr || query->structSize < sizeof(*query) ||
        query->abiVersion != TILEXR_MOONEP_ABI_VERSION_V2 ||
        info->structSize < sizeof(*info) || info->abiVersion != TILEXR_MOONEP_ABI_VERSION_V2 ||
        query->flags != TILEXR_MOONEP_FLAG_NONE) {
        return TileXR::TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    const TileXRMoonEpTensorV1 *gradients[TileXRMoonEp::kReduceGradProjectionCount] = {
        query->gate, query->up, query->down};
    TileXRMoonEp::ReduceGradLayout layout {};
    const int ret = TileXRMoonEp::TileXRMoonEpPrepareReduceGradLayout(query->comm,
        query->plan, gradients, query->requestedUdmaChunkBytes, &layout);
    if (ret != TileXR::TILEXR_SUCCESS) {
        return ret;
    }
    TileXRMoonEp::FillWorkspaceInfo(layout, info);
    return TileXR::TILEXR_SUCCESS;
}

extern "C" int TileXRMoonEpReduceGradPrepareV2(
    const TileXRMoonEpReduceGradPrepareArgsV2 *args,
    TileXRMoonEpReduceGradPreparedV2 *prepared)
{
    if (prepared != nullptr) {
        *prepared = nullptr;
    }
    if (args == nullptr || prepared == nullptr || args->structSize < sizeof(*args) ||
        args->abiVersion != TILEXR_MOONEP_ABI_VERSION_V2 || args->comm == nullptr ||
        args->plan == nullptr || args->gate == nullptr || args->up == nullptr ||
        args->down == nullptr || args->flags != TILEXR_MOONEP_FLAG_NONE) {
        return TileXR::TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    TileXRMoonEp::ReduceGradPrepareParams params {};
    params.comm = args->comm;
    params.plan = args->plan;
    params.gradients[TileXRMoonEp::kReduceGradGate] = args->gate;
    params.gradients[TileXRMoonEp::kReduceGradUp] = args->up;
    params.gradients[TileXRMoonEp::kReduceGradDown] = args->down;
    params.workspace = args->workspace;
    params.workspaceBytes = args->workspaceBytes;
    params.requestedChunkBytes = args->requestedUdmaChunkBytes;
    for (uint32_t projection = 0;
        projection < TileXRMoonEp::kReduceGradProjectionCount; ++projection) {
        params.sources[projection] = args->sources[projection];
    }
    TileXRMoonEp::ReduceGradPreparedContext *context = nullptr;
    const int ret = TileXRMoonEp::TileXRMoonEpCreateReduceGradPreparedContext(
        params, &context);
    if (ret == TileXR::TILEXR_SUCCESS) {
        *prepared = static_cast<TileXRMoonEpReduceGradPreparedV2>(context);
    }
    return ret;
}

extern "C" int TileXRMoonEpReduceGradDestroyPreparedV2(
    TileXRMoonEpReduceGradPreparedV2 prepared)
{
    return TileXRMoonEp::TileXRMoonEpDestroyReduceGradPreparedContext(
        static_cast<TileXRMoonEp::ReduceGradPreparedContext *>(prepared));
}

extern "C" int TileXRMoonEpReduceGradV2(const TileXRMoonEpReduceGradArgsV2 *args,
    aclrtStream stream)
{
    if (args == nullptr || args->structSize < sizeof(*args) ||
        args->abiVersion != TILEXR_MOONEP_ABI_VERSION_V2 || args->prepared == nullptr ||
        args->plan == nullptr || args->gate == nullptr || args->up == nullptr ||
        args->down == nullptr || args->status == nullptr || args->waitIterations == 0 ||
        args->flags != TILEXR_MOONEP_FLAG_NONE || stream == nullptr) {
        return TileXR::TILEXR_ERROR_PARA_CHECK_FAIL;
    }

    TileXRMoonEp::ReduceGradLaunchParams params {};
    params.plan = args->plan;
    params.gradients[TileXRMoonEp::kReduceGradGate] = args->gate;
    params.gradients[TileXRMoonEp::kReduceGradUp] = args->up;
    params.gradients[TileXRMoonEp::kReduceGradDown] = args->down;
    params.status = args->status;
    params.waitIterations = args->waitIterations;
    params.stream = stream;
    for (uint32_t projection = 0;
        projection < TileXRMoonEp::kReduceGradProjectionCount; ++projection) {
        params.sources[projection] = args->sources[projection];
    }
    auto *context = static_cast<TileXRMoonEp::ReduceGradPreparedContext *>(args->prepared);
    int ret = TileXRMoonEp::TileXRMoonEpValidateReduceGradLaunch(params, *context);
    if (ret != TileXR::TILEXR_SUCCESS) {
        return ret;
    }

    std::lock_guard<std::mutex> guard(TileXRMoonEp::g_reduceGradEnqueueMutex);
    const aclError memsetRet = aclrtMemsetAsync(args->status->data, sizeof(int32_t), 0,
        sizeof(int32_t), stream);
    if (memsetRet != ACL_SUCCESS) {
        return TileXR::TILEXR_ERROR_MKIRT;
    }
    const int launchRet = TileXRMoonEp::TileXRMoonEpLaunchReduceGradKernel(
        params, *context);
    if (launchRet != TileXR::TILEXR_SUCCESS &&
        aclrtSynchronizeStream(stream) != ACL_SUCCESS) {
        return TileXR::TILEXR_ERROR_MKIRT;
    }
    return launchRet;
}
