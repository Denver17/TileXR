cmake_minimum_required(VERSION 3.16)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(TileXR_MoonEP_Combine_V2_Tests LANGUAGES CXX)
endif()

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
include(GNUInstallDirs)
enable_testing()

set(TILEXR_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../..")
set(TILEXR_MOONEP_COMBINE_V2_FAKE_INCLUDE_DIR
    "${CMAKE_CURRENT_BINARY_DIR}/fake_include")
file(MAKE_DIRECTORY "${TILEXR_MOONEP_COMBINE_V2_FAKE_INCLUDE_DIR}/acl")
file(MAKE_DIRECTORY "${TILEXR_MOONEP_COMBINE_V2_FAKE_INCLUDE_DIR}/runtime")

file(WRITE "${TILEXR_MOONEP_COMBINE_V2_FAKE_INCLUDE_DIR}/acl/acl_base.h"
"#ifndef TILEXR_MOONEP_COMBINE_V2_TEST_ACL_BASE_H\n"
"#define TILEXR_MOONEP_COMBINE_V2_TEST_ACL_BASE_H\n"
"typedef void *aclrtStream;\n"
"typedef int aclError;\n"
"#define ACL_SUCCESS 0\n"
"#endif\n")

file(WRITE "${TILEXR_MOONEP_COMBINE_V2_FAKE_INCLUDE_DIR}/acl/acl_rt.h"
"#ifndef TILEXR_MOONEP_COMBINE_V2_TEST_ACL_RT_H\n"
"#define TILEXR_MOONEP_COMBINE_V2_TEST_ACL_RT_H\n"
"#include <stddef.h>\n"
"#include <stdint.h>\n"
"#include \"acl_base.h\"\n"
"typedef enum aclrtDevAttr { ACL_DEV_ATTR_VECTOR_CORE_NUM = 0, ACL_DEV_ATTR_UBUF_PER_VECTOR_CORE = 1 } aclrtDevAttr;\n"
"typedef enum aclrtMemcpyKind { ACL_MEMCPY_DEVICE_TO_DEVICE = 3 } aclrtMemcpyKind;\n"
"#ifdef __cplusplus\nextern \"C\" {\n#endif\n"
"aclError aclrtGetDevice(int32_t *deviceId);\n"
"aclError aclrtGetDeviceInfo(uint32_t deviceId, aclrtDevAttr attr, int64_t *value);\n"
"aclError aclrtMemcpyAsync(void *dst, size_t dstMax, const void *src, size_t count, aclrtMemcpyKind kind, aclrtStream stream);\n"
"#ifdef __cplusplus\n}\n#endif\n"
"#endif\n")

file(WRITE "${TILEXR_MOONEP_COMBINE_V2_FAKE_INCLUDE_DIR}/runtime/kernel.h"
"#ifndef TILEXR_MOONEP_COMBINE_V2_TEST_RUNTIME_KERNEL_H\n"
"#define TILEXR_MOONEP_COMBINE_V2_TEST_RUNTIME_KERNEL_H\n"
"#include <stddef.h>\n"
"#include <stdint.h>\n"
"typedef void *rtStream_t;\n"
"typedef int rtError_t;\n"
"#define RT_ERROR_NONE 0\n"
"typedef struct rtArgsEx { void *args; size_t argsSize; } rtArgsEx_t;\n"
"typedef struct rtTaskCfgInfo { uint8_t schemMode; uint32_t localMemorySize; } rtTaskCfgInfo_t;\n"
"#ifdef __cplusplus\nextern \"C\" {\n#endif\n"
"rtError_t rtKernelLaunchWithFlagV2(const void *stubFunc, uint32_t blockDim, rtArgsEx_t *argsInfo, void *smDesc, rtStream_t stream, uint32_t flags, const rtTaskCfgInfo_t *cfgInfo);\n"
"#ifdef __cplusplus\n}\n#endif\n"
"#endif\n")

file(WRITE "${TILEXR_MOONEP_COMBINE_V2_FAKE_INCLUDE_DIR}/tilexr_api.h"
"#ifndef TILEXR_MOONEP_COMBINE_V2_TEST_TILEXR_API_H\n"
"#define TILEXR_MOONEP_COMBINE_V2_TEST_TILEXR_API_H\n"
"#include <stdint.h>\n"
    "#include \"comm_args.h\"\n"
    "#include \"tilexr_udma_fullmesh.h\"\n"
    "#include \"tilexr_udma_reg.h\"\n"
"typedef void *TileXRCommPtr;\n"
"extern \"C\" {\n"
"int TileXRGetCommArgsHost(TileXRCommPtr comm, TileXR::CommArgs *&commArgs);\n"
"int TileXRGetCommArgsDev(TileXRCommPtr comm, GM_ADDR &commArgs);\n"
"int TileXRGetUDMARegistryHost(TileXRCommPtr comm, const TileXR::TileXRUDMARegistry **registry);\n"
    "int TileXRUDMAGetQpCount(TileXRCommPtr comm, uint32_t *qpCount);\n"
    "int TileXRUDMAFullmeshQuery(TileXRCommPtr comm, TileXR::TileXRUDMAFullmeshHostView *view);\n"
"int TileXRCommNextMagic(TileXRCommPtr comm, int64_t *magic);\n"
"}\n"
"#endif\n")

set(TILEXR_MOONEP_COMBINE_V2_INCLUDE_DIRS
    ${TILEXR_MOONEP_COMBINE_V2_FAKE_INCLUDE_DIR}
    ${TILEXR_ROOT}/src/include
    ${TILEXR_ROOT}/src/moonep/common
    ${TILEXR_ROOT}/src/moonep/combine_v2/common
    ${TILEXR_ROOT}/src/moonep/combine_v2/host
)

add_executable(test_tilexr_moonep_combine_v2_schedule
    unit/test_combine_v2_schedule.cpp)
target_include_directories(test_tilexr_moonep_combine_v2_schedule PRIVATE
    ${TILEXR_MOONEP_COMBINE_V2_INCLUDE_DIRS})

add_executable(test_tilexr_moonep_combine_v2_layout
    unit/test_combine_v2_layout.cpp
    ${TILEXR_ROOT}/src/moonep/combine_v2/host/combine_v2_layout.cpp)
target_include_directories(test_tilexr_moonep_combine_v2_layout PRIVATE
    ${TILEXR_MOONEP_COMBINE_V2_INCLUDE_DIRS})

add_executable(test_tilexr_moonep_combine_v2_host
    unit/test_combine_v2_host.cpp
    ${TILEXR_ROOT}/src/moonep/combine_v2/host/combine_v2_layout.cpp
    ${TILEXR_ROOT}/src/moonep/combine_v2/host/combine_v2_host.cpp
    ${TILEXR_ROOT}/src/moonep/combine_v2/host/tilexr_moonep_combine_v2.cpp)
target_include_directories(test_tilexr_moonep_combine_v2_host PRIVATE
    ${TILEXR_MOONEP_COMBINE_V2_INCLUDE_DIRS})

add_executable(test_tilexr_moonep_combine_v2_launch
    unit/test_combine_v2_launch.cpp
    ${TILEXR_ROOT}/src/moonep/combine_v2/host/combine_v2_launch.cpp)
target_include_directories(test_tilexr_moonep_combine_v2_launch BEFORE PRIVATE
    ${TILEXR_MOONEP_COMBINE_V2_INCLUDE_DIRS})

add_executable(test_tilexr_moonep_combine_v2_public_abi
    unit/test_combine_v2_public_abi.cpp)
target_include_directories(test_tilexr_moonep_combine_v2_public_abi PRIVATE
    ${TILEXR_MOONEP_COMBINE_V2_FAKE_INCLUDE_DIR}
    ${TILEXR_ROOT}/src/include)

if(TARGET tilexr-moonep-combine-v2 AND TARGET tile-comm)
    add_executable(tilexr_moonep_combine_v2_perf
        demo/tilexr_moonep_combine_v2_hardware_probe.cpp)
    target_include_directories(tilexr_moonep_combine_v2_perf PRIVATE
        ${TILEXR_ROOT}/src/include
        ${TILEXR_ROOT}/src/moonep/common
        ${TILEXR_ROOT}/src/moonep/combine_v2/common
        ${TILEXR_ROOT}/src/moonep/combine_v2/host
        ${ASCEND_HOME_PATH}/${ARCH}-linux/pkg_inc
        ${ASCEND_HOME_PATH}/${ARCH}-linux/pkg_inc/runtime
        ${ASCEND_HOME_PATH}/${ARCH}-linux/include)
    target_link_directories(tilexr_moonep_combine_v2_perf PRIVATE
        ${ASCEND_HOME_PATH}/${ARCH}-linux/lib64
        ${ASCEND_DRIVER_PATH}/lib64/driver)
    target_link_libraries(tilexr_moonep_combine_v2_perf PRIVATE
        tilexr-moonep-combine-v2
        tile-comm
        ascendcl
        runtime)
    target_link_options(tilexr_moonep_combine_v2_perf PRIVATE
        "LINKER:-rpath-link,${ASCEND_HOME_PATH}/${ARCH}-linux/lib64"
        "LINKER:-rpath-link,${ASCEND_DRIVER_PATH}/lib64/driver")
    set_target_properties(tilexr_moonep_combine_v2_perf PROPERTIES
        BUILD_WITH_INSTALL_RPATH TRUE
        INSTALL_RPATH "$ORIGIN/../${CMAKE_INSTALL_LIBDIR};${ASCEND_HOME_PATH}/${ARCH}-linux/lib64;${ASCEND_DRIVER_PATH}/lib64/driver")
endif()

add_test(NAME test_tilexr_moonep_combine_v2_schedule
    COMMAND test_tilexr_moonep_combine_v2_schedule)
add_test(NAME test_tilexr_moonep_combine_v2_layout
    COMMAND test_tilexr_moonep_combine_v2_layout)
add_test(NAME test_tilexr_moonep_combine_v2_host
    COMMAND test_tilexr_moonep_combine_v2_host)
add_test(NAME test_tilexr_moonep_combine_v2_launch
    COMMAND test_tilexr_moonep_combine_v2_launch)
add_test(NAME test_tilexr_moonep_combine_v2_public_abi
    COMMAND test_tilexr_moonep_combine_v2_public_abi)

install(TARGETS
    test_tilexr_moonep_combine_v2_schedule
    test_tilexr_moonep_combine_v2_layout
    test_tilexr_moonep_combine_v2_host
    test_tilexr_moonep_combine_v2_launch
    test_tilexr_moonep_combine_v2_public_abi
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

if(TARGET tilexr_moonep_combine_v2_perf)
    install(TARGETS tilexr_moonep_combine_v2_perf
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()
