#
# Copyright (c) 2024-2026 TileXR Project
# Licensed under the Apache License, Version 2.0
#

cmake_minimum_required(VERSION 3.16)
project(TileXR_Memory_Tests)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
option(BUILD_TILEXR_MEMORY_DEMO "Build TileXR peer-memory DataCopy demo with Ascend C kernel" ON)
set(TILEXR_MEMORY_DEMO_SOC_TYPE "Ascend950" CACHE STRING "SOC type used for the TileXR memory demo kernel")

set(ASCEND_HOME_PATH $ENV{ASCEND_HOME_PATH})
set(ARCH $ENV{ARCH})
if(NOT ARCH)
    set(ARCH $ENV{TILEXR_OS_ARCH})
endif()
if(NOT ARCH)
    execute_process(
        COMMAND uname -m
        OUTPUT_VARIABLE ARCH
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    if(ARCH STREQUAL "arm64")
        set(ARCH "aarch64")
    endif()
endif()

set(ASCEND_DRIVER_PATH $ENV{ASCEND_DRIVER_PATH})
if(NOT ASCEND_DRIVER_PATH)
    set(ASCEND_DRIVER_PATH "/usr/local/Ascend/driver")
endif()

set(TILEXR_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../..")

message(STATUS "ASCEND_HOME_PATH: ${ASCEND_HOME_PATH}")
message(STATUS "ARCH: ${ARCH}")
message(STATUS "TILEXR_ROOT: ${TILEXR_ROOT}")

find_library(TILEXR_LIB tile-comm
    HINTS "${TILEXR_ROOT}/install/lib" "${TILEXR_ROOT}/build/src/comm"
    REQUIRED)

message(STATUS "Found tile-comm: ${TILEXR_LIB}")

include_directories(
    ${ASCEND_HOME_PATH}/${ARCH}-linux/pkg_inc/
    ${ASCEND_HOME_PATH}/${ARCH}-linux/pkg_inc/runtime/
    ${ASCEND_HOME_PATH}/${ARCH}-linux/include/
    ${ASCEND_DRIVER_PATH}/kernel/inc
    ${TILEXR_ROOT}/3rdparty
    ${TILEXR_ROOT}/src/include
)

link_directories(
    ${ASCEND_DRIVER_PATH}/lib64/driver
    ${ASCEND_HOME_PATH}/${ARCH}-linux/lib64
    ${ASCEND_HOME_PATH}/${ARCH}-linux/devlib
)

set(INSTALL_TARGETS)

if(BUILD_TILEXR_MEMORY_DEMO)
    find_program(BISHENG_EXECUTABLE bisheng)
    if(NOT BISHENG_EXECUTABLE)
        message(WARNING "bisheng not found; skip tilexr_memory_demo")
    else()
        if(TILEXR_MEMORY_DEMO_SOC_TYPE STREQUAL "Ascend950")
            set(TILEXR_MEMORY_NPU_ARCH "dav-3510")
            set(TILEXR_MEMORY_AICORE_ARCH "--cce-aicore-arch=dav-c310-vec")
            set(TILEXR_MEMORY_CATLASS_ARCH "3510")
        else()
            set(TILEXR_MEMORY_NPU_ARCH "dav-2201")
            set(TILEXR_MEMORY_AICORE_ARCH "--cce-aicore-arch=dav-c220-vec")
            set(TILEXR_MEMORY_CATLASS_ARCH "2201")
        endif()

        execute_process(
            COMMAND ${BISHENG_EXECUTABLE} -v
            OUTPUT_VARIABLE BISHENG_VERSION_OUTPUT
            ERROR_VARIABLE BISHENG_VERSION_OUTPUT
            RESULT_VARIABLE BISHENG_VERSION_RESULT
        )
        if(BISHENG_VERSION_RESULT EQUAL 0 AND BISHENG_VERSION_OUTPUT MATCHES "([0-9]{8})")
            set(TILEXR_MEMORY_BISHENG_DATE "${CMAKE_MATCH_1}")
        else()
            set(TILEXR_MEMORY_BISHENG_DATE "0")
        endif()
        if(TILEXR_MEMORY_BISHENG_DATE GREATER_EQUAL 20250428)
            set(TILEXR_MEMORY_KERNEL_COMPILE_OPTIONS
                -xasc
                -Xhost-start -ftrapv -Xhost-end
                --npu-arch=${TILEXR_MEMORY_NPU_ARCH}
                --cce-auto-infer-kernel-type=false
            )
            set(TILEXR_MEMORY_KERNEL_LINK_OPTIONS --cce-fatobj-link)
        else()
            set(TILEXR_MEMORY_KERNEL_COMPILE_OPTIONS
                -xcce
                -Xhost-start -ftrapv -Xhost-end
                -mllvm -cce-aicore-stack-size=0x8000
                -mllvm -cce-aicore-function-stack-size=0x8000
                -mllvm -cce-aicore-record-overflow=true
                -mllvm -cce-aicore-addr-transform
                -mllvm -cce-aicore-dcci-insert-for-scalar=false
                ${TILEXR_MEMORY_AICORE_ARCH}
            )
            set(TILEXR_MEMORY_KERNEL_LINK_OPTIONS --cce-fatobj-link ${TILEXR_MEMORY_AICORE_ARCH})
        endif()

        set(TILEXR_MEMORY_DEMO_KERNEL_SO "${CMAKE_CURRENT_BINARY_DIR}/libtilexr_memory_demo_kernel.so")
        set(TILEXR_MEMORY_DEMO_KERNEL_INCLUDES
            -I${ASCEND_HOME_PATH}/compiler/tikcpp
            -I${ASCEND_HOME_PATH}/compiler/tikcpp/tikcfw
            -I${ASCEND_HOME_PATH}/compiler/tikcpp/tikcfw/impl
            -I${ASCEND_HOME_PATH}/compiler/tikcpp/tikcfw/interface
            -I${ASCEND_HOME_PATH}/${ARCH}-linux/tikcpp
            -I${ASCEND_HOME_PATH}/${ARCH}-linux/tikcpp/tikcfw
            -I${ASCEND_HOME_PATH}/${ARCH}-linux/tikcpp/tikcfw/impl
            -I${ASCEND_HOME_PATH}/${ARCH}-linux/tikcpp/tikcfw/interface
            -I${ASCEND_HOME_PATH}/${ARCH}-linux/pkg_inc/
            -I${ASCEND_HOME_PATH}/${ARCH}-linux/pkg_inc/runtime/
            -I${ASCEND_HOME_PATH}/${ARCH}-linux/include/
            -I${ASCEND_DRIVER_PATH}/kernel/inc
            -I${TILEXR_ROOT}/3rdparty
            -I${TILEXR_ROOT}/src/include
        )
        add_custom_command(
            OUTPUT "${TILEXR_MEMORY_DEMO_KERNEL_SO}"
            COMMAND ${BISHENG_EXECUTABLE}
                ${TILEXR_MEMORY_KERNEL_COMPILE_OPTIONS}
                -std=gnu++17
                -fPIC
                -shared
                ${TILEXR_MEMORY_KERNEL_LINK_OPTIONS}
                -DCATLASS_ARCH=${TILEXR_MEMORY_CATLASS_ARCH}
                ${TILEXR_MEMORY_DEMO_KERNEL_INCLUDES}
                "${CMAKE_CURRENT_SOURCE_DIR}/demo/tilexr_memory_demo_kernel.cpp"
                -L${ASCEND_DRIVER_PATH}/lib64/driver
                -L${ASCEND_HOME_PATH}/${ARCH}-linux/lib64
                -L${ASCEND_HOME_PATH}/${ARCH}-linux/devlib
                -lruntime
                -lascendcl
                -lstdc++
                -lm
                -ltiling_api
                -lplatform
                -lc_sec
                -ldl
                -lnnopbase
                -lpthread
                -o "${TILEXR_MEMORY_DEMO_KERNEL_SO}"
            DEPENDS
                "${CMAKE_CURRENT_SOURCE_DIR}/demo/tilexr_memory_demo_kernel.cpp"
                "${TILEXR_ROOT}/src/include/comm_args.h"
                "${TILEXR_ROOT}/src/include/tilexr_sync.h"
            VERBATIM
            COMMENT "Building TileXR peer-memory DataCopy demo kernel with bisheng"
        )
        add_custom_target(tilexr_memory_demo_kernel ALL DEPENDS "${TILEXR_MEMORY_DEMO_KERNEL_SO}")

        add_executable(tilexr_memory_demo
            demo/tilexr_memory_demo.cpp
        )
        add_dependencies(tilexr_memory_demo tilexr_memory_demo_kernel)
        target_link_directories(tilexr_memory_demo PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
        target_link_libraries(tilexr_memory_demo
            "${TILEXR_MEMORY_DEMO_KERNEL_SO}"
            ${TILEXR_LIB}
            ascendcl
            runtime
            ascend_hal
        )
        list(APPEND INSTALL_TARGETS tilexr_memory_demo)
        install(FILES "${TILEXR_MEMORY_DEMO_KERNEL_SO}" DESTINATION ${CMAKE_INSTALL_PREFIX}/lib)
        message(STATUS "TileXR memory demo enabled with ${BISHENG_EXECUTABLE}, SOC=${TILEXR_MEMORY_DEMO_SOC_TYPE}")
    endif()
endif()

if(INSTALL_TARGETS)
    install(TARGETS ${INSTALL_TARGETS}
        RUNTIME DESTINATION ${CMAKE_INSTALL_PREFIX}/bin
        LIBRARY DESTINATION ${CMAKE_INSTALL_PREFIX}/lib
    )
endif()
