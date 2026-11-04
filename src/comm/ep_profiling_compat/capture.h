/* EP-only compatibility capture. Independent of the standalone profiling SDK. */
#ifndef TILEXR_EP_PROFILING_COMPAT_CAPTURE_H
#define TILEXR_EP_PROFILING_COMPAT_CAPTURE_H

#include "runtime.h"
#include "../../include/comm_args.h"

namespace TileXR {

// Owns the legacy EP capture regions, never the collectives internal-resource slot.
// The referenced CommArgs must outlive this owner. Finalize is retryable and must
// complete before communicator destruction; Reset only releases owned memory.
class EpProfilingCapture {
public:
    explicit EpProfilingCapture(CommArgs &commArgs) : commArgs_(commArgs)
    {
    }
    ~EpProfilingCapture();
    EpProfilingCapture(const EpProfilingCapture &) = delete;
    EpProfilingCapture &operator=(const EpProfilingCapture &) = delete;
    int Init(int rank, int device, int commDomain);
    int Finalize();
    void Reset();
    int ReserveLaunch(uint32_t recordsPerLaunch, uint32_t *launchSlot);
    int ReserveLatestLaunch(uint32_t recordsPerLaunch, uint32_t *launchSlot);
    int ReserveLatestCombineLaunch(uint32_t recordsPerLaunch, uint32_t *launchSlot);

private:
    struct ProfilingRegion {
        TileXRProfilingRuntimeConfig config {};
        TileXRProfilingLaunchState launches;
        GM_ADDR timestamps = nullptr;
        GM_ADDR eventIds = nullptr;
        GM_ADDR recordCounts = nullptr;
        GM_ADDR droppedCounts = nullptr;
        bool finalized = false;
    };
    int InitRegion(ProfilingRegion &region);
    int FinalizeRegion(ProfilingRegion &region);
    CommArgs &commArgs_;
    int rank_ = 0;
    ProfilingRegion profiling_;
    ProfilingRegion combineProfiling_;
};

} // namespace TileXR
#endif // TILEXR_EP_PROFILING_COMPAT_CAPTURE_H
