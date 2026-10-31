#
# Copyright (c) 2024-2026 TileXR Project
# CMakeLists.txt for optional TileXR collectives library
#

include(GNUInstallDirs)

if(TILEXR_BUILD_COLLECTIVES)
add_subdirectory(kernels)
include(cmake/alltoall_a5.cmake)
include(${CMAKE_SOURCE_DIR}/src/cmake/TileXRKernelArtifact.cmake)

set(TILEXR_COLLECTIVES_SOURCE_FILE
        host/collective_backend.cpp
        host/collective_launcher.cpp
        host/collective_utils.cpp
        host/collective_kernel.cpp
        host/tilexr_collectives.cpp
        host/collective_backend.h
        ${CMAKE_SOURCE_DIR}/src/common/tilexr_kernel_binary_loader.cpp
)

add_library(tilexr-collectives SHARED ${TILEXR_COLLECTIVES_SOURCE_FILE})
add_dependencies(tilexr-collectives tilexr_collectives_op
        tilexr_alltoall_a5_kernel tilexr_alltoall_a5_kernel_128)
tilexr_attach_kernel_artifacts(tilexr-collectives FILES
        ${TILEXR_COLLECTIVES_OP}
        ${TILEXR_GROUPED_KERNEL_BINARY}
        ${TILEXR_GROUPED_KERNEL_128_BINARY}
    TARGETS
        tilexr_collectives_op
        tilexr_alltoall_a5_kernel
        tilexr_alltoall_a5_kernel_128)
if(TILEXR_COLLECTIVES_ENABLE_PROFILING)
    target_compile_definitions(tilexr-collectives PRIVATE TILEXR_ENABLE_PROFILING=1)
endif()
if(TILEXR_COLLECTIVES_AICORE_ARCH STREQUAL "dav-c310-vec")
    target_compile_definitions(tilexr-collectives PUBLIC TILEXR_COLLECTIVES_C310_ATOMIC_LIMITS=1)
endif()

target_include_directories(tilexr-collectives
        PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
        ${ASCEND_HOME_PATH}/${ARCH}-linux/pkg_inc/
        ${ASCEND_HOME_PATH}/${ARCH}-linux/pkg_inc/runtime/
        ${ASCEND_HOME_PATH}/${ARCH}-linux/include/
        ${ASCEND_DRIVER_PATH}/kernel/inc
        PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/host
        ${CMAKE_CURRENT_SOURCE_DIR}/kernels
        ${CMAKE_CURRENT_SOURCE_DIR}/../comm
        ${CMAKE_SOURCE_DIR}/src/common
)

target_link_directories(tilexr-collectives
        PRIVATE
        ${ASCEND_DRIVER_PATH}/lib64/driver
        ${ASCEND_HOME_PATH}/${ARCH}-linux/lib64
)

target_link_libraries(tilexr-collectives
        PUBLIC
        tile-comm
        PRIVATE
        tilexr-diagnostics
        TileXR::profiling
        ascendcl
        runtime
        profapi
        ascend_hal
        ${CMAKE_DL_LIBS}
)

install(TARGETS tilexr-collectives LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/../include/tilexr_collectives.h
        ${CMAKE_CURRENT_SOURCE_DIR}/../include/tilexr_collectives_perf.h
        ${CMAKE_CURRENT_SOURCE_DIR}/../include/tilexr_perf_trace.h
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/kernels/tilexr_alltoall_a5.mapping.json
        DESTINATION ${CMAKE_INSTALL_DATADIR}/tilexr/profiling)
endif()
