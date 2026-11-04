cmake_minimum_required(VERSION 3.16)
project(TileXRMatchedWrite LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 14)
find_package(Threads REQUIRED)
set(TILEXR_ROOT "" CACHE PATH "TileXR checkout")
set(TILEXR_INSTALL "" CACHE PATH "Production install")
set(CANN "$ENV{ASCEND_HOME_PATH}")
string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" HOST_ARCH)
if(HOST_ARCH MATCHES "^(amd64|x86_64)$")
    set(HOST_ARCH x86_64)
elseif(HOST_ARCH MATCHES "^(arm64|aarch64)$")
    set(HOST_ARCH aarch64)
else()
    message(FATAL_ERROR "Unsupported CANN host architecture: ${CMAKE_SYSTEM_PROCESSOR}")
endif()
set(PLATFORM "${CANN}/${HOST_ARCH}-linux")
find_program(BISHENG bisheng HINTS "${CANN}/compiler/ccec_compiler/bin" "${CANN}/tools/bisheng_compiler/bin" REQUIRED)
get_filename_component(COMPILER_BIN "${BISHENG}" DIRECTORY)
find_program(LINKER ld.lld HINTS "${COMPILER_BIN}" REQUIRED)
set(INCLUDES "${CMAKE_CURRENT_SOURCE_DIR}" "${TILEXR_ROOT}/src/include"
    "${CANN}/compiler/tikcpp" "${CANN}/compiler/tikcpp/tikcfw"
    "${CANN}/compiler/tikcpp/tikcfw/impl" "${CANN}/compiler/tikcpp/tikcfw/interface"
    "${PLATFORM}/tikcpp" "${PLATFORM}/tikcpp/tikcfw"
    "${PLATFORM}/tikcpp/tikcfw/impl" "${PLATFORM}/tikcpp/tikcfw/interface"
    "${PLATFORM}/asc/include" "${PLATFORM}/ascendc/include"
    "${PLATFORM}/include" "${PLATFORM}/pkg_inc" "${PLATFORM}/pkg_inc/runtime"
    "${CANN}/include" "${CANN}/pkg_inc" "${CANN}/pkg_inc/runtime"
    "/usr/local/Ascend/driver/kernel/inc")
foreach(INC IN LISTS INCLUDES)
    list(APPEND INCLUDE_FLAGS "-I${INC}")
endforeach()
find_library(TILE_COMM tile-comm HINTS "${TILEXR_INSTALL}/lib" "${TILEXR_INSTALL}/lib64" REQUIRED)
find_library(ACL ascendcl HINTS "${CANN}/lib64" "${PLATFORM}/lib64" REQUIRED)
find_library(RUNTIME runtime HINTS "${CANN}/lib64" "${PLATFORM}/lib64" REQUIRED)

function(add_rdma_benchmark name)
    set(kernel "${CMAKE_CURRENT_BINARY_DIR}/${name}_kernel")
    add_custom_command(OUTPUT "${kernel}.o"
        COMMAND "${BISHENG}" -O2 -xcce -std=gnu++17 --cce-aicore-only --cce-aicore-arch=dav-c310-vec
            -mllvm -cce-aicore-stack-size=0x8000 -mllvm -cce-aicore-function-stack-size=0x8000
            -mllvm -cce-aicore-record-overflow=true -mllvm -cce-aicore-addr-transform
            -mllvm -cce-aicore-dcci-insert-for-scalar=false ${INCLUDE_FLAGS}
            -c "${CMAKE_CURRENT_SOURCE_DIR}/${name}_kernel.cpp" -o "${kernel}_rel.o"
        COMMAND "${LINKER}" -m aicorelinux -Ttext=0 "${kernel}_rel.o"
            --static --allow-multiple-definition -o "${kernel}.o"
        DEPENDS "${name}_kernel.cpp" bench_abi.h case_layout.h ${ARGN}
            "${TILEXR_ROOT}/src/include/tilexr_rdma_device.h" "${TILEXR_ROOT}/src/include/tilexr_rdma_wire.h"
            "${TILEXR_ROOT}/src/include/tilexr_rdma_device_types.h"
            "${TILEXR_ROOT}/src/include/tilexr_rdma_pipeline.h"
        VERBATIM)
    add_custom_target(${name}_kernel ALL DEPENDS "${kernel}.o")
    add_executable(${name} "${name}.cpp" "${TILEXR_ROOT}/src/comm/tools/socket/tilexr_sock_exchange.cpp")
    target_include_directories(${name} PRIVATE ${INCLUDES} "${TILEXR_ROOT}/src/comm" "${TILEXR_ROOT}/3rdparty")
    target_compile_options(${name} PRIVATE -O2 -Wall -Wextra -fvisibility=hidden)
    target_link_libraries(${name} PRIVATE "${TILE_COMM}" "${ACL}" "${RUNTIME}" Threads::Threads)
    add_dependencies(${name} ${name}_kernel)
endfunction()

add_rdma_benchmark(bench)
# One AIV and scratch buffer own each independent QP in the shared launch.
add_rdma_benchmark(multiqp "${TILEXR_ROOT}/src/include/tilexr_rdma_write.h"
    "${TILEXR_ROOT}/src/include/tilexr_rdma_write_options.h")
