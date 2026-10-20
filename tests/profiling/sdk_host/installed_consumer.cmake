function(run)
    execute_process(COMMAND ${ARGV} RESULT_VARIABLE result)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "Installed SDK consumer command failed: ${ARGV}")
    endif()
endfunction()
set(config_arguments)
set(ctest_config_arguments)
if(NOT "${TEST_CONFIG}" STREQUAL "")
    set(config_arguments --config "${TEST_CONFIG}")
    set(ctest_config_arguments -C "${TEST_CONFIG}")
endif()
file(MAKE_DIRECTORY "${TEST_ROOT}")
# Copy only the release headers and package metadata; tests/examples/comm are
# absent. Build consumers with only the installed prefix on the package path.
file(MAKE_DIRECTORY "${TEST_ROOT}/release/src/include")
file(COPY "${SDK_SOURCE}/../include/tilexr" DESTINATION "${TEST_ROOT}/release/src/include")
file(COPY "${SDK_SOURCE}" DESTINATION "${TEST_ROOT}/release/src")
file(MAKE_DIRECTORY "${TEST_ROOT}/release/examples/profiling")
run("${CMAKE_COMMAND}" -S "${TEST_ROOT}/release/src/profiling" -B "${TEST_ROOT}/sdk-build"
    -G "${TEST_GENERATOR}" "-DCMAKE_CXX_COMPILER=${TEST_COMPILER}"
    "-DCMAKE_MAKE_PROGRAM=${TEST_MAKE_PROGRAM}"
    -DTILEXR_PROFILING_BUILD_TESTS=OFF -DTILEXR_PROFILING_WITH_ACL=OFF -DTILEXR_PROFILING_BUILD_EXAMPLES=OFF)
run("${CMAKE_COMMAND}" --install "${TEST_ROOT}/sdk-build" --prefix "${TEST_ROOT}/prefix" ${config_arguments})
file(COPY "${CONSUMER_SOURCE}/" DESTINATION "${TEST_ROOT}/consumer")
get_filename_component(test_root_absolute "${TEST_ROOT}" ABSOLUTE)
foreach(owned release sdk-build)
    get_filename_component(owned_absolute "${TEST_ROOT}/${owned}" ABSOLUTE)
    string(FIND "${owned_absolute}" "${test_root_absolute}/" under_root)
    if(NOT under_root EQUAL 0)
        message(FATAL_ERROR "Refusing to remove path outside installed consumer test root")
    endif()
    file(REMOVE_RECURSE "${owned_absolute}")
endforeach()
run("${CMAKE_COMMAND}" -S "${TEST_ROOT}/consumer" -B "${TEST_ROOT}/consumer-build"
    -G "${TEST_GENERATOR}" "-DCMAKE_CXX_COMPILER=${TEST_COMPILER}"
    "-DCMAKE_MAKE_PROGRAM=${TEST_MAKE_PROGRAM}" "-DCMAKE_PREFIX_PATH=${TEST_ROOT}/prefix")
run("${CMAKE_COMMAND}" --build "${TEST_ROOT}/consumer-build" ${config_arguments})
run("${CMAKE_CTEST_COMMAND}" --test-dir "${TEST_ROOT}/consumer-build" ${ctest_config_arguments} --output-on-failure)
