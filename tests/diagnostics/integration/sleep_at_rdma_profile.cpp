/*
 * 功能：在同一 stream 连续提交 sleep_at 和 64-WQE kernel，校验数据并保存 SDK records 与模型快照。
 * 调用关系：clock E2E 在校准后调用；本文件注册外置 kernel，调用 RDMA 和 profiling Host API。
 * 资源与生命周期：借用 communicator、detector 和 stream，持有 MR backing、辅助 buffer、capture 和 binary 注册。
 *   正常路径先等待两端完成再注销和释放；异常路径直接退出进程，不继续显式释放 RDMA buffer。
 */
#include "sleep_at_rdma_profile.h"
#include "straggler_session.h"
#include "tilexr_rdma_write_options.h"
#include "tilexr/profiling.h"
#include <runtime/kernel.h>
#include <array>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <limits.h>
#include <stdexcept>
#include <vector>

namespace {
using namespace SleepAtRdmaProfile;
uint8_t signature;

void Require(bool ok, const char *operation)
{
    if (!ok) { throw std::runtime_error(operation); }
}
void Check(int status, const char *operation) { Require(status == 0, operation); }
void Check(const TileXR::Profiling::Status &status, const char *operation)
{
    if (!status) { throw std::runtime_error(std::string(operation) + ": " + status.message); }
}
template<class T> std::array<T, 2> Gather(TileXRCommPtr comm, const T &local)
{
    std::array<T, 2> peers {};
    Check(TileXRCommHostAllGatherTimeout(comm, &local, sizeof(local), peers.data(), 5000), "profile rank agreement");
    return peers;
}

uint8_t Pattern(int rank, uint64_t byte)
{
    return static_cast<uint8_t>((rank + 1)*37 + byte*13 + byte/kBytes);
}

void Run(TileXRCommPtr comm, TileXRStraggler detector, aclrtStream stream,
         int rank, int device, const std::string &directory)
{
    using namespace TileXRDiagnostics::Host;
    auto &session = *static_cast<StragglerSession *>(detector);
    Require(session.clockUsesRdma && session.hostArgs->rankSize == 2, "profile requires two RDMA clock ranks");
    TileXRClockDriftModel model {};
    Check(TileXRClockGetDriftModel(detector, rank, &model), "snapshot drift model");
    Require(model.state == TILEXR_CLOCK_DRIFT_READY, "profile requires READY drift model");

    // Load/register and allocate capture before selecting the future target.
    char executable[PATH_MAX] {};
    Require(realpath("/proc/self/exe", executable) != nullptr, "resolve executable");
    const std::string path(executable);
    std::ifstream input(path.substr(0, path.find_last_of('/') + 1) +
        TILEXR_SLEEP_RDMA_KERNEL_RELATIVE_PATH, std::ios::binary);
    Require(bool(input), "open external RDMA profile kernel");
    const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(input)), {});
    Require(bytes.size() > 64 && bytes.size() < 64*1024*1024, "kernel binary extent");
    rtDevBinary_t binary {};
    binary.magic = 0x41415246;
    binary.data = bytes.data();
    binary.length = static_cast<uint32_t>(bytes.size());
    void *registration = nullptr;
    Check(rtDevBinaryRegister(&binary, &registration), "register RDMA profile binary");
    const char *name = "tilexr_sleep_at_rdma_profile";
    Check(rtFunctionRegister(registration, &signature, name, name, 0), "register RDMA profile entry");

    void *memory = nullptr, *viewDevice = nullptr, *resultDevice = nullptr;
    Check(aclrtMalloc(&memory, kRegionBytes, ACL_MEM_MALLOC_HUGE_ONLY), "allocate profile payload MR");
    Check(aclrtMalloc(&viewDevice, sizeof(TileXRRDMADeviceView), ACL_MEM_MALLOC_NORMAL_ONLY), "allocate profile view");
    Check(aclrtMalloc(&resultDevice, 64, ACL_MEM_MALLOC_NORMAL_ONLY), "allocate profile result");
    std::vector<uint8_t> expected(kRegionBytes, 0xa5);
    for (uint64_t i = 0; i < kPayload; ++i) { expected[kSource+i] = Pattern(rank, i); }
    Check(aclrtMemcpy(memory, kRegionBytes, expected.data(), expected.size(), ACL_MEMCPY_HOST_TO_DEVICE), "initialize source and guards");
    TileXRRDMAMemHandle handle = 0;
    Check(TileXRRDMARegister(comm, memory, kRegionBytes, &handle), "register profile MR");
    TileXRRDMADeviceView view {};
    Check(TileXRRDMAGetDeviceView(comm, handle, 1-rank, &view), "get profile device view");
    Check(TileXR::RdmaWire::ValidatePipelineView(view), "validate pipeline view");
    Require(view.localAddress == reinterpret_cast<uintptr_t>(memory) && view.localBytes == kRegionBytes &&
        view.remoteBytes == kRegionBytes && view.queue.pendingCapacity >= kWqes &&
        TileXR::RdmaWire::PipelineWindow(view.queue, 1) >= kWqes, "profile MR/queue capacity");
    Check(aclrtMemcpy(viewDevice, sizeof(view), &view, sizeof(view), ACL_MEMCPY_HOST_TO_DEVICE), "copy profile view");

    TileXR::Profiling::CaptureConfig config;
    config.rank = rank;
    config.logicalStreamCount = 1;
    config.recordsPerStream = 8;
    if (rank == 0) { Check(TileXR::Profiling::Detail::NewCaptureId(&config.captureId), "create capture identity"); }
    config.captureId = Gather(comm, config.captureId)[0];
    TileXR::Profiling::AclCapture capture;
    Check(capture.Initialize(config, stream), "initialize capture");

    // Refresh the current root cycle with the existing sampler. This is the
    // final legacy RDMA use: the following WRITE permanently enters pipeline mode.
    int64_t magic = 0;
    Check(TileXRCommNextMagic(comm, &magic), "probe magic");
    Check(LaunchClockSyncKernel(session, 0, 4, magic, model.cyclesPerSecond*2, stream), "probe current cycle");
    Check(aclrtSynchronizeStream(stream), "complete current-cycle probe");
    TileXR::TileXRClockSyncDeviceSample timing {};
    const auto *timingAddress = static_cast<const uint8_t *>(session.deviceSamples) + 4*sizeof(timing);
    Check(aclrtMemcpy(&timing, sizeof(timing), timingAddress, sizeof(timing), ACL_MEMCPY_DEVICE_TO_HOST), "read current cycle");
    Require(timing.status == TileXR::TILEXR_CLOCK_SYNC_SAMPLE_SUCCESS && timing.t3 != 0, "current-cycle sample");
    const uint64_t rootCycle = Gather(comm, timing.t3)[0];
    Require(rootCycle <= uint64_t(INT64_MAX) - model.cyclesPerSecond/5, "reference target range");
    const int64_t target = static_cast<int64_t>(rootCycle + model.cyclesPerSecond/5);
    uint64_t deadline = 0;
    Check(ClockSleepDeadline(model, target, &deadline), "convert sleep target");
    Require(Gather(comm, target)[0] == target, "shared reference target");

    struct KernelArgs { void *view, *result, *profile; } args {viewDevice, resultDevice, capture.DeviceArgs()};
    rtArgsEx_t arguments {};
    arguments.args = &args;
    arguments.argsSize = sizeof(args);
    rtTaskCfgInfo_t task {};
    task.schemMode = 1;
    Check(TileXRClockSleepAt(detector, target, stream), "enqueue sleep_at");
    // No Host barrier/synchronization is permitted between these two launches.
    Check(rtKernelLaunchWithFlagV2(&signature, 1, &arguments, nullptr,
        static_cast<rtStream_t>(stream), 0, &task), "enqueue following RDMA kernel");
    Check(aclrtSynchronizeStream(stream), "complete following RDMA kernel");
    const uint32_t localComplete = 1;
    Require(Gather(comm, localComplete) == std::array<uint32_t, 2>{{1, 1}}, "both senders completed");

    std::array<uint64_t, 8> result {};
    Check(aclrtMemcpy(result.data(), 64, resultDevice, 64, ACL_MEMCPY_DEVICE_TO_HOST), "read RDMA result");
    std::cout << "RDMA_PROFILE_QUEUE rank=" << rank << " status=" << result[1]
        << " producer_before=" << result[2] << " producer_after=" << result[3]
        << " completed=" << result[4] << " cq_consumer=" << result[5]
        << " error=" << result[6] << " syndrome=" << result[7] << std::endl;
    Require(result[0] == kMagic && result[1] == 0 && result[3] == result[2] + kWqes &&
        result[4] == result[3] && result[6] == 0 && result[7] == 0, "64-WQE completion counters");
    std::vector<uint8_t> observed(kRegionBytes);
    Check(aclrtMemcpy(observed.data(), observed.size(), memory, kRegionBytes, ACL_MEMCPY_DEVICE_TO_HOST), "read source, destination and guards");
    for (uint64_t i = 0; i < kPayload; ++i) { expected[kTarget+i] = Pattern(1-rank, i); }
    Require(observed == expected, "profile source/destination/guard verification");
    const std::string stem = directory + "/sleep_at_rdma_rank" + std::to_string(rank);
    Check(capture.Save(stem + ".records.bin"), "save SDK profile records");
    const auto *snapshot = capture.Snapshot();
    Require(snapshot != nullptr && snapshot->streams.size() == 1, "single profiling lane");
    const auto &lane = snapshot->streams[0];
    Require(lane.complete && lane.dropped == 0 && lane.records.size() == 2 &&
        lane.records[0].eventId == kEntry && lane.records[1].eventId == kExit, "exactly entry/exit profile markers");
    const uint64_t begin = lane.records[0].timestamp, end = lane.records[1].timestamp;
    Require(end > begin && begin >= deadline, "kernel entry follows local deadline");
    int64_t correctedBegin = 0, correctedEnd = 0;
    Check(TileXRClockCorrectCycle(&model, begin, &correctedBegin), "correct entry cycle");
    Check(TileXRClockCorrectCycle(&model, end, &correctedEnd), "correct exit cycle");
    Require(correctedBegin >= target && correctedEnd > correctedBegin, "kernel reference-domain interval");
    std::ofstream sidecar(stem + ".json");
    sidecar << std::setprecision(17)
        << "{\"schema\":\"tilexr_sleep_at_rdma_profile.v1\",\"rank\":" << rank
        << ",\"device\":" << device << ",\"capture_id\":\"" << TileXR::Profiling::CaptureIdString(snapshot->captureId)
        << "\",\"wqes\":" << kWqes << ",\"bytes_per_wqe\":" << kBytes << ",\"qpn\":" << view.queue.qpn
        << ",\"target\":" << target << ",\"local_deadline\":" << deadline
        << ",\"raw_entry\":" << begin << ",\"raw_exit\":" << end
        << ",\"corrected_entry\":" << correctedBegin << ",\"corrected_exit\":" << correctedEnd
        << ",\"cycles_per_second\":" << model.cyclesPerSecond << ",\"model_generation\":" << model.trainedAtGeneration
        << ",\"anchor_cycle\":" << model.anchorCycle << ",\"offset_cycles\":" << model.offsetCycles
        << ",\"rate_correction\":" << model.rateCorrection << ",\"state\":" << model.state
        << ",\"producer_before\":" << result[2] << ",\"producer_after\":" << result[3]
        << ",\"completed\":" << result[4] << ",\"cq_consumer\":" << result[5]
        << ",\"clock_model_bound_ns\":" << session.clock.precisionNs
        << ",\"data_and_guards_verified\":true,\"markers\":2}\n";
    sidecar.close();
    Require(bool(sidecar), "save profile model sidecar");
    Check(capture.Close(), "close profile capture");
    Require(Gather(comm, localComplete) == std::array<uint32_t, 2>{{1, 1}}, "both profiles verified");
    Check(TileXRRDMAUnregister(comm, handle), "unregister profile MR");
    Check(aclrtFree(memory), "free profile MR");
    Check(aclrtFree(viewDevice), "free profile view");
    Check(aclrtFree(resultDevice), "free profile result");
    Check(rtDevBinaryUnRegister(registration), "unregister profile binary");
    std::cout << "[rank " << rank << "] PASS sleep_at RDMA profile wqes=" << kWqes
        << " raw_entry=" << begin << " corrected_entry=" << correctedBegin << " target=" << target << std::endl;
}
}

bool RunSleepAtRdmaProfile(TileXRCommPtr comm, TileXRStraggler detector,
    aclrtStream stream, int rank, int device, const std::string &outputDir)
{
    try {
        Run(comm, detector, stream, rank, device, outputDir);
        return true;
    } catch (const std::exception &error) {
        std::cerr << "sleep_at RDMA profile failed: " << error.what()
            << "; retaining RDMA allocations until process exit" << std::endl;
        std::_Exit(1);
    }
}
