#ifndef TILEXR_TEST_MOCK_ACL_H
#define TILEXR_TEST_MOCK_ACL_H
#include <cstddef>
typedef int aclError;
typedef void *aclrtStream;
typedef void *aclrtContext;
enum { ACL_SUCCESS=0, ACL_MEM_MALLOC_HUGE_FIRST=0, ACL_MEMCPY_HOST_TO_DEVICE=1, ACL_MEMCPY_DEVICE_TO_HOST=2 };
aclError aclrtGetCurrentContext(aclrtContext *);
aclError aclrtMalloc(void **, size_t, int);
aclError aclrtMemset(void *, size_t, int, size_t);
aclError aclrtMemcpy(void *, size_t, const void *, size_t, int);
aclError aclrtSynchronizeStream(aclrtStream);
aclError aclrtFree(void *);
#endif
