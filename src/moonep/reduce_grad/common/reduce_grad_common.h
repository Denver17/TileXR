#ifndef TILEXR_MOONEP_REDUCE_GRAD_COMMON_H
#define TILEXR_MOONEP_REDUCE_GRAD_COMMON_H

#include <cstdint>

#include "tilexr_udma_reg.h"
#include "tilexr_udma_types.h"

namespace TileXRMoonEp {

constexpr uint64_t kReduceGradDefaultChunkBytes = UINT64_C(8) << 20;
constexpr uint64_t kReduceGradUdmaAlignment = 512;
constexpr uint64_t kReduceGradWorkspaceAlignment = UINT64_C(2) << 20;
constexpr uint64_t kReduceGradBankCount = 2;
constexpr uint64_t kReduceGradLaneStateStrideBytes = UINT64_C(8) << 10;
constexpr uint64_t kReduceGradLaneFlagStrideBytes = TileXR::TILEXR_UDMA_CACHE_LINE_SIZE;
constexpr uint64_t kReduceGradBankReadyOffset = 0;
constexpr uint64_t kReduceGradDoneTokenBytes =
    TileXR::TILEXR_UDMA_CACHE_LINE_SIZE;
constexpr uint64_t kReduceGradDoneTokenCount = 32;
constexpr uint64_t kReduceGradDoneBankStrideBytes =
    kReduceGradDoneTokenCount * kReduceGradDoneTokenBytes;
constexpr uint64_t kReduceGradBankDoneOffset =
    kReduceGradBankCount * kReduceGradLaneFlagStrideBytes;
constexpr uint64_t kReduceGradBankItemOffset =
    kReduceGradBankDoneOffset +
    kReduceGradBankCount * kReduceGradDoneBankStrideBytes;
constexpr uint64_t kReduceGradLaneErrorOffset =
    kReduceGradBankItemOffset +
    kReduceGradBankCount * kReduceGradLaneFlagStrideBytes;
static_assert(kReduceGradLaneErrorOffset + kReduceGradLaneFlagStrideBytes <=
    kReduceGradLaneStateStrideBytes, "ReduceGrad lane state exceeds its stride");

constexpr uint64_t kReduceGradKernelTileBytes = UINT64_C(40) * 1024;
constexpr uint32_t kReduceGradProjectionCount = 3;
constexpr uint32_t kReduceGradProfileRegionCount = 4;
constexpr uint32_t kReduceGradStagingRegion = 0;
constexpr int64_t kReduceGradMinRankCount = 4;
constexpr uint32_t kReduceGradMaxUdmaQpCount = 8;
constexpr uint32_t kReduceGradMaxTransportQpCount =
    TileXR::TILEXR_UDMA_PROFILE_MAX_QP_BINDINGS;
constexpr uint32_t kReduceGradMinMultiRankQpCount = kReduceGradProjectionCount;
constexpr uint32_t kReduceGradInvalidPhysicalQp = UINT32_MAX;
constexpr int64_t kReduceGradMaxAivBlockCount = 64;
constexpr int32_t kReduceGradDeviceInvalidState = 1;
constexpr int32_t kReduceGradDeviceUdmaTimeout = 2;
constexpr int32_t kReduceGradDeviceUdmaCqError = 3;
constexpr int32_t kReduceGradDeviceLeaderTimeout = 4;
constexpr int32_t kReduceGradDeviceHelperTimeout = 5;
constexpr int32_t kReduceGradBarrierArrivalStep = 0x5246;
constexpr int32_t kReduceGradBarrierStep = 0x5247;
constexpr int32_t kReduceGradBarrierFailureStep = kReduceGradBarrierStep + 1;

enum ReduceGradBankItemKind : uint32_t {
    kReduceGradBankWork = 0,
    kReduceGradBankTerminal = 1,
};

struct alignas(TileXR::TILEXR_UDMA_CACHE_LINE_SIZE) ReduceGradBankItem {
    uint64_t token = 0;
    uint64_t chunkIndex = 0;
    uint64_t chunkBytes = 0;
    uint32_t kind = kReduceGradBankTerminal;
    uint32_t projection = 0;
    uint32_t localExpert = 0;
    uint32_t waveStart = 0;
    uint32_t contributorCount = 0;
    uint32_t remoteContributorCount = 0;
    uint64_t reserved[2] = {};
};

static_assert(sizeof(ReduceGradBankItem) == kReduceGradLaneFlagStrideBytes,
    "ReduceGrad bank item must occupy exactly one cache line");

enum ReduceGradProjection : uint32_t {
    kReduceGradGate = 0,
    kReduceGradUp = 1,
    kReduceGradDown = 2,
};

struct ReduceGradLayout {
    int64_t rank = 0;
    int64_t rankSize = 0;
    int64_t expertCount = 0;
    int64_t expertsPerRank = 0;
    int64_t prefetchSlots = 0;
    int64_t blockDim = 0;

    uint64_t rowElements[kReduceGradProjectionCount] = {};
    uint64_t rowBytes[kReduceGradProjectionCount] = {};
    uint64_t chunkCounts[kReduceGradProjectionCount] = {};
    uint32_t projectionQpBase[kReduceGradProjectionCount] = {};
    uint32_t projectionQpCounts[kReduceGradProjectionCount] = {};
    uint32_t qpProjection[kReduceGradMaxUdmaQpCount] = {};
    uint32_t lanePhysicalQps[kReduceGradMaxUdmaQpCount] = {};
    uint32_t transportQpCount = 0;
    uint32_t qpCount = 0;
    uint32_t laneCount = 0;
    uint32_t barrierPhysicalQp = kReduceGradInvalidPhysicalQp;

    uint64_t laneStateBytes = 0;
    uint64_t stagingOffset = 0;
    uint64_t bankStrideBytes = 0;
    uint64_t laneStrideBytes = 0;
    uint64_t chunkBytes = 0;
    uint64_t barrierOffset = 0;
    uint64_t barrierBytes = 0;
    uint64_t workspaceBytes = 0;
};

struct ReduceGradKernelArgs {
    uint8_t *commArgs = nullptr;
    uint8_t *profileInfo = nullptr;
    uint8_t *profileRegistry = nullptr;
    uint8_t *expertsToCopy = nullptr;
    uint8_t *gradients[kReduceGradProjectionCount] = {};
    uint8_t *sources[kReduceGradProjectionCount] = {};
    uint8_t *workspace = nullptr;
    uint8_t *status = nullptr;

    int64_t rank = 0;
    int64_t rankSize = 0;
    int64_t expertCount = 0;
    int64_t expertsPerRank = 0;
    int64_t prefetchSlots = 0;
    uint64_t rowElements[kReduceGradProjectionCount] = {};
    uint64_t rowBytes[kReduceGradProjectionCount] = {};
    uint64_t chunkCounts[kReduceGradProjectionCount] = {};
    uint32_t projectionQpBase[kReduceGradProjectionCount] = {};
    uint32_t projectionQpCounts[kReduceGradProjectionCount] = {};
    uint32_t lanePhysicalQps[kReduceGradMaxUdmaQpCount] = {};
    uint32_t transportQpCount = 0;
    uint32_t qpCount = 0;
    uint32_t laneCount = 0;
    uint32_t barrierPhysicalQp = kReduceGradInvalidPhysicalQp;

    uint64_t laneStateBytes = 0;
    uint64_t stagingOffset = 0;
    uint64_t bankStrideBytes = 0;
    uint64_t laneStrideBytes = 0;
    uint64_t chunkBytes = 0;
    uint64_t barrierOffset = 0;
    uint64_t barrierBytes = 0;
    uint64_t workspaceBytes = 0;
    uint64_t waitIterations = 0;
    int64_t magic = 0;
};

} // namespace TileXRMoonEp

#endif // TILEXR_MOONEP_REDUCE_GRAD_COMMON_H
