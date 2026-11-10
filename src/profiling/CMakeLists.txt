# 独立安装 src/include 下的 profiling SDK；仅显式启用手动检查或 examples/profiling 示例。
# Install the standalone profiling headers from src/include; enable manual checks or examples/profiling only on request.
cmake_minimum_required(VERSION 3.16)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(TileXRProfiling VERSION 1.0.0 LANGUAGES CXX)
endif()
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
option(TILEXR_PROFILING_BUILD_TESTS "Build standalone profiling Host tests" OFF)
option(TILEXR_PROFILING_WITH_ACL "Build validation and examples using actual CANN ACL" OFF)
option(TILEXR_PROFILING_BUILD_EXAMPLES "Build the independent Add4 example" OFF)
set(TILEXR_PROFILING_SOC "" CACHE STRING "Actual target SoC for the optional example")
add_library(tilexr_profiling INTERFACE)
add_library(TileXR::profiling ALIAS tilexr_profiling)
set_target_properties(tilexr_profiling PROPERTIES EXPORT_NAME profiling)
target_compile_features(tilexr_profiling INTERFACE cxx_std_14)
target_include_directories(tilexr_profiling INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
set(profiling_cmake_dir "${CMAKE_INSTALL_LIBDIR}/cmake/TileXRProfiling")
install(TARGETS tilexr_profiling EXPORT TileXRProfilingTargets)
install(FILES ../include/tilexr/profiling.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/tilexr)
install(DIRECTORY ../include/tilexr/profiling DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/tilexr)
install(FILES LICENSE VERSION DESTINATION ${CMAKE_INSTALL_DATADIR}/tilexr-profiling)
install(DIRECTORY ../../examples/profiling/ DESTINATION ${CMAKE_INSTALL_DATADIR}/tilexr-profiling/examples/add4
    PATTERN "results" EXCLUDE)
install(EXPORT TileXRProfilingTargets NAMESPACE TileXR:: DESTINATION ${profiling_cmake_dir})
configure_package_config_file(TileXRProfilingConfig.cmake.in
    "${CMAKE_CURRENT_BINARY_DIR}/TileXRProfilingConfig.cmake" INSTALL_DESTINATION ${profiling_cmake_dir})
write_basic_package_version_file("${CMAKE_CURRENT_BINARY_DIR}/TileXRProfilingConfigVersion.cmake"
    VERSION 1.0.0 COMPATIBILITY SameMajorVersion)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/TileXRProfilingConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/TileXRProfilingConfigVersion.cmake" DESTINATION ${profiling_cmake_dir})
if(TILEXR_PROFILING_BUILD_TESTS)
    enable_testing()
    add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../../tests/profiling/sdk_host" "${CMAKE_CURRENT_BINARY_DIR}/sdk_host")
endif()
if(TILEXR_PROFILING_WITH_ACL)
    find_path(TILEXR_PROFILING_ACL_INCLUDE_DIR acl/acl.h
        HINTS "$ENV{ASCEND_HOME_PATH}/include" "$ENV{ASCEND_TOOLKIT_HOME}/include")
    find_library(TILEXR_PROFILING_ACL_LIBRARY ascendcl
        HINTS "$ENV{ASCEND_HOME_PATH}/lib64" "$ENV{ASCEND_TOOLKIT_HOME}/lib64")
    if(NOT TILEXR_PROFILING_ACL_INCLUDE_DIR OR NOT TILEXR_PROFILING_ACL_LIBRARY)
        message(FATAL_ERROR "WITH_ACL requires the actual CANN headers and ascendcl library")
    endif()
endif()
if(TILEXR_PROFILING_BUILD_EXAMPLES)
    if(NOT TILEXR_PROFILING_WITH_ACL OR TILEXR_PROFILING_SOC STREQUAL "")
        message(FATAL_ERROR "Examples require WITH_ACL=ON and an explicit TILEXR_PROFILING_SOC")
    endif()
    add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../../examples/profiling" "${CMAKE_CURRENT_BINARY_DIR}/examples")
endif()
