include(GNUInstallDirs)

find_program(BISHENG_EXECUTABLE bisheng)
if(NOT BISHENG_EXECUTABLE)
    message(FATAL_ERROR "bisheng not found; source the CANN environment before configuring MoonEP reduce-grad")
endif()
include(${CMAKE_SOURCE_DIR}/src/moonep/cmake/MoonEpKernel.cmake)
include(${CMAKE_SOURCE_DIR}/src/cmake/TileXRKernelArtifact.cmake)

set(TILEXR_MOONEP_REDUCE_GRAD_DEFAULT_SOC_TYPE "$ENV{TILEXR_SOC_NAME}")
if(NOT TILEXR_MOONEP_REDUCE_GRAD_DEFAULT_SOC_TYPE)
    set(TILEXR_MOONEP_REDUCE_GRAD_DEFAULT_SOC_TYPE "Ascend950")
endif()
set(TILEXR_MOONEP_REDUCE_GRAD_SOC_TYPE "${TILEXR_MOONEP_REDUCE_GRAD_DEFAULT_SOC_TYPE}"
    CACHE STRING "TileXR MoonEP reduce-grad kernel SOC type")
string(TOLOWER "${TILEXR_MOONEP_REDUCE_GRAD_SOC_TYPE}"
    TILEXR_MOONEP_REDUCE_GRAD_SOC_TYPE_LOWER)
if(NOT TILEXR_MOONEP_REDUCE_GRAD_SOC_TYPE_LOWER MATCHES "ascend950|ascend910a5|910a5")
    message(FATAL_ERROR "TileXR MoonEP reduce-grad is A5-only")
endif()

set(TILEXR_MOONEP_REDUCE_GRAD_KERNEL_SOURCE
    "${CMAKE_CURRENT_SOURCE_DIR}/kernels/tilexr_moonep_reduce_grad_kernel.cpp")
set(TILEXR_MOONEP_REDUCE_GRAD_KERNEL_BINARY
    "${CMAKE_CURRENT_BINARY_DIR}/tilexr_moonep_reduce_grad_kernel.o")
set(TILEXR_MOONEP_REDUCE_GRAD_KERNEL_OPTIONS
    -xcce
    -mllvm -cce-aicore-stack-size=0x8000
    -mllvm -cce-aicore-function-stack-size=0x8000
    -mllvm -cce-aicore-record-overflow=true
    -mllvm -cce-aicore-addr-transform
    -mllvm -cce-aicore-dcci-insert-for-scalar=false
    --cce-aicore-arch=dav-c310-vec
)
set(TILEXR_MOONEP_REDUCE_GRAD_KERNEL_INCLUDES
    -I${ASCEND_HOME_PATH}/compiler/tikcpp
    -I${ASCEND_HOME_PATH}/compiler/tikcpp/tikcfw
    -I${ASCEND_HOME_PATH}/compiler/tikcpp/tikcfw/impl
    -I${ASCEND_HOME_PATH}/compiler/tikcpp/tikcfw/interface
    -I${ASCEND_HOME_PATH}/${ARCH}-linux/asc/include
    -I${ASCEND_HOME_PATH}/${ARCH}-linux/ascendc/include
    -I${ASCEND_HOME_PATH}/${ARCH}-linux/include
    -I${ASCEND_HOME_PATH}/${ARCH}-linux/pkg_inc
    -I${ASCEND_HOME_PATH}/${ARCH}-linux/pkg_inc/runtime
    -I${ASCEND_DRIVER_PATH}/kernel/inc
    -I${CMAKE_SOURCE_DIR}/src/include
    -I${CMAKE_SOURCE_DIR}/src/moonep/common
    -I${CMAKE_CURRENT_SOURCE_DIR}/common
)

tilexr_add_moonep_kernel(tilexr_moonep_reduce_grad_kernel
    SOURCE "${TILEXR_MOONEP_REDUCE_GRAD_KERNEL_SOURCE}"
    BINARY "${TILEXR_MOONEP_REDUCE_GRAD_KERNEL_BINARY}"
    OPTIONS ${TILEXR_MOONEP_REDUCE_GRAD_KERNEL_OPTIONS}
    INCLUDES ${TILEXR_MOONEP_REDUCE_GRAD_KERNEL_INCLUDES}
    DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/common/reduce_grad_common.h"
        "${CMAKE_SOURCE_DIR}/src/include/comm_args.h"
        "${CMAKE_SOURCE_DIR}/src/include/tilexr_sync.h"
        "${CMAKE_SOURCE_DIR}/src/include/tilexr_udma.h"
        "${CMAKE_SOURCE_DIR}/src/include/tilexr_udma_reg.h"
        "${CMAKE_SOURCE_DIR}/src/include/tilexr_udma_types.h"
)

add_library(tilexr-moonep-reduce-grad SHARED
    ${CMAKE_SOURCE_DIR}/src/common/tilexr_kernel_binary_loader.cpp
    host/reduce_grad_layout.cpp
    host/reduce_grad_host.cpp
    host/reduce_grad_launch.cpp
)
add_dependencies(tilexr-moonep-reduce-grad tilexr_moonep_reduce_grad_kernel)
tilexr_attach_kernel_artifacts(tilexr-moonep-reduce-grad FILES
    ${TILEXR_MOONEP_REDUCE_GRAD_KERNEL_BINARY}
    TARGETS tilexr_moonep_reduce_grad_kernel)
target_compile_features(tilexr-moonep-reduce-grad PRIVATE cxx_std_14)
target_include_directories(tilexr-moonep-reduce-grad
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../../include
    PRIVATE
        ${CMAKE_SOURCE_DIR}/src/moonep/common
        ${CMAKE_SOURCE_DIR}/src/common
        ${CMAKE_CURRENT_SOURCE_DIR}/common
        ${CMAKE_CURRENT_SOURCE_DIR}/host
        ${ASCEND_HOME_PATH}/${ARCH}-linux/pkg_inc
        ${ASCEND_HOME_PATH}/${ARCH}-linux/pkg_inc/runtime
)
target_link_directories(tilexr-moonep-reduce-grad PRIVATE
    ${ASCEND_DRIVER_PATH}/lib64/driver
    ${ASCEND_HOME_PATH}/${ARCH}-linux/lib64
)
target_link_libraries(tilexr-moonep-reduce-grad PRIVATE
    tile-comm ascendcl runtime ${CMAKE_DL_LIBS})
set_target_properties(tilexr-moonep-reduce-grad PROPERTIES
    BUILD_WITH_INSTALL_RPATH TRUE
    INSTALL_RPATH "$ORIGIN"
    VERSION 1.0.0
    SOVERSION 1
)

install(TARGETS tilexr-moonep-reduce-grad LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
