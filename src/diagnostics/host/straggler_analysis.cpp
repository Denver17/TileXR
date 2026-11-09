/*
 * 功能：校验四时间戳采样并估计 rank offset；对齐观测记录，用 median/MAD 和阈值判定快慢 rank。
 * 调用关系：straggler_session.cpp 提供采样、记录和模型；READY 模型通过 clock_drift.cpp 校正时间戳。
 * 资源与生命周期：输入由调用方持有，计算结果写入传入的 result/report；不操作 stream 或 device buffer。
 * Purpose: Estimate offsets from validated exchanges and classify aligned observations using median/MAD thresholds.
 * Calls: straggler_session.cpp supplies inputs; READY models use clock_drift.cpp for timestamp correction.
 * Lifetime: Inputs belong to the caller and outputs go to result/report; no stream or device buffer is operated here.
 */
#include "straggler_analysis.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

#include "tilexr_types.h"

namespace TileXRDiagnostics {
namespace Host {
namespace {

bool CycleDifference(uint64_t lhs, uint64_t rhs, int64_t &difference)
{
    if (lhs >= rhs) {
        const uint64_t value = lhs - rhs;
        if (value > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
            return false;
        }
        difference = static_cast<int64_t>(value);
        return true;
    }
    const uint64_t value = rhs - lhs;
    if (value > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
        return false;
    }
    difference = -static_cast<int64_t>(value);
    return true;
}

uint64_t MedianUnsigned(std::vector<uint64_t> values)
{
    if (values.empty()) {
        return 0;
    }
    std::sort(values.begin(), values.end());
    const size_t middle = values.size() / 2;
    if ((values.size() & 1u) != 0u) {
        return values[middle];
    }
    return values[middle - 1] / 2u + values[middle] / 2u +
        ((values[middle - 1] & 1u) != 0u && (values[middle] & 1u) != 0u ? 1u : 0u);
}

int64_t MedianSigned(std::vector<int64_t> values)
{
    if (values.empty()) {
        return 0;
    }
    std::sort(values.begin(), values.end());
    const size_t middle = values.size() / 2;
    if ((values.size() & 1u) != 0u) {
        return values[middle];
    }
    const long double sum = static_cast<long double>(values[middle - 1]) +
        static_cast<long double>(values[middle]);
    return static_cast<int64_t>(sum / 2.0L);
}

uint64_t AbsoluteDifference(uint64_t lhs, uint64_t rhs)
{
    return lhs >= rhs ? lhs - rhs : rhs - lhs;
}

bool AddSignedOffset(uint64_t cycle, int64_t offset, int64_t &aligned)
{
    const long double value = static_cast<long double>(cycle) + static_cast<long double>(offset);
    if (value < static_cast<long double>(std::numeric_limits<int64_t>::min()) ||
        value > static_cast<long double>(std::numeric_limits<int64_t>::max())) {
        return false;
    }
    aligned = static_cast<int64_t>(value);
    return true;
}

} // namespace

uint64_t CyclesToNs(uint64_t cycles, uint64_t cyclesPerSecond)
{
    if (cyclesPerSecond == 0) {
        return 0;
    }
    const long double value = static_cast<long double>(cycles) * 1000000000.0L /
        static_cast<long double>(cyclesPerSecond);
    if (value >= static_cast<long double>(std::numeric_limits<uint64_t>::max())) {
        return std::numeric_limits<uint64_t>::max();
    }
    return static_cast<uint64_t>(value + 0.5L);
}

bool SignedCyclesToNs(int64_t cycles, uint64_t cyclesPerSecond, int64_t &nanoseconds)
{
    if (cyclesPerSecond == 0) {
        return false;
    }
    const long double value = static_cast<long double>(cycles) * 1000000000.0L /
        static_cast<long double>(cyclesPerSecond);
    if (value < static_cast<long double>(std::numeric_limits<int64_t>::min()) ||
        value > static_cast<long double>(std::numeric_limits<int64_t>::max())) {
        return false;
    }
    nanoseconds = static_cast<int64_t>(value >= 0.0L ? value + 0.5L : value - 0.5L);
    return true;
}

bool EstimateClockRank(const std::vector<TileXR::TileXRClockSyncDeviceSample> &samples,
                       uint64_t cyclesPerSecond,
                       int rank,
                       TileXRClockRankResult &result)
{
    result = TileXRClockRankResult {};
    result.rank = rank;
    if (rank < 0 || cyclesPerSecond == 0 || samples.empty()) {
        result.status = TileXR::TILEXR_ERROR_PARA_CHECK_FAIL;
        return false;
    }
    struct Candidate {
        uint64_t delay;
        int64_t offset;
        uint32_t sequence;
    };
    std::vector<Candidate> candidates;
    std::vector<uint64_t> delays;
    candidates.reserve(samples.size());
    delays.reserve(samples.size());
    for (size_t index = 0; index < samples.size(); ++index) {
        const TileXR::TileXRClockSyncDeviceSample &sample = samples[index];
        if (sample.status == TileXR::TILEXR_CLOCK_SYNC_SAMPLE_TERMINAL_FAILURE) {
            result.status = TileXR::TILEXR_ERROR_TIMEOUT;
            result.rejectedSamples = static_cast<uint32_t>(samples.size());
            return false;
        }
        if (sample.status != TileXR::TILEXR_CLOCK_SYNC_SAMPLE_SUCCESS ||
            sample.sequence != index || sample.t0 == 0 || sample.t1 == 0 ||
            sample.t2 == 0 || sample.t3 == 0 || sample.t3 < sample.t0 || sample.t2 < sample.t1) {
            continue;
        }
        const uint64_t localElapsed = sample.t3 - sample.t0;
        const uint64_t referenceElapsed = sample.t2 - sample.t1;
        if (localElapsed < referenceElapsed) {
            continue;
        }
        const uint64_t delay = localElapsed - referenceElapsed;
        int64_t first = 0;
        int64_t second = 0;
        if (!CycleDifference(sample.t1, sample.t0, first) ||
            !CycleDifference(sample.t2, sample.t3, second)) {
            continue;
        }
        const long double offsetValue =
            (static_cast<long double>(first) + static_cast<long double>(second)) / 2.0L;
        if (offsetValue < static_cast<long double>(std::numeric_limits<int64_t>::min()) ||
            offsetValue > static_cast<long double>(std::numeric_limits<int64_t>::max())) {
            continue;
        }
        Candidate candidate {delay, static_cast<int64_t>(offsetValue), sample.sequence};
        candidates.push_back(candidate);
        delays.push_back(delay);
    }
    result.validSamples = static_cast<uint32_t>(candidates.size());
    result.rejectedSamples = static_cast<uint32_t>(samples.size() - candidates.size());
    const size_t percentageMinimum =
        (samples.size() * TileXR::TILEXR_CLOCK_SYNC_MIN_VALID_PERCENT + 99u) / 100u;
    const size_t minimumValid = std::max<size_t>(
        TileXR::TILEXR_CLOCK_SYNC_MIN_SAMPLES, percentageMinimum);
    if (candidates.size() < minimumValid) {
        result.status = TileXR::TILEXR_ERROR_INTERNAL;
        return false;
    }
    if (rank == 0) {
        result.status = TileXR::TILEXR_SUCCESS;
        return true;
    }
    const Candidate *best = &candidates[0];
    for (const Candidate &candidate : candidates) {
        if (candidate.delay < best->delay ||
            (candidate.delay == best->delay && candidate.sequence < best->sequence)) {
            best = &candidate;
        }
    }
    result.status = TileXR::TILEXR_SUCCESS;
    result.offsetCycles = best->offset;
    result.minRoundTripCycles = best->delay;
    result.medianRoundTripCycles = MedianUnsigned(delays);
    result.errorBoundCycles = (best->delay + 1u) / 2u;
    if (!SignedCyclesToNs(result.offsetCycles, cyclesPerSecond, result.offsetNs)) {
        result.status = TileXR::TILEXR_ERROR_INTERNAL;
        return false;
    }
    result.minRoundTripNs = CyclesToNs(result.minRoundTripCycles, cyclesPerSecond);
    result.medianRoundTripNs = CyclesToNs(result.medianRoundTripCycles, cyclesPerSecond);
    result.errorBoundNs = CyclesToNs(result.errorBoundCycles, cyclesPerSecond);
    return true;
}

int ClassifyStragglerRecords(const TileXRStragglerConfig &config,
                              const TileXRClockSyncResult &clock,
                              const std::vector<TileXR::TileXRStragglerTimingRecord> &records,
                              TileXRStragglerReport &report,
                              const TileXRClockDriftModel *driftModels)
{
    report = TileXRStragglerReport {};
    if (clock.version != TileXR::TILEXR_STRAGGLER_ABI_VERSION || clock.rankSize == 0 ||
        clock.rankSize > TILEXR_STRAGGLER_MAX_RANKS || records.size() != clock.rankSize ||
        config.relativeThreshold <= 0.0 || config.madThreshold <= 0.0 ||
        config.absoluteThresholdNs == 0 || clock.cyclesPerSecond == 0) {
        return TileXR::TILEXR_ERROR_PARA_CHECK_FAIL;
    }

    report.version = TileXR::TILEXR_STRAGGLER_ABI_VERSION;
    report.rankSize = clock.rankSize;
    report.calibrationGeneration = clock.generation;
    report.clockPrecisionNs = clock.precisionNs;
    report.absoluteThresholdNs = config.absoluteThresholdNs;
    report.relativeThreshold = config.relativeThreshold;
    report.madThreshold = config.madThreshold;
    std::vector<uint64_t> durations;
    std::vector<uint64_t> preCommunicationDurations;
    std::vector<int64_t> starts;
    std::vector<int64_t> ends;
    durations.reserve(records.size());
    preCommunicationDurations.reserve(records.size());
    starts.reserve(records.size());
    ends.reserve(records.size());
    bool complete = true;
    const uint64_t launchId = records[0].launchId;
    const uint64_t operationId = records[0].operationId;
    const uint32_t componentKind = records[0].componentKind;
    const uint32_t operationKind = records[0].operationKind;
    const uint32_t stageId = records[0].stageId;
    const uint64_t messageBytes = records[0].messageBytes;

    for (size_t index = 0; index < records.size(); ++index) {
        const TileXR::TileXRStragglerTimingRecord &record = records[index];
        TileXRStragglerRankObservation &observation = report.ranks[index];
        observation.rank = static_cast<int32_t>(index);
        observation.status = record.status == TileXR::TILEXR_TIMING_RECORD_STATUS_COMPLETE ?
            TileXR::TILEXR_SUCCESS : TileXR::TILEXR_ERROR_INTERNAL;
        observation.classification = TILEXR_STRAGGLER_UNKNOWN;
        observation.localStartCycle = record.startCycle;
        observation.localReadyCycle = record.readyCycle;
        observation.localEndCycle = record.endCycle;
        observation.blockCount = record.blockCount;
        if (record.rank != index || record.launchId != launchId ||
            record.operationId != operationId || record.componentKind != componentKind ||
            record.operationKind != operationKind || record.stageId != stageId ||
            record.messageBytes != messageBytes) {
            observation.reasonFlags |= TILEXR_STRAGGLER_REASON_LAUNCH_MISMATCH;
            complete = false;
            continue;
        }
        if (record.generation != clock.generation) {
            observation.reasonFlags |= TILEXR_STRAGGLER_REASON_CLOCK_UNSYNCHRONIZED;
            complete = false;
            continue;
        }
        if (record.status != TileXR::TILEXR_TIMING_RECORD_STATUS_COMPLETE ||
            record.readyCycle < record.startCycle || record.endCycle < record.readyCycle ||
            clock.ranks[index].status != TileXR::TILEXR_SUCCESS) {
            if (record.status == TileXR::TILEXR_TIMING_RECORD_STATUS_ERROR) {
                observation.reasonFlags |= TILEXR_STRAGGLER_REASON_OBSERVATION_ERROR;
            }
            observation.reasonFlags |= record.status == TileXR::TILEXR_TIMING_RECORD_STATUS_COMPLETE ?
                TILEXR_STRAGGLER_REASON_CLOCK_UNSYNCHRONIZED : TILEXR_STRAGGLER_REASON_INCOMPLETE;
            complete = false;
            continue;
        }
        int64_t alignedStartCycle = 0;
        int64_t alignedReadyCycle = 0;
        int64_t alignedEndCycle = 0;
        auto align = [&](uint64_t cycle, int64_t &aligned) {
            if (driftModels != nullptr && driftModels[index].state == TILEXR_CLOCK_DRIFT_READY) {
                return driftModels[index].rank == static_cast<int32_t>(index) &&
                    driftModels[index].cyclesPerSecond == clock.cyclesPerSecond &&
                    TileXRClockCorrectCycle(&driftModels[index],cycle,&aligned) == TileXR::TILEXR_SUCCESS;
            }
            return AddSignedOffset(cycle,clock.ranks[index].offsetCycles,aligned);
        };
        if (!align(record.startCycle, alignedStartCycle) ||
            !align(record.readyCycle, alignedReadyCycle) ||
            !align(record.endCycle, alignedEndCycle) ||
            !SignedCyclesToNs(alignedStartCycle, clock.cyclesPerSecond, observation.alignedStartNs) ||
            !SignedCyclesToNs(alignedReadyCycle, clock.cyclesPerSecond, observation.alignedReadyNs) ||
            !SignedCyclesToNs(alignedEndCycle, clock.cyclesPerSecond, observation.alignedEndNs)) {
            observation.reasonFlags |= TILEXR_STRAGGLER_REASON_CLOCK_UNSYNCHRONIZED;
            complete = false;
            continue;
        }
        observation.durationNs = CyclesToNs(record.endCycle - record.startCycle, clock.cyclesPerSecond);
        observation.preCommunicationNs = CyclesToNs(
            record.readyCycle - record.startCycle, clock.cyclesPerSecond);
        durations.push_back(observation.durationNs);
        preCommunicationDurations.push_back(observation.preCommunicationNs);
        starts.push_back(observation.alignedStartNs);
        ends.push_back(observation.alignedEndNs);
    }

    report.launchId = launchId;
    report.operationId = operationId;
    report.componentKind = componentKind;
    report.operationKind = operationKind;
    report.stageId = stageId;
    report.messageBytes = messageBytes;
    report.complete = complete ? 1u : 0u;
    if (!complete || durations.size() != records.size()) {
        return TileXR::TILEXR_ERROR_INTERNAL;
    }

    report.medianDurationNs = MedianUnsigned(durations);
    std::vector<uint64_t> deviations;
    deviations.reserve(durations.size());
    for (uint64_t duration : durations) {
        deviations.push_back(AbsoluteDifference(duration, report.medianDurationNs));
    }
    report.madDurationNs = MedianUnsigned(deviations);
    report.medianPreCommunicationNs = MedianUnsigned(preCommunicationDurations);
    std::vector<uint64_t> preCommunicationDeviations;
    preCommunicationDeviations.reserve(preCommunicationDurations.size());
    for (uint64_t duration : preCommunicationDurations) {
        preCommunicationDeviations.push_back(
            AbsoluteDifference(duration, report.medianPreCommunicationNs));
    }
    report.madPreCommunicationNs = MedianUnsigned(preCommunicationDeviations);
    const int64_t medianStartNs = MedianSigned(starts);
    std::vector<uint64_t> startDeviations;
    startDeviations.reserve(starts.size());
    for (int64_t start : starts) {
        const long double delta = std::fabs(static_cast<long double>(start) -
                                           static_cast<long double>(medianStartNs));
        startDeviations.push_back(static_cast<uint64_t>(delta));
    }
    const uint64_t startMadNs = MedianUnsigned(startDeviations);
    const int64_t medianEndNs = MedianSigned(ends);
    std::vector<uint64_t> endDeviations;
    endDeviations.reserve(ends.size());
    for (int64_t end : ends) {
        const long double delta = std::fabs(static_cast<long double>(end) -
                                            static_cast<long double>(medianEndNs));
        endDeviations.push_back(static_cast<uint64_t>(delta));
    }
    const uint64_t endMadNs = MedianUnsigned(endDeviations);

    for (size_t index = 0; index < records.size(); ++index) {
        TileXRStragglerRankObservation &observation = report.ranks[index];
        const long double delta = static_cast<long double>(observation.durationNs) -
            static_cast<long double>(report.medianDurationNs);
        observation.durationDeltaNs = static_cast<int64_t>(delta);
        observation.relativeDelta = report.medianDurationNs == 0 ? 0.0 :
            static_cast<double>(delta / static_cast<long double>(report.medianDurationNs));
        observation.robustScore = report.madDurationNs == 0 ? 0.0 :
            0.67448975 * static_cast<double>(delta / static_cast<long double>(report.madDurationNs));

        const bool absoluteGate = std::fabs(delta) >= static_cast<long double>(config.absoluteThresholdNs);
        const bool relativeGate = std::fabs(observation.relativeDelta) >= config.relativeThreshold;
        const bool robustGate = report.madDurationNs == 0 ||
            std::fabs(observation.robustScore) >= config.madThreshold;
        const bool durationOutlier = absoluteGate && relativeGate && robustGate;
        if (durationOutlier) {
            observation.reasonFlags |= TILEXR_STRAGGLER_REASON_DURATION;
        }

        const long double preCommunicationDelta =
            static_cast<long double>(observation.preCommunicationNs) -
            static_cast<long double>(report.medianPreCommunicationNs);
        observation.preCommunicationDeltaNs = static_cast<int64_t>(preCommunicationDelta);
        observation.preCommunicationRobustScore = report.madPreCommunicationNs == 0 ? 0.0 :
            0.67448975 * static_cast<double>(preCommunicationDelta /
                static_cast<long double>(report.madPreCommunicationNs));
        const bool preCommunicationAbsoluteGate = preCommunicationDelta >=
            static_cast<long double>(config.absoluteThresholdNs);
        const bool preCommunicationRelativeGate = report.medianDurationNs != 0 &&
            preCommunicationDelta / static_cast<long double>(report.medianDurationNs) >=
                config.relativeThreshold;
        const bool preCommunicationRobustGate = report.madPreCommunicationNs == 0 ||
            observation.preCommunicationRobustScore >= config.madThreshold;
        const bool preCommunicationOutlier = preCommunicationAbsoluteGate &&
            preCommunicationRelativeGate && preCommunicationRobustGate;
        if (preCommunicationOutlier) {
            observation.reasonFlags |= TILEXR_STRAGGLER_REASON_PRE_COMMUNICATION;
        }

        const long double startDelta = static_cast<long double>(observation.alignedStartNs) -
            static_cast<long double>(medianStartNs);
        observation.startDeltaNs = static_cast<int64_t>(startDelta);
        observation.startRobustScore = startMadNs == 0 ? 0.0 :
            0.67448975 * static_cast<double>(startDelta / static_cast<long double>(startMadNs));
        const bool startAbsoluteGate = std::fabs(startDelta) >=
            static_cast<long double>(config.absoluteThresholdNs);
        const bool startRobustGate = startMadNs == 0 ||
            std::fabs(observation.startRobustScore) >= config.madThreshold;
        const bool startOutlier = startAbsoluteGate && startRobustGate;
        const bool lateStart = startOutlier && startDelta > 0;
        const bool earlyStart = startOutlier && startDelta < 0;
        if (lateStart) {
            observation.reasonFlags |= TILEXR_STRAGGLER_REASON_LATE_START;
        }
        if (earlyStart) {
            observation.reasonFlags |= TILEXR_STRAGGLER_REASON_EARLY_START;
        }

        const long double endDelta = static_cast<long double>(observation.alignedEndNs) -
            static_cast<long double>(medianEndNs);
        observation.completionDeltaNs = static_cast<int64_t>(endDelta);
        observation.completionRobustScore = endMadNs == 0 ? 0.0 :
            0.67448975 * static_cast<double>(endDelta / static_cast<long double>(endMadNs));
        const bool endAbsoluteGate = std::fabs(endDelta) >=
            static_cast<long double>(config.absoluteThresholdNs);
        const bool endRelativeGate = report.medianDurationNs != 0 &&
            std::fabs(endDelta / static_cast<long double>(report.medianDurationNs)) >=
                config.relativeThreshold;
        const bool endRobustGate = endMadNs == 0 ||
            std::fabs(observation.completionRobustScore) >= config.madThreshold;
        const bool completionOutlier = endAbsoluteGate && endRelativeGate && endRobustGate;
        if (completionOutlier) {
            observation.reasonFlags |= TILEXR_STRAGGLER_REASON_COMPLETION;
        }

        const bool slow = preCommunicationOutlier ||
            (completionOutlier && endDelta > 0) ||
            (durationOutlier && delta > 0 && !earlyStart);
        const bool fast = (completionOutlier && endDelta < 0) ||
            (durationOutlier && delta < 0 && !lateStart);
        observation.classification = slow ? TILEXR_STRAGGLER_SLOW :
            (fast ? TILEXR_STRAGGLER_FAST : TILEXR_STRAGGLER_NORMAL);
    }
    return TileXR::TILEXR_SUCCESS;
}

} // namespace Host
} // namespace TileXRDiagnostics
