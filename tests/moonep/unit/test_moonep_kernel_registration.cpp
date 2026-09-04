#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <fstream>
#include <iostream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

#include "moonep_kernel_registration.h"
#include "runtime/kernel.h"
#include "tilexr_types.h"

namespace {

int gFailures = 0;
int gBinaryRegisterCalls = 0;
int gBinaryUnregisterCalls = 0;
int gFunctionRegisterCalls = 0;
rtError_t gFunctionRegisterResult = RT_ERROR_NONE;
rtDevBinary_t gRegisteredBinary {};

void Check(bool condition, const char *message)
{
    if (!condition) {
        std::cerr << "FAIL: " << message << std::endl;
        ++gFailures;
    }
}

bool MakeDirectory(const std::string &path)
{
    return mkdir(path.c_str(), 0755) == 0 || errno == EEXIST;
}

std::string KernelDirectory()
{
    Dl_info info {};
    if (dladdr(reinterpret_cast<const void *>(&KernelDirectory), &info) == 0 ||
        info.dli_fname == nullptr) {
        return std::string();
    }
    char *resolved = realpath(info.dli_fname, nullptr);
    if (resolved == nullptr) {
        return std::string();
    }
    std::string executable(resolved);
    std::free(resolved);
    const std::string::size_type separator = executable.find_last_of('/');
    if (separator == std::string::npos) {
        return std::string();
    }
    const std::string base = executable.substr(0, separator);
    Check(MakeDirectory(base + "/tilexr"), "create tilexr directory");
    Check(MakeDirectory(base + "/tilexr/kernels"), "create kernel directory");
    return base + "/tilexr/kernels";
}

} // namespace

extern "C" rtError_t rtDevBinaryRegister(const rtDevBinary_t *binary,
    void **handle)
{
    ++gBinaryRegisterCalls;
    if (binary != nullptr) {
        gRegisteredBinary = *binary;
    }
    if (handle != nullptr) {
        *handle = reinterpret_cast<void *>(uintptr_t {0x1234});
    }
    return RT_ERROR_NONE;
}

extern "C" rtError_t rtDevBinaryUnRegister(void *)
{
    ++gBinaryUnregisterCalls;
    return RT_ERROR_NONE;
}

extern "C" rtError_t rtFunctionRegister(void *, const void *, const char_t *,
    const void *, uint32_t)
{
    ++gFunctionRegisterCalls;
    return gFunctionRegisterResult;
}

int main()
{
    TileXRMoonEp::KernelRegistrationState missingState;
    Check(TileXRMoonEp::EnsureMoonEpKernelRegistered(missingState,
        "missing_registration_kernel.o",
        TileXRMoonEp::KernelSignature(TileXRMoonEp::kPlannerKernelSignature),
        "missing_kernel") == TileXR::TILEXR_ERROR_NOT_INITIALIZED,
        "missing binary returns not initialized");
    Check(gBinaryRegisterCalls == 0, "missing binary does not register");

    const std::string fileName = "registration_kernel_" +
        std::to_string(getpid()) + ".o";
    const std::string path = KernelDirectory() + "/" + fileName;
    const unsigned char expected[] = {0x7f, 'E', 'L', 'F', 0x03};
    {
        std::ofstream output(path.c_str(), std::ios::binary);
        output.write(reinterpret_cast<const char *>(expected), sizeof(expected));
        Check(static_cast<bool>(output), "write registration fixture");
    }

    TileXRMoonEp::KernelRegistrationState retryState;
    gFunctionRegisterResult = -1;
    Check(TileXRMoonEp::EnsureMoonEpKernelRegistered(retryState,
        fileName.c_str(),
        TileXRMoonEp::KernelSignature(TileXRMoonEp::kPlannerKernelSignature),
        "registration_kernel") == TileXR::TILEXR_ERROR_MKIRT,
        "function registration failure is translated");
    Check(gBinaryRegisterCalls == 1 && gFunctionRegisterCalls == 1 &&
        gBinaryUnregisterCalls == 1, "failed registration releases handle");

    gFunctionRegisterResult = RT_ERROR_NONE;
    Check(TileXRMoonEp::EnsureMoonEpKernelRegistered(retryState,
        fileName.c_str(),
        TileXRMoonEp::KernelSignature(TileXRMoonEp::kPlannerKernelSignature),
        "registration_kernel") == TileXR::TILEXR_SUCCESS,
        "registration retry succeeds");
    Check(retryState.registered && gBinaryRegisterCalls == 2 &&
        gFunctionRegisterCalls == 2, "successful registration is retained");
    Check(gRegisteredBinary.data != nullptr &&
        gRegisteredBinary.length == sizeof(expected) &&
        std::memcmp(gRegisteredBinary.data, expected, sizeof(expected)) == 0,
        "external bytes reach runtime descriptor");

    Check(TileXRMoonEp::EnsureMoonEpKernelRegistered(retryState,
        fileName.c_str(),
        TileXRMoonEp::KernelSignature(TileXRMoonEp::kPlannerKernelSignature),
        "registration_kernel") == TileXR::TILEXR_SUCCESS,
        "registered state is reusable");
    Check(gBinaryRegisterCalls == 2 && gFunctionRegisterCalls == 2,
        "registered state avoids duplicate runtime calls");
    Check(unlink(path.c_str()) == 0, "remove registration fixture");

    return gFailures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
