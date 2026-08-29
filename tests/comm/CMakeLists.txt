#
# Copyright (c) 2024-2026 TileXR Project
# CMakeLists.txt for TileXR comm tests
#

cmake_minimum_required(VERSION 3.16)
project(TileXR_Comm_Tests)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(TILEXR_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../..")

add_executable(test_tilexr_log
    unit/test_tilexr_log.cpp
)

target_include_directories(test_tilexr_log PRIVATE
    ${TILEXR_ROOT}/src/comm
)
target_compile_definitions(test_tilexr_log PRIVATE
    TILEXR_LOG_DISABLE_SPDLOG=1
)

add_executable(test_tilexr_log_spdlog_compile
    unit/test_tilexr_log_spdlog_compile.cpp
)

target_include_directories(test_tilexr_log_spdlog_compile PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/unit/fake_spdlog
    ${TILEXR_ROOT}/src/comm
)

install(TARGETS
    test_tilexr_log
    test_tilexr_log_spdlog_compile
    RUNTIME DESTINATION ${CMAKE_INSTALL_PREFIX}/bin
)
