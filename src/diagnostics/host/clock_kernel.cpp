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

int8_t *ClockSyncFunSig()
{
    const uint64_t sig =
        (static_cast<uint64_t>(TileXR::TileXRType::CLOCK_SYNC) <<
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
    ret = rtFunctionRegister(handle, ClockSyncFunSig(),
        "TileXRClockSync", "TileXRClockSync", 0);
    if (ret != RT_ERROR_NONE) {
        std::cerr << "TileXR diagnostics rtFunctionRegister failed, ret=" << ret
                  << ", kernel=TileXRClockSync" << std::endl;
        g_registrationStatus = TileXR::TILEXR_ERROR_MKIRT;
        return g_registrationStatus;
    }
    g_binaryHandle = handle;
    g_registered = true;
    g_registrationStatus = TileXR::TILEXR_SUCCESS;
    return TileXR::TILEXR_SUCCESS;
}

} // namespace

int LaunchClockSyncKernel(const StragglerSession &session, uint32_t warmupSamples,
                          uint32_t samples, int64_t magic, uint64_t timeoutCycles,
                          aclrtStream stream)
{
    if (session.devArgs == nullptr || session.deviceSamples == nullptr || samples == 0 || stream == nullptr) {
        return TileXR::TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    const int registerRet = EnsureDiagnosticsKernelRegistered();
    if (registerRet != TileXR::TILEXR_SUCCESS) {
        return registerRet;
    }
    ClockSyncKernelArgs args {};
    args.commArgsPtr = session.devArgs;
    args.samples = session.deviceSamples;
    args.sampleCount = samples;
    args.magic = static_cast<uint64_t>(magic);
    args.timeoutCycles = timeoutCycles;
    args.warmupSampleCount = warmupSamples;
    rtTaskCfgInfo_t cfgInfo {};
    cfgInfo.schemMode = 1;
    rtArgsEx_t argsInfo {};
    argsInfo.args = &args;
    argsInfo.argsSize = sizeof(args);
    const rtError_t ret = rtKernelLaunchWithFlagV2(
        ClockSyncFunSig(), 1, &argsInfo, nullptr,
        static_cast<rtStream_t>(stream), 0, &cfgInfo);
    return ret == RT_ERROR_NONE ? TileXR::TILEXR_SUCCESS : TileXR::TILEXR_ERROR_MKIRT;
}

} // namespace Host
} // namespace TileXRDiagnostics
