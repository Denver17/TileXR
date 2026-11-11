/*
 * 功能：声明 drift 拟合、时间校正所需的 tracker 和 sleep_at deadline 换算接口。
 * 调用关系：session 持有 tracker，clock_drift.cpp 读取采样并更新模型。
 */
#ifndef TILEXR_DIAGNOSTICS_HOST_CLOCK_DRIFT_H
#define TILEXR_DIAGNOSTICS_HOST_CLOCK_DRIFT_H

#include <vector>
#include "tilexr_straggler.h"
#include "tilexr_straggler_layout.h"

namespace TileXRDiagnostics {
namespace Host {

struct ClockDriftPoint {
    uint64_t localCycle;
    long double offsetCycles;
};

struct ClockDriftTracker {
    uint64_t windowUs = 0;
    bool closed = false;
    bool tracking = false;
    uint32_t initialGeneration = 0;
    double residualNs = 0;
    bool probeValid = false;
    bool updated = false;
    std::vector<ClockDriftPoint> points;
    TileXRClockDriftModel model {};
};

// Invert a READY affine model and ceil the local deadline. Output is unchanged on error.
int ClockSleepDeadline(const TileXRClockDriftModel &model, int64_t referenceCycle,
                        uint64_t *localDeadline);

TileXRClockDriftModel FitClockDrift(const std::vector<ClockDriftPoint> &points,
                                   uint64_t frequency, int rank);
void UpdateClockDrift(ClockDriftTracker &tracker,
                       const std::vector<TileXR::TileXRClockSyncDeviceSample> &samples,
                       uint64_t frequency, int rank, uint32_t generation);

} // namespace Host
} // namespace TileXRDiagnostics

#endif
