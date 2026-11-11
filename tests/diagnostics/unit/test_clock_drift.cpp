/*
 * 功能：用合成采样检查 drift 拟合、反馈更新、cycle 校正及 sleep_at deadline 的取整和范围边界。
 * 调用关系：直接调用 clock_drift.cpp 的接口；不启动 NPU kernel。
 */
#include <cmath>
#include <iostream>
#include <limits>
#include "clock_drift.h"
#include "tilexr_types.h"

using TileXRDiagnostics::Host::ClockDriftPoint;
using TileXRDiagnostics::Host::FitClockDrift;
namespace {
int failures = 0;
void Check(bool ok, const char *name) { if (!ok) { ++failures; std::cerr << name << '\n'; } }
void TestLinear(double rate)
{
    const uint64_t epoch = 1000000000000000ull;
    const long double initial = 200000000000.0L;
    std::vector<ClockDriftPoint> points;
    for (uint64_t i = 0; i <= 20; ++i) {
        points.push_back({epoch+i*1000000000ull,initial+rate*i*1000000000.0L});
    }
    auto model = FitClockDrift(points,1000000000ull,1);
    Check(model.state == TILEXR_CLOCK_DRIFT_READY,"linear fit ready");
    Check(std::fabs(model.rateCorrection-rate) < 1e-13,"rate sign and scale");
    for (uint64_t age : {0u,21u,40u,80u}) {
        const uint64_t local = epoch+age*1000000000ull;
        int64_t aligned = 0;
        Check(TileXRClockCorrectCycle(&model,local,&aligned) == TileXR::TILEXR_SUCCESS,"apply affine");
        const long double expected = static_cast<long double>(local)+initial+rate*age*1000000000.0L;
        Check(std::fabs(aligned-expected) <= 0.51L,"held-out beyond training or before anchor");
    }
    auto noisy = points;
    for (size_t i = 0; i < noisy.size(); ++i) { noisy[i].offsetCycles += i%2 ? 40 : -40; }
    model = FitClockDrift(noisy,1000000000ull,1);
    Check(model.state == TILEXR_CLOCK_DRIFT_READY,"mean noisy observations fit");
    Check(std::fabs(model.rateCorrection-rate) < 1e-9,"noise averaged without reversing rate");
}
void TestShortAndInvalid()
{
    const uint64_t epoch = 1000000000000000ull;
    auto model = FitClockDrift({{epoch,100},{epoch+20000,100.006L}},1000000000ull,1);
    Check(model.state == TILEXR_CLOCK_DRIFT_INSUFFICIENT,"20us two points cannot establish stable drift");
    int64_t aligned = 123;
    Check(TileXRClockCorrectCycle(&model,epoch,&aligned) != TileXR::TILEXR_SUCCESS && aligned == 123,
        "rejected model cannot modify timestamp");
    model = FitClockDrift({{epoch,100},{epoch,100},{epoch,100},{epoch,100}},1000000000ull,1);
    Check(model.state == TILEXR_CLOCK_DRIFT_INSUFFICIENT,"zero span rejected");
    model = FitClockDrift({{epoch,100},{epoch+1,101},{epoch+2,102},{epoch+3,103}},1000000000ull,1);
    Check(model.state == TILEXR_CLOCK_DRIFT_UNSTABLE,"implausible noisy slope rejected");
    model = FitClockDrift({{epoch,100},{epoch+1,100},{epoch+2,100},{epoch+3,100}},0,1);
    Check(model.state != TILEXR_CLOCK_DRIFT_READY,"unknown frequency rejected");
    model = FitClockDrift({{epoch,100},{epoch+1,100},{epoch+2,100},{epoch+3,100}},1000000000ull,1);
    Check(model.state == TILEXR_CLOCK_DRIFT_READY,"zero drift is valid");
    Check(TileXRClockCorrectCycle(&model,std::numeric_limits<uint64_t>::max(),&aligned) !=
        TileXR::TILEXR_SUCCESS,"aligned cycle overflow rejected");
    model.rateCorrection = std::numeric_limits<double>::quiet_NaN();
    Check(TileXRClockCorrectCycle(&model,epoch,&aligned) != TileXR::TILEXR_SUCCESS,"nonfinite rejected");
}
void TestTrackerFreezes()
{
    TileXRDiagnostics::Host::ClockDriftTracker tracker;
    tracker.windowUs = 20;
    std::vector<TileXR::TileXRClockSyncDeviceSample> samples(4);
    for (size_t i = 0; i < samples.size(); ++i) {
        auto &s = samples[i];
        s.t0 = 1000000+i*22000; s.t1 = s.t0+7100; s.t2 = s.t1+100; s.t3 = s.t0+14100;
        s.sequence = static_cast<uint32_t>(i);
    }
    TileXRDiagnostics::Host::UpdateClockDrift(tracker,samples,1000000000ull,1,1);
    Check(tracker.closed && tracker.model.sampleCount == 2,"closes at first crossing, no silent long window");
    Check(tracker.model.state == TILEXR_CLOCK_DRIFT_INSUFFICIENT,"short window reported insufficient");
    TileXRDiagnostics::Host::UpdateClockDrift(tracker,samples,1000000000ull,1,2);
    Check(tracker.points.size() == 2 && tracker.model.trainedAtGeneration == 1,"frozen model stays frozen");
}

TileXRDiagnostics::Host::ClockDriftTracker TrackingModel()
{
    TileXRDiagnostics::Host::ClockDriftTracker tracker;
    tracker.windowUs = 10000000;
    tracker.tracking = true;
    tracker.closed = true;
    const uint64_t epoch = 1000000000000000ull;
    tracker.model = FitClockDrift({{epoch-10000000000ull,200000},
        {epoch-5000000000ull,200000},{epoch-1000000000ull,200000},
        {epoch,200000}},1000000000ull,1);
    tracker.model.trainedAtGeneration = 1;
    return tracker;
}

std::vector<TileXR::TileXRClockSyncDeviceSample> Burst(uint64_t ageSeconds,double rate)
{
    const uint64_t epoch = 1000000000000000ull;
    std::vector<TileXR::TileXRClockSyncDeviceSample> samples(64);
    for (size_t i = 0; i < samples.size(); ++i) {
        auto &s = samples[i];
        s.t0 = epoch+ageSeconds*1000000000ull+i*2000;
        const int64_t offset = std::llround(200000+rate*(ageSeconds*1000000000.0L+i*2000+500));
        s.t1 = s.t0+offset+100;
        s.t2 = s.t1+800;
        s.t3 = s.t0+1000;
        s.sequence = static_cast<uint32_t>(i);
    }
    return samples;
}

void TestTrackingFeedback()
{
    for (double rate : {-20e-9,20e-9}) {
        auto tracker = TrackingModel();
        const auto samples = Burst(60,rate);
        TileXRDiagnostics::Host::UpdateClockDrift(tracker,samples,1000000000ull,1,2);
        Check(tracker.updated && tracker.probeValid,"confirmed error triggers update");
        Check(std::fabs(tracker.model.rateCorrection-rate*0.25) < 1e-12,
            "feedback rate direction, units and gain");
        Check(tracker.model.trainedAtGeneration == 2,"accepted update advances model generation");
        int64_t aligned = 0;
        const auto &heldout = samples[48];
        Check(TileXRClockCorrectCycle(&tracker.model,heldout.t0,&aligned) == TileXR::TILEXR_SUCCESS,
            "updated model usable");
        Check(std::fabs(static_cast<long double>(aligned)-heldout.t1+100) <= 2,
            "phase reanchor removes current error on held-out samples");
    }
    auto tracker = TrackingModel();
    TileXRDiagnostics::Host::UpdateClockDrift(tracker,Burst(60,10e-9),1000000000ull,1,2);
    Check(!tracker.updated && tracker.probeValid && tracker.model.trainedAtGeneration == 1,
        "subthreshold probe retains model and anchor");
    TileXRDiagnostics::Host::UpdateClockDrift(tracker,Burst(120,10e-9),1000000000ull,1,3);
    Check(tracker.updated && std::fabs(tracker.model.rateCorrection-2.5e-9) < 1e-12,
        "elapsed time starts at last accepted update, not last probe");
    auto mixed = Burst(60,20e-9);
    for (size_t i = 16; i < 32; ++i) { mixed[i].t1 -= 2400; mixed[i].t2 -= 2400; }
    tracker = TrackingModel();
    TileXRDiagnostics::Host::UpdateClockDrift(tracker,mixed,1000000000ull,1,2);
    Check(!tracker.updated,"opposite residual signs cannot confirm update");
    mixed = Burst(60,20e-9); mixed[17].status = 1;
    tracker = TrackingModel();
    TileXRDiagnostics::Host::UpdateClockDrift(tracker,mixed,1000000000ull,1,2);
    Check(!tracker.updated && !tracker.probeValid,"invalid probe is not reported as accurate");
    tracker = TrackingModel();tracker.tracking = false;
    TileXRDiagnostics::Host::UpdateClockDrift(tracker,Burst(60,20e-9),1000000000ull,1,2);
    Check(!tracker.updated && tracker.model.trainedAtGeneration == 1,"frozen mode stays frozen");
}

void TestLogicalRankZeroReference()
{
    TileXRDiagnostics::Host::ClockDriftTracker tracker;
    tracker.windowUs = 10000000;
    tracker.tracking = true;
    for (uint32_t generation = 1; generation <= 2; ++generation) {
        TileXRDiagnostics::Host::UpdateClockDrift(tracker, Burst((generation - 1) * 10, 20e-9),
            1000000000ull, 0, generation);
    }
    Check(tracker.model.state == TILEXR_CLOCK_DRIFT_READY && tracker.model.rank == 0,
        "logical rank zero establishes reference model");
    Check(tracker.model.offsetCycles == 0 && tracker.model.rateCorrection == 0,
        "reference offset and rate stay zero");
    TileXRDiagnostics::Host::UpdateClockDrift(tracker, Burst(70, 20e-9), 1000000000ull, 0, 3);
    Check(!tracker.updated && tracker.model.trainedAtGeneration == 2,
        "maintenance does not move reference clock");
    const uint64_t local = tracker.model.anchorCycle + 60000000000ull;
    int64_t aligned = 0;
    Check(TileXRClockCorrectCycle(&tracker.model, local, &aligned) == TileXR::TILEXR_SUCCESS &&
        aligned == static_cast<int64_t>(local), "reference correction is identity");
}
void TestSleepDeadline()
{
    using TileXRDiagnostics::Host::ClockSleepDeadline;
    TileXRClockDriftModel m {};
    m.version = 1; m.state = TILEXR_CLOCK_DRIFT_READY;
    m.cyclesPerSecond = 1000000000; m.anchorCycle = 1000000000000000ull;
    for (double offset : {-1000.25, 0.0, 1000.25}) {
        for (double rate : {-230e-9, 0.0, 300e-9}) {
            m.offsetCycles = offset; m.rateCorrection = rate;
            for (int64_t age : {-1000000000ll, 0ll, 80000000000ll}) {
                const int64_t target = static_cast<int64_t>(m.anchorCycle) + age;
                uint64_t deadline = 123;
                Check(ClockSleepDeadline(m,target,&deadline) == 0,"inverse READY model");
                // Evaluate relative to the target, without losing fractions by
                // adding the absolute epoch; the selected cycle must not be early.
                const auto residual = [&](uint64_t local) {
                    const long double delta = local >= m.anchorCycle ?
                        static_cast<long double>(local-m.anchorCycle) :
                        -static_cast<long double>(m.anchorCycle-local);
                    return delta-age+m.offsetCycles+static_cast<long double>(rate)*delta;
                };
                Check(residual(deadline) >= -1e-8L,"ceil never early");
                Check(residual(deadline-1) < 1e-8L,"earliest integer local deadline");
            }
        }
    }
    m.rateCorrection = 0; m.offsetCycles = 0.25;
    m.anchorCycle = (uint64_t {1} << 63) - 100;
    uint64_t deadline = 123;
    Check(ClockSleepDeadline(m,static_cast<int64_t>(m.anchorCycle)+3,&deadline) == 0 &&
        deadline == m.anchorCycle+3,"large epoch preserves fractional rounding");
    m.offsetCycles = -0.25;
    Check(ClockSleepDeadline(m,static_cast<int64_t>(m.anchorCycle)+3,&deadline) == 0 &&
        deadline == m.anchorCycle+4,"negative fractional offset rounds up near int64 limit");
    m.anchorCycle = 0;
    Check(ClockSleepDeadline(m,std::numeric_limits<int64_t>::max(),&deadline) == 0 &&
        deadline == (uint64_t {1} << 63),"far reference preserves subcycle offset");
    m.offsetCycles = -std::ldexp(1.0,63);
    Check(ClockSleepDeadline(m,std::numeric_limits<int64_t>::max(),&deadline) == 0 &&
        deadline == std::numeric_limits<uint64_t>::max(),"largest local deadline accepted");
    Check(ClockSleepDeadline(m,std::numeric_limits<int64_t>::min(),&deadline) == 0 &&
        deadline == 0,"minimum signed reference without negation overflow");
    m.anchorCycle = 100; m.offsetCycles = -101;
    Check(ClockSleepDeadline(m,-1,&deadline) == 0 && deadline == 100,"signed reference domain");
    m.offsetCycles = 0;
    Check(ClockSleepDeadline(m,0,&deadline) == 0 && deadline == 0,"zero deadline valid");
    auto failsWithoutWrite = [&](const char *name) {
        deadline = 123;
        Check(ClockSleepDeadline(m,-1,&deadline) != 0 && deadline == 123,name);
    };
    failsWithoutWrite("negative local deadline rejected");
    m.offsetCycles = -std::ldexp(1.0,65);
    failsWithoutWrite("overflow rejected before integer conversion");
    m.offsetCycles = std::numeric_limits<double>::infinity();
    failsWithoutWrite("infinite offset rejected");
    m.offsetCycles = 0; m.rateCorrection = std::numeric_limits<double>::quiet_NaN();
    failsWithoutWrite("NaN rate rejected");
    m.rateCorrection = 0.001;
    failsWithoutWrite("rate sanity boundary rejected");
    m.rateCorrection = 0; m.cyclesPerSecond = 0;
    failsWithoutWrite("unknown frequency rejected");
    m.cyclesPerSecond = 1000000000; m.version = 2;
    failsWithoutWrite("unknown version rejected");
    m.version = 1;
    for (uint32_t state : {TILEXR_CLOCK_DRIFT_DISABLED,TILEXR_CLOCK_DRIFT_COLLECTING,
            TILEXR_CLOCK_DRIFT_INSUFFICIENT,TILEXR_CLOCK_DRIFT_UNSTABLE}) {
        m.state = state;
        Check(ClockSleepDeadline(m,100,&deadline) == TileXR::TILEXR_ERROR_NOT_INITIALIZED,
            "all unready states rejected");
    }
    m.state = TILEXR_CLOCK_DRIFT_READY;
    Check(ClockSleepDeadline(m,100,nullptr) != 0,"null output rejected");
}
} // namespace
int main()
{
    TestLinear(300e-9); TestLinear(-230e-9); TestLinear(0);
    TestShortAndInvalid(); TestTrackerFreezes();
    TestTrackingFeedback();
    TestLogicalRankZeroReference();
    TestSleepDeadline();
    std::cout << "clock drift failures=" << failures << '\n';
    return failures == 0 ? 0 : 1;
}
