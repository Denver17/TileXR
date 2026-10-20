# Independent profiled Add4

This Linux example consumes only the installed public profiling headers and
CANN. It creates its ACL device/stream, calls `AclCapture::Initialize(stream,4)`,
passes `DeviceArgs()` to its kernel, and calls `Save(path)` and `Close()`.
The application owns ACL initialization, device/context, stream, numerical
validation and kernel launch. No communicator or TileXR shared library is used.

Four AIV blocks each add four 2,048-element float32 inputs. Each block owns one
logical lane and writes 20 markers using `TileXR::Profiling::Recorder<20>`.
Input-copy intervals repeat sequentially; three explicit partial-sum instances
overlap within `accumulate_window`. IDs `1/2` define the unique critical
`kernel_total` interval. Flush follows the final measured marker, after the
operator's own output-copy dependency has completed. Marker calls add no
automatic pipeline barrier.

Install the independent SDK and Python package first, then source the actual
CANN 9.1 environment. The SDK install includes this entire example at
`share/tilexr-profiling/examples/add4`; copy that directory to a clean external
project to exercise installed consumption:

```sh
cmake -S /path/to/add4 -B /tmp/add4-build \
  -DCMAKE_PREFIX_PATH=/path/to/sdk-prefix \
  -DTILEXR_PROFILING_SOC=Ascend950
cmake --build /tmp/add4-build -j2
cmake --install /tmp/add4-build --prefix /tmp/add4-install
TILEXR_ADD4_DEVICE_ID=0 /tmp/add4-install/bin/tilexr_profiled_add4_demo /tmp/new.records.bin
```

The build emits a pure AICore ELF, installs it under
`${CMAKE_INSTALL_LIBDIR}/tilexr/kernels`, and loads it relative to the executable
using `/proc/self/exe`. Registration uses `rtDevBinaryRegister` and
`rtFunctionRegister`; launch uses `rtKernelLaunchWithFlagV2`. The executable does
not embed a kernel object or depend on a private TileXR loader. Relative bin/lib
install directories preserve this behavior when the whole prefix moves. The
kernel is also available under the same relative build-tree layout. Normal CANN
runtime library discovery must resolve the real runtime/driver libraries.

For capture and all three reports, run `bash run_demo.sh NEW_OUTPUT_DIRECTORY`
with these variables set to actual local values:

| Variable | Required value |
|---|---|
| `ASCEND_HOME_PATH` | Active target CANN installation |
| `TILEXR_PROFILING_PREFIX` | Installed independent SDK prefix |
| `TILEXR_PROFILING_SOC` | `Ascend950`, the only current example target |
| `TILEXR_PROFILING_CYCLES_PER_US` | Confirmed positive conversion for `GetSystemCycle()` |
| `TILEXR_PROFILING_CLOCK_BASIS` | Version-matched source or calibration justifying it |
| `TILEXR_ADD4_DEVICE_ID` | Selected card index; defaults to `0` |

Optionally supply `TILEXR_PROFILING_CANN_VERSION`,
`TILEXR_PROFILING_DRIVER_VERSION`, and `TILEXR_PROFILING_FIRMWARE_VERSION`.
Unknown values stay unknown. `environment.txt` retains compiler/platform output,
standards, macro, kernel/SDK version and conversion basis; `operator.log`
records the actual SoC, external binary path and numerical/record checks. This
runner never reserves devices: choose an available card using your normal
environment policy. It performs no communication-environment qualification.

The mapping template deliberately has `cycles_per_us: null` and cannot be
analyzed directly. The runner fills it with the confirmed supplied value. Never
copy a timer conversion from another SoC. Reports use the installed canonical
`python3 -m tilexr_profiling` entry and include caller-supplied provenance.
The mapping event forms, hierarchy and statistical meanings are documented in
the package's `FORMAT.md`; SDK ownership and errors are in `API_REFERENCE.md`.

Only a successful explicit hardware run establishes numerical output, four
COMMITTED lanes, 20 records per lane and complete reports. No NPU test is
registered in default CTest. CANN compilation and NPU execution were unavailable
in the Windows authoring environment; this migration retains the existing
Ascend950 `dav-c310-vec` compiler path and device `gnu++17` standard. Host code
uses C++14. Device C++14, other SoCs, instruction visibility, spill and overhead
remain unverified. Low-overhead claims additionally require measurements of
compile-disabled, local-markers-only, and markers-plus-flush builds on the same
target, including recorder capacities large enough to expose spill.

When modifying the example, keep kernel data alive through binary registration
and retain the borrowed stream/context through `Close()` and capture
destruction. A launch error or Save error does not imply no work is in flight;
failure cleanup synchronizes before freeing operator memory and conservatively
retains resources if synchronization fails.
