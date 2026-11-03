function(add_profiling_host_test name)
    add_executable(${name} ${ARGN})
    target_link_libraries(${name} PRIVATE TileXR::profiling)
    set_target_properties(${name} PROPERTIES
        CXX_STANDARD 14 CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO)
    add_test(NAME ${name} COMMAND ${name})
    set_tests_properties(${name} PROPERTIES LABELS profiling_host)
endfunction()

add_profiling_host_test(profiling_sdk_host test_host.cpp odr.cpp)
add_profiling_host_test(profiling_sdk_acl_mock test_acl.cpp)
target_include_directories(profiling_sdk_acl_mock BEFORE PRIVATE mock)
add_profiling_host_test(profiling_capture_queue_mock test_capture_queue.cpp)
target_include_directories(profiling_capture_queue_mock BEFORE PRIVATE mock)
target_compile_definitions(profiling_capture_queue_mock PRIVATE TILEXR_ENABLE_PROFILING=1)

set(profiling_device_test_dir "${CMAKE_CURRENT_LIST_DIR}/../sdk_device")
add_profiling_host_test(profiling_device_protocol "${profiling_device_test_dir}/test_device.cpp")
target_include_directories(profiling_device_protocol PRIVATE "${profiling_device_test_dir}/stubs")
target_compile_definitions(profiling_device_protocol PRIVATE
    __CCE__ __CCE_IS_AICORE__ TILEXR_ENABLE_PROFILING=1)
