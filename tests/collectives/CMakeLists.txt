#
# Copyright (c) 2024-2026 TileXR Project
# CMakeLists.txt for TileXR collectives tests
#

cmake_minimum_required(VERSION 3.16)
project(TileXR_Collectives_Tests)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
include(GNUInstallDirs)
enable_testing()
find_package(Python3 COMPONENTS Interpreter)

set(TILEXR_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../..")
set(TILEXR_INSTALL_PREFIX "${TILEXR_ROOT}/install" CACHE PATH "TileXR install prefix for installed collectives headers")
set(TILEXR_INSTALL_LIBDIR "${CMAKE_INSTALL_LIBDIR}" CACHE STRING "TileXR install library directory relative to TILEXR_INSTALL_PREFIX")
set(TILEXR_INSTALL_INCLUDEDIR "${CMAKE_INSTALL_INCLUDEDIR}" CACHE STRING "TileXR install include directory relative to TILEXR_INSTALL_PREFIX")
set(TILEXR_COLLECTIVES_LIB "" CACHE FILEPATH "Path to libtilexr-collectives.so")
set(TILEXR_LIB "" CACHE FILEPATH "Path to libtile-comm.so")
if(IS_ABSOLUTE "${TILEXR_INSTALL_LIBDIR}")
    set(TILEXR_INSTALL_LIB_SEARCH_DIR "${TILEXR_INSTALL_LIBDIR}")
else()
    set(TILEXR_INSTALL_LIB_SEARCH_DIR "${TILEXR_INSTALL_PREFIX}/${TILEXR_INSTALL_LIBDIR}")
endif()
if(IS_ABSOLUTE "${TILEXR_INSTALL_INCLUDEDIR}")
    set(TILEXR_INSTALL_INCLUDE_SEARCH_DIR "${TILEXR_INSTALL_INCLUDEDIR}")
else()
    set(TILEXR_INSTALL_INCLUDE_SEARCH_DIR "${TILEXR_INSTALL_PREFIX}/${TILEXR_INSTALL_INCLUDEDIR}")
endif()

set(_tilexr_default_ascend_home_path "$ENV{ASCEND_HOME_PATH}")
if(NOT _tilexr_default_ascend_home_path)
    set(_tilexr_default_ascend_home_path "/usr/local/Ascend/ascend-toolkit/latest")
endif()
set(ASCEND_HOME_PATH "${_tilexr_default_ascend_home_path}" CACHE PATH "Ascend toolkit path")

set(_tilexr_default_arch "$ENV{ARCH}")
if(NOT _tilexr_default_arch)
    set(_tilexr_default_arch "$ENV{TILEXR_OS_ARCH}")
endif()
if(NOT _tilexr_default_arch)
    execute_process(
        COMMAND uname -m
        OUTPUT_VARIABLE _tilexr_default_arch
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    if(_tilexr_default_arch STREQUAL "arm64")
        set(_tilexr_default_arch "aarch64")
    endif()
endif()
set(ARCH "${_tilexr_default_arch}" CACHE STRING "Target OS architecture")

set(_tilexr_default_ascend_driver_path "$ENV{ASCEND_DRIVER_PATH}")
if(NOT _tilexr_default_ascend_driver_path)
    set(_tilexr_default_ascend_driver_path "/usr/local/Ascend/driver")
endif()
set(ASCEND_DRIVER_PATH "${_tilexr_default_ascend_driver_path}" CACHE PATH "Ascend driver path")

set(TILEXR_COLLECTIVES_IN_TREE OFF)
if(TARGET tilexr-collectives)
    set(TILEXR_COLLECTIVES_IN_TREE ON)
endif()

add_executable(test_tilexr_collectives_header_compile
    unit/test_tilexr_collectives_header_compile.cpp
)

add_executable(test_tilexr_int32_pattern
    unit/test_tilexr_int32_pattern.cpp
)

add_executable(test_tilexr_collective_backend_options
    unit/test_tilexr_collective_backend_options.cpp
)

target_include_directories(test_tilexr_collective_backend_options PRIVATE
    ${TILEXR_ROOT}/src/collectives/host
)

add_executable(test_tilexr_collectives_correctness
    integration/test_tilexr_collectives_correctness.cpp
)

add_executable(tilexr_collective_perf
    tilexr-tests/tilexr_collective_perf.cpp
)

add_executable(test_tilexr_collectives_stub_behavior
    unit/test_tilexr_collectives_stub_behavior.cpp
)

add_executable(test_tilexr_collectives_uninitialized_comm
    unit/test_tilexr_collectives_uninitialized_comm.cpp
)

add_executable(test_collective_host_utils
    unit/test_collective_host_utils.cpp
)

target_include_directories(test_collective_host_utils PRIVATE
    ${TILEXR_ROOT}/src/collectives/host
    ${TILEXR_ROOT}/src/collectives/kernels
)

add_executable(test_tilexr_comm_next_magic
    unit/test_tilexr_comm_next_magic.cpp
)

add_executable(test_prepare_host_launch_context
    unit/test_prepare_host_launch_context.cpp
)

add_executable(test_alltoall_a5_candidate
    unit/test_alltoall_a5_candidate.cpp
)

target_include_directories(test_alltoall_a5_candidate PRIVATE
    ${TILEXR_ROOT}/src/collectives/host
)

target_include_directories(test_prepare_host_launch_context PRIVATE
    ${TILEXR_ROOT}/src/collectives/host
    ${TILEXR_ROOT}/src/collectives/kernels
)

if(TILEXR_COLLECTIVES_IN_TREE)
    set(TILEXR_COLLECTIVES_TEST_TARGET tilexr-collectives)
    set(TILEXR_COMM_TEST_TARGET tile-comm)
else()
    if(NOT EXISTS "${TILEXR_INSTALL_INCLUDE_SEARCH_DIR}/tilexr_collectives.h")
        message(FATAL_ERROR
            "Installed tilexr_collectives.h not found at ${TILEXR_INSTALL_INCLUDE_SEARCH_DIR}/tilexr_collectives.h. "
            "Run cmake --install after building TileXR with -DTILEXR_BUILD_COLLECTIVES=ON or set TILEXR_INSTALL_PREFIX."
        )
    endif()

    if(NOT TILEXR_COLLECTIVES_LIB)
        find_library(TILEXR_COLLECTIVES_LIB_FOUND tilexr-collectives
            HINTS
            "${TILEXR_INSTALL_LIB_SEARCH_DIR}"
            NO_DEFAULT_PATH
        )
        if(TILEXR_COLLECTIVES_LIB_FOUND)
            set(TILEXR_COLLECTIVES_LIB "${TILEXR_COLLECTIVES_LIB_FOUND}")
        endif()
    endif()

    if(NOT TILEXR_COLLECTIVES_LIB)
        message(FATAL_ERROR
            "libtilexr-collectives.so not found under ${TILEXR_INSTALL_LIB_SEARCH_DIR}. "
            "Build/install TileXR with -DTILEXR_BUILD_COLLECTIVES=ON and set TILEXR_INSTALL_PREFIX, "
            "or pass -DTILEXR_COLLECTIVES_LIB=/path/to/libtilexr-collectives.so."
        )
    endif()

    if(NOT TILEXR_LIB)
        find_library(TILEXR_LIB_FOUND tile-comm
            HINTS
            "${TILEXR_INSTALL_LIB_SEARCH_DIR}"
            NO_DEFAULT_PATH
        )
        if(TILEXR_LIB_FOUND)
            set(TILEXR_LIB "${TILEXR_LIB_FOUND}")
        endif()
    endif()

    if(NOT TILEXR_LIB)
        message(FATAL_ERROR
            "libtile-comm.so not found under ${TILEXR_INSTALL_LIB_SEARCH_DIR}. "
            "Build/install TileXR and set TILEXR_INSTALL_PREFIX, "
            "or pass -DTILEXR_LIB=/path/to/libtile-comm.so."
        )
    endif()

    set(TILEXR_INSTALLED_INCLUDE_DIRS
        ${TILEXR_INSTALL_INCLUDE_SEARCH_DIR}
    )
    foreach(_tilexr_installed_include_dir
        ${ASCEND_DRIVER_PATH}/kernel/inc
        ${ASCEND_HOME_PATH}/${ARCH}-linux/pkg_inc/
        ${ASCEND_HOME_PATH}/${ARCH}-linux/pkg_inc/runtime/
        ${ASCEND_HOME_PATH}/${ARCH}-linux/include/
    )
        if(EXISTS "${_tilexr_installed_include_dir}")
            list(APPEND TILEXR_INSTALLED_INCLUDE_DIRS "${_tilexr_installed_include_dir}")
        endif()
    endforeach()

    add_library(tilexr-comm-installed SHARED IMPORTED)
    set_target_properties(tilexr-comm-installed PROPERTIES
        IMPORTED_LOCATION "${TILEXR_LIB}"
        INTERFACE_INCLUDE_DIRECTORIES "${TILEXR_INSTALLED_INCLUDE_DIRS}"
    )

    add_library(tilexr-collectives-installed SHARED IMPORTED)
    set_target_properties(tilexr-collectives-installed PROPERTIES
        IMPORTED_LOCATION "${TILEXR_COLLECTIVES_LIB}"
        INTERFACE_INCLUDE_DIRECTORIES "${TILEXR_INSTALLED_INCLUDE_DIRS}"
        INTERFACE_LINK_LIBRARIES tilexr-comm-installed
    )

    set(TILEXR_COLLECTIVES_TEST_TARGET tilexr-collectives-installed)
    set(TILEXR_COMM_TEST_TARGET tilexr-comm-installed)
endif()

set(TILEXR_COLLECTIVES_TEST_LINK_DIRS
    ${ASCEND_DRIVER_PATH}/lib64/driver
    ${ASCEND_HOME_PATH}/${ARCH}-linux/lib64
    ${ASCEND_HOME_PATH}/${ARCH}-linux/devlib
)

function(TileXRAddAscendRpathLink target_name)
    target_link_options(${target_name} PRIVATE
        "LINKER:-rpath-link,${ASCEND_HOME_PATH}/${ARCH}-linux/lib64"
        "LINKER:-rpath-link,${ASCEND_DRIVER_PATH}/lib64/driver"
    )
endfunction()

target_link_libraries(test_tilexr_collectives_header_compile
    ${TILEXR_COLLECTIVES_TEST_TARGET}
)

target_link_libraries(test_tilexr_collective_backend_options
    ${TILEXR_COLLECTIVES_TEST_TARGET}
)
target_link_directories(test_tilexr_collectives_correctness PRIVATE
    ${TILEXR_COLLECTIVES_TEST_LINK_DIRS}
)

target_link_libraries(test_tilexr_collectives_correctness
    ${TILEXR_COLLECTIVES_TEST_TARGET}
    ascendcl
)

target_link_directories(tilexr_collective_perf PRIVATE
    ${TILEXR_COLLECTIVES_TEST_LINK_DIRS}
)

target_link_libraries(tilexr_collective_perf
    ${TILEXR_COLLECTIVES_TEST_TARGET}
    ascendcl
)

target_link_libraries(test_tilexr_collectives_stub_behavior
    ${TILEXR_COLLECTIVES_TEST_TARGET}
)

target_link_libraries(test_tilexr_collectives_uninitialized_comm
    ${TILEXR_COLLECTIVES_TEST_TARGET}
)

target_link_libraries(test_collective_host_utils
    ${TILEXR_COLLECTIVES_TEST_TARGET}
)

target_link_libraries(test_tilexr_comm_next_magic
    ${TILEXR_COMM_TEST_TARGET}
)
target_include_directories(test_tilexr_comm_next_magic PRIVATE
    ${TILEXR_ROOT}/src/comm
)

target_link_libraries(test_prepare_host_launch_context
    ${TILEXR_COLLECTIVES_TEST_TARGET}
)

target_link_libraries(test_alltoall_a5_candidate
    ${TILEXR_COLLECTIVES_TEST_TARGET}
)

foreach(_tilexr_collectives_link_target
        test_tilexr_collectives_header_compile
        test_tilexr_collective_backend_options
        test_tilexr_collectives_correctness
        tilexr_collective_perf
        test_tilexr_collectives_stub_behavior
        test_tilexr_collectives_uninitialized_comm
        test_collective_host_utils
        test_tilexr_comm_next_magic
        test_prepare_host_launch_context
        test_alltoall_a5_candidate)
    TileXRAddAscendRpathLink(${_tilexr_collectives_link_target})
endforeach()

add_test(NAME test_tilexr_collectives_header_compile COMMAND test_tilexr_collectives_header_compile)
add_test(NAME test_tilexr_int32_pattern COMMAND test_tilexr_int32_pattern)
add_test(NAME test_tilexr_collective_backend_options COMMAND test_tilexr_collective_backend_options)
add_test(NAME test_tilexr_collectives_stub_behavior COMMAND test_tilexr_collectives_stub_behavior)
add_test(NAME test_tilexr_collectives_uninitialized_comm COMMAND test_tilexr_collectives_uninitialized_comm)
add_test(NAME test_collective_host_utils COMMAND test_collective_host_utils)
add_test(NAME test_tilexr_comm_next_magic COMMAND test_tilexr_comm_next_magic)
add_test(NAME test_prepare_host_launch_context COMMAND test_prepare_host_launch_context)
add_test(NAME test_alltoall_a5_candidate COMMAND test_alltoall_a5_candidate)
if(Python3_Interpreter_FOUND)
    add_test(
        NAME test_alltoall_a5_profiling_mapping
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/unit/test_alltoall_a5_profiling_mapping.py
    )
    add_test(
        NAME test_vllm_collectives_patch
        COMMAND ${Python3_EXECUTABLE}
                -m pytest -q
                ${CMAKE_CURRENT_SOURCE_DIR}/unit/test_vllm_collectives_patch.py
    )
endif()

install(TARGETS
    test_tilexr_collectives_header_compile
    test_tilexr_int32_pattern
    test_tilexr_collective_backend_options
    test_tilexr_collectives_correctness
    tilexr_collective_perf
    test_tilexr_collectives_stub_behavior
    test_tilexr_collectives_uninitialized_comm
    test_collective_host_utils
    test_tilexr_comm_next_magic
    test_prepare_host_launch_context
    test_alltoall_a5_candidate
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

install(PROGRAMS
    run_collectives_correctness.sh
    ${CMAKE_CURRENT_SOURCE_DIR}/../../tools/collectives/run_collective_perf.sh
    DESTINATION ${CMAKE_INSTALL_BINDIR}
)

install(FILES
    ${CMAKE_CURRENT_SOURCE_DIR}/../../tools/collectives/tilexr_collectives.mapping.json
    DESTINATION ${CMAKE_INSTALL_DATADIR}/tilexr/collectives
)

add_executable(test_tilexr_alltoall_a5_layout
        unit/test_alltoall_a5_layout.cpp)
target_compile_features(test_tilexr_alltoall_a5_layout PRIVATE cxx_std_14)
target_include_directories(test_tilexr_alltoall_a5_layout PRIVATE
        ${TILEXR_ROOT}/src/collectives/kernels
        ${TILEXR_ROOT}/src/include)
add_test(NAME test_tilexr_alltoall_a5_layout
        COMMAND test_tilexr_alltoall_a5_layout)
