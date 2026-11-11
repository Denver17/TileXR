/*
 * 功能：加载并注册 clock sync 和 sleep_at 入口，向调用方的 stream 提交 kernel。
 * 调用关系：session 调用 launch 接口；本文件使用 kernel binary loader 和 CANN runtime。
 * 资源与生命周期：成功注册的 binary handle 保存在静态状态中；采样 buffer 和 stream 由调用方持有。
 */
#include "clock_kernel.h"

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <mutex>

#include "runtime/kernel.h"
#include "straggler_session.h"
#include "tilexr_types.h"

#include "tilexr_kernel_binary_loader.h"

namespace TileXRDiagnostics {
namespace Host {
namespace {

constexpr uint32_t kRtDevBinaryMagicElfAivec = 0x41415246;
constexpr uint64_t FUNSIG_OFFSET_BITS = 16;
constexpr uint64_t FUNSIG_SKEW = 0x1000;

std::mutex g_registrationMutex;
bool g_registered = false;
int g_registrationStatus = TileXR::TILEXR_ERROR_NOT_INITIALIZED;
void *g_binaryHandle = nullptr;

int8_t *ClockFunSig(TileXR::TileXRType type)
{
    const uint64_t sig =
        (static_cast<uint64_t>(type) <<
            FUNSIG_OFFSET_BITS << FUNSIG_OFFSET_BITS) + FUNSIG_SKEW;
    return reinterpret_cast<int8_t *>(sig);
}

int EnsureDiagnosticsKernelRegistered()
{
    std::lock_guard<std::mutex> guard(g_registrationMutex);
    if (g_registered) {
        return TileXR::TILEXR_SUCCESS;
    }
    TileXR::KernelBinaryView kernelBinary {};
    if (!TileXR::LoadKernelBinary(
            reinterpret_cast<const void *>(&EnsureDiagnosticsKernelRegistered),
            "tilexr_diagnostics_op.o", &kernelBinary)) {
        return TileXR::TILEXR_ERROR_NOT_INITIALIZED;
    }

    rtDevBinary_t binary {};
    binary.data = kernelBinary.data;
    binary.length = static_cast<uint32_t>(kernelBinary.size);
    binary.magic = kRtDevBinaryMagicElfAivec;
    binary.version = 0;
    void *handle = nullptr;
    rtError_t ret = rtDevBinaryRegister(&binary, &handle);
    if (ret != RT_ERROR_NONE) {
        std::cerr << "TileXR diagnostics rtDevBinaryRegister failed, ret=" << ret
                  << ", binarySize=" << kernelBinary.size << std::endl;
        g_registrationStatus = TileXR::TILEXR_ERROR_MKIRT;
        return g_registrationStatus;
    }
    const TileXR::TileXRType types[] = {TileXR::TileXRType::CLOCK_SYNC,
                                        TileXR::TileXRType::CLOCK_SLEEP_AT};
    const char *names[] = {"TileXRClockSync", "TileXRClockSleepAt"};
    for (size_t index = 0; index < 2; ++index) {
        ret = rtFunctionRegister(handle, ClockFunSig(types[index]), names[index], names[index], 0);
        if (ret != RT_ERROR_NONE) {
            std::cerr << "TileXR diagnostics rtFunctionRegister failed, ret=" << ret
                      << ", kernel=" << names[index] << std::endl;
            g_registrationStatus = TileXR::TILEXR_ERROR_MKIRT;
            return g_registrationStatus;
        }
    }
    g_binaryHandle = handle;
    g_registered = true;
    g_registrationStatus = TileXR::TILEXR_SUCCESS;
    return TileXR::TILEXR_SUCCESS;
}

template <typename Args>
int LaunchClockKernel(TileXR::TileXRType type, Args &args, aclrtStream stream)
{
    const int status = EnsureDiagnosticsKernelRegistered();
    if (status != TileXR::TILEXR_SUCCESS) { return status; }
    rtTaskCfgInfo_t config {};
    config.schemMode = 1;
    rtArgsEx_t argsInfo {};
    argsInfo.args = &args;
    argsInfo.argsSize = sizeof(args);
    const rtError_t ret = rtKernelLaunchWithFlagV2(
        ClockFunSig(type), 1, &argsInfo, nullptr,
        static_cast<rtStream_t>(stream), 0, &config);
    return ret == RT_ERROR_NONE ? TileXR::TILEXR_SUCCESS : TileXR::TILEXR_ERROR_MKIRT;
}

} // namespace

int LaunchClockSleepAtKernel(uint64_t localDeadline, aclrtStream stream)
{
    if (stream == nullptr) { return TileXR::TILEXR_ERROR_PARA_CHECK_FAIL; }
    ClockSleepAtKernelArgs args {localDeadline};
    return LaunchClockKernel(TileXR::TileXRType::CLOCK_SLEEP_AT, args, stream);
}

int LaunchClockSyncKernel(const StragglerSession &session, uint32_t warmupSamples,
                          uint32_t samples, int64_t magic, uint64_t timeoutCycles,
                          aclrtStream stream)
{
    if (session.devArgs == nullptr || session.deviceSamples == nullptr || samples == 0 || stream == nullptr) {
        return TileXR::TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    ClockSyncKernelArgs args {};
    args.commArgsPtr = session.devArgs;
    args.samples = session.deviceSamples;
    args.sampleCount = samples;
    args.magic = static_cast<uint64_t>(magic);
    args.timeoutCycles = timeoutCycles;
    args.warmupSampleCount = warmupSamples;
    args.rdmaMemory = session.clockRdmaMemory;
    return LaunchClockKernel(TileXR::TileXRType::CLOCK_SYNC, args, stream);
}

} // namespace Host
} // namespace TileXRDiagnostics
