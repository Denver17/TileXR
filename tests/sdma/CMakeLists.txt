cmake_minimum_required(VERSION 3.16)
project(TileXR_SDMA_Tests)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
option(BUILD_TILEXR_SDMA_DEMO "Build TileXR SDMA data-plane demo with Ascend C kernel" ON)
set(TILEXR_SDMA_DEMO_SOC_TYPE "Ascend910B" CACHE STRING "SOC type used for the TileXR SDMA demo kernel")

set(ASCEND_HOME_PATH $ENV{ASCEND_HOME_PATH})
set(ARCH $ENV{ARCH})
if(NOT ARCH)
    set(ARCH $ENV{TILEXR_OS_ARCH})
endif()
if(NOT ARCH)
    execute_process(COMMAND uname -m OUTPUT_VARIABLE ARCH OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(ARCH STREQUAL "arm64")
        set(ARCH "aarch64")
    endif()
endif()

set(ASCEND_DRIVER_PATH $ENV{ASCEND_DRIVER_PATH})
if(NOT ASCEND_DRIVER_PATH)
    set(ASCEND_DRIVER_PATH "/usr/local/Ascend/driver")
endif()

set(TILEXR_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../..")
set(TILEXR_INSTALLED_SDMA_CONFIG "${TILEXR_ROOT}/install/include/tilexr_sdma_config.h")
set(TILEXR_TEST_HAVE_PTO_SDMA OFF)
if(EXISTS "${TILEXR_INSTALLED_SDMA_CONFIG}")
    file(READ "${TILEXR_INSTALLED_SDMA_CONFIG}" TILEXR_INSTALLED_SDMA_CONFIG_TEXT)
    if(TILEXR_INSTALLED_SDMA_CONFIG_TEXT MATCHES "#define[ \t]+TILEXR_HAVE_PTO_SDMA[ \t]+1")
        set(TILEXR_TEST_HAVE_PTO_SDMA ON)
    endif()
endif()
message(STATUS "TILEXR_TEST_HAVE_PTO_SDMA: ${TILEXR_TEST_HAVE_PTO_SDMA}")

find_library(TILEXR_LIB tile-comm
    HINTS "${TILEXR_ROOT}/install/lib64" "${TILEXR_ROOT}/install/lib" "${TILEXR_ROOT}/build/src/comm"
    REQUIRED)

link_directories(
    ${ASCEND_DRIVER_PATH}/lib64/driver
    ${ASCEND_HOME_PATH}/${ARCH}-linux/lib64
    ${ASCEND_HOME_PATH}/${ARCH}-linux/devlib
)

add_executable(test_tilexr_sdma_metadata
    unit/test_tilexr_sdma_metadata.cpp
)
target_include_directories(test_tilexr_sdma_metadata PRIVATE
    ${TILEXR_ROOT}/install/include
    ${TILEXR_ROOT}/src/include
    ${ASCEND_HOME_PATH}/${ARCH}-linux/pkg_inc/
    ${ASCEND_HOME_PATH}/${ARCH}-linux/pkg_inc/runtime/
    ${ASCEND_HOME_PATH}/${ARCH}-linux/include/
    ${ASCEND_DRIVER_PATH}/kernel/inc
)

add_executable(test_tilexr_sdma_api_invalid
    unit/test_tilexr_sdma_api_invalid.cpp
)
target_include_directories(test_tilexr_sdma_api_invalid PRIVATE
    ${TILEXR_ROOT}/install/include
    ${TILEXR_ROOT}/src/include
    ${ASCEND_HOME_PATH}/${ARCH}-linux/pkg_inc/
    ${ASCEND_HOME_PATH}/${ARCH}-linux/pkg_inc/runtime/
    ${ASCEND_HOME_PATH}/${ARCH}-linux/include/
    ${ASCEND_DRIVER_PATH}/kernel/inc
)
target_link_libraries(test_tilexr_sdma_api_invalid
    ${TILEXR_LIB}
    ascendcl
    runtime
    ascend_hal
)

add_executable(test_tilexr_sdma_transport_disabled
    unit/test_tilexr_sdma_transport_disabled.cpp
    ${TILEXR_ROOT}/src/comm/sdma/tilexr_sdma_transport.cpp
)
target_include_directories(test_tilexr_sdma_transport_disabled PRIVATE
    ${TILEXR_ROOT}/src/comm
    ${TILEXR_ROOT}/src/include
    ${ASCEND_HOME_PATH}/${ARCH}-linux/pkg_inc/
    ${ASCEND_HOME_PATH}/${ARCH}-linux/pkg_inc/runtime/
    ${ASCEND_HOME_PATH}/${ARCH}-linux/include/
    ${ASCEND_DRIVER_PATH}/kernel/inc
)

add_executable(test_tilexr_sdma_header_compile
    unit/test_tilexr_sdma_header_compile.cpp
)
target_include_directories(test_tilexr_sdma_header_compile PRIVATE
    ${TILEXR_ROOT}/install/include
    ${ASCEND_HOME_PATH}/${ARCH}-linux/pkg_inc/
    ${ASCEND_HOME_PATH}/${ARCH}-linux/pkg_inc/runtime/
    ${ASCEND_HOME_PATH}/${ARCH}-linux/include/
    ${ASCEND_DRIVER_PATH}/kernel/inc
)

add_executable(test_tilexr_sdma_disabled_comm
    integration/test_tilexr_sdma_disabled_comm.cpp
)
target_include_directories(test_tilexr_sdma_disabled_comm PRIVATE
    ${TILEXR_ROOT}/install/include
    ${TILEXR_ROOT}/src/include
    ${ASCEND_HOME_PATH}/${ARCH}-linux/pkg_inc/
    ${ASCEND_HOME_PATH}/${ARCH}-linux/pkg_inc/runtime/
    ${ASCEND_HOME_PATH}/${ARCH}-linux/include/
    ${ASCEND_DRIVER_PATH}/kernel/inc
)
target_link_libraries(test_tilexr_sdma_disabled_comm
    ${TILEXR_LIB}
    ascendcl
    runtime
)
set(INSTALL_TARGETS
    test_tilexr_sdma_metadata
    test_tilexr_sdma_api_invalid
    test_tilexr_sdma_transport_disabled
    test_tilexr_sdma_header_compile
    test_tilexr_sdma_disabled_comm
)

if(BUILD_TILEXR_SDMA_DEMO AND NOT TILEXR_TEST_HAVE_PTO_SDMA)
    message(WARNING "installed tilexr_sdma_config.h disables PTO SDMA; skip tilexr_sdma_demo")
elseif(BUILD_TILEXR_SDMA_DEMO)
    find_program(BISHENG_EXECUTABLE bisheng)
    if(NOT BISHENG_EXECUTABLE)
        message(WARNING "bisheng not found; skip tilexr_sdma_demo")
    else()
        if(TILEXR_SDMA_DEMO_SOC_TYPE STREQUAL "Ascend950")
            set(TILEXR_SDMA_NPU_ARCH "dav-3510")
            set(TILEXR_SDMA_AICORE_ARCH "--cce-aicore-arch=dav-c310-vec")
            set(TILEXR_SDMA_CATLASS_ARCH "3510")
        else()
            set(TILEXR_SDMA_NPU_ARCH "dav-2201")
            set(TILEXR_SDMA_AICORE_ARCH "--cce-aicore-arch=dav-c220-vec")
            set(TILEXR_SDMA_CATLASS_ARCH "2201")
        endif()

        execute_process(
            COMMAND ${BISHENG_EXECUTABLE} -v
            OUTPUT_VARIABLE BISHENG_VERSION_OUTPUT
            ERROR_VARIABLE BISHENG_VERSION_OUTPUT
            RESULT_VARIABLE BISHENG_VERSION_RESULT
        )
        if(BISHENG_VERSION_RESULT EQUAL 0 AND BISHENG_VERSION_OUTPUT MATCHES "([0-9]{8})")
            set(TILEXR_SDMA_BISHENG_DATE "${CMAKE_MATCH_1}")
        else()
            set(TILEXR_SDMA_BISHENG_DATE "0")
        endif()
        if(TILEXR_SDMA_BISHENG_DATE GREATER_EQUAL 20250428)
            set(TILEXR_SDMA_KERNEL_COMPILE_OPTIONS
                -xasc
                -Xhost-start -ftrapv -Xhost-end
                --npu-arch=${TILEXR_SDMA_NPU_ARCH}
                --cce-auto-infer-kernel-type=false
            )
            set(TILEXR_SDMA_KERNEL_LINK_OPTIONS --cce-fatobj-link)
        else()
            set(TILEXR_SDMA_KERNEL_COMPILE_OPTIONS
                -xcce
                -Xhost-start -ftrapv -Xhost-end
                -mllvm -cce-aicore-stack-size=0x8000
                -mllvm -cce-aicore-function-stack-size=0x8000
                -mllvm -cce-aicore-record-overflow=true
                -mllvm -cce-aicore-addr-transform
                -mllvm -cce-aicore-dcci-insert-for-scalar=false
                ${TILEXR_SDMA_AICORE_ARCH}
            )
            set(TILEXR_SDMA_KERNEL_LINK_OPTIONS --cce-fatobj-link ${TILEXR_SDMA_AICORE_ARCH})
        endif()

        set(TILEXR_SDMA_DEMO_KERNEL_SO "${CMAKE_CURRENT_BINARY_DIR}/libtilexr_sdma_demo_kernel.so")
        set(TILEXR_SDMA_DEMO_KERNEL_INCLUDES
            -I${ASCEND_HOME_PATH}/compiler/tikcpp
            -I${ASCEND_HOME_PATH}/compiler/tikcpp/tikcfw
            -I${ASCEND_HOME_PATH}/compiler/tikcpp/tikcfw/impl
            -I${ASCEND_HOME_PATH}/compiler/tikcpp/tikcfw/interface
            -I${ASCEND_HOME_PATH}/${ARCH}-linux/tikcpp
            -I${ASCEND_HOME_PATH}/${ARCH}-linux/tikcpp/tikcfw
            -I${ASCEND_HOME_PATH}/${ARCH}-linux/tikcpp/tikcfw/impl
            -I${ASCEND_HOME_PATH}/${ARCH}-linux/tikcpp/tikcfw/interface
            -I${ASCEND_HOME_PATH}/${ARCH}-linux/pkg_inc/
            -I${ASCEND_HOME_PATH}/${ARCH}-linux/pkg_inc/runtime/
            -I${ASCEND_HOME_PATH}/${ARCH}-linux/include/
            -I${ASCEND_DRIVER_PATH}/kernel/inc
            -I${TILEXR_ROOT}/src/include
        )
        add_custom_command(
            OUTPUT "${TILEXR_SDMA_DEMO_KERNEL_SO}"
            COMMAND ${BISHENG_EXECUTABLE}
                ${TILEXR_SDMA_KERNEL_COMPILE_OPTIONS}
                -std=gnu++17
                -fPIC
                -shared
                ${TILEXR_SDMA_KERNEL_LINK_OPTIONS}
                -DCATLASS_ARCH=${TILEXR_SDMA_CATLASS_ARCH}
                -DTILEXR_HAVE_PTO_SDMA=1
                ${TILEXR_SDMA_DEMO_KERNEL_INCLUDES}
                "${CMAKE_CURRENT_SOURCE_DIR}/demo/tilexr_sdma_demo_kernel.cpp"
                -L${ASCEND_DRIVER_PATH}/lib64
                -L${ASCEND_DRIVER_PATH}/lib64/driver
                -L${ASCEND_HOME_PATH}/${ARCH}-linux/lib64
                -L${ASCEND_HOME_PATH}/${ARCH}-linux/devlib
                -lruntime
                -lascendcl
                -lstdc++
                -lm
                -ldl
                -lnnopbase
                -lpthread
                -o "${TILEXR_SDMA_DEMO_KERNEL_SO}"
            DEPENDS
                "${CMAKE_CURRENT_SOURCE_DIR}/demo/tilexr_sdma_demo_kernel.cpp"
                "${TILEXR_ROOT}/src/include/tilexr_sdma.h"
                "${TILEXR_ROOT}/src/include/tilexr_sdma_compat.h"
                "${TILEXR_ROOT}/src/include/tilexr_sdma_types.h"
            VERBATIM
            COMMENT "Building TileXR SDMA demo kernel with bisheng"
        )
        add_custom_target(tilexr_sdma_demo_kernel ALL DEPENDS "${TILEXR_SDMA_DEMO_KERNEL_SO}")

        add_executable(tilexr_sdma_demo
            demo/tilexr_sdma_demo.cpp
        )
        add_dependencies(tilexr_sdma_demo tilexr_sdma_demo_kernel)
        target_include_directories(tilexr_sdma_demo PRIVATE
            ${TILEXR_ROOT}/install/include
            ${TILEXR_ROOT}/src/include
            ${ASCEND_HOME_PATH}/${ARCH}-linux/pkg_inc/
            ${ASCEND_HOME_PATH}/${ARCH}-linux/pkg_inc/runtime/
            ${ASCEND_HOME_PATH}/${ARCH}-linux/include/
            ${ASCEND_DRIVER_PATH}/kernel/inc
        )
        target_link_directories(tilexr_sdma_demo PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
        target_link_libraries(tilexr_sdma_demo
            "${TILEXR_SDMA_DEMO_KERNEL_SO}"
            ${TILEXR_LIB}
            ascendcl
            runtime
            ascend_hal
        )
        list(APPEND INSTALL_TARGETS tilexr_sdma_demo)
        install(FILES "${TILEXR_SDMA_DEMO_KERNEL_SO}" DESTINATION ${CMAKE_INSTALL_PREFIX}/lib)
        message(STATUS "TileXR SDMA demo enabled with ${BISHENG_EXECUTABLE}, SOC=${TILEXR_SDMA_DEMO_SOC_TYPE}")
    endif()
endif()

install(TARGETS ${INSTALL_TARGETS}
    RUNTIME DESTINATION ${CMAKE_INSTALL_PREFIX}/bin
    LIBRARY DESTINATION ${CMAKE_INSTALL_PREFIX}/lib
)
