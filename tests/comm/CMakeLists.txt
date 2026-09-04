#
# Copyright (c) 2024-2026 TileXR Project
# CMakeLists.txt for TileXR comm tests
#

cmake_minimum_required(VERSION 3.16)
project(TileXR_Comm_Tests)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
enable_testing()

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

add_executable(test_tilexr_kernel_binary_loader
    unit/test_tilexr_kernel_binary_loader.cpp
    ${TILEXR_ROOT}/src/common/tilexr_kernel_binary_loader.cpp
)
target_include_directories(test_tilexr_kernel_binary_loader PRIVATE
    ${TILEXR_ROOT}/src/common
)
target_link_libraries(test_tilexr_kernel_binary_loader PRIVATE ${CMAKE_DL_LIBS})

add_test(NAME test_tilexr_kernel_binary_loader
    COMMAND test_tilexr_kernel_binary_loader)

install(TARGETS
    test_tilexr_log
    test_tilexr_log_spdlog_compile
    test_tilexr_kernel_binary_loader
    RUNTIME DESTINATION ${CMAKE_INSTALL_PREFIX}/bin
)
