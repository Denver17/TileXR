// Minimal test-only ACL declarations for Relay launch/context failure injection.
#pragma once
#include <cstdint>
#include "acl_base.h"
using aclrtContext = void *;
enum aclrtDevAttr { ACL_DEV_ATTR_CUBE_CORE_NUM = 102 };
extern "C" aclError aclrtGetDevice(int32_t *);
extern "C" aclError aclrtGetDeviceInfo(uint32_t, aclrtDevAttr, int64_t *);
extern "C" aclError aclrtSynchronizeStreamWithTimeout(aclrtStream, int32_t);
extern "C" aclError aclrtGetCurrentContext(aclrtContext *);
extern "C" const char *aclrtGetSocName();
