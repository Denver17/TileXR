# Copyright (c) 2024 Huawei Technologies Co., Ltd.
# This file is a part of the CANN Open Software.
# Licensed under CANN Open Software License Agreement Version 1.0 (the "License").
# Please refer to the License for details. You may not use this file except in compliance with the License.
# THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
# INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
# See LICENSE in the root of the software repository for the full text of the License.
#

# 功能：检测可选传输所需的头文件和库，构建 libtile-comm 并安装公共头文件。
# 调用关系：由根 CMakeLists.txt 引入；为 src/comm 下的实现设置 CANN/driver 依赖和编译选项。
#
# Purpose: Probe optional transport dependencies, build libtile-comm and install public headers.
# Calls: Included by the root CMakeLists.txt; sets CANN/driver dependencies and compile options for src/comm.
include(GNUInstallDirs)
include(CMakePushCheckState)
include(CheckIncludeFileCXX)
include(CheckCXXSourceCompiles)

option(TILEXR_BUILD_RDMA "Build optional standard verbs RDMA support when headers are available" ON)
if(TILEXR_BUILD_RDMA)
    check_include_file_cxx("infiniband/verbs.h" TILEXR_HAVE_IBVERBS_HEADER)
    find_path(TILEXR_IBV_EXTEND_INCLUDE ibv_extend.h
        HINTS "${ASCEND_DRIVER_PATH}/include")
    if(TILEXR_IBV_EXTEND_INCLUDE AND TILEXR_HAVE_IBVERBS_HEADER)
        cmake_push_check_state(RESET)
        set(CMAKE_REQUIRED_INCLUDES "${TILEXR_IBV_EXTEND_INCLUDE}")
        check_cxx_source_compiles("
#include <ibv_extend.h>
#if IBV_EXTEND_VERSION_MAJOR != 4 || IBV_EXTEND_VERSION_MINOR < 2
#error unsupported extension ABI
#endif
int main() { queue_info q{}; ibv_extend_ops ops{}; (void)q; (void)ops; return 0; }
" TILEXR_HAVE_IBV_EXTEND_HEADER)
        cmake_pop_check_state()
    endif()
endif()

set(TILEXR_PTO_SDMA_INCLUDE_DIRS
        ${ASCEND_HOME_PATH}/${ARCH}-linux/include
        ${ASCEND_HOME_PATH}/${ARCH}-linux/pkg_inc
        ${ASCEND_DRIVER_PATH}/kernel/inc)

unset(TILEXR_HAVE_PTO_SDMA_WORKSPACE CACHE)
unset(TILEXR_HAVE_PTO_SDMA_WORKSPACE)
unset(TILEXR_HAVE_PTO_SDMA_WORKSPACE_API CACHE)
unset(TILEXR_HAVE_PTO_SDMA_WORKSPACE_API)
unset(TILEXR_PTO_NNOPBASE_LIB CACHE)
unset(TILEXR_PTO_NNOPBASE_LIB)
unset(TILEXR_PTO_NNOPBASE_DEVLIB CACHE)
unset(TILEXR_PTO_NNOPBASE_DEVLIB)

cmake_push_check_state(RESET)
set(CMAKE_REQUIRED_INCLUDES
        ${TILEXR_PTO_SDMA_INCLUDE_DIRS})
check_include_file_cxx("pto/npu/comm/async/sdma/sdma_workspace_manager.hpp" TILEXR_HAVE_PTO_SDMA_WORKSPACE)

if(DEFINED CMAKE_TRY_COMPILE_TARGET_TYPE)
    set(_tilexr_had_try_compile_target_type ON)
    set(_tilexr_saved_try_compile_target_type "${CMAKE_TRY_COMPILE_TARGET_TYPE}")
else()
    set(_tilexr_had_try_compile_target_type OFF)
endif()
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
check_cxx_source_compiles("
#include <type_traits>
#include \"pto/npu/comm/async/sdma/sdma_workspace_manager.hpp\"
int main()
{
    pto::comm::sdma::SdmaWorkspaceManager manager;
    static_assert(std::is_same<decltype(manager.Init()), bool>::value, \"Init must return bool\");
    static_assert(std::is_same<decltype(manager.Finalize()), void>::value, \"Finalize must return void\");
    void *workspace = manager.GetWorkspaceAddr();
    (void)workspace;
    return 0;
}
" TILEXR_HAVE_PTO_SDMA_WORKSPACE_API)
if(_tilexr_had_try_compile_target_type)
    set(CMAKE_TRY_COMPILE_TARGET_TYPE "${_tilexr_saved_try_compile_target_type}")
else()
    unset(CMAKE_TRY_COMPILE_TARGET_TYPE)
endif()
cmake_pop_check_state()

set(TILEXR_HAVE_PTO_SDMA_INTRIN OFF)
foreach(_tilexr_pto_sdma_include_dir IN LISTS TILEXR_PTO_SDMA_INCLUDE_DIRS)
    if(EXISTS "${_tilexr_pto_sdma_include_dir}/pto/npu/comm/async/sdma/sdma_async_intrin.hpp")
        set(TILEXR_HAVE_PTO_SDMA_INTRIN ON)
    endif()
endforeach()

set(_tilexr_pto_nnopbase_runtime "${ASCEND_HOME_PATH}/${ARCH}-linux/lib64/libnnopbase.so")
set(_tilexr_pto_nnopbase_devlib "${ASCEND_HOME_PATH}/${ARCH}-linux/devlib/libnnopbase.so")
if(EXISTS "${_tilexr_pto_nnopbase_runtime}")
    set(TILEXR_PTO_NNOPBASE_LIB "${_tilexr_pto_nnopbase_runtime}")
elseif(EXISTS "${_tilexr_pto_nnopbase_devlib}")
    set(TILEXR_PTO_NNOPBASE_DEVLIB "${_tilexr_pto_nnopbase_devlib}")
endif()

if(TILEXR_HAVE_PTO_SDMA_WORKSPACE AND
        TILEXR_HAVE_PTO_SDMA_WORKSPACE_API AND
        TILEXR_HAVE_PTO_SDMA_INTRIN AND
        TILEXR_PTO_NNOPBASE_LIB)
    set(TILEXR_HAVE_PTO_SDMA ON)
else()
    set(TILEXR_HAVE_PTO_SDMA OFF)
endif()
message(STATUS "TILEXR_HAVE_PTO_SDMA: ${TILEXR_HAVE_PTO_SDMA}")
if(TILEXR_PTO_NNOPBASE_DEVLIB AND NOT TILEXR_PTO_NNOPBASE_LIB)
    message(WARNING
            "PTO SDMA headers were found but libnnopbase.so is only present in CANN devlib; "
            "disable TILEXR_HAVE_PTO_SDMA to avoid a runtime dependency on devlib HAL.")
endif()

if(TILEXR_HAVE_PTO_SDMA)
    set(TILEXR_HAVE_PTO_SDMA_VALUE 1)
else()
    set(TILEXR_HAVE_PTO_SDMA_VALUE 0)
endif()

configure_file(
        ${CMAKE_CURRENT_SOURCE_DIR}/../include/tilexr_sdma_config.h.in
        ${CMAKE_CURRENT_BINARY_DIR}/include/tilexr_sdma_config.h
        @ONLY)

set(TILEXR_SOURCE_FILE tilexr_comm.cpp
        ep_profiling_compat/runtime.cpp
        ep_profiling_compat/capture.cpp
        rdma/tilexr_rdma_api.cpp
        rdma/tilexr_rdma_config.cpp
        rdma/tilexr_rdma_transport.cpp
        rdma/tilexr_rdma_verbs.cpp
        rdma/tilexr_rdma_nda.cpp
        comm_wrap.cpp
        tilexr_internal.cpp
        tilexr_internal.h
        tilexr_log.h
        ccu/tilexr_ccu_abi_constants.h
        ccu/tilexr_ccu_alltoall_program.h
        ccu/tilexr_ccu_alltoall_program.cpp
        ccu/tilexr_ccu_barrier_program.h
        ccu/tilexr_ccu_barrier_program.cpp
        ccu/tilexr_ccu_backend.h
        ccu/tilexr_ccu_backend.cpp
        ccu/tilexr_ccu_collective_planner.h
        ccu/tilexr_ccu_collective_planner.cpp
        ccu/tilexr_ccu_direct_orchestrator.h
        ccu/tilexr_ccu_direct_orchestrator.cpp
        ccu/tilexr_ccu_direct_runtime.h
        ccu/tilexr_ccu_direct_runtime.cpp
        ccu/tilexr_ccu_driver_adapter.h
        ccu/tilexr_ccu_driver_adapter.cpp
        ccu/tilexr_ccu_hccp_types.h
        ccu/tilexr_ccu_hccp_loader.h
        ccu/tilexr_ccu_hccp_loader.cpp
        ccu/tilexr_ccu_launch_package.h
        ccu/tilexr_ccu_launch_package.cpp
        ccu/tilexr_ccu_install_provider.h
        ccu/tilexr_ccu_install_provider.cpp
        ccu/tilexr_ccu_lower_layer_plan_builder.h
        ccu/tilexr_ccu_lower_layer_plan_builder.cpp
        ccu/tilexr_ccu_lower_layer_payloads.h
        ccu/tilexr_ccu_lower_layer_payloads.cpp
        ccu/tilexr_ccu_memory_program.h
        ccu/tilexr_ccu_memory_program.cpp
        ccu/tilexr_ccu_microcode.h
        ccu/tilexr_ccu_microcode.cpp
        ccu/tilexr_ccu_producer_plan.h
        ccu/tilexr_ccu_producer_plan.cpp
        ccu/tilexr_ccu_provider.h
        ccu/tilexr_ccu_provider.cpp
        ccu/tilexr_ccu_ra_custom_channel_provider.h
        ccu/tilexr_ccu_ra_custom_channel_provider.cpp
        ccu/tilexr_ccu_repository.h
        ccu/tilexr_ccu_repository.cpp
        ccu/tilexr_ccu_resource_allocator.h
        ccu/tilexr_ccu_resource_allocator.cpp
        ccu/tilexr_ccu_runtime.h
        ccu/tilexr_ccu_runtime.cpp
        ccu/tilexr_ccu_signal_wait_program.h
        ccu/tilexr_ccu_signal_wait_program.cpp
        ccu/tilexr_ccu_topology.h
        ccu/tilexr_ccu_topology.cpp
        ccu/tilexr_ccu_runtime_session.h
        ccu/tilexr_ccu_runtime_session.cpp
        ccu/tilexr_ccu_executor.h
        ccu/tilexr_ccu_executor.cpp
        ccu/tilexr_ccu_specs.h
        ccu/tilexr_ccu_specs.cpp
        tools/socket/tilexr_sock_exchange.h
        tools/socket/tilexr_sock_exchange.cpp
        udma/tilexr_hccp_defs.h
        udma/tilexr_hccp_loader.h
        udma/tilexr_hccp_loader.cpp
        udma/tilexr_udma_config.h
        udma/tilexr_udma_config.cpp
        udma/tilexr_udma_root_info.h
        udma/tilexr_udma_root_info.cpp
        udma/tilexr_udma_layout.h
        udma/tilexr_udma_layout.cpp
        udma/tilexr_udma_context.h
        udma/tilexr_udma_context.cpp
        udma/tilexr_udma_transport.h
        udma/tilexr_udma_transport.cpp
        sdma/tilexr_sdma_transport.h
        sdma/tilexr_sdma_transport.cpp
        sdma/tilexr_sdma_a5_cleanup.h
        sdma/tilexr_sdma_a5_backend.h
        sdma/tilexr_sdma_a5_backend.cpp
)

add_library(tile-comm SHARED ${TILEXR_SOURCE_FILE})
if(TILEXR_BUILD_RDMA AND TILEXR_HAVE_IBVERBS_HEADER)
    target_compile_definitions(tile-comm PRIVATE TILEXR_HAVE_IBVERBS=1)
else()
    target_compile_definitions(tile-comm PRIVATE TILEXR_HAVE_IBVERBS=0)
endif()
if(TILEXR_BUILD_RDMA AND TILEXR_HAVE_IBVERBS_HEADER AND TILEXR_HAVE_IBV_EXTEND_HEADER)
    target_include_directories(tile-comm PRIVATE "${TILEXR_IBV_EXTEND_INCLUDE}")
    target_compile_definitions(tile-comm PRIVATE TILEXR_HAVE_IBV_EXTEND=1)
else()
    target_compile_definitions(tile-comm PRIVATE TILEXR_HAVE_IBV_EXTEND=0)
endif()
set_target_properties(tile-comm PROPERTIES
        BUILD_WITH_INSTALL_RPATH FALSE
        SKIP_BUILD_RPATH TRUE
        INSTALL_RPATH ""
        SKIP_INSTALL_RPATH TRUE)
target_compile_definitions(tile-comm PRIVATE TILEXR_CCU_TESTING=1)

if(TILEXR_HAVE_PTO_SDMA)
    target_compile_definitions(tile-comm PRIVATE TILEXR_HAVE_PTO_SDMA=1)
else()
    target_compile_definitions(tile-comm PRIVATE TILEXR_HAVE_PTO_SDMA=0)
endif()

target_include_directories(tile-comm
        PUBLIC
        ${CMAKE_CURRENT_BINARY_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
        ${ASCEND_DRIVER_PATH}/kernel/inc
        ${ASCEND_HOME_PATH}/${ARCH}-linux/pkg_inc/
        ${ASCEND_HOME_PATH}/${ARCH}-linux/pkg_inc/profiling/
        ${ASCEND_HOME_PATH}/${ARCH}-linux/pkg_inc/runtime/
        ${ASCEND_HOME_PATH}/${ARCH}-linux/pkg_inc/runtime/runtime/
        ${ASCEND_HOME_PATH}/${ARCH}-linux/include/
        PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/../../3rdparty
        ${CMAKE_CURRENT_SOURCE_DIR}/../../3rdparty/spdlog/include
)
target_link_directories(tile-comm
        PRIVATE
        ${ASCEND_DRIVER_PATH}/lib64/driver
        ${ASCEND_HOME_PATH}/${ARCH}-linux/lib64
)

target_link_libraries(tile-comm
        PRIVATE
        ascendcl
        runtime
        profapi
        ascend_hal
        dl
)

if(TILEXR_HAVE_PTO_SDMA)
    target_link_libraries(tile-comm
            PRIVATE
            ${TILEXR_PTO_NNOPBASE_LIB}
    )
endif()

install(TARGETS tile-comm LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(FILES
        ${CMAKE_CURRENT_BINARY_DIR}/include/tilexr_sdma_config.h
        ${CMAKE_CURRENT_SOURCE_DIR}/../include/tilexr_api.h
        ${CMAKE_CURRENT_SOURCE_DIR}/../include/tilexr_rdma.h
        ${CMAKE_CURRENT_SOURCE_DIR}/../include/tilexr_rdma_device_types.h
        ${CMAKE_CURRENT_SOURCE_DIR}/../include/tilexr_rdma_device.h
        ${CMAKE_CURRENT_SOURCE_DIR}/../include/tilexr_rdma_wire.h
        ${CMAKE_CURRENT_SOURCE_DIR}/../include/tilexr_rdma_pipeline.h
        ${CMAKE_CURRENT_SOURCE_DIR}/../include/tilexr_rdma_write.h
        ${CMAKE_CURRENT_SOURCE_DIR}/../include/tilexr_rdma_write_options.h
        ${CMAKE_CURRENT_SOURCE_DIR}/../include/tilexr_types.h
        ${CMAKE_CURRENT_SOURCE_DIR}/../include/comm_args.h
        ${CMAKE_CURRENT_SOURCE_DIR}/../include/tilexr_sync.h
        ${CMAKE_CURRENT_SOURCE_DIR}/../include/tilexr_data_as_flag.h
        ${CMAKE_CURRENT_SOURCE_DIR}/../include/tilexr_perf_trace.h
        ${CMAKE_CURRENT_SOURCE_DIR}/../include/tilexr_profiling.h
        ${CMAKE_CURRENT_SOURCE_DIR}/../include/tilexr_udma.h
        ${CMAKE_CURRENT_SOURCE_DIR}/../include/tilexr_udma_fullmesh.h
        ${CMAKE_CURRENT_SOURCE_DIR}/../include/tilexr_udma_reg.h
        ${CMAKE_CURRENT_SOURCE_DIR}/../include/tilexr_udma_types.h
        ${CMAKE_CURRENT_SOURCE_DIR}/../include/tilexr_sdma_types.h
        ${CMAKE_CURRENT_SOURCE_DIR}/../include/tilexr_sdma_a5_types.h
        ${CMAKE_CURRENT_SOURCE_DIR}/../include/tilexr_sdma_a5.h
        ${CMAKE_CURRENT_SOURCE_DIR}/../include/tilexr_sdma.h
        ${CMAKE_CURRENT_SOURCE_DIR}/../include/tilexr_sdma_compat.h
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(CODE "string(CONCAT _tilexr_collective_header \"tilexr_\" \"collectives.h\")
string(CONCAT _tilexr_collective_perf_header \"tilexr_\" \"collectives\" \"_perf.h\")
string(CONCAT _tilexr_collective_straggler_header \"tilexr_\" \"collective\" \"_straggler.h\")
string(CONCAT _tilexr_straggler_layout_header \"tilexr_\" \"straggler\" \"_layout.h\")
set(_tilexr_optional_headers
    \"\${_tilexr_collective_header}\"
    \"\${_tilexr_collective_perf_header}\"
    \"\${_tilexr_collective_straggler_header}\"
    \"\${_tilexr_straggler_layout_header}\"
)
foreach(_tilexr_optional_header IN LISTS _tilexr_optional_headers)
    if(IS_ABSOLUTE \"${CMAKE_INSTALL_INCLUDEDIR}\")
        set(_tilexr_optional_header_path \"\$ENV{DESTDIR}${CMAKE_INSTALL_INCLUDEDIR}/\${_tilexr_optional_header}\")
    else()
        set(_tilexr_optional_header_path \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_INCLUDEDIR}/\${_tilexr_optional_header}\")
    endif()
    file(REMOVE \"\${_tilexr_optional_header_path}\")
endforeach()")
