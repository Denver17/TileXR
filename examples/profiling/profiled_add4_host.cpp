/* Copyright (c) 2026 TileXR Project. CANN Open Software License Agreement v2.0. */
// 最小 Add4 Host，直接调用 AclCapture 并加载外置 kernel；设备与 stream 生命周期由本示例持有。
// Minimal Add4 Host using AclCapture and an external kernel; this example owns device and stream lifetime.

#include <tilexr/profiling.h>
#include <runtime/kernel.h>
#include <limits.h>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

namespace {
constexpr uint32_t kBlockDim = 4U;
constexpr uint32_t kTotalElements = kBlockDim * 2048U;
constexpr const char *kKernelName = "tilexr_profiled_add4_kernel";
uint8_t kernelSignature;

// Keep the external kernel relocatable with the executable.
std::vector<uint8_t> LoadKernel()
{
    char executable[PATH_MAX] {};
    realpath("/proc/self/exe", executable);
    const std::string path(executable);
    std::ifstream input;
    input.exceptions(std::ios::failbit | std::ios::badbit);
    input.open(path.substr(0, path.find_last_of('/') + 1) + TILEXR_ADD4_KERNEL_RELATIVE_PATH,
        std::ios::binary);
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(input), {});
}
} // namespace

int main(int, char **argv)
{
    const char *deviceText = std::getenv("TILEXR_ADD4_DEVICE_ID");
    const int device = deviceText ? std::atoi(deviceText) : 0;
    const size_t bytes = kTotalElements * sizeof(float);
    aclInit(nullptr);
    aclrtSetDevice(device);
    std::cout << "actual SoC=" << aclrtGetSocName() << ", device=" << device << '\n';
    aclrtStream stream = nullptr;
    aclrtCreateStream(&stream);

    const auto binary = LoadKernel();
    rtDevBinary_t devBinary {};
    devBinary.magic = RT_DEV_BINARY_MAGIC_ELF_AIVEC;
    devBinary.data = binary.data();
    devBinary.length = binary.size();
    void *registration = nullptr;
    rtDevBinaryRegister(&devBinary, &registration);
    rtFunctionRegister(registration, &kernelSignature, kKernelName, kKernelName, 0);

    void *input[4] {};
    for (uint32_t j = 0; j < 4; ++j) {
        const std::vector<float> hostInput(kTotalElements, float(j + 1));
        aclrtMalloc(&input[j], bytes, ACL_MEM_MALLOC_HUGE_FIRST);
        aclrtMemcpy(input[j], bytes, hostInput.data(), bytes, ACL_MEMCPY_HOST_TO_DEVICE);
    }
    void *output = nullptr;
    aclrtMalloc(&output, bytes, ACL_MEM_MALLOC_HUGE_FIRST);
    {
        TileXR::Profiling::AclCapture capture;
        TileXR::Profiling::CaptureConfig config;
        config.logicalStreamCount = kBlockDim;
        config.enabled = TILEXR_ENABLE_PROFILING == 1;
        capture.Initialize(config, stream);

        struct KernelArgs { void *input0, *input1, *input2, *input3, *output, *profile; } args {
            input[0], input[1], input[2], input[3], output, capture.DeviceArgs()};
        rtArgsEx_t argsInfo {};
        argsInfo.args = &args;
        argsInfo.argsSize = sizeof(args);
        rtTaskCfgInfo_t taskConfig {};
        taskConfig.schemMode = 1;
        rtKernelLaunchWithFlagV2(&kernelSignature, kBlockDim, &argsInfo, nullptr,
            static_cast<rtStream_t>(stream), 0U, &taskConfig);
        aclrtSynchronizeStream(stream);
        capture.Save(argv[1]);
        capture.Close();
    }
    for (auto *allocation : input) aclrtFree(allocation);
    aclrtFree(output);
    rtDevBinaryUnRegister(registration);
    aclrtDestroyStream(stream);
    aclrtResetDevice(device);
    aclFinalize();
    return 0;
}
