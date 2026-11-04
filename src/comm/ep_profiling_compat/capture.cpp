/* EP-only communicator capture lifecycle; records.v1 compatibility. */
#include "capture.h"

#include <acl/acl_rt.h>
#include <limits>

#include "../tilexr_log.h"
#include "../../include/tilexr_types.h"
#include "../../ep/common/ep_profiling_compat.h"

namespace TileXR {
namespace {
void FreeDeviceMemory(GM_ADDR &memory)
{
    if (memory != nullptr) {
        const aclError ret = aclrtFree(memory);
        if (ret != ACL_SUCCESS) {
            TILEXR_LOG(ERROR) << "Free EP profiling memory failed: " << ret;
        }
    }
    memory = nullptr;
}
} // namespace

EpProfilingCapture::~EpProfilingCapture()
{
    Reset();
}

int EpProfilingCapture::Init(int rank, int device, int commDomain)
{
    rank_ = rank;
    std::string error;
    if (!LoadTileXRProfilingRuntimeConfig(rank_, device, commDomain, &profiling_.config, &error) ||
        !LoadTileXRCombineProfilingRuntimeConfig(rank_, device, commDomain, &combineProfiling_.config, &error)) {
        TILEXR_LOG(ERROR) << "Invalid TileXR profiling configuration: " << error;
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    if (profiling_.config.enabled && combineProfiling_.config.enabled &&
        profiling_.config.recordsPath == combineProfiling_.config.recordsPath) {
        TILEXR_LOG(ERROR) << "Dispatch and Combine profiling records paths must differ";
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    int ret = InitRegion(profiling_);
    if (ret == TILEXR_SUCCESS) {
        ret = InitRegion(combineProfiling_);
    }
    if (ret != TILEXR_SUCCESS) {
        Reset();
        return ret;
    }
    commArgs_.tilexrProfilingTimestampsGM = profiling_.timestamps;
    commArgs_.tilexrProfilingEventIdsGM = profiling_.eventIds;
    commArgs_.tilexrProfilingRecordCountsGM = profiling_.recordCounts;
    commArgs_.tilexrProfilingDroppedCountsGM = profiling_.droppedCounts;
    commArgs_.tilexrProfilingRecordsPerCore = profiling_.config.recordsPerCore;
    commArgs_.tilexrProfilingCoreCapacity = profiling_.config.coreCapacity;
    commArgs_.tilexrCombineProfilingTimestampsGM = combineProfiling_.timestamps;
    commArgs_.tilexrCombineProfilingEventIdsGM = combineProfiling_.eventIds;
    commArgs_.tilexrCombineProfilingRecordCountsGM = combineProfiling_.recordCounts;
    commArgs_.tilexrCombineProfilingDroppedCountsGM = combineProfiling_.droppedCounts;
    commArgs_.tilexrCombineProfilingRecordsPerCore = combineProfiling_.config.recordsPerCore;
    commArgs_.tilexrCombineProfilingCoreCapacity = combineProfiling_.config.coreCapacity;
    if (profiling_.config.enabled || combineProfiling_.config.enabled) {
        commArgs_.extraFlag |= ExtraFlag::PROFILING;
    }
    return TILEXR_SUCCESS;
}

int EpProfilingCapture::InitRegion(ProfilingRegion &region)
{
    if (!region.config.enabled) {
        return TILEXR_SUCCESS;
    }
    const uint64_t slotCount = static_cast<uint64_t>(region.config.coreCapacity) * region.config.recordsPerCore;
    if (slotCount > std::numeric_limits<size_t>::max() / sizeof(uint64_t)) {
        TILEXR_LOG(ERROR) << "TileXR profiling allocation size overflow";
        return TILEXR_ERROR_INTERNAL;
    }
    const size_t timestampBytes = static_cast<size_t>(slotCount) * sizeof(uint64_t);
    const size_t eventIdBytes = static_cast<size_t>(slotCount) * sizeof(uint32_t);
    const size_t counterBytes =
        static_cast<size_t>(region.config.coreCapacity) * TILEXR_PROFILING_COUNT_STRIDE * sizeof(uint32_t);

    const auto allocateZeroed = [](GM_ADDR *destination, size_t bytes) -> aclError {
        aclError ret = aclrtMalloc(reinterpret_cast<void **>(destination), bytes, ACL_MEM_MALLOC_HUGE_FIRST);
        if (ret != ACL_SUCCESS) {
            return ret;
        }
        ret = aclrtMemset(*destination, bytes, 0, bytes);
        if (ret != ACL_SUCCESS) {
            (void)aclrtFree(*destination);
            *destination = nullptr;
        }
        return ret;
    };

    aclError ret = allocateZeroed(&region.timestamps, timestampBytes);
    if (ret == ACL_SUCCESS) {
        ret = allocateZeroed(&region.eventIds, eventIdBytes);
    }
    if (ret == ACL_SUCCESS) {
        ret = allocateZeroed(&region.recordCounts, counterBytes);
    }
    if (ret == ACL_SUCCESS) {
        ret = allocateZeroed(&region.droppedCounts, counterBytes);
    }
    if (ret != ACL_SUCCESS) {
        TILEXR_LOG(ERROR) << "TileXR profiling device allocation failed: " << ret;
        return TILEXR_ERROR_INTERNAL;
    }

    TILEXR_LOG(INFO) << "TileXR profiling enabled: records=" << region.config.recordsPath
                     << ", recordsPerCore=" << region.config.recordsPerCore;
    return TILEXR_SUCCESS;
}

int EpProfilingCapture::ReserveLaunch(uint32_t recordsPerLaunch, uint32_t *launchSlot)
{
    return profiling_.launches.Reserve(profiling_.config, recordsPerLaunch, launchSlot) ? TILEXR_SUCCESS :
                                                                                          TILEXR_ERROR_PARA_CHECK_FAIL;
}

int EpProfilingCapture::ReserveLatestLaunch(uint32_t recordsPerLaunch, uint32_t *launchSlot)
{
    return profiling_.launches.ReserveLatest(profiling_.config, recordsPerLaunch, launchSlot) ?
        TILEXR_SUCCESS :
        TILEXR_ERROR_PARA_CHECK_FAIL;
}

int EpProfilingCapture::ReserveLatestCombineLaunch(uint32_t recordsPerLaunch, uint32_t *launchSlot)
{
    return combineProfiling_.launches.ReserveLatest(combineProfiling_.config, recordsPerLaunch, launchSlot) ?
        TILEXR_SUCCESS :
        TILEXR_ERROR_PARA_CHECK_FAIL;
}

int EpProfilingCapture::Finalize()
{
    if ((!profiling_.config.enabled || profiling_.finalized) &&
        (!combineProfiling_.config.enabled || combineProfiling_.finalized)) {
        return TILEXR_SUCCESS;
    }
    const aclError sync = aclrtSynchronizeDevice();
    if (sync != ACL_SUCCESS) {
        TILEXR_LOG(ERROR) << "TileXR profiling device synchronization failed: " << sync;
        return TILEXR_ERROR_INTERNAL;
    }
    // Attempt each enabled export even if the other region fails, retaining
    // independent evidence and never overwriting a completed region on retry.
    const int dispatch = FinalizeRegion(profiling_);
    const int combine = FinalizeRegion(combineProfiling_);
    return dispatch != TILEXR_SUCCESS ? dispatch : combine;
}

int EpProfilingCapture::FinalizeRegion(ProfilingRegion &region)
{
    if (!region.config.enabled || region.finalized) {
        return TILEXR_SUCCESS;
    }
    const uint64_t slotCount = static_cast<uint64_t>(region.config.coreCapacity) * region.config.recordsPerCore;
    std::vector<uint32_t> occupiedCounts(region.config.coreCapacity, 0U);
    std::vector<uint32_t> droppedCounts(region.config.coreCapacity, 0U);
    std::vector<uint32_t> rawCounters(
        static_cast<size_t>(region.config.coreCapacity) * TILEXR_PROFILING_COUNT_STRIDE, 0U);
    std::vector<uint64_t> timestamps(static_cast<size_t>(slotCount), 0U);
    std::vector<uint32_t> eventIds(static_cast<size_t>(slotCount), 0U);

    const size_t counterBytes = rawCounters.size() * sizeof(uint32_t);
    aclError ret =
        aclrtMemcpy(rawCounters.data(), counterBytes, region.recordCounts, counterBytes, ACL_MEMCPY_DEVICE_TO_HOST);
    if (ret == ACL_SUCCESS) {
        for (uint32_t core = 0U; core < region.config.coreCapacity; ++core) {
            occupiedCounts[core] = rawCounters[static_cast<size_t>(core) * TILEXR_PROFILING_COUNT_STRIDE];
        }
        ret = aclrtMemcpy(
            rawCounters.data(), counterBytes, region.droppedCounts, counterBytes, ACL_MEMCPY_DEVICE_TO_HOST);
    }
    if (ret == ACL_SUCCESS) {
        for (uint32_t core = 0U; core < region.config.coreCapacity; ++core) {
            droppedCounts[core] = rawCounters[static_cast<size_t>(core) * TILEXR_PROFILING_COUNT_STRIDE];
        }
        ret = aclrtMemcpy(timestamps.data(), timestamps.size() * sizeof(uint64_t), region.timestamps,
            timestamps.size() * sizeof(uint64_t), ACL_MEMCPY_DEVICE_TO_HOST);
    }
    if (ret == ACL_SUCCESS) {
        ret = aclrtMemcpy(eventIds.data(), eventIds.size() * sizeof(uint32_t), region.eventIds,
            eventIds.size() * sizeof(uint32_t), ACL_MEMCPY_DEVICE_TO_HOST);
    }
    if (ret != ACL_SUCCESS) {
        TILEXR_LOG(ERROR) << "TileXR profiling device capture copy failed: " << ret;
        return TILEXR_ERROR_INTERNAL;
    }

    std::string error;
    region.config.incomplete = region.launches.Incomplete();
    region.config.latestLaunch = region.launches.LatestLaunch();
    if (!WriteTileXRProfilingRecords(
            region.config, rank_, occupiedCounts, droppedCounts, timestamps, eventIds, &error)) {
        TILEXR_LOG(ERROR) << "TileXR profiling records export failed: " << error;
        return TILEXR_ERROR_INTERNAL;
    }
    TILEXR_LOG(INFO) << "TileXR profiling records written to " << region.config.recordsPath;
    region.finalized = true;
    return TILEXR_SUCCESS;
}

void EpProfilingCapture::Reset()
{
    for (ProfilingRegion *region : {&profiling_, &combineProfiling_}) {
        FreeDeviceMemory(region->timestamps);
        FreeDeviceMemory(region->eventIds);
        FreeDeviceMemory(region->recordCounts);
        FreeDeviceMemory(region->droppedCounts);
        region->config = TileXRProfilingRuntimeConfig {};
        region->launches.Reset();
        region->finalized = false;
    }
    commArgs_.tilexrProfilingTimestampsGM = nullptr;
    commArgs_.tilexrProfilingEventIdsGM = nullptr;
    commArgs_.tilexrProfilingRecordCountsGM = nullptr;
    commArgs_.tilexrProfilingDroppedCountsGM = nullptr;
    commArgs_.tilexrProfilingRecordsPerCore = 0U;
    commArgs_.tilexrProfilingCoreCapacity = 0U;
    commArgs_.tilexrCombineProfilingTimestampsGM = nullptr;
    commArgs_.tilexrCombineProfilingEventIdsGM = nullptr;
    commArgs_.tilexrCombineProfilingRecordCountsGM = nullptr;
    commArgs_.tilexrCombineProfilingDroppedCountsGM = nullptr;
    commArgs_.tilexrCombineProfilingRecordsPerCore = 0U;
    commArgs_.tilexrCombineProfilingCoreCapacity = 0U;
    commArgs_.extraFlag &= ~ExtraFlag::PROFILING;
}

} // namespace TileXR
