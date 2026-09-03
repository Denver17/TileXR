include(GNUInstallDirs)

if(NOT TILEXR_BUILD_MOONEP AND NOT TILEXR_BUILD_MOONEP_PLANNER)
    return()
endif()

if(NOT TARGET tilexr-moonep-planner)
    add_subdirectory(planner_v2)
endif()
if(NOT TARGET tilexr-moonep-planner-v3-objects)
    add_subdirectory(planner)
endif()

if(NOT TILEXR_BUILD_MOONEP)
    return()
endif()

if(NOT TARGET tilexr-moonep-dispatch)
    add_subdirectory(dispatch)
endif()
if(NOT TARGET tilexr-moonep-combine-v2)
    add_subdirectory(combine_v2)
endif()
if(NOT TARGET tilexr-moonep-prefetch-weight)
    add_subdirectory(prefetch_weight)
endif()
if(NOT TARGET tilexr-moonep-reduce-grad)
    add_subdirectory(reduce_grad)
endif()

add_library(tilexr-moonep SHARED
    host/tilexr_moonep.cpp
)

target_compile_features(tilexr-moonep PRIVATE cxx_std_14)
target_include_directories(tilexr-moonep
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
    PRIVATE
        ${ASCEND_HOME_PATH}/${ARCH}-linux/pkg_inc
        ${ASCEND_HOME_PATH}/${ARCH}-linux/pkg_inc/runtime
        ${ASCEND_HOME_PATH}/${ARCH}-linux/include
)
target_link_directories(tilexr-moonep PRIVATE
    ${ASCEND_HOME_PATH}/${ARCH}-linux/lib64
)
target_link_libraries(tilexr-moonep PRIVATE
    tilexr-moonep-planner
    tilexr-moonep-dispatch
    tilexr-moonep-combine-v2
    tilexr-moonep-prefetch-weight
    tilexr-moonep-reduce-grad
    tile-comm
    ascendcl
    runtime
)

set_target_properties(tilexr-moonep PROPERTIES
    VERSION 1.0.0
    SOVERSION 1
    INSTALL_RPATH "$ORIGIN"
)

install(TARGETS tilexr-moonep LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/../include/tilexr_moonep.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

if((BUILD_TESTING OR TILEXR_BUILD_TESTS) AND NOT TARGET test_tilexr_moonep_host)
    add_subdirectory(${CMAKE_SOURCE_DIR}/tests/moonep
        ${CMAKE_BINARY_DIR}/tests/moonep)
endif()
