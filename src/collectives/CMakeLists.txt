#
# Copyright (c) 2024-2026 TileXR Project
# CMakeLists.txt for optional TileXR collectives library
#

include(GNUInstallDirs)

if(TILEXR_BUILD_COLLECTIVES)
add_subdirectory(kernels)
include(cmake/alltoall_a5.cmake)

set(TILEXR_COLLECTIVES_KERNEL_EMBED_CPP
        ${CMAKE_CURRENT_BINARY_DIR}/tilexr_collectives_kernel_embed.cpp
)

add_custom_command(
        OUTPUT ${TILEXR_COLLECTIVES_KERNEL_EMBED_CPP}
        COMMAND ${CMAKE_COMMAND}
                -DTILEXR_COLLECTIVES_OP=${TILEXR_COLLECTIVES_OP}
                -DTILEXR_COLLECTIVES_KERNEL_EMBED_CPP=${TILEXR_COLLECTIVES_KERNEL_EMBED_CPP}
                -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_collectives_kernel.cmake
        DEPENDS tilexr_collectives_op ${TILEXR_COLLECTIVES_OP}
                ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_collectives_kernel.cmake
        VERBATIM
)

set(TILEXR_COLLECTIVES_SOURCE_FILE
        host/collective_backend.cpp
        host/collective_launcher.cpp
        host/collective_utils.cpp
        host/collective_kernel.cpp
        host/tilexr_collectives.cpp
        host/collective_backend.h
        ${TILEXR_COLLECTIVES_KERNEL_EMBED_CPP}
        ${TILEXR_GROUPED_KERNEL_EMBED_CPP}
        ${TILEXR_GROUPED_KERNEL_128_EMBED_CPP}
)

add_library(tilexr-collectives SHARED ${TILEXR_COLLECTIVES_SOURCE_FILE})
add_dependencies(tilexr-collectives tilexr_collectives_op
        tilexr_alltoall_a5_kernel tilexr_alltoall_a5_kernel_128)
if(TILEXR_COLLECTIVES_ENABLE_PROFILING)
    target_compile_definitions(tilexr-collectives PRIVATE TILEXR_ENABLE_PROFILING=1)
endif()
set_source_files_properties(${TILEXR_COLLECTIVES_KERNEL_EMBED_CPP}
        PROPERTIES OBJECT_DEPENDS "${TILEXR_COLLECTIVES_OP}"
)
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
        ascendcl
        runtime
        profapi
        ascend_hal
        dl
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
