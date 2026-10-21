# Standalone profiling Host validation

See the [current design](../../../docs/specs/profiling.md) and
[testing guide](../../../tools/profiling/TESTING.md) for lifecycle invariants,
the full test matrix, platform gaps, and evidence handoff.

Run without CANN, the root TileXR project, or submodules:

```sh
cmake -S src/profiling -B build/profiling-host -DTILEXR_PROFILING_BUILD_TESTS=ON
cmake --build build/profiling-host
ctest --test-dir build/profiling-host --output-on-failure -L profiling_host
```

`profiling_sdk_host` validates bounded layout arithmetic, compact non-aligned
counts, every completion state, timestamp zero, stable ordering, v1 bytes,
and (on Linux) no-overwrite publication in concurrent processes. Injectable
file operations exercise write, fsync, close, hard-link, temporary collision,
and post-publication cleanup failures. A failure after the hard link succeeds
is a successful publication with a cleanup warning; retrying it as an
unpublished capture can misreport a successful measurement.

`profiling_sdk_acl_mock` uses an aligned CPU allocation and a small ACL
substitute. It checks partial initialization cleanup, current-context ownership,
per-stream waiting, synchronization/D2H/free retry, disabled capture, movement,
and snapshot lifetime. It does not execute ACL or NPU instructions. Stream
synchronization failure must retain workspace; freeing it would allow in-flight
work to access released GM. Explicit `Close` permits a retry and reports the
failure while the destructor can only diagnose and conservatively retain GM.

`profiling_sdk_installed_consumer` copies only release SDK files into a staging
tree, installs them, removes that tree, then compiles and executes an external
C++14 executable plus shared library and multiple translation units using only
the installation prefix. No source-tree includes or profiling binary library
are available to that consumer. The device protocol and switch tests use CPU
substitutes, as documented in [the device test README](../sdk_device/README.md).

With Python >=3.9 available, `profiling_sdk_python_roundtrip` compiles separate
Host and stubbed-device translation units. Real Recorder/DirectWriter output
passes through mocked ACL D2H, BuildSnapshot, EncodeV1, and the canonical Python
reader/report API. It checks conditional slots, timestamp-zero backfill and
stable ties, a dropped prefix, committed empty and unsubmitted lanes, plus the
resulting incomplete report. This cross-language test catches producer/encoder/
reader drift that independent fixtures cannot; it proves no NPU visibility.

For actual ACL compile/link coverage, set `TILEXR_PROFILING_WITH_ACL=ON` in a
CANN environment and run label `profiling_cann`. That group checks headers and
linkage only. No NPU test is registered here; NPU execution requires the example's
explicit device configuration and remains a separate validation boundary.
