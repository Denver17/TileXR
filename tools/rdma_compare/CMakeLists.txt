# 功能：构建 bench/multiqp Host 程序和独立 AIV ELF，并为两端设置一致的 Debug、profiling 与单 WQE 模式宏。
# 调用关系：bisheng 和 ld.lld 编译、链接 kernel；Host 程序链接已安装的 TileXR、ACL、runtime 及 socket exchange 源码。
# 边界：本工程只生成 benchmark 产物，不启动测试或将 kernel binary 嵌入 Host 程序。
#
# Purpose: Build bench/multiqp Host executables and separate AIV ELFs with matching Debug, profiling and single-WQE macros.
# Calls: bisheng/ld.lld build kernels; Host targets use installed TileXR, ACL, runtime and socket exchange sources.
# Scope: Generates benchmark artifacts without executing tests or embedding kernel binaries in Host executables.
cmake_minimum_required(VERSION 3.16)
project(TileXRMatchedWrite LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 14)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()
option(TILEXR_RDMA_PROFILE "Enable existing SDK profiling in Debug RDMA kernels" ON)
option(TILEXR_RDMA_SINGLE_WQE_CALLS "Make multiqp call WRITE once per WQE, then wait once per QP" OFF)
set(RDMA_DEBUG_VALUE "$<IF:$<CONFIG:Debug>,1,0>")
set(RDMA_PROFILE_VALUE "$<AND:$<CONFIG:Debug>,$<BOOL:${TILEXR_RDMA_PROFILE}>>")
find_package(Threads REQUIRED)
set(TILEXR_ROOT "" CACHE PATH "TileXR checkout")
set(TILEXR_INSTALL "" CACHE PATH "Production install")
set(CANN "$ENV{ASCEND_HOME_PATH}")
string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" HOST_ARCH)
if(HOST_ARCH MATCHES "^(amd64|x86_64)$")
    set(HOST_ARCH x86_64)
elseif(HOST_ARCH MATCHES "^(arm64|aarch64)$")
    set(HOST_ARCH aarch64)
else()
    message(FATAL_ERROR "Unsupported CANN host architecture: ${CMAKE_SYSTEM_PROCESSOR}")
endif()
set(PLATFORM "${CANN}/${HOST_ARCH}-linux")
find_program(BISHENG bisheng HINTS "${CANN}/compiler/ccec_compiler/bin" "${CANN}/tools/bisheng_compiler/bin" REQUIRED)
get_filename_component(COMPILER_BIN "${BISHENG}" DIRECTORY)
find_program(LINKER ld.lld HINTS "${COMPILER_BIN}" REQUIRED)
set(INCLUDES "${CMAKE_CURRENT_SOURCE_DIR}" "${TILEXR_ROOT}/src/include"
    "${CANN}/compiler/tikcpp" "${CANN}/compiler/tikcpp/tikcfw"
    "${CANN}/compiler/tikcpp/tikcfw/impl" "${CANN}/compiler/tikcpp/tikcfw/interface"
    "${PLATFORM}/tikcpp" "${PLATFORM}/tikcpp/tikcfw"
    "${PLATFORM}/tikcpp/tikcfw/impl" "${PLATFORM}/tikcpp/tikcfw/interface"
    "${PLATFORM}/asc/include" "${PLATFORM}/ascendc/include"
    "${PLATFORM}/include" "${PLATFORM}/pkg_inc" "${PLATFORM}/pkg_inc/runtime"
    "${CANN}/include" "${CANN}/pkg_inc" "${CANN}/pkg_inc/runtime"
    "/usr/local/Ascend/driver/kernel/inc")
foreach(INC IN LISTS INCLUDES)
    list(APPEND INCLUDE_FLAGS "-I${INC}")
endforeach()
find_library(TILE_COMM tile-comm HINTS "${TILEXR_INSTALL}/lib" "${TILEXR_INSTALL}/lib64" REQUIRED)
find_library(ACL ascendcl HINTS "${CANN}/lib64" "${PLATFORM}/lib64" REQUIRED)
find_library(RUNTIME runtime HINTS "${CANN}/lib64" "${PLATFORM}/lib64" REQUIRED)

function(add_rdma_benchmark name)
    set(kernel "${CMAKE_CURRENT_BINARY_DIR}/${name}_kernel")
    add_custom_command(OUTPUT "${kernel}.o"
        COMMAND "${BISHENG}" -O2 -xcce -std=gnu++17 --cce-aicore-only --cce-aicore-arch=dav-c310-vec
            -mllvm -cce-aicore-stack-size=0x8000 -mllvm -cce-aicore-function-stack-size=0x8000
            -mllvm -cce-aicore-record-overflow=true -mllvm -cce-aicore-addr-transform
            -mllvm -cce-aicore-dcci-insert-for-scalar=false ${INCLUDE_FLAGS}
            "-DTILEXR_RDMA_DEBUG=${RDMA_DEBUG_VALUE}" "-DTILEXR_ENABLE_PROFILING=${RDMA_PROFILE_VALUE}"
            "-DTILEXR_RDMA_SINGLE_WQE_CALLS=$<BOOL:${TILEXR_RDMA_SINGLE_WQE_CALLS}>"
            -c "${CMAKE_CURRENT_SOURCE_DIR}/${name}_kernel.cpp" -o "${kernel}_rel.o"
        COMMAND "${LINKER}" -m aicorelinux -Ttext=0 "${kernel}_rel.o"
            --static --allow-multiple-definition -o "${kernel}.o"
        DEPENDS "${name}_kernel.cpp" bench_abi.h case_layout.h ${ARGN}
            "${TILEXR_ROOT}/src/include/tilexr_rdma_device.h" "${TILEXR_ROOT}/src/include/tilexr_rdma_wire.h"
            "${TILEXR_ROOT}/src/include/tilexr_rdma_device_types.h"
            "${TILEXR_ROOT}/src/include/tilexr_rdma_pipeline.h"
            ${RDMA_DEVICE_HEADERS}
        VERBATIM)
    add_custom_target(${name}_kernel ALL DEPENDS "${kernel}.o")
    add_executable(${name} "${name}.cpp" "${TILEXR_ROOT}/src/comm/tools/socket/tilexr_sock_exchange.cpp")
    target_include_directories(${name} PRIVATE ${INCLUDES} "${TILEXR_ROOT}/src/comm" "${TILEXR_ROOT}/3rdparty")
    target_compile_options(${name} PRIVATE -O2 -Wall -Wextra -fvisibility=hidden)
    target_compile_definitions(${name} PRIVATE
        "TILEXR_RDMA_DEBUG=${RDMA_DEBUG_VALUE}" "TILEXR_ENABLE_PROFILING=${RDMA_PROFILE_VALUE}"
        "TILEXR_RDMA_SINGLE_WQE_CALLS=$<BOOL:${TILEXR_RDMA_SINGLE_WQE_CALLS}>")
    target_link_libraries(${name} PRIVATE "${TILE_COMM}" "${ACL}" "${RUNTIME}" Threads::Threads)
    add_dependencies(${name} ${name}_kernel)
endfunction()

file(GLOB RDMA_DEVICE_HEADERS CONFIGURE_DEPENDS
    "${TILEXR_ROOT}/src/include/tilexr_rdma*.h"
    "${TILEXR_ROOT}/src/include/tilexr/profiling/*.h"
    "${TILEXR_ROOT}/src/include/tilexr/profiling/detail/*.h")

add_rdma_benchmark(bench)
# One AIV and scratch buffer own each independent QP in the shared launch.
add_rdma_benchmark(multiqp "${TILEXR_ROOT}/src/include/tilexr_rdma_write.h"
    "${TILEXR_ROOT}/src/include/tilexr_rdma_write_options.h")
