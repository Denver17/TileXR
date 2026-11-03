# TileXR profiling

[English](README.md) | [简体中文](README.zh-CN.md)

## Scope and dependencies

This is a standalone profiling tool for CANN applications. TileXR is one
consumer, not a required runtime environment. Installing the SDK/package from
this repository does not require building or installing the TileXR communication
runtime, collectives, or EP libraries. The `tilexr/` include path,
`TileXR::Profiling` namespace and `TileXR::profiling` CMake target are public
SDK names; they do not imply a dependency on `libtile-comm.so`.

| Component | Dependencies | SoC scope |
|---|---|---|
| Offline analyzer | Python ≥3.9 | No device or SoC requirement; no CANN or TileXR runtime |
| Capture SDK | C++14 Host + CANN ACL; Ascend C for device instrumentation; Linux file export | No hard-coded Ascend950 gate; each target still needs compiler/runtime/timer validation |
| Add4 example | Standalone SDK + CANN, no TileXR runtime | Currently restricted to Ascend950 by this example's build path |
| TileXR integration | TileXR application + the same SDK | Optional consumer, not a prerequisite for external operators |

The current real device-compiler evidence is Ascend950 / CANN 9.1 compilation,
without NPU execution. This is not proof that every CANN version or Ascend SoC
works. For another SoC, use its supported Ascend C target, verify timer conversion,
and validate actual records on that hardware. Do not reuse Add4's
`dav-c310-vec` compiler target as a universal setting.

## 1. Choose a workflow

| Goal | Requirements | Start here | Success means |
|---|---|---|---|
| Analyze existing captures | Python ≥3.9; mapping + matching records | Section 2 | Three reports and the expected completeness status |
| Instrument an operator | C++14 Host, target CANN toolchain; NPU for execution | Section 3 | One capture per invocation, then offline analysis |
| Run a complete example | Linux, CANN 9.1, Ascend950, confirmed timer conversion | [Add4 English](../../examples/profiling/README.md) / [中文](../../examples/profiling/README.zh-CN.md) | Numerical and lane checks plus complete reports |
| Validate without an NPU | Python, CMake ≥3.16, C++14; CANN for real compiler checks | Section 4 and [testing guide](TESTING.md) | Passed tests at the stated validation layer |

Shell commands below run from the repository root in Bash unless stated otherwise.
Replace example input paths with your files; do not run the capture example on
a machine without an NPU. SDK installation and offline analysis need no device.

The offline analyzer reads `tilexr_profiling_mapping.v1` JSON and
`tilexr_profiling_records.v1` binary files. It needs Python 3.9 or newer and
has no runtime dependencies on CANN, NPUs, TileXR libraries, or the source tree.

For the implemented architecture, ownership and invariants, see the
[current design](../../docs/specs/profiling.md). For test selection, commands,
acceptance criteria and evidence handoff, see the [testing guide](TESTING.md).
Both guides include Chinese explanations for human and AI maintainers.

## 2. Install and analyze offline

Use a dedicated Python environment if needed (`python3 -m venv .venv` then
`source .venv/bin/activate`). Source installation requires preinstalled
`setuptools>=61` and `wheel`; the analyzer itself has no third-party dependencies.
Choose one installation method below.

```bash
# From a source checkout, using preinstalled setuptools and wheel:
python3 -m pip install --no-build-isolation --no-deps tools/profiling
# Or install a locally built wheel without an index:
python3 -m pip install --no-index --no-deps tilexr_profiling-0.1.0-py3-none-any.whl

python3 -m tilexr_profiling --mapping operator.mapping.json \
  --records capture.records.bin --output-dir profile-report \
  --attribute kernel_version=my-version
```

Repeat `--records` for rank files from the same capture UUID. The destination
must be absent or empty. Outputs are `perfetto_trace.json`, `analysis.md`, and
`report.html`; they work offline. Exit status is 0 for complete supplied inputs,
1 for fatal input or output failure, and 2 for incomplete captures with reports.
An argparse usage error also returns 2 and produces no reports.

Open `report.html` in a browser, read `analysis.md`, or load
`perfetto_trace.json` in a compatible Perfetto viewer. Check report diagnostics
as well as the exit code. Status 0 applies only to supplied files: independently
check that every required rank is present. Different invocations must not be
combined even when their event IDs match.

The mapping defines nonzero event IDs and exactly one critical interval per
lane. Its `cycles_per_us` must come from a confirmed conversion for the target
timer; values in fixtures or other SoC mappings are not calibration evidence.
Use [FORMAT.md](FORMAT.md) for a complete mapping example and statistics, or
the [Add4 template](../../examples/profiling/profiled_add4.mapping.template.json),
whose deliberate `null` conversion must be filled before analysis.

The equivalent Python API is:

```python
from pathlib import Path
from tilexr_profiling import generate_reports

result = generate_reports(Path("operator.mapping.json"),
                          [Path("capture.records.bin")], Path("profile-report"),
                          attributes={"kernel_version": "my-version"})
print(result.status)
```

## 3. Install and integrate the capture SDK

Online capture uses the independent header SDK. Install it without building
TileXR or initializing its submodules:

```bash
cmake -S src/profiling -B build/profiling-sdk
cmake --install build/profiling-sdk --prefix "$PWD/install-profiling"
```

Include `<tilexr/profiling.h>` and use `TileXR::Profiling::AclCapture`:

The following is integration pseudocode. The caller must create ACL context and
stream, implement `check` and `LaunchMyKernel`, and check each API's own return
type. It is not a standalone compilable program. A CMake consumer uses
`find_package(TileXRProfiling 1 CONFIG REQUIRED)` and links
`TileXR::profiling`; supply `-DCMAKE_PREFIX_PATH=/path/to/install-profiling`.
This interface target supplies headers/C++14, not ACL libraries; the application
must locate and link CANN separately. See Add4 for a complete consumer.

For an external CANN application, the minimal CMake wiring below uses only the
installed SDK prefix and CANN. `my_operator.cpp` is your application's source,
which owns ACL initialization and kernel launch; this is build wiring, not a
complete operator implementation. Source the actual CANN environment first.

```cmake
cmake_minimum_required(VERSION 3.16)
project(MyProfiledOperator LANGUAGES CXX)
find_package(TileXRProfiling 1 CONFIG REQUIRED)
find_path(CANN_ACL_INCLUDE_DIR acl/acl.h HINTS "$ENV{ASCEND_HOME_PATH}/include")
find_library(CANN_ACL_LIBRARY ascendcl HINTS "$ENV{ASCEND_HOME_PATH}/lib64")
add_executable(my_operator my_operator.cpp)
target_compile_features(my_operator PRIVATE cxx_std_14)
target_include_directories(my_operator PRIVATE "${CANN_ACL_INCLUDE_DIR}")
target_link_libraries(my_operator PRIVATE TileXR::profiling "${CANN_ACL_LIBRARY}")
```

Configure this external project with
`cmake -S . -B build -DCMAKE_PREFIX_PATH=/path/to/install-profiling`.
Your own kernel/launcher may require additional CANN build and link settings.
After SDK installation, this consumer needs neither the TileXR source tree nor
TileXR shared libraries. Use the separately installed Python wheel to analyze
its captures on the same machine or another machine without CANN.

```cpp
TileXR::Profiling::AclCapture capture;
check(capture.Initialize(stream, logicalStreamCount));
check(LaunchMyKernel(stream, capture.DeviceArgs())); // caller's own launcher
check(capture.Save("capture.records.bin"));
check(capture.Close());
```

In the kernel, construct `TileXR::Profiling::Recorder<2>` before the measured
interval, call `RecordMarker<0,1>()` and `RecordMarker<1,2>()` around the work,
then `Flush(profileGM, logicalStream)`. Build the device code with
`TILEXR_ENABLE_PROFILING=1`; undefined or 0 disables it. Host lifecycle types
and the disk format do not change with this macro. Each lane has one writer
and each capture measures one kernel invocation. Warmup uses no capture.
Supply the timer's confirmed `cycles_per_us` in the mapping; there is no
universal frequency default.

For asynchronous integrations, keep captures alive until an existing safe
completion point. `Save`, `Collect`, and destruction can wait for the bound
stream. See [API reference](API_REFERENCE.md), [format and statistics](FORMAT.md),
and the [standalone Add4 example](../../examples/profiling/README.md).

`DeviceArgs()` is a device-memory descriptor address, not a Host stack object.
Keep the original context/stream alive until `Close()` succeeds; retry recoverable
errors as specified in the API reference. Do not destroy borrowed resources
merely because launch or Save failed. Multi-rank callers distribute the same
fresh nonzero capture UUID, use distinct ranks and output paths, and supply all
required files to the analyzer. The SDK does not coordinate ranks.

## 4. Validate at the available environment layer

Profiling is validated manually when needed and has no dedicated CI workflow or
Host CI cases. Root test switches do not enable the profiling suite. Keep only
critical resource-lifetime, record-integrity and publication regressions; ordinary
metadata, reporting, packaging and documentation changes do not require tests.

```bash
PYTHONPATH=tools/profiling/python python3 -B -m unittest discover -s tests/profiling/python -p 'test_*.py'
cmake -S src/profiling -B build/profiling-host -DTILEXR_PROFILING_BUILD_TESTS=ON
cmake --build build/profiling-host
ctest --test-dir build/profiling-host --output-on-failure
```

The optional CMake configuration registers four C++14 Host/CPU/mock checks with
the `profiling_host` label, without Python, package-consumer or switch matrices.
They do not establish NPU visibility, device compiler compatibility, marker
overhead, register spill or UDMA transfer. For real CANN compilation and explicit
Add4 execution, follow the [manual guide](TESTING.md#3-cann-和显式硬件验证).
On Windows-mounted WSL checkouts, Bash scripts must have LF line endings;
CRLF syntax failures are not kernel/compiler failures.

## 5. Troubleshooting and handoff

| Symptom | Check / next action |
|---|---|
| `No module named tilexr_profiling` | Verify installation in the same Python interpreter; inspect `python3 -c 'import tilexr_profiling; print(tilexr_profiling.__file__)'` |
| Existing output directory rejected | Use a new or empty directory; never overwrite a previous report set |
| Missing/invalid clock conversion | Supply a confirmed positive timer conversion and retain its source; do not guess |
| Mixed UUID or duplicate stream | Analyze only one invocation; verify rank/lane identity and input selection |
| Exit 2 | Distinguish incomplete reports from command usage errors by checking diagnostics and artifacts |
| CANN runtime headers missing | Source the intended CANN 9.1 environment; inspect the actual toolkit layout and configure a fresh build |

For human or AI handoff, record the source commit and dirty diff, environment,
exact commands/cwd, input files and expected ranks, capture UUID, clock conversion
and basis, exit statuses, diagnostics, and artifact paths. State unrun layers
explicitly. Preserve existing changes and use fresh output paths. Follow
[TESTING.md](TESTING.md) for acceptance evidence and
[the current design](../../docs/specs/profiling.md) for contracts; do not replace
hardware evidence with mock results. Keep both language versions synchronized
when changing commands, prerequisites or success criteria.

Migration: install this package and replace `tools.profiling` imports/commands
with `tilexr_profiling`. The old Python entry point and communicator-owned
multi-launch exporter have been removed. Historical v1 files remain readable.
