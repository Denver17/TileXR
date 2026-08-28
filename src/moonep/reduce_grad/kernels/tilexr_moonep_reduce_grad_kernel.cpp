#include "kernel_operator.h"
#include <cstdint>

#include "comm_args.h"
#include "reduce_grad_common.h"
#include "tilexr_sync.h"
#include "tilexr_udma.h"

namespace TileXRMoonEp {
namespace Kernel {

constexpr uint32_t kSyncBufferBytes =
    TileXR::TILEXR_MAX_RANK_SIZE * 4U * sizeof(int64_t);
constexpr uint32_t kWqeBufferBytes = TileXR::TILEXR_UDMA_WQE_SCRATCH_BYTES;
constexpr uint32_t kItemBufferBytes = sizeof(ReduceGradBankItem);

struct ReduceGradPendingWave {
    bool used[TileXR::TILEXR_MAX_RANK_SIZE];
    uint32_t frontiers[TileXR::TILEXR_MAX_RANK_SIZE];
    uint64_t chunkIndex;
    uint64_t itemChunkBytes;
    uint32_t sequence;
    uint32_t projection;
    uint32_t localExpert;
    uint32_t waveStart;
    uint32_t contributorCount;
    uint32_t remoteContributorCount;
};

template <AscendC::HardEvent event>
__aicore__ inline void SyncEvent(AscendC::TEventID eventId)
{
    AscendC::SetFlag<event>(eventId);
    AscendC::WaitFlag<event>(eventId);
}

__aicore__ inline uint64_t MinU64(uint64_t lhs, uint64_t rhs)
{
    return lhs < rhs ? lhs : rhs;
}

__aicore__ inline uint64_t CeilDivU64(uint64_t value, uint64_t divisor)
{
    return divisor == 0U ? 0U : value / divisor + (value % divisor == 0U ? 0U : 1U);
}

class ReduceGradKernel {
public:
    __aicore__ inline void Init(GM_ADDR commArgs, GM_ADDR profileInfo,
        GM_ADDR profileRegistry, GM_ADDR expertsToCopy,
        GM_ADDR gateGradient, GM_ADDR upGradient, GM_ADDR downGradient,
        GM_ADDR gateSource, GM_ADDR upSource, GM_ADDR downSource,
        GM_ADDR workspace, GM_ADDR status,
        int64_t rank, int64_t rankSize, int64_t expertCount,
        int64_t expertsPerRank, int64_t prefetchSlots,
        uint64_t gateRowElements, uint64_t upRowElements,
        uint64_t downRowElements, uint64_t gateRowBytes,
        uint64_t upRowBytes, uint64_t downRowBytes,
        uint64_t gateChunkCount, uint64_t upChunkCount,
        uint64_t downChunkCount, uint32_t gateQpBase,
        uint32_t upQpBase, uint32_t downQpBase,
        uint32_t gateQpCount, uint32_t upQpCount,
        uint32_t downQpCount, uint32_t lane0PhysicalQp,
        uint32_t lane1PhysicalQp, uint32_t lane2PhysicalQp,
        uint32_t lane3PhysicalQp, uint32_t lane4PhysicalQp,
        uint32_t lane5PhysicalQp, uint32_t lane6PhysicalQp,
        uint32_t lane7PhysicalQp, uint32_t transportQpCount,
        uint32_t qpCount, uint32_t laneCount, uint32_t barrierPhysicalQp,
        uint64_t laneStateBytes, uint64_t stagingOffset,
        uint64_t bankStrideBytes, uint64_t laneStrideBytes,
        uint64_t chunkBytes, uint64_t barrierOffset,
        uint64_t barrierBytes, uint64_t workspaceBytes,
        uint64_t waitIterations, int64_t magic)
    {
        args_ = reinterpret_cast<__gm__ TileXR::CommArgs *>(commArgs);
        profileInfo_ = reinterpret_cast<__gm__ TileXR::UDMAInfo *>(profileInfo);
        profileRegistry_ = reinterpret_cast<__gm__ TileXR::TileXRUDMAProfileRegistry *>(
            profileRegistry);
        expertsToCopy_ = reinterpret_cast<__gm__ int32_t *>(expertsToCopy);
        gradients_[kReduceGradGate] = gateGradient;
        gradients_[kReduceGradUp] = upGradient;
        gradients_[kReduceGradDown] = downGradient;
        sources_[kReduceGradGate] = gateSource;
        sources_[kReduceGradUp] = upSource;
        sources_[kReduceGradDown] = downSource;
        workspace_ = workspace;
        status_ = reinterpret_cast<__gm__ uint32_t *>(status);
        rank_ = rank;
        rankSize_ = rankSize;
        expertCount_ = expertCount;
        expertsPerRank_ = expertsPerRank;
        prefetchSlots_ = prefetchSlots;
        rowElements_[kReduceGradGate] = gateRowElements;
        rowElements_[kReduceGradUp] = upRowElements;
        rowElements_[kReduceGradDown] = downRowElements;
        rowBytes_[kReduceGradGate] = gateRowBytes;
        rowBytes_[kReduceGradUp] = upRowBytes;
        rowBytes_[kReduceGradDown] = downRowBytes;
        chunkCounts_[kReduceGradGate] = gateChunkCount;
        chunkCounts_[kReduceGradUp] = upChunkCount;
        chunkCounts_[kReduceGradDown] = downChunkCount;
        projectionQpBase_[kReduceGradGate] = gateQpBase;
        projectionQpBase_[kReduceGradUp] = upQpBase;
        projectionQpBase_[kReduceGradDown] = downQpBase;
        projectionQpCounts_[kReduceGradGate] = gateQpCount;
        projectionQpCounts_[kReduceGradUp] = upQpCount;
        projectionQpCounts_[kReduceGradDown] = downQpCount;
        lanePhysicalQps_[0] = lane0PhysicalQp;
        lanePhysicalQps_[1] = lane1PhysicalQp;
        lanePhysicalQps_[2] = lane2PhysicalQp;
        lanePhysicalQps_[3] = lane3PhysicalQp;
        lanePhysicalQps_[4] = lane4PhysicalQp;
        lanePhysicalQps_[5] = lane5PhysicalQp;
        lanePhysicalQps_[6] = lane6PhysicalQp;
        lanePhysicalQps_[7] = lane7PhysicalQp;
        transportQpCount_ = transportQpCount;
        qpCount_ = qpCount;
        laneCount_ = laneCount;
        barrierPhysicalQp_ = barrierPhysicalQp;
        laneStateBytes_ = laneStateBytes;
        stagingOffset_ = stagingOffset;
        bankStrideBytes_ = bankStrideBytes;
        laneStrideBytes_ = laneStrideBytes;
        chunkBytes_ = chunkBytes;
        barrierOffset_ = barrierOffset;
        barrierBytes_ = barrierBytes;
        workspaceBytes_ = workspaceBytes;
        waitIterations_ = waitIterations == 0U ? 1U : waitIterations;
        magic_ = magic;
        blockIdx_ = static_cast<uint32_t>(AscendC::GetBlockIdx());
        blockCount_ = static_cast<uint32_t>(AscendC::GetBlockNum());

        pipe_.InitBuffer(accumBuf_, kReduceGradKernelTileBytes);
        pipe_.InitBuffer(accumPongBuf_, kReduceGradKernelTileBytes);
        pipe_.InitBuffer(inputPingBuf_, kReduceGradKernelTileBytes);
        pipe_.InitBuffer(inputPongBuf_, kReduceGradKernelTileBytes);
        pipe_.InitBuffer(syncBuf_, kSyncBufferBytes);
        pipe_.InitBuffer(wqeBuf_, kWqeBufferBytes);
        pipe_.InitBuffer(itemBuf_, kItemBufferBytes);
    }

    __aicore__ inline void Process()
    {
        if (args_ == nullptr || status_ == nullptr) {
            return;
        }
        if (blockIdx_ == 0U) {
            AscendC::AtomicExch(status_, static_cast<uint32_t>(0U));
        }
        AscendC::SyncAll<true>();

        if (!CollectiveConfigurationValid()) {
            PublishStatus(kReduceGradDeviceInvalidState);
            return;
        }
        const bool configured = ConfigurationValid();
        if (!configured) {
            PublishStatus(kReduceGradDeviceInvalidState);
        } else {
            ValidatePlanEntries();
        }
        AscendC::SyncAll<true>();
        if (DeviceStatus() == 0U) {
            if (blockIdx_ < laneCount_) {
                RunLaneLeader(blockIdx_);
            } else {
                RunLaneHelper();
            }
        }

        AscendC::SyncAll<true>();
        if (blockIdx_ == 0U) {
            CrossRankBarrier();
        }
        AscendC::SyncAll<true>();
        if (DeviceStatus() == 0U) {
            ClearLocalSources();
        }
    }

private:
    __aicore__ inline bool CollectiveConfigurationValid() const
    {
        return rank_ >= 0 && rank_ < rankSize_ &&
            rankSize_ >= kReduceGradMinRankCount &&
            rankSize_ <= TileXR::TILEXR_MAX_RANK_SIZE && blockCount_ != 0U &&
            blockIdx_ < blockCount_ && magic_ > 0 && args_->peerMems[rank_] != nullptr;
    }

    __aicore__ inline bool DataPointersValid() const
    {
        if (expertsToCopy_ == nullptr) {
            return false;
        }
        for (uint32_t projection = 0U;
            projection < kReduceGradProjectionCount; ++projection) {
            if (gradients_[projection] == nullptr || sources_[projection] == nullptr) {
                return false;
            }
        }
        return true;
    }

    __aicore__ inline bool ConfigurationValid() const
    {
        const bool crossNode = args_->localRankSize > 0 &&
            args_->localRankSize < rankSize_;
        if (!DataPointersValid() || expertCount_ <= 0 ||
            expertsPerRank_ <= 0 || expertCount_ != expertsPerRank_ * rankSize_ ||
            expertCount_ > INT32_MAX || prefetchSlots_ <= 0 ||
            prefetchSlots_ > INT32_MAX || prefetchSlots_ > INT32_MAX / rankSize_ ||
            blockCount_ < 2U * laneCount_) {
            return false;
        }
        for (uint32_t projection = 0U;
            projection < kReduceGradProjectionCount; ++projection) {
            if (rowElements_[projection] == 0U || rowBytes_[projection] == 0U ||
                rowBytes_[projection] != rowElements_[projection] * sizeof(float)) {
                return false;
            }
        }
        if (workspace_ == nullptr || profileInfo_ == nullptr ||
            profileRegistry_ == nullptr ||
            transportQpCount_ < kReduceGradMinMultiRankQpCount ||
            transportQpCount_ > kReduceGradMaxTransportQpCount ||
            qpCount_ < kReduceGradProjectionCount ||
            qpCount_ > kReduceGradMaxUdmaQpCount || laneCount_ != qpCount_ ||
            chunkBytes_ == 0U ||
            chunkBytes_ > UINT32_MAX || laneStateBytes_ <
                laneCount_ * kReduceGradLaneStateStrideBytes ||
            stagingOffset_ != laneStateBytes_ ||
            bankStrideBytes_ != static_cast<uint64_t>(rankSize_) * chunkBytes_ ||
            laneStrideBytes_ != kReduceGradBankCount * bankStrideBytes_ ||
            workspaceBytes_ < stagingOffset_ + laneCount_ * laneStrideBytes_ ||
            !TileXR::UDMAProfileRegistryValid(args_, profileInfo_, profileRegistry_) ||
            profileRegistry_->regionCount != kReduceGradProfileRegionCount ||
            profileRegistry_->qpCount != transportQpCount_) {
            return false;
        }
        const uint64_t payloadEnd = stagingOffset_ +
            static_cast<uint64_t>(laneCount_) * laneStrideBytes_;
        if (crossNode) {
            if (barrierPhysicalQp_ >= transportQpCount_ ||
                barrierBytes_ != static_cast<uint64_t>(rankSize_) *
                    TileXR::TILEXR_UDMA_CACHE_LINE_SIZE ||
                barrierOffset_ < payloadEnd ||
                barrierOffset_ % TileXR::TILEXR_UDMA_CACHE_LINE_SIZE != 0U ||
                barrierOffset_ > workspaceBytes_ ||
                barrierBytes_ > workspaceBytes_ - barrierOffset_) {
                return false;
            }
        } else if (barrierPhysicalQp_ != kReduceGradInvalidPhysicalQp ||
            barrierOffset_ != 0U || barrierBytes_ != 0U) {
            return false;
        }
        for (uint32_t lane = 0U; lane < laneCount_; ++lane) {
            if (lanePhysicalQps_[lane] >= transportQpCount_) {
                return false;
            }
            if (crossNode && lanePhysicalQps_[lane] == barrierPhysicalQp_) {
                return false;
            }
            for (uint32_t prior = 0U; prior < lane; ++prior) {
                if (lanePhysicalQps_[prior] == lanePhysicalQps_[lane]) {
                    return false;
                }
            }
        }
        uint32_t cursor = 0U;
        for (uint32_t projection = 0U;
            projection < kReduceGradProjectionCount; ++projection) {
            if (projectionQpCounts_[projection] == 0U ||
                projectionQpBase_[projection] != cursor ||
                chunkCounts_[projection] != CeilDivU64(
                    rowBytes_[projection], chunkBytes_)) {
                return false;
            }
            cursor += projectionQpCounts_[projection];
        }
        return cursor == qpCount_;
    }

    __attribute__((always_inline)) inline __aicore__ void PublishStatus(uint32_t value)
    {
        if (value != 0U) {
            AscendC::AtomicCas(status_, static_cast<uint32_t>(0U), value);
        }
    }

    __aicore__ inline uint32_t DeviceStatus() const
    {
        return AscendC::AtomicAdd(status_, static_cast<uint32_t>(0U));
    }

    __aicore__ inline void ValidatePlanEntries()
    {
        const uint64_t entries = static_cast<uint64_t>(rankSize_) *
            static_cast<uint64_t>(prefetchSlots_);
        for (uint64_t entry = blockIdx_; entry < entries; entry += blockCount_) {
            const int32_t expert = expertsToCopy_[entry];
            if (expert < -1 || expert >= expertCount_) {
                PublishStatus(kReduceGradDeviceInvalidState);
            }
        }
    }

    __aicore__ inline uint64_t LaneToken(uint32_t sequence) const
    {
        return (static_cast<uint64_t>(static_cast<uint32_t>(magic_)) << 32U) |
            static_cast<uint64_t>(sequence);
    }

    __aicore__ inline uint32_t ProjectionForLane(uint32_t lane) const
    {
        for (uint32_t projection = 0U;
            projection < kReduceGradProjectionCount; ++projection) {
            const uint32_t begin = projectionQpBase_[projection];
            if (lane >= begin && lane < begin + projectionQpCounts_[projection]) {
                return projection;
            }
        }
        return kReduceGradProjectionCount;
    }

    __attribute__((always_inline)) inline __aicore__ uint32_t PhysicalQp(
        uint32_t lane) const
    {
        return lanePhysicalQps_[lane];
    }

    __aicore__ inline uint64_t LoadLaneToken(__gm__ uint64_t *token) const
    {
        TileXR::UDMACleanCacheLines(
            reinterpret_cast<__gm__ uint8_t *>(token), sizeof(uint64_t));
        return *token;
    }

    __aicore__ inline void StoreLaneToken(
        __gm__ uint64_t *token, uint64_t value) const
    {
        *token = value;
        TileXR::UDMACleanCacheLines(
            reinterpret_cast<__gm__ uint8_t *>(token), sizeof(uint64_t));
    }

    __aicore__ inline __gm__ uint64_t *BankReady(uint32_t lane, uint32_t bank) const
    {
        return reinterpret_cast<__gm__ uint64_t *>(workspace_ +
            static_cast<uint64_t>(lane) * kReduceGradLaneStateStrideBytes +
            kReduceGradBankReadyOffset +
            static_cast<uint64_t>(bank) * kReduceGradLaneFlagStrideBytes);
    }

    __aicore__ inline __gm__ uint64_t *BankDoneToken(
        uint32_t lane, uint32_t bank, uint32_t helperIndex) const
    {
        return reinterpret_cast<__gm__ uint64_t *>(workspace_ +
            static_cast<uint64_t>(lane) * kReduceGradLaneStateStrideBytes +
            kReduceGradBankDoneOffset +
            static_cast<uint64_t>(bank) * kReduceGradDoneBankStrideBytes +
            static_cast<uint64_t>(helperIndex) * kReduceGradDoneTokenBytes);
    }

    __aicore__ inline __gm__ ReduceGradBankItem *BankItem(
        uint32_t lane, uint32_t bank) const
    {
        return reinterpret_cast<__gm__ ReduceGradBankItem *>(workspace_ +
            static_cast<uint64_t>(lane) * kReduceGradLaneStateStrideBytes +
            kReduceGradBankItemOffset +
            static_cast<uint64_t>(bank) * kReduceGradLaneFlagStrideBytes);
    }

    __aicore__ inline __gm__ uint64_t *LaneAbort(uint32_t lane) const
    {
        return reinterpret_cast<__gm__ uint64_t *>(workspace_ +
            static_cast<uint64_t>(lane) * kReduceGradLaneStateStrideBytes +
            kReduceGradLaneErrorOffset);
    }

    __aicore__ inline uint64_t LaneAbortToken() const
    {
        return LaneToken(UINT32_MAX);
    }

    __aicore__ inline bool LaneAborted(uint32_t lane) const
    {
        return LoadLaneToken(LaneAbort(lane)) == LaneAbortToken();
    }

    __aicore__ inline void AbortLane(uint32_t lane)
    {
        StoreLaneToken(LaneAbort(lane), LaneAbortToken());
    }

    __aicore__ inline void FailLane(uint32_t lane)
    {
        AbortLane(lane);
    }

    __aicore__ inline GM_ADDR BankPayload(uint32_t lane, uint32_t bank) const
    {
        return workspace_ + stagingOffset_ +
            static_cast<uint64_t>(lane) * laneStrideBytes_ +
            static_cast<uint64_t>(bank) * bankStrideBytes_;
    }

    __aicore__ inline void HelperShape(uint32_t lane,
        uint32_t &helperCount, uint32_t &helperBegin) const
    {
        const uint32_t totalHelpers = blockCount_ - laneCount_;
        const uint32_t base = totalHelpers / laneCount_;
        const uint32_t extra = totalHelpers % laneCount_;
        helperCount = base + (lane < extra ? 1U : 0U);
        helperBegin = laneCount_ + lane * base + (lane < extra ? lane : extra);
    }

    __aicore__ inline void HelperLane(uint32_t &lane,
        uint32_t &helperIndex, uint32_t &helperCount) const
    {
        const uint32_t ordinal = blockIdx_ - laneCount_;
        const uint32_t totalHelpers = blockCount_ - laneCount_;
        const uint32_t base = totalHelpers / laneCount_;
        const uint32_t extra = totalHelpers % laneCount_;
        const uint32_t wider = (base + 1U) * extra;
        if (ordinal < wider) {
            lane = ordinal / (base + 1U);
            helperIndex = ordinal % (base + 1U);
            helperCount = base + 1U;
        } else {
            const uint32_t tailOrdinal = ordinal - wider;
            lane = extra + tailOrdinal / base;
            helperIndex = tailOrdinal % base;
            helperCount = base;
        }
    }

    __aicore__ inline uint32_t CountContributors(int32_t globalExpert) const
    {
        uint32_t count = 0U;
        const uint64_t entries = static_cast<uint64_t>(rankSize_) *
            static_cast<uint64_t>(prefetchSlots_);
        for (uint64_t entry = 0U; entry < entries; ++entry) {
            if (expertsToCopy_[entry] == globalExpert) {
                ++count;
            }
        }
        return count;
    }

    __attribute__((always_inline)) inline __aicore__ bool IssueWave(
        uint32_t lane, uint32_t projection,
        uint32_t bank, int32_t globalExpert, uint64_t chunkIndex,
        uint32_t waveStart, uint32_t contributorCount,
        uint32_t sequence, ReduceGradPendingWave &pending)
    {
        AscendC::LocalTensor<uint8_t> wqeScratch = wqeBuf_.Get<uint8_t>();
        const uint64_t chunkOffset = chunkIndex * chunkBytes_;
        const uint64_t bytes = MinU64(
            rowBytes_[projection] - chunkOffset, chunkBytes_);
        for (int32_t sourceRank = 0; sourceRank < rankSize_; ++sourceRank) {
            pending.used[sourceRank] = false;
            pending.frontiers[sourceRank] = 0U;
        }
        pending.chunkIndex = chunkIndex;
        pending.itemChunkBytes = bytes;
        pending.sequence = sequence;
        pending.projection = projection;
        pending.localExpert = static_cast<uint32_t>(
            globalExpert - rank_ * expertsPerRank_);
        pending.waveStart = waveStart;
        pending.contributorCount = contributorCount;
        pending.remoteContributorCount = 0U;
        uint32_t matching = 0U;
        uint32_t collected = 0U;
        bool ok = true;

        const uint64_t entries = static_cast<uint64_t>(rankSize_) *
            static_cast<uint64_t>(prefetchSlots_);
        for (uint64_t entry = 0U; entry < entries && collected < contributorCount;
            ++entry) {
            if (expertsToCopy_[entry] != globalExpert) {
                continue;
            }
            if (matching++ < waveStart) {
                continue;
            }
            const int32_t sourceRank = static_cast<int32_t>(
                entry / static_cast<uint64_t>(prefetchSlots_));
            const uint32_t slot = static_cast<uint32_t>(
                entry % static_cast<uint64_t>(prefetchSlots_));
            if (sourceRank != rank_) {
                const uint64_t localOffset = stagingOffset_ +
                    static_cast<uint64_t>(lane) * laneStrideBytes_ +
                    static_cast<uint64_t>(bank) * bankStrideBytes_ +
                    static_cast<uint64_t>(pending.remoteContributorCount) * chunkBytes_;
                const uint64_t remoteOffset = static_cast<uint64_t>(slot) *
                    rowBytes_[projection] + chunkOffset;
                const uint32_t post = TileXR::UDMAProfileGetNbiOnQpDeferred(
                    args_, profileInfo_, profileRegistry_, wqeScratch,
                    sourceRank, PhysicalQp(lane), kReduceGradStagingRegion, localOffset,
                    projection + 1U, remoteOffset, bytes);
                if (post != TileXR::TILEXR_UDMA_STATUS_SUCCESS) {
                    ok = false;
                    PublishStatus(kReduceGradDeviceUdmaCqError);
                } else {
                    pending.used[sourceRank] = true;
                }
                ++pending.remoteContributorCount;
            }
            ++collected;
        }
        if (collected != contributorCount) {
            ok = false;
            PublishStatus(kReduceGradDeviceInvalidState);
        }

        for (int32_t sourceRank = 0; sourceRank < rankSize_; ++sourceRank) {
            if (pending.used[sourceRank]) {
                pending.frontiers[sourceRank] = TileXR::UDMAProfileCompletionFrontier(
                    args_, profileInfo_, profileRegistry_, sourceRank,
                    PhysicalQp(lane));
            }
        }
        for (int32_t sourceRank = 0; sourceRank < rankSize_; ++sourceRank) {
            if (!pending.used[sourceRank]) {
                continue;
            }
            const uint32_t flush = TileXR::UDMAProfileFlushQpDoorbell(
                args_, profileInfo_, profileRegistry_, sourceRank,
                PhysicalQp(lane));
            if (flush != TileXR::TILEXR_UDMA_STATUS_SUCCESS) {
                ok = false;
                PublishStatus(kReduceGradDeviceUdmaCqError);
            }
        }
        return ok;
    }

    __attribute__((always_inline)) inline __aicore__ bool CompleteWave(
        uint32_t lane, const ReduceGradPendingWave &pending)
    {
        bool ok = true;
        for (int32_t sourceRank = 0; sourceRank < rankSize_; ++sourceRank) {
            if (!pending.used[sourceRank]) {
                continue;
            }
            const uint32_t quiet = TileXR::UDMAProfileQuietStatusOnQpUntil(
                args_, profileInfo_, profileRegistry_, sourceRank,
                PhysicalQp(lane),
                pending.frontiers[sourceRank]);
            if (quiet != TileXR::TILEXR_UDMA_STATUS_SUCCESS) {
                ok = false;
                PublishStatus(quiet == TileXR::TILEXR_UDMA_STATUS_CQ_TIMEOUT ?
                    kReduceGradDeviceUdmaTimeout : kReduceGradDeviceUdmaCqError);
            }
        }
        return ok;
    }

    __aicore__ inline bool WaitForHelpers(uint32_t lane, uint32_t bank,
        uint32_t helperCount, uint32_t sequence)
    {
        const uint64_t expected = LaneToken(sequence);
        for (uint32_t helper = 0U; helper < helperCount; ++helper) {
            uint64_t attempts = 0U;
            while (LoadLaneToken(BankDoneToken(lane, bank, helper)) != expected) {
                if (LaneAborted(lane)) {
                    return false;
                }
                if (++attempts >= waitIterations_) {
                    PublishStatus(kReduceGradDeviceLeaderTimeout);
                    AbortLane(lane);
                    return false;
                }
            }
        }
        return true;
    }

    __attribute__((always_inline)) inline __aicore__ void PublishItem(
        uint32_t lane, uint32_t bank,
        uint64_t token, uint64_t chunkIndex, uint64_t itemChunkBytes,
        uint32_t kind, uint32_t projection, uint32_t localExpert,
        uint32_t waveStart, uint32_t contributorCount,
        uint32_t remoteContributorCount)
    {
        AscendC::LocalTensor<uint64_t> local = itemBuf_.Get<uint64_t>();
        __ubuf__ uint64_t *localWords = reinterpret_cast<__ubuf__ uint64_t *>(
            local.GetPhyAddr());
        localWords[0] = token;
        localWords[1] = chunkIndex;
        localWords[2] = itemChunkBytes;
        localWords[3] = PackUint32Pair(kind, projection);
        localWords[4] = PackUint32Pair(localExpert, waveStart);
        localWords[5] = PackUint32Pair(
            contributorCount, remoteContributorCount);
        localWords[6] = 0U;
        localWords[7] = 0U;
        AscendC::GlobalTensor<uint64_t> destination;
        destination.SetGlobalBuffer(
            reinterpret_cast<__gm__ uint64_t *>(BankItem(lane, bank)),
            sizeof(ReduceGradBankItem) / sizeof(uint64_t));
        const AscendC::DataCopyExtParams params {
            1U, sizeof(ReduceGradBankItem), 0U, 0U, 0U};
        SyncEvent<AscendC::HardEvent::S_MTE3>(EVENT_ID3);
        AscendC::DataCopyPad(destination, local, params);
        SyncEvent<AscendC::HardEvent::MTE3_S>(EVENT_ID3);
        StoreLaneToken(BankReady(lane, bank), token);
    }

    __aicore__ inline uint64_t PackUint32Pair(uint32_t low, uint32_t high) const
    {
        return static_cast<uint64_t>(low) |
            (static_cast<uint64_t>(high) << 32U);
    }

    __attribute__((always_inline)) inline __aicore__ void LoadItem(
        uint32_t lane, uint32_t bank, uint64_t &token, uint64_t &chunkIndex,
        uint64_t &itemChunkBytes, uint32_t &kind, uint32_t &projection,
        uint32_t &localExpert, uint32_t &waveStart, uint32_t &contributorCount,
        uint32_t &remoteContributorCount)
    {
        AscendC::LocalTensor<uint64_t> local = itemBuf_.Get<uint64_t>();
        __ubuf__ uint64_t *localWords = reinterpret_cast<__ubuf__ uint64_t *>(
            local.GetPhyAddr());
        AscendC::GlobalTensor<uint64_t> source;
        source.SetGlobalBuffer(
            reinterpret_cast<__gm__ uint64_t *>(BankItem(lane, bank)),
            sizeof(ReduceGradBankItem) / sizeof(uint64_t));
        const AscendC::DataCopyExtParams params {
            1U, sizeof(ReduceGradBankItem), 0U, 0U, 0U};
        const AscendC::DataCopyPadExtParams<uint64_t> pad {false, 0U, 0U, 0U};
        AscendC::DataCopyPad(local, source, params, pad);
        SyncEvent<AscendC::HardEvent::MTE2_S>(EVENT_ID3);

        token = localWords[0];
        chunkIndex = localWords[1];
        itemChunkBytes = localWords[2];
        const uint64_t kindProjection = localWords[3];
        const uint64_t expertWave = localWords[4];
        const uint64_t contributors = localWords[5];
        kind = static_cast<uint32_t>(kindProjection);
        projection = static_cast<uint32_t>(kindProjection >> 32U);
        localExpert = static_cast<uint32_t>(expertWave);
        waveStart = static_cast<uint32_t>(expertWave >> 32U);
        contributorCount = static_cast<uint32_t>(contributors);
        remoteContributorCount = static_cast<uint32_t>(contributors >> 32U);
    }

    __aicore__ inline void RunLaneLeader(uint32_t lane)
    {
        uint32_t helperCount = 0U;
        uint32_t helperBegin = 0U;
        HelperShape(lane, helperCount, helperBegin);
        (void)helperBegin;
        const uint32_t projection = ProjectionForLane(lane);
        if (projection >= kReduceGradProjectionCount || helperCount == 0U) {
            PublishStatus(kReduceGradDeviceInvalidState);
            FailLane(lane);
            return;
        }
        const uint32_t projectionLane = lane - projectionQpBase_[projection];
        ReduceGradPendingWave pending[kReduceGradBankCount];
        bool bankIssued[kReduceGradBankCount] = {};
        bool bankPublished[kReduceGradBankCount] = {};
        uint32_t bankSequence[kReduceGradBankCount] = {};
        uint32_t sequence = 1U;
        uint32_t nextReadySequence = 1U;
        bool stop = false;

        for (int64_t localExpert = 0; localExpert < expertsPerRank_ && !stop;
            ++localExpert) {
            const int32_t globalExpert = static_cast<int32_t>(
                rank_ * expertsPerRank_ + localExpert);
            const uint32_t totalContributors = CountContributors(globalExpert);
            if (totalContributors == 0U) {
                continue;
            }
            for (uint64_t chunk = 0U;
                chunk < chunkCounts_[projection] && !stop; ++chunk) {
                const uint64_t workOrdinal =
                    static_cast<uint64_t>(localExpert) * chunkCounts_[projection] + chunk;
                if (workOrdinal % projectionQpCounts_[projection] != projectionLane) {
                    continue;
                }
                for (uint32_t waveStart = 0U;
                    waveStart < totalContributors && !stop;
                    waveStart += static_cast<uint32_t>(rankSize_)) {
                    const uint32_t bank = (sequence - 1U) % kReduceGradBankCount;
                    if (bankPublished[bank]) {
                        if (!WaitForHelpers(
                                lane, bank, helperCount, bankSequence[bank])) {
                            return;
                        }
                        bankPublished[bank] = false;
                    }
                    if (bankIssued[bank]) {
                        PublishStatus(kReduceGradDeviceInvalidState);
                        FailLane(lane);
                        return;
                    }
                    const uint32_t contributorCount = static_cast<uint32_t>(MinU64(
                        totalContributors - waveStart,
                        static_cast<uint32_t>(rankSize_)));
                    const bool issued = IssueWave(lane, projection, bank,
                        globalExpert, chunk, waveStart, contributorCount,
                        sequence, pending[bank]);
                    if (!issued) {
                        FailLane(lane);
                        return;
                    }
                    bankIssued[bank] = true;

                    if (sequence >= kReduceGradBankCount) {
                        const uint32_t readyBank =
                            (nextReadySequence - 1U) % kReduceGradBankCount;
                        if (!bankIssued[readyBank] ||
                            pending[readyBank].sequence != nextReadySequence) {
                            PublishStatus(kReduceGradDeviceInvalidState);
                            FailLane(lane);
                            return;
                        }
                        const ReduceGradPendingWave &ready = pending[readyBank];
                        if (!CompleteWave(lane, pending[readyBank])) {
                            FailLane(lane);
                            return;
                        }
                        PublishItem(lane, readyBank, LaneToken(ready.sequence),
                            ready.chunkIndex, ready.itemChunkBytes,
                            kReduceGradBankWork, ready.projection,
                            ready.localExpert, ready.waveStart,
                            ready.contributorCount, ready.remoteContributorCount);
                        bankIssued[readyBank] = false;
                        bankPublished[readyBank] = true;
                        bankSequence[readyBank] = ready.sequence;
                        ++nextReadySequence;
                    }
                    ++sequence;
                }
            }
        }

        while (nextReadySequence < sequence) {
            const uint32_t readyBank =
                (nextReadySequence - 1U) % kReduceGradBankCount;
            if (!bankIssued[readyBank] ||
                pending[readyBank].sequence != nextReadySequence) {
                PublishStatus(kReduceGradDeviceInvalidState);
                FailLane(lane);
                return;
            }
            const ReduceGradPendingWave &ready = pending[readyBank];
            if (!CompleteWave(lane, pending[readyBank])) {
                FailLane(lane);
                return;
            }
            PublishItem(lane, readyBank, LaneToken(ready.sequence),
                ready.chunkIndex, ready.itemChunkBytes,
                kReduceGradBankWork, ready.projection,
                ready.localExpert, ready.waveStart,
                ready.contributorCount, ready.remoteContributorCount);
            bankIssued[readyBank] = false;
            bankPublished[readyBank] = true;
            bankSequence[readyBank] = ready.sequence;
            ++nextReadySequence;
        }
        for (uint32_t bank = 0U; bank < kReduceGradBankCount; ++bank) {
            if (bankPublished[bank]) {
                if (!WaitForHelpers(lane, bank, helperCount, bankSequence[bank])) {
                    return;
                }
            }
        }
        const uint32_t terminalBank = (sequence - 1U) % kReduceGradBankCount;
        PublishItem(lane, terminalBank, LaneToken(sequence), 0U, 0U,
            kReduceGradBankTerminal, 0U, 0U, 0U, 0U, 0U);
    }

    __attribute__((always_inline)) inline __aicore__ uint32_t CollectWaveContributors(
        uint32_t localExpert, uint32_t waveStart, uint32_t contributorCount,
        int32_t contributors[]) const
    {
        const int32_t globalExpert = static_cast<int32_t>(
            rank_ * expertsPerRank_ + localExpert);
        uint32_t matching = 0U;
        uint32_t collected = 0U;
        const uint64_t entries = static_cast<uint64_t>(rankSize_) *
            static_cast<uint64_t>(prefetchSlots_);
        for (uint64_t entry = 0U;
            entry < entries && collected < contributorCount; ++entry) {
            if (expertsToCopy_[entry] != globalExpert) {
                continue;
            }
            if (matching++ < waveStart) {
                continue;
            }
            contributors[collected++] = static_cast<int32_t>(entry);
        }
        return collected;
    }

    __attribute__((always_inline)) inline __aicore__ GM_ADDR ContributorAddress(
        uint32_t projection,
        int32_t contributor, GM_ADDR bankPayload, uint32_t &remoteOrdinal,
        uint64_t chunkOffset, uint64_t tileOffset) const
    {
        const int32_t sourceRank = contributor / static_cast<int32_t>(prefetchSlots_);
        const uint32_t slot = static_cast<uint32_t>(
            contributor % static_cast<int32_t>(prefetchSlots_));
        if (sourceRank == rank_) {
            return sources_[projection] + static_cast<uint64_t>(slot) *
                rowBytes_[projection] + chunkOffset + tileOffset;
        }
        return bankPayload + static_cast<uint64_t>(remoteOrdinal++) *
            chunkBytes_ + tileOffset;
    }

    __aicore__ inline void CopyGmToUb(AscendC::LocalTensor<float> destination,
        GM_ADDR source, uint32_t bytes)
    {
        AscendC::GlobalTensor<float> sourceGlobal;
        sourceGlobal.SetGlobalBuffer(reinterpret_cast<__gm__ float *>(source));
        const AscendC::DataCopyExtParams params {1U, bytes, 0U, 0U, 0U};
        const AscendC::DataCopyPadExtParams<float> pad {false, 0U, 0U, 0U};
        AscendC::DataCopyPad(destination, sourceGlobal, params, pad);
    }

    __aicore__ inline void CopyUbToGm(GM_ADDR destination,
        AscendC::LocalTensor<float> source, uint32_t bytes)
    {
        AscendC::GlobalTensor<float> destinationGlobal;
        destinationGlobal.SetGlobalBuffer(reinterpret_cast<__gm__ float *>(destination));
        const AscendC::DataCopyExtParams params {1U, bytes, 0U, 0U, 0U};
        AscendC::DataCopyPad(destinationGlobal, source, params);
    }

    __attribute__((always_inline)) inline __aicore__ void PrefetchAccumulator(
        GM_ADDR output, uint64_t tileOffset, uint32_t tileBytes,
        uint32_t accumIndex, bool storePending[])
    {
        AscendC::LocalTensor<float> accum = accumIndex == 0U ?
            accumBuf_.Get<float>() : accumPongBuf_.Get<float>();
        const AscendC::TEventID event = accumIndex == 0U ? EVENT_ID2 : EVENT_ID3;
        if (storePending[accumIndex]) {
            AscendC::WaitFlag<AscendC::HardEvent::MTE3_MTE2>(event);
            storePending[accumIndex] = false;
        }
        CopyGmToUb(accum, output + tileOffset, tileBytes);
        AscendC::SetFlag<AscendC::HardEvent::MTE2_V>(event);
    }

    __attribute__((always_inline)) inline __aicore__ void AccumulateTile(
        uint32_t projection,
        GM_ADDR output, GM_ADDR bankPayload, const int32_t contributors[],
        uint32_t contributorCount, uint64_t chunkOffset, uint64_t tileOffset,
        uint32_t tileBytes, uint32_t accumIndex, bool storePending[],
        bool hasNextTile, uint64_t nextTileOffset, uint32_t nextTileBytes)
    {
        AscendC::LocalTensor<float> accum = accumIndex == 0U ?
            accumBuf_.Get<float>() : accumPongBuf_.Get<float>();
        const AscendC::TEventID storeEvent = accumIndex == 0U ?
            EVENT_ID2 : EVENT_ID3;
        AscendC::LocalTensor<float> inputs[2] = {
            inputPingBuf_.Get<float>(), inputPongBuf_.Get<float>()};
        AscendC::WaitFlag<AscendC::HardEvent::MTE2_V>(storeEvent);

        uint32_t remoteOrdinal = 0U;
        bool reusePending[2] = {false, false};
        GM_ADDR firstSource = ContributorAddress(projection, contributors[0],
            bankPayload, remoteOrdinal, chunkOffset, tileOffset);
        CopyGmToUb(inputs[0], firstSource, tileBytes);
        AscendC::SetFlag<AscendC::HardEvent::MTE2_V>(EVENT_ID0);

        for (uint32_t contributor = 0U;
            contributor < contributorCount; ++contributor) {
            const uint32_t current = contributor & 1U;
            const AscendC::TEventID currentEvent = current == 0U ? EVENT_ID0 : EVENT_ID1;
            AscendC::WaitFlag<AscendC::HardEvent::MTE2_V>(currentEvent);

            if (contributor + 1U < contributorCount) {
                const uint32_t next = current ^ 1U;
                const AscendC::TEventID nextEvent = next == 0U ? EVENT_ID0 : EVENT_ID1;
                if (reusePending[next]) {
                    AscendC::WaitFlag<AscendC::HardEvent::V_MTE2>(nextEvent);
                    reusePending[next] = false;
                }
                GM_ADDR nextSource = ContributorAddress(projection,
                    contributors[contributor + 1U], bankPayload, remoteOrdinal,
                    chunkOffset, tileOffset);
                CopyGmToUb(inputs[next], nextSource, tileBytes);
                AscendC::SetFlag<AscendC::HardEvent::MTE2_V>(nextEvent);
            } else if (hasNextTile) {
                PrefetchAccumulator(output, nextTileOffset, nextTileBytes,
                    accumIndex ^ 1U, storePending);
            }

            AscendC::Add(accum, accum, inputs[current], tileBytes / sizeof(float));
            AscendC::SetFlag<AscendC::HardEvent::V_MTE2>(currentEvent);
            reusePending[current] = true;
        }
        if (reusePending[0]) {
            AscendC::WaitFlag<AscendC::HardEvent::V_MTE2>(EVENT_ID0);
        }
        if (reusePending[1]) {
            AscendC::WaitFlag<AscendC::HardEvent::V_MTE2>(EVENT_ID1);
        }
        SyncEvent<AscendC::HardEvent::V_MTE3>(storeEvent);
        CopyUbToGm(output + tileOffset, accum, tileBytes);
        AscendC::SetFlag<AscendC::HardEvent::MTE3_MTE2>(storeEvent);
        storePending[accumIndex] = true;
    }

    __attribute__((always_inline)) inline __aicore__ void ProcessBankItem(
        uint32_t lane, uint32_t bank,
        uint32_t projection, uint32_t localExpert, uint64_t chunkIndex,
        uint64_t itemChunkBytes, uint32_t waveStart, uint32_t contributorCount,
        uint32_t remoteContributorCount, uint32_t helperIndex, uint32_t helperCount)
    {
        if (projection >= kReduceGradProjectionCount ||
            localExpert >= static_cast<uint32_t>(expertsPerRank_) ||
            contributorCount == 0U ||
            contributorCount > static_cast<uint32_t>(rankSize_) ||
            chunkIndex >= chunkCounts_[projection] ||
            itemChunkBytes == 0U || itemChunkBytes > chunkBytes_) {
            PublishStatus(kReduceGradDeviceInvalidState);
            return;
        }
        int32_t contributors[TileXR::TILEXR_MAX_RANK_SIZE] = {};
        if (CollectWaveContributors(localExpert, waveStart, contributorCount,
                contributors) != contributorCount) {
            PublishStatus(kReduceGradDeviceInvalidState);
            return;
        }
        uint32_t observedRemoteContributors = 0U;
        for (uint32_t contributor = 0U;
            contributor < contributorCount; ++contributor) {
            const int32_t sourceRank = contributors[contributor] /
                static_cast<int32_t>(prefetchSlots_);
            if (sourceRank != rank_) {
                ++observedRemoteContributors;
            }
        }
        if (observedRemoteContributors != remoteContributorCount) {
            PublishStatus(kReduceGradDeviceInvalidState);
            return;
        }
        const uint64_t chunkOffset = chunkIndex * chunkBytes_;
        GM_ADDR output = gradients_[projection] +
            static_cast<uint64_t>(rank_ * expertsPerRank_ + localExpert) *
                rowBytes_[projection] + chunkOffset;
        GM_ADDR bankPayload = BankPayload(lane, bank);
        const uint64_t tiles = CeilDivU64(itemChunkBytes,
            kReduceGradKernelTileBytes);
        if (helperIndex >= tiles) {
            return;
        }
        bool storePending[2] = {false, false};
        uint32_t accumIndex = 0U;
        const uint64_t firstTileOffset =
            static_cast<uint64_t>(helperIndex) * kReduceGradKernelTileBytes;
        const uint32_t firstTileBytes = static_cast<uint32_t>(MinU64(
            itemChunkBytes - firstTileOffset, kReduceGradKernelTileBytes));
        PrefetchAccumulator(output, firstTileOffset, firstTileBytes,
            accumIndex, storePending);
        for (uint64_t tile = helperIndex; tile < tiles; tile += helperCount) {
            const uint64_t tileOffset = tile * kReduceGradKernelTileBytes;
            const uint32_t tileBytes = static_cast<uint32_t>(MinU64(
                itemChunkBytes - tileOffset, kReduceGradKernelTileBytes));
            const uint64_t nextTile = tile + helperCount;
            const bool hasNextTile = nextTile < tiles;
            const uint64_t nextTileOffset = nextTile * kReduceGradKernelTileBytes;
            const uint32_t nextTileBytes = hasNextTile ?
                static_cast<uint32_t>(MinU64(
                    itemChunkBytes - nextTileOffset, kReduceGradKernelTileBytes)) : 0U;
            AccumulateTile(projection, output, bankPayload, contributors,
                contributorCount, chunkOffset, tileOffset, tileBytes,
                accumIndex, storePending, hasNextTile,
                nextTileOffset, nextTileBytes);
            accumIndex ^= 1U;
        }
        for (uint32_t index = 0U; index < 2U; ++index) {
            if (storePending[index]) {
                const AscendC::TEventID storeEvent = index == 0U ?
                    EVENT_ID2 : EVENT_ID3;
                AscendC::WaitFlag<AscendC::HardEvent::MTE3_MTE2>(storeEvent);
            }
        }
    }

    __aicore__ inline bool WaitForBank(uint32_t lane, uint32_t bank,
        uint32_t sequence)
    {
        const uint64_t expected = LaneToken(sequence);
        uint64_t attempts = 0U;
        while (true) {
            const uint64_t observed = LoadLaneToken(BankReady(lane, bank));
            if (observed == expected) {
                return true;
            }
            if (LaneAborted(lane)) {
                return false;
            }
            if (++attempts >= waitIterations_) {
                PublishStatus(kReduceGradDeviceHelperTimeout);
                AbortLane(lane);
                return false;
            }
        }
    }

    __aicore__ inline void RunLaneHelper()
    {
        uint32_t lane = 0U;
        uint32_t helperIndex = 0U;
        uint32_t helperCount = 0U;
        HelperLane(lane, helperIndex, helperCount);
        uint32_t sequence = 1U;
        while (true) {
            const uint32_t bank = (sequence - 1U) % kReduceGradBankCount;
            if (!WaitForBank(lane, bank, sequence)) {
                return;
            }
            uint64_t token = 0U;
            uint64_t chunkIndex = 0U;
            uint64_t itemChunkBytes = 0U;
            uint32_t kind = 0U;
            uint32_t projection = 0U;
            uint32_t localExpert = 0U;
            uint32_t waveStart = 0U;
            uint32_t contributorCount = 0U;
            uint32_t remoteContributorCount = 0U;
            LoadItem(lane, bank, token, chunkIndex, itemChunkBytes, kind,
                projection, localExpert, waveStart, contributorCount,
                remoteContributorCount);
            if (token != LaneToken(sequence)) {
                PublishStatus(kReduceGradDeviceInvalidState);
            } else if (kind == kReduceGradBankTerminal) {
                StoreLaneToken(BankDoneToken(
                    lane, bank, helperIndex), LaneToken(sequence));
                break;
            } else if (kind == kReduceGradBankWork) {
                ProcessBankItem(lane, bank, projection, localExpert, chunkIndex,
                    itemChunkBytes, waveStart, contributorCount,
                    remoteContributorCount, helperIndex, helperCount);
            } else {
                PublishStatus(kReduceGradDeviceInvalidState);
            }
            StoreLaneToken(BankDoneToken(
                lane, bank, helperIndex), LaneToken(sequence));
            ++sequence;
        }
    }

    __aicore__ inline void CrossRankBarrier()
    {
        if (barrierPhysicalQp_ != kReduceGradInvalidPhysicalQp) {
            CrossRankRegisteredBarrier();
            return;
        }
        GM_ADDR shareAddrs[TileXR::TILEXR_MAX_RANK_SIZE] = {};
        AscendC::GlobalTensor<GM_ADDR> peerMemTable;
        peerMemTable.SetGlobalBuffer(&(args_->peerMems[0]),
            TileXR::TILEXR_MAX_RANK_SIZE);
        for (int32_t peer = 0; peer < rankSize_; ++peer) {
            shareAddrs[peer] = peerMemTable.GetValue(peer);
        }
        GM_ADDR localShareAddr = shareAddrs[rank_];
        for (int32_t peer = 0; peer < rankSize_; ++peer) {
            if (shareAddrs[peer] == nullptr) {
                PublishStatus(kReduceGradDeviceInvalidState);
                shareAddrs[peer] = localShareAddr;
            }
        }
        SyncCollectives sync;
        sync.Init(static_cast<int>(rank_), static_cast<int>(rankSize_),
            shareAddrs, syncBuf_);
        const int32_t localStep = DeviceStatus() == 0U ?
            kReduceGradBarrierStep : kReduceGradBarrierFailureStep;
        sync.SetInnerFlag(static_cast<int32_t>(magic_), localStep);
        for (int32_t peer = 0; peer < rankSize_; ++peer) {
            sync.WaitInnerFlag(static_cast<int32_t>(magic_),
                kReduceGradBarrierStep, peer, 0);
        }
        const int64_t successValue =
            (static_cast<int64_t>(static_cast<int32_t>(magic_)) << MAGIC_OFFSET) |
            static_cast<int64_t>(kReduceGradBarrierStep);
        for (int32_t peer = 0; peer < rankSize_; ++peer) {
            if (sync.GetInnerFlag(peer, 0) != successValue) {
                PublishStatus(kReduceGradDeviceInvalidState);
            }
        }
    }

    __aicore__ inline uint64_t BarrierValue(int32_t step) const
    {
        return (static_cast<uint64_t>(static_cast<uint32_t>(magic_)) <<
            MAGIC_OFFSET) | static_cast<uint32_t>(step);
    }

    __aicore__ inline __gm__ uint64_t *BarrierToken(int32_t sourceRank) const
    {
        return reinterpret_cast<__gm__ uint64_t *>(workspace_ + barrierOffset_ +
            static_cast<uint64_t>(sourceRank) *
                TileXR::TILEXR_UDMA_CACHE_LINE_SIZE);
    }

    __aicore__ inline void PublishLocalBarrierToken(uint64_t value)
    {
        AscendC::LocalTensor<uint64_t> token = itemBuf_.Get<uint64_t>();
        token.SetValue(0U, value);
        for (uint32_t word = 1U;
            word < TileXR::TILEXR_UDMA_CACHE_LINE_SIZE / sizeof(uint64_t);
            ++word) {
            token.SetValue(word, 0U);
        }
        AscendC::GlobalTensor<uint8_t> destination;
        destination.SetGlobalBuffer(
            reinterpret_cast<__gm__ uint8_t *>(BarrierToken(rank_)));
        const AscendC::DataCopyExtParams copyOut {1U,
            static_cast<uint32_t>(TileXR::TILEXR_UDMA_CACHE_LINE_SIZE),
            0U, 0U, 0U};
        SyncEvent<AscendC::HardEvent::S_MTE3>(EVENT_ID3);
        AscendC::DataCopyPad(destination,
            token.ReinterpretCast<uint8_t>(), copyOut);
        SyncEvent<AscendC::HardEvent::MTE3_S>(EVENT_ID3);
    }

    __aicore__ inline bool PublishRegisteredBarrierRound(int32_t step)
    {
        PublishLocalBarrierToken(BarrierValue(step));
        AscendC::LocalTensor<uint8_t> wqeScratch = wqeBuf_.Get<uint8_t>();
        uint32_t frontiers[TileXR::TILEXR_MAX_RANK_SIZE] = {};
        bool posted[TileXR::TILEXR_MAX_RANK_SIZE] = {};
        const uint64_t tokenOffset = barrierOffset_ +
            static_cast<uint64_t>(rank_) *
                TileXR::TILEXR_UDMA_CACHE_LINE_SIZE;
        bool ok = true;
        for (int32_t targetRank = 0; targetRank < rankSize_; ++targetRank) {
            if (targetRank == rank_) {
                continue;
            }
            const uint32_t post = TileXR::UDMAProfilePutNbiOnQpDeferred(
                args_, profileInfo_, profileRegistry_, wqeScratch, targetRank,
                barrierPhysicalQp_, kReduceGradStagingRegion, tokenOffset,
                kReduceGradStagingRegion, tokenOffset,
                TileXR::TILEXR_UDMA_CACHE_LINE_SIZE);
            if (post != TileXR::TILEXR_UDMA_STATUS_SUCCESS) {
                PublishStatus(kReduceGradDeviceInvalidState);
                ok = false;
                continue;
            }
            posted[targetRank] = true;
            frontiers[targetRank] = TileXR::UDMAProfileCompletionFrontier(
                args_, profileInfo_, profileRegistry_, targetRank,
                barrierPhysicalQp_);
        }
        for (int32_t targetRank = 0; targetRank < rankSize_; ++targetRank) {
            if (!posted[targetRank]) {
                continue;
            }
            const uint32_t flush = TileXR::UDMAProfileFlushQpDoorbell(
                args_, profileInfo_, profileRegistry_, targetRank,
                barrierPhysicalQp_);
            if (flush != TileXR::TILEXR_UDMA_STATUS_SUCCESS) {
                PublishStatus(kReduceGradDeviceInvalidState);
                ok = false;
            }
        }
        for (int32_t targetRank = 0; targetRank < rankSize_; ++targetRank) {
            if (!posted[targetRank]) {
                continue;
            }
            const uint32_t quiet = TileXR::UDMAProfileQuietStatusOnQpUntil(
                args_, profileInfo_, profileRegistry_, targetRank,
                barrierPhysicalQp_, frontiers[targetRank]);
            if (quiet != TileXR::TILEXR_UDMA_STATUS_SUCCESS) {
                PublishStatus(quiet == TileXR::TILEXR_UDMA_STATUS_CQ_TIMEOUT ?
                    kReduceGradDeviceUdmaTimeout : kReduceGradDeviceUdmaCqError);
                ok = false;
            }
        }
        return ok;
    }

    __aicore__ inline bool WaitRegisteredBarrierRound(
        int32_t expectedStep, bool acceptFailure)
    {
        const uint64_t expected = BarrierValue(expectedStep);
        const uint64_t failure = BarrierValue(kReduceGradBarrierFailureStep);
        for (uint64_t iteration = 0U; iteration < waitIterations_; ++iteration) {
            bool ready = true;
            for (int32_t sourceRank = 0; sourceRank < rankSize_; ++sourceRank) {
                const uint64_t observed = LoadLaneToken(BarrierToken(sourceRank));
                if (observed != expected && (!acceptFailure || observed != failure)) {
                    ready = false;
                    break;
                }
            }
            if (ready) {
                if (acceptFailure) {
                    for (int32_t sourceRank = 0; sourceRank < rankSize_; ++sourceRank) {
                        if (LoadLaneToken(BarrierToken(sourceRank)) != expected) {
                            PublishStatus(kReduceGradDeviceInvalidState);
                        }
                    }
                }
                return true;
            }
        }
        PublishStatus(kReduceGradDeviceUdmaTimeout);
        return false;
    }

    __aicore__ inline void CrossRankRegisteredBarrier()
    {
        if (!PublishRegisteredBarrierRound(kReduceGradBarrierArrivalStep) ||
            !WaitRegisteredBarrierRound(kReduceGradBarrierArrivalStep, false)) {
            return;
        }
        const int32_t finalStep = DeviceStatus() == 0U ?
            kReduceGradBarrierStep : kReduceGradBarrierFailureStep;
        if (!PublishRegisteredBarrierRound(finalStep)) {
            return;
        }
        (void)WaitRegisteredBarrierRound(kReduceGradBarrierStep, true);
    }

    __aicore__ inline void ClearLocalSources()
    {
        AscendC::LocalTensor<float> zeros = accumBuf_.Get<float>();
        AscendC::Duplicate(zeros, 0.0F,
            static_cast<int32_t>(kReduceGradKernelTileBytes / sizeof(float)));
        SyncEvent<AscendC::HardEvent::V_MTE3>(EVENT_ID2);
        bool copyPending = false;
        uint64_t rowTileBase = 0U;
        for (uint32_t projection = 0U;
            projection < kReduceGradProjectionCount; ++projection) {
            const uint64_t tilesPerRow = CeilDivU64(
                rowBytes_[projection], kReduceGradKernelTileBytes);
            for (int64_t slot = 0; slot < prefetchSlots_; ++slot) {
                const uint64_t rowBlock = rowTileBase % blockCount_;
                const uint64_t firstTile =
                    (static_cast<uint64_t>(blockIdx_) + blockCount_ - rowBlock) %
                    blockCount_;
                rowTileBase += tilesPerRow;
                if (expertsToCopy_[rank_ * prefetchSlots_ + slot] < 0) {
                    continue;
                }
                GM_ADDR destination = sources_[projection] +
                    static_cast<uint64_t>(slot) * rowBytes_[projection];
                for (uint64_t tile = firstTile; tile < tilesPerRow;
                    tile += static_cast<uint64_t>(blockCount_)) {
                    const uint64_t offset = tile * kReduceGradKernelTileBytes;
                    const uint32_t bytes = static_cast<uint32_t>(MinU64(
                        rowBytes_[projection] - offset,
                        kReduceGradKernelTileBytes));
                    CopyUbToGm(destination + offset, zeros, bytes);
                    copyPending = true;
                }
            }
        }
        if (copyPending) {
            SyncEvent<AscendC::HardEvent::MTE3_S>(EVENT_ID2);
        }
    }

    __gm__ TileXR::CommArgs *args_{nullptr};
    __gm__ TileXR::UDMAInfo *profileInfo_{nullptr};
    __gm__ TileXR::TileXRUDMAProfileRegistry *profileRegistry_{nullptr};
    __gm__ int32_t *expertsToCopy_{nullptr};
    GM_ADDR gradients_[kReduceGradProjectionCount] = {};
    GM_ADDR sources_[kReduceGradProjectionCount] = {};
    GM_ADDR workspace_{nullptr};
    __gm__ uint32_t *status_{nullptr};
    int64_t rank_{0};
    int64_t rankSize_{0};
    int64_t expertCount_{0};
    int64_t expertsPerRank_{0};
    int64_t prefetchSlots_{0};
    uint64_t rowElements_[kReduceGradProjectionCount] = {};
    uint64_t rowBytes_[kReduceGradProjectionCount] = {};
    uint64_t chunkCounts_[kReduceGradProjectionCount] = {};
    uint32_t projectionQpBase_[kReduceGradProjectionCount] = {};
    uint32_t projectionQpCounts_[kReduceGradProjectionCount] = {};
    uint32_t lanePhysicalQps_[kReduceGradMaxUdmaQpCount] = {};
    uint32_t transportQpCount_{0};
    uint32_t qpCount_{0};
    uint32_t laneCount_{0};
    uint32_t barrierPhysicalQp_{kReduceGradInvalidPhysicalQp};
    uint64_t laneStateBytes_{0};
    uint64_t stagingOffset_{0};
    uint64_t bankStrideBytes_{0};
    uint64_t laneStrideBytes_{0};
    uint64_t chunkBytes_{0};
    uint64_t barrierOffset_{0};
    uint64_t barrierBytes_{0};
    uint64_t workspaceBytes_{0};
    uint64_t waitIterations_{1};
    int64_t magic_{0};
    uint32_t blockIdx_{0};
    uint32_t blockCount_{0};
    AscendC::TPipe pipe_;
    AscendC::TBuf<AscendC::QuePosition::VECCALC> accumBuf_;
    AscendC::TBuf<AscendC::QuePosition::VECCALC> accumPongBuf_;
    AscendC::TBuf<AscendC::QuePosition::VECCALC> inputPingBuf_;
    AscendC::TBuf<AscendC::QuePosition::VECCALC> inputPongBuf_;
    AscendC::TBuf<AscendC::QuePosition::VECCALC> syncBuf_;
    AscendC::TBuf<AscendC::QuePosition::VECCALC> wqeBuf_;
    AscendC::TBuf<AscendC::QuePosition::VECCALC> itemBuf_;
};

} // namespace Kernel
} // namespace TileXRMoonEp

extern "C" __global__ __aicore__ void tilexr_moonep_reduce_grad_kernel(
    GM_ADDR commArgs, GM_ADDR profileInfo, GM_ADDR profileRegistry,
    GM_ADDR expertsToCopy, GM_ADDR gateGradient, GM_ADDR upGradient,
    GM_ADDR downGradient, GM_ADDR gateSource, GM_ADDR upSource,
    GM_ADDR downSource, GM_ADDR workspace, GM_ADDR status,
    int64_t rank, int64_t rankSize, int64_t expertCount,
    int64_t expertsPerRank, int64_t prefetchSlots,
    uint64_t gateRowElements, uint64_t upRowElements,
    uint64_t downRowElements, uint64_t gateRowBytes,
    uint64_t upRowBytes, uint64_t downRowBytes,
    uint64_t gateChunkCount, uint64_t upChunkCount,
    uint64_t downChunkCount, uint32_t gateQpBase,
    uint32_t upQpBase, uint32_t downQpBase,
    uint32_t gateQpCount, uint32_t upQpCount,
    uint32_t downQpCount, uint32_t lane0PhysicalQp,
    uint32_t lane1PhysicalQp, uint32_t lane2PhysicalQp,
    uint32_t lane3PhysicalQp, uint32_t lane4PhysicalQp,
    uint32_t lane5PhysicalQp, uint32_t lane6PhysicalQp,
    uint32_t lane7PhysicalQp, uint32_t transportQpCount,
    uint32_t qpCount, uint32_t laneCount, uint32_t barrierPhysicalQp,
    uint64_t laneStateBytes, uint64_t stagingOffset,
    uint64_t bankStrideBytes, uint64_t laneStrideBytes,
    uint64_t chunkBytes, uint64_t barrierOffset,
    uint64_t barrierBytes, uint64_t workspaceBytes,
    uint64_t waitIterations, int64_t magic)
{
    if constexpr (g_coreType == AscendC::AIV) {
        TileXRMoonEp::Kernel::ReduceGradKernel op;
        op.Init(commArgs, profileInfo, profileRegistry, expertsToCopy,
            gateGradient, upGradient, downGradient, gateSource, upSource,
            downSource, workspace, status, rank, rankSize, expertCount,
            expertsPerRank, prefetchSlots, gateRowElements, upRowElements,
            downRowElements, gateRowBytes, upRowBytes, downRowBytes,
            gateChunkCount, upChunkCount, downChunkCount, gateQpBase,
            upQpBase, downQpBase, gateQpCount, upQpCount, downQpCount,
            lane0PhysicalQp, lane1PhysicalQp, lane2PhysicalQp,
            lane3PhysicalQp, lane4PhysicalQp, lane5PhysicalQp,
            lane6PhysicalQp, lane7PhysicalQp, transportQpCount,
            qpCount, laneCount, barrierPhysicalQp, laneStateBytes,
            stagingOffset, bankStrideBytes, laneStrideBytes, chunkBytes,
            barrierOffset, barrierBytes, workspaceBytes, waitIterations, magic);
        op.Process();
    }
}
