#pragma once
#include "bench_abi.h"
#include <stdint.h>

struct MultiQpSummary {
    uint64_t start = 0, end = 0, latestStart = 0, earliestEnd = 0;
    uint64_t duration = 0, payloadBytes = 0;
    double gbps = 0;
};

// A common-clock envelope measures one simultaneous launch. Summing individual
// bandwidths would ignore AIV start skew and can overstate the actual throughput.
inline bool SummarizeQps(const BenchResult* results, uint32_t qps,
    uint32_t payload, uint32_t count, MultiQpSummary& out, uint32_t numSge = 1U)
{
    out = {};
    if (!results || !qps || qps > 16U || !payload || !count ||
        !numSge || numSge > 16U || (numSge & (numSge - 1U))) { return false; }
    out.start = UINT64_MAX; out.earliestEnd = UINT64_MAX;
    for (uint32_t q = 0; q < qps; ++q) {
        const auto& r = results[q];
        if (r.magic != 0x5458524D41544348ULL || r.status || r.error || r.end <= r.start) { return false; }
        if (r.start < out.start) { out.start = r.start; }
        if (r.end > out.end) { out.end = r.end; }
        if (r.start > out.latestStart) { out.latestStart = r.start; }
        if (r.end < out.earliestEnd) { out.earliestEnd = r.end; }
    }
    const uint64_t bytesPerSge = uint64_t(payload) * count;
    if (bytesPerSge > UINT64_MAX / numSge) { return false; }
    const uint64_t bytesPerQp = bytesPerSge * numSge;
    if (bytesPerQp > UINT64_MAX / qps) { return false; }
    out.payloadBytes = bytesPerQp * qps;
    out.duration = out.end - out.start;
    out.gbps = double(out.payloadBytes) * 8.0 / double(out.duration);
    return true;
}
