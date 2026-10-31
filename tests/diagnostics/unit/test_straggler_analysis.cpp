#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <type_traits>
#include <vector>

#include "straggler_analysis.h"
#include "tilexr_straggler.h"
#include "tilexr_straggler_layout.h"
#include "tilexr_types.h"

namespace {

int g_failures = 0;

template <typename T>
void CheckEq(const char *label, T actual, T expected)
{
    if (actual != expected) {
        std::cerr << label << " actual=" << actual << " expected=" << expected << std::endl;
        ++g_failures;
    }
}

void CheckTrue(const char *label, bool value)
{
    if (!value) {
        std::cerr << label << " failed" << std::endl;
        ++g_failures;
    }
}

TileXR::TileXRClockSyncDeviceSample Sample(uint64_t t0, uint64_t t1, uint64_t t2, uint64_t t3,
                                           uint32_t sequence, uint32_t status = 0)
{
    TileXR::TileXRClockSyncDeviceSample sample {};
    sample.t0 = t0;
    sample.t1 = t1;
    sample.t2 = t2;
    sample.t3 = t3;
    sample.sequence = sequence;
    sample.status = status;
    return sample;
}

TileXR::TileXRStragglerTimingRecord Record(uint32_t rank, uint64_t launch,
                                             uint64_t start, uint64_t duration)
{
    TileXR::TileXRStragglerTimingRecord record {};
    record.rank = rank;
    record.launchId = launch;
    record.operationId = launch;
    record.componentKind = TILEXR_STRAGGLER_COMPONENT_COLLECTIVES;
    record.operationKind = static_cast<uint32_t>(TileXR::TileXRType::ALL_REDUCE);
    record.messageBytes = 4096;
    record.startCycle = start;
    record.readyCycle = start;
    record.endCycle = start + duration;
    record.status = TileXR::TILEXR_TIMING_RECORD_STATUS_COMPLETE;
    record.generation = 7;
    return record;
}

TileXRClockSyncResult Clock(uint32_t rankSize)
{
    TileXRClockSyncResult clock {};
    clock.version = TileXR::TILEXR_STRAGGLER_ABI_VERSION;
    clock.rankSize = rankSize;
    clock.sampleCount = 32;
    clock.generation = 7;
    clock.cyclesPerSecond = 1000000000ull;
    clock.precisionCycles = 100;
    clock.precisionNs = 100;
    for (uint32_t rank = 0; rank < rankSize; ++rank) {
        clock.ranks[rank].rank = static_cast<int32_t>(rank);
        clock.ranks[rank].status = TileXR::TILEXR_SUCCESS;
    }
    return clock;
}

TileXRStragglerConfig Config()
{
    TileXRStragglerConfig config {};
    config.clockSyncSamples = 32;
    config.clockSyncWarmupSamples = 4;
    config.recordCapacity = 64;
    config.relativeThreshold = 0.2;
    config.madThreshold = 3.0;
    config.absoluteThresholdNs = 50;
    config.timeoutMs = 1000;
    config.maxCalibrationAgeMs = 60000;
    return config;
}

void TestLayout()
{
    CheckEq("protocol slot size", sizeof(TileXR::TileXRClockSyncProtocolSlot), static_cast<size_t>(64));
    CheckEq("sample size", sizeof(TileXR::TileXRClockSyncDeviceSample), static_cast<size_t>(64));
    CheckEq("timing record size", sizeof(TileXR::TileXRStragglerTimingRecord), static_cast<size_t>(80));
    CheckTrue("clock result standard layout", std::is_standard_layout<TileXRClockSyncResult>::value);
    CheckTrue("report standard layout", std::is_standard_layout<TileXRStragglerReport>::value);
    CheckEq("config size", sizeof(TileXRStragglerConfig), static_cast<size_t>(56));
    CheckEq("config output offset", offsetof(TileXRStragglerConfig, outputDir),
            static_cast<size_t>(48));
    CheckEq("clock rank result size", sizeof(TileXRClockRankResult), static_cast<size_t>(80));
    CheckEq("clock result size", sizeof(TileXRClockSyncResult), static_cast<size_t>(10288));
    CheckEq("clock result ranks offset", offsetof(TileXRClockSyncResult, ranks), static_cast<size_t>(48));
    CheckEq("rank observation size", sizeof(TileXRStragglerRankObservation), static_cast<size_t>(160));
    CheckEq("rank observation block count offset", offsetof(TileXRStragglerRankObservation, blockCount),
            static_cast<size_t>(152));
    CheckEq("operation size", sizeof(TileXRStragglerOperation), static_cast<size_t>(32));
    CheckEq("observation size", sizeof(TileXRStragglerObservation), static_cast<size_t>(72));
    CheckEq("report size", sizeof(TileXRStragglerReport), static_cast<size_t>(20616));
    CheckEq("report ranks offset", offsetof(TileXRStragglerReport, ranks), static_cast<size_t>(136));
    CheckEq("record block count offset", offsetof(TileXR::TileXRStragglerTimingRecord, blockCount),
            static_cast<size_t>(72));
    CheckEq("rank result count", sizeof(((TileXRClockSyncResult *)nullptr)->ranks) /
        sizeof(TileXRClockRankResult), static_cast<size_t>(TILEXR_STRAGGLER_MAX_RANKS));
    CheckTrue("clock region before payload",
        TileXR::TILEXR_CLOCK_SYNC_REGION_OFFSET + TileXR::TILEXR_CLOCK_SYNC_REGION_BYTES <=
        static_cast<uint64_t>(TileXR::IPC_DATA_OFFSET));
}

void TestClockEstimatorSelectsMinimumDelay()
{
    std::vector<TileXR::TileXRClockSyncDeviceSample> samples;
    // All samples completed. Sequence 1 has the minimum delay.
    samples.push_back(Sample(1000, 1120, 1130, 1050, 0)); // delay 40
    samples.push_back(Sample(2000, 2110, 2120, 2030, 1)); // delay 20
    samples.push_back(Sample(3000, 3115, 3125, 3040, 2)); // delay 30
    samples.push_back(Sample(4000, 4140, 4150, 4090, 3)); // delay 80

    TileXRClockRankResult result {};
    CheckTrue("estimate rank", TileXRDiagnostics::Host::EstimateClockRank(
        samples, 1000000000ull, 1, result));
    CheckEq("rank status", result.status, TileXR::TILEXR_SUCCESS);
    CheckEq("offset cycles", result.offsetCycles, static_cast<int64_t>(100));
    CheckEq("minimum RTT", result.minRoundTripCycles, static_cast<uint64_t>(20));
    CheckEq("median RTT", result.medianRoundTripCycles, static_cast<uint64_t>(35));
    CheckEq("error bound", result.errorBoundCycles, static_cast<uint64_t>(10));
    CheckEq("offset ns", result.offsetNs, static_cast<int64_t>(100));
    CheckEq("valid samples", result.validSamples, static_cast<uint32_t>(4));
    CheckEq("rejected samples", result.rejectedSamples, static_cast<uint32_t>(0));
}

void TestClockEstimatorRejectsInvalidSamples()
{
    std::vector<TileXR::TileXRClockSyncDeviceSample> samples;
    samples.push_back(Sample(100, 200, 190, 300, 0));
    samples.push_back(Sample(400, 500, 510, 390, 1));
    TileXRClockRankResult result {};
    CheckTrue("reject invalid clock samples", !TileXRDiagnostics::Host::EstimateClockRank(
        samples, 1000000000ull, 2, result));
    CheckEq("invalid clock status", result.status, TileXR::TILEXR_ERROR_INTERNAL);
}

void TestClockEstimatorRejectsUnwrittenSamples()
{
    std::vector<TileXR::TileXRClockSyncDeviceSample> samples(4);
    TileXRClockRankResult result {};
    CheckTrue("reject unwritten clock samples", !TileXRDiagnostics::Host::EstimateClockRank(
        samples, 1000000000ull, 1, result));
    CheckEq("unwritten clock status", result.status, TileXR::TILEXR_ERROR_INTERNAL);
}

void TestClockEstimatorRejectsPartialCompletion()
{
    std::vector<TileXR::TileXRClockSyncDeviceSample> samples;
    samples.push_back(Sample(1000, 1100, 1100, 1000, 0));
    samples.push_back(Sample(2000, 2100, 2100, 2000, 1));
    samples.push_back(Sample(0, 0, 0, 0, 2,
                             TileXR::TILEXR_CLOCK_SYNC_SAMPLE_TERMINAL_FAILURE));
    samples.push_back(Sample(0, 0, 0, 0, 0));
    TileXRClockRankResult result {};
    CheckTrue("reject partial clock completion", !TileXRDiagnostics::Host::EstimateClockRank(
        samples, 1000000000ull, 1, result));
    CheckEq("partial clock status", result.status, TileXR::TILEXR_ERROR_TIMEOUT);
}

void TestClockEstimatorRequiresSeventyFivePercentValid()
{
    std::vector<TileXR::TileXRClockSyncDeviceSample> samples;
    for (uint32_t sequence = 0; sequence < 8; ++sequence) {
        samples.push_back(sequence < 5 ?
            Sample(1000 + sequence * 100, 1100 + sequence * 100,
                   1100 + sequence * 100, 1000 + sequence * 100, sequence) :
            Sample(0, 0, 0, 0, sequence,
                   TileXR::TILEXR_CLOCK_SYNC_SAMPLE_INVALID_TIMING));
    }
    TileXRClockRankResult result {};
    CheckTrue("reject clock sample set below 75 percent",
        !TileXRDiagnostics::Host::EstimateClockRank(samples, 1000000000ull, 1, result));
    CheckEq("below-threshold valid sample count", result.validSamples, static_cast<uint32_t>(5));
}

void TestClassifiesSlowAndFastRanks()
{
    TileXRClockSyncResult clock = Clock(5);
    std::vector<TileXR::TileXRStragglerTimingRecord> records;
    records.push_back(Record(0, 9, 10000, 1000));
    records.push_back(Record(1, 9, 10010, 1000));
    records.push_back(Record(2, 9, 10020, 1000));
    records.push_back(Record(3, 9, 10030, 1600));
    records.push_back(Record(4, 9, 10040, 400));

    TileXRStragglerReport report {};
    CheckEq("classify", TileXRDiagnostics::Host::ClassifyStragglerRecords(
        Config(), clock, records, report), TileXR::TILEXR_SUCCESS);
    CheckEq("median duration", report.medianDurationNs, static_cast<uint64_t>(1000));
    CheckEq("zero MAD", report.madDurationNs, static_cast<uint64_t>(0));
    CheckEq("normal rank", report.ranks[0].classification,
        static_cast<uint32_t>(TILEXR_STRAGGLER_NORMAL));
    CheckEq("slow rank", report.ranks[3].classification,
        static_cast<uint32_t>(TILEXR_STRAGGLER_SLOW));
    CheckEq("fast rank", report.ranks[4].classification,
        static_cast<uint32_t>(TILEXR_STRAGGLER_FAST));
    CheckTrue("slow duration reason",
        (report.ranks[3].reasonFlags & TILEXR_STRAGGLER_REASON_DURATION) != 0);
}

void TestClassifiesPreCommunicationStall()
{
    TileXRClockSyncResult clock = Clock(4);
    std::vector<TileXR::TileXRStragglerTimingRecord> records;
    for (uint32_t rank = 0; rank < 4; ++rank) {
        records.push_back(Record(rank, 15, 1000, 5100));
    }
    records[1].readyCycle = records[1].startCycle + 5000;

    TileXRStragglerConfig config = Config();
    config.absoluteThresholdNs = 1000;
    TileXRStragglerReport report {};
    CheckEq("classify pre-communication stall", TileXRDiagnostics::Host::ClassifyStragglerRecords(
        config, clock, records, report), TileXR::TILEXR_SUCCESS);
    CheckEq("stalled rank is slow", report.ranks[1].classification,
        static_cast<uint32_t>(TILEXR_STRAGGLER_SLOW));
    CheckTrue("stalled rank reason",
        (report.ranks[1].reasonFlags & TILEXR_STRAGGLER_REASON_PRE_COMMUNICATION) != 0);
    CheckEq("non-stalled rank remains normal", report.ranks[0].classification,
        static_cast<uint32_t>(TILEXR_STRAGGLER_NORMAL));

    config.relativeThreshold = 2.0;
    CheckEq("classify pre-communication relative floor",
        TileXRDiagnostics::Host::ClassifyStragglerRecords(config, clock, records, report),
        TileXR::TILEXR_SUCCESS);
    CheckEq("relative floor suppresses insignificant pre-communication stall",
        report.ranks[1].classification, static_cast<uint32_t>(TILEXR_STRAGGLER_NORMAL));
}

void TestAbsoluteFloorAndLateCompletion()
{
    TileXRClockSyncResult clock = Clock(4);
    std::vector<TileXR::TileXRStragglerTimingRecord> records;
    records.push_back(Record(0, 10, 1000, 100));
    records.push_back(Record(1, 10, 1000, 100));
    records.push_back(Record(2, 10, 2000, 100));
    records.push_back(Record(3, 10, 1000, 130));

    TileXRStragglerConfig config = Config();
    config.absoluteThresholdNs = 50;
    TileXRStragglerReport report {};
    CheckEq("classify late start", TileXRDiagnostics::Host::ClassifyStragglerRecords(
        config, clock, records, report), TileXR::TILEXR_SUCCESS);
    CheckEq("small absolute delta remains normal", report.ranks[3].classification,
        static_cast<uint32_t>(TILEXR_STRAGGLER_NORMAL));
    CheckEq("late completion is slow", report.ranks[2].classification,
        static_cast<uint32_t>(TILEXR_STRAGGLER_SLOW));
    CheckTrue("late start reason",
        (report.ranks[2].reasonFlags & TILEXR_STRAGGLER_REASON_LATE_START) != 0);
    CheckTrue("late completion reason",
        (report.ranks[2].reasonFlags & TILEXR_STRAGGLER_REASON_COMPLETION) != 0);
    CheckTrue("late completion is not a duration outlier",
        (report.ranks[2].reasonFlags & TILEXR_STRAGGLER_REASON_DURATION) == 0);
}

void TestLateStartDoesNotLookFast()
{
    TileXRClockSyncResult clock = Clock(4);
    std::vector<TileXR::TileXRStragglerTimingRecord> records;
    records.push_back(Record(0, 14, 1000, 1100));
    records.push_back(Record(1, 14, 1000, 1100));
    records.push_back(Record(2, 14, 1000, 1100));
    records.push_back(Record(3, 14, 1900, 200));

    TileXRStragglerReport report {};
    CheckEq("classify late-start short duration", TileXRDiagnostics::Host::ClassifyStragglerRecords(
        Config(), clock, records, report), TileXR::TILEXR_SUCCESS);
    CheckEq("late-start short duration remains normal", report.ranks[3].classification,
        static_cast<uint32_t>(TILEXR_STRAGGLER_NORMAL));
    CheckTrue("late-start short duration reason",
        (report.ranks[3].reasonFlags & TILEXR_STRAGGLER_REASON_LATE_START) != 0);
    CheckTrue("short duration remains observable",
        (report.ranks[3].reasonFlags & TILEXR_STRAGGLER_REASON_DURATION) != 0);
}

void TestEarlyStartWaitDoesNotLookSlow()
{
    TileXRClockSyncResult clock = Clock(4);
    std::vector<TileXR::TileXRStragglerTimingRecord> records;
    records.push_back(Record(0, 15, 2000, 1000));
    records.push_back(Record(1, 15, 2000, 1000));
    records.push_back(Record(2, 15, 2000, 1000));
    records.push_back(Record(3, 15, 1000, 2000));
    TileXRStragglerReport report {};
    CheckEq("classify early-start wait", TileXRDiagnostics::Host::ClassifyStragglerRecords(
        Config(), clock, records, report), TileXR::TILEXR_SUCCESS);
    CheckEq("early-start wait remains normal", report.ranks[3].classification,
        static_cast<uint32_t>(TILEXR_STRAGGLER_NORMAL));
    CheckTrue("early-start reason",
        (report.ranks[3].reasonFlags & TILEXR_STRAGGLER_REASON_EARLY_START) != 0);
}

void TestCompletionMadUsesConsistencyFactor()
{
    TileXRClockSyncResult clock = Clock(7);
    const uint64_t ends[] = {800, 900, 1000, 1000, 1000, 1100, 1120};
    std::vector<TileXR::TileXRStragglerTimingRecord> records;
    for (uint32_t rank = 0; rank < 7; ++rank) {
        records.push_back(Record(rank, 17, ends[rank] - 400, 400));
    }
    TileXRStragglerConfig config = Config();
    config.madThreshold = 1.0;
    TileXRStragglerReport report {};
    CheckEq("classify completion MAD boundary", TileXRDiagnostics::Host::ClassifyStragglerRecords(
        config, clock, records, report), TileXR::TILEXR_SUCCESS);
    CheckTrue("scaled completion score is below threshold",
        std::fabs(report.ranks[6].completionRobustScore) < config.madThreshold);
    CheckTrue("unscaled completion ratio would exceed threshold",
        std::fabs(static_cast<double>(report.ranks[6].completionDeltaNs) / 100.0) >=
            config.madThreshold);
    CheckTrue("completion reason respects scaled score",
        (report.ranks[6].reasonFlags & TILEXR_STRAGGLER_REASON_COMPLETION) == 0);
}

void TestObservationErrorReason()
{
    TileXRClockSyncResult clock = Clock(2);
    std::vector<TileXR::TileXRStragglerTimingRecord> records;
    records.push_back(Record(0, 16, 1000, 100));
    records.push_back(Record(1, 16, 1000, 100));
    records[1].status = TileXR::TILEXR_TIMING_RECORD_STATUS_ERROR;
    TileXRStragglerReport report {};
    CheckEq("observation error fails", TileXRDiagnostics::Host::ClassifyStragglerRecords(
        Config(), clock, records, report), TileXR::TILEXR_ERROR_INTERNAL);
    CheckTrue("observation error reason",
        (report.ranks[1].reasonFlags & TILEXR_STRAGGLER_REASON_OBSERVATION_ERROR) != 0);
}

void TestRejectsMismatchedLaunch()
{
    TileXRClockSyncResult clock = Clock(2);
    std::vector<TileXR::TileXRStragglerTimingRecord> records;
    records.push_back(Record(0, 11, 1000, 100));
    records.push_back(Record(1, 12, 1000, 100));
    TileXRStragglerReport report {};
    CheckEq("mismatched launch fails", TileXRDiagnostics::Host::ClassifyStragglerRecords(
        Config(), clock, records, report), TileXR::TILEXR_ERROR_INTERNAL);
    CheckEq("incomplete report", report.complete, static_cast<uint32_t>(0));
    CheckTrue("launch mismatch reason",
        (report.ranks[1].reasonFlags & TILEXR_STRAGGLER_REASON_LAUNCH_MISMATCH) != 0);
}

void TestRejectsMismatchedCalibration()
{
    TileXRClockSyncResult clock = Clock(2);
    std::vector<TileXR::TileXRStragglerTimingRecord> records;
    records.push_back(Record(0, 13, 1000, 100));
    records.push_back(Record(1, 13, 1000, 100));
    records[1].generation = clock.generation + 1;
    TileXRStragglerReport report {};
    CheckEq("mismatched calibration fails", TileXRDiagnostics::Host::ClassifyStragglerRecords(
        Config(), clock, records, report), TileXR::TILEXR_ERROR_INTERNAL);
    CheckTrue("clock mismatch reason",
        (report.ranks[1].reasonFlags & TILEXR_STRAGGLER_REASON_CLOCK_UNSYNCHRONIZED) != 0);
}

} // namespace

int main()
{
    TestLayout();
    TestClockEstimatorSelectsMinimumDelay();
    TestClockEstimatorRejectsInvalidSamples();
    TestClockEstimatorRejectsUnwrittenSamples();
    TestClockEstimatorRejectsPartialCompletion();
    TestClockEstimatorRequiresSeventyFivePercentValid();
    TestClassifiesSlowAndFastRanks();
    TestClassifiesPreCommunicationStall();
    TestAbsoluteFloorAndLateCompletion();
    TestLateStartDoesNotLookFast();
    TestEarlyStartWaitDoesNotLookSlow();
    TestCompletionMadUsesConsistencyFactor();
    TestObservationErrorReason();
    TestRejectsMismatchedLaunch();
    TestRejectsMismatchedCalibration();
    return g_failures == 0 ? 0 : 1;
}
