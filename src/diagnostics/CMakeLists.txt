include(GNUInstallDirs)

add_subdirectory(kernels)

include(${CMAKE_SOURCE_DIR}/src/cmake/TileXRKernelArtifact.cmake)
add_library(tilexr-diagnostics SHARED
    host/clock_kernel.cpp
    host/straggler_analysis.cpp
    host/straggler_session.cpp
    ${CMAKE_SOURCE_DIR}/src/common/tilexr_kernel_binary_loader.cpp)
tilexr_attach_kernel_artifacts(tilexr-diagnostics
    FILES "${TILEXR_DIAGNOSTICS_OP}"
    TARGETS tilexr_diagnostics_op)
target_compile_features(tilexr-diagnostics PRIVATE cxx_std_14)
target_include_directories(tilexr-diagnostics
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
        ${ASCEND_HOME_PATH}/${ARCH}-linux/pkg_inc
        ${ASCEND_HOME_PATH}/${ARCH}-linux/pkg_inc/runtime
        ${ASCEND_HOME_PATH}/${ARCH}-linux/include
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/host
        ${CMAKE_SOURCE_DIR}/src/common
        ${ASCEND_DRIVER_PATH}/kernel/inc)
target_link_directories(tilexr-diagnostics PRIVATE
    ${ASCEND_DRIVER_PATH}/lib64/driver
    ${ASCEND_HOME_PATH}/${ARCH}-linux/lib64)
target_link_libraries(tilexr-diagnostics
    PUBLIC tile-comm
    PRIVATE ascendcl runtime ascend_hal dl)

install(TARGETS tilexr-diagnostics LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(FILES
    ${CMAKE_CURRENT_SOURCE_DIR}/../include/tilexr_straggler.h
    ${CMAKE_CURRENT_SOURCE_DIR}/../include/tilexr_straggler_device.h
    ${CMAKE_CURRENT_SOURCE_DIR}/../include/tilexr_straggler_layout.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
