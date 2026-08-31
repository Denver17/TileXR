#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <vector>

#include "acl/acl.h"
#include "runtime/kernel.h"
#include "tilexr_api.h"
#include "tilexr_types.h"

extern "C" {
extern const unsigned char TileXRProfiledAdd4KernelBinaryData[];
extern const std::size_t TileXRProfiledAdd4KernelBinarySize;
}

namespace {

constexpr uintptr_t kKernelSignatureValue = UINT64_C(0x5458525000000004);
constexpr const char* kKernelName = "tilexr_profiled_add4_kernel";
constexpr uint32_t kBlockDim = 4U;
constexpr uint32_t kElementsPerCore = 2048U;
constexpr uint32_t kTotalElements = kBlockDim * kElementsPerCore;

struct KernelRegistration {
    void* binaryHandle = nullptr;
};

class DeviceBuffer {
public:
    DeviceBuffer() = default;
    ~DeviceBuffer()
    {
        if (data_ != nullptr) {
            (void)aclrtFree(data_);
        }
    }

    DeviceBuffer(const DeviceBuffer&) = delete;
    DeviceBuffer& operator=(const DeviceBuffer&) = delete;

    bool Allocate(std::size_t bytes)
    {
        bytes_ = bytes;
        return aclrtMalloc(&data_, bytes, ACL_MEM_MALLOC_HUGE_FIRST) == ACL_SUCCESS;
    }

    void* Data() const { return data_; }
    std::size_t Size() const { return bytes_; }

private:
    void* data_ = nullptr;
    std::size_t bytes_ = 0U;
};

bool CheckAcl(const char* operation, aclError result)
{
    if (result == ACL_SUCCESS) {
        return true;
    }
    std::cerr << operation << " failed, ret=" << result << std::endl;
    return false;
}

bool CheckTileXR(const char* operation, int result)
{
    if (result == TileXR::TILEXR_SUCCESS) {
        return true;
    }
    std::cerr << operation << " failed, ret=" << result << std::endl;
    return false;
}

int DeviceId()
{
    const char* value = std::getenv("TILEXR_ADD4_DEVICE_ID");
    return value == nullptr ? 0 : std::atoi(value);
}

const void* KernelSignature()
{
    return reinterpret_cast<const void*>(kKernelSignatureValue);
}

bool RegisterKernel(KernelRegistration& registration)
{
    rtDevBinary_t binary {};
    binary.magic = RT_DEV_BINARY_MAGIC_ELF_AIVEC;
    binary.version = 0;
    binary.data = TileXRProfiledAdd4KernelBinaryData;
    binary.length = static_cast<uint64_t>(TileXRProfiledAdd4KernelBinarySize);
    rtError_t result = rtDevBinaryRegister(&binary, &registration.binaryHandle);
    if (result != RT_ERROR_NONE) {
        std::cerr << "rtDevBinaryRegister failed, ret=" << result << std::endl;
        return false;
    }
    result = rtFunctionRegister(registration.binaryHandle, KernelSignature(),
        kKernelName, kKernelName, 0);
    if (result != RT_ERROR_NONE) {
        std::cerr << "rtFunctionRegister failed, ret=" << result << std::endl;
        (void)rtDevBinaryUnRegister(registration.binaryHandle);
        registration.binaryHandle = nullptr;
        return false;
    }
    return true;
}

void UnregisterKernel(KernelRegistration& registration)
{
    if (registration.binaryHandle != nullptr) {
        (void)rtDevBinaryUnRegister(registration.binaryHandle);
    }
    registration.binaryHandle = nullptr;
}

bool LaunchKernel(aclrtStream stream, const DeviceBuffer (&inputs)[4],
    const DeviceBuffer& output, GM_ADDR commArgs)
{
    struct KernelArgs {
        void* input0;
        void* input1;
        void* input2;
        void* input3;
        void* output;
        GM_ADDR commArgs;
    } args {
        inputs[0].Data(), inputs[1].Data(), inputs[2].Data(), inputs[3].Data(),
        output.Data(), commArgs,
    };

    rtArgsEx_t argsInfo {};
    argsInfo.args = &args;
    argsInfo.argsSize = sizeof(args);
    rtTaskCfgInfo_t config {};
    config.schemMode = 1;
    const rtError_t result = rtKernelLaunchWithFlagV2(KernelSignature(), kBlockDim,
        &argsInfo, nullptr, static_cast<rtStream_t>(stream), 0U, &config);
    if (result != RT_ERROR_NONE) {
        std::cerr << "rtKernelLaunchWithFlagV2 failed, ret=" << result << std::endl;
        return false;
    }
    return true;
}

bool ValidateOutput(const std::vector<float> (&inputs)[4], const std::vector<float>& output)
{
    for (uint32_t index = 0U; index < kTotalElements; ++index) {
        const float expected = inputs[0][index] + inputs[1][index] +
            inputs[2][index] + inputs[3][index];
        if (std::fabs(output[index] - expected) > 1.0e-5f) {
            std::cerr << "Add4 mismatch at index " << index << ": got=" << output[index]
                      << ", expected=" << expected << std::endl;
            return false;
        }
    }
    return true;
}

} // namespace

int main()
{
    const int deviceId = DeviceId();
    bool aclInitialized = false;
    bool deviceSet = false;
    aclrtStream stream = nullptr;
    TileXRCommPtr comm = nullptr;
    KernelRegistration registration;
    int exitCode = 1;

    do {
        if (!CheckAcl("aclInit", aclInit(nullptr))) {
            break;
        }
        aclInitialized = true;
        if (!CheckAcl("aclrtSetDevice", aclrtSetDevice(deviceId))) {
            break;
        }
        deviceSet = true;
        if (!CheckAcl("aclrtCreateStream", aclrtCreateStream(&stream)) ||
            !CheckTileXR("TileXRCommInitRankLocal",
                TileXRCommInitRankLocal(1, 0, &comm))) {
            break;
        }
        GM_ADDR commArgs = nullptr;
        if (!CheckTileXR("TileXRGetCommArgsDev", TileXRGetCommArgsDev(comm, commArgs)) ||
            commArgs == nullptr || !RegisterKernel(registration)) {
            break;
        }

        std::vector<float> hostInputs[4];
        for (uint32_t input = 0U; input < 4U; ++input) {
            hostInputs[input].resize(kTotalElements);
            for (uint32_t index = 0U; index < kTotalElements; ++index) {
                hostInputs[input][index] = static_cast<float>(
                    (index % (17U + input * 2U)) + input * 3U);
            }
        }
        std::vector<float> hostOutput(kTotalElements, 0.0f);
        DeviceBuffer deviceInputs[4];
        DeviceBuffer deviceOutput;
        bool allocated = deviceOutput.Allocate(hostOutput.size() * sizeof(float));
        for (uint32_t input = 0U; input < 4U && allocated; ++input) {
            allocated = deviceInputs[input].Allocate(hostInputs[input].size() * sizeof(float));
        }
        if (!allocated) {
            std::cerr << "device allocation failed" << std::endl;
            break;
        }

        bool copied = true;
        for (uint32_t input = 0U; input < 4U && copied; ++input) {
            copied = CheckAcl("aclrtMemcpy input H2D",
                aclrtMemcpy(deviceInputs[input].Data(), deviceInputs[input].Size(),
                    hostInputs[input].data(), hostInputs[input].size() * sizeof(float),
                    ACL_MEMCPY_HOST_TO_DEVICE));
        }
        if (!copied || !LaunchKernel(stream, deviceInputs, deviceOutput, commArgs) ||
            !CheckAcl("aclrtSynchronizeStream", aclrtSynchronizeStream(stream)) ||
            !CheckAcl("aclrtMemcpy output D2H", aclrtMemcpy(hostOutput.data(),
                hostOutput.size() * sizeof(float), deviceOutput.Data(), deviceOutput.Size(),
                ACL_MEMCPY_DEVICE_TO_HOST)) ||
            !ValidateOutput(hostInputs, hostOutput)) {
            break;
        }

        std::cout << "Add4 correctness passed; profiling records will be exported by "
                     "TileXRCommDestroy" << std::endl;
        exitCode = 0;
    } while (false);

    UnregisterKernel(registration);
    if (comm != nullptr) {
        if (!CheckTileXR("TileXRCommDestroy", TileXRCommDestroy(comm))) {
            exitCode = 1;
        }
    }
    if (stream != nullptr) {
        (void)aclrtDestroyStream(stream);
    }
    if (deviceSet) {
        (void)aclrtResetDevice(deviceId);
    }
    if (aclInitialized) {
        (void)aclFinalize();
    }
    return exitCode;
}
