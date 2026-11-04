#
# Copyright (c) 2024-2026 TileXR Project
# CMakeLists.txt for collectives-owned CCE kernels
#

list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake/cce")
enable_language(CCE)

set(TILEXR_COLLECTIVES_1OP_BIN_SIZE 10485760 CACHE STRING "Padded size for TileXR collectives CCE binary")
set(_tilexr_collectives_default_soc_type "$ENV{TILEXR_COLLECTIVES_SOC_TYPE}")
if(NOT _tilexr_collectives_default_soc_type)
    set(_tilexr_collectives_default_soc_type "$ENV{TILEXR_SOC_NAME}")
endif()
set(TILEXR_COLLECTIVES_SOC_TYPE "" CACHE STRING "SOC type used for TileXR collectives CCE kernels")
if(NOT TILEXR_COLLECTIVES_SOC_TYPE AND _tilexr_collectives_default_soc_type)
    set(TILEXR_COLLECTIVES_SOC_TYPE "${_tilexr_collectives_default_soc_type}" CACHE STRING
        "SOC type used for TileXR collectives CCE kernels" FORCE)
endif()

file(GLOB_RECURSE TILEXR_COLLECTIVES_KERNEL_DEPENDS CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/*.h"
        "${CMAKE_SOURCE_DIR}/src/include/tilexr/profiling.h"
        "${CMAKE_SOURCE_DIR}/src/include/tilexr/profiling/*.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/kernels/*.cce"
)

# Rank-sized AllGather state exceeds the former 16,000-byte function limit
# with the 256-rank ABI. Keep function and total stack budgets at 32 KiB.
set(CCE_COMPILE_OPTION
        -O2
        -std=gnu++17
        --cce-aicore-only
        -Wno-deprecated-declarations
        "SHELL:-mllvm -cce-aicore-long-call"
        "SHELL:-mllvm -cce-aicore-stack-size=0x8000"
        "SHELL:-mllvm -cce-aicore-function-stack-size=0x8000"
        "SHELL:-mllvm -cce-aicore-record-overflow=false"
        "SHELL:-mllvm -cce-aicore-addr-transform"
        "SHELL:-mllvm --cce-aicore-jump-expand=true"
)

string(TOLOWER "${TILEXR_COLLECTIVES_SOC_TYPE}" TILEXR_COLLECTIVES_SOC_TYPE_NORMALIZED)
if(TILEXR_COLLECTIVES_SOC_TYPE_NORMALIZED MATCHES "^ascend950")
    set(TILEXR_COLLECTIVES_AICORE_ARCH dav-c310-vec)
else()
    set(TILEXR_COLLECTIVES_AICORE_ARCH dav-c220-vec)
endif()
set_source_files_properties(tilexr_lccl_op.cpp PROPERTIES LANGUAGE CCE)
set_source_files_properties(tilexr_lccl_op.cpp PROPERTIES
        OBJECT_DEPENDS "${TILEXR_COLLECTIVES_KERNEL_DEPENDS}"
)

include_directories(
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/kernels
        ${CMAKE_SOURCE_DIR}/src/include
        ${ASCEND_HOME_PATH}/${ARCH}-linux/pkg_inc/
        ${ASCEND_HOME_PATH}/${ARCH}-linux/pkg_inc/runtime/
        ${ASCEND_HOME_PATH}/${ARCH}-linux/include/
        ${ASCEND_HOME_PATH}/${ARCH}-linux/include/ascendc
        ${ASCEND_HOME_PATH}/${ARCH}-linux/ascendc/include
        ${ASCEND_HOME_PATH}/${ARCH}-linux/tikcpp/tikcfw/
        ${ASCEND_HOME_PATH}/${ARCH}-linux/tikcpp/tikcfw/interface/
        ${ASCEND_HOME_PATH}/${ARCH}-linux/tikcpp/tikcfw/impl/
        ${ASCEND_HOME_PATH}/toolkit/toolchain/hcc/aarch64-target-linux-gnu/include/c++/7.3.0
        ${ASCEND_HOME_PATH}/toolkit/toolchain/hcc/aarch64-target-linux-gnu/include/c++/7.3.0/aarch64-target-linux-gnu/
)

add_library(tilexr_collectives_op_tmp OBJECT
        tilexr_lccl_op.cpp
)

target_compile_options(tilexr_collectives_op_tmp PRIVATE
        ${CCE_COMPILE_OPTION}
        --cce-aicore-arch=${TILEXR_COLLECTIVES_AICORE_ARCH}
)
if(TILEXR_COLLECTIVES_ENABLE_PROFILING)
    target_compile_definitions(tilexr_collectives_op_tmp PRIVATE TILEXR_ENABLE_PROFILING=1)
endif()

set(TILEXR_COLLECTIVES_OP "${CMAKE_CURRENT_BINARY_DIR}/tilexr_collectives_op.o")
add_custom_command(
        OUTPUT "${TILEXR_COLLECTIVES_OP}"
        COMMAND ${CMAKE_CCE_LINKER} -m aicorelinux -Ttext=0
                $<TARGET_OBJECTS:tilexr_collectives_op_tmp>
                --static -o "${TILEXR_COLLECTIVES_OP}" --allow-multiple-definition
        COMMAND truncate -c -s ${TILEXR_COLLECTIVES_1OP_BIN_SIZE} "${TILEXR_COLLECTIVES_OP}"
        DEPENDS tilexr_collectives_op_tmp $<TARGET_OBJECTS:tilexr_collectives_op_tmp>
        COMMAND_EXPAND_LISTS
        VERBATIM
)

add_custom_target(tilexr_collectives_op DEPENDS "${TILEXR_COLLECTIVES_OP}")
set(TILEXR_COLLECTIVES_OP "${TILEXR_COLLECTIVES_OP}" PARENT_SCOPE)
set(TILEXR_COLLECTIVES_1OP_BIN_SIZE "${TILEXR_COLLECTIVES_1OP_BIN_SIZE}" PARENT_SCOPE)
set(TILEXR_COLLECTIVES_AICORE_ARCH "${TILEXR_COLLECTIVES_AICORE_ARCH}" PARENT_SCOPE)
