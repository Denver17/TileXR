function(tilexr_checker_test name)
  add_executable(${name} ${ARGN})
  target_link_libraries(${name} PRIVATE tilexr-checker-core)
  target_include_directories(${name} PRIVATE
    ${PROJECT_SOURCE_DIR}/src/include
  )
  target_compile_definitions(${name} PRIVATE TILEXR_SOURCE_ROOT="${PROJECT_SOURCE_DIR}")
  add_test(NAME ${name} COMMAND ${name})
endfunction()

function(tilexr_checker_shim_test name)
  tilexr_checker_test(${name} ${ARGN})
  target_include_directories(${name} BEFORE PRIVATE
    ${PROJECT_SOURCE_DIR}/tools/checker/shim
  )
endfunction()

tilexr_checker_test(test_tilexr_checker_world
  unit/test_checker_world.cpp
)

tilexr_checker_test(test_tilexr_checker_oracles
  unit/test_checker_oracles.cpp
)

tilexr_checker_shim_test(test_tilexr_checker_shim_events
  unit/test_checker_shim_events.cpp
)

tilexr_checker_test(test_tilexr_checker_diagnostics
  unit/test_checker_diagnostics.cpp
)

tilexr_checker_test(test_tilexr_checker_executor
  unit/test_checker_executor.cpp
)

tilexr_checker_test(test_tilexr_checker_reports
  unit/test_checker_reports.cpp
)

tilexr_checker_test(test_tilexr_checker_cli_args
  unit/test_checker_cli_args.cpp
)

tilexr_checker_test(test_tilexr_checker_trace_adapter_generator
  unit/test_checker_trace_adapter_generator.cpp
)

tilexr_checker_test(test_tilexr_checker_trace_bundle_verifier
  unit/test_checker_trace_bundle_verifier.cpp
)

tilexr_checker_test(test_tilexr_checker_trace_materializer
  unit/test_checker_trace_materializer.cpp
)

tilexr_checker_test(test_tilexr_checker_trace_schedule
  unit/test_checker_trace_schedule.cpp
)

tilexr_checker_test(test_tilexr_checker_ep_dispatch_oracle_probe
  unit/test_checker_ep_dispatch_oracle_probe.cpp
)

tilexr_checker_shim_test(test_tilexr_checker_header_probe
  unit/test_checker_header_probe.cpp
)
target_include_directories(test_tilexr_checker_header_probe PRIVATE
  ${PROJECT_SOURCE_DIR}/src/collectives/host
)

tilexr_checker_shim_test(test_tilexr_checker_allgather_hdb_header_probe
  unit/test_checker_allgather_hdb_header_probe.cpp
)
target_include_directories(test_tilexr_checker_allgather_hdb_header_probe PRIVATE
  ${PROJECT_SOURCE_DIR}/src/collectives/kernels
)
target_compile_features(test_tilexr_checker_allgather_hdb_header_probe PRIVATE cxx_std_17)

tilexr_checker_shim_test(test_tilexr_checker_allreduce_big_data_header_probe
  unit/test_checker_allreduce_big_data_header_probe.cpp
)
target_include_directories(test_tilexr_checker_allreduce_big_data_header_probe PRIVATE
  ${PROJECT_SOURCE_DIR}/src/collectives/kernels
)
target_compile_features(test_tilexr_checker_allreduce_big_data_header_probe PRIVATE cxx_std_17)

add_custom_target(tilexr_checker_all
  DEPENDS
    tilexr_checker
    test_tilexr_checker_world
    test_tilexr_checker_oracles
    test_tilexr_checker_shim_events
    test_tilexr_checker_diagnostics
    test_tilexr_checker_executor
    test_tilexr_checker_reports
    test_tilexr_checker_cli_args
    test_tilexr_checker_trace_adapter_generator
    test_tilexr_checker_trace_bundle_verifier
    test_tilexr_checker_trace_materializer
    test_tilexr_checker_trace_schedule
    test_tilexr_checker_ep_dispatch_oracle_probe
    test_tilexr_checker_header_probe
    test_tilexr_checker_allgather_hdb_header_probe
    test_tilexr_checker_allreduce_big_data_header_probe
)
