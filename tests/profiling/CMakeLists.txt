cmake_minimum_required(VERSION 3.16)

option(TILEXR_BUILD_PROFILING_DEMO "Build the profiled Add4 hardware demo" OFF)
set(TILEXR_PROFILING_DEMO_SOC_TYPE "Ascend950" CACHE STRING
    "SoC type used for the profiled Add4 demo")

set(TILEXR_PROFILING_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../..")

add_executable(test_profiled_add4_demo_sources
    unit/test_profiled_add4_demo_sources.cpp
)
target_compile_features(test_profiled_add4_demo_sources PRIVATE cxx_std_14)
target_compile_definitions(test_profiled_add4_demo_sources PRIVATE
    TILEXR_SOURCE_ROOT="${TILEXR_PROFILING_ROOT}"
)
add_test(NAME test_profiled_add4_demo_sources COMMAND test_profiled_add4_demo_sources)

add_executable(test_profiling_runtime
    unit/test_profiling_runtime.cpp
    ${TILEXR_PROFILING_ROOT}/src/comm/tilexr_profiling_runtime.cpp
)
target_compile_features(test_profiling_runtime PRIVATE cxx_std_14)
target_include_directories(test_profiling_runtime PRIVATE
    ${TILEXR_PROFILING_ROOT}/src/comm
    ${TILEXR_PROFILING_ROOT}/src/include
)
add_test(NAME test_profiling_runtime COMMAND test_profiling_runtime)

if(NOT TILEXR_BUILD_PROFILING_DEMO)
    return()
endif()

if(NOT TILEXR_PROFILING_DEMO_SOC_TYPE STREQUAL "Ascend950")
    message(FATAL_ERROR
        "The profiled Add4 demo currently supports TILEXR_PROFILING_DEMO_SOC_TYPE=Ascend950 only")
endif()

find_program(TILEXR_PROFILING_BISHENG NAMES bisheng)
if(NOT TILEXR_PROFILING_BISHENG)
    message(FATAL_ERROR "bisheng was not found in the active CANN environment")
endif()
find_program(TILEXR_PROFILING_CCE_LINKER
    NAMES ld.lld cce-ld
    HINTS
        "${ASCEND_HOME_PATH}/bin"
        "${ASCEND_HOME_PATH}/tools/bisheng_compiler/bin"
)
if(NOT TILEXR_PROFILING_CCE_LINKER)
    message(FATAL_ERROR "CCE linker was not found in the active CANN environment")
endif()

set(TILEXR_PROFILED_ADD4_KERNEL_SOURCE
    "${CMAKE_CURRENT_SOURCE_DIR}/demo/profiled_add4_kernel.cpp")
set(TILEXR_PROFILED_ADD4_PUBLIC_HEADER
    "${TILEXR_PROFILING_ROOT}/src/include/tilexr_profiling.h")
set(TILEXR_PROFILED_ADD4_KERNEL_RELOCATABLE
    "${CMAKE_CURRENT_BINARY_DIR}/profiled_add4_kernel_rel.o")
set(TILEXR_PROFILED_ADD4_KERNEL_BINARY
    "${CMAKE_CURRENT_BINARY_DIR}/profiled_add4_kernel.o")
set(TILEXR_PROFILED_ADD4_KERNEL_EMBED
    "${CMAKE_CURRENT_BINARY_DIR}/profiled_add4_kernel_embed.cpp")

set(TILEXR_PROFILED_ADD4_KERNEL_INCLUDES
    -I${ASCEND_HOME_PATH}/compiler/tikcpp
    -I${ASCEND_HOME_PATH}/compiler/tikcpp/tikcfw
    -I${ASCEND_HOME_PATH}/compiler/tikcpp/tikcfw/impl
    -I${ASCEND_HOME_PATH}/compiler/tikcpp/tikcfw/interface
    -I${ASCEND_HOME_PATH}/${ARCH}-linux/tikcpp
    -I${ASCEND_HOME_PATH}/${ARCH}-linux/tikcpp/tikcfw
    -I${ASCEND_HOME_PATH}/${ARCH}-linux/tikcpp/tikcfw/impl
    -I${ASCEND_HOME_PATH}/${ARCH}-linux/tikcpp/tikcfw/interface
    -I${ASCEND_HOME_PATH}/${ARCH}-linux/pkg_inc
    -I${ASCEND_HOME_PATH}/${ARCH}-linux/pkg_inc/runtime
    -I${ASCEND_HOME_PATH}/${ARCH}-linux/include
    -I${ASCEND_HOME_PATH}/${ARCH}-linux/asc/include
    -I${ASCEND_HOME_PATH}/${ARCH}-linux/ascendc/include
    -I${ASCEND_DRIVER_PATH}/kernel/inc
    -I${TILEXR_PROFILING_ROOT}/src/include
)

add_custom_command(
    OUTPUT "${TILEXR_PROFILED_ADD4_KERNEL_BINARY}"
    COMMAND ${TILEXR_PROFILING_BISHENG}
        -xcce
        -mllvm -cce-aicore-stack-size=0x8000
        -mllvm -cce-aicore-function-stack-size=0x8000
        -mllvm -cce-aicore-record-overflow=true
        -mllvm -cce-aicore-addr-transform
        -mllvm -cce-aicore-dcci-insert-for-scalar=false
        --cce-aicore-arch=dav-c310-vec
        --cce-aicore-only
        -std=gnu++17
        -DTILEXR_ENABLE_PROFILING=1
        ${TILEXR_PROFILED_ADD4_KERNEL_INCLUDES}
        -c "${TILEXR_PROFILED_ADD4_KERNEL_SOURCE}"
        -o "${TILEXR_PROFILED_ADD4_KERNEL_RELOCATABLE}"
    COMMAND ${TILEXR_PROFILING_CCE_LINKER}
        -m aicorelinux -Ttext=0
        "${TILEXR_PROFILED_ADD4_KERNEL_RELOCATABLE}"
        --static --allow-multiple-definition
        -o "${TILEXR_PROFILED_ADD4_KERNEL_BINARY}"
    DEPENDS
        "${TILEXR_PROFILED_ADD4_KERNEL_SOURCE}"
        "${TILEXR_PROFILED_ADD4_PUBLIC_HEADER}"
        "${TILEXR_PROFILING_ROOT}/src/include/comm_args.h"
    VERBATIM
    COMMENT "Building pure AICore profiled Add4 kernel"
)
add_custom_target(tilexr_profiled_add4_kernel
    DEPENDS "${TILEXR_PROFILED_ADD4_KERNEL_BINARY}")

add_custom_command(
    OUTPUT "${TILEXR_PROFILED_ADD4_KERNEL_EMBED}"
    COMMAND ${CMAKE_COMMAND}
        -DTILEXR_KERNEL_BINARY=${TILEXR_PROFILED_ADD4_KERNEL_BINARY}
        -DTILEXR_KERNEL_EMBED_CPP=${TILEXR_PROFILED_ADD4_KERNEL_EMBED}
        -DTILEXR_KERNEL_DATA_SYMBOL=TileXRProfiledAdd4KernelBinaryData
        -DTILEXR_KERNEL_SIZE_SYMBOL=TileXRProfiledAdd4KernelBinarySize
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_aicore_kernel.cmake"
    DEPENDS
        "${TILEXR_PROFILED_ADD4_KERNEL_BINARY}"
        "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_aicore_kernel.cmake"
    VERBATIM
    COMMENT "Embedding pure AICore profiled Add4 kernel"
)

add_executable(tilexr_profiled_add4_demo
    demo/profiled_add4_host.cpp
    "${TILEXR_PROFILED_ADD4_KERNEL_EMBED}"
)
add_dependencies(tilexr_profiled_add4_demo tilexr_profiled_add4_kernel)
target_compile_features(tilexr_profiled_add4_demo PRIVATE cxx_std_14)
target_include_directories(tilexr_profiled_add4_demo PRIVATE
    ${TILEXR_PROFILING_ROOT}/src/include
    ${ASCEND_HOME_PATH}/${ARCH}-linux/pkg_inc
    ${ASCEND_HOME_PATH}/${ARCH}-linux/pkg_inc/runtime
    ${ASCEND_HOME_PATH}/${ARCH}-linux/include
    ${ASCEND_DRIVER_PATH}/kernel/inc
)
target_link_directories(tilexr_profiled_add4_demo PRIVATE
    ${ASCEND_DRIVER_PATH}/lib64/driver
    ${ASCEND_HOME_PATH}/${ARCH}-linux/lib64
)
target_link_libraries(tilexr_profiled_add4_demo PRIVATE
    tile-comm
    ascendcl
    runtime
    ascend_hal
)
