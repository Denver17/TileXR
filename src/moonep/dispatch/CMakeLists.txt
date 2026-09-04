include(GNUInstallDirs)

find_program(BISHENG_EXECUTABLE bisheng)
if(NOT BISHENG_EXECUTABLE)
    message(FATAL_ERROR "bisheng not found; source the CANN environment before configuring MoonEP dispatch")
endif()
include(${CMAKE_SOURCE_DIR}/src/moonep/cmake/MoonEpKernel.cmake)
include(${CMAKE_SOURCE_DIR}/src/cmake/TileXRKernelArtifact.cmake)

option(TILEXR_MOONEP_DISPATCH_ENABLE_PROFILING
    "Enable per-AIV profiling in the URMA MoonEP Dispatch kernel" OFF)
option(TILEXR_MOONEP_DISPATCH_ENABLE_DFX
    "Enable detailed per-AIV DFX in the URMA MoonEP Dispatch kernel" OFF)
if(TILEXR_MOONEP_DISPATCH_ENABLE_PROFILING AND
    NOT TILEXR_MOONEP_DISPATCH_ENABLE_DFX)
    message(FATAL_ERROR
        "TILEXR_MOONEP_DISPATCH_ENABLE_PROFILING requires TILEXR_MOONEP_DISPATCH_ENABLE_DFX")
endif()
set(TILEXR_MOONEP_DISPATCH_SYSTEM_CYCLES_PER_SECOND "1000000000" CACHE STRING
    "Ascend950 GetSystemCycle ticks per second for the URMA Dispatch timeout")
if(NOT "${TILEXR_MOONEP_DISPATCH_SYSTEM_CYCLES_PER_SECOND}" MATCHES "^[1-9][0-9]*$")
    message(FATAL_ERROR
        "TILEXR_MOONEP_DISPATCH_SYSTEM_CYCLES_PER_SECOND must be a positive integer")
endif()

set(TILEXR_MOONEP_DISPATCH_DEFAULT_SOC_TYPE "$ENV{TILEXR_SOC_NAME}")
if(NOT TILEXR_MOONEP_DISPATCH_DEFAULT_SOC_TYPE)
    set(TILEXR_MOONEP_DISPATCH_DEFAULT_SOC_TYPE "Ascend950")
endif()
set(TILEXR_MOONEP_DISPATCH_SOC_TYPE "${TILEXR_MOONEP_DISPATCH_DEFAULT_SOC_TYPE}"
    CACHE STRING "TileXR MoonEP dispatch kernel SOC type")
string(TOLOWER "${TILEXR_MOONEP_DISPATCH_SOC_TYPE}" TILEXR_MOONEP_DISPATCH_SOC_TYPE_LOWER)
if(NOT TILEXR_MOONEP_DISPATCH_SOC_TYPE_LOWER MATCHES "ascend950|ascend910a5|910a5")
    message(FATAL_ERROR
        "TileXR MoonEP dispatch is A5-only; got TILEXR_MOONEP_DISPATCH_SOC_TYPE=${TILEXR_MOONEP_DISPATCH_SOC_TYPE}")
endif()

set(TILEXR_MOONEP_DISPATCH_KERNEL_SOURCE
    "${CMAKE_CURRENT_SOURCE_DIR}/kernels/tilexr_moonep_dispatch_kernel.cpp")
set(TILEXR_MOONEP_DISPATCH_KERNEL_BINARY
    "${CMAKE_CURRENT_BINARY_DIR}/tilexr_moonep_dispatch_kernel.o")
set(TILEXR_MOONEP_DISPATCH_KERNEL_OPTIONS
    -xcce
    -mllvm -cce-aicore-stack-size=0x8000
    -mllvm -cce-aicore-function-stack-size=0x8000
    -mllvm -cce-aicore-record-overflow=true
    -mllvm -cce-aicore-addr-transform
    -mllvm -cce-aicore-dcci-insert-for-scalar=false
    --cce-aicore-arch=dav-c310-vec
)
set(TILEXR_MOONEP_DISPATCH_KERNEL_INCLUDES
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
    -I${CMAKE_SOURCE_DIR}/src/moonep/common
    -I${CMAKE_CURRENT_SOURCE_DIR}/common
)

tilexr_add_moonep_kernel(tilexr_moonep_dispatch_kernel
    SOURCE "${TILEXR_MOONEP_DISPATCH_KERNEL_SOURCE}"
    BINARY "${TILEXR_MOONEP_DISPATCH_KERNEL_BINARY}"
    OPTIONS ${TILEXR_MOONEP_DISPATCH_KERNEL_OPTIONS}
    INCLUDES ${TILEXR_MOONEP_DISPATCH_KERNEL_INCLUDES}
    DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/common/dispatch_common.h"
        "${CMAKE_SOURCE_DIR}/src/moonep/common/moonep_peer_window.h"
        "${CMAKE_SOURCE_DIR}/src/include/comm_args.h"
        "${CMAKE_SOURCE_DIR}/src/include/tilexr_sync.h"
)

set(TILEXR_MOONEP_DISPATCH_URMA_KERNEL_SOURCE
    "${CMAKE_CURRENT_SOURCE_DIR}/urma/kernels/tilexr_moonep_dispatch_kernel.cpp")
set(TILEXR_MOONEP_DISPATCH_URMA_KERNEL_BINARY
    "${CMAKE_CURRENT_BINARY_DIR}/tilexr_moonep_dispatch_urma_kernel.o")
set(TILEXR_MOONEP_DISPATCH_URMA_KERNEL_OPTIONS
    -xcce
    -O2
    -DCATLASS_ARCH=3510
    --cce-auto-sync
    -mllvm -cce-aicore-stack-size=0x8000
    -mllvm -cce-aicore-function-stack-size=0x8000
    -mllvm -cce-aicore-record-overflow=true
    -mllvm -cce-aicore-addr-transform
    -mllvm -cce-aicore-dcci-insert-for-scalar=false
    --cce-aicore-arch=dav-c310-vec
)
if(TILEXR_MOONEP_DISPATCH_ENABLE_DFX)
    list(APPEND TILEXR_MOONEP_DISPATCH_URMA_KERNEL_OPTIONS
        -DTILEXR_MOONEP_DISPATCH_ENABLE_DFX=1)
endif()
if(TILEXR_MOONEP_DISPATCH_ENABLE_PROFILING)
    list(APPEND TILEXR_MOONEP_DISPATCH_URMA_KERNEL_OPTIONS
        -DTILEXR_MOONEP_DISPATCH_ENABLE_PROFILING=1)
endif()
set(TILEXR_MOONEP_DISPATCH_URMA_KERNEL_INCLUDES
    -I${ASCEND_HOME_PATH}/compiler/tikcpp
    -I${ASCEND_HOME_PATH}/compiler/tikcpp/tikcfw
    -I${ASCEND_HOME_PATH}/compiler/tikcpp/tikcfw/impl
    -I${ASCEND_HOME_PATH}/compiler/tikcpp/tikcfw/interface
    -I${ASCEND_HOME_PATH}/${ARCH}-linux/ascendc/include
    -I${ASCEND_HOME_PATH}/${ARCH}-linux/include
    -I${ASCEND_HOME_PATH}/${ARCH}-linux/pkg_inc
    -I${ASCEND_HOME_PATH}/${ARCH}-linux/pkg_inc/runtime
    -I${ASCEND_DRIVER_PATH}/kernel/inc
    -I${CMAKE_SOURCE_DIR}/3rdparty
    -I${CMAKE_SOURCE_DIR}/src/include
    -I${CMAKE_CURRENT_SOURCE_DIR}/urma/common
)

tilexr_add_moonep_kernel(tilexr_moonep_dispatch_urma_kernel
    SOURCE "${TILEXR_MOONEP_DISPATCH_URMA_KERNEL_SOURCE}"
    BINARY "${TILEXR_MOONEP_DISPATCH_URMA_KERNEL_BINARY}"
    OPTIONS ${TILEXR_MOONEP_DISPATCH_URMA_KERNEL_OPTIONS}
    INCLUDES ${TILEXR_MOONEP_DISPATCH_URMA_KERNEL_INCLUDES}
    DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/urma/common/dispatch_common.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/urma/common/dispatch_credit.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/urma/common/dispatch_profile.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/urma/common/dispatch_schedule.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/urma/common/dispatch_trace.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/urma/common/dispatch_wqe_batch.h"
        "${CMAKE_SOURCE_DIR}/src/include/comm_args.h"
        "${CMAKE_SOURCE_DIR}/src/include/tilexr_udma.h"
        "${CMAKE_SOURCE_DIR}/src/include/tilexr_udma_fullmesh.h"
        "${CMAKE_SOURCE_DIR}/src/include/tilexr_udma_reg.h"
        "${CMAKE_SOURCE_DIR}/src/include/tilexr_udma_types.h"
)

add_library(tilexr-moonep-dispatch SHARED
    ${CMAKE_SOURCE_DIR}/src/moonep/common/moonep_kernel_registration.cpp
    ${CMAKE_SOURCE_DIR}/src/common/tilexr_kernel_binary_loader.cpp
    host/dispatch_layout.cpp
    host/dispatch_host.cpp
    host/dispatch_launch.cpp
    host/tilexr_moonep_dispatch.cpp
    urma/host/dispatch_layout.cpp
    urma/host/dispatch_host.cpp
    urma/host/dispatch_launch.cpp
)
add_dependencies(tilexr-moonep-dispatch
    tilexr_moonep_dispatch_kernel
    tilexr_moonep_dispatch_urma_kernel)
tilexr_attach_kernel_artifacts(tilexr-moonep-dispatch FILES
    ${TILEXR_MOONEP_DISPATCH_KERNEL_BINARY}
    ${TILEXR_MOONEP_DISPATCH_URMA_KERNEL_BINARY}
    TARGETS
    tilexr_moonep_dispatch_kernel
    tilexr_moonep_dispatch_urma_kernel)
target_compile_features(tilexr-moonep-dispatch PRIVATE cxx_std_14)
target_include_directories(tilexr-moonep-dispatch
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../../include
    PRIVATE
        ${CMAKE_SOURCE_DIR}/src/moonep/common
        ${CMAKE_SOURCE_DIR}/src/common
        ${CMAKE_CURRENT_SOURCE_DIR}/common
        ${CMAKE_CURRENT_SOURCE_DIR}/host
        ${CMAKE_CURRENT_SOURCE_DIR}/urma/common
        ${CMAKE_CURRENT_SOURCE_DIR}/urma/host
        ${ASCEND_HOME_PATH}/${ARCH}-linux/pkg_inc
        ${ASCEND_HOME_PATH}/${ARCH}-linux/pkg_inc/runtime
)
target_compile_definitions(tilexr-moonep-dispatch PRIVATE
    TILEXR_MOONEP_DISPATCH_SYSTEM_CYCLES_PER_SECOND=${TILEXR_MOONEP_DISPATCH_SYSTEM_CYCLES_PER_SECOND})
target_link_directories(tilexr-moonep-dispatch PRIVATE
    ${ASCEND_DRIVER_PATH}/lib64/driver
    ${ASCEND_HOME_PATH}/${ARCH}-linux/lib64
    ${ASCEND_HOME_PATH}/${ARCH}-linux/devlib
)
target_link_libraries(tilexr-moonep-dispatch PRIVATE
    tile-comm ascendcl runtime ${CMAKE_DL_LIBS}
)
set_target_properties(tilexr-moonep-dispatch PROPERTIES
    BUILD_WITH_INSTALL_RPATH TRUE
    INSTALL_RPATH "$ORIGIN"
    VERSION 1.0.0
    SOVERSION 1
)

install(TARGETS tilexr-moonep-dispatch LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
