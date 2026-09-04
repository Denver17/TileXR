cmake_minimum_required(VERSION 3.16)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(TileXR_MoonEP_Tests LANGUAGES C CXX)
    enable_testing()
else()
    enable_language(C)
endif()

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(TILEXR_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../..")
set(TILEXR_MOONEP_FAKE_INCLUDE_DIR "${CMAKE_CURRENT_BINARY_DIR}/fake_include")
file(MAKE_DIRECTORY "${TILEXR_MOONEP_FAKE_INCLUDE_DIR}/acl")
file(MAKE_DIRECTORY "${TILEXR_MOONEP_FAKE_INCLUDE_DIR}/runtime")

file(WRITE "${TILEXR_MOONEP_FAKE_INCLUDE_DIR}/acl/acl_base.h"
"#ifndef TILEXR_MOONEP_TEST_ACL_BASE_H\n"
"#define TILEXR_MOONEP_TEST_ACL_BASE_H\n"
"typedef void *aclrtStream;\n"
"typedef int aclError;\n"
"#define ACL_SUCCESS 0\n"
"#endif\n")

file(WRITE "${TILEXR_MOONEP_FAKE_INCLUDE_DIR}/acl/acl_rt.h"
"#ifndef TILEXR_MOONEP_TEST_ACL_RT_H\n"
"#define TILEXR_MOONEP_TEST_ACL_RT_H\n"
"#include <stddef.h>\n"
"#include <stdint.h>\n"
"#include \"acl_base.h\"\n"
"typedef enum aclrtMemcpyKind { ACL_MEMCPY_DEVICE_TO_DEVICE = 3 } aclrtMemcpyKind;\n"
"typedef enum aclrtDevAttr { ACL_DEV_ATTR_VECTOR_CORE_NUM = 0, ACL_DEV_ATTR_UBUF_PER_VECTOR_CORE = 1 } aclrtDevAttr;\n"
"#ifdef __cplusplus\nextern \"C\" {\n#endif\n"
"aclError aclrtMemsetAsync(void *devPtr, size_t maxCount, int32_t value, size_t count, aclrtStream stream);\n"
"aclError aclrtMemcpyAsync(void *dst, size_t destMax, const void *src, size_t count, aclrtMemcpyKind kind, aclrtStream stream);\n"
"aclError aclrtSynchronizeStream(aclrtStream stream);\n"
"aclError aclrtGetDevice(int32_t *deviceId);\n"
"aclError aclrtGetDeviceInfo(uint32_t deviceId, aclrtDevAttr attr, int64_t *value);\n"
"#ifdef __cplusplus\n}\n#endif\n"
"#endif\n")

file(WRITE "${TILEXR_MOONEP_FAKE_INCLUDE_DIR}/runtime/kernel.h"
"#ifndef TILEXR_MOONEP_TEST_RUNTIME_KERNEL_H\n"
"#define TILEXR_MOONEP_TEST_RUNTIME_KERNEL_H\n"
"#include <stddef.h>\n"
"#include <stdint.h>\n"
"typedef char char_t;\n"
"typedef int32_t rtError_t;\n"
"typedef void *rtStream_t;\n"
"typedef struct rtDevBinary { uint32_t magic; uint32_t version; const void *data; uint64_t length; } rtDevBinary_t;\n"
"typedef struct rtArgsEx { void *args; size_t argsSize; } rtArgsEx_t;\n"
"typedef struct rtSmDesc { uint32_t reserved; } rtSmDesc_t;\n"
"typedef struct rtTaskCfgInfo { uint32_t schemMode; uint32_t localMemorySize; } rtTaskCfgInfo_t;\n"
"#define RT_ERROR_NONE 0\n"
"#define RT_DEV_BINARY_MAGIC_ELF_AIVEC 0x41415246U\n"
"#ifdef __cplusplus\nextern \"C\" {\n#endif\n"
"rtError_t rtDevBinaryRegister(const rtDevBinary_t *binary, void **handle);\n"
"rtError_t rtDevBinaryUnRegister(void *handle);\n"
"rtError_t rtFunctionRegister(void *handle, const void *signature, const char_t *name, const void *kernelInfoExt, uint32_t flags);\n"
"rtError_t rtKernelLaunchWithFlagV2(const void *signature, uint32_t blockDim, rtArgsEx_t *argsInfo, rtSmDesc_t *smDesc, rtStream_t stream, uint32_t flags, const rtTaskCfgInfo_t *cfgInfo);\n"
"#ifdef __cplusplus\n}\n#endif\n"
"#endif\n")

file(WRITE "${TILEXR_MOONEP_FAKE_INCLUDE_DIR}/tilexr_moonep_planner.h"
"#ifndef TILEXR_MOONEP_TEST_PLANNER_H\n"
"#define TILEXR_MOONEP_TEST_PLANNER_H\n"
"#include <stdint.h>\n"
"#include \"acl/acl_base.h\"\n"
"#ifdef __cplusplus\nextern \"C\" {\n#endif\n"
"typedef void *TileXRCommPtr;\n"
"int TileXRMoonEpPlannerGetWorkspaceSizeV2(TileXRCommPtr comm, int64_t s, int64_t k, int64_t expertCount, uint64_t *workspaceBytes, int64_t *dispatchedCapacity);\n"
"int TileXRMoonEpPlannerV2(const int32_t *topkExpertIds, const int32_t *tokensPerExpert, TileXRCommPtr comm, int64_t s, int64_t k, int64_t expertCount, void *workspace, uint64_t workspaceBytes, int32_t *dst, int32_t *cuSeqlens, int32_t *expertsToCopy, int32_t *remoteStats, int32_t *plannerStatus, uint64_t waitIterations, aclrtStream stream);\n"
"int TileXRMoonEpPlannerGetWorkspaceSizeV3(TileXRCommPtr comm, int64_t s, int64_t k, int64_t expertCount, int64_t b, int64_t tokenPadding, uint64_t *workspaceBytes, int64_t *nvS);\n"
"int TileXRMoonEpPlannerV3(const int32_t *topkExpertIds, const int32_t *tokensPerExpert, TileXRCommPtr comm, int64_t s, int64_t k, int64_t expertCount, int64_t b, int64_t tokenPadding, void *workspace, uint64_t workspaceBytes, int32_t *dst, int32_t *cuSeqlens, int32_t *expertsToCopy, int32_t *zeroFillRanges, int32_t *remoteStats, int32_t *dupCounts, int32_t *plannerStatus, uint64_t waitIterations, aclrtStream stream);\n"
"#ifdef __cplusplus\n}\n#endif\n"
"#endif\n")

file(WRITE "${TILEXR_MOONEP_FAKE_INCLUDE_DIR}/tilexr_api.h"
"#ifndef TILEXR_MOONEP_TEST_TILEXR_API_H\n"
"#define TILEXR_MOONEP_TEST_TILEXR_API_H\n"
"#ifdef __cplusplus\n"
"#include \"comm_args.h\"\n"
"#include \"tilexr_udma_fullmesh.h\"\n"
"#include \"tilexr_udma_reg.h\"\n"
"extern \"C\" {\n"
"typedef void *TileXRCommPtr;\n"
"typedef uint32_t TileXRUDMAProfileHandle;\n"
"int TileXRGetCommArgsHost(TileXRCommPtr comm, TileXR::CommArgs *&commArgs);\n"
"int TileXRGetCommArgsDev(TileXRCommPtr comm, GM_ADDR &commArgs);\n"
"int TileXRGetUDMARegistryHost(TileXRCommPtr comm, const TileXR::TileXRUDMARegistry **registry);\n"
"int TileXRUDMAGetQpCount(TileXRCommPtr comm, uint32_t *qpCount);\n"
"int TileXRUDMAFullmeshQuery(TileXRCommPtr comm, TileXR::TileXRUDMAFullmeshHostView *view);\n"
"int TileXRUDMAProfileRegister(TileXRCommPtr comm, const TileXR::TileXRUDMAProfileDesc *desc, TileXRUDMAProfileHandle *handle);\n"
"int TileXRUDMAProfileUnregister(TileXRCommPtr comm, TileXRUDMAProfileHandle handle);\n"
"int TileXRUDMAProfileQuery(TileXRCommPtr comm, TileXRUDMAProfileHandle handle, TileXR::TileXRUDMAProfileView *view);\n"
"int TileXRCommNextMagic(TileXRCommPtr comm, int64_t *magic);\n"
"}\n"
"#endif\n"
"#endif\n")

set(TILEXR_MOONEP_TEST_INCLUDE_DIRS
    ${TILEXR_MOONEP_FAKE_INCLUDE_DIR}
    ${TILEXR_ROOT}/src/include
    ${TILEXR_ROOT}/src/common
)

add_executable(test_tilexr_moonep_c_header
    unit/test_tilexr_moonep_c_header.c
)
target_include_directories(test_tilexr_moonep_c_header PRIVATE
    ${TILEXR_MOONEP_TEST_INCLUDE_DIRS})

add_executable(test_tilexr_moonep_abi_layout
    unit/test_tilexr_moonep_abi_layout.cpp
)
target_include_directories(test_tilexr_moonep_abi_layout PRIVATE
    ${TILEXR_MOONEP_TEST_INCLUDE_DIRS})

add_executable(test_tilexr_moonep_kernel_registration
    unit/test_moonep_kernel_registration.cpp
    ${TILEXR_ROOT}/src/moonep/common/moonep_kernel_registration.cpp
    ${TILEXR_ROOT}/src/common/tilexr_kernel_binary_loader.cpp
)
target_include_directories(test_tilexr_moonep_kernel_registration PRIVATE
    ${TILEXR_MOONEP_TEST_INCLUDE_DIRS}
    ${TILEXR_ROOT}/src/moonep/common)
target_link_libraries(test_tilexr_moonep_kernel_registration PRIVATE
    ${CMAKE_DL_LIBS})

add_executable(test_tilexr_moonep_host
    unit/test_tilexr_moonep_host.cpp
    ${TILEXR_ROOT}/src/moonep/host/tilexr_moonep.cpp
)
target_include_directories(test_tilexr_moonep_host PRIVATE
    ${TILEXR_MOONEP_TEST_INCLUDE_DIRS})

add_executable(test_tilexr_moonep_stage_layout
    unit/test_tilexr_moonep_stage_layout.cpp
    ${TILEXR_ROOT}/src/moonep/dispatch/host/dispatch_layout.cpp
)
target_include_directories(test_tilexr_moonep_stage_layout PRIVATE
    ${TILEXR_MOONEP_TEST_INCLUDE_DIRS}
    ${TILEXR_ROOT}/src/moonep/common
    ${TILEXR_ROOT}/src/moonep/dispatch/host
)

add_executable(test_tilexr_moonep_stage_reference
    unit/test_tilexr_moonep_stage_reference.cpp
    reference/moonep_stage_reference.cpp
)
target_include_directories(test_tilexr_moonep_stage_reference PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/reference
)

add_executable(test_tilexr_moonep_dispatch_urma_layout
    unit/test_tilexr_moonep_dispatch_layout.cpp
    ${TILEXR_ROOT}/src/moonep/dispatch/urma/host/dispatch_layout.cpp
)
target_include_directories(test_tilexr_moonep_dispatch_urma_layout PRIVATE
    ${TILEXR_MOONEP_TEST_INCLUDE_DIRS}
    ${TILEXR_ROOT}/src/moonep/dispatch/urma/common
    ${TILEXR_ROOT}/src/moonep/dispatch/urma/host
)

add_executable(test_tilexr_moonep_dispatch_urma_schedule
    unit/test_tilexr_moonep_dispatch_schedule.cpp
)

add_executable(test_tilexr_moonep_dispatch_urma_host
    unit/test_tilexr_moonep_dispatch_urma_host.cpp
    ${TILEXR_ROOT}/src/moonep/dispatch/urma/host/dispatch_layout.cpp
    ${TILEXR_ROOT}/src/moonep/dispatch/urma/host/dispatch_host.cpp
)
target_include_directories(test_tilexr_moonep_dispatch_urma_host PRIVATE
    ${TILEXR_MOONEP_TEST_INCLUDE_DIRS}
    ${TILEXR_ROOT}/src/moonep/dispatch/urma/common
    ${TILEXR_ROOT}/src/moonep/dispatch/urma/host
)

add_executable(test_tilexr_moonep_dispatch_urma_launch
    unit/test_tilexr_moonep_dispatch_urma_launch.cpp
    ${TILEXR_ROOT}/src/moonep/dispatch/urma/host/dispatch_layout.cpp
    ${TILEXR_ROOT}/src/moonep/dispatch/urma/host/dispatch_launch.cpp
)
target_include_directories(test_tilexr_moonep_dispatch_urma_launch PRIVATE
    ${TILEXR_MOONEP_TEST_INCLUDE_DIRS}
    ${TILEXR_ROOT}/src/moonep/dispatch/urma/common
    ${TILEXR_ROOT}/src/moonep/dispatch/urma/host
)
target_include_directories(test_tilexr_moonep_dispatch_urma_schedule PRIVATE
    ${TILEXR_MOONEP_TEST_INCLUDE_DIRS}
    ${TILEXR_ROOT}/src/moonep/dispatch/urma/common
)

add_executable(test_tilexr_moonep_dispatch_host
    unit/test_tilexr_moonep_dispatch_host.cpp
    ${TILEXR_ROOT}/src/moonep/dispatch/host/dispatch_layout.cpp
    ${TILEXR_ROOT}/src/moonep/dispatch/host/dispatch_host.cpp
    ${TILEXR_ROOT}/src/moonep/dispatch/host/tilexr_moonep_dispatch.cpp
)
target_include_directories(test_tilexr_moonep_dispatch_host PRIVATE
    ${TILEXR_MOONEP_TEST_INCLUDE_DIRS}
    ${TILEXR_ROOT}/src/moonep/common
    ${TILEXR_ROOT}/src/moonep/dispatch/common
    ${TILEXR_ROOT}/src/moonep/dispatch/host
)

add_executable(test_tilexr_moonep_prefetch_weight_host
    unit/test_tilexr_moonep_prefetch_weight_host.cpp
    ${TILEXR_ROOT}/src/moonep/prefetch_weight/host/prefetch_weight_layout.cpp
    ${TILEXR_ROOT}/src/moonep/prefetch_weight/host/prefetch_weight_host.cpp
    ${TILEXR_ROOT}/src/moonep/prefetch_weight/host/tilexr_moonep_prefetch_weight.cpp
)
target_include_directories(test_tilexr_moonep_prefetch_weight_host PRIVATE
    ${TILEXR_MOONEP_TEST_INCLUDE_DIRS}
    ${TILEXR_ROOT}/src/moonep/common
    ${TILEXR_ROOT}/src/moonep/prefetch_weight/common
    ${TILEXR_ROOT}/src/moonep/prefetch_weight/host)

add_executable(test_tilexr_moonep_reduce_grad_layout
    unit/test_tilexr_moonep_reduce_grad_layout.cpp
    ${TILEXR_ROOT}/src/moonep/reduce_grad/host/reduce_grad_layout.cpp
)
target_include_directories(test_tilexr_moonep_reduce_grad_layout PRIVATE
    ${TILEXR_MOONEP_TEST_INCLUDE_DIRS}
    ${TILEXR_ROOT}/src/moonep/reduce_grad/common
    ${TILEXR_ROOT}/src/moonep/reduce_grad/host)

add_executable(test_tilexr_moonep_reduce_grad_host
    unit/test_tilexr_moonep_reduce_grad_host.cpp
    ${TILEXR_ROOT}/src/moonep/reduce_grad/host/reduce_grad_layout.cpp
    ${TILEXR_ROOT}/src/moonep/reduce_grad/host/reduce_grad_host.cpp
)
target_include_directories(test_tilexr_moonep_reduce_grad_host PRIVATE
    ${TILEXR_MOONEP_TEST_INCLUDE_DIRS}
    ${TILEXR_ROOT}/src/moonep/common
    ${TILEXR_ROOT}/src/moonep/reduce_grad/common
    ${TILEXR_ROOT}/src/moonep/reduce_grad/host)

add_executable(test_tilexr_moonep_reduce_grad_launch
    unit/test_tilexr_moonep_reduce_grad_launch.cpp
    ${TILEXR_ROOT}/src/moonep/reduce_grad/host/reduce_grad_launch.cpp
)
target_include_directories(test_tilexr_moonep_reduce_grad_launch PRIVATE
    ${TILEXR_MOONEP_TEST_INCLUDE_DIRS}
    ${TILEXR_ROOT}/src/moonep/reduce_grad/common
    ${TILEXR_ROOT}/src/moonep/reduce_grad/host)

add_test(NAME test_tilexr_moonep_c_header COMMAND test_tilexr_moonep_c_header)
add_test(NAME test_tilexr_moonep_abi_layout COMMAND test_tilexr_moonep_abi_layout)
add_test(NAME test_tilexr_moonep_kernel_registration
    COMMAND test_tilexr_moonep_kernel_registration)
add_test(NAME test_tilexr_moonep_host COMMAND test_tilexr_moonep_host)
add_test(NAME test_tilexr_moonep_stage_layout COMMAND test_tilexr_moonep_stage_layout)
add_test(NAME test_tilexr_moonep_stage_reference COMMAND test_tilexr_moonep_stage_reference)
add_test(NAME test_tilexr_moonep_dispatch_urma_layout
    COMMAND test_tilexr_moonep_dispatch_urma_layout)
add_test(NAME test_tilexr_moonep_dispatch_urma_schedule
    COMMAND test_tilexr_moonep_dispatch_urma_schedule)
add_test(NAME test_tilexr_moonep_dispatch_urma_host
    COMMAND test_tilexr_moonep_dispatch_urma_host)
add_test(NAME test_tilexr_moonep_dispatch_urma_launch
    COMMAND test_tilexr_moonep_dispatch_urma_launch)
add_test(NAME test_tilexr_moonep_dispatch_host COMMAND test_tilexr_moonep_dispatch_host)
add_test(NAME test_tilexr_moonep_prefetch_weight_host COMMAND test_tilexr_moonep_prefetch_weight_host)
add_test(NAME test_tilexr_moonep_reduce_grad_layout COMMAND test_tilexr_moonep_reduce_grad_layout)
add_test(NAME test_tilexr_moonep_reduce_grad_host COMMAND test_tilexr_moonep_reduce_grad_host)
add_test(NAME test_tilexr_moonep_reduce_grad_launch COMMAND test_tilexr_moonep_reduce_grad_launch)
