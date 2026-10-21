# TileXR profiling

The offline analyzer reads `tilexr_profiling_mapping.v1` JSON and
`tilexr_profiling_records.v1` binary files. It needs Python 3.9 or newer and
has no runtime dependencies on CANN, NPUs, TileXR libraries, or the source tree.

For the implemented architecture, ownership and invariants, see the
[current design](../../docs/specs/profiling.md). For test selection, commands,
acceptance criteria and evidence handoff, see the [testing guide](TESTING.md).
Both guides include Chinese explanations for human and AI maintainers.

```bash
# From a source checkout, using preinstalled setuptools and wheel:
python3 -m pip install --no-build-isolation --no-deps tools/profiling
# Or install a locally built wheel without an index:
python3 -m pip install --no-index --no-deps tilexr_profiling-0.1.0-py3-none-any.whl

python3 -m tilexr_profiling --mapping operator.mapping.json \
  --records capture.records.bin --output-dir profile-report \
  --attribute kernel_version=my-version --attribute soc=Ascend950
```

Repeat `--records` for rank files from the same capture UUID. The destination
must be absent or empty. Outputs are `perfetto_trace.json`, `analysis.md`, and
`report.html`; they work offline. Exit status is 0 for complete supplied inputs,
1 for fatal input or output failure, and 2 for incomplete captures with reports.
An argparse usage error also returns 2 and produces no reports.

```python
from pathlib import Path
from tilexr_profiling import generate_reports

result = generate_reports(Path("operator.mapping.json"),
                          [Path("capture.records.bin")], Path("profile-report"),
                          attributes={"kernel_version": "my-version"})
print(result.status)
```

Online capture uses the independent header SDK. Install it without building
TileXR or initializing its submodules:

```bash
cmake -S src/profiling -B build/profiling-sdk
cmake --install build/profiling-sdk --prefix "$PWD/install-profiling"
```

Include `<tilexr/profiling.h>` and use `TileXR::Profiling::AclCapture`:

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

Build and validate using preinstalled build tools:

```bash
python3 -m pip install --no-build-isolation --no-deps -e tools/profiling
python3 -B -m unittest discover -s tests/profiling/python -p 'test_*.py'
python3 scripts/ci/check_profiling_python.py
cmake -S src/profiling -B build/profiling-host -DTILEXR_PROFILING_BUILD_TESTS=ON
cmake --build build/profiling-host
ctest --test-dir build/profiling-host --output-on-failure -L profiling_host
```

The CI workflow targets Python 3.9 and 3.12; check its run results before claiming
minimum-version validation. CTest labels distinguish
`profiling_host`, `profiling_python`, `profiling_cann`, and explicitly enabled
`profiling_npu` work. Host/mocked device tests do not establish NPU visibility,
device compiler compatibility, marker overhead, register spill, or UDMA transfer.

Migration: install this package and replace `tools.profiling` imports/commands
with `tilexr_profiling`. The old Python entry point and communicator-owned
multi-launch exporter have been removed. Historical v1 files remain readable.
