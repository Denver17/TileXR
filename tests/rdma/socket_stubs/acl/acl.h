#pragma once
#include <stdint.h>
#define ACL_SUCCESS 0
#ifdef __cplusplus
extern "C" {
#endif
int aclrtGetDevice(int32_t* device);
#ifdef __cplusplus
}
#endif
