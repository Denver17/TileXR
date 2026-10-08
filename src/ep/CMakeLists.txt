include(GNUInstallDirs)
include(${CMAKE_SOURCE_DIR}/src/cmake/TileXREpCoreConfig.cmake)
set(TILEXR_EP_SERVER_DEDUP_ENABLE "auto" CACHE STRING "Server dedup policy (auto, 0 or 1)")
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

include(${CMAKE_SOURCE_DIR}/src/cmake/TileXRKernelArtifact.cmake)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/src/collectives/kernels/cmake")
enable_language(CCE)

option(TILEXR_EP_COMBINE_MEMORY_ENABLE_PROFILING
    "Enable per-AIV profiling in the EP Combine Memory kernel" OFF)
option(TILEXR_EP_DISPATCH_MEMORY_ENABLE_PROFILING
    "Enable per-AIV profiling in the EP Dispatch Memory kernel" OFF)
option(TILEXR_EP_SERVER_DEDUP_ENABLE_PROFILING
    "Enable standard GM event capture in the Server Dedup kernel" OFF)
option(TILEXR_EP_BUILD_MEMORY_BOUNDARY_TEST
    "Build the isolated A5 4 GiB peer-window boundary test" OFF)

# EP is distributed as one package. All kernels are A5-only binaries;
# unsupported devices are rejected by libtilexr-ep.so at runtime.
set(TILEXR_EP_AICORE_ARCH "--cce-aicore-arch=dav-c310-vec")
set(TILEXR_EP_CATLASS_ARCH "3510")
set(TILEXR_EP_KERNEL_INCLUDES
    -I${ASCEND_HOME_PATH}/compiler/tikcpp
    -I${ASCEND_HOME_PATH}/compiler/tikcpp/tikcfw
    -I${ASCEND_HOME_PATH}/compiler/tikcpp/tikcfw/impl
    -I${ASCEND_HOME_PATH}/compiler/tikcpp/tikcfw/interface
    -I${ASCEND_HOME_PATH}/${ARCH}-linux/tikcpp
    -I${ASCEND_HOME_PATH}/${ARCH}-linux/tikcpp/tikcfw
    -I${ASCEND_HOME_PATH}/${ARCH}-linux/tikcpp/tikcfw/impl
    -I${ASCEND_HOME_PATH}/${ARCH}-linux/tikcpp/tikcfw/interface
    -I${ASCEND_HOME_PATH}/${ARCH}-linux/asc/include
    -I${ASCEND_HOME_PATH}/${ARCH}-linux/pkg_inc
    -I${ASCEND_HOME_PATH}/${ARCH}-linux/pkg_inc/runtime
    -I${ASCEND_HOME_PATH}/${ARCH}-linux/runtime/include
    -I${ASCEND_HOME_PATH}/${ARCH}-linux/include
    -I${ASCEND_DRIVER_PATH}/kernel/inc
    -I${CMAKE_SOURCE_DIR}/3rdparty
    -I${CMAKE_SOURCE_DIR}/src/include
    -I${CMAKE_CURRENT_SOURCE_DIR}/common
    -I${CMAKE_CURRENT_SOURCE_DIR}/kernels
)

function(tilexr_add_ep_aicore_kernel target source output)
    cmake_parse_arguments(ARG "" "" "DEPENDS;DEFINITIONS" ${ARGN})
    set_source_files_properties("${source}" PROPERTIES
        LANGUAGE CCE
        OBJECT_DEPENDS "${ARG_DEPENDS}")
    add_library(${target}_tmp OBJECT "${source}")
    target_compile_options(${target}_tmp PRIVATE
        -O2
        -std=gnu++17
        --cce-aicore-only
        --cce-auto-sync
        -Wno-deprecated-declarations
        "SHELL:-mllvm -cce-aicore-stack-size=0x8000"
        "SHELL:-mllvm -cce-aicore-function-stack-size=0x8000"
        "SHELL:-mllvm -cce-aicore-record-overflow=false"
        "SHELL:-mllvm -cce-aicore-addr-transform"
        "SHELL:-mllvm -cce-aicore-dcci-insert-for-scalar=false"
        ${TILEXR_EP_AICORE_ARCH}
        ${TILEXR_EP_KERNEL_INCLUDES})
    target_compile_definitions(${target}_tmp PRIVATE
        CATLASS_ARCH=${TILEXR_EP_CATLASS_ARCH}
        ${TILEXR_EP_CORE_DEFINITIONS}
        ${ARG_DEFINITIONS})

    add_custom_command(
        OUTPUT "${output}"
        COMMAND ${CMAKE_CCE_LINKER} -m aicorelinux -Ttext=0
            $<TARGET_OBJECTS:${target}_tmp>
            --static -o "${output}" --allow-multiple-definition
        COMMAND truncate -c -s 10485760 "${output}"
        # OBJECT targets impose build order; the object files trigger relinking.
        DEPENDS ${target}_tmp $<TARGET_OBJECTS:${target}_tmp> ${ARG_DEPENDS}
        COMMAND_EXPAND_LISTS
        VERBATIM
        COMMENT "Linking ${target} A5 AICore binary")
    add_custom_target(${target} ALL DEPENDS "${output}")
endfunction()

set(TILEXR_EP_DISPATCH_KERNEL_SOURCE
    "${CMAKE_CURRENT_SOURCE_DIR}/kernels/tilexr_ep_dispatch_kernel.cpp")
set(TILEXR_EP_COMBINE_KERNEL_SOURCE
    "${CMAKE_CURRENT_SOURCE_DIR}/kernels/tilexr_ep_combine_kernel.cpp")
set(TILEXR_EP_DISPATCH_MEMORY_KERNEL_SOURCE
    "${CMAKE_CURRENT_SOURCE_DIR}/kernels/tilexr_ep_dispatch_memory_kernel.cpp")
set(TILEXR_EP_COMBINE_MEMORY_KERNEL_SOURCE
    "${CMAKE_CURRENT_SOURCE_DIR}/kernels/tilexr_ep_combine_memory_kernel.cpp")

set(TILEXR_EP_DISPATCH_KERNEL_OP
    "${CMAKE_CURRENT_BINARY_DIR}/tilexr_ep_dispatch_kernel.o")
set(TILEXR_EP_COMBINE_KERNEL_OP
    "${CMAKE_CURRENT_BINARY_DIR}/tilexr_ep_combine_kernel.o")
set(TILEXR_EP_DISPATCH_MEMORY_OP
    "${CMAKE_CURRENT_BINARY_DIR}/tilexr_ep_dispatch_memory_kernel.o")
set(TILEXR_EP_COMBINE_MEMORY_OP
    "${CMAKE_CURRENT_BINARY_DIR}/tilexr_ep_combine_memory_kernel.o")

tilexr_add_ep_aicore_kernel(tilexr_ep_dispatch_kernel
    "${TILEXR_EP_DISPATCH_KERNEL_SOURCE}" "${TILEXR_EP_DISPATCH_KERNEL_OP}"
    DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/kernels/tilexr_ep_dispatch_helpers.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/kernels/tilexr_ep_combine_helpers.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/common/ep_window.h"
        "${CMAKE_SOURCE_DIR}/src/include/comm_args.h"
        "${CMAKE_SOURCE_DIR}/src/include/tilexr_sync.h")

tilexr_add_ep_aicore_kernel(tilexr_ep_combine_kernel
    "${TILEXR_EP_COMBINE_KERNEL_SOURCE}" "${TILEXR_EP_COMBINE_KERNEL_OP}"
    DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/kernels/tilexr_ep_kernel_common.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/common/ep_window.h"
        "${CMAKE_SOURCE_DIR}/src/include/comm_args.h"
        "${CMAKE_SOURCE_DIR}/src/include/tilexr_sync.h"
        "${CMAKE_SOURCE_DIR}/src/include/tilexr_udma.h")

set(_tilexr_dispatch_memory_definitions)
if(TILEXR_EP_DISPATCH_MEMORY_ENABLE_PROFILING)
    list(APPEND _tilexr_dispatch_memory_definitions
        TILEXR_EP_DISPATCH_MEMORY_ENABLE_PROFILING=1)
endif()
tilexr_add_ep_aicore_kernel(tilexr_ep_dispatch_memory_kernel
    "${TILEXR_EP_DISPATCH_MEMORY_KERNEL_SOURCE}" "${TILEXR_EP_DISPATCH_MEMORY_OP}"
    DEFINITIONS ${_tilexr_dispatch_memory_definitions}
    DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/kernels/tilexr_ep_dispatch_memory_kernel.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/kernels/tilexr_ep_mxfp8_quant.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/common/ep_dispatch_memory_profile.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/common/ep_memory_window.h"
        "${CMAKE_SOURCE_DIR}/src/include/comm_args.h"
        "${CMAKE_SOURCE_DIR}/src/include/tilexr_types.h")

set(_tilexr_combine_memory_definitions)
if(TILEXR_EP_COMBINE_MEMORY_ENABLE_PROFILING)
    list(APPEND _tilexr_combine_memory_definitions
        TILEXR_EP_COMBINE_MEMORY_ENABLE_PROFILING=1)
endif()
tilexr_add_ep_aicore_kernel(tilexr_ep_combine_memory_kernel
    "${TILEXR_EP_COMBINE_MEMORY_KERNEL_SOURCE}" "${TILEXR_EP_COMBINE_MEMORY_OP}"
    DEFINITIONS ${_tilexr_combine_memory_definitions}
    DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/kernels/tilexr_ep_combine_memory_kernel.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/kernels/tilexr_ep_mxfp8_quant.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/common/ep_combine_memory_profile.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/common/ep_memory_window.h"
        "${CMAKE_SOURCE_DIR}/src/include/comm_args.h"
        "${CMAKE_SOURCE_DIR}/src/include/tilexr_data_as_flag.h"
        "${CMAKE_SOURCE_DIR}/src/include/tilexr_types.h")

set(TILEXR_EP_SERVER_DEDUP_KERNEL_OP
    "${CMAKE_CURRENT_BINARY_DIR}/tilexr_ep_dispatch_memory_server_dedup_kernel.o")
tilexr_add_ep_aicore_kernel(tilexr_ep_dispatch_memory_server_dedup_kernel
    "${CMAKE_CURRENT_SOURCE_DIR}/kernels/tilexr_ep_dispatch_memory_server_dedup_kernel.cpp"
    "${TILEXR_EP_SERVER_DEDUP_KERNEL_OP}"
    DEFINITIONS TILEXR_EP_SERVER_DEDUP_ENABLE=${TILEXR_EP_SERVER_DEDUP_LAYOUT_ENABLE}
        TILEXR_EP_SERVER_DEDUP_AUTO_ENABLE=${TILEXR_EP_SERVER_DEDUP_AUTO_ENABLE}
        $<$<BOOL:${TILEXR_EP_SERVER_DEDUP_ENABLE_PROFILING}>:TILEXR_ENABLE_PROFILING=1>
    DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/kernels/tilexr_ep_dispatch_memory_server_dedup_kernel.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/kernels/tilexr_ep_server_dedup_counts.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/kernels/tilexr_ep_server_dedup_send.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/kernels/tilexr_ep_server_dedup_select.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/kernels/tilexr_ep_server_dedup_recv.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/common/ep_packet_receive_layout.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/kernels/tilexr_ep_server_dedup_receive_tasks.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/kernels/tilexr_ep_server_dedup_receive_packet.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/kernels/tilexr_ep_server_dedup_profile.h"
        "${CMAKE_SOURCE_DIR}/src/include/tilexr_profiling.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/kernels/tilexr_ep_mxfp8_quant.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/common/ep_memory_server_dedup_mode.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/common/ep_memory_server_dedup_count_layout.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/common/ep_memory_server_dedup_buffer_layout.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/common/ep_memory_server_dedup_data_layout.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/common/ep_memory_server_dedup_profile.h"
        "${CMAKE_SOURCE_DIR}/src/include/comm_args.h"
        "${CMAKE_SOURCE_DIR}/src/include/tilexr_types.h")

set(TILEXR_EP_COMBINE_SERVER_DEDUP_KERNEL_OP
    "${CMAKE_CURRENT_BINARY_DIR}/tilexr_ep_combine_memory_server_dedup_kernel.o")
tilexr_add_ep_aicore_kernel(tilexr_ep_combine_memory_server_dedup_kernel
    "${CMAKE_CURRENT_SOURCE_DIR}/kernels/tilexr_ep_combine_memory_server_dedup_kernel.cpp"
    "${TILEXR_EP_COMBINE_SERVER_DEDUP_KERNEL_OP}"
    DEFINITIONS TILEXR_EP_SERVER_DEDUP_ENABLE=${TILEXR_EP_SERVER_DEDUP_LAYOUT_ENABLE}
        TILEXR_EP_SERVER_DEDUP_AUTO_ENABLE=${TILEXR_EP_SERVER_DEDUP_AUTO_ENABLE}
        $<$<BOOL:${TILEXR_EP_SERVER_DEDUP_ENABLE_PROFILING}>:TILEXR_ENABLE_PROFILING=1>
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/kernels/tilexr_ep_combine_memory_server_dedup_kernel.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/kernels/tilexr_ep_server_dedup_select.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/kernels/tilexr_ep_server_dedup_partial.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/kernels/tilexr_ep_server_dedup_source_aggregate.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/common/ep_memory_server_dedup_mode.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/common/ep_memory_server_dedup_combine_config.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/common/ep_memory_server_dedup_buffer_layout.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/common/ep_memory_server_dedup_count_layout.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/common/ep_memory_server_dedup_combine_tiling.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/common/ep_memory_server_dedup_combine_profile.h"
        "${CMAKE_SOURCE_DIR}/src/include/tilexr_profiling.h"
        "${CMAKE_SOURCE_DIR}/src/include/comm_args.h")

set(TILEXR_EP_KERNEL_TARGETS
    tilexr_ep_combine_memory_server_dedup_kernel
    tilexr_ep_dispatch_memory_server_dedup_kernel
    tilexr_ep_dispatch_kernel
    tilexr_ep_combine_kernel
    tilexr_ep_dispatch_memory_kernel
    tilexr_ep_combine_memory_kernel)
set(TILEXR_EP_KERNEL_OBJECTS
    "${TILEXR_EP_COMBINE_SERVER_DEDUP_KERNEL_OP}"
    "${TILEXR_EP_SERVER_DEDUP_KERNEL_OP}"
    "${TILEXR_EP_DISPATCH_KERNEL_OP}"
    "${TILEXR_EP_COMBINE_KERNEL_OP}"
    "${TILEXR_EP_DISPATCH_MEMORY_OP}"
    "${TILEXR_EP_COMBINE_MEMORY_OP}")

add_library(tilexr-ep SHARED
    host/ep_layout.cpp
    host/ep_memory_layout.cpp
    host/ep_dispatch_host.cpp
    host/ep_dispatch_memory_host.cpp
    host/ep_combine_memory_host.cpp
    host/ep_launch_context.cpp
    host/ep_kernel_launch.cpp
    host/tilexr_ep_dispatch.cpp
    host/tilexr_ep_memory_dispatch.cpp
    host/tilexr_ep_memory_combine.cpp
    host/ep_memory_server_dedup_layout.cpp
    host/ep_memory_server_dedup_host.cpp
    host/ep_memory_server_dedup_launch.cpp
    host/tilexr_ep_memory_server_dedup_dispatch.cpp
    host/ep_memory_server_dedup_combine_layout.cpp
    host/ep_memory_server_dedup_combine_host.cpp
    host/ep_memory_server_dedup_combine_launch.cpp
    host/tilexr_ep_memory_server_dedup_combine.cpp
    ${CMAKE_SOURCE_DIR}/src/common/tilexr_kernel_binary_loader.cpp
)

add_dependencies(tilexr-ep ${TILEXR_EP_KERNEL_TARGETS})
target_compile_definitions(tilexr-ep PUBLIC
    ${TILEXR_EP_CORE_DEFINITIONS}
    TILEXR_EP_SERVER_DEDUP_ENABLE=${TILEXR_EP_SERVER_DEDUP_LAYOUT_ENABLE}
    TILEXR_EP_SERVER_DEDUP_AUTO_ENABLE=${TILEXR_EP_SERVER_DEDUP_AUTO_ENABLE}
    PRIVATE
    TILEXR_EP_SERVER_DEDUP_ENABLE_PROFILING=$<BOOL:${TILEXR_EP_SERVER_DEDUP_ENABLE_PROFILING}>)
tilexr_attach_kernel_artifacts(tilexr-ep
    FILES ${TILEXR_EP_KERNEL_OBJECTS}
    TARGETS ${TILEXR_EP_KERNEL_TARGETS})

target_include_directories(tilexr-ep
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/common
        ${CMAKE_CURRENT_SOURCE_DIR}/host
        ${CMAKE_SOURCE_DIR}/src/common
)

target_link_directories(tilexr-ep PRIVATE
    ${ASCEND_DRIVER_PATH}/lib64/driver
    ${ASCEND_HOME_PATH}/${ARCH}-linux/lib64)

target_link_libraries(tilexr-ep PRIVATE
    tile-comm
    ascendcl
    runtime
    ascend_hal
    ${CMAKE_DL_LIBS})

set_target_properties(tilexr-ep PROPERTIES INSTALL_RPATH "$ORIGIN")

install(TARGETS tilexr-ep LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/../include/tilexr_ep.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

if(TILEXR_EP_BUILD_MEMORY_BOUNDARY_TEST)
    set(_boundary_dir "${CMAKE_SOURCE_DIR}/tests/ep/integration")
    set(_boundary_binary "${CMAKE_CURRENT_BINARY_DIR}/tilexr_ep_memory_boundary_test_kernel.o")
    tilexr_add_ep_aicore_kernel(tilexr_ep_memory_boundary_test_kernel
        "${_boundary_dir}/memory_boundary_kernel.cpp" "${_boundary_binary}"
        DEPENDS "${CMAKE_SOURCE_DIR}/src/include/comm_args.h")
    add_library(tilexr-ep-memory-boundary-test SHARED
        "${_boundary_dir}/memory_boundary_launch.cpp"
        "${CMAKE_SOURCE_DIR}/src/common/tilexr_kernel_binary_loader.cpp")
    target_include_directories(tilexr-ep-memory-boundary-test PRIVATE
        "${CMAKE_SOURCE_DIR}/src/include" "${CMAKE_SOURCE_DIR}/src/common"
        "${ASCEND_HOME_PATH}/${ARCH}-linux/include"
        "${ASCEND_HOME_PATH}/${ARCH}-linux/pkg_inc")
    target_link_directories(tilexr-ep-memory-boundary-test PRIVATE "${ASCEND_HOME_PATH}/${ARCH}-linux/lib64")
    target_link_libraries(tilexr-ep-memory-boundary-test PRIVATE runtime ${CMAKE_DL_LIBS})
    set_target_properties(tilexr-ep-memory-boundary-test PROPERTIES INSTALL_RPATH "$ORIGIN")
    add_dependencies(tilexr-ep-memory-boundary-test tilexr_ep_memory_boundary_test_kernel)
    tilexr_attach_kernel_artifacts(tilexr-ep-memory-boundary-test
        FILES "${_boundary_binary}" TARGETS tilexr_ep_memory_boundary_test_kernel)
    install(TARGETS tilexr-ep-memory-boundary-test LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
endif()

option(TILEXR_EP_BUILD_COMBINE_MEMORY_BOUNDARY_TEST
    "Build the test-only two-window Combine boundary probe" OFF)
if(TILEXR_EP_BUILD_COMBINE_MEMORY_BOUNDARY_TEST)
    set(_combine_boundary_dir "${CMAKE_SOURCE_DIR}/tests/ep/integration")
    set(_combine_boundary_binary "${CMAKE_CURRENT_BINARY_DIR}/tilexr_ep_combine_memory_boundary_test_kernel.o")
    tilexr_add_ep_aicore_kernel(tilexr_ep_combine_memory_boundary_test_kernel
        "${_combine_boundary_dir}/combine_memory_boundary_kernel.cpp" "${_combine_boundary_binary}"
        DEPENDS "${_combine_boundary_dir}/combine_memory_boundary_test.h"
            "${CMAKE_CURRENT_SOURCE_DIR}/common/ep_memory_server_dedup_buffer_layout.h"
            "${CMAKE_SOURCE_DIR}/src/include/comm_args.h"
            "${CMAKE_SOURCE_DIR}/src/include/tilexr_types.h")
    add_library(tilexr-ep-combine-memory-boundary-test SHARED
        "${_combine_boundary_dir}/combine_memory_boundary_launch.cpp"
        "${CMAKE_SOURCE_DIR}/src/common/tilexr_kernel_binary_loader.cpp")
    target_include_directories(tilexr-ep-combine-memory-boundary-test PRIVATE
        "${CMAKE_SOURCE_DIR}/src/include" "${CMAKE_SOURCE_DIR}/src/common"
        "${CMAKE_CURRENT_SOURCE_DIR}/common"
        "${ASCEND_HOME_PATH}/${ARCH}-linux/include"
        "${ASCEND_HOME_PATH}/${ARCH}-linux/pkg_inc")
    target_link_directories(tilexr-ep-combine-memory-boundary-test PRIVATE "${ASCEND_HOME_PATH}/${ARCH}-linux/lib64")
    target_link_libraries(tilexr-ep-combine-memory-boundary-test PRIVATE runtime ${CMAKE_DL_LIBS})
    set_target_properties(tilexr-ep-combine-memory-boundary-test PROPERTIES INSTALL_RPATH "$ORIGIN")
    add_dependencies(tilexr-ep-combine-memory-boundary-test tilexr_ep_combine_memory_boundary_test_kernel)
    tilexr_attach_kernel_artifacts(tilexr-ep-combine-memory-boundary-test
        FILES "${_combine_boundary_binary}" TARGETS tilexr_ep_combine_memory_boundary_test_kernel)
    install(TARGETS tilexr-ep-combine-memory-boundary-test LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
endif()

install(FILES profiling/server_dedup.mapping.json profiling/server_dedup_combine.mapping.json
    DESTINATION ${CMAKE_INSTALL_DATADIR}/tilexr/profiling)
install(DIRECTORY ${CMAKE_SOURCE_DIR}/tools/profiling
    DESTINATION ${CMAKE_INSTALL_DATADIR}/tilexr/tools
    FILES_MATCHING PATTERN "*.py" PATTERN "__pycache__" EXCLUDE)
