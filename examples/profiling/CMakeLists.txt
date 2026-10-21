cmake_minimum_required(VERSION 3.16)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(TileXRProfiledAdd4 LANGUAGES CXX)
endif()
include(GNUInstallDirs)
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "The standalone Add4 example currently supports Linux only")
endif()
if(NOT TARGET TileXR::profiling)
    find_package(TileXRProfiling 1 CONFIG REQUIRED)
endif()
set(TILEXR_PROFILING_SOC "" CACHE STRING "Explicit actual SoC used for Add4")
if(NOT TILEXR_PROFILING_SOC STREQUAL "Ascend950")
    message(FATAL_ERROR "Add4 currently supports explicit TILEXR_PROFILING_SOC=Ascend950 only")
endif()
set(TILEXR_PROFILING_CANN_ROOT "$ENV{ASCEND_HOME_PATH}" CACHE PATH "Active CANN toolkit root")
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64)$")
    set(add4_arch aarch64)
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64)$")
    set(add4_arch x86_64)
else()
    message(FATAL_ERROR "Unsupported Add4 Host architecture: ${CMAKE_SYSTEM_PROCESSOR}")
endif()
set(add4_cann "${TILEXR_PROFILING_CANN_ROOT}")
find_path(add4_acl_include acl/acl.h HINTS "${add4_cann}/include" "${add4_cann}/${add4_arch}-linux/include")
find_path(add4_runtime_include runtime/kernel.h HINTS "${add4_cann}/include"
    "${add4_cann}/${add4_arch}-linux/pkg_inc" "${add4_cann}/${add4_arch}-linux/pkg_inc/runtime")
find_library(add4_acl_library ascendcl HINTS "${add4_cann}/lib64" "${add4_cann}/${add4_arch}-linux/lib64")
find_library(add4_runtime_library runtime HINTS "${add4_cann}/lib64" "${add4_cann}/${add4_arch}-linux/lib64")
find_program(add4_bisheng bisheng HINTS "${add4_cann}/bin" "${add4_cann}/tools/bisheng_compiler/bin")
find_program(add4_cce_linker NAMES ld.lld cce-ld HINTS "${add4_cann}/bin" "${add4_cann}/tools/bisheng_compiler/bin")
foreach(required add4_acl_include add4_runtime_include add4_acl_library add4_runtime_library add4_bisheng add4_cce_linker)
    if(NOT ${required})
        message(FATAL_ERROR "Missing CANN dependency: ${required}; source the target CANN environment")
    endif()
endforeach()
if(IS_ABSOLUTE "${CMAKE_INSTALL_BINDIR}" OR IS_ABSOLUTE "${CMAKE_INSTALL_LIBDIR}")
    message(FATAL_ERROR "Add4 requires relative bin/lib install directories for relocatable kernel loading")
endif()
file(RELATIVE_PATH add4_kernel_relative "${CMAKE_INSTALL_FULL_BINDIR}"
    "${CMAKE_INSTALL_FULL_LIBDIR}/tilexr/kernels/profiled_add4_kernel.o")
set(add4_kernel_dir "${CMAKE_CURRENT_BINARY_DIR}/${CMAKE_INSTALL_LIBDIR}/tilexr/kernels")
set(add4_kernel "${add4_kernel_dir}/profiled_add4_kernel.o")
set(add4_sdk_include "$<TARGET_PROPERTY:TileXR::profiling,INTERFACE_INCLUDE_DIRECTORIES>")
add_custom_command(OUTPUT "${add4_kernel}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${add4_kernel_dir}"
    COMMAND "${add4_bisheng}" -xcce
        -mllvm -cce-aicore-stack-size=0x8000 -mllvm -cce-aicore-function-stack-size=0x8000
        -mllvm -cce-aicore-record-overflow=true -mllvm -cce-aicore-addr-transform
        -mllvm -cce-aicore-dcci-insert-for-scalar=false
        --cce-aicore-arch=dav-c310-vec --cce-aicore-only -std=gnu++17 -DTILEXR_ENABLE_PROFILING=1
        "-I$<JOIN:${add4_sdk_include},;-I>"
        "-I${add4_cann}/compiler/tikcpp" "-I${add4_cann}/compiler/tikcpp/tikcfw"
        "-I${add4_cann}/compiler/tikcpp/tikcfw/impl" "-I${add4_cann}/compiler/tikcpp/tikcfw/interface"
        "-I${add4_cann}/${add4_arch}-linux/tikcpp" "-I${add4_cann}/${add4_arch}-linux/tikcpp/tikcfw"
        "-I${add4_cann}/${add4_arch}-linux/tikcpp/tikcfw/impl" "-I${add4_cann}/${add4_arch}-linux/tikcpp/tikcfw/interface"
        "-I${add4_cann}/${add4_arch}-linux/pkg_inc" "-I${add4_cann}/${add4_arch}-linux/pkg_inc/runtime"
        "-I${add4_cann}/${add4_arch}-linux/include" "-I${add4_cann}/${add4_arch}-linux/asc/include"
        "-I${add4_cann}/${add4_arch}-linux/ascendc/include"
        -c "${CMAKE_CURRENT_SOURCE_DIR}/profiled_add4_kernel.cpp" -o "${add4_kernel_dir}/profiled_add4_kernel_rel.o"
    COMMAND "${add4_cce_linker}" -m aicorelinux -Ttext=0
        "${add4_kernel_dir}/profiled_add4_kernel_rel.o" --static --allow-multiple-definition -o "${add4_kernel}"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/profiled_add4_kernel.cpp"
        "${add4_sdk_include}/tilexr/profiling.h"
        "${add4_sdk_include}/tilexr/profiling/types.h"
        "${add4_sdk_include}/tilexr/profiling/device.h"
        "${add4_sdk_include}/tilexr/profiling/direct.h"
        "${add4_sdk_include}/tilexr/profiling/detail/device.h"
    COMMAND_EXPAND_LISTS VERBATIM COMMENT "Building external Ascend950 Add4 AICore binary")
add_custom_target(tilexr_profiled_add4_kernel DEPENDS "${add4_kernel}")
add_executable(tilexr_profiled_add4_demo profiled_add4_host.cpp)
add_dependencies(tilexr_profiled_add4_demo tilexr_profiled_add4_kernel)
target_link_libraries(tilexr_profiled_add4_demo PRIVATE TileXR::profiling "${add4_acl_library}" "${add4_runtime_library}")
target_include_directories(tilexr_profiled_add4_demo PRIVATE "${add4_acl_include}" "${add4_runtime_include}"
    "${add4_cann}/${add4_arch}-linux/pkg_inc")
target_compile_definitions(tilexr_profiled_add4_demo PRIVATE "TILEXR_ADD4_KERNEL_RELATIVE_PATH=\"${add4_kernel_relative}\"")
set_target_properties(tilexr_profiled_add4_demo PROPERTIES CXX_STANDARD 14 CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS NO RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/${CMAKE_INSTALL_BINDIR}")
install(TARGETS tilexr_profiled_add4_demo RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
install(FILES "${add4_kernel}" DESTINATION "${CMAKE_INSTALL_LIBDIR}/tilexr/kernels")
# No NPU test is registered by default. Run the explicit runner on a chosen card.
