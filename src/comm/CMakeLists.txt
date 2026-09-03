#
# Copyright (c) 2024 Huawei Technologies Co., Ltd.
# This file is a part of the CANN Open Software.
# Licensed under CANN Open Software License Agreement Version 1.0 (the "License").
# Please refer to the License for details. You may not use this file except in compliance with the License.
# THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
# INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
# See LICENSE in the root of the software repository for the full text of the License.
#

include(GNUInstallDirs)
include(CMakePushCheckState)
include(CheckIncludeFileCXX)
include(CheckCXXSourceCompiles)

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
        comm_wrap.cpp
        tilexr_profiling_runtime.cpp
        tilexr_profiling_runtime.h
        tilexr_internal.cpp
        tilexr_internal.h
        tilexr_log.h
        ccu/tilexr_ccu_abi_constants.h
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
)

add_library(tile-comm SHARED ${TILEXR_SOURCE_FILE})
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
        ${CMAKE_CURRENT_SOURCE_DIR}/../include/tilexr_sdma.h
        ${CMAKE_CURRENT_SOURCE_DIR}/../include/tilexr_sdma_compat.h
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(CODE "string(CONCAT _tilexr_collective_header \"tilexr_\" \"collectives.h\")
string(CONCAT _tilexr_collective_perf_header \"tilexr_\" \"collectives\" \"_perf.h\")
set(_tilexr_optional_headers
    \"\${_tilexr_collective_header}\"
    \"\${_tilexr_collective_perf_header}\"
)
foreach(_tilexr_optional_header IN LISTS _tilexr_optional_headers)
    if(IS_ABSOLUTE \"${CMAKE_INSTALL_INCLUDEDIR}\")
        set(_tilexr_optional_header_path \"\$ENV{DESTDIR}${CMAKE_INSTALL_INCLUDEDIR}/\${_tilexr_optional_header}\")
    else()
        set(_tilexr_optional_header_path \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_INCLUDEDIR}/\${_tilexr_optional_header}\")
    endif()
    file(REMOVE \"\${_tilexr_optional_header_path}\")
endforeach()")
