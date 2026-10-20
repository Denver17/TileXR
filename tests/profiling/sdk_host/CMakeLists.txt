add_executable(profiling_sdk_host test_host.cpp odr.cpp)
target_link_libraries(profiling_sdk_host PRIVATE TileXR::profiling)
set_target_properties(profiling_sdk_host PROPERTIES CXX_STANDARD 14 CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO)
add_test(NAME profiling_sdk_host COMMAND profiling_sdk_host)
set_tests_properties(profiling_sdk_host PROPERTIES LABELS profiling_host)
add_executable(profiling_sdk_acl_mock test_acl.cpp)
target_include_directories(profiling_sdk_acl_mock BEFORE PRIVATE mock)
target_link_libraries(profiling_sdk_acl_mock PRIVATE TileXR::profiling)
set_target_properties(profiling_sdk_acl_mock PROPERTIES CXX_STANDARD 14 CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO)
add_test(NAME profiling_sdk_acl_mock COMMAND profiling_sdk_acl_mock)
set_tests_properties(profiling_sdk_acl_mock PROPERTIES LABELS profiling_host)
add_executable(profiling_capture_queue_mock test_capture_queue.cpp)
target_include_directories(profiling_capture_queue_mock BEFORE PRIVATE mock)
target_link_libraries(profiling_capture_queue_mock PRIVATE TileXR::profiling)
target_compile_definitions(profiling_capture_queue_mock PRIVATE TILEXR_ENABLE_PROFILING=1)
set_target_properties(profiling_capture_queue_mock PROPERTIES CXX_STANDARD 14 CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO)
add_test(NAME profiling_capture_queue_mock COMMAND profiling_capture_queue_mock)
set_tests_properties(profiling_capture_queue_mock PROPERTIES LABELS profiling_host)
add_test(NAME profiling_sdk_installed_consumer COMMAND ${CMAKE_COMMAND}
    "-DSDK_SOURCE=${CMAKE_CURRENT_LIST_DIR}/../../../src/profiling"
    "-DCONSUMER_SOURCE=${CMAKE_CURRENT_LIST_DIR}/consumer"
    "-DTEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/installed-check"
    "-DTEST_GENERATOR=${CMAKE_GENERATOR}"
    "-DTEST_COMPILER=${CMAKE_CXX_COMPILER}"
    "-DTEST_MAKE_PROGRAM=${CMAKE_MAKE_PROGRAM}"
    "-DTEST_CONFIG=$<CONFIG>"
    -P ${CMAKE_CURRENT_LIST_DIR}/installed_consumer.cmake)
set_tests_properties(profiling_sdk_installed_consumer PROPERTIES LABELS profiling_host)
set(profiling_device_test_dir "${CMAKE_CURRENT_LIST_DIR}/../sdk_device")
add_executable(profiling_device_protocol "${profiling_device_test_dir}/test_device.cpp")
target_link_libraries(profiling_device_protocol PRIVATE TileXR::profiling)
target_include_directories(profiling_device_protocol PRIVATE "${profiling_device_test_dir}/stubs")
target_compile_definitions(profiling_device_protocol PRIVATE __CCE__ __CCE_IS_AICORE__ TILEXR_ENABLE_PROFILING=1)
set_target_properties(profiling_device_protocol PROPERTIES CXX_STANDARD 14 CXX_STANDARD_REQUIRED YES)
add_test(NAME profiling_device_protocol COMMAND profiling_device_protocol)
set_tests_properties(profiling_device_protocol PROPERTIES LABELS profiling_host)
if(Python3_Interpreter_FOUND)
    add_executable(profiling_sdk_roundtrip test_roundtrip_host.cpp test_roundtrip_device.cpp)
    target_link_libraries(profiling_sdk_roundtrip PRIVATE TileXR::profiling)
    target_include_directories(profiling_sdk_roundtrip PRIVATE mock "${profiling_device_test_dir}/stubs")
    set_source_files_properties(test_roundtrip_device.cpp PROPERTIES
        COMPILE_DEFINITIONS "__CCE__;__CCE_IS_AICORE__;TILEXR_ENABLE_PROFILING=1")
    set_target_properties(profiling_sdk_roundtrip PROPERTIES
        CXX_STANDARD 14 CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO)
    add_test(NAME profiling_sdk_python_roundtrip COMMAND ${CMAKE_COMMAND} -E env
        "PYTHONPATH=${CMAKE_CURRENT_LIST_DIR}/../../../tools/profiling/python"
        ${Python3_EXECUTABLE} -B "${CMAKE_CURRENT_LIST_DIR}/check_roundtrip.py"
        "$<TARGET_FILE:profiling_sdk_roundtrip>")
    set_tests_properties(profiling_sdk_python_roundtrip PROPERTIES LABELS "profiling_host;profiling_python")
endif()
foreach(side host device)
    foreach(switch undefined 0 1)
        set(name "profiling_switch_${side}_${switch}")
        add_executable(${name} "${profiling_device_test_dir}/test_switch.cpp")
        target_link_libraries(${name} PRIVATE TileXR::profiling)
        target_include_directories(${name} PRIVATE "${profiling_device_test_dir}/stubs")
        if(side STREQUAL "device")
            target_compile_definitions(${name} PRIVATE __CCE__ __CCE_IS_AICORE__)
        endif()
        if(NOT switch STREQUAL "undefined")
            target_compile_definitions(${name} PRIVATE "TILEXR_ENABLE_PROFILING=${switch}")
        endif()
        set_target_properties(${name} PROPERTIES CXX_STANDARD 14 CXX_STANDARD_REQUIRED YES)
        add_test(NAME ${name} COMMAND ${name})
        set_tests_properties(${name} PROPERTIES LABELS profiling_host)
    endforeach()
endforeach()
