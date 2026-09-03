#
# Copyright (c) 2024-2026 TileXR Project
# CMakeLists.txt for TileXR EP tests
#

cmake_minimum_required(VERSION 3.16)
project(TileXR_EP_Tests)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
include(GNUInstallDirs)
enable_testing()

option(BUILD_TILEXR_EP_DEMO "Build TileXR EP dispatch hardware demo" OFF)
option(BUILD_TILEXR_EP_PLAN_MULTIRANK_TEST "Build the MoonEP Plan multi-rank peer-memory hardware test" OFF)

set(TILEXR_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../..")
set(TILEXR_INSTALL_PREFIX "${TILEXR_ROOT}/install" CACHE PATH "TileXR install prefix")
set(TILEXR_INSTALL_LIBDIR "${CMAKE_INSTALL_LIBDIR}" CACHE STRING "TileXR install library directory")
set(TILEXR_INSTALL_INCLUDEDIR "${CMAKE_INSTALL_INCLUDEDIR}" CACHE STRING "TileXR install include directory")

set(_tilexr_default_ascend_home_path "$ENV{ASCEND_HOME_PATH}")
if(NOT _tilexr_default_ascend_home_path)
    set(_tilexr_default_ascend_home_path "/usr/local/Ascend/ascend-toolkit/latest")
endif()

find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
    add_test(NAME test_dispatch_memory_trace
        COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/python/test_dispatch_memory_trace.py")
    add_test(NAME test_msprof_kernel_stats
        COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/python/test_msprof_kernel_stats.py")
endif()
set(ASCEND_HOME_PATH "${_tilexr_default_ascend_home_path}" CACHE PATH "Ascend toolkit path")

set(_tilexr_default_arch "$ENV{ARCH}")
if(NOT _tilexr_default_arch)
    set(_tilexr_default_arch "$ENV{TILEXR_OS_ARCH}")
endif()
if(NOT _tilexr_default_arch)
    execute_process(
        COMMAND uname -m
        OUTPUT_VARIABLE _tilexr_default_arch
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    if(_tilexr_default_arch STREQUAL "arm64")
        set(_tilexr_default_arch "aarch64")
    endif()
endif()
set(ARCH "${_tilexr_default_arch}" CACHE STRING "Target OS architecture")

set(_tilexr_default_ascend_driver_path "$ENV{ASCEND_DRIVER_PATH}")
if(NOT _tilexr_default_ascend_driver_path)
    set(_tilexr_default_ascend_driver_path "/usr/local/Ascend/driver")
endif()
set(ASCEND_DRIVER_PATH "${_tilexr_default_ascend_driver_path}" CACHE PATH "Ascend driver path")

set(TILEXR_EP_TEST_INCLUDE_DIRS
    ${TILEXR_ROOT}/src/include
    ${TILEXR_ROOT}/src/ep/common
    ${TILEXR_ROOT}/src/ep/host
    ${TILEXR_ROOT}/src/moonep/common
    ${TILEXR_ROOT}/src/moonep/planner_v2/common
    ${TILEXR_ROOT}/src/moonep/planner_v2/host
)

set(TILEXR_EP_FAKE_RUNTIME_INCLUDE_DIR "${CMAKE_CURRENT_BINARY_DIR}/fake_runtime_include")
file(MAKE_DIRECTORY "${TILEXR_EP_FAKE_RUNTIME_INCLUDE_DIR}/acl")
file(MAKE_DIRECTORY "${TILEXR_EP_FAKE_RUNTIME_INCLUDE_DIR}/runtime")
file(WRITE "${TILEXR_EP_FAKE_RUNTIME_INCLUDE_DIR}/acl/acl_base.h" [=[
#ifndef TILEXR_EP_FAKE_ACL_BASE_H
#define TILEXR_EP_FAKE_ACL_BASE_H
typedef int aclError;
typedef void *aclrtStream;
#define ACL_SUCCESS 0
#endif
]=])
file(WRITE "${TILEXR_EP_FAKE_RUNTIME_INCLUDE_DIR}/acl/acl_rt.h" [=[
#ifndef TILEXR_EP_FAKE_ACL_RT_H
#define TILEXR_EP_FAKE_ACL_RT_H
#include <stddef.h>
#include <stdint.h>
#include "acl_base.h"
typedef enum aclrtDevAttr {
    ACL_DEV_ATTR_VECTOR_CORE_NUM = 0,
    ACL_DEV_ATTR_UBUF_PER_VECTOR_CORE = 1
} aclrtDevAttr;
typedef enum aclrtMemcpyKind {
    ACL_MEMCPY_HOST_TO_DEVICE = 1,
    ACL_MEMCPY_DEVICE_TO_HOST = 2,
    ACL_MEMCPY_DEVICE_TO_DEVICE = 3
} aclrtMemcpyKind;
#ifdef __cplusplus
extern "C" {
#endif
aclError aclrtGetDevice(int32_t *deviceId);
aclError aclrtGetDeviceInfo(uint32_t deviceId, aclrtDevAttr attr, int64_t *value);
aclError aclrtSynchronizeStream(aclrtStream stream);
aclError aclrtMemcpy(void *dst, size_t destMax, const void *src,
    size_t count, aclrtMemcpyKind kind);
#ifdef __cplusplus
}
#endif
#endif
]=])
file(WRITE "${TILEXR_EP_FAKE_RUNTIME_INCLUDE_DIR}/runtime/kernel.h" [=[
#ifndef TILEXR_EP_FAKE_RUNTIME_KERNEL_H
#define TILEXR_EP_FAKE_RUNTIME_KERNEL_H
#include <stddef.h>
#include <stdint.h>
typedef char char_t;
typedef int32_t rtError_t;
typedef void *rtStream_t;
typedef struct rtDevBinary {
    uint32_t magic;
    uint32_t version;
    const void *data;
    uint64_t length;
} rtDevBinary_t;
typedef struct rtArgsEx {
    void *args;
    size_t argsSize;
} rtArgsEx_t;
typedef struct rtSmDesc {
    uint32_t reserved;
} rtSmDesc_t;
typedef struct rtTaskCfgInfo {
    uint32_t schemMode;
    uint32_t localMemorySize;
} rtTaskCfgInfo_t;
#define RT_ERROR_NONE 0
#ifdef __cplusplus
extern "C" {
#endif
rtError_t rtDevBinaryRegister(const rtDevBinary_t *binary, void **handle);
rtError_t rtFunctionRegister(void *handle, const void *signature,
    const char_t *name, const void *kernelInfoExt, uint32_t flags);
rtError_t rtKernelLaunchWithFlagV2(const void *signature, uint32_t blockDim,
    rtArgsEx_t *argsInfo, rtSmDesc_t *smDesc, rtStream_t stream,
    uint32_t flags, const rtTaskCfgInfo_t *cfgInfo);
#ifdef __cplusplus
}
#endif
#endif
]=])
file(WRITE "${TILEXR_EP_FAKE_RUNTIME_INCLUDE_DIR}/runtime/rt.h" [=[
#ifndef TILEXR_EP_FAKE_RUNTIME_RT_H
#define TILEXR_EP_FAKE_RUNTIME_RT_H
#include <stdint.h>
typedef int32_t rtError_t;
#define RT_ERROR_NONE 0
#ifdef __cplusplus
extern "C" {
#endif
rtError_t rtGetSocVersion(char *version, const uint32_t maxLen);
#ifdef __cplusplus
}
#endif
#endif
]=])
foreach(dir
    "${ASCEND_DRIVER_PATH}/kernel/inc"
    "${ASCEND_HOME_PATH}/${ARCH}-linux/pkg_inc"
    "${ASCEND_HOME_PATH}/${ARCH}-linux/pkg_inc/runtime"
    "${ASCEND_HOME_PATH}/${ARCH}-linux/include"
)
    if(EXISTS "${dir}")
        list(APPEND TILEXR_EP_TEST_INCLUDE_DIRS "${dir}")
    endif()
endforeach()

find_path(TILEXR_EP_ACL_BASE_INCLUDE_DIR acl/acl_base.h
    PATHS ${TILEXR_EP_TEST_INCLUDE_DIRS}
    NO_DEFAULT_PATH
)
if(NOT TILEXR_EP_ACL_BASE_INCLUDE_DIR)
    set(TILEXR_EP_ACL_STUB_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated_acl_stub")
    file(MAKE_DIRECTORY "${TILEXR_EP_ACL_STUB_DIR}/acl")
    file(MAKE_DIRECTORY "${TILEXR_EP_ACL_STUB_DIR}/runtime")
    file(WRITE "${TILEXR_EP_ACL_STUB_DIR}/acl/acl_base.h" [=[
#ifndef TILEXR_EP_TEST_ACL_BASE_H
#define TILEXR_EP_TEST_ACL_BASE_H
typedef int aclError;
typedef void *aclrtStream;
typedef void *aclrtEvent;
#endif
]=])
    file(WRITE "${TILEXR_EP_ACL_STUB_DIR}/acl/acl_rt.h" [=[
#ifndef TILEXR_EP_TEST_ACL_RT_H
#define TILEXR_EP_TEST_ACL_RT_H
#include <stddef.h>
#include "acl_base.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef enum aclrtMemcpyKind {
    ACL_MEMCPY_HOST_TO_DEVICE = 1,
    ACL_MEMCPY_DEVICE_TO_DEVICE = 3
} aclrtMemcpyKind;
aclError aclrtStreamWaitEvent(aclrtStream stream, aclrtEvent event);
aclError aclrtMemcpy(void *dst, size_t destMax, const void *src,
    size_t count, aclrtMemcpyKind kind);
aclError aclrtMemcpyAsync(void *dst, size_t destMax, const void *src,
    size_t count, aclrtMemcpyKind kind, aclrtStream stream);
#ifdef __cplusplus
}
#endif
#endif
]=])
    file(WRITE "${TILEXR_EP_ACL_STUB_DIR}/runtime/kernel.h" [=[
#ifndef TILEXR_EP_TEST_RUNTIME_KERNEL_H
#define TILEXR_EP_TEST_RUNTIME_KERNEL_H
#include <stddef.h>
#include <stdint.h>
typedef int rtError_t;
typedef void *rtStream_t;
typedef struct rtArgsEx {
    void *args;
    size_t argsSize;
} rtArgsEx_t;
typedef struct rtTaskCfgInfo {
    int32_t schemMode;
} rtTaskCfgInfo_t;
typedef struct rtSmDesc rtSmDesc_t;
#define RT_ERROR_NONE 0
rtError_t rtKernelLaunchWithFlagV2(const void *func, uint32_t blockDim,
    rtArgsEx_t *argsInfo, rtSmDesc_t *smDesc, rtStream_t stream, uint32_t flags,
    const rtTaskCfgInfo_t *cfgInfo);
#endif
]=])
    list(APPEND TILEXR_EP_TEST_INCLUDE_DIRS "${TILEXR_EP_ACL_STUB_DIR}")
endif()

add_executable(test_tilexr_ep_layout
    unit/test_tilexr_ep_layout.cpp
    ${TILEXR_ROOT}/src/ep/host/ep_layout.cpp
    ${TILEXR_ROOT}/src/ep/host/ep_memory_layout.cpp
)

add_executable(test_tilexr_ep_expert_ids unit/test_tilexr_ep_expert_ids.cpp)

add_executable(test_tilexr_moonep_planner_public_abi unit/test_tilexr_moonep_planner_public_abi.cpp)

add_executable(test_tilexr_ep_plan_layout
    unit/test_tilexr_ep_plan_layout.cpp
    ${TILEXR_ROOT}/src/moonep/planner_v2/common/ep_plan_layout.cpp
)

add_executable(test_tilexr_ep_plan_reference
    unit/test_tilexr_ep_plan_reference.cpp
    ${TILEXR_ROOT}/src/moonep/planner_v2/reference/ep_plan_reference.cpp
)
add_executable(test_tilexr_ep_plan_algorithm
    unit/test_tilexr_ep_plan_algorithm.cpp
    ${TILEXR_ROOT}/src/moonep/planner_v2/common/ep_plan_algorithm.cpp
    ${TILEXR_ROOT}/src/moonep/planner_v2/reference/ep_plan_reference.cpp
)
add_executable(test_tilexr_ep_plan_downstream
    unit/test_tilexr_ep_plan_downstream.cpp
    ${TILEXR_ROOT}/src/moonep/planner_v2/common/ep_plan_downstream.cpp
    ${TILEXR_ROOT}/src/moonep/planner_v2/reference/ep_plan_reference.cpp
)

add_executable(test_tilexr_moonep_planner_compat
    unit/test_tilexr_moonep_planner_compat.cpp
    ${TILEXR_ROOT}/src/moonep/planner_v2/common/ep_plan_layout.cpp
    ${TILEXR_ROOT}/src/moonep/planner_v2/host/tilexr_moonep_planner.cpp
)

add_executable(test_tilexr_ep_plan_host
    unit/test_tilexr_ep_plan_host.cpp
    ${TILEXR_ROOT}/src/moonep/planner_v2/common/ep_plan_layout.cpp
    ${TILEXR_ROOT}/src/moonep/planner_v2/host/ep_plan_host.cpp
    ${TILEXR_ROOT}/src/moonep/planner_v2/host/planner_launch.cpp
    ${TILEXR_ROOT}/src/moonep/planner_v2/host/tilexr_moonep_planner.cpp
)

add_executable(test_tilexr_ep_host_validation
    unit/test_tilexr_ep_host_validation.cpp
    ${TILEXR_ROOT}/src/ep/host/ep_layout.cpp
    ${TILEXR_ROOT}/src/ep/host/ep_memory_layout.cpp
    ${TILEXR_ROOT}/src/ep/host/ep_dispatch_host.cpp
    ${TILEXR_ROOT}/src/ep/host/ep_dispatch_memory_host.cpp
    ${TILEXR_ROOT}/src/ep/host/ep_combine_memory_host.cpp
)

add_executable(test_tilexr_ep_kernel_launch
    unit/test_tilexr_ep_kernel_launch.cpp
    ${TILEXR_ROOT}/src/ep/host/ep_kernel_launch.cpp
    ${TILEXR_ROOT}/src/ep/host/ep_layout.cpp
    ${TILEXR_ROOT}/src/ep/host/ep_memory_layout.cpp
    ${TILEXR_ROOT}/src/ep/host/ep_dispatch_memory_host.cpp
    ${TILEXR_ROOT}/src/ep/host/ep_combine_memory_host.cpp
)

add_executable(test_tilexr_ep_memory_unsupported
    unit/test_tilexr_ep_memory_unsupported.cpp
    ${TILEXR_ROOT}/src/ep/host/tilexr_ep_memory_dispatch.cpp
    ${TILEXR_ROOT}/src/ep/host/tilexr_ep_memory_combine.cpp
)

target_include_directories(test_tilexr_ep_layout PRIVATE
    ${TILEXR_EP_TEST_INCLUDE_DIRS}
    ${CMAKE_CURRENT_SOURCE_DIR}/demo
)

target_include_directories(test_tilexr_ep_expert_ids PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/demo
)

target_include_directories(test_tilexr_ep_host_validation PRIVATE
    ${TILEXR_EP_TEST_INCLUDE_DIRS}
)

target_include_directories(test_tilexr_ep_kernel_launch BEFORE PRIVATE
    ${TILEXR_EP_FAKE_RUNTIME_INCLUDE_DIR}
    ${TILEXR_EP_TEST_INCLUDE_DIRS}
)

target_include_directories(test_tilexr_ep_memory_unsupported BEFORE PRIVATE
    ${TILEXR_EP_FAKE_RUNTIME_INCLUDE_DIR}
    ${TILEXR_EP_TEST_INCLUDE_DIRS}
)
target_compile_definitions(test_tilexr_ep_memory_unsupported PRIVATE
    TILEXR_EP_MEMORY_SUPPORTED=0)

target_include_directories(test_tilexr_ep_plan_layout PRIVATE
    ${TILEXR_EP_TEST_INCLUDE_DIRS}
)

target_include_directories(test_tilexr_ep_plan_reference PRIVATE
    ${TILEXR_EP_TEST_INCLUDE_DIRS}
    ${TILEXR_ROOT}/src/moonep/planner_v2/reference
)
target_include_directories(test_tilexr_ep_plan_algorithm PRIVATE
    ${TILEXR_EP_TEST_INCLUDE_DIRS}
    ${TILEXR_ROOT}/src/moonep/planner_v2/reference
)
target_include_directories(test_tilexr_ep_plan_downstream PRIVATE
    ${TILEXR_EP_TEST_INCLUDE_DIRS}
    ${TILEXR_ROOT}/src/moonep/planner_v2/reference
)
target_include_directories(test_tilexr_moonep_planner_public_abi PRIVATE ${TILEXR_EP_TEST_INCLUDE_DIRS})

target_include_directories(test_tilexr_moonep_planner_compat PRIVATE
    ${TILEXR_EP_TEST_INCLUDE_DIRS}
    ${TILEXR_ROOT}/src/moonep/planner_v2/host
)

target_include_directories(test_tilexr_ep_plan_host PRIVATE
    ${TILEXR_EP_TEST_INCLUDE_DIRS}
    ${TILEXR_ROOT}/src/moonep/planner_v2/host
)

add_test(NAME test_tilexr_ep_layout COMMAND test_tilexr_ep_layout)
add_test(NAME test_tilexr_ep_expert_ids COMMAND test_tilexr_ep_expert_ids)
add_test(NAME test_tilexr_ep_host_validation COMMAND test_tilexr_ep_host_validation)
add_test(NAME test_tilexr_ep_kernel_launch COMMAND test_tilexr_ep_kernel_launch)
add_test(NAME test_tilexr_ep_memory_unsupported COMMAND test_tilexr_ep_memory_unsupported)
add_test(NAME test_tilexr_moonep_planner_public_abi COMMAND test_tilexr_moonep_planner_public_abi)
add_test(NAME test_tilexr_ep_plan_layout COMMAND test_tilexr_ep_plan_layout)
add_test(NAME test_tilexr_ep_plan_reference COMMAND test_tilexr_ep_plan_reference)
add_test(NAME test_tilexr_ep_plan_algorithm COMMAND test_tilexr_ep_plan_algorithm)
add_test(NAME test_tilexr_ep_plan_downstream COMMAND test_tilexr_ep_plan_downstream)
add_test(NAME test_tilexr_ep_plan_host COMMAND test_tilexr_ep_plan_host)
add_test(NAME test_tilexr_moonep_planner_compat COMMAND test_tilexr_moonep_planner_compat)

install(TARGETS
    test_tilexr_ep_layout
    test_tilexr_ep_expert_ids
    test_tilexr_ep_host_validation
    test_tilexr_ep_kernel_launch
    test_tilexr_ep_memory_unsupported
    test_tilexr_ep_plan_layout
    test_tilexr_ep_plan_reference
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

if(BUILD_TILEXR_EP_DEMO)
    if(IS_ABSOLUTE "${TILEXR_INSTALL_LIBDIR}")
        set(TILEXR_INSTALL_LIB_SEARCH_DIR "${TILEXR_INSTALL_LIBDIR}")
    else()
        set(TILEXR_INSTALL_LIB_SEARCH_DIR "${TILEXR_INSTALL_PREFIX}/${TILEXR_INSTALL_LIBDIR}")
    endif()
    if(IS_ABSOLUTE "${TILEXR_INSTALL_INCLUDEDIR}")
        set(TILEXR_INSTALL_INCLUDE_SEARCH_DIR "${TILEXR_INSTALL_INCLUDEDIR}")
    else()
        set(TILEXR_INSTALL_INCLUDE_SEARCH_DIR "${TILEXR_INSTALL_PREFIX}/${TILEXR_INSTALL_INCLUDEDIR}")
    endif()

    find_library(TILEXR_EP_LIB tilexr-ep HINTS "${TILEXR_INSTALL_LIB_SEARCH_DIR}" REQUIRED)
    find_library(TILEXR_COMM_LIB tile-comm HINTS "${TILEXR_INSTALL_LIB_SEARCH_DIR}" REQUIRED)

    add_executable(tilexr_ep_dispatch_demo demo/tilexr_ep_dispatch_demo.cpp)

    target_include_directories(tilexr_ep_dispatch_demo PRIVATE
        "${TILEXR_INSTALL_INCLUDE_SEARCH_DIR}"
        "${TILEXR_ROOT}/src/ep/common"
        "${ASCEND_HOME_PATH}/${ARCH}-linux/include"
        "${ASCEND_HOME_PATH}/${ARCH}-linux/pkg_inc"
        "${ASCEND_HOME_PATH}/${ARCH}-linux/pkg_inc/runtime"
    )

    target_link_directories(tilexr_ep_dispatch_demo PRIVATE
        "${TILEXR_INSTALL_LIB_SEARCH_DIR}"
        "${ASCEND_HOME_PATH}/${ARCH}-linux/lib64"
        "${ASCEND_HOME_PATH}/${ARCH}-linux/devlib"
        "${ASCEND_DRIVER_PATH}/lib64/driver"
    )

    target_link_libraries(tilexr_ep_dispatch_demo PRIVATE
        ${TILEXR_EP_LIB}
        ${TILEXR_COMM_LIB}
        ascendcl
        runtime
        ascend_hal
    )

    # devlib is a link-time fallback only. An RPATH here can outrank the
    # runner's LD_LIBRARY_PATH and load the stub HAL instead of the driver HAL.
    set_target_properties(tilexr_ep_dispatch_demo PROPERTIES
        SKIP_BUILD_RPATH TRUE
        INSTALL_RPATH ""
    )

    install(TARGETS tilexr_ep_dispatch_demo RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

if(BUILD_TILEXR_EP_PLAN_MULTIRANK_TEST)
    if(IS_ABSOLUTE "${TILEXR_INSTALL_LIBDIR}")
        set(TILEXR_PLAN_MULTIRANK_LIB_SEARCH_DIR "${TILEXR_INSTALL_LIBDIR}")
    else()
        set(TILEXR_PLAN_MULTIRANK_LIB_SEARCH_DIR "${TILEXR_INSTALL_PREFIX}/${TILEXR_INSTALL_LIBDIR}")
    endif()
    if(IS_ABSOLUTE "${TILEXR_INSTALL_INCLUDEDIR}")
        set(TILEXR_PLAN_MULTIRANK_INCLUDE_SEARCH_DIR "${TILEXR_INSTALL_INCLUDEDIR}")
    else()
        set(TILEXR_PLAN_MULTIRANK_INCLUDE_SEARCH_DIR "${TILEXR_INSTALL_PREFIX}/${TILEXR_INSTALL_INCLUDEDIR}")
    endif()

    find_library(TILEXR_PLAN_MULTIRANK_PLANNER_LIB tilexr-moonep-planner
        HINTS "${TILEXR_PLAN_MULTIRANK_LIB_SEARCH_DIR}" REQUIRED)
    find_library(TILEXR_PLAN_MULTIRANK_COMM_LIB tile-comm
        HINTS "${TILEXR_PLAN_MULTIRANK_LIB_SEARCH_DIR}" REQUIRED)

    # This consumer intentionally compiles no Planner implementation source. It verifies
    # that the installed public header and production shared library export the four
    # downstream route helpers required by Dispatch and Combine.
    add_executable(test_tilexr_ep_plan_public_helper_consumer
        integration/test_tilexr_ep_plan_public_helper_consumer.cpp
    )
    target_include_directories(test_tilexr_ep_plan_public_helper_consumer PRIVATE
        "${TILEXR_PLAN_MULTIRANK_INCLUDE_SEARCH_DIR}"
        "${ASCEND_HOME_PATH}/${ARCH}-linux/include"
        "${ASCEND_HOME_PATH}/${ARCH}-linux/pkg_inc"
        "${ASCEND_HOME_PATH}/${ARCH}-linux/pkg_inc/runtime"
    )
    target_link_directories(test_tilexr_ep_plan_public_helper_consumer PRIVATE
        "${TILEXR_PLAN_MULTIRANK_LIB_SEARCH_DIR}"
        "${ASCEND_HOME_PATH}/${ARCH}-linux/lib64"
        "${ASCEND_DRIVER_PATH}/lib64/driver"
    )
    target_link_libraries(test_tilexr_ep_plan_public_helper_consumer PRIVATE
        ${TILEXR_PLAN_MULTIRANK_PLANNER_LIB}
    )
    set_target_properties(test_tilexr_ep_plan_public_helper_consumer PROPERTIES
        BUILD_RPATH "${TILEXR_PLAN_MULTIRANK_LIB_SEARCH_DIR};${ASCEND_HOME_PATH}/${ARCH}-linux/lib64;${ASCEND_DRIVER_PATH}/lib64/driver"
        INSTALL_RPATH "${TILEXR_PLAN_MULTIRANK_LIB_SEARCH_DIR};${ASCEND_HOME_PATH}/${ARCH}-linux/lib64;${ASCEND_DRIVER_PATH}/lib64/driver"
    )
    add_test(NAME test_tilexr_ep_plan_public_helper_consumer
        COMMAND test_tilexr_ep_plan_public_helper_consumer)

    add_executable(test_tilexr_ep_plan_multirank
        integration/test_tilexr_ep_plan_multirank.cpp
        ${TILEXR_ROOT}/src/moonep/planner_v2/common/ep_plan_layout.cpp
        ${TILEXR_ROOT}/src/moonep/planner_v2/common/ep_plan_downstream.cpp
        ${TILEXR_ROOT}/src/moonep/planner_v2/reference/ep_plan_reference.cpp
    )
    target_include_directories(test_tilexr_ep_plan_multirank PRIVATE
        ${TILEXR_EP_TEST_INCLUDE_DIRS}
        ${TILEXR_ROOT}/src/moonep/planner_v2/reference
        "${TILEXR_PLAN_MULTIRANK_INCLUDE_SEARCH_DIR}"
        "${ASCEND_HOME_PATH}/${ARCH}-linux/include"
        "${ASCEND_HOME_PATH}/${ARCH}-linux/pkg_inc"
        "${ASCEND_HOME_PATH}/${ARCH}-linux/pkg_inc/runtime"
    )
    target_link_directories(test_tilexr_ep_plan_multirank PRIVATE
        "${TILEXR_PLAN_MULTIRANK_LIB_SEARCH_DIR}"
        "${ASCEND_HOME_PATH}/${ARCH}-linux/lib64"
        "${ASCEND_DRIVER_PATH}/lib64/driver"
    )
    target_link_libraries(test_tilexr_ep_plan_multirank PRIVATE
        ${TILEXR_PLAN_MULTIRANK_PLANNER_LIB}
        ${TILEXR_PLAN_MULTIRANK_COMM_LIB}
        ascendcl
        runtime
        ascend_hal
    )
    set_target_properties(test_tilexr_ep_plan_multirank PROPERTIES
        BUILD_RPATH "${TILEXR_PLAN_MULTIRANK_LIB_SEARCH_DIR};${ASCEND_HOME_PATH}/${ARCH}-linux/lib64;${ASCEND_DRIVER_PATH}/lib64/driver"
        INSTALL_RPATH "${TILEXR_PLAN_MULTIRANK_LIB_SEARCH_DIR};${ASCEND_HOME_PATH}/${ARCH}-linux/lib64;${ASCEND_DRIVER_PATH}/lib64/driver"
    )
    install(TARGETS
        test_tilexr_ep_plan_public_helper_consumer
        test_tilexr_ep_plan_multirank
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()
