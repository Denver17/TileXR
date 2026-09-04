include(CMakeParseArguments)
include(GNUInstallDirs)

function(tilexr_attach_kernel_artifacts host_target)
    set(multiValueArgs FILES TARGETS)
    cmake_parse_arguments(ARTIFACT "" "" "${multiValueArgs}" ${ARGN})

    if(NOT TARGET ${host_target})
        message(FATAL_ERROR "Unknown Host target for kernel artifacts: ${host_target}")
    endif()
    if(NOT ARTIFACT_FILES)
        message(FATAL_ERROR "tilexr_attach_kernel_artifacts requires FILES")
    endif()
    if(NOT ARTIFACT_TARGETS)
        message(FATAL_ERROR "tilexr_attach_kernel_artifacts requires TARGETS")
    endif()
    foreach(kernel_target IN LISTS ARTIFACT_TARGETS)
        if(NOT TARGET ${kernel_target})
            message(FATAL_ERROR "Unknown kernel artifact target: ${kernel_target}")
        endif()
    endforeach()

    get_target_property(host_output_dir ${host_target}
        LIBRARY_OUTPUT_DIRECTORY)
    if(NOT host_output_dir OR host_output_dir MATCHES "-NOTFOUND$")
        if(CMAKE_LIBRARY_OUTPUT_DIRECTORY)
            set(host_output_dir "${CMAKE_LIBRARY_OUTPUT_DIRECTORY}")
        else()
            get_target_property(host_output_dir ${host_target} BINARY_DIR)
            if(NOT CMAKE_CFG_INTDIR STREQUAL ".")
                string(APPEND host_output_dir "/${CMAKE_CFG_INTDIR}")
            endif()
        endif()
    endif()
    set(kernel_output_dir "${host_output_dir}/tilexr/kernels")

    list(GET ARTIFACT_FILES 0 first_artifact)
    get_filename_component(first_artifact_name "${first_artifact}" NAME_WE)
    string(MAKE_C_IDENTIFIER
        "${host_target}_${first_artifact_name}_kernel_artifacts" artifact_target)

    set(stage_commands
        COMMAND ${CMAKE_COMMAND} -E make_directory
            "${kernel_output_dir}")
    foreach(artifact IN LISTS ARTIFACT_FILES)
        get_filename_component(artifact_name "${artifact}" NAME)
        list(APPEND stage_commands
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${artifact}"
                "${kernel_output_dir}/${artifact_name}")
    endforeach()

    add_custom_target(${artifact_target}
        ${stage_commands}
        VERBATIM
        COMMENT "Staging external AICore kernels for ${host_target}")
    add_dependencies(${artifact_target} ${ARTIFACT_TARGETS})
    add_dependencies(${host_target} ${artifact_target})

    install(FILES ${ARTIFACT_FILES}
        DESTINATION "${CMAKE_INSTALL_LIBDIR}/tilexr/kernels")
endfunction()
