include(GNUInstallDirs)

find_program(BISHENG_EXECUTABLE bisheng)
if(NOT BISHENG_EXECUTABLE)
    message(FATAL_ERROR "bisheng not found; source the CANN environment before configuring Planner V2")
endif()
include(${CMAKE_SOURCE_DIR}/src/moonep/cmake/MoonEpKernel.cmake)
include(${CMAKE_SOURCE_DIR}/src/cmake/TileXRKernelArtifact.cmake)

set(TILEXR_MOONEP_PLANNER_V2_DEFAULT_SOC_TYPE "$ENV{TILEXR_SOC_NAME}")
if(NOT TILEXR_MOONEP_PLANNER_V2_DEFAULT_SOC_TYPE)
    set(TILEXR_MOONEP_PLANNER_V2_DEFAULT_SOC_TYPE "Ascend950")
endif()
set(TILEXR_MOONEP_PLANNER_V2_SOC_TYPE "${TILEXR_MOONEP_PLANNER_V2_DEFAULT_SOC_TYPE}"
    CACHE STRING "TileXR optimized Planner V2 kernel SOC type")
string(TOLOWER "${TILEXR_MOONEP_PLANNER_V2_SOC_TYPE}" TILEXR_MOONEP_PLANNER_V2_SOC_TYPE_LOWER)

if(NOT TILEXR_MOONEP_PLANNER_V2_SOC_TYPE_LOWER MATCHES "ascend950|ascend910a5|910a5")
    message(FATAL_ERROR
        "TileXR Planner V2 is A5-only; got TILEXR_MOONEP_PLANNER_V2_SOC_TYPE=${TILEXR_MOONEP_PLANNER_V2_SOC_TYPE}")
endif()

set(TILEXR_MOONEP_PLANNER_V2_KERNEL_SOURCE
    "${CMAKE_CURRENT_SOURCE_DIR}/kernels/tilexr_moonep_planner_kernel.cpp")
set(TILEXR_MOONEP_PLANNER_V2_KERNEL_BINARY
    "${CMAKE_CURRENT_BINARY_DIR}/tilexr_moonep_planner_v2_kernel.o")

set(TILEXR_MOONEP_PLANNER_V2_KERNEL_OPTIONS
    -xcce
    -Xhost-start -ftrapv -Xhost-end
    -mllvm -cce-aicore-stack-size=0x8000
    -mllvm -cce-aicore-function-stack-size=0x8000
    -mllvm -cce-aicore-record-overflow=true
    -mllvm -cce-aicore-addr-transform
    -mllvm -cce-aicore-dcci-insert-for-scalar=false
    --cce-aicore-arch=dav-c310-vec
)

set(TILEXR_MOONEP_PLANNER_V2_KERNEL_INCLUDES
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

tilexr_add_moonep_kernel(tilexr_moonep_planner_v2_kernel
    SOURCE "${TILEXR_MOONEP_PLANNER_V2_KERNEL_SOURCE}"
    BINARY "${TILEXR_MOONEP_PLANNER_V2_KERNEL_BINARY}"
    OPTIONS ${TILEXR_MOONEP_PLANNER_V2_KERNEL_OPTIONS}
    INCLUDES ${TILEXR_MOONEP_PLANNER_V2_KERNEL_INCLUDES}
    DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/common/ep_plan_algorithm.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/common/ep_plan_algorithm_impl.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/common/ep_plan_types.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/common/ep_plan_peer_mailbox.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/common/planner_common.h"
        "${CMAKE_SOURCE_DIR}/src/include/comm_args.h"
        "${CMAKE_SOURCE_DIR}/src/include/tilexr_ep_plan.h"
        "${CMAKE_SOURCE_DIR}/src/include/tilexr_sync.h"
)

add_library(tilexr-moonep-planner SHARED
    ${CMAKE_SOURCE_DIR}/src/moonep/common/moonep_kernel_registration.cpp
    ${CMAKE_SOURCE_DIR}/src/common/tilexr_kernel_binary_loader.cpp
    common/ep_plan_layout.cpp
    common/ep_plan_downstream.cpp
    host/ep_plan_host.cpp
    host/planner_layout.cpp
    host/planner_host.cpp
    host/planner_launch.cpp
    host/tilexr_moonep_planner.cpp
)
add_dependencies(tilexr-moonep-planner tilexr_moonep_planner_v2_kernel)
tilexr_attach_kernel_artifacts(tilexr-moonep-planner FILES
    ${TILEXR_MOONEP_PLANNER_V2_KERNEL_BINARY}
    TARGETS tilexr_moonep_planner_v2_kernel)
target_compile_features(tilexr-moonep-planner PRIVATE cxx_std_14)

target_include_directories(tilexr-moonep-planner
    PUBLIC ${CMAKE_SOURCE_DIR}/src/include
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/common
        ${CMAKE_CURRENT_SOURCE_DIR}/host
        ${CMAKE_SOURCE_DIR}/src/moonep/common
        ${CMAKE_SOURCE_DIR}/src/common
        ${ASCEND_HOME_PATH}/${ARCH}-linux/pkg_inc
        ${ASCEND_HOME_PATH}/${ARCH}-linux/pkg_inc/runtime
)
target_link_directories(tilexr-moonep-planner PRIVATE
    ${ASCEND_DRIVER_PATH}/lib64/driver
    ${ASCEND_HOME_PATH}/${ARCH}-linux/lib64
)
target_link_libraries(tilexr-moonep-planner PRIVATE
    tile-comm
    ascendcl
    runtime
    ${CMAKE_DL_LIBS}
)
set_target_properties(tilexr-moonep-planner PROPERTIES
    INSTALL_RPATH "$ORIGIN"
    VERSION 2.0.0
    SOVERSION 2
)

install(TARGETS tilexr-moonep-planner LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(FILES
    ${CMAKE_SOURCE_DIR}/src/include/tilexr_moonep_planner.h
    ${CMAKE_SOURCE_DIR}/src/include/tilexr_ep_plan.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
