#ifndef TILEXR_KERNEL_BINARY_LOADER_H
#define TILEXR_KERNEL_BINARY_LOADER_H

#include <cstddef>

namespace TileXR {

struct KernelBinaryView {
    const unsigned char *data = nullptr;
    std::size_t size = 0;
};

bool LoadKernelBinary(const void *libraryAnchor, const char *fileName,
    KernelBinaryView *binary);

} // namespace TileXR

#endif // TILEXR_KERNEL_BINARY_LOADER_H
