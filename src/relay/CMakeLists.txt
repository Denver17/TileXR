# Build/install the optional, operator-independent Relay Host runtime.
# Python sources and prebuilt ELF/Plan pairs are packaged without running TileLang.
include(GNUInstallDirs)
add_library(tilexr-relay SHARED tilexr_relay.cpp
    ${CMAKE_SOURCE_DIR}/src/common/tilexr_kernel_binary_loader.cpp)
target_compile_features(tilexr-relay PUBLIC cxx_std_14)
target_include_directories(tilexr-relay PUBLIC
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/src/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_include_directories(tilexr-relay PRIVATE ${CMAKE_SOURCE_DIR}/src/common)
target_link_directories(tilexr-relay PRIVATE "${ASCEND_HOME_PATH}/${ARCH}-linux/lib64")
target_link_libraries(tilexr-relay PUBLIC tile-comm PRIVATE ascendcl runtime ${CMAKE_DL_LIBS})
install(TARGETS tilexr-relay LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(FILES ${CMAKE_SOURCE_DIR}/src/include/tilexr_relay.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
option(TILEXR_INSTALL_RELAY_PYTHON "Install optional Relay Python runtime/build package" OFF)
if(TILEXR_INSTALL_RELAY_PYTHON)
    install(DIRECTORY ${CMAKE_SOURCE_DIR}/src/tilelang/python/tilexr_relay
        DESTINATION ${CMAKE_INSTALL_DATADIR}/tilexr/tilelang
        FILES_MATCHING PATTERN "*.py")
    install(FILES ${CMAKE_SOURCE_DIR}/scripts/build_tilexr_relay.py
        DESTINATION ${CMAKE_INSTALL_DATADIR}/tilexr/tilelang)
endif()
set(TILEXR_RELAY_KERNEL_DIR "" CACHE PATH "Prebuilt content-addressed Relay ELF/manifest directory")
if(TILEXR_RELAY_KERNEL_DIR)
    file(GLOB relay_objects CONFIGURE_DEPENDS "${TILEXR_RELAY_KERNEL_DIR}/tilexr_relay_*.o")
    foreach(object IN LISTS relay_objects)
        get_filename_component(name "${object}" NAME_WE)
        if(NOT EXISTS "${TILEXR_RELAY_KERNEL_DIR}/${name}.json")
            message(FATAL_ERROR "Missing Relay manifest for ${object}")
        endif()
        install(FILES "${object}" "${TILEXR_RELAY_KERNEL_DIR}/${name}.json"
            DESTINATION ${CMAKE_INSTALL_LIBDIR}/tilexr/kernels COMPONENT TileRelayKernels)
    endforeach()
endif()
