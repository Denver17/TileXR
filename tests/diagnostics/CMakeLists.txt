# 功能：构建 diagnostics CPU 和 clock 集成测试，生成两 marker 的 RDMA 测试 binary，注册 CTest 并安装工具。
# 调用关系：顶层构建加入本目录；CPU 测试链接 diagnostics，集成程序还链接 CANN runtime。
cmake_minimum_required(VERSION 3.16)
project(TileXR_Diagnostics_Tests)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
include(GNUInstallDirs)
find_package(Threads REQUIRED)
find_package(Python3 COMPONENTS Interpreter QUIET)

set(TILEXR_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../..")

add_executable(test_clock_drift unit/test_clock_drift.cpp)
target_include_directories(test_clock_drift PRIVATE ${TILEXR_ROOT}/src/diagnostics/host)
target_link_libraries(test_clock_drift PRIVATE tilexr-diagnostics)
target_compile_options(test_clock_drift PRIVATE -Wall -Wextra -Werror)
add_test(NAME clock_drift_cpu COMMAND test_clock_drift)

add_executable(test_clock_rdma_queue unit/test_clock_rdma_queue.cpp)
target_include_directories(test_clock_rdma_queue PRIVATE
    ${TILEXR_ROOT}/src/include
    ${TILEXR_ROOT}/src/diagnostics/kernels)
target_compile_options(test_clock_rdma_queue PRIVATE -Wall -Wextra -Werror)

add_executable(test_tilexr_straggler_header_compile
    unit/test_tilexr_straggler_header_compile.cpp)
target_link_libraries(test_tilexr_straggler_header_compile PRIVATE tilexr-diagnostics)

add_executable(test_tilexr_diagnostics_kernel_ownership
    unit/test_tilexr_diagnostics_kernel_ownership.cpp)
target_compile_definitions(test_tilexr_diagnostics_kernel_ownership PRIVATE
    TILEXR_SOURCE_ROOT="${TILEXR_ROOT}")

add_executable(test_straggler_analysis
    unit/test_straggler_analysis.cpp)
target_include_directories(test_straggler_analysis PRIVATE
    ${TILEXR_ROOT}/src/diagnostics/host)
target_link_libraries(test_straggler_analysis PRIVATE tilexr-diagnostics)

add_executable(test_straggler_session
    unit/test_straggler_session.cpp)
target_include_directories(test_straggler_session PRIVATE
    ${TILEXR_ROOT}/src/diagnostics/host)
target_link_libraries(test_straggler_session PRIVATE tilexr-diagnostics Threads::Threads)

add_executable(test_tilexr_clock_sync_e2e
    integration/test_clock_sync_e2e.cpp
    integration/sleep_at_rdma_profile.cpp)
target_include_directories(test_tilexr_clock_sync_e2e PRIVATE
    ${TILEXR_ROOT}/src/diagnostics/host)
target_link_directories(test_tilexr_clock_sync_e2e PRIVATE
    ${ASCEND_HOME_PATH}/${ARCH}-linux/lib64
    ${ASCEND_DRIVER_PATH}/lib64/driver)
target_link_options(test_tilexr_clock_sync_e2e PRIVATE
    "LINKER:-rpath-link,${ASCEND_HOME_PATH}/${ARCH}-linux/lib64"
    "LINKER:-rpath-link,${ASCEND_DRIVER_PATH}/lib64/driver")
target_link_libraries(test_tilexr_clock_sync_e2e PRIVATE tilexr-diagnostics ascendcl runtime Threads::Threads)

# The two-marker test binary is separate from the production diagnostics binary.
list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake/cce")
enable_language(CCE)
set_source_files_properties(integration/sleep_at_rdma_profile_kernel.cpp PROPERTIES LANGUAGE CCE)
add_library(tilexr_sleep_at_rdma_profile_tmp OBJECT integration/sleep_at_rdma_profile_kernel.cpp)
target_include_directories(tilexr_sleep_at_rdma_profile_tmp PRIVATE
    $<TARGET_PROPERTY:tilexr_diagnostics_op_tmp,INCLUDE_DIRECTORIES>)
target_compile_options(tilexr_sleep_at_rdma_profile_tmp PRIVATE
    $<TARGET_PROPERTY:tilexr_diagnostics_op_tmp,COMPILE_OPTIONS>)
target_compile_definitions(tilexr_sleep_at_rdma_profile_tmp PRIVATE
    TILEXR_ENABLE_PROFILING=1 TILEXR_RDMA_DEBUG=0)
set(_sleep_rdma_binary "${CMAKE_CURRENT_BINARY_DIR}/tilexr_sleep_at_rdma_profile_op.o")
add_custom_command(OUTPUT "${_sleep_rdma_binary}"
    COMMAND ${CMAKE_CCE_LINKER} -m aicorelinux -Ttext=0
        $<TARGET_OBJECTS:tilexr_sleep_at_rdma_profile_tmp> --static
        --allow-multiple-definition -o "${_sleep_rdma_binary}"
    DEPENDS tilexr_sleep_at_rdma_profile_tmp $<TARGET_OBJECTS:tilexr_sleep_at_rdma_profile_tmp>
    COMMAND_EXPAND_LISTS VERBATIM)
add_custom_target(tilexr_sleep_at_rdma_profile_op DEPENDS "${_sleep_rdma_binary}")
add_dependencies(test_tilexr_clock_sync_e2e tilexr_sleep_at_rdma_profile_op)
file(RELATIVE_PATH _sleep_rdma_relative "${CMAKE_INSTALL_FULL_BINDIR}"
    "${CMAKE_INSTALL_FULL_LIBDIR}/tilexr/kernels/tilexr_sleep_at_rdma_profile_op.o")
target_compile_definitions(test_tilexr_clock_sync_e2e PRIVATE
    "TILEXR_SLEEP_RDMA_KERNEL_RELATIVE_PATH=\"${_sleep_rdma_relative}\"")
install(FILES "${_sleep_rdma_binary}" DESTINATION ${CMAKE_INSTALL_LIBDIR}/tilexr/kernels)

add_test(NAME test_tilexr_straggler_header_compile COMMAND test_tilexr_straggler_header_compile)
add_test(NAME test_tilexr_diagnostics_kernel_ownership COMMAND test_tilexr_diagnostics_kernel_ownership)
add_test(NAME test_straggler_analysis COMMAND test_straggler_analysis)
add_test(NAME test_straggler_session COMMAND test_straggler_session)
add_test(NAME clock_rdma_queue_cpu COMMAND test_clock_rdma_queue)
set_tests_properties(clock_rdma_queue_cpu PROPERTIES TIMEOUT 10)
if(Python3_Interpreter_FOUND)
    add_test(NAME sleep_at_rdma_profile_export_cpu
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/unit/test_sleep_at_rdma_profile_export.py)
endif()

install(TARGETS
    test_clock_drift
    test_clock_rdma_queue
    test_tilexr_straggler_header_compile
    test_tilexr_diagnostics_kernel_ownership
    test_straggler_analysis
    test_straggler_session
    test_tilexr_clock_sync_e2e
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(PROGRAMS
    analyze_clock_drift.py
    analyze_clock_residual.py
    export_sleep_at_rdma_profile.py
    run_clock_sync_e2e.sh
    run_clock_sync_e2e_multihost.py
    run_clock_sync_e2e_multihost_worker.sh
    DESTINATION ${CMAKE_INSTALL_BINDIR})
