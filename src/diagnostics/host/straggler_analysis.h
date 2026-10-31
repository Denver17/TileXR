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
                              TileXRStragglerReport &report);

uint64_t CyclesToNs(uint64_t cycles, uint64_t cyclesPerSecond);
bool SignedCyclesToNs(int64_t cycles, uint64_t cyclesPerSecond, int64_t &nanoseconds);

} // namespace Host
} // namespace TileXRDiagnostics

#endif // TILEXR_DIAGNOSTICS_HOST_STRAGGLER_ANALYSIS_H
