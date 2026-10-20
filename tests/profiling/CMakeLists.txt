# The root adds src/profiling once, after this directory. Use that same SDK
# validation suite rather than a separate communication-linked implementation.
set(TILEXR_PROFILING_BUILD_TESTS ON CACHE BOOL "Build standalone profiling Host tests")
