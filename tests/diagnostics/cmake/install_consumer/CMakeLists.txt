cmake_minimum_required(VERSION 3.16)
project(TileXRDiagnosticsInstallConsumer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_path(TILEXR_DIAGNOSTICS_INCLUDE_DIR tilexr_straggler.h
    HINTS "${TILEXR_INSTALL_PREFIX}/${TILEXR_INSTALL_INCLUDEDIR}"
    NO_DEFAULT_PATH REQUIRED)
find_library(TILEXR_DIAGNOSTICS_LIBRARY tilexr-diagnostics
    HINTS "${TILEXR_INSTALL_PREFIX}/${TILEXR_INSTALL_LIBDIR}"
    NO_DEFAULT_PATH REQUIRED)
find_library(TILEXR_COMM_LIBRARY tile-comm
    HINTS "${TILEXR_INSTALL_PREFIX}/${TILEXR_INSTALL_LIBDIR}"
    NO_DEFAULT_PATH REQUIRED)

add_executable(tilexr_diagnostics_install_consumer main.cpp)
target_include_directories(tilexr_diagnostics_install_consumer PRIVATE
    "${TILEXR_DIAGNOSTICS_INCLUDE_DIR}"
    "${ASCEND_HOME_PATH}/${ARCH}-linux/pkg_inc"
    "${ASCEND_HOME_PATH}/${ARCH}-linux/pkg_inc/runtime"
    "${ASCEND_HOME_PATH}/${ARCH}-linux/include")
target_link_options(tilexr_diagnostics_install_consumer PRIVATE
    "LINKER:-rpath-link,${TILEXR_INSTALL_PREFIX}/${TILEXR_INSTALL_LIBDIR}"
    "LINKER:-rpath-link,${ASCEND_HOME_PATH}/${ARCH}-linux/lib64"
    "LINKER:-rpath-link,${ASCEND_DRIVER_PATH}/lib64/driver")
target_link_libraries(tilexr_diagnostics_install_consumer PRIVATE
    "${TILEXR_DIAGNOSTICS_LIBRARY}"
    "${TILEXR_COMM_LIBRARY}")
