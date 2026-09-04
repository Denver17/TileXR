#
# Copyright (c) 2024 Huawei Technologies Co., Ltd.
# CMakeLists.txt for UDMA tests
#

cmake_minimum_required(VERSION 3.16)
project(TileXR_UDMA_Tests)

# 使用 C++14
set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
include(CTest)
enable_testing()
option(BUILD_TILEXR_UDMA_DEMO "Build TileXR UDMA communication demo with Ascend C kernel" ON)
set(TILEXR_UDMA_DEMO_SOC_TYPE "Ascend950" CACHE STRING "SOC type used for the TileXR UDMA demo kernel")

# 获取环境变量
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

# TileXR 路径
set(TILEXR_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../..")

message(STATUS "ASCEND_HOME_PATH: ${ASCEND_HOME_PATH}")
message(STATUS "ARCH: ${ARCH}")
message(STATUS "TILEXR_ROOT: ${TILEXR_ROOT}")

# 查找库
find_library(TILEXR_LIB tile-comm
    HINTS "${TILEXR_ROOT}/install/lib" "${TILEXR_ROOT}/build/src/comm"
    REQUIRED)

message(STATUS "Found tile-comm: ${TILEXR_LIB}")

# 包含目录
include_directories(
    ${ASCEND_HOME_PATH}/${ARCH}-linux/pkg_inc/
    ${ASCEND_HOME_PATH}/${ARCH}-linux/pkg_inc/runtime/
    ${ASCEND_HOME_PATH}/${ARCH}-linux/include/
    ${ASCEND_DRIVER_PATH}/kernel/inc
    ${TILEXR_ROOT}/3rdparty
    ${TILEXR_ROOT}/src/include
)

# 链接目录
link_directories(
    ${ASCEND_DRIVER_PATH}/lib64/driver
    ${ASCEND_HOME_PATH}/${ARCH}-linux/lib64
    ${ASCEND_HOME_PATH}/${ARCH}-linux/devlib
)

add_executable(test_tilexr_udma_registry
    unit/test_tilexr_udma_registry.cpp
)

target_include_directories(test_tilexr_udma_registry PRIVATE
    ${TILEXR_ROOT}/src/include
)

add_executable(test_tilexr_udma_device_api
    unit/test_tilexr_udma_device_api.cpp
)

target_compile_definitions(test_tilexr_udma_device_api PRIVATE
    TILEXR_UDMA_FORCE_ENABLE
)

target_include_directories(test_tilexr_udma_device_api PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/unit/stubs
    ${TILEXR_ROOT}/src/include
)

add_executable(test_tilexr_udma_transport_layout
    unit/test_tilexr_udma_transport_layout.cpp
    ${TILEXR_ROOT}/src/comm/udma/tilexr_udma_layout.cpp
)

target_include_directories(test_tilexr_udma_transport_layout PRIVATE
    ${TILEXR_ROOT}/src/include
    ${TILEXR_ROOT}/src/comm
    ${TILEXR_ROOT}/src/collectives/kernels
)

add_executable(test_tilexr_udma_route_config
    unit/test_tilexr_udma_route_config.cpp
    ${TILEXR_ROOT}/src/comm/udma/tilexr_udma_config.cpp
)

target_include_directories(test_tilexr_udma_route_config PRIVATE
    ${TILEXR_ROOT}/src/comm
)

add_executable(test_tilexr_udma_rootinfo_parser
    unit/test_tilexr_udma_rootinfo_parser.cpp
    ${TILEXR_ROOT}/src/comm/udma/tilexr_udma_root_info.cpp
)

target_include_directories(test_tilexr_udma_rootinfo_parser PRIVATE
    ${TILEXR_ROOT}/src/comm
)

add_executable(test_tilexr_udma_peermem_sources
    unit/test_tilexr_udma_peermem_sources.cpp
)
target_compile_definitions(test_tilexr_udma_peermem_sources PRIVATE
    TILEXR_SOURCE_DIR="${TILEXR_ROOT}"
)

# 集成测试：TileXR UDMA
add_executable(test_tilexr_udma
    integration/test_tilexr_udma.cpp
)

target_link_libraries(test_tilexr_udma
    ${TILEXR_LIB}
    ascendcl
    runtime
    ascend_hal
)

set(INSTALL_TARGETS
    test_tilexr_udma
    test_tilexr_udma_registry
    test_tilexr_udma_device_api
    test_tilexr_udma_transport_layout
    test_tilexr_udma_route_config
    test_tilexr_udma_rootinfo_parser
    test_tilexr_udma_peermem_sources
)

add_test(NAME test_tilexr_udma_registry COMMAND test_tilexr_udma_registry)
add_test(NAME test_tilexr_udma_device_api COMMAND test_tilexr_udma_device_api)
add_test(NAME test_tilexr_udma_transport_layout COMMAND test_tilexr_udma_transport_layout)
add_test(NAME test_tilexr_udma_route_config COMMAND test_tilexr_udma_route_config)
add_test(NAME test_tilexr_udma_rootinfo_parser COMMAND test_tilexr_udma_rootinfo_parser)
add_test(NAME test_tilexr_udma_peermem_sources COMMAND test_tilexr_udma_peermem_sources)

if(BUILD_TILEXR_UDMA_DEMO)
    find_program(BISHENG_EXECUTABLE bisheng)
    if(NOT BISHENG_EXECUTABLE)
        message(WARNING "bisheng not found; skip tilexr_udma_demo")
    else()
        if(TILEXR_UDMA_DEMO_SOC_TYPE STREQUAL "Ascend950")
            set(TILEXR_UDMA_NPU_ARCH "dav-3510")
            set(TILEXR_UDMA_AICORE_ARCH "--cce-aicore-arch=dav-c310-vec")
            set(TILEXR_UDMA_CATLASS_ARCH "3510")
        else()
            set(TILEXR_UDMA_NPU_ARCH "dav-2201")
            set(TILEXR_UDMA_AICORE_ARCH "--cce-aicore-arch=dav-c220-vec")
            set(TILEXR_UDMA_CATLASS_ARCH "2201")
        endif()

        execute_process(
            COMMAND ${BISHENG_EXECUTABLE} -v
            OUTPUT_VARIABLE BISHENG_VERSION_OUTPUT
            ERROR_VARIABLE BISHENG_VERSION_OUTPUT
            RESULT_VARIABLE BISHENG_VERSION_RESULT
        )
        if(BISHENG_VERSION_RESULT EQUAL 0 AND BISHENG_VERSION_OUTPUT MATCHES "([0-9]{8})")
            set(TILEXR_UDMA_BISHENG_DATE "${CMAKE_MATCH_1}")
        else()
            set(TILEXR_UDMA_BISHENG_DATE "0")
        endif()
        if(TILEXR_UDMA_BISHENG_DATE GREATER_EQUAL 20250428)
            set(TILEXR_UDMA_KERNEL_COMPILE_OPTIONS
                -xasc
                -Xhost-start -ftrapv -Xhost-end
                --npu-arch=${TILEXR_UDMA_NPU_ARCH}
                --cce-auto-infer-kernel-type=false
            )
            set(TILEXR_UDMA_KERNEL_LINK_OPTIONS --cce-fatobj-link)
        else()
            set(TILEXR_UDMA_KERNEL_COMPILE_OPTIONS
                -xcce
                -Xhost-start -ftrapv -Xhost-end
                -mllvm -cce-aicore-stack-size=0x8000
                -mllvm -cce-aicore-function-stack-size=0x8000
                -mllvm -cce-aicore-record-overflow=true
                -mllvm -cce-aicore-addr-transform
                -mllvm -cce-aicore-dcci-insert-for-scalar=false
                ${TILEXR_UDMA_AICORE_ARCH}
            )
            set(TILEXR_UDMA_KERNEL_LINK_OPTIONS --cce-fatobj-link ${TILEXR_UDMA_AICORE_ARCH})
        endif()

        set(TILEXR_UDMA_DEMO_KERNEL_SO "${CMAKE_CURRENT_BINARY_DIR}/libtilexr_udma_demo_kernel.so")
        set(TILEXR_UDMA_DEMO_KERNEL_INCLUDES
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

        if(TILEXR_UDMA_DEMO_SOC_TYPE STREQUAL "Ascend950")
            find_program(TILEXR_UDMA_CCE_LINKER
                NAMES ld.lld cce-ld
                HINTS
                    "${ASCEND_HOME_PATH}/bin"
                    "${ASCEND_HOME_PATH}/tools/bisheng_compiler/bin"
            )
            if(NOT TILEXR_UDMA_CCE_LINKER)
                message(FATAL_ERROR "CCE linker not found in the selected CANN installation")
            endif()
            set(TILEXR_UDMA_PROFILE_PROBE_KERNEL_SOURCE
                "${CMAKE_CURRENT_SOURCE_DIR}/demo/tilexr_udma_profile_probe_kernel.cpp")
            set(TILEXR_UDMA_PROFILE_PROBE_KERNEL_RELOCATABLE
                "${CMAKE_CURRENT_BINARY_DIR}/tilexr_udma_profile_probe_kernel_rel.o")
            set(TILEXR_UDMA_PROFILE_PROBE_KERNEL_BINARY
                "${CMAKE_CURRENT_BINARY_DIR}/tilexr_udma_profile_probe_kernel.o")
            set(TILEXR_UDMA_PROFILE_PROBE_KERNEL_EMBED
                "${CMAKE_CURRENT_BINARY_DIR}/tilexr_udma_profile_probe_kernel_embed.cpp")
            set(TILEXR_UDMA_PROFILE_PROBE_KERNEL_INCLUDES
                ${TILEXR_UDMA_DEMO_KERNEL_INCLUDES}
                -I${ASCEND_HOME_PATH}/${ARCH}-linux/asc/include
                -I${ASCEND_HOME_PATH}/${ARCH}-linux/ascendc/include
            )
            add_custom_command(
                OUTPUT "${TILEXR_UDMA_PROFILE_PROBE_KERNEL_BINARY}"
                COMMAND ${BISHENG_EXECUTABLE}
                    -xcce
                    -mllvm -cce-aicore-stack-size=0x8000
                    -mllvm -cce-aicore-function-stack-size=0x8000
                    -mllvm -cce-aicore-record-overflow=true
                    -mllvm -cce-aicore-addr-transform
                    -mllvm -cce-aicore-dcci-insert-for-scalar=false
                    --cce-aicore-arch=dav-c310-vec
                    --cce-aicore-only
                    -std=gnu++17
                    ${TILEXR_UDMA_PROFILE_PROBE_KERNEL_INCLUDES}
                    -c "${TILEXR_UDMA_PROFILE_PROBE_KERNEL_SOURCE}"
                    -o "${TILEXR_UDMA_PROFILE_PROBE_KERNEL_RELOCATABLE}"
                COMMAND ${TILEXR_UDMA_CCE_LINKER}
                    -m aicorelinux -Ttext=0
                    "${TILEXR_UDMA_PROFILE_PROBE_KERNEL_RELOCATABLE}"
                    --static --allow-multiple-definition
                    -o "${TILEXR_UDMA_PROFILE_PROBE_KERNEL_BINARY}"
                DEPENDS
                    "${TILEXR_UDMA_PROFILE_PROBE_KERNEL_SOURCE}"
                    "${TILEXR_ROOT}/src/include/tilexr_udma.h"
                    "${TILEXR_ROOT}/src/include/tilexr_udma_reg.h"
                VERBATIM
                COMMENT "Building pure AICore TileXR UDMA profile probe kernel"
            )
            add_custom_target(tilexr_udma_profile_probe_kernel ALL
                DEPENDS "${TILEXR_UDMA_PROFILE_PROBE_KERNEL_BINARY}")
            add_custom_command(
                OUTPUT "${TILEXR_UDMA_PROFILE_PROBE_KERNEL_EMBED}"
                COMMAND ${CMAKE_COMMAND}
                    -DTILEXR_MOONEP_KERNEL_BINARY=${TILEXR_UDMA_PROFILE_PROBE_KERNEL_BINARY}
                    -DTILEXR_MOONEP_KERNEL_EMBED_CPP=${TILEXR_UDMA_PROFILE_PROBE_KERNEL_EMBED}
                    -DTILEXR_MOONEP_KERNEL_DATA_SYMBOL=TileXRUDMAProfileProbeKernelBinaryData
                    -DTILEXR_MOONEP_KERNEL_SIZE_SYMBOL=TileXRUDMAProfileProbeKernelBinarySize
                    -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_aicore_kernel.cmake"
                DEPENDS
                    "${TILEXR_UDMA_PROFILE_PROBE_KERNEL_BINARY}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_aicore_kernel.cmake"
                VERBATIM
                COMMENT "Embedding pure AICore TileXR UDMA profile probe kernel"
            )

            add_executable(tilexr_udma_profile_probe
                demo/tilexr_udma_profile_probe.cpp
                "${TILEXR_UDMA_PROFILE_PROBE_KERNEL_EMBED}"
            )
            add_dependencies(tilexr_udma_profile_probe tilexr_udma_profile_probe_kernel)
            target_link_libraries(tilexr_udma_profile_probe
                ${TILEXR_LIB}
                ascendcl
                runtime
                ascend_hal
            )
            set_target_properties(tilexr_udma_profile_probe PROPERTIES
                BUILD_WITH_INSTALL_RPATH TRUE
                INSTALL_RPATH "$ORIGIN/../lib"
            )
            list(APPEND INSTALL_TARGETS tilexr_udma_profile_probe)
            message(STATUS "TileXR UDMA persistent-profile hardware probe enabled")

            set(TILEXR_UDMA_PEERMEM_PROBE_KERNEL_SOURCE
                "${CMAKE_CURRENT_SOURCE_DIR}/demo/tilexr_udma_peermem_probe_kernel.cpp")
            set(TILEXR_UDMA_PEERMEM_PROBE_KERNEL_RELOCATABLE
                "${CMAKE_CURRENT_BINARY_DIR}/tilexr_udma_peermem_probe_kernel_rel.o")
            set(TILEXR_UDMA_PEERMEM_PROBE_KERNEL_BINARY
                "${CMAKE_CURRENT_BINARY_DIR}/tilexr_udma_peermem_probe_kernel.o")
            set(TILEXR_UDMA_PEERMEM_PROBE_KERNEL_EMBED
                "${CMAKE_CURRENT_BINARY_DIR}/tilexr_udma_peermem_probe_kernel_embed.cpp")
            add_custom_command(
                OUTPUT "${TILEXR_UDMA_PEERMEM_PROBE_KERNEL_BINARY}"
                COMMAND ${BISHENG_EXECUTABLE}
                    -xcce
                    -mllvm -cce-aicore-stack-size=0x8000
                    -mllvm -cce-aicore-function-stack-size=0x8000
                    -mllvm -cce-aicore-record-overflow=true
                    -mllvm -cce-aicore-addr-transform
                    -mllvm -cce-aicore-dcci-insert-for-scalar=false
                    --cce-aicore-arch=dav-c310-vec
                    --cce-aicore-only
                    -std=gnu++17
                    ${TILEXR_UDMA_PROFILE_PROBE_KERNEL_INCLUDES}
                    -I${CMAKE_CURRENT_SOURCE_DIR}/demo
                    -c "${TILEXR_UDMA_PEERMEM_PROBE_KERNEL_SOURCE}"
                    -o "${TILEXR_UDMA_PEERMEM_PROBE_KERNEL_RELOCATABLE}"
                COMMAND ${TILEXR_UDMA_CCE_LINKER}
                    -m aicorelinux -Ttext=0
                    "${TILEXR_UDMA_PEERMEM_PROBE_KERNEL_RELOCATABLE}"
                    --static --allow-multiple-definition
                    -o "${TILEXR_UDMA_PEERMEM_PROBE_KERNEL_BINARY}"
                DEPENDS
                    "${TILEXR_UDMA_PEERMEM_PROBE_KERNEL_SOURCE}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/demo/tilexr_udma_peermem_probe_layout.h"
                    "${TILEXR_ROOT}/src/include/tilexr_udma.h"
                VERBATIM
                COMMENT "Building pure AICore TileXR UDMA peer-memory probe kernel"
            )
            add_custom_target(tilexr_udma_peermem_probe_kernel ALL
                DEPENDS "${TILEXR_UDMA_PEERMEM_PROBE_KERNEL_BINARY}")
            add_custom_command(
                OUTPUT "${TILEXR_UDMA_PEERMEM_PROBE_KERNEL_EMBED}"
                COMMAND ${CMAKE_COMMAND}
                    -DTILEXR_MOONEP_KERNEL_BINARY=${TILEXR_UDMA_PEERMEM_PROBE_KERNEL_BINARY}
                    -DTILEXR_MOONEP_KERNEL_EMBED_CPP=${TILEXR_UDMA_PEERMEM_PROBE_KERNEL_EMBED}
                    -DTILEXR_MOONEP_KERNEL_DATA_SYMBOL=TileXRUDMAPeerMemProbeKernelBinaryData
                    -DTILEXR_MOONEP_KERNEL_SIZE_SYMBOL=TileXRUDMAPeerMemProbeKernelBinarySize
                    -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_aicore_kernel.cmake"
                DEPENDS
                    "${TILEXR_UDMA_PEERMEM_PROBE_KERNEL_BINARY}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_aicore_kernel.cmake"
                VERBATIM
                COMMENT "Embedding pure AICore TileXR UDMA peer-memory probe kernel"
            )
            add_executable(tilexr_udma_peermem_probe
                demo/tilexr_udma_peermem_probe.cpp
                "${TILEXR_UDMA_PEERMEM_PROBE_KERNEL_EMBED}"
            )
            add_dependencies(tilexr_udma_peermem_probe tilexr_udma_peermem_probe_kernel)
            target_include_directories(tilexr_udma_peermem_probe PRIVATE
                ${CMAKE_CURRENT_SOURCE_DIR}/demo)
            target_link_libraries(tilexr_udma_peermem_probe
                ${TILEXR_LIB}
                ascendcl
                runtime
                ascend_hal
            )
            set_target_properties(tilexr_udma_peermem_probe PROPERTIES
                BUILD_WITH_INSTALL_RPATH TRUE
                INSTALL_RPATH "$ORIGIN/../lib"
            )
            list(APPEND INSTALL_TARGETS tilexr_udma_peermem_probe)
            message(STATUS "TileXR UDMA peer-memory alias hardware probe enabled")
        endif()

        add_custom_command(
            OUTPUT "${TILEXR_UDMA_DEMO_KERNEL_SO}"
            COMMAND ${BISHENG_EXECUTABLE}
                ${TILEXR_UDMA_KERNEL_COMPILE_OPTIONS}
                -std=gnu++17
                -fPIC
                -shared
                ${TILEXR_UDMA_KERNEL_LINK_OPTIONS}
                -DCATLASS_ARCH=${TILEXR_UDMA_CATLASS_ARCH}
                ${TILEXR_UDMA_DEMO_KERNEL_INCLUDES}
                "${CMAKE_CURRENT_SOURCE_DIR}/demo/tilexr_udma_demo_kernel.cpp"
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
                -o "${TILEXR_UDMA_DEMO_KERNEL_SO}"
            DEPENDS
                "${CMAKE_CURRENT_SOURCE_DIR}/demo/tilexr_udma_demo_kernel.cpp"
                "${TILEXR_ROOT}/src/include/tilexr_udma.h"
            VERBATIM
            COMMENT "Building TileXR UDMA demo kernel with bisheng"
        )
        add_custom_target(tilexr_udma_demo_kernel ALL DEPENDS "${TILEXR_UDMA_DEMO_KERNEL_SO}")

        add_executable(tilexr_udma_demo
            demo/tilexr_udma_demo.cpp
        )
        add_dependencies(tilexr_udma_demo tilexr_udma_demo_kernel)
        target_link_directories(tilexr_udma_demo PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
        target_link_libraries(tilexr_udma_demo
            "${TILEXR_UDMA_DEMO_KERNEL_SO}"
            ${TILEXR_LIB}
            ascendcl
            runtime
            ascend_hal
        )
        list(APPEND INSTALL_TARGETS tilexr_udma_demo)
        install(FILES "${TILEXR_UDMA_DEMO_KERNEL_SO}" DESTINATION ${CMAKE_INSTALL_PREFIX}/lib)
        message(STATUS "TileXR UDMA demo enabled with ${BISHENG_EXECUTABLE}, SOC=${TILEXR_UDMA_DEMO_SOC_TYPE}")
    endif()
endif()

# 安装目标
install(TARGETS ${INSTALL_TARGETS}
    RUNTIME DESTINATION ${CMAKE_INSTALL_PREFIX}/bin
    LIBRARY DESTINATION ${CMAKE_INSTALL_PREFIX}/lib
)
