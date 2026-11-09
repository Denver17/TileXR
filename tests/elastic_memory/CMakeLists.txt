# 构建 Host allocator 测试、独立 AICore 测试 binary 和 ACL/EP 测试程序。
# 仅将 Host allocator 注册到 CTest；NPU 测试由调用者选择空闲设备后启动。
add_executable(test_elastic_allocator test_allocator.cpp)
target_include_directories(test_elastic_allocator PRIVATE ${CMAKE_SOURCE_DIR}/src/elastic_memory)
add_test(NAME elastic_allocator COMMAND test_elastic_allocator)
list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake/cce")
enable_language(CCE)
set_source_files_properties(pool_kernel.cpp PROPERTIES LANGUAGE CCE)
add_library(elastic_pool_kernel_tmp OBJECT pool_kernel.cpp)
target_include_directories(elastic_pool_kernel_tmp PRIVATE ${CMAKE_SOURCE_DIR}/src/elastic_memory
    ${ASCEND_HOME_PATH}/${ARCH}-linux/tikcpp/tikcfw
    ${ASCEND_HOME_PATH}/${ARCH}-linux/tikcpp/tikcfw/interface
    ${ASCEND_HOME_PATH}/${ARCH}-linux/tikcpp/tikcfw/impl)
target_compile_options(elastic_pool_kernel_tmp PRIVATE -O2 -std=gnu++17 --cce-aicore-only
    --cce-aicore-arch=dav-c310-vec
    "SHELL:-mllvm -cce-aicore-stack-size=0x8000"
    "SHELL:-mllvm -cce-aicore-function-stack-size=0x8000")
set(pool_kernel "${CMAKE_CURRENT_BINARY_DIR}/elastic_pool_kernel.o")
add_custom_command(OUTPUT "${pool_kernel}"
    COMMAND ${CMAKE_CCE_LINKER} -m aicorelinux -Ttext=0 $<TARGET_OBJECTS:elastic_pool_kernel_tmp>
        --static -o "${pool_kernel}" --allow-multiple-definition
    DEPENDS elastic_pool_kernel_tmp $<TARGET_OBJECTS:elastic_pool_kernel_tmp>
    COMMAND_EXPAND_LISTS VERBATIM)
add_custom_target(elastic_pool_kernel ALL DEPENDS "${pool_kernel}")
add_executable(test_elastic_pool test_pool.cpp)
target_link_directories(test_elastic_pool PRIVATE ${ASCEND_HOME_PATH}/${ARCH}-linux/lib64)
target_link_libraries(test_elastic_pool PRIVATE tilexr-elastic-memory ascendcl runtime pthread)
add_dependencies(test_elastic_pool elastic_pool_kernel)
add_library(elastic_growth_fault SHARED growth_fault_injection.cpp)
target_compile_definitions(elastic_growth_fault PRIVATE TILEXR_GROWTH_FAULT_PRELOAD=1)
target_link_directories(elastic_growth_fault PRIVATE ${ASCEND_HOME_PATH}/${ARCH}-linux/lib64)
target_link_libraries(elastic_growth_fault PRIVATE ascendcl dl)
add_executable(test_elastic_growth_fault growth_fault_injection.cpp)
target_link_directories(test_elastic_growth_fault PRIVATE ${ASCEND_HOME_PATH}/${ARCH}-linux/lib64)
target_link_libraries(test_elastic_growth_fault PRIVATE tilexr-elastic-memory ascendcl runtime pthread dl)
add_dependencies(test_elastic_growth_fault elastic_pool_kernel elastic_growth_fault)
if(TARGET tilexr-ep)
    add_executable(test_elastic_compact_ep test_compact_ep.cpp)
    target_link_directories(test_elastic_compact_ep PRIVATE ${ASCEND_HOME_PATH}/${ARCH}-linux/lib64)
    target_link_libraries(test_elastic_compact_ep PRIVATE tilexr-elastic-memory tilexr-ep tile-comm ascendcl runtime pthread)
endif()
