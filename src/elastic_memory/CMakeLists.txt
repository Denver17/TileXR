# 构建不依赖 Torch 的 ElasticMemory Host 库，供 EP 和 adapter 链接。
# 安装 Host API、共享 ABI、ring 算法和 include/device 下的 Device 入口。
include(GNUInstallDirs)
add_library(tilexr-elastic-memory SHARED pool.cpp)
target_compile_features(tilexr-elastic-memory PUBLIC cxx_std_14)
target_include_directories(tilexr-elastic-memory PUBLIC
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/src/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_link_directories(tilexr-elastic-memory PRIVATE "${ASCEND_HOME_PATH}/${ARCH}-linux/lib64")
target_link_libraries(tilexr-elastic-memory PRIVATE ascendcl pthread)
install(TARGETS tilexr-elastic-memory LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(FILES ${CMAKE_SOURCE_DIR}/src/include/tilexr_elastic_memory.h
    ${CMAKE_SOURCE_DIR}/src/include/tilexr_elastic_memory_abi.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(FILES ${CMAKE_SOURCE_DIR}/src/include/device/tilexr_elastic_memory_device.h
    ${CMAKE_SOURCE_DIR}/src/include/device/tilexr_elastic_memory_profile.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/device)
install(FILES allocator.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
