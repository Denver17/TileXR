# Minimal profiled Add4

[English](README.md) | [简体中文](README.zh-CN.md)

The example adds four FP32 arrays using four AIV blocks and the standalone
profiling SDK. It does not use the TileXR communication runtime.

```text
add4/
├── *.cpp      Host and kernel
├── CMakeLists.txt
├── *.py       Manifest, qualification and calibration helpers
├── run_demo.sh
├── README.md
└── README.zh-CN.md
```

The Host's profiling code is `AclCapture`, `CaptureConfig`, `Initialize`,
`DeviceArgs`, `Save` and `Close` as direct calls. The minimal Host omits return-value,
SoC and numerical checks, output readback, and validation branches. Python prepares
the report manifest; optional qualification and calibration helpers remain
independent of the Host/kernel.

The kernel uses named event constants such as `kEventAccumulateBegin` and
`kEventPartialSum0End` in direct `Recorder<17>::RecordMarker` calls, then `Flush`.
Only the SDK's `TILEXR_PROFILE_ACTIVE` controls device recording. CMake supplies the same
numeric `TILEXR_ENABLE_PROFILING` to Host and kernel; Host assigns it to
`CaptureConfig.enabled`. There are no per-marker predicates or ON/OFF launch
branches. OFF executes the same Add4 kernel and saves no records.

The [mapping](profiled_add4.mapping.template.json) has one shared begin at
slot **9U**, event **20**, and three partial-sum ends **201/202/203**. Each lane
records 17 markers in consecutive slots 0–16.
The three partial sums measure cumulative windows starting at the same raw
record. Input copies reuse events 10/11 sequentially. Operator dependency
barriers remain explicit; markers do not add them.

## Build and run

Requires Linux, CMake ≥3.16, a C++14 Host compiler, CANN 9.1, and the installed
[SDK and Python package](../../tools/profiling/README.md). This example's build target is
Ascend950; the SDK itself is not restricted to that SoC.

```bash
# From the repository root, or use an installed copy's add4 directory.
cmake -S examples/profiling -B build/add4 \
  -DCMAKE_PREFIX_PATH=/path/to/profiling-prefix \
  -DTILEXR_PROFILING_SOC=Ascend950
cmake --build build/add4 -j2
cmake --install build/add4 --prefix /tmp/add4-install
# Hardware only; choose an available card and a new output file.
TILEXR_ADD4_DEVICE_ID=0 /tmp/add4-install/bin/tilexr_profiled_add4_demo /tmp/new.records.bin
```

CMake's `TILEXR_ENABLE_PROFILING` option defaults to ON. OFF runs without capture.
Kernel binaries stay external under `${CMAKE_INSTALL_LIBDIR}/tilexr/kernels`
and load relative to the executable, including after prefix relocation. Launch
uses `rtDevBinaryRegister`, `rtFunctionRegister` and `rtKernelLaunchWithFlagV2`.

For a build/run/report example, source CANN and export `TILEXR_PROFILING_PREFIX`,
`TILEXR_PROFILING_CYCLES_PER_US` and `TILEXR_PROFILING_CLOCK_BASIS`, then run:

```bash
bash examples/profiling/run_demo.sh /tmp/new-add4-results
```

Supply a confirmed timer conversion and its source; the template intentionally
contains `cycles_per_us: null`. The runner writes records, the resolved mapping,
`manifest.json`, the operator log and three reports. `manifest.py` declares the
four expected lanes and byte counts before launch, then binds the exported
capture UUID. Both the default runner and `npu_check.py` pass this manifest to
analysis. Records contain timestamps/event IDs, not workload bytes; omitting the
manifest leaves bytes and bandwidth unavailable.

Each lane handles 2048 FP32 elements (8192 bytes per array). `kernel_total`
counts four GM reads and one GM write; `copy_input` counts four GM reads and
`copy_out` one GM write. Each Add counts two UB reads and one UB write, so
`accumulate_window` counts nine arrays. The three cumulative `partial_sum`
windows contain 1+2+3 Adds and count 18 arrays in total, matching their summed
inclusive durations. All metrics use `per_lane_total`; parent/child rows are
not additive. This is logical lane-time bandwidth, not measured physical GM/UB
throughput. Full semantics remain in the folded report data.

The minimal Host does not export health observations; their status remains
unverified. Optional diagnostic APIs are in the [API reference](../../tools/profiling/docs/API_REFERENCE.md).

## Results and maintenance

Generated records, mappings, manifests and reports stay in the output directory
chosen for that run. They are not part of this feature change. SDK installation
ships the example source and READMEs; generated evidence is excluded.

Keep ACL device/stream and binary registration alive until capture cleanup has
finished. Keep the explicit stream wait before `Save` and cleanup: OFF disables
capture synchronization but still launches the kernel. Capacity/drop/event-sequence
checks belong in SDK protocol tests or the optional qualifier. The qualifier and
timer probe validate captures, not Add4 numerical results.

Keep numeric IDs consistent with the mapping when naming events; pairing uses
IDs, not C++ names. Verify exported event sequences after instrumentation changes.
