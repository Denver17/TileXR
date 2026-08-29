#
# Copyright (c) 2024-2026 TileXR Project
# CMakeLists.txt for TileXR DataAsFlag tests
#

cmake_minimum_required(VERSION 3.16)
project(TileXR_DataAsFlag_Tests)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
include(GNUInstallDirs)
enable_testing()

set(TILEXR_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../..")

add_executable(test_tilexr_data_as_flag_header_compile
    unit/test_tilexr_data_as_flag_header_compile.cpp
)

target_include_directories(test_tilexr_data_as_flag_header_compile PRIVATE
    ${TILEXR_ROOT}/src/include
)

add_test(NAME test_tilexr_data_as_flag_header_compile COMMAND test_tilexr_data_as_flag_header_compile)

install(TARGETS
    test_tilexr_data_as_flag_header_compile
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
