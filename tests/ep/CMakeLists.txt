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
set(TILEXR_EP_SERVER_DEDUP_ENABLE "1" CACHE STRING "Server dedup policy (auto, 0 or 1)")
set_property(CACHE TILEXR_EP_SERVER_DEDUP_ENABLE PROPERTY STRINGS auto 0 1)
if(TILEXR_EP_SERVER_DEDUP_ENABLE STREQUAL "auto")
    set(TILEXR_EP_SERVER_DEDUP_LAYOUT_ENABLE 1)
    set(TILEXR_EP_SERVER_DEDUP_AUTO_ENABLE 1)
elseif(TILEXR_EP_SERVER_DEDUP_ENABLE MATCHES "^[01]$")
    set(TILEXR_EP_SERVER_DEDUP_LAYOUT_ENABLE ${TILEXR_EP_SERVER_DEDUP_ENABLE})
    set(TILEXR_EP_SERVER_DEDUP_AUTO_ENABLE 0)
else()
    message(FATAL_ERROR "TILEXR_EP_SERVER_DEDUP_ENABLE must be auto, 0 or 1")
endif()
add_compile_definitions(
    TILEXR_EP_SERVER_DEDUP_ENABLE=$<IF:$<OR:$<BOOL:$<TARGET_PROPERTY:TILEXR_TEST_DEDUP_ONLY>>,$<BOOL:$<TARGET_PROPERTY:TILEXR_TEST_DEDUP_AUTO>>>,1,${TILEXR_EP_SERVER_DEDUP_LAYOUT_ENABLE}>
    TILEXR_EP_SERVER_DEDUP_AUTO_ENABLE=$<IF:$<BOOL:$<TARGET_PROPERTY:TILEXR_TEST_DEDUP_ONLY>>,0,$<IF:$<BOOL:$<TARGET_PROPERTY:TILEXR_TEST_DEDUP_AUTO>>,1,${TILEXR_EP_SERVER_DEDUP_AUTO_ENABLE}>>)

option(BUILD_TILEXR_EP_DEMO "Build TileXR EP dispatch hardware demo" OFF)
option(BUILD_TILEXR_EP_PLAN_MULTIRANK_TEST "Build the MoonEP Plan multi-rank peer-memory hardware test" OFF)
option(BUILD_TILEXR_EP_SERVER_DEDUP_COMBINE_REGISTRATION_TEST "Build installed A5 Combine empty-kernel consumer" OFF)
option(BUILD_TILEXR_EP_SERVER_DEDUP_REGISTRATION_TEST "Build installed A5 server dedup registration consumer" OFF)
option(BUILD_TILEXR_EP_SERVER_DEDUP_CHAIN_TEST "Build installed A5 server dedup chain consumer" OFF)
option(BUILD_TILEXR_EP_MEMORY_BOUNDARY_TEST "Enable the installed 4 GiB boundary test in the chain consumer" OFF)
option(BUILD_TILEXR_EP_COMBINE_MEMORY_BOUNDARY_TEST "Enable the installed 8 GiB Combine boundary probe" OFF)

set(TILEXR_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../..")
include(${TILEXR_ROOT}/src/cmake/TileXREpCoreConfig.cmake)
add_compile_definitions(${TILEXR_EP_CORE_DEFINITIONS})
set(TILEXR_INSTALL_PREFIX "${TILEXR_ROOT}/install" CACHE PATH "TileXR install prefix")
set(TILEXR_INSTALL_LIBDIR "${CMAKE_INSTALL_LIBDIR}" CACHE STRING "TileXR install library directory")
set(TILEXR_INSTALL_INCLUDEDIR "${CMAKE_INSTALL_INCLUDEDIR}" CACHE STRING "TileXR install include directory")

set(_tilexr_default_ascend_home_path "$ENV{ASCEND_HOME_PATH}")
if(NOT _tilexr_default_ascend_home_path)
    set(_tilexr_default_ascend_home_path "/usr/local/Ascend/ascend-toolkit/latest")
endif()

find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
    add_test(NAME test_tilexr_ep_server_dedup_combine_config
        COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_server_dedup_combine_config.py")
    set_tests_properties(test_tilexr_ep_server_dedup_combine_config PROPERTIES TIMEOUT 120)
    add_test(NAME test_dispatch_memory_trace
        COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/python/test_dispatch_memory_trace.py")
    add_test(NAME test_msprof_kernel_stats
        COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/python/test_msprof_kernel_stats.py")
    add_test(NAME test_server_dedup_address_space
        COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/python/test_server_dedup_address_space.py")
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
    ${TILEXR_ROOT}/src/common
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
typedef enum aclrtMemMallocPolicy {
    ACL_MEM_MALLOC_HUGE_FIRST = 0, ACL_MEM_MALLOC_NORMAL_ONLY = 2
} aclrtMemMallocPolicy;
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
const char *aclrtGetSocName(void);
aclError aclrtSynchronizeStream(aclrtStream stream);
aclError aclrtMalloc(void **devPtr, size_t size, aclrtMemMallocPolicy policy);
aclError aclrtFree(void *devPtr);
aclError aclrtMemcpy(void *dst, size_t destMax, const void *src,
    size_t count, aclrtMemcpyKind kind);
#ifdef __cplusplus
}
#endif
#endif
]=])
# The C3 target compiles the existing consumer against observable Host substitutes.
file(WRITE "${TILEXR_EP_FAKE_RUNTIME_INCLUDE_DIR}/acl/acl.h" [=[
#ifndef TILEXR_EP_FAKE_ACL_H
#define TILEXR_EP_FAKE_ACL_H
#include "acl_rt.h"
#ifdef __cplusplus
extern "C" {
#endif
aclError aclInit(const char *);
aclError aclFinalize(void);
aclError aclrtSetDevice(int32_t);
aclError aclrtResetDevice(int32_t);
aclError aclrtCreateStream(aclrtStream *);
aclError aclrtDestroyStream(aclrtStream);
aclError aclrtMemset(void *, size_t, int32_t, size_t);
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
typedef struct rtHostInputInfo {
    uint32_t addrOffset;
    uint32_t dataOffset;
} rtHostInputInfo_t;
typedef struct rtArgsEx {
    void *args;
    rtHostInputInfo_t *hostInputInfoPtr;
    uint32_t argsSize;
    uint32_t tilingAddrOffset;
    uint32_t tilingDataOffset;
    uint16_t hostInputInfoNum;
    uint8_t hasTiling;
    uint8_t isNoNeedH2DCopy;
    uint8_t reserved[4];
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
rtError_t rtDevBinaryUnRegister(void *handle);
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

set(TILEXR_EP_SERVER_DEDUP_HOST_SOURCES
    ${TILEXR_ROOT}/src/ep/host/ep_memory_server_dedup_layout.cpp
    ${TILEXR_ROOT}/src/ep/host/ep_memory_server_dedup_host.cpp
    ${TILEXR_ROOT}/src/ep/host/tilexr_ep_memory_server_dedup_dispatch.cpp)
add_executable(test_tilexr_ep_server_dedup_protocol unit/test_tilexr_ep_server_dedup_protocol.cpp)
add_executable(test_tilexr_ep_server_dedup_reference unit/test_tilexr_ep_server_dedup_reference.cpp)
target_include_directories(test_tilexr_ep_server_dedup_reference PRIVATE ${TILEXR_EP_TEST_INCLUDE_DIRS})
add_test(NAME test_tilexr_ep_server_dedup_reference COMMAND test_tilexr_ep_server_dedup_reference)
set_tests_properties(test_tilexr_ep_server_dedup_reference PROPERTIES LABELS ep-sdedup-reference TIMEOUT 120)
add_executable(test_tilexr_ep_server_dedup_layout
    unit/test_tilexr_ep_server_dedup_layout.cpp
    ${TILEXR_ROOT}/src/ep/host/ep_memory_server_dedup_layout.cpp)
add_executable(test_tilexr_ep_server_dedup_host_validation
    unit/test_tilexr_ep_server_dedup_host_validation.cpp
    ${TILEXR_EP_SERVER_DEDUP_HOST_SOURCES})
add_executable(test_tilexr_ep_server_dedup_host_profile_validation
    unit/test_tilexr_ep_server_dedup_host_validation.cpp
    ${TILEXR_EP_SERVER_DEDUP_HOST_SOURCES})
target_compile_definitions(test_tilexr_ep_server_dedup_host_profile_validation PRIVATE TILEXR_EP_SERVER_DEDUP_ENABLE_PROFILING=1)
add_executable(test_tilexr_ep_server_dedup_public_abi
    unit/test_tilexr_ep_server_dedup_public_abi.cpp
    ${TILEXR_EP_SERVER_DEDUP_HOST_SOURCES})
foreach(test_name IN ITEMS protocol layout host_validation host_profile_validation public_abi)
    set(test_target test_tilexr_ep_server_dedup_${test_name})
    target_include_directories(${test_target} PRIVATE ${TILEXR_EP_TEST_INCLUDE_DIRS})
    add_test(NAME ${test_target} COMMAND ${test_target})
    set_tests_properties(${test_target} PROPERTIES LABELS ep-sdedup-p1-abi-host TIMEOUT 120)
    install(TARGETS ${test_target} RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endforeach()

# Combine public ABI and Host validation replace launch; C2 separately tests
# the production launcher and installed A5 artifact.
set(TILEXR_EP_SERVER_DEDUP_COMBINE_HOST_SOURCES
    ${TILEXR_ROOT}/src/ep/host/ep_memory_server_dedup_combine_layout.cpp
    ${TILEXR_ROOT}/src/ep/host/ep_memory_server_dedup_combine_host.cpp
    ${TILEXR_ROOT}/src/ep/host/tilexr_ep_memory_server_dedup_combine.cpp)
add_executable(test_tilexr_ep_server_dedup_combine_layout
    unit/test_tilexr_ep_server_dedup_combine_layout.cpp
    ${TILEXR_ROOT}/src/ep/host/ep_memory_server_dedup_combine_layout.cpp)
add_executable(test_tilexr_ep_server_dedup_combine_host_validation
    unit/test_tilexr_ep_server_dedup_combine_host_validation.cpp
    ${TILEXR_EP_SERVER_DEDUP_COMBINE_HOST_SOURCES})
add_executable(test_tilexr_ep_server_dedup_combine_public_abi
    unit/test_tilexr_ep_server_dedup_combine_public_abi.cpp
    ${TILEXR_EP_SERVER_DEDUP_COMBINE_HOST_SOURCES})
foreach(test_name IN ITEMS layout host_validation public_abi)
    set(test_target test_tilexr_ep_server_dedup_combine_${test_name})
    target_include_directories(${test_target} PRIVATE ${TILEXR_EP_TEST_INCLUDE_DIRS})
    add_test(NAME ${test_target} COMMAND ${test_target})
    set_tests_properties(${test_target} PROPERTIES LABELS ep-sdedup-combine-p1-abi-host TIMEOUT 120)
    install(TARGETS ${test_target} RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endforeach()

# C3 uses the same source and production layout validation as the device consumer.
# Runtime substitutes record every call; the dry-run test requires no such calls.
find_package(Threads REQUIRED)
add_executable(test_tilexr_ep_server_dedup_chain_dry_run
    integration/test_tilexr_ep_server_dedup_chain.cpp
    integration/server_dedup_chain_stub.cpp
    ${TILEXR_ROOT}/src/comm/tilexr_profiling_runtime.cpp
    ${TILEXR_ROOT}/src/ep/host/ep_memory_server_dedup_layout.cpp
    ${TILEXR_ROOT}/src/ep/host/ep_memory_server_dedup_combine_layout.cpp
    ${TILEXR_ROOT}/src/ep/host/ep_memory_layout.cpp
    ${TILEXR_ROOT}/src/ep/host/ep_layout.cpp)
target_include_directories(test_tilexr_ep_server_dedup_chain_dry_run PRIVATE
    ${TILEXR_EP_FAKE_RUNTIME_INCLUDE_DIR} ${TILEXR_EP_TEST_INCLUDE_DIRS} ${TILEXR_ROOT}/src/comm)
target_link_libraries(test_tilexr_ep_server_dedup_chain_dry_run PRIVATE Threads::Threads)
# Global-route and numeric oracles run on the CPU even in performance mode.
# Keep them optimized under the workflow's Debug build, with strict FP order.
target_compile_options(test_tilexr_ep_server_dedup_chain_dry_run PRIVATE -O2 -fno-fast-math -ffp-contract=off)
target_compile_definitions(test_tilexr_ep_server_dedup_chain_dry_run PRIVATE TILEXR_EP_COMBINE_MEMORY_BOUNDARY_TEST=1)
install(TARGETS test_tilexr_ep_server_dedup_chain_dry_run RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
if(Python3_Interpreter_FOUND)
    add_test(NAME test_tilexr_ep_server_dedup_combine_dry_run
        COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/integration/test_server_dedup_combine_dry_run.py"
            --consumer $<TARGET_FILE:test_tilexr_ep_server_dedup_chain_dry_run>)
    set_tests_properties(test_tilexr_ep_server_dedup_combine_dry_run PROPERTIES
        LABELS ep-sdedup-combine-p1-dry-run TIMEOUT 120)
endif()

# C4 reuses the same Host consumer for lifecycle/fault-injection coverage.
if(Python3_Interpreter_FOUND)
    add_test(NAME test_tilexr_ep_server_dedup_combine_m5_consumer
        COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/integration/test_server_dedup_combine_m5.py"
            --consumer $<TARGET_FILE:test_tilexr_ep_server_dedup_chain_dry_run>)
    set_tests_properties(test_tilexr_ep_server_dedup_combine_m5_consumer PROPERTIES
        LABELS ep-sdedup-combine-p2-m5 TIMEOUT 240)
    add_test(NAME test_tilexr_ep_server_dedup_combine_m4_consumer
        COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/integration/test_server_dedup_combine_m4.py"
            --consumer $<TARGET_FILE:test_tilexr_ep_server_dedup_chain_dry_run>)
    set_tests_properties(test_tilexr_ep_server_dedup_combine_m4_consumer PROPERTIES
        LABELS ep-sdedup-combine-p2-m4 TIMEOUT 180)
    add_test(NAME test_tilexr_ep_server_dedup_combine_m3_consumer
        COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/integration/test_server_dedup_combine_m3.py"
            --consumer $<TARGET_FILE:test_tilexr_ep_server_dedup_chain_dry_run>)
    set_tests_properties(test_tilexr_ep_server_dedup_combine_m3_consumer PROPERTIES
        LABELS ep-sdedup-combine-p2-m3 TIMEOUT 180)
    add_test(NAME test_tilexr_ep_server_dedup_combine_m12_consumer
        COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/integration/test_server_dedup_combine_m12.py"
            --consumer $<TARGET_FILE:test_tilexr_ep_server_dedup_chain_dry_run>)
    set_tests_properties(test_tilexr_ep_server_dedup_combine_m12_consumer PROPERTIES
        LABELS ep-sdedup-combine-p2-m12 TIMEOUT 180)
    add_test(NAME test_tilexr_ep_server_dedup_combine_chain
        COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/integration/test_server_dedup_combine_chain.py"
            --consumer $<TARGET_FILE:test_tilexr_ep_server_dedup_chain_dry_run>)
    set_tests_properties(test_tilexr_ep_server_dedup_combine_chain PROPERTIES
        LABELS ep-sdedup-combine-p1-chain-single TIMEOUT 120)
endif()
# C5 adds continuous batch and shared-magic lifecycle coverage to that consumer.
if(Python3_Interpreter_FOUND)
    add_test(NAME test_tilexr_ep_server_dedup_combine_multi
        COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/integration/test_server_dedup_combine_multi.py"
            --consumer $<TARGET_FILE:test_tilexr_ep_server_dedup_chain_dry_run>)
    set_tests_properties(test_tilexr_ep_server_dedup_combine_multi PROPERTIES
        LABELS ep-sdedup-combine-p1-chain-multi TIMEOUT 120)
endif()
# C7 keeps the boundary probe separate from the business-operator batch.
if(Python3_Interpreter_FOUND)
    add_test(NAME test_tilexr_ep_server_dedup_combine_boundary_consumer
        COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/integration/test_server_dedup_combine_boundary.py"
            --consumer $<TARGET_FILE:test_tilexr_ep_server_dedup_chain_dry_run>)
    set_tests_properties(test_tilexr_ep_server_dedup_combine_boundary_consumer PROPERTIES
        LABELS ep-sdedup-combine-p1-memory-boundary TIMEOUT 120)
endif()
add_executable(test_tilexr_ep_server_dedup_combine_boundary_launch
    unit/test_tilexr_ep_server_dedup_combine_boundary_launch.cpp
    integration/combine_memory_boundary_launch.cpp)
target_include_directories(test_tilexr_ep_server_dedup_combine_boundary_launch PRIVATE
    ${TILEXR_EP_FAKE_RUNTIME_INCLUDE_DIR} ${TILEXR_EP_TEST_INCLUDE_DIRS} ${TILEXR_ROOT}/src/common)
target_link_libraries(test_tilexr_ep_server_dedup_combine_boundary_launch PRIVATE ${CMAKE_DL_LIBS} Threads::Threads)
add_executable(test_tilexr_ep_server_dedup_combine_boundary_kernel
    unit/test_tilexr_ep_server_dedup_combine_boundary_kernel.cpp)
target_include_directories(test_tilexr_ep_server_dedup_combine_boundary_kernel BEFORE PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/unit/combine_memory_boundary_fake"
    ${TILEXR_ROOT}/src/include ${TILEXR_ROOT}/src/ep/common)
foreach(test_name IN ITEMS launch kernel)
    set(test_target test_tilexr_ep_server_dedup_combine_boundary_${test_name})
    add_test(NAME ${test_target} COMMAND ${test_target})
    set_tests_properties(${test_target} PROPERTIES LABELS ep-sdedup-combine-p1-memory-boundary TIMEOUT 30)
    install(TARGETS ${test_target} RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endforeach()
add_executable(test_tilexr_ep_server_dedup_combine_delay
    unit/test_tilexr_ep_server_dedup_combine_delay.cpp
    ${TILEXR_ROOT}/src/ep/kernels/tilexr_ep_combine_memory_server_dedup_kernel.cpp)
target_include_directories(test_tilexr_ep_server_dedup_combine_delay BEFORE PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/unit/server_dedup_combine_delay_fake"
    "${TILEXR_ROOT}/src/include"
    "${TILEXR_ROOT}/src/ep/common")
add_test(NAME test_tilexr_ep_server_dedup_combine_delay
    COMMAND test_tilexr_ep_server_dedup_combine_delay)
set_tests_properties(test_tilexr_ep_server_dedup_combine_delay PROPERTIES TIMEOUT 30)
install(TARGETS test_tilexr_ep_server_dedup_combine_delay RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

# C6 reuses the consumer and separately validates production profile regions.
if(Python3_Interpreter_FOUND)
    add_test(NAME test_tilexr_ep_server_dedup_combine_profile
        COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/integration/test_server_dedup_combine_profile.py"
            --consumer $<TARGET_FILE:test_tilexr_ep_server_dedup_chain_dry_run>)
    set_tests_properties(test_tilexr_ep_server_dedup_combine_profile PROPERTIES
        LABELS ep-sdedup-combine-p1-profile TIMEOUT 120)
endif()
add_executable(test_tilexr_ep_server_dedup_combine_profile_regions
    unit/test_tilexr_ep_server_dedup_combine_profile_regions.cpp
    ${TILEXR_ROOT}/src/comm/tilexr_profiling_runtime.cpp)
target_include_directories(test_tilexr_ep_server_dedup_combine_profile_regions BEFORE PRIVATE
    "${TILEXR_ROOT}/tests/profiling/unit/stubs" "${TILEXR_ROOT}/src/comm" ${TILEXR_EP_TEST_INCLUDE_DIRS})
target_compile_definitions(test_tilexr_ep_server_dedup_combine_profile_regions PRIVATE
    __CCE__=1 __CCE_IS_AICORE__=1 TILEXR_ENABLE_PROFILING=1)
target_link_libraries(test_tilexr_ep_server_dedup_combine_profile_regions PRIVATE Threads::Threads)
add_test(NAME test_tilexr_ep_server_dedup_combine_profile_regions COMMAND test_tilexr_ep_server_dedup_combine_profile_regions)
set_tests_properties(test_tilexr_ep_server_dedup_combine_profile_regions PROPERTIES TIMEOUT 120)
install(TARGETS test_tilexr_ep_server_dedup_combine_profile_regions RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

add_executable(test_tilexr_ep_server_dedup_combine_profile_kernel
    unit/test_tilexr_ep_server_dedup_combine_profile_kernel.cpp)
target_include_directories(test_tilexr_ep_server_dedup_combine_profile_kernel BEFORE PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/unit/server_dedup_combine_delay_fake" ${TILEXR_EP_TEST_INCLUDE_DIRS})
target_compile_definitions(test_tilexr_ep_server_dedup_combine_profile_kernel PRIVATE
    __CCE__=1 __CCE_IS_AICORE__=1 TILEXR_ENABLE_PROFILING=1)
add_test(NAME test_tilexr_ep_server_dedup_combine_profile_kernel COMMAND test_tilexr_ep_server_dedup_combine_profile_kernel)
set_tests_properties(test_tilexr_ep_server_dedup_combine_profile_kernel PROPERTIES TIMEOUT 120)
install(TARGETS test_tilexr_ep_server_dedup_combine_profile_kernel RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

add_executable(test_tilexr_ep_server_dedup_combine_m12
    unit/test_tilexr_ep_server_dedup_combine_m12.cpp)
target_include_directories(test_tilexr_ep_server_dedup_combine_m12 BEFORE PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/unit/server_dedup_combine_delay_fake"
    "${TILEXR_ROOT}/src/include" "${TILEXR_ROOT}/src/ep/common")
add_test(NAME test_tilexr_ep_server_dedup_combine_m12 COMMAND test_tilexr_ep_server_dedup_combine_m12)
set_tests_properties(test_tilexr_ep_server_dedup_combine_m12 PROPERTIES LABELS ep-sdedup-combine-p2-m12 TIMEOUT 120)
install(TARGETS test_tilexr_ep_server_dedup_combine_m12 RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

add_executable(test_tilexr_ep_server_dedup_combine_m3
    unit/test_tilexr_ep_server_dedup_combine_m3.cpp)
target_include_directories(test_tilexr_ep_server_dedup_combine_m3 BEFORE PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/unit/server_dedup_combine_delay_fake"
    "${TILEXR_ROOT}/src/include" "${TILEXR_ROOT}/src/ep/common")
add_test(NAME test_tilexr_ep_server_dedup_combine_m3 COMMAND test_tilexr_ep_server_dedup_combine_m3)
set_tests_properties(test_tilexr_ep_server_dedup_combine_m3 PROPERTIES LABELS ep-sdedup-combine-p2-m3 TIMEOUT 120)
install(TARGETS test_tilexr_ep_server_dedup_combine_m3 RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

add_executable(test_tilexr_ep_server_dedup_combine_m4
    unit/test_tilexr_ep_server_dedup_combine_m4.cpp)
target_include_directories(test_tilexr_ep_server_dedup_combine_m4 BEFORE PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/unit/server_dedup_combine_delay_fake"
    "${TILEXR_ROOT}/src/include" "${TILEXR_ROOT}/src/ep/common")
add_test(NAME test_tilexr_ep_server_dedup_combine_m4 COMMAND test_tilexr_ep_server_dedup_combine_m4)
set_tests_properties(test_tilexr_ep_server_dedup_combine_m4 PROPERTIES LABELS ep-sdedup-combine-p2-m4 TIMEOUT 120)
install(TARGETS test_tilexr_ep_server_dedup_combine_m4 RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

add_executable(test_tilexr_ep_server_dedup_combine_m5
    unit/test_tilexr_ep_server_dedup_combine_m5.cpp)
target_include_directories(test_tilexr_ep_server_dedup_combine_m5 BEFORE PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/unit/server_dedup_combine_delay_fake"
    "${TILEXR_ROOT}/src/include" "${TILEXR_ROOT}/src/ep/common")
add_test(NAME test_tilexr_ep_server_dedup_combine_m5 COMMAND test_tilexr_ep_server_dedup_combine_m5)
set_tests_properties(test_tilexr_ep_server_dedup_combine_m5 PROPERTIES LABELS ep-sdedup-combine-p2-m5 TIMEOUT 180)
install(TARGETS test_tilexr_ep_server_dedup_combine_m5 RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

# Unequal groups catch an M5 owner/UB budget still tied to the Read core count.
foreach(_m5_send_cores IN ITEMS 16 28 48 52)
    set(_m5_split_target test_tilexr_ep_server_dedup_combine_m5_send${_m5_send_cores})
    add_executable(${_m5_split_target} unit/test_tilexr_ep_server_dedup_combine_m5.cpp)
    target_include_directories(${_m5_split_target} BEFORE PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/unit/server_dedup_combine_delay_fake" ${TILEXR_EP_TEST_INCLUDE_DIRS})
    target_compile_definitions(${_m5_split_target} PRIVATE
        $<$<NOT:$<BOOL:${TILEXR_EP_SERVER_DEDUP_COMBINE_SEND_CORE_NUM}>>:TILEXR_EP_SERVER_DEDUP_COMBINE_SEND_CORE_NUM=${_m5_send_cores}>)
    set_property(TARGET ${_m5_split_target} PROPERTY TILEXR_TEST_COMBINE_SEND_CORES ${_m5_send_cores})
    add_test(NAME ${_m5_split_target} COMMAND ${_m5_split_target})
    set_tests_properties(${_m5_split_target} PROPERTIES LABELS ep-sdedup-combine-p2-m5 TIMEOUT 180)
endforeach()

# Runs the production count/cumsum methods against synchronous CPU API substitutes.
# This tests arithmetic, strided addresses and cleanup, not CANN or device ordering.
foreach(test_target test_tilexr_ep_server_dedup_counts test_tilexr_ep_server_dedup_counts_profile
        test_tilexr_ep_server_dedup_counts_send20 test_tilexr_ep_server_dedup_counts_send28
        test_tilexr_ep_server_dedup_counts_send36 test_tilexr_ep_server_dedup_counts_send44)
    add_executable(${test_target} unit/test_tilexr_ep_server_dedup_counts.cpp)
    target_include_directories(${test_target} PRIVATE
        unit/server_dedup_count_fake ${TILEXR_EP_TEST_INCLUDE_DIRS}
        ${TILEXR_ROOT}/src/ep/kernels)
    target_compile_definitions(${test_target} PRIVATE __CCE__=1 __CCE_IS_AICORE__=1)
    if(test_target MATCHES "_send([0-9]+)$")
        set(_count_send_cores ${CMAKE_MATCH_1})
        target_compile_definitions(${test_target} PRIVATE
            $<$<NOT:$<BOOL:${TILEXR_EP_SERVER_DEDUP_SEND_CORE_NUM}>>:TILEXR_EP_SERVER_DEDUP_SEND_CORE_NUM=${_count_send_cores}>)
        set_property(TARGET ${test_target} PROPERTY TILEXR_TEST_DISPATCH_SEND_CORES ${_count_send_cores})
    endif()
    add_test(NAME ${test_target} COMMAND ${test_target})
    # Includes empty/uneven count segments and both sides of the BS512 threshold.
    set_tests_properties(${test_target} PROPERTIES TIMEOUT 240)
    install(TARGETS ${test_target} RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endforeach()
target_compile_definitions(test_tilexr_ep_server_dedup_counts_profile PRIVATE TILEXR_ENABLE_PROFILING=1)

add_executable(test_tilexr_ep_server_dedup_profile unit/test_tilexr_ep_server_dedup_profile.cpp)
target_include_directories(test_tilexr_ep_server_dedup_profile PRIVATE
    unit/server_dedup_count_fake ${TILEXR_EP_TEST_INCLUDE_DIRS} ${TILEXR_ROOT}/src/ep/kernels)
target_compile_definitions(test_tilexr_ep_server_dedup_profile PRIVATE __CCE__=1 __CCE_IS_AICORE__=1 TILEXR_ENABLE_PROFILING=1)
add_test(NAME test_tilexr_ep_server_dedup_profile COMMAND test_tilexr_ep_server_dedup_profile)
set_tests_properties(test_tilexr_ep_server_dedup_profile PROPERTIES TIMEOUT 30)
install(TARGETS test_tilexr_ep_server_dedup_profile RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

foreach(test_target test_tilexr_ep_server_dedup_source_send test_tilexr_ep_server_dedup_source_send_profile test_tilexr_ep_server_dedup_source_send_auto)
    add_executable(${test_target} unit/test_tilexr_ep_server_dedup_source_send.cpp
        ${CMAKE_CURRENT_LIST_DIR}/../../src/ep/kernels/tilexr_ep_dispatch_memory_server_dedup_kernel.cpp)
    target_include_directories(${test_target} PRIVATE
        unit/server_dedup_count_fake ${TILEXR_EP_TEST_INCLUDE_DIRS} ${TILEXR_ROOT}/src/ep/kernels)
    target_compile_definitions(${test_target} PRIVATE __CCE__=1 __CCE_IS_AICORE__=1)
    set_target_properties(${test_target} PROPERTIES CXX_STANDARD 17)
    add_test(NAME ${test_target} COMMAND ${test_target})
    # Five core splits plus runtime mode/window reuse take about 118s on blue.
    set_tests_properties(${test_target} PROPERTIES TIMEOUT 180)
    install(TARGETS ${test_target} RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endforeach()
target_compile_definitions(test_tilexr_ep_server_dedup_source_send_profile PRIVATE __CCE__=1 __CCE_IS_AICORE__=1 TILEXR_ENABLE_PROFILING=1)

foreach(test_target test_tilexr_ep_server_dedup_receive_packet test_tilexr_ep_server_dedup_receive_profile test_tilexr_ep_server_dedup_receive_packet_auto)
    add_executable(${test_target} unit/test_tilexr_ep_server_dedup_receive_packet.cpp)
    target_include_directories(${test_target} PRIVATE
        unit/server_dedup_count_fake ${TILEXR_EP_TEST_INCLUDE_DIRS} ${TILEXR_ROOT}/src/ep/kernels)
    target_compile_definitions(${test_target} PRIVATE __CCE__=1 __CCE_IS_AICORE__=1)
    set_target_properties(${test_target} PROPERTIES CXX_STANDARD 17)
    add_test(NAME ${test_target} COMMAND ${test_target})
    # Each executable now exercises all five runtime core splits.
    set_tests_properties(${test_target} PROPERTIES TIMEOUT 150)
    install(TARGETS ${test_target} RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endforeach()
target_compile_definitions(test_tilexr_ep_server_dedup_receive_profile PRIVATE __CCE__=1 __CCE_IS_AICORE__=1 TILEXR_ENABLE_PROFILING=1)

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
    ${TILEXR_ROOT}/src/ep/host/ep_memory_server_dedup_launch.cpp
    ${TILEXR_ROOT}/src/ep/host/ep_memory_server_dedup_combine_launch.cpp
    ${TILEXR_ROOT}/src/ep/host/ep_layout.cpp
    ${TILEXR_ROOT}/src/ep/host/ep_memory_layout.cpp
    ${TILEXR_ROOT}/src/ep/host/ep_dispatch_memory_host.cpp
    ${TILEXR_ROOT}/src/ep/host/ep_combine_memory_host.cpp
)

add_executable(test_tilexr_ep_server_dedup_combine_launch
    $<TARGET_PROPERTY:test_tilexr_ep_kernel_launch,SOURCES>)
target_include_directories(test_tilexr_ep_server_dedup_combine_launch BEFORE PRIVATE
    ${TILEXR_EP_FAKE_RUNTIME_INCLUDE_DIR} ${TILEXR_EP_TEST_INCLUDE_DIRS})
target_compile_definitions(test_tilexr_ep_server_dedup_combine_launch PRIVATE TILEXR_EP_TEST_COMBINE_C2=1)
add_test(NAME test_tilexr_ep_server_dedup_combine_launch COMMAND test_tilexr_ep_server_dedup_combine_launch)
set_tests_properties(test_tilexr_ep_server_dedup_combine_launch PROPERTIES
    LABELS ep-sdedup-combine-p1-empty-kernel TIMEOUT 120)
install(TARGETS test_tilexr_ep_server_dedup_combine_launch RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

add_executable(test_tilexr_ep_server_dedup_combine_profile_launch
    $<TARGET_PROPERTY:test_tilexr_ep_kernel_launch,SOURCES>)
target_include_directories(test_tilexr_ep_server_dedup_combine_profile_launch BEFORE PRIVATE
    ${TILEXR_EP_FAKE_RUNTIME_INCLUDE_DIR} ${TILEXR_EP_TEST_INCLUDE_DIRS})
target_compile_definitions(test_tilexr_ep_server_dedup_combine_profile_launch PRIVATE
    TILEXR_EP_TEST_COMBINE_C2=1 TILEXR_EP_SERVER_DEDUP_ENABLE_PROFILING=1)
add_test(NAME test_tilexr_ep_server_dedup_combine_profile_launch COMMAND test_tilexr_ep_server_dedup_combine_profile_launch)
set_tests_properties(test_tilexr_ep_server_dedup_combine_profile_launch PROPERTIES TIMEOUT 120)
install(TARGETS test_tilexr_ep_server_dedup_combine_profile_launch RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

add_executable(test_tilexr_ep_kernel_launch_profile
    $<TARGET_PROPERTY:test_tilexr_ep_kernel_launch,SOURCES>)
target_include_directories(test_tilexr_ep_kernel_launch_profile BEFORE PRIVATE
    ${TILEXR_EP_FAKE_RUNTIME_INCLUDE_DIR} ${TILEXR_EP_TEST_INCLUDE_DIRS})
target_compile_definitions(test_tilexr_ep_kernel_launch_profile PRIVATE
    TILEXR_EP_SERVER_DEDUP_ENABLE_PROFILING=1)
add_test(NAME test_tilexr_ep_kernel_launch_profile COMMAND test_tilexr_ep_kernel_launch_profile)
install(TARGETS test_tilexr_ep_kernel_launch_profile RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

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
    test_tilexr_ep_plan_layout
    test_tilexr_ep_plan_reference
    test_tilexr_ep_plan_algorithm
    test_tilexr_ep_plan_downstream
    test_tilexr_ep_plan_host
    test_tilexr_moonep_planner_public_abi
    test_tilexr_moonep_planner_compat
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

if(BUILD_TILEXR_EP_SERVER_DEDUP_REGISTRATION_TEST)
    # 安装包 consumer 不编译 Host 实现，不引入 fake runtime 头文件。
    find_library(TILEXR_EP_SERVER_DEDUP_INSTALLED_LIB tilexr-ep
        PATHS "${TILEXR_INSTALL_PREFIX}/${TILEXR_INSTALL_LIBDIR}" NO_DEFAULT_PATH REQUIRED)
    add_executable(test_tilexr_ep_server_dedup_registration
        integration/test_tilexr_ep_server_dedup_registration.cpp)
    target_include_directories(test_tilexr_ep_server_dedup_registration PRIVATE
        "${TILEXR_INSTALL_PREFIX}/${TILEXR_INSTALL_INCLUDEDIR}"
        "${ASCEND_HOME_PATH}/${ARCH}-linux/include")
    target_link_directories(test_tilexr_ep_server_dedup_registration PRIVATE
        "${ASCEND_HOME_PATH}/${ARCH}-linux/lib64"
        "${ASCEND_DRIVER_PATH}/lib64/driver")
    target_link_libraries(test_tilexr_ep_server_dedup_registration PRIVATE
        ${TILEXR_EP_SERVER_DEDUP_INSTALLED_LIB} ascendcl ${CMAKE_DL_LIBS})
    set_target_properties(test_tilexr_ep_server_dedup_registration PROPERTIES
        SKIP_BUILD_RPATH TRUE INSTALL_RPATH "$ORIGIN/../${TILEXR_INSTALL_LIBDIR}")
    add_test(NAME test_tilexr_ep_server_dedup_registration COMMAND test_tilexr_ep_server_dedup_registration)
    set_tests_properties(test_tilexr_ep_server_dedup_registration PROPERTIES
        LABELS ep-sdedup-p1-empty-kernel TIMEOUT 120)
    install(TARGETS test_tilexr_ep_server_dedup_registration RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

if(BUILD_TILEXR_EP_SERVER_DEDUP_COMBINE_REGISTRATION_TEST)
    find_library(TILEXR_EP_COMBINE_DEDUP_INSTALLED_LIB tilexr-ep
        PATHS "${TILEXR_INSTALL_PREFIX}/${TILEXR_INSTALL_LIBDIR}" NO_DEFAULT_PATH REQUIRED)
    add_executable(test_tilexr_ep_server_dedup_combine_registration
        integration/test_tilexr_ep_server_dedup_combine_registration.cpp)
    target_include_directories(test_tilexr_ep_server_dedup_combine_registration PRIVATE
        "${TILEXR_INSTALL_PREFIX}/${TILEXR_INSTALL_INCLUDEDIR}"
        "${TILEXR_ROOT}/src/ep/host" "${TILEXR_ROOT}/src/ep/common"
        "${ASCEND_HOME_PATH}/${ARCH}-linux/include")
    target_link_directories(test_tilexr_ep_server_dedup_combine_registration PRIVATE
        "${ASCEND_HOME_PATH}/${ARCH}-linux/lib64" "${ASCEND_DRIVER_PATH}/lib64/driver")
    target_link_libraries(test_tilexr_ep_server_dedup_combine_registration PRIVATE
        ${TILEXR_EP_COMBINE_DEDUP_INSTALLED_LIB} ascendcl ${CMAKE_DL_LIBS})
    set_target_properties(test_tilexr_ep_server_dedup_combine_registration PROPERTIES
        SKIP_BUILD_RPATH TRUE INSTALL_RPATH "$ORIGIN/../${TILEXR_INSTALL_LIBDIR}")
    add_test(NAME test_tilexr_ep_server_dedup_combine_registration COMMAND test_tilexr_ep_server_dedup_combine_registration)
    set_tests_properties(test_tilexr_ep_server_dedup_combine_registration PROPERTIES
        LABELS ep-sdedup-combine-p1-empty-kernel TIMEOUT 120)
    install(TARGETS test_tilexr_ep_server_dedup_combine_registration RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

if(BUILD_TILEXR_EP_SERVER_DEDUP_CHAIN_TEST)
    find_library(TILEXR_EP_CHAIN_INSTALLED_LIB tilexr-ep
        PATHS "${TILEXR_INSTALL_PREFIX}/${TILEXR_INSTALL_LIBDIR}" NO_DEFAULT_PATH REQUIRED)
    find_library(TILEXR_EP_CHAIN_COMM_LIB tile-comm
        PATHS "${TILEXR_INSTALL_PREFIX}/${TILEXR_INSTALL_LIBDIR}" NO_DEFAULT_PATH REQUIRED)
    add_executable(test_tilexr_ep_server_dedup_chain
        integration/test_tilexr_ep_server_dedup_chain.cpp)
    target_compile_options(test_tilexr_ep_server_dedup_chain PRIVATE -O2 -fno-fast-math -ffp-contract=off)
    # 内部 layout 声明供 caller 使用；实现和公共入口均链接已安装生产库。
    target_include_directories(test_tilexr_ep_server_dedup_chain PRIVATE
        "${TILEXR_INSTALL_PREFIX}/${TILEXR_INSTALL_INCLUDEDIR}"
        "${TILEXR_ROOT}/src/ep/host"
        "${TILEXR_ROOT}/src/ep/common"
        "${ASCEND_HOME_PATH}/${ARCH}-linux/include")
    target_link_directories(test_tilexr_ep_server_dedup_chain PRIVATE
        "${ASCEND_HOME_PATH}/${ARCH}-linux/lib64"
        "${ASCEND_DRIVER_PATH}/lib64/driver")
    target_link_libraries(test_tilexr_ep_server_dedup_chain PRIVATE
        ${TILEXR_EP_CHAIN_INSTALLED_LIB} ${TILEXR_EP_CHAIN_COMM_LIB} ascendcl)
    if(BUILD_TILEXR_EP_MEMORY_BOUNDARY_TEST)
        find_library(TILEXR_EP_BOUNDARY_TEST_LIB tilexr-ep-memory-boundary-test
            PATHS "${TILEXR_INSTALL_PREFIX}/${TILEXR_INSTALL_LIBDIR}" NO_DEFAULT_PATH REQUIRED)
        target_link_libraries(test_tilexr_ep_server_dedup_chain PRIVATE ${TILEXR_EP_BOUNDARY_TEST_LIB})
        target_compile_definitions(test_tilexr_ep_server_dedup_chain PRIVATE TILEXR_EP_MEMORY_BOUNDARY_TEST=1)
    endif()
    if(BUILD_TILEXR_EP_COMBINE_MEMORY_BOUNDARY_TEST)
        find_library(TILEXR_EP_COMBINE_BOUNDARY_TEST_LIB tilexr-ep-combine-memory-boundary-test
            PATHS "${TILEXR_INSTALL_PREFIX}/${TILEXR_INSTALL_LIBDIR}" NO_DEFAULT_PATH REQUIRED)
        target_link_libraries(test_tilexr_ep_server_dedup_chain PRIVATE ${TILEXR_EP_COMBINE_BOUNDARY_TEST_LIB})
        target_compile_definitions(test_tilexr_ep_server_dedup_chain PRIVATE TILEXR_EP_COMBINE_MEMORY_BOUNDARY_TEST=1)
    endif()
    set_target_properties(test_tilexr_ep_server_dedup_chain PROPERTIES
        SKIP_BUILD_RPATH TRUE INSTALL_RPATH "$ORIGIN/../${TILEXR_INSTALL_LIBDIR}")
    # 由现有 validation controller 按 rank 调度，不作为每节点 Host ctest。
    install(TARGETS test_tilexr_ep_server_dedup_chain RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
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

# Historical M1/M2/M4 handoff fixtures require forwarding. Keep their baseline
# explicit; the shared source-aggregate fixture exercises full M3/M6 in mode 0.
set_target_properties(test_tilexr_ep_server_dedup_combine_m12
    test_tilexr_ep_server_dedup_combine_m3 test_tilexr_ep_server_dedup_combine_m4
    test_tilexr_ep_server_dedup_chain_dry_run test_tilexr_ep_server_dedup_combine_delay
    PROPERTIES TILEXR_TEST_DEDUP_ONLY TRUE)

# Automatic execution keeps the dedup resource layout at both BS boundaries.
add_executable(test_tilexr_ep_server_dedup_auto_combine
    unit/test_tilexr_ep_server_dedup_auto_combine.cpp)
target_include_directories(test_tilexr_ep_server_dedup_auto_combine BEFORE PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/unit/server_dedup_combine_delay_fake" ${TILEXR_EP_TEST_INCLUDE_DIRS})
add_test(NAME test_tilexr_ep_server_dedup_auto_combine COMMAND test_tilexr_ep_server_dedup_auto_combine)
set_tests_properties(test_tilexr_ep_server_dedup_auto_combine PROPERTIES TIMEOUT 180)
set_target_properties(test_tilexr_ep_server_dedup_source_send_auto
    test_tilexr_ep_server_dedup_receive_packet_auto test_tilexr_ep_server_dedup_auto_combine
    PROPERTIES TILEXR_TEST_DEDUP_AUTO TRUE)

if(Python3_Interpreter_FOUND)
    add_test(NAME test_tilexr_ep_server_dedup_auto_consumer
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/integration/test_server_dedup_auto.py")
    set_tests_properties(test_tilexr_ep_server_dedup_auto_consumer PROPERTIES TIMEOUT 180)
endif()

add_executable(test_tilexr_ep_server_dedup_auto_policy
    unit/test_tilexr_ep_server_dedup_auto_policy.cpp
    ${TILEXR_ROOT}/src/ep/host/ep_memory_server_dedup_layout.cpp
    ${TILEXR_ROOT}/src/ep/host/ep_memory_server_dedup_combine_layout.cpp)
target_include_directories(test_tilexr_ep_server_dedup_auto_policy PRIVATE ${TILEXR_EP_TEST_INCLUDE_DIRS})
set_target_properties(test_tilexr_ep_server_dedup_auto_policy PROPERTIES TILEXR_TEST_DEDUP_AUTO TRUE)
add_test(NAME test_tilexr_ep_server_dedup_auto_policy COMMAND test_tilexr_ep_server_dedup_auto_policy)

# These historical fixtures assert the exact M4 -> M5 handoff cadence. Preserve
# that phase-specific baseline in auto builds; the auto matrix covers M3/M6 too.
if(TILEXR_EP_SERVER_DEDUP_ENABLE STREQUAL "auto")
    set_target_properties(test_tilexr_ep_server_dedup_combine_m5
        test_tilexr_ep_server_dedup_combine_m5_send16 test_tilexr_ep_server_dedup_combine_m5_send28
        test_tilexr_ep_server_dedup_combine_m5_send48 test_tilexr_ep_server_dedup_combine_m5_send52
        PROPERTIES TILEXR_TEST_DEDUP_ONLY TRUE)
endif()
foreach(_auto_send_cores IN ITEMS 16 28 48 52)
    set(_auto_split_target test_tilexr_ep_server_dedup_auto_combine_send${_auto_send_cores})
    add_executable(${_auto_split_target} unit/test_tilexr_ep_server_dedup_auto_combine.cpp)
    target_include_directories(${_auto_split_target} BEFORE PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/unit/server_dedup_combine_delay_fake" ${TILEXR_EP_TEST_INCLUDE_DIRS})
    target_compile_definitions(${_auto_split_target} PRIVATE
        $<$<NOT:$<BOOL:${TILEXR_EP_SERVER_DEDUP_COMBINE_SEND_CORE_NUM}>>:TILEXR_EP_SERVER_DEDUP_COMBINE_SEND_CORE_NUM=${_auto_send_cores}>)
    set_target_properties(${_auto_split_target} PROPERTIES TILEXR_TEST_DEDUP_AUTO TRUE
        TILEXR_TEST_COMBINE_SEND_CORES ${_auto_send_cores})
    add_test(NAME ${_auto_split_target} COMMAND ${_auto_split_target})
    set_tests_properties(${_auto_split_target} PROPERTIES TIMEOUT 180)
endforeach()
