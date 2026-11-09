/*
 * 功能：声明 Host 侧 clock offset 估计、时间单位换算和观测记录分类接口。
 * 调用关系：straggler_session.cpp 调用这些接口，将采样和记录转换为公开的 clock result 与 report。
 * Purpose: Declare Host offset estimation, time conversion and observation classification.
 * Calls: straggler_session.cpp uses these interfaces to produce public clock results and reports.
 */
#ifndef TILEXR_DIAGNOSTICS_HOST_STRAGGLER_ANALYSIS_H
#define TILEXR_DIAGNOSTICS_HOST_STRAGGLER_ANALYSIS_H

#include <cstdint>
#include <vector>

#include "tilexr_straggler.h"
#include "tilexr_straggler_layout.h"

namespace TileXRDiagnostics {
namespace Host {

bool EstimateClockRank(const std::vector<TileXR::TileXRClockSyncDeviceSample> &samples,
                       uint64_t cyclesPerSecond,
                       int rank,
                       TileXRClockRankResult &result);

int ClassifyStragglerRecords(const TileXRStragglerConfig &config,
                              const TileXRClockSyncResult &clock,
                              const std::vector<TileXR::TileXRStragglerTimingRecord> &records,
                              TileXRStragglerReport &report,
                              const TileXRClockDriftModel *driftModels = nullptr);

uint64_t CyclesToNs(uint64_t cycles, uint64_t cyclesPerSecond);
bool SignedCyclesToNs(int64_t cycles, uint64_t cyclesPerSecond, int64_t &nanoseconds);

} // namespace Host
} // namespace TileXRDiagnostics

#endif // TILEXR_DIAGNOSTICS_HOST_STRAGGLER_ANALYSIS_H
