# Standalone profiling Host validation

See the [current design](../../../docs/specs/profiling.md) and
[testing guide](../../../tools/profiling/TESTING.md) for lifecycle invariants,
the manual checks, platform gaps, and evidence handoff.

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

`profiling_capture_queue_mock` protects bounded capture allocation, asynchronous
business execution and common cleanup/retry phases. The remaining device protocol
check uses CPU substitutes as described in the
[device README](../sdk_device/README.md).

These four checks are explicit local opt-ins, independent of root test switches
and CI. Packaging, installed consumers, SDK/Python round trips, ACL compile-only
checks and switch matrices have been removed. Use the Add4 example manually for
real CANN compilation or NPU execution; Host mocks do not establish either.
