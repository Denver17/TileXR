/*
 * 功能：拟合相对逻辑 rank 0 的 offset 和 rate，按确认的残差更新补偿，并校正本地时间戳。
 * 调用关系：straggler_session.cpp 提供采样；公开校正接口也供 straggler_analysis.cpp 使用。
 * 资源与生命周期：采样点和模型保存在调用方持有的 ClockDriftTracker 中，本文件不分配 device buffer。
 * Purpose: Fit and maintain offsets/rates relative to logical rank 0 and correct local timestamps.
 * Calls: straggler_session.cpp supplies samples; straggler_analysis.cpp also uses the correction API.
 * Lifetime: The caller owns the tracker, its points and model; no device buffers are allocated here.
 */
#include "clock_drift.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include "tilexr_types.h"

namespace TileXRDiagnostics {
namespace Host {
namespace {

long double Delta(uint64_t value, uint64_t anchor)
{
    return value >= anchor ? static_cast<long double>(value - anchor) :
        -static_cast<long double>(anchor - value);
}

bool ValidClockSample(const TileXR::TileXRClockSyncDeviceSample &sample, size_t sequence)
{
    return sample.status == TileXR::TILEXR_CLOCK_SYNC_SAMPLE_SUCCESS &&
        sample.sequence == sequence && sample.t0 != 0 && sample.t1 != 0 && sample.t2 != 0 &&
        sample.t3 >= sample.t0 && sample.t2 >= sample.t1 &&
        sample.t3 - sample.t0 >= sample.t2 - sample.t1;
}

long double Median(std::vector<long double> values)
{
    std::sort(values.begin(),values.end());
    const size_t mid = values.size()/2;
    return values.size()%2 ? values[mid] : (values[mid-1]+values[mid])/2;
}

void TrackClockDrift(ClockDriftTracker &tracker,
                     const std::vector<TileXR::TileXRClockSyncDeviceSample> &samples,
                     uint64_t frequency, int rank, uint32_t generation)
{
    // Two contiguous groups confirm the sign/threshold; remaining samples can
    // independently audit the proposed correction before publication.
    if (tracker.model.state != TILEXR_CLOCK_DRIFT_READY || rank == 0 ||
        samples.size() < 32) { return; }
    const auto old = tracker.model;
    std::vector<long double> errors[2], offsets;
    const auto &last = samples[31];
    if (last.t3 < last.t0) { return; }
    const uint64_t anchor = last.t0+(last.t3-last.t0)/2;
    if (anchor <= old.anchorCycle) { return; }
    for (size_t i = 0; i < 32; ++i) {
        const auto &s = samples[i];
        if (!ValidClockSample(s, i) ||
            (i != 0 && s.t0 <= samples[i-1].t0)) { return; }
        const long double x = Delta(s.t0,old.anchorCycle)+(s.t3-s.t0)/2.0L;
        const long double offset = (Delta(s.t1,s.t0)+Delta(s.t2,s.t3))/2;
        errors[i/16].push_back(old.offsetCycles+old.rateCorrection*x-offset);
        const long double toAnchor = Delta(anchor,s.t0)-(s.t3-s.t0)/2.0L;
        offsets.push_back(offset+old.rateCorrection*toAnchor);
    }
    const long double first = Median(errors[0])*1000000000.0L/frequency;
    const long double second = Median(errors[1])*1000000000.0L/frequency;
    tracker.residualNs = static_cast<double>((first+second)/2);
    tracker.probeValid = true;
    if (std::fabs(first) <= 1000 || std::fabs(second) <= 1000 || first*second <= 0) { return; }
    const long double offset = Median(offsets);
    const long double measuredRate = (offset-old.offsetCycles)/(anchor-old.anchorCycle);
    const long double rate = 0.75L*old.rateCorrection+0.25L*measuredRate;
    if (!std::isfinite(rate) || std::fabs(rate) >= 0.001L || !std::isfinite(offset)) { return; }
    tracker.model.anchorCycle = anchor;
    tracker.model.offsetCycles = static_cast<double>(offset);
    tracker.model.rateCorrection = static_cast<double>(rate);
    tracker.model.trainedAtGeneration = generation;
    tracker.updated = true;
}

} // namespace

TileXRClockDriftModel FitClockDrift(const std::vector<ClockDriftPoint> &points,
                                   uint64_t frequency, int rank)
{
    TileXRClockDriftModel model {};
    model.version = 1;
    model.rank = rank;
    model.cyclesPerSecond = frequency;
    model.sampleCount = static_cast<uint32_t>(points.size());
    model.state = TILEXR_CLOCK_DRIFT_INSUFFICIENT;
    if (points.empty() || frequency == 0 || rank < 0) { return model; }
    model.anchorCycle = points.back().localCycle;
    if (model.anchorCycle < points.front().localCycle) { return model; }
    model.spanCycles = model.anchorCycle - points.front().localCycle;
    if (points.size() < 4 || model.spanCycles == 0) { return model; }
    long double meanX = 0, meanY = 0;
    for (size_t i = 0; i < points.size(); ++i) {
        if (!std::isfinite(points[i].offsetCycles) ||
            (i != 0 && points[i].localCycle <= points[i-1].localCycle)) { return model; }
        meanX += Delta(points[i].localCycle, model.anchorCycle);
        meanY += points[i].offsetCycles;
    }
    meanX /= points.size(); meanY /= points.size();
    long double xx = 0, xy = 0;
    for (const auto &point : points) {
        const long double x = Delta(point.localCycle, model.anchorCycle) - meanX;
        xx += x*x;
        xy += x*(point.offsetCycles - meanY);
    }
    if (xx <= 0) { return model; }
    const long double rate = xy/xx;
    const long double offset = meanY - rate*meanX;
    long double squaredError = 0;
    for (const auto &point : points) {
        const long double error = point.offsetCycles - offset -
            rate*Delta(point.localCycle, model.anchorCycle);
        squaredError += error*error;
    }
    model.offsetCycles = static_cast<double>(offset);
    model.rateCorrection = static_cast<double>(rate);
    model.fitRmsCycles = static_cast<double>(std::sqrt(squaredError/points.size()));
    model.rateStandardError = static_cast<double>(
        std::sqrt(squaredError/(points.size()-2)/xx));
    // A sanity gate, not a confidence guarantee for long-term extrapolation.
    model.state = std::isfinite(model.offsetCycles) && std::isfinite(model.rateCorrection) &&
        std::isfinite(model.rateStandardError) && std::fabs(rate) < 0.001L &&
        model.rateStandardError < 0.000001 ? TILEXR_CLOCK_DRIFT_READY : TILEXR_CLOCK_DRIFT_UNSTABLE;
    return model;
}

void UpdateClockDrift(ClockDriftTracker &tracker,
                       const std::vector<TileXR::TileXRClockSyncDeviceSample> &samples,
                       uint64_t frequency, int rank, uint32_t generation)
{
    tracker.updated = false;
    tracker.residualNs = 0;
    tracker.probeValid = false;
    if (tracker.windowUs == 0) { return; }
    if (tracker.closed) {
        if (tracker.tracking) { TrackClockDrift(tracker,samples,frequency,rank,generation); }
        return;
    }
    tracker.model.version = 1;
    tracker.model.state = TILEXR_CLOCK_DRIFT_COLLECTING;
    tracker.model.rank = rank;
    tracker.model.cyclesPerSecond = frequency;
    tracker.model.requestedWindowUs = tracker.windowUs;
    const long double target = static_cast<long double>(frequency)*tracker.windowUs/1000000.0L;
    for (size_t i = 0; i < samples.size(); ++i) {
        const auto &s = samples[i];
        if (!ValidClockSample(s, i)) {
            ++tracker.model.rejectedSamples;
            continue;
        }
        const uint64_t midpoint = s.t0 + (s.t3-s.t0)/2;
        if (!tracker.points.empty() && midpoint <= tracker.points.back().localCycle) {
            ++tracker.model.rejectedSamples;
            continue;
        }
        const long double offset = rank == 0 ? 0 :
            (Delta(s.t1,s.t0) + Delta(s.t2,s.t3))/2;
        tracker.points.push_back({midpoint,offset});
        tracker.model.sampleCount = static_cast<uint32_t>(tracker.points.size());
        tracker.model.spanCycles = midpoint-tracker.points.front().localCycle;
        if (static_cast<long double>(tracker.model.spanCycles) >= target) {
            const uint32_t rejected = tracker.model.rejectedSamples;
            tracker.model = FitClockDrift(tracker.points,frequency,rank);
            tracker.model.requestedWindowUs = tracker.windowUs;
            tracker.model.rejectedSamples = rejected;
            tracker.model.trainedAtGeneration = generation;
            tracker.initialGeneration = generation;
            tracker.updated = tracker.model.state == TILEXR_CLOCK_DRIFT_READY;
            tracker.closed = true;
            break;
        }
    }
}

} // namespace Host
} // namespace TileXRDiagnostics

extern "C" int TileXRClockCorrectCycle(const TileXRClockDriftModel *model,
                                        uint64_t localCycle, int64_t *referenceCycle)
{
    if (model == nullptr || referenceCycle == nullptr || model->version != 1 ||
        model->state != TILEXR_CLOCK_DRIFT_READY || model->cyclesPerSecond == 0 ||
        !std::isfinite(model->offsetCycles) || !std::isfinite(model->rateCorrection) ||
        std::fabs(model->rateCorrection) >= 0.001) { return TileXR::TILEXR_ERROR_PARA_CHECK_FAIL; }
    const long double delta = localCycle >= model->anchorCycle ?
        static_cast<long double>(localCycle-model->anchorCycle) :
        -static_cast<long double>(model->anchorCycle-localCycle);
    const long double value = static_cast<long double>(localCycle) + model->offsetCycles +
        static_cast<long double>(model->rateCorrection)*delta;
    const long double rounded = std::round(value);
    if (!std::isfinite(rounded) || rounded < std::numeric_limits<int64_t>::min() ||
        rounded > std::numeric_limits<int64_t>::max()) { return TileXR::TILEXR_ERROR_PARA_CHECK_FAIL; }
    *referenceCycle = static_cast<int64_t>(rounded);
    return TileXR::TILEXR_SUCCESS;
}
