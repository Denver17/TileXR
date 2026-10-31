cmake_minimum_required(VERSION 3.16)
project(TileXR_Diagnostics_Tests)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
include(GNUInstallDirs)
find_package(Threads REQUIRED)

set(TILEXR_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../..")

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
    integration/test_clock_sync_e2e.cpp)
target_link_directories(test_tilexr_clock_sync_e2e PRIVATE
    ${ASCEND_HOME_PATH}/${ARCH}-linux/lib64
    ${ASCEND_DRIVER_PATH}/lib64/driver)
target_link_options(test_tilexr_clock_sync_e2e PRIVATE
    "LINKER:-rpath-link,${ASCEND_HOME_PATH}/${ARCH}-linux/lib64"
    "LINKER:-rpath-link,${ASCEND_DRIVER_PATH}/lib64/driver")
target_link_libraries(test_tilexr_clock_sync_e2e PRIVATE tilexr-diagnostics ascendcl)

add_test(NAME test_tilexr_straggler_header_compile COMMAND test_tilexr_straggler_header_compile)
add_test(NAME test_tilexr_diagnostics_kernel_ownership COMMAND test_tilexr_diagnostics_kernel_ownership)
add_test(NAME test_straggler_analysis COMMAND test_straggler_analysis)
add_test(NAME test_straggler_session COMMAND test_straggler_session)

install(TARGETS
    test_tilexr_straggler_header_compile
    test_tilexr_diagnostics_kernel_ownership
    test_straggler_analysis
    test_straggler_session
    test_tilexr_clock_sync_e2e
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(PROGRAMS
    run_clock_sync_e2e.sh
    run_clock_sync_e2e_multihost.py
    run_clock_sync_e2e_multihost_worker.sh
    DESTINATION ${CMAKE_INSTALL_BINDIR})
