#include "tilexr_kernel_binary_loader.h"

#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace TileXR {
namespace {

using BinaryBytes = std::vector<unsigned char>;

std::mutex gBinaryCacheMutex;
std::map<std::string, std::shared_ptr<BinaryBytes>> gBinaryCache;

bool IsSafeBasename(const char *fileName)
{
    if (fileName == nullptr || fileName[0] == '\0' ||
        std::strcmp(fileName, ".") == 0 || std::strcmp(fileName, "..") == 0) {
        return false;
    }
    return std::strchr(fileName, '/') == nullptr &&
        std::strchr(fileName, '\\') == nullptr;
}

bool ResolveKernelPath(const void *libraryAnchor, const char *fileName,
    std::string *path)
{
    if (libraryAnchor == nullptr || path == nullptr || !IsSafeBasename(fileName)) {
        std::cerr << "TileXR kernel binary path has invalid arguments" << std::endl;
        return false;
    }

    Dl_info info {};
    if (dladdr(libraryAnchor, &info) == 0 || info.dli_fname == nullptr ||
        info.dli_fname[0] == '\0') {
        std::cerr << "TileXR failed to locate the owning Host library for kernel "
                  << fileName << std::endl;
        return false;
    }

    char *resolvedLibrary = realpath(info.dli_fname, nullptr);
    if (resolvedLibrary == nullptr) {
        std::cerr << "TileXR failed to resolve Host library path " << info.dli_fname
                  << ": " << std::strerror(errno) << std::endl;
        return false;
    }

    std::string libraryPath(resolvedLibrary);
    std::free(resolvedLibrary);
    const std::string::size_type separator = libraryPath.find_last_of('/');
    if (separator == std::string::npos) {
        std::cerr << "TileXR Host library path has no directory: "
                  << libraryPath << std::endl;
        return false;
    }

    *path = libraryPath.substr(0, separator) + "/tilexr/kernels/" + fileName;
    return true;
}

bool ReadKernelBinary(const std::string &path,
    std::shared_ptr<BinaryBytes> *bytes)
{
    std::ifstream input(path.c_str(), std::ios::binary | std::ios::ate);
    if (!input) {
        std::cerr << "TileXR failed to open kernel binary " << path
                  << ": " << std::strerror(errno) << std::endl;
        return false;
    }

    const std::streamoff size = input.tellg();
    if (size <= 0 || static_cast<unsigned long long>(size) >
        static_cast<unsigned long long>(std::numeric_limits<uint32_t>::max())) {
        std::cerr << "TileXR kernel binary has invalid size: " << path << std::endl;
        return false;
    }

    const std::size_t byteCount = static_cast<std::size_t>(size);
    std::shared_ptr<BinaryBytes> loaded(new BinaryBytes(byteCount));
    input.seekg(0, std::ios::beg);
    input.read(reinterpret_cast<char *>(loaded->data()),
        static_cast<std::streamsize>(byteCount));
    if (!input || static_cast<std::size_t>(input.gcount()) != byteCount) {
        std::cerr << "TileXR failed to read complete kernel binary " << path
                  << std::endl;
        return false;
    }

    *bytes = loaded;
    return true;
}

} // namespace

bool LoadKernelBinary(const void *libraryAnchor, const char *fileName,
    KernelBinaryView *binary)
{
    if (binary == nullptr) {
        std::cerr << "TileXR kernel binary output is null" << std::endl;
        return false;
    }
    *binary = KernelBinaryView {};

    std::string path;
    if (!ResolveKernelPath(libraryAnchor, fileName, &path)) {
        return false;
    }

    std::lock_guard<std::mutex> guard(gBinaryCacheMutex);
    const auto cached = gBinaryCache.find(path);
    if (cached != gBinaryCache.end()) {
        binary->data = cached->second->data();
        binary->size = cached->second->size();
        return true;
    }

    std::shared_ptr<BinaryBytes> loaded;
    if (!ReadKernelBinary(path, &loaded)) {
        return false;
    }
    const auto inserted = gBinaryCache.emplace(path, loaded);
    binary->data = inserted.first->second->data();
    binary->size = inserted.first->second->size();
    return true;
}

} // namespace TileXR
