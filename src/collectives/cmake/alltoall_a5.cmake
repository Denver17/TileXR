include(GNUInstallDirs)

find_program(BISHENG_EXECUTABLE bisheng)
if(NOT BISHENG_EXECUTABLE)
    message(FATAL_ERROR "bisheng not found; source the CANN environment before building A5 AllToAll")
endif()
include(${CMAKE_SOURCE_DIR}/src/moonep/cmake/MoonEpKernel.cmake)

set(TILEXR_GROUPED_KERNEL_SOURCE
    "${CMAKE_CURRENT_SOURCE_DIR}/kernels/tilexr_alltoall_a5_kernel.cpp")
set(TILEXR_GROUPED_KERNEL_BINARY
    "${CMAKE_CURRENT_BINARY_DIR}/tilexr_alltoall_a5_kernel.o")
set(TILEXR_GROUPED_KERNEL_128_BINARY
    "${CMAKE_CURRENT_BINARY_DIR}/tilexr_alltoall_a5_kernel_128.o")

set(TILEXR_GROUPED_KERNEL_OPTIONS
    -xcce
    -O2
    -DCATLASS_ARCH=3510
    -mllvm -cce-aicore-stack-size=0x8000
    -mllvm -cce-aicore-function-stack-size=0x8000
    -mllvm -cce-aicore-record-overflow=true
    -mllvm -cce-aicore-addr-transform
    -mllvm -cce-aicore-dcci-insert-for-scalar=false
    --cce-aicore-arch=dav-c310-vec)
if(TILEXR_COLLECTIVES_ENABLE_PROFILING)
    list(APPEND TILEXR_GROUPED_KERNEL_OPTIONS -DTILEXR_ENABLE_PROFILING=1)
endif()

set(TILEXR_GROUPED_KERNEL_INCLUDES
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
    -I${CMAKE_CURRENT_SOURCE_DIR}/kernels)

# Header SDK edits must rebuild both external kernel variants with their Host.
file(GLOB_RECURSE TILEXR_GROUPED_PROFILING_HEADERS CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/src/include/tilexr/profiling/*.h")
list(APPEND TILEXR_GROUPED_PROFILING_HEADERS "${CMAKE_SOURCE_DIR}/src/include/tilexr/profiling.h")

tilexr_add_moonep_kernel(tilexr_alltoall_a5_kernel
    SOURCE "${TILEXR_GROUPED_KERNEL_SOURCE}"
    BINARY "${TILEXR_GROUPED_KERNEL_BINARY}"
    OPTIONS ${TILEXR_GROUPED_KERNEL_OPTIONS} -DTILEXR_GROUPED_KERNEL_VARIANT_64=1
    INCLUDES ${TILEXR_GROUPED_KERNEL_INCLUDES}
    DEPENDS ${TILEXR_GROUPED_PROFILING_HEADERS}
        "${CMAKE_CURRENT_SOURCE_DIR}/kernels/alltoall_a5_common.h"
        "${CMAKE_SOURCE_DIR}/src/include/comm_args.h"
        "${CMAKE_SOURCE_DIR}/src/include/tilexr_udma.h"
        "${CMAKE_SOURCE_DIR}/src/include/tilexr_sdma.h")

tilexr_add_moonep_kernel(tilexr_alltoall_a5_kernel_128
    SOURCE "${TILEXR_GROUPED_KERNEL_SOURCE}"
    BINARY "${TILEXR_GROUPED_KERNEL_128_BINARY}"
    OPTIONS ${TILEXR_GROUPED_KERNEL_OPTIONS} -DTILEXR_GROUPED_KERNEL_VARIANT_128=1
    INCLUDES ${TILEXR_GROUPED_KERNEL_INCLUDES}
    DEPENDS ${TILEXR_GROUPED_PROFILING_HEADERS}
        "${CMAKE_CURRENT_SOURCE_DIR}/kernels/alltoall_a5_common.h"
        "${CMAKE_SOURCE_DIR}/src/include/comm_args.h"
        "${CMAKE_SOURCE_DIR}/src/include/tilexr_udma.h"
        "${CMAKE_SOURCE_DIR}/src/include/tilexr_sdma.h")
