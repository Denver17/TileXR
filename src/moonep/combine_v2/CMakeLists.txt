include(GNUInstallDirs)

find_program(BISHENG_EXECUTABLE bisheng)
if(NOT BISHENG_EXECUTABLE)
    message(FATAL_ERROR "bisheng not found; source the CANN environment before configuring MoonEP Combine V2")
endif()
include(${CMAKE_SOURCE_DIR}/src/moonep/cmake/MoonEpKernel.cmake)

option(TILEXR_MOONEP_COMBINE_V2_ENABLE_PROFILING
    "Enable per-AIV profiling in the MoonEP Combine V2 kernel" OFF)

set(TILEXR_MOONEP_COMBINE_V2_DEFAULT_SOC_TYPE "$ENV{TILEXR_SOC_NAME}")
if(NOT TILEXR_MOONEP_COMBINE_V2_DEFAULT_SOC_TYPE)
    set(TILEXR_MOONEP_COMBINE_V2_DEFAULT_SOC_TYPE "Ascend950")
endif()
set(TILEXR_MOONEP_COMBINE_V2_SOC_TYPE
    "${TILEXR_MOONEP_COMBINE_V2_DEFAULT_SOC_TYPE}"
    CACHE STRING "TileXR MoonEP Combine V2 kernel SOC type")
string(TOLOWER "${TILEXR_MOONEP_COMBINE_V2_SOC_TYPE}"
    TILEXR_MOONEP_COMBINE_V2_SOC_TYPE_LOWER)
if(NOT TILEXR_MOONEP_COMBINE_V2_SOC_TYPE_LOWER MATCHES "ascend950|ascend910a5|910a5")
    message(FATAL_ERROR
        "TileXR MoonEP Combine V2 is A5-only; got TILEXR_MOONEP_COMBINE_V2_SOC_TYPE=${TILEXR_MOONEP_COMBINE_V2_SOC_TYPE}")
endif()

set(TILEXR_MOONEP_COMBINE_V2_KERNEL_SOURCE
    "${CMAKE_CURRENT_SOURCE_DIR}/kernels/tilexr_moonep_combine_v2_kernel.cpp")
set(TILEXR_MOONEP_COMBINE_V2_KERNEL_BINARY
    "${CMAKE_CURRENT_BINARY_DIR}/tilexr_moonep_combine_v2_kernel.o")
set(TILEXR_MOONEP_COMBINE_V2_KERNEL_EMBED_CPP
    "${CMAKE_CURRENT_BINARY_DIR}/tilexr_moonep_combine_v2_kernel_embed.cpp")

set(TILEXR_MOONEP_COMBINE_V2_KERNEL_OPTIONS
    -xcce
    -O2
    -DCATLASS_ARCH=3510
    --cce-auto-sync
    -Wno-deprecated-declarations
    -mllvm -cce-aicore-stack-size=0x8000
    -mllvm -cce-aicore-function-stack-size=0x8000
    -mllvm -cce-aicore-record-overflow=true
    -mllvm -cce-aicore-addr-transform
    -mllvm -cce-aicore-dcci-insert-for-scalar=false
    --cce-aicore-arch=dav-c310-vec
)
if(TILEXR_MOONEP_COMBINE_V2_ENABLE_PROFILING)
    list(APPEND TILEXR_MOONEP_COMBINE_V2_KERNEL_OPTIONS
        -DTILEXR_MOONEP_COMBINE_V2_ENABLE_PROFILING=1)
endif()

set(TILEXR_MOONEP_COMBINE_V2_KERNEL_INCLUDES
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

tilexr_add_moonep_kernel(tilexr_moonep_combine_v2_kernel
    SOURCE "${TILEXR_MOONEP_COMBINE_V2_KERNEL_SOURCE}"
    BINARY "${TILEXR_MOONEP_COMBINE_V2_KERNEL_BINARY}"
    EMBED_CPP "${TILEXR_MOONEP_COMBINE_V2_KERNEL_EMBED_CPP}"
    DATA_SYMBOL TileXRMoonEpCombineV2KernelBinaryData
    SIZE_SYMBOL TileXRMoonEpCombineV2KernelBinarySize
    OPTIONS ${TILEXR_MOONEP_COMBINE_V2_KERNEL_OPTIONS}
    INCLUDES ${TILEXR_MOONEP_COMBINE_V2_KERNEL_INCLUDES}
    DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/kernels/tilexr_moonep_combine_v2_kernel.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/common/combine_v2_profile.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/common/combine_v2_schedule.h"
        "${CMAKE_SOURCE_DIR}/src/moonep/common/moonep_combine_schedule.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/common/combine_v2_wqe_batch.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/common/combine_v2_weight.h"
        "${CMAKE_SOURCE_DIR}/src/include/comm_args.h"
        "${CMAKE_SOURCE_DIR}/src/include/tilexr_udma.h"
        "${CMAKE_SOURCE_DIR}/src/include/tilexr_udma_fullmesh.h"
        "${CMAKE_SOURCE_DIR}/src/include/tilexr_udma_reg.h"
        "${CMAKE_SOURCE_DIR}/src/include/tilexr_udma_types.h"
)
set_source_files_properties("${TILEXR_MOONEP_COMBINE_V2_KERNEL_EMBED_CPP}"
    PROPERTIES GENERATED TRUE)

add_library(tilexr-moonep-combine-v2 SHARED
    ${CMAKE_SOURCE_DIR}/src/moonep/common/moonep_kernel_registration.cpp
    host/combine_v2_layout.cpp
    host/combine_v2_host.cpp
    host/combine_v2_launch.cpp
    host/tilexr_moonep_combine_v2.cpp
    ${TILEXR_MOONEP_COMBINE_V2_KERNEL_EMBED_CPP}
)
add_dependencies(tilexr-moonep-combine-v2 tilexr_moonep_combine_v2_kernel)
target_compile_features(tilexr-moonep-combine-v2 PRIVATE cxx_std_14)
target_include_directories(tilexr-moonep-combine-v2
    PUBLIC ${CMAKE_SOURCE_DIR}/src/include
    PRIVATE
        ${CMAKE_SOURCE_DIR}/src/moonep/common
        ${CMAKE_CURRENT_SOURCE_DIR}/common
        ${CMAKE_CURRENT_SOURCE_DIR}/host
        ${ASCEND_HOME_PATH}/${ARCH}-linux/pkg_inc
        ${ASCEND_HOME_PATH}/${ARCH}-linux/pkg_inc/runtime
)
target_link_directories(tilexr-moonep-combine-v2 PRIVATE
    ${ASCEND_DRIVER_PATH}/lib64/driver
    ${ASCEND_HOME_PATH}/${ARCH}-linux/lib64
)
target_link_libraries(tilexr-moonep-combine-v2 PRIVATE
    tile-comm
    ascendcl
    runtime
)
set_target_properties(tilexr-moonep-combine-v2 PROPERTIES
    INSTALL_RPATH "$ORIGIN"
    VERSION 2.0.0
    SOVERSION 2
)

install(TARGETS tilexr-moonep-combine-v2
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(FILES ${CMAKE_SOURCE_DIR}/src/include/tilexr_moonep_combine_v2.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
