include(GNUInstallDirs)

if(NOT TARGET tilexr-moonep-planner)
    message(FATAL_ERROR "Planner V3 requires the PR96 Planner V2 shared-library target")
endif()

find_program(BISHENG_EXECUTABLE bisheng)
if(NOT BISHENG_EXECUTABLE)
    message(FATAL_ERROR "bisheng not found; source the CANN environment before configuring the MoonEP planner")
endif()
include(${CMAKE_SOURCE_DIR}/src/moonep/cmake/MoonEpKernel.cmake)
include(${CMAKE_SOURCE_DIR}/src/cmake/TileXRKernelArtifact.cmake)

set(TILEXR_MOONEP_PLANNER_DEFAULT_SOC_TYPE "$ENV{TILEXR_SOC_NAME}")
if(NOT TILEXR_MOONEP_PLANNER_DEFAULT_SOC_TYPE)
    set(TILEXR_MOONEP_PLANNER_DEFAULT_SOC_TYPE "Ascend950")
endif()
set(TILEXR_MOONEP_PLANNER_SOC_TYPE "${TILEXR_MOONEP_PLANNER_DEFAULT_SOC_TYPE}"
    CACHE STRING "TileXR MoonEP planner kernel SOC type")
string(TOLOWER "${TILEXR_MOONEP_PLANNER_SOC_TYPE}" TILEXR_MOONEP_PLANNER_SOC_TYPE_LOWER)

if(NOT TILEXR_MOONEP_PLANNER_SOC_TYPE_LOWER MATCHES "ascend950|ascend910a5|910a5")
    message(FATAL_ERROR
        "TileXR MoonEP planner is A5-only; got TILEXR_MOONEP_PLANNER_SOC_TYPE=${TILEXR_MOONEP_PLANNER_SOC_TYPE}")
endif()

set(TILEXR_MOONEP_PLANNER_KERNEL_SOURCE
    "${CMAKE_CURRENT_SOURCE_DIR}/kernels/tilexr_moonep_planner_kernel.cpp")
set(TILEXR_MOONEP_PLANNER_KERNEL_BINARY
    "${CMAKE_CURRENT_BINARY_DIR}/tilexr_moonep_planner_kernel.o")

set(TILEXR_MOONEP_PLANNER_KERNEL_OPTIONS
    -xcce
    -mllvm -cce-aicore-stack-size=0x8000
    -mllvm -cce-aicore-function-stack-size=0x8000
    -mllvm -cce-aicore-record-overflow=true
    -mllvm -cce-aicore-addr-transform
    -mllvm -cce-aicore-dcci-insert-for-scalar=false
    --cce-aicore-arch=dav-c310-vec
)

set(TILEXR_MOONEP_PLANNER_KERNEL_INCLUDES
    -I${ASCEND_HOME_PATH}/compiler/tikcpp
    -I${ASCEND_HOME_PATH}/compiler/tikcpp/tikcfw
    -I${ASCEND_HOME_PATH}/compiler/tikcpp/tikcfw/impl
    -I${ASCEND_HOME_PATH}/compiler/tikcpp/tikcfw/interface
    -I${ASCEND_HOME_PATH}/${ARCH}-linux/ascendc/include
    -I${ASCEND_HOME_PATH}/${ARCH}-linux/include
    -I${ASCEND_HOME_PATH}/${ARCH}-linux/pkg_inc
    -I${ASCEND_HOME_PATH}/${ARCH}-linux/pkg_inc/runtime
    -I${ASCEND_DRIVER_PATH}/kernel/inc
    -I${CMAKE_SOURCE_DIR}/src/include
    -I${CMAKE_CURRENT_SOURCE_DIR}/common
)

tilexr_add_moonep_kernel(tilexr_moonep_planner_kernel
    SOURCE "${TILEXR_MOONEP_PLANNER_KERNEL_SOURCE}"
    BINARY "${TILEXR_MOONEP_PLANNER_KERNEL_BINARY}"
    OPTIONS ${TILEXR_MOONEP_PLANNER_KERNEL_OPTIONS}
    INCLUDES ${TILEXR_MOONEP_PLANNER_KERNEL_INCLUDES}
    DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/common/planner_common.h"
        "${CMAKE_SOURCE_DIR}/src/include/comm_args.h"
        "${CMAKE_SOURCE_DIR}/src/include/tilexr_sync.h"
)

add_library(tilexr-moonep-planner-v3-objects OBJECT
    host/planner_layout.cpp
    host/planner_host.cpp
    host/planner_launch.cpp
    host/tilexr_moonep_planner.cpp
)
add_dependencies(tilexr-moonep-planner-v3-objects tilexr_moonep_planner_kernel)
target_compile_features(tilexr-moonep-planner-v3-objects PRIVATE cxx_std_14)
set_target_properties(tilexr-moonep-planner-v3-objects PROPERTIES
    POSITION_INDEPENDENT_CODE ON)

target_include_directories(tilexr-moonep-planner-v3-objects
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../../include
        ${CMAKE_CURRENT_SOURCE_DIR}/common
        ${CMAKE_CURRENT_SOURCE_DIR}/host
        ${CMAKE_SOURCE_DIR}/src/moonep/common
        ${ASCEND_HOME_PATH}/${ARCH}-linux/pkg_inc
        ${ASCEND_HOME_PATH}/${ARCH}-linux/pkg_inc/runtime
)
target_sources(tilexr-moonep-planner PRIVATE
    $<TARGET_OBJECTS:tilexr-moonep-planner-v3-objects>)
tilexr_attach_kernel_artifacts(tilexr-moonep-planner FILES
    ${TILEXR_MOONEP_PLANNER_KERNEL_BINARY}
    TARGETS tilexr_moonep_planner_kernel)
