#include <cstddef>
#include <type_traits>

#include "tilexr_straggler.h"
#include "tilexr_straggler_device.h"
#include "tilexr_straggler_layout.h"

namespace {

using CreateFn = int (*)(TileXRCommPtr, const TileXRStragglerConfig *, TileXRStraggler *);
using ClockFn = int (*)(TileXRStraggler, aclrtStream, TileXRClockSyncResult *);
using BeginFn = int (*)(TileXRStraggler, const TileXRStragglerOperation *, aclrtStream,
                        TileXRStragglerObservation *);
using BeginForCommFn = int (*)(TileXRCommPtr, const TileXRStragglerOperation *, aclrtStream,
                               TileXRStragglerObservation *);
using EndFn = int (*)(TileXRStraggler, const TileXRStragglerObservation *, aclrtStream, int);
using CollectFn = int (*)(TileXRStraggler, aclrtStream, TileXRStragglerReport *);

} // namespace

int main()
{
    static_assert(std::is_standard_layout<TileXRStragglerOperation>::value, "operation ABI");
    static_assert(std::is_standard_layout<TileXRStragglerObservation>::value, "observation ABI");
    static_assert(std::is_standard_layout<TileXRStragglerConfig>::value, "config ABI");
    static_assert(std::is_standard_layout<TileXRClockSyncResult>::value, "clock result ABI");
    static_assert(offsetof(TileXRStragglerConfig, clockSyncWarmupSamples) == 4,
                  "warmup sample offset");
    static_assert(offsetof(TileXRClockSyncResult, warmupSampleCount) == 20,
                  "clock-result warmup offset");
    static_assert(offsetof(TileXRClockSyncResult, cyclesPerSecond) == 24,
                  "clock-result cycles-per-second offset");
    static_assert(sizeof(TileXRClockSyncResult) == 10288, "clock result size");
    static_assert(sizeof(TileXRStragglerOperation) == 32, "operation size");
    static_assert(sizeof(TileXRStragglerObservation) == 72, "observation size");
    static_assert(sizeof(TileXR::TileXRStragglerTimingRecord) == 80, "record size");
    static_assert(sizeof(TileXR::TileXRStragglerDeviceControl) == 64, "device control size");
    static_assert(sizeof(TileXR::TileXRStragglerDeviceSlot) == 192, "device slot size");
    static_assert(offsetof(TileXR::TileXRStragglerTimingRecord, blockCount) == 72,
                  "record block-count offset");
    CreateFn create = &TileXRStragglerCreate;
    ClockFn clock = &TileXRClockSynchronize;
    BeginFn begin = &TileXRStragglerObservationBegin;
    BeginForCommFn beginForComm = &TileXRStragglerObservationBeginForComm;
    EndFn end = &TileXRStragglerObservationEnd;
    CollectFn collect = &TileXRStragglerCollect;
    TileXRStragglerOperation ep {};
    ep.componentKind = TILEXR_STRAGGLER_COMPONENT_EP;
    TileXRStragglerOperation moonEp {};
    moonEp.componentKind = TILEXR_STRAGGLER_COMPONENT_MOONEP;
    return create && clock && begin && beginForComm && end && collect &&
        ep.componentKind != moonEp.componentKind ? 0 : 1;
}
