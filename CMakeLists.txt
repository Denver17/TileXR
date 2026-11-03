# -----------------------------------------------------------------------------------------------------------
# Copyright (c) 2025 Huawei Technologies Co., Ltd.
# This program is free software, you can redistribute it and/or modify it under the terms and conditions of
# CANN Open Software License Agreement Version 2.0 (the "License").
# Please refer to the License for details. You may not use this file except in compliance with the License.
# THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
# INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
# See LICENSE in the root of the software repository for the full text of the License.
# -----------------------------------------------------------------------------------------------------------

# Configure the core runtime and opt-in libraries, tests and install components.
# TileLang/Relay consume prebuilt device artifacts; this build does not run TVM.
cmake_minimum_required(VERSION 3.16)

project(tilexr LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 14)
option(TILEXR_BUILD_COLLECTIVES "Build optional TileXR collectives library" OFF)
option(TILEXR_BUILD_DIAGNOSTICS "Build common TileXR clock and straggler diagnostics" OFF)
option(TILEXR_BUILD_RELAY "Build optional generic TileRelay runtime" OFF)
option(TILEXR_BUILD_EP "Build TileXR EP communication library" OFF)
option(TILEXR_BUILD_MOONEP_PLANNER "Build A5-only MoonEP Planner library" OFF)
option(TILEXR_BUILD_MOONEP "Build TileXR MoonEP ABI and native stage libraries" OFF)
option(TILEXR_BUILD_TESTS "Build TileXR tests" OFF)
option(TILEXR_BUILD_CHECKER "Build TileXR no-NPU checker" OFF)
include(CTest)
if(BUILD_TESTING OR TILEXR_BUILD_TESTS)
    enable_testing()
endif()
if(BUILD_TESTING OR TILEXR_BUILD_TESTS)
    add_subdirectory(tests/data_as_flag)
endif()
if(TILEXR_BUILD_TESTS)
    add_subdirectory(tests/ci)
endif()
include(GNUInstallDirs)

add_link_options(-Wl,-z,relro,-z,now)
add_link_options(-s)
# 获取环境变量
IF (CMAKE_HOST_SYSTEM_PROCESSOR MATCHES "aarch64")
    SET(ARCH aarch64)
ELSEIF (CMAKE_HOST_SYSTEM_PROCESSOR MATCHES "x86_64")
    SET(ARCH x86_64)
ENDIF ()
if (DEFINED ENV{ASCEND_HOME_PATH})
    set(ASCEND_HOME_PATH $ENV{ASCEND_HOME_PATH})
else ()
    message("ASCEND_HOME_PATH not set! using default path!")
    set(ASCEND_HOME_PATH /usr/local/Ascend/ascend-toolkit/latest)
endif ()

set(_tilexr_default_ascend_driver_path "$ENV{ASCEND_DRIVER_PATH}")
if(NOT _tilexr_default_ascend_driver_path)
    set(_tilexr_default_ascend_driver_path /usr/local/Ascend/driver)
endif()
set(ASCEND_DRIVER_PATH "${_tilexr_default_ascend_driver_path}" CACHE PATH "Ascend driver path")

message("== ASCEND_HOME_PATH: ${ASCEND_HOME_PATH}")
message("== ASCEND_DRIVER_PATH: ${ASCEND_DRIVER_PATH}")

include_directories(
        ${CMAKE_CURRENT_LIST_DIR}/src/include/
        ${ASCEND_HOME_PATH}/${ARCH}-linux/pkg_inc/
        ${ASCEND_HOME_PATH}/${ARCH}-linux/pkg_inc/profiling/
        ${ASCEND_HOME_PATH}/${ARCH}-linux/pkg_inc/runtime/
        ${ASCEND_HOME_PATH}/${ARCH}-linux/include/
        ${ASCEND_HOME_PATH}/${ARCH}-linux/include/experiment
        ${ASCEND_HOME_PATH}/${ARCH}-linux/include/experiment/runtime/
        ${ASCEND_HOME_PATH}/${ARCH}-linux/include/experiment/msprof/
        ${CMAKE_SOURCE_DIR}/3rdparty/spdlog/include
        ${CMAKE_SOURCE_DIR}/3rdparty/
)
add_subdirectory(src/profiling)
add_subdirectory(src/comm)
if(TILEXR_BUILD_DIAGNOSTICS OR TILEXR_BUILD_COLLECTIVES OR TILEXR_BUILD_EP OR TILEXR_BUILD_MOONEP)
    add_subdirectory(src/diagnostics)
    if(BUILD_TESTING OR TILEXR_BUILD_TESTS)
        add_subdirectory(tests/diagnostics)
        configure_file(
            tests/diagnostics/cmake/install_prefix_smoke.cmake.in
            ${CMAKE_BINARY_DIR}/tilexr_diagnostics_install_smoke.cmake
            @ONLY
        )
        add_test(NAME tilexr_diagnostics_install_smoke
            COMMAND ${CMAKE_COMMAND} -P ${CMAKE_BINARY_DIR}/tilexr_diagnostics_install_smoke.cmake)
    endif()
endif()
if(TILEXR_BUILD_RELAY)
    add_subdirectory(src/relay)
    if(BUILD_TESTING OR TILEXR_BUILD_TESTS)
        add_subdirectory(tests/relay)
    endif()
endif()
if(TILEXR_BUILD_EP)
    add_subdirectory(src/ep)
endif()
if(TILEXR_BUILD_MOONEP OR TILEXR_BUILD_MOONEP_PLANNER)
    add_subdirectory(src/moonep)
    if(BUILD_TESTING OR TILEXR_BUILD_TESTS)
        add_subdirectory(tests/moonep_planner)
        if(TILEXR_BUILD_MOONEP)
            add_subdirectory(tests/moonep_combine_v2)
        endif()
    endif()
endif()
if(TILEXR_BUILD_COLLECTIVES)
    add_subdirectory(src/collectives)
    if(BUILD_TESTING OR TILEXR_BUILD_TESTS)
        add_subdirectory(tests/collectives)
        configure_file(
            tests/collectives/cmake/install_prefix_smoke.cmake.in
            ${CMAKE_BINARY_DIR}/tilexr_collectives_install_smoke.cmake
            @ONLY
        )
        add_test(NAME tilexr_collectives_install_smoke
            COMMAND ${CMAKE_COMMAND} -P ${CMAKE_BINARY_DIR}/tilexr_collectives_install_smoke.cmake)
    endif()
endif()
if(TILEXR_BUILD_CHECKER)
    add_subdirectory(tools/checker)
    if(BUILD_TESTING OR TILEXR_BUILD_TESTS)
        add_subdirectory(tests/checker)
    endif()
endif()
