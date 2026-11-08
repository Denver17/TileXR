# 功能：构建并注册 RDMA Host/CPU 测试，覆盖 transport、WRITE/Wait、UB context、profiling 和 benchmark 布局。
# 调用关系：独立 CMake 工程直接编译 runtime 源码与测试 stub；按 verbs、扩展 ABI 和 ACL 头文件可用性启用 fake DSO 测试。
# 边界：CTest 使用 CPU 模型或 fake driver，不启动 NPU 数据传输。
#
# Purpose: Build/register RDMA Host/CPU tests for transport, WRITE/Wait, UB contexts, profiling and benchmark layouts.
# Calls: This standalone CMake project compiles runtime sources and stubs; verbs, extension ABI and ACL headers gate fake-DSO tests.
# Scope: CTest uses CPU models or fake drivers and does not launch NPU transfers.
cmake_minimum_required(VERSION 3.16)
project(TileXR_Rdma_Tests LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
include(CTest)
find_package(Threads REQUIRED)
include(CheckIncludeFileCXX)
check_include_file_cxx("infiniband/verbs.h" HAVE_VERBS)
find_path(TILEXR_IBV_EXTEND_INCLUDE ibv_extend.h HINTS "$ENV{ASCEND_DRIVER_PATH}/include" /usr/local/Ascend/driver/include)
if(HAVE_VERBS AND TILEXR_IBV_EXTEND_INCLUDE)
    include(CMakePushCheckState)
    include(CheckCXXSourceCompiles)
    cmake_push_check_state(RESET)
    set(CMAKE_REQUIRED_INCLUDES "${TILEXR_IBV_EXTEND_INCLUDE}")
    check_cxx_source_compiles("
#include <ibv_extend.h>
#if IBV_EXTEND_VERSION_MAJOR != 4 || IBV_EXTEND_VERSION_MINOR < 2
#error unsupported extension ABI
#endif
int main() { queue_info q{}; (void)q; return 0; }
" HAVE_IBV_EXTEND)
    cmake_pop_check_state()
endif()
set(TILEXR_ROOT "${CMAKE_CURRENT_LIST_DIR}/../..")
add_library(rdma_core STATIC
    ${TILEXR_ROOT}/src/comm/rdma/tilexr_rdma_config.cpp
    ${TILEXR_ROOT}/src/comm/rdma/tilexr_rdma_transport.cpp)
target_include_directories(rdma_core PUBLIC ${TILEXR_ROOT}/src/comm ${TILEXR_ROOT}/src/include)
target_compile_definitions(rdma_core PRIVATE TILEXR_LOG_DISABLE_SPDLOG=1)
target_compile_options(rdma_core PRIVATE -Wall -Wextra -Werror)
add_executable(test_rdma_transport test_rdma_transport.cpp)
target_link_libraries(test_rdma_transport PRIVATE rdma_core Threads::Threads)
target_compile_options(test_rdma_transport PRIVATE -Wall -Wextra -Werror -UNDEBUG)
add_test(NAME rdma_transport COMMAND test_rdma_transport)
set_tests_properties(rdma_transport PROPERTIES TIMEOUT 30)
add_executable(test_rdma_header test_rdma_header.c)
target_include_directories(test_rdma_header PRIVATE ${TILEXR_ROOT}/src/include)
add_test(NAME rdma_c_header COMMAND test_rdma_header)
add_library(rdma_api_compile OBJECT ${TILEXR_ROOT}/src/comm/rdma/tilexr_rdma_api.cpp)
target_link_libraries(rdma_api_compile PRIVATE rdma_core)
target_compile_definitions(rdma_api_compile PRIVATE TILEXR_LOG_DISABLE_SPDLOG=1)
foreach(enabled 0 1)
    if(enabled AND NOT HAVE_VERBS)
        continue()
    endif()
    add_executable(test_rdma_verbs_${enabled} test_rdma_verbs.cpp
        ${TILEXR_ROOT}/src/comm/rdma/tilexr_rdma_verbs.cpp
        ${TILEXR_ROOT}/src/comm/rdma/tilexr_rdma_nda.cpp)
    target_link_libraries(test_rdma_verbs_${enabled} PRIVATE rdma_core ${CMAKE_DL_LIBS})
    target_compile_definitions(test_rdma_verbs_${enabled} PRIVATE
        TILEXR_HAVE_IBVERBS=${enabled} TILEXR_LOG_DISABLE_SPDLOG=1)
    target_compile_options(test_rdma_verbs_${enabled} PRIVATE -Wall -Wextra -Werror -UNDEBUG)
    add_test(NAME rdma_verbs_${enabled} COMMAND test_rdma_verbs_${enabled})
endforeach()

if(HAVE_VERBS)
    add_library(fake_ibverbs SHARED fake_verbs.cpp)
    set_target_properties(fake_ibverbs PROPERTIES OUTPUT_NAME ibverbs SOVERSION 1
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/fake-driver")
    target_compile_options(fake_ibverbs PRIVATE -Wall -Wextra -Werror -UNDEBUG)
    add_library(fake_ascend_hal SHARED fake_hal.cpp)
    target_include_directories(fake_ascend_hal PRIVATE ${TILEXR_ROOT}/src/comm)
    target_compile_options(fake_ascend_hal PRIVATE -Wall -Wextra -Werror -UNDEBUG)
    target_link_libraries(fake_ascend_hal PRIVATE fake_ibverbs)
    set_target_properties(fake_ascend_hal PROPERTIES OUTPUT_NAME ascend_hal
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/fake-driver")
    add_executable(test_rdma_verbs_driver test_rdma_verbs_driver.cpp
        ${TILEXR_ROOT}/src/comm/rdma/tilexr_rdma_verbs.cpp
        ${TILEXR_ROOT}/src/comm/rdma/tilexr_rdma_nda.cpp)
    target_link_libraries(test_rdma_verbs_driver PRIVATE rdma_core ${CMAKE_DL_LIBS})
    target_compile_definitions(test_rdma_verbs_driver PRIVATE TILEXR_HAVE_IBVERBS=1 TILEXR_LOG_DISABLE_SPDLOG=1)
    target_compile_options(test_rdma_verbs_driver PRIVATE -Wall -Wextra -Werror -UNDEBUG)
    add_dependencies(test_rdma_verbs_driver fake_ascend_hal)
    add_test(NAME rdma_verbs_driver COMMAND test_rdma_verbs_driver)
    set_tests_properties(rdma_verbs_driver PROPERTIES
        ENVIRONMENT "LD_LIBRARY_PATH=$<TARGET_FILE_DIR:fake_ibverbs>" TIMEOUT 10)
endif()

add_executable(test_rdma_wire test_rdma_wire.cpp)
target_include_directories(test_rdma_wire PRIVATE ${TILEXR_ROOT}/src/include)
target_compile_options(test_rdma_wire PRIVATE -Wall -Wextra -Werror -UNDEBUG)
add_test(NAME rdma_ubnic_wire COMMAND test_rdma_wire)
add_test(NAME rdma_ubnic_wire_no_wrap COMMAND test_rdma_wire --no-wrap)

add_executable(test_rdma_pipeline test_rdma_pipeline.cpp)
target_compile_definitions(test_rdma_pipeline PRIVATE TILEXR_RDMA_DEBUG=1)
target_include_directories(test_rdma_pipeline PRIVATE ${TILEXR_ROOT}/src/include)
target_compile_options(test_rdma_pipeline PRIVATE -Wall -Wextra -Werror -UNDEBUG)
add_test(NAME rdma_pipeline_cpu COMMAND test_rdma_pipeline)

add_executable(test_rdma_write_options test_rdma_write_options.cpp)
target_compile_definitions(test_rdma_write_options PRIVATE TILEXR_RDMA_DEBUG=1)
target_include_directories(test_rdma_write_options PRIVATE
    ${TILEXR_ROOT}/src/include ${CMAKE_CURRENT_LIST_DIR}/write_stubs)
target_compile_options(test_rdma_write_options PRIVATE -Wall -Wextra -Werror -UNDEBUG)
add_test(NAME rdma_write_api_cpu COMMAND test_rdma_write_options)

foreach(rdma_debug 0 1)
    add_executable(test_rdma_context_${rdma_debug} test_rdma_context.cpp)
    target_include_directories(test_rdma_context_${rdma_debug} PRIVATE
        ${TILEXR_ROOT}/src/include ${CMAKE_CURRENT_LIST_DIR}/write_stubs)
    target_compile_definitions(test_rdma_context_${rdma_debug} PRIVATE TILEXR_RDMA_DEBUG=${rdma_debug})
    target_compile_options(test_rdma_context_${rdma_debug} PRIVATE -Wall -Wextra -Werror)
    add_test(NAME rdma_context_${rdma_debug}_cpu COMMAND test_rdma_context_${rdma_debug})
endforeach()
add_executable(test_rdma_context_profile test_rdma_context.cpp)
target_include_directories(test_rdma_context_profile PRIVATE
    ${TILEXR_ROOT}/src/include ${CMAKE_CURRENT_LIST_DIR}/write_stubs)
target_compile_definitions(test_rdma_context_profile PRIVATE
    TILEXR_RDMA_DEBUG=1 TILEXR_ENABLE_PROFILING=1 __CCE__=1 __CCE_IS_AICORE__=1)
target_compile_options(test_rdma_context_profile PRIVATE -Wall -Wextra -Werror)
add_test(NAME rdma_context_profile_cpu COMMAND test_rdma_context_profile)

add_executable(test_rdma_sge_layout test_rdma_sge_layout.cpp)
target_compile_options(test_rdma_sge_layout PRIVATE -Wall -Wextra -Werror -UNDEBUG)
add_test(NAME rdma_sge_layout_cpu COMMAND test_rdma_sge_layout)

add_executable(test_rdma_multiqp_summary test_rdma_multiqp_summary.cpp)
target_compile_options(test_rdma_multiqp_summary PRIVATE -Wall -Wextra -Werror -UNDEBUG)
add_test(NAME rdma_multiqp_summary_cpu COMMAND test_rdma_multiqp_summary)

add_executable(test_rdma_multiqp_layout test_rdma_multiqp_layout.cpp)
target_compile_options(test_rdma_multiqp_layout PRIVATE -Wall -Wextra -Werror -UNDEBUG)
add_test(NAME rdma_multiqp_layout_cpu COMMAND test_rdma_multiqp_layout)

add_executable(test_rdma_multiqp_single_layout test_rdma_multiqp_layout.cpp)
target_compile_definitions(test_rdma_multiqp_single_layout PRIVATE TILEXR_RDMA_SINGLE_WQE_CALLS=1)
target_compile_options(test_rdma_multiqp_single_layout PRIVATE -Wall -Wextra -Werror -UNDEBUG)
add_test(NAME rdma_multiqp_single_layout_cpu COMMAND test_rdma_multiqp_single_layout)

find_package(Python3 COMPONENTS Interpreter REQUIRED)
add_executable(test_rdma_socket_control test_rdma_socket_control.cpp
    ${TILEXR_ROOT}/src/comm/tools/socket/tilexr_sock_exchange.cpp)
target_link_libraries(test_rdma_socket_control PRIVATE rdma_core Threads::Threads)
target_include_directories(test_rdma_socket_control PRIVATE ${CMAKE_CURRENT_LIST_DIR}/socket_stubs)
target_compile_definitions(test_rdma_socket_control PRIVATE TILEXR_LOG_DISABLE_SPDLOG=1)
target_compile_options(test_rdma_socket_control PRIVATE -Wall -Wextra -Werror -UNDEBUG)
add_test(NAME rdma_socket_control_cpu COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_LIST_DIR}/run_socket_control.py $<TARGET_FILE:test_rdma_socket_control>)
set_tests_properties(rdma_socket_control_cpu PROPERTIES TIMEOUT 30)
add_test(NAME rdma_compare_plan_cpu COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_LIST_DIR}/test_compare_plan.py)

# The NDA success path runs against fake DSOs, without launching an NPU.
if(HAVE_IBV_EXTEND)
    find_path(TILEXR_NDA_ACL_INCLUDE acl/acl.h HINTS
        "$ENV{ASCEND_HOME_PATH}/include" "$ENV{ASCEND_HOME_PATH}/aarch64-linux/include")
    if(TILEXR_NDA_ACL_INCLUDE)
        add_library(fake_nda_acl SHARED fake_nda_acl.cpp)
        target_include_directories(fake_nda_acl PRIVATE "${TILEXR_NDA_ACL_INCLUDE}")
        target_compile_options(fake_nda_acl PRIVATE -Wall -Wextra -Werror -UNDEBUG)
        set_target_properties(fake_nda_acl PROPERTIES OUTPUT_NAME ascendcl
            LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/fake-driver")
        add_library(fake_ibv_extend SHARED fake_ibv_extend.cpp)
        target_link_libraries(fake_ibv_extend PRIVATE fake_ibverbs)
        target_include_directories(fake_ibv_extend PRIVATE "${TILEXR_IBV_EXTEND_INCLUDE}")
        target_compile_options(fake_ibv_extend PRIVATE -UNDEBUG)
        set_target_properties(fake_ibv_extend PROPERTIES OUTPUT_NAME ibv_extend
            LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/fake-driver")
        add_executable(test_rdma_nda test_rdma_nda.cpp
            ${TILEXR_ROOT}/src/comm/rdma/tilexr_rdma_nda.cpp)
        target_include_directories(test_rdma_nda PRIVATE "${TILEXR_IBV_EXTEND_INCLUDE}" "${TILEXR_NDA_ACL_INCLUDE}")
        target_compile_definitions(test_rdma_nda PRIVATE
            TILEXR_HAVE_IBVERBS=1 TILEXR_HAVE_IBV_EXTEND=1 TILEXR_LOG_DISABLE_SPDLOG=1)
        target_compile_options(test_rdma_nda PRIVATE -Wall -Wextra -Werror -UNDEBUG)
        target_link_libraries(test_rdma_nda PRIVATE rdma_core ${CMAKE_DL_LIBS})
        add_dependencies(test_rdma_nda fake_ibv_extend fake_nda_acl fake_ascend_hal)
        add_test(NAME rdma_nda_resources COMMAND test_rdma_nda)
        set_tests_properties(rdma_nda_resources PROPERTIES
            ENVIRONMENT "LD_LIBRARY_PATH=$<TARGET_FILE_DIR:fake_ibverbs>" TIMEOUT 15)
        add_executable(test_rdma_direct_provider test_rdma_direct_provider.cpp
            ${TILEXR_ROOT}/src/comm/rdma/tilexr_rdma_verbs.cpp
            ${TILEXR_ROOT}/src/comm/rdma/tilexr_rdma_nda.cpp)
        target_include_directories(test_rdma_direct_provider PRIVATE
            "${TILEXR_IBV_EXTEND_INCLUDE}" "${TILEXR_NDA_ACL_INCLUDE}")
        target_compile_definitions(test_rdma_direct_provider PRIVATE
            TILEXR_HAVE_IBVERBS=1 TILEXR_HAVE_IBV_EXTEND=1 TILEXR_LOG_DISABLE_SPDLOG=1)
        target_compile_options(test_rdma_direct_provider PRIVATE -Wall -Wextra -Werror -UNDEBUG)
        target_link_libraries(test_rdma_direct_provider PRIVATE rdma_core ${CMAKE_DL_LIBS})
        add_dependencies(test_rdma_direct_provider fake_ibv_extend fake_nda_acl fake_ascend_hal)
        add_test(NAME rdma_direct_provider COMMAND test_rdma_direct_provider)
        set_tests_properties(rdma_direct_provider PROPERTIES
            ENVIRONMENT "LD_LIBRARY_PATH=$<TARGET_FILE_DIR:fake_ibverbs>" TIMEOUT 15)
    endif()
endif()
