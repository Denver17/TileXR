#include "combine_v2_host.h"

#include <cstddef>
#include <limits>

#include "acl/acl_rt.h"
#include "tilexr_types.h"

extern "C" int TileXRMoonEpCombineGetWorkspaceSizeV2(
    int64_t bs, int64_t h, int64_t topK, int64_t nvS, uint32_t dtype,
    uint64_t *workspaceBytes, uint64_t *profileOffset,
    uint64_t *outputEpoch0Offset, uint64_t *outputEpoch1Offset)
{
    if (workspaceBytes == nullptr || profileOffset == nullptr ||
        outputEpoch0Offset == nullptr || outputEpoch1Offset == nullptr) {
        return TILEXR_MOONEP_ERROR_INVALID_ARGUMENT;
    }
    *workspaceBytes = 0;
    *profileOffset = 0;
    *outputEpoch0Offset = 0;
    *outputEpoch1Offset = 0;

    TileXRMoonEp::CombineV2Layout layout {};
    const int ret = TileXRMoonEp::TileXRMoonEpBuildCombineV2Layout(
        bs, h, topK, nvS, dtype, &layout);
    if (ret != TILEXR_MOONEP_SUCCESS) {
        return ret;
    }
    *workspaceBytes = layout.totalBytes;
    *profileOffset = layout.profileOffset;
    *outputEpoch0Offset = layout.scratchOffset[0];
    *outputEpoch1Offset = layout.scratchOffset[1];
    return TILEXR_MOONEP_SUCCESS;
}

extern "C" int TileXRMoonEpCombineV2(void *registeredWorkspace,
    const int32_t *dstLocal, TileXRCommPtr comm, int64_t bs, int64_t h,
    int64_t topK, int64_t nvS, uint32_t aivCoreNum,
    uint64_t *activeOutputOffset,
    uint32_t dtype, aclrtStream stream)
{
    TileXRMoonEp::CombineV2Params params {};
    params.registeredWorkspace = registeredWorkspace;
    params.dstLocal = dstLocal;
    params.comm = comm;
    params.bs = bs;
    params.h = h;
    params.topK = topK;
    params.nvS = nvS;
    params.aivCoreNum = aivCoreNum;
    params.activeOutputOffset = activeOutputOffset;
    params.dtype = dtype;
    params.reduceHidden = false;
    params.stream = stream;
    return TileXRMoonEp::TileXRMoonEpRunCombineV2(params);
}

extern "C" int TileXRMoonEpCombineStageV2(void *registeredWorkspace,
    uint64_t registeredWorkspaceBytes, const int32_t *dstLocal,
    TileXRCommPtr comm, int64_t bs, int64_t h, int64_t topK, int64_t nvS,
    uint32_t aivCoreNum, const void *hiddenNvsh, void *hiddenSh,
    const float *routeWeightsNvs, float *routeWeightsSk,
    uint32_t dtype, aclrtStream stream)
{
    if (registeredWorkspace == nullptr || registeredWorkspaceBytes == 0U ||
        registeredWorkspaceBytes > std::numeric_limits<std::size_t>::max() ||
        dstLocal == nullptr || comm == nullptr || hiddenNvsh == nullptr ||
        hiddenSh == nullptr || stream == nullptr ||
        dtype != TILEXR_MOONEP_DTYPE_BFLOAT16 ||
        ((routeWeightsNvs == nullptr) != (routeWeightsSk == nullptr))) {
        return TILEXR_MOONEP_ERROR_INVALID_ARGUMENT;
    }

    TileXRMoonEp::CombineV2Layout hiddenLayout {};
    int ret = TileXRMoonEp::TileXRMoonEpBuildCombineV2Layout(
        bs, h, topK, nvS, dtype, &hiddenLayout);
    if (ret != TILEXR_MOONEP_SUCCESS ||
        registeredWorkspaceBytes < hiddenLayout.totalBytes) {
        return ret == TILEXR_MOONEP_SUCCESS ?
            TILEXR_MOONEP_ERROR_INVALID_ARGUMENT : ret;
    }

    const std::size_t workspaceBytes = static_cast<std::size_t>(
        registeredWorkspaceBytes);
    const std::size_t hiddenInputBytes = static_cast<std::size_t>(
        hiddenLayout.expertBytes);
    const std::size_t hiddenOutputBytes = static_cast<std::size_t>(
        hiddenLayout.outputBytes);
    if (aclrtMemcpyAsync(registeredWorkspace, workspaceBytes, hiddenNvsh,
            hiddenInputBytes, ACL_MEMCPY_DEVICE_TO_DEVICE, stream) != ACL_SUCCESS) {
        return TILEXR_MOONEP_ERROR_INTERNAL;
    }

    uint64_t activeOutputOffset = 0U;
    TileXRMoonEp::CombineV2Params params {};
    params.registeredWorkspace = registeredWorkspace;
    params.dstLocal = dstLocal;
    params.comm = comm;
    params.bs = bs;
    params.h = h;
    params.topK = topK;
    params.nvS = nvS;
    params.aivCoreNum = aivCoreNum;
    params.activeOutputOffset = &activeOutputOffset;
    params.dtype = dtype;
    params.reduceHidden = true;
    params.stream = stream;
    ret = TileXRMoonEp::TileXRMoonEpRunCombineV2(params);
    if (ret != TILEXR_MOONEP_SUCCESS) {
        return ret;
    }
    const uint8_t *hiddenOutput = static_cast<const uint8_t *>(
        registeredWorkspace) + hiddenLayout.outputOffset;
    if (aclrtMemcpyAsync(hiddenSh, hiddenOutputBytes, hiddenOutput,
            hiddenOutputBytes, ACL_MEMCPY_DEVICE_TO_DEVICE, stream) != ACL_SUCCESS) {
        return TILEXR_MOONEP_ERROR_INTERNAL;
    }

    if (routeWeightsNvs == nullptr) {
        return TILEXR_MOONEP_SUCCESS;
    }

    TileXRMoonEp::CombineV2Layout weightLayout {};
    ret = TileXRMoonEp::TileXRMoonEpBuildCombineV2Layout(
        bs, 1, topK, nvS, TILEXR_MOONEP_DTYPE_FLOAT32, &weightLayout);
    if (ret != TILEXR_MOONEP_SUCCESS ||
        registeredWorkspaceBytes < weightLayout.totalBytes) {
        return ret == TILEXR_MOONEP_SUCCESS ?
            TILEXR_MOONEP_ERROR_INVALID_ARGUMENT : ret;
    }
    const std::size_t weightInputBytes = static_cast<std::size_t>(
        weightLayout.expertBytes);
    if (aclrtMemcpyAsync(registeredWorkspace, workspaceBytes,
            routeWeightsNvs, weightInputBytes, ACL_MEMCPY_DEVICE_TO_DEVICE,
            stream) != ACL_SUCCESS) {
        return TILEXR_MOONEP_ERROR_INTERNAL;
    }

    params.h = 1;
    params.dtype = TILEXR_MOONEP_DTYPE_FLOAT32;
    params.reduceHidden = false;
    activeOutputOffset = 0U;
    ret = TileXRMoonEp::TileXRMoonEpRunCombineV2(params);
    if (ret != TILEXR_MOONEP_SUCCESS) {
        return ret;
    }
    const std::size_t weightOutputBytes = static_cast<std::size_t>(
        static_cast<uint64_t>(bs) * static_cast<uint64_t>(topK) *
            sizeof(float));
    const uint8_t *weightOutput = static_cast<const uint8_t *>(
        registeredWorkspace) + activeOutputOffset;
    if (aclrtMemcpyAsync(routeWeightsSk, weightOutputBytes, weightOutput,
            weightOutputBytes, ACL_MEMCPY_DEVICE_TO_DEVICE, stream) != ACL_SUCCESS) {
        return TILEXR_MOONEP_ERROR_INTERNAL;
    }
    return TILEXR_MOONEP_SUCCESS;
}

extern "C" int TileXRMoonEpCombineStageV2Fused(void *registeredWorkspace,
    uint64_t registeredWorkspaceBytes, const int32_t *dstLocal,
    TileXRCommPtr comm, TileXRCommPtr weightMemoryComm,
    int64_t bs, int64_t h, int64_t topK, int64_t nvS,
    uint32_t aivCoreNum, const void *hiddenNvsh, void *hiddenSh,
    const float *routeWeightsNvs, float *routeWeightsSk,
    uint32_t dtype, aclrtStream stream)
{
    if (registeredWorkspace == nullptr || registeredWorkspaceBytes == 0U ||
        registeredWorkspaceBytes > std::numeric_limits<std::size_t>::max() ||
        dstLocal == nullptr || comm == nullptr || hiddenNvsh == nullptr ||
        hiddenSh == nullptr || stream == nullptr ||
        dtype != TILEXR_MOONEP_DTYPE_BFLOAT16 ||
        ((routeWeightsNvs == nullptr) != (routeWeightsSk == nullptr))) {
        return TILEXR_MOONEP_ERROR_INVALID_ARGUMENT;
    }

    TileXR::CommArgs *hostArgs = nullptr;
    int ret = TileXRGetCommArgsHost(comm, hostArgs);
    if (ret != TileXR::TILEXR_SUCCESS || hostArgs == nullptr) {
        return ret == TileXR::TILEXR_SUCCESS ?
            TILEXR_MOONEP_ERROR_INTERNAL : ret;
    }
    if (hostArgs->localRankSize < hostArgs->rankSize) {
        return TileXRMoonEpCombineStageV2(registeredWorkspace,
            registeredWorkspaceBytes, dstLocal, comm, bs, h, topK, nvS,
            aivCoreNum, hiddenNvsh, hiddenSh, routeWeightsNvs,
            routeWeightsSk, dtype, stream);
    }
    if (routeWeightsNvs != nullptr && weightMemoryComm == nullptr) {
        return TILEXR_MOONEP_ERROR_INVALID_ARGUMENT;
    }

    TileXRMoonEp::CombineV2Layout hiddenLayout {};
    ret = TileXRMoonEp::TileXRMoonEpBuildCombineV2Layout(
        bs, h, topK, nvS, dtype, &hiddenLayout);
    if (ret != TILEXR_MOONEP_SUCCESS ||
        registeredWorkspaceBytes < hiddenLayout.totalBytes) {
        return ret == TILEXR_MOONEP_SUCCESS ?
            TILEXR_MOONEP_ERROR_INVALID_ARGUMENT : ret;
    }

    const std::size_t workspaceBytes = static_cast<std::size_t>(
        registeredWorkspaceBytes);
    const std::size_t hiddenInputBytes = static_cast<std::size_t>(
        hiddenLayout.expertBytes);
    const std::size_t hiddenOutputBytes = static_cast<std::size_t>(
        hiddenLayout.outputBytes);
    if (aclrtMemcpyAsync(registeredWorkspace, workspaceBytes, hiddenNvsh,
            hiddenInputBytes, ACL_MEMCPY_DEVICE_TO_DEVICE, stream) !=
            ACL_SUCCESS) {
        return TILEXR_MOONEP_ERROR_INTERNAL;
    }

    uint64_t activeOutputOffset = 0U;
    TileXRMoonEp::CombineV2Params params {};
    params.registeredWorkspace = registeredWorkspace;
    params.dstLocal = dstLocal;
    params.comm = comm;
    params.weightMemoryComm = weightMemoryComm;
    params.bs = bs;
    params.h = h;
    params.topK = topK;
    params.nvS = nvS;
    params.aivCoreNum = aivCoreNum;
    params.activeOutputOffset = &activeOutputOffset;
    params.dtype = dtype;
    params.reduceHidden = true;
    params.routeWeightsNvs = routeWeightsNvs;
    params.routeWeightsSk = routeWeightsSk;
    params.stream = stream;
    ret = TileXRMoonEp::TileXRMoonEpRunCombineV2(params);
    if (ret != TILEXR_MOONEP_SUCCESS) {
        return ret;
    }

    const uint8_t *hiddenOutput = static_cast<const uint8_t *>(
        registeredWorkspace) + hiddenLayout.outputOffset;
    if (aclrtMemcpyAsync(hiddenSh, hiddenOutputBytes, hiddenOutput,
            hiddenOutputBytes, ACL_MEMCPY_DEVICE_TO_DEVICE, stream) !=
            ACL_SUCCESS) {
        return TILEXR_MOONEP_ERROR_INTERNAL;
    }
    return TILEXR_MOONEP_SUCCESS;
}
