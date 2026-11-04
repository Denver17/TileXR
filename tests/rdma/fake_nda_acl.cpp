// Host-only ACL stand-in for NDA resource and callback behavior tests.
#include <acl/acl.h>
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>

namespace {
std::map<void*, size_t> allocations;
std::map<void*, size_t> mappings;
std::string failure;
int remaining = 0;
bool Fail(const char* op)
{
    if (failure != op || remaining == 0) { return false; }
    --remaining;
    return true;
}
}
extern "C" {
void tilexr_fake_acl_fail(const char* op, int count) { failure = op; remaining = count; }
int tilexr_fake_acl_allocations() { return static_cast<int>(allocations.size()); }
int tilexr_fake_acl_mappings() { return static_cast<int>(mappings.size()); }
aclError aclrtGetDevice(int32_t* device) { *device = 0; return 0; }
aclError aclrtMalloc(void** ptr, size_t bytes, aclrtMemMallocPolicy)
{
    *ptr = nullptr;
    if (Fail("alloc") || posix_memalign(ptr, 4096, bytes) != 0) { return 1; }
    allocations[*ptr] = bytes;
    return 0;
}
aclError aclrtFree(void* ptr)
{
    if (Fail("free")) { return 1; }
    assert(allocations.erase(ptr) == 1);
    std::free(ptr);
    return 0;
}
aclError aclrtMemset(void* ptr, size_t capacity, int32_t value, size_t bytes)
{
    if (bytes > capacity || Fail("memset")) { return 1; }
    std::memset(ptr, value, bytes);
    return 0;
}
aclError aclrtMemcpy(void* dst, size_t capacity, const void* src, size_t bytes, aclrtMemcpyKind)
{
    if (bytes > capacity || Fail("memcpy")) { return 1; }
    std::memcpy(dst, src, bytes);
    return 0;
}
aclError aclrtHostRegister(void* ptr, uint64_t bytes, aclrtHostRegisterType flags, void** device)
{
    assert(flags == ACL_HOST_REGISTER_IOMEMORY && !mappings.count(ptr));
    if (Fail("map")) { return 1; }
    mappings[ptr] = bytes;
    *device = ptr;
    return 0;
}
aclError aclrtHostUnregister(void* ptr)
{
    if (Fail("unmap")) { return 1; }
    assert(mappings.erase(ptr) == 1);
    return 0;
}
}
