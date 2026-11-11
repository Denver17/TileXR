# 功能：按指定 SoC 编译 clock sync 和 sleep_at，链接并填充独立 AICore binary。
# 调用关系：diagnostics Host 构建读取导出的 artifact 路径，clock launcher 在运行时加载该文件。
list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake/cce")
enable_language(CCE)

set(TILEXR_DIAGNOSTICS_1OP_BIN_SIZE 1048576 CACHE STRING
    "Padded size for TileXR diagnostics CCE binary")
set(_tilexr_diagnostics_default_soc "$ENV{TILEXR_SOC_NAME}")
set(TILEXR_DIAGNOSTICS_SOC_TYPE "${_tilexr_diagnostics_default_soc}" CACHE STRING
    "SoC type used for TileXR diagnostics CCE kernels")
if(NOT TILEXR_DIAGNOSTICS_SOC_TYPE)
    message(FATAL_ERROR
        "TILEXR_DIAGNOSTICS_SOC_TYPE is required; set TILEXR_SOC_NAME or pass the cache variable explicitly")
endif()

string(TOLOWER "${TILEXR_DIAGNOSTICS_SOC_TYPE}" TILEXR_DIAGNOSTICS_SOC_TYPE_NORMALIZED)
if(TILEXR_DIAGNOSTICS_SOC_TYPE_NORMALIZED MATCHES "ascend950|ascend910_9|ascend910-9|a5")
    set(TILEXR_DIAGNOSTICS_AICORE_ARCH dav-c310-vec)
elseif(TILEXR_DIAGNOSTICS_SOC_TYPE_NORMALIZED MATCHES "ascend910b|910b")
    set(TILEXR_DIAGNOSTICS_AICORE_ARCH dav-c220-vec)
else()
    message(FATAL_ERROR
        "Unsupported TILEXR_DIAGNOSTICS_SOC_TYPE=${TILEXR_DIAGNOSTICS_SOC_TYPE}; refusing an implicit AICore fallback")
endif()
message(STATUS
    "TileXR diagnostics SoC ${TILEXR_DIAGNOSTICS_SOC_TYPE} -> ${TILEXR_DIAGNOSTICS_AICORE_ARCH}")

set_source_files_properties(clock_sync_kernel.cpp clock_sleep_at_kernel.cpp PROPERTIES LANGUAGE CCE)
add_library(tilexr_diagnostics_op_tmp OBJECT clock_sync_kernel.cpp clock_sleep_at_kernel.cpp)
option(TILEXR_DIAGNOSTICS_CLOCK_PROFILE "Instrument individual clock RDMA sends" OFF)
target_compile_definitions(tilexr_diagnostics_op_tmp PRIVATE
    TILEXR_DIAGNOSTICS_CLOCK_PROFILE=$<BOOL:${TILEXR_DIAGNOSTICS_CLOCK_PROFILE}>)
target_include_directories(tilexr_diagnostics_op_tmp PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/src/include
    ${ASCEND_HOME_PATH}/${ARCH}-linux/pkg_inc
    ${ASCEND_HOME_PATH}/${ARCH}-linux/pkg_inc/runtime
    ${ASCEND_HOME_PATH}/${ARCH}-linux/include
    ${ASCEND_HOME_PATH}/${ARCH}-linux/include/ascendc
    ${ASCEND_HOME_PATH}/${ARCH}-linux/ascendc/include
    ${ASCEND_HOME_PATH}/${ARCH}-linux/tikcpp/tikcfw
    ${ASCEND_HOME_PATH}/${ARCH}-linux/tikcpp/tikcfw/interface
    ${ASCEND_HOME_PATH}/${ARCH}-linux/tikcpp/tikcfw/impl
    ${ASCEND_HOME_PATH}/toolkit/toolchain/hcc/aarch64-target-linux-gnu/include/c++/7.3.0
    ${ASCEND_HOME_PATH}/toolkit/toolchain/hcc/aarch64-target-linux-gnu/include/c++/7.3.0/aarch64-target-linux-gnu)
target_compile_options(tilexr_diagnostics_op_tmp PRIVATE
    -O2
    -std=gnu++17
    --cce-aicore-only
    -Wno-deprecated-declarations
    "SHELL:-mllvm -cce-aicore-long-call"
    "SHELL:-mllvm -cce-aicore-function-stack-size=16000"
    "SHELL:-mllvm -cce-aicore-record-overflow=false"
    "SHELL:-mllvm -cce-aicore-addr-transform"
    "SHELL:-mllvm --cce-aicore-jump-expand=true"
    --cce-aicore-arch=${TILEXR_DIAGNOSTICS_AICORE_ARCH})

set(TILEXR_DIAGNOSTICS_OP "${CMAKE_CURRENT_BINARY_DIR}/tilexr_diagnostics_op.o")
add_custom_command(
    OUTPUT "${TILEXR_DIAGNOSTICS_OP}"
    COMMAND ${CMAKE_CCE_LINKER} -m aicorelinux -Ttext=0
        $<TARGET_OBJECTS:tilexr_diagnostics_op_tmp>
        --static -o "${TILEXR_DIAGNOSTICS_OP}" --allow-multiple-definition
    COMMAND truncate -c -s ${TILEXR_DIAGNOSTICS_1OP_BIN_SIZE} "${TILEXR_DIAGNOSTICS_OP}"
    DEPENDS tilexr_diagnostics_op_tmp $<TARGET_OBJECTS:tilexr_diagnostics_op_tmp>
    COMMAND_EXPAND_LISTS
    VERBATIM)
add_custom_target(tilexr_diagnostics_op DEPENDS "${TILEXR_DIAGNOSTICS_OP}")

set(TILEXR_DIAGNOSTICS_OP "${TILEXR_DIAGNOSTICS_OP}" PARENT_SCOPE)
set(TILEXR_DIAGNOSTICS_AICORE_ARCH "${TILEXR_DIAGNOSTICS_AICORE_ARCH}" PARENT_SCOPE)
