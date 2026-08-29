cmake_minimum_required(VERSION 3.16)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(TileXR_MoonEP_Planner_Tests LANGUAGES CXX)
endif()

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
include(GNUInstallDirs)
enable_testing()

option(BUILD_TILEXR_MOONEP_PLANNER_DEMO "Build A5 MoonEP planner hardware demo" OFF)
set(TILEXR_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../..")
set(TILEXR_MOONEP_PLANNER_FAKE_INCLUDE_DIR "${CMAKE_CURRENT_BINARY_DIR}/fake_include")
file(MAKE_DIRECTORY "${TILEXR_MOONEP_PLANNER_FAKE_INCLUDE_DIR}/acl")
file(WRITE "${TILEXR_MOONEP_PLANNER_FAKE_INCLUDE_DIR}/acl/acl_base.h"
"#ifndef TILEXR_MOONEP_PLANNER_TEST_ACL_BASE_H\n"
"#define TILEXR_MOONEP_PLANNER_TEST_ACL_BASE_H\n"
"typedef void *aclrtStream;\n"
"#endif\n")

add_executable(test_tilexr_moonep_planner_layout
    unit/test_planner_layout.cpp
    ${TILEXR_ROOT}/src/moonep/planner_v2/host/planner_layout.cpp
)

add_executable(test_tilexr_moonep_planner_v3_layout
    unit/test_planner_v3_layout.cpp
    ${TILEXR_ROOT}/src/moonep/planner/host/planner_layout.cpp
)
target_include_directories(test_tilexr_moonep_planner_v3_layout PRIVATE
    ${TILEXR_ROOT}/src/include
    ${TILEXR_ROOT}/src/moonep/planner/common
    ${TILEXR_ROOT}/src/moonep/planner/host
)
target_include_directories(test_tilexr_moonep_planner_layout PRIVATE
    ${TILEXR_ROOT}/src/include
    ${TILEXR_ROOT}/src/moonep/planner_v2/common
    ${TILEXR_ROOT}/src/moonep/planner_v2/host
)

add_executable(test_tilexr_moonep_planner_reference
    unit/test_planner_reference.cpp
    reference/planner_reference.cpp
)
target_include_directories(test_tilexr_moonep_planner_reference PRIVATE reference)

add_executable(test_tilexr_moonep_planner_host
    unit/test_planner_host.cpp
    ${TILEXR_ROOT}/src/moonep/planner_v2/host/planner_host.cpp
    ${TILEXR_ROOT}/src/moonep/planner_v2/host/planner_layout.cpp
)
target_include_directories(test_tilexr_moonep_planner_host PRIVATE
    ${TILEXR_MOONEP_PLANNER_FAKE_INCLUDE_DIR}
    ${TILEXR_ROOT}/src/include
    ${TILEXR_ROOT}/src/moonep/planner_v2/common
    ${TILEXR_ROOT}/src/moonep/planner_v2/host
)

add_executable(test_tilexr_moonep_planner_public_abi unit/test_planner_public_abi.cpp)
target_include_directories(test_tilexr_moonep_planner_public_abi PRIVATE
    ${TILEXR_MOONEP_PLANNER_FAKE_INCLUDE_DIR}
    ${TILEXR_ROOT}/src/include
)

add_test(NAME test_tilexr_moonep_planner_layout COMMAND test_tilexr_moonep_planner_layout)
add_test(NAME test_tilexr_moonep_planner_v3_layout COMMAND test_tilexr_moonep_planner_v3_layout)
add_test(NAME test_tilexr_moonep_planner_reference COMMAND test_tilexr_moonep_planner_reference)
add_test(NAME test_tilexr_moonep_planner_host COMMAND test_tilexr_moonep_planner_host)
add_test(NAME test_tilexr_moonep_planner_public_abi COMMAND test_tilexr_moonep_planner_public_abi)

install(TARGETS
    test_tilexr_moonep_planner_layout
    test_tilexr_moonep_planner_v3_layout
    test_tilexr_moonep_planner_reference
    test_tilexr_moonep_planner_host
    test_tilexr_moonep_planner_public_abi
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

if(BUILD_TILEXR_MOONEP_PLANNER_DEMO)
    set(TILEXR_INSTALL_PREFIX "${TILEXR_ROOT}/install" CACHE PATH "TileXR install prefix")
    set(ASCEND_HOME_PATH "$ENV{ASCEND_HOME_PATH}" CACHE PATH "Ascend toolkit path")
    set(ASCEND_DRIVER_PATH "$ENV{ASCEND_DRIVER_PATH}" CACHE PATH "Ascend driver path")
    if(NOT ASCEND_HOME_PATH)
        set(ASCEND_HOME_PATH "/usr/local/Ascend/ascend-toolkit/latest")
    endif()
    if(NOT ASCEND_DRIVER_PATH)
        set(ASCEND_DRIVER_PATH "/usr/local/Ascend/driver")
    endif()
    execute_process(COMMAND uname -m OUTPUT_VARIABLE ARCH OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(ARCH STREQUAL "arm64")
        set(ARCH aarch64)
    endif()
    find_library(TILEXR_PLANNER_LIB tilexr-moonep-planner
        HINTS ${TILEXR_INSTALL_PREFIX}/lib64 ${TILEXR_INSTALL_PREFIX}/lib REQUIRED)
    find_library(TILEXR_COMM_LIB tile-comm
        HINTS ${TILEXR_INSTALL_PREFIX}/lib64 ${TILEXR_INSTALL_PREFIX}/lib REQUIRED)
    add_executable(tilexr_moonep_planner_demo
        demo/tilexr_moonep_planner_demo.cpp
        reference/planner_reference.cpp)
    target_include_directories(tilexr_moonep_planner_demo PRIVATE
        reference
        ${TILEXR_INSTALL_PREFIX}/include
        ${ASCEND_HOME_PATH}/${ARCH}-linux/include
        ${ASCEND_HOME_PATH}/${ARCH}-linux/pkg_inc
        ${ASCEND_HOME_PATH}/${ARCH}-linux/pkg_inc/runtime)
    target_link_directories(tilexr_moonep_planner_demo PRIVATE
        ${TILEXR_INSTALL_PREFIX}/lib64
        ${ASCEND_HOME_PATH}/${ARCH}-linux/lib64
        ${ASCEND_DRIVER_PATH}/lib64/driver)
    target_link_libraries(tilexr_moonep_planner_demo PRIVATE
        ${TILEXR_PLANNER_LIB} ${TILEXR_COMM_LIB} ascendcl runtime ascend_hal)
    install(TARGETS tilexr_moonep_planner_demo RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()
