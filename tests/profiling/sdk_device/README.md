# Device protocol CPU tests

See the [current design](../../../docs/specs/profiling.md) and
[testing guide](../../../tools/profiling/TESTING.md) for the ABI contract and
the separate CPU, CANN compiler, and NPU acceptance boundaries.

These tests compile the actual SDK device headers with a small Ascend C surface
substitute. They exercise sparse/empty slots, slot replacement, compact counts,
overflow, zero and maximum timestamps, initialization reuse, invalid IDs,
single-lane ownership, repeated publication, saturated arithmetic, ABI validation,
address alignment and overflow, and the 65535/65536 lane boundary.

From the repository root, using a C++14 compiler:

```sh
c++ -std=c++14 -Wall -Wextra -pedantic -D__CCE__ -D__CCE_IS_AICORE__ \
  -DTILEXR_ENABLE_PROFILING=1 -Isrc/include \
  -Itests/profiling/sdk_device/stubs tests/profiling/sdk_device/test_device.cpp \
  -o /tmp/tilexr-device-protocol
/tmp/tilexr-device-protocol
```

The standalone SDK CMake registers only the device protocol check alongside
three critical Host/ACL/queue checks. There is no switch-value test matrix or
profiling CI job; enable the manual suite with `TILEXR_PROFILING_BUILD_TESTS=ON`.

The substitute provides CPU pointers and a controllable cycle intrinsic only.
It proves no device instruction ordering, scalar store visibility, physical
timer semantics, CANN compatibility, register placement, spill behavior, or
overhead. Those require target CANN compilation and explicit NPU execution.
In particular, a final scalar state store is not a release primitive: consumers
must wait for the bound ACL stream before reading payload or state. Never use
these tests to justify live Host polling or shared-lane writers.

The reusable lesson from the old sparse writer is to initialize validity IDs,
read timestamps only after checking validity, and distinguish compact record
counts from physical alignment. A count rounded to eight hides missing events
and cannot distinguish an empty committed lane from an unexecuted lane; the
new completion state carries that distinction.
