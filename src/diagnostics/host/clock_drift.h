/*
 * 功能：声明初始拟合和反馈更新接口，以及保存采样点、模型和探测状态的 ClockDriftTracker。
 * 调用关系：straggler_session.cpp 持有 tracker，clock_drift.cpp 更新相对逻辑 rank 0 的模型。
 * Purpose: Declare fitting/feedback interfaces and the tracker storing points, model and probe state.
 * Calls: straggler_session.cpp owns the tracker; clock_drift.cpp updates its model relative to logical rank 0.
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

TileXRClockDriftModel FitClockDrift(const std::vector<ClockDriftPoint> &points,
                                   uint64_t frequency, int rank);
void UpdateClockDrift(ClockDriftTracker &tracker,
                       const std::vector<TileXR::TileXRClockSyncDeviceSample> &samples,
                       uint64_t frequency, int rank, uint32_t generation);

} // namespace Host
} // namespace TileXRDiagnostics

#endif
