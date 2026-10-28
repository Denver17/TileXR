// Test-only ACL handle and status types; real SDK compatibility uses installed consumers.
#pragma once
using aclrtStream = void *;
using aclError = int;
constexpr int ACL_SUCCESS = 0;
