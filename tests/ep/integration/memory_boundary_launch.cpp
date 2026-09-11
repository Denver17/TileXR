#include "memory_boundary_test.h"

#include <mutex>
#include "runtime/kernel.h"
#include "tilexr_kernel_binary_loader.h"
#include "tilexr_types.h"

namespace TileXRMemoryBoundaryTest {
namespace {
std::mutex mutex;
void *binaryHandle = nullptr;
uint8_t stub = 0;
constexpr char kName[] = "tilexr_ep_memory_boundary_test_kernel";

int Register()
{
    std::lock_guard<std::mutex> guard(mutex);
    if (binaryHandle != nullptr) return TileXR::TILEXR_SUCCESS;
    TileXR::KernelBinaryView view {};
    if (!TileXR::LoadKernelBinary(reinterpret_cast<const void *>(&Register),
            "tilexr_ep_memory_boundary_test_kernel.o", &view)) {
        return TileXR::TILEXR_ERROR_NOT_INITIALIZED;
    }
    rtDevBinary_t binary {};
    binary.magic = 0x41415246U;
    binary.data = view.data;
    binary.length = view.size;
    void *handle = nullptr;
    int ret = rtDevBinaryRegister(&binary, &handle);
    if (ret == RT_ERROR_NONE) ret = rtFunctionRegister(handle, &stub, kName, kName, 0U);
    if (ret != RT_ERROR_NONE) {
        if (handle != nullptr) (void)rtDevBinaryUnRegister(handle);
        return ret;
    }
    binaryHandle = handle;
    return TileXR::TILEXR_SUCCESS;
}
}

int Launch(void *commArgs, void *result, aclrtStream stream)
{
    if (commArgs == nullptr || result == nullptr || stream == nullptr) return TileXR::TILEXR_ERROR_PARA_CHECK_FAIL;
    const int ret = Register();
    if (ret != 0) return ret;
    struct Args { void *commArgs; void *result; uint64_t expectedBytes; } args {commArgs, result, kPeerBytes};
    rtArgsEx_t info {};
    info.args = &args;
    info.argsSize = sizeof(args);
    rtTaskCfgInfo_t config {};
    return rtKernelLaunchWithFlagV2(&stub, 1U, &info, nullptr, static_cast<rtStream_t>(stream), 0U, &config);
}
}
