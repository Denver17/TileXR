// Allocates, initializes and uploads bounded ACL device buffers.
// Session releases them after synchronization; Worker failure exits bypass cleanup.
#include "buffer.h"
#include <algorithm>
namespace unitest {
Buffer::Buffer(size_t bytes) : bytes_(std::max<size_t>(bytes, 32)) {
    if (bytes_ > 1024ULL*1024*1024) throw std::runtime_error("buffer exceeds 1 GiB");
    check(aclrtMalloc(&data_, bytes_, ACL_MEM_MALLOC_HUGE_FIRST), "aclrtMalloc");
    check(aclrtMemset(data_, bytes_, 0xa5, bytes_), "aclrtMemset");
}
Buffer::~Buffer() { if (data_) aclrtFree(data_); }
void Buffer::upload(const void *data, size_t bytes) {
    if (bytes > bytes_) throw std::runtime_error("input exceeds capacity");
    if (bytes) check(aclrtMemcpy(data_, bytes_, data, bytes, ACL_MEMCPY_HOST_TO_DEVICE), "H2D");
}
void Buffer::poison() { check(aclrtMemset(data_, bytes_, 0xa5, bytes_), "output reset"); }
void Buffer::release() {
    if (!data_) return;
    check(aclrtFree(data_), "aclrtFree");
    data_ = nullptr;
}
} // namespace unitest
