/* Copyright (c) 2026 TileXR Project. CANN Open Software License Agreement v2.0. */
#include <tilexr/profiling.h>
#include <runtime/kernel.h>
#include <unistd.h>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

namespace {
constexpr uint32_t kBlockDim = 4U;
constexpr uint32_t kTotalElements = kBlockDim * 2048U;
constexpr const char *kKernelName = "tilexr_profiled_add4_kernel";
uint8_t kernelSignature;
bool CheckAcl(const char *operation, aclError result)
{
    if (result == ACL_SUCCESS) return true;
    std::cerr << operation << " failed, ret=" << result << '\n'; return false;
}
bool CheckProfile(const char *operation, const TileXR::Profiling::Status &status)
{
    if (status) {
        if (!status.message.empty()) std::cerr << operation << ": " << status.message << '\n';
        return true;
    }
    std::cerr << operation << " failed: " << status.message << ", ACL=" << status.aclError << '\n';
    return false;
}
bool DeviceId(int *device)
{
    const char *text = std::getenv("TILEXR_ADD4_DEVICE_ID");
    if (!text) { *device = 0; return true; }
    char *end = nullptr;
    const long value = std::strtol(text, &end, 10);
    if (!*text || *end || value < 0 || value > std::numeric_limits<int>::max()) {
        std::cerr << "TILEXR_ADD4_DEVICE_ID must be a nonnegative device index\n";
        return false;
    }
    *device = static_cast<int>(value); return true;
}
// Executable-relative loading also works after relocating the installed prefix.
bool LoadKernel(std::vector<uint8_t> *bytes)
{
    std::vector<char> executable(1024);
    ssize_t length;
    for (;;) {
        length = readlink("/proc/self/exe", executable.data(), executable.size());
        if (length < 0) { std::cerr << "cannot resolve /proc/self/exe\n"; return false; }
        if (size_t(length) < executable.size()) break;
        if (executable.size() >= 1048576) return false;
        executable.resize(executable.size() * 2);
    }
    const std::string path(executable.data(), size_t(length));
    const std::string binaryPath = path.substr(0, path.find_last_of('/') + 1) + TILEXR_ADD4_KERNEL_RELATIVE_PATH;
    std::ifstream input(binaryPath, std::ios::binary | std::ios::ate);
    const std::streamoff size = input ? std::streamoff(input.tellg()) : std::streamoff(-1);
    if (size <= 0 || size > 67108864) {
        std::cerr << "cannot read bounded AICore binary: " << binaryPath << '\n'; return false;
    }
    bytes->resize(size_t(size)); input.seekg(0);
    if (!input.read(reinterpret_cast<char *>(bytes->data()), size)) return false;
    std::cout << "loaded external kernel=" << binaryPath << ", bytes=" << bytes->size() << '\n';
    return true;
}
bool RegisterKernel(const std::vector<uint8_t> &bytes, void **handle)
{
    rtDevBinary_t binary {};
    binary.magic = RT_DEV_BINARY_MAGIC_ELF_AIVEC;
    binary.data = bytes.data(); binary.length = uint64_t(bytes.size());
    rtError_t result = rtDevBinaryRegister(&binary, handle);
    if (result == RT_ERROR_NONE)
        result = rtFunctionRegister(*handle, &kernelSignature, kKernelName, kKernelName, 0);
    if (result == RT_ERROR_NONE) return true;
    std::cerr << "kernel registration failed, ret=" << result << '\n'; return false;
}
bool LaunchKernel(aclrtStream stream, void *const (&input)[4], void *output, void *profile)
{
    struct KernelArgs { void *input0, *input1, *input2, *input3, *output, *profile; } args {
        input[0], input[1], input[2], input[3], output, profile};
    rtArgsEx_t argsInfo {}; argsInfo.args = &args; argsInfo.argsSize = sizeof(args);
    rtTaskCfgInfo_t config {}; config.schemMode = 1;
    const rtError_t result = rtKernelLaunchWithFlagV2(&kernelSignature, kBlockDim,
        &argsInfo, nullptr, static_cast<rtStream_t>(stream), 0U, &config);
    if (result == RT_ERROR_NONE) return true;
    std::cerr << "rtKernelLaunchWithFlagV2 failed, ret=" << result << '\n'; return false;
}
bool Validate(const std::vector<float> (&input)[4], const std::vector<float> &output)
{
    for (uint32_t i = 0; i < kTotalElements; ++i) {
        const float expected = input[0][i] + input[1][i] + input[2][i] + input[3][i];
        if (!std::isfinite(output[i]) || std::fabs(output[i] - expected) > 1.0e-5f) {
            std::cerr << "Add4 mismatch index=" << i << ", got=" << output[i]
                << ", expected=" << expected << '\n'; return false;
        }
    }
    return true;
}
} // namespace

int main(int argc, char **argv)
{
    if (argc != 2) { std::cerr << "usage: tilexr_profiled_add4_demo NEW_RECORDS_PATH\n"; return 2; }
    int device;
    if (!DeviceId(&device)) return 2;
    std::vector<uint8_t> binary;
    if (!LoadKernel(&binary)) return 1;
    bool aclInitialized = false, deviceSet = false;
    aclrtStream stream = nullptr;
    void *registration = nullptr;
    void *input[4] {};
    void *output = nullptr;
    const size_t bytes = kTotalElements * sizeof(float);
    int exitCode = 1;
    {
        TileXR::Profiling::AclCapture capture;
        do {
            if (!CheckAcl("aclInit", aclInit(nullptr))) break;
            aclInitialized = true;
            if (!CheckAcl("aclrtSetDevice", aclrtSetDevice(device))) break;
            deviceSet = true;
            const char *soc = aclrtGetSocName();
            if (!soc || std::strncmp(soc, "Ascend950", 9) != 0) {
                std::cerr << "Add4 requires the explicitly supported Ascend950 SoC\n";
                break;
            }
            std::cout << "actual SoC=" << soc << ", device=" << device << '\n';
            if (!CheckAcl("aclrtCreateStream", aclrtCreateStream(&stream)) || !RegisterKernel(binary, &registration)) break;
            std::vector<float> hostInput[4];
            bool ready = true;
            for (uint32_t j = 0; j < 4 && ready; ++j) {
                hostInput[j].resize(kTotalElements);
                for (uint32_t i = 0; i < kTotalElements; ++i)
                    hostInput[j][i] = float((i % (17U + j * 2U)) + j * 3U);
                ready = CheckAcl("input allocation", aclrtMalloc(&input[j], bytes, ACL_MEM_MALLOC_HUGE_FIRST));
                if (ready) ready = CheckAcl("input H2D", aclrtMemcpy(input[j], bytes,
                    hostInput[j].data(), bytes, ACL_MEMCPY_HOST_TO_DEVICE));
            }
            if (!ready || !CheckAcl("output allocation", aclrtMalloc(&output, bytes, ACL_MEM_MALLOC_HUGE_FIRST))) break;
            // One ordinary capture per kernel; application owns all ACL objects.
            if (!CheckProfile("Initialize", capture.Initialize(stream, kBlockDim)) ||
                !LaunchKernel(stream, input, output, capture.DeviceArgs()) ||
                !CheckProfile("Save", capture.Save(argv[1]))) break;
            const auto *snapshot = capture.Snapshot();
            if (!snapshot || snapshot->streams.size() != kBlockDim) break;
            bool complete = true;
            for (const auto &lane : snapshot->streams)
                complete = complete && lane.complete && lane.dropped == 0 && lane.records.size() == 20;
            if (!complete) { std::cerr << "expected four committed 20-record lanes\n"; break; }
            std::vector<float> hostOutput(kTotalElements);
            if (!CheckAcl("output D2H", aclrtMemcpy(hostOutput.data(), bytes, output,
                bytes, ACL_MEMCPY_DEVICE_TO_HOST)) || !Validate(hostInput, hostOutput)) break;
            std::cout << "Add4 correctness passed; 4 committed lanes x 20 records saved\n";
            exitCode = 0;
        } while (false);
        if (!CheckProfile("Close", capture.Close())) exitCode = 1;
        // Capture destruction occurs before its borrowed stream/context expire.
    }
    if (stream && !CheckAcl("final stream synchronization", aclrtSynchronizeStream(stream))) {
        std::cerr << "retaining device buffers, registration and ACL resources after failed synchronization\n";
        return 1;
    }
    for (auto *allocation : input)
        if (allocation && !CheckAcl("input free", aclrtFree(allocation))) exitCode = 1;
    if (output && !CheckAcl("output free", aclrtFree(output))) exitCode = 1;
    if (registration) (void)rtDevBinaryUnRegister(registration);
    if (stream) (void)aclrtDestroyStream(stream);
    if (deviceSet) (void)aclrtResetDevice(device);
    if (aclInitialized) (void)aclFinalize();
    return exitCode;
}
