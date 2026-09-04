#include "tilexr_kernel_binary_loader.h"

#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <fstream>
#include <iostream>
#include <limits>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace {

int gFailed = 0;

void Check(bool condition, const char *message)
{
    if (!condition) {
        std::cerr << "FAIL: " << message << std::endl;
        ++gFailed;
    }
}

const void *Anchor()
{
    return reinterpret_cast<const void *>(&Anchor);
}

bool MakeDirectory(const std::string &path)
{
    return mkdir(path.c_str(), 0755) == 0 || errno == EEXIST;
}

std::string KernelDirectory()
{
    Dl_info info {};
    if (dladdr(Anchor(), &info) == 0 || info.dli_fname == nullptr) {
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

std::string UniqueName(const char *stem)
{
    return std::string(stem) + "_" + std::to_string(getpid()) + ".o";
}

void WriteBytes(const std::string &path, const unsigned char *data,
    std::size_t size)
{
    std::ofstream output(path.c_str(), std::ios::binary);
    output.write(reinterpret_cast<const char *>(data),
        static_cast<std::streamsize>(size));
    Check(static_cast<bool>(output), "write kernel fixture");
}

} // namespace

int main()
{
    const std::string directory = KernelDirectory();
    Check(!directory.empty(), "resolve test executable directory");

    const unsigned char expected[] = {0x7f, 'E', 'L', 'F', 0x01, 0x02};
    const std::string validName = UniqueName("valid_kernel");
    const std::string validPath = directory + "/" + validName;
    WriteBytes(validPath, expected, sizeof(expected));

    TileXR::KernelBinaryView binary {};
    Check(TileXR::LoadKernelBinary(Anchor(), validName.c_str(), &binary),
        "load valid kernel");
    Check(binary.size == sizeof(expected), "preserve kernel size");
    Check(binary.data != nullptr &&
        std::memcmp(binary.data, expected, sizeof(expected)) == 0,
        "preserve kernel bytes");

    const unsigned char *cachedData = binary.data;
    Check(unlink(validPath.c_str()) == 0, "remove cached fixture");
    binary = TileXR::KernelBinaryView {};
    Check(TileXR::LoadKernelBinary(Anchor(), validName.c_str(), &binary),
        "reuse cached kernel after file removal");
    Check(binary.data == cachedData, "keep cached byte address stable");

    Check(!TileXR::LoadKernelBinary(Anchor(), "../unsafe.o", &binary),
        "reject parent path");
    Check(!TileXR::LoadKernelBinary(Anchor(), "missing_kernel.o", &binary),
        "reject missing file");
    Check(!TileXR::LoadKernelBinary(nullptr, "kernel.o", &binary),
        "reject null anchor");
    Check(!TileXR::LoadKernelBinary(Anchor(), "kernel.o", nullptr),
        "reject null output");

    const std::string emptyName = UniqueName("empty_kernel");
    const std::string emptyPath = directory + "/" + emptyName;
    WriteBytes(emptyPath, expected, 0);
    Check(!TileXR::LoadKernelBinary(Anchor(), emptyName.c_str(), &binary),
        "reject empty kernel");
    Check(unlink(emptyPath.c_str()) == 0, "remove empty fixture");

    const std::string largeName = UniqueName("large_kernel");
    const std::string largePath = directory + "/" + largeName;
    WriteBytes(largePath, expected, 0);
    Check(truncate(largePath.c_str(),
        static_cast<off_t>(std::numeric_limits<uint32_t>::max()) + 1) == 0,
        "create sparse oversized fixture");
    Check(!TileXR::LoadKernelBinary(Anchor(), largeName.c_str(), &binary),
        "reject oversized kernel");
    Check(unlink(largePath.c_str()) == 0, "remove oversized fixture");

    if (gFailed != 0) {
        std::cerr << "Kernel binary loader checks failed: " << gFailed << std::endl;
        return 1;
    }
    std::cout << "Kernel binary loader checks passed" << std::endl;
    return 0;
}
