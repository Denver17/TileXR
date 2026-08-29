# TileXR Build Verification

**Updated:** 2026-08-04

This checklist reflects the current TileXR codebase. The core runtime builds `libtile-comm.so` without a compile-time or link-time shmem dependency.

## Environment

```bash
cd /path/to/TileXR
source scripts/common_env.sh
npu-smi info
```

Expected:

- CANN 9.1.0 environment is visible through `ASCEND_HOME_PATH`.
- `scripts/common_env.sh` detects architecture and SOC information.
- NPU driver version is 25.1.rc1 or later.

## Build Core Runtime

```bash
cmake -S . -B /tmp/tilexr-build -DCMAKE_INSTALL_PREFIX="$PWD/install"
cmake --build /tmp/tilexr-build --target tile-comm -j"$(nproc)"
cmake --install /tmp/tilexr-build
```

Expected:

```bash
test -f install/lib/libtile-comm.so
```

Check dynamic dependencies:

```bash
ldd install/lib/libtile-comm.so | grep -E "ascendcl|runtime|ascend_hal|profapi"
ldd install/lib/libtile-comm.so | grep -i shmem || true
```

Expected:

- CANN runtime libraries are resolved.
- The shmem grep prints nothing for the current TileXR UDMA implementation.

## Build UDMA Tests

```bash
cd tests/udma
bash build.sh
```

Expected host-side artifacts:

```bash
test -x install/bin/test_tilexr_udma_transport_layout
test -x install/bin/test_tilexr_udma_registry
test -x install/bin/test_tilexr_udma
```

If `bisheng` is available, the AICore demo artifacts should also exist:

```bash
test -x install/bin/tilexr_udma_demo
test -f install/lib/libtilexr_udma_demo_kernel.so
```

If `bisheng` is unavailable, `build.sh` may skip only the demo target while still building host-only tests.

## Build MoonEP Five-Stage Runtime

MoonEP native five-stage support requires CANN 9.1 and an A5 target compiler. Building
does not require launching an NPU kernel:

```bash
source /path/to/cann-9.1.0/set_env.sh
cmake -S . -B build-moonep \
  -DTILEXR_BUILD_MOONEP=ON \
  -DTILEXR_BUILD_TESTS=ON \
  -DCMAKE_INSTALL_PREFIX="$PWD/install"
cmake --build build-moonep -j"$(nproc)"
cmake --install build-moonep
```

MoonEP custom-kernel commands must list the implementation header selected by
the kernel entry in `DEPENDS`. Otherwise an implementation-header edit can
leave a stale embedded kernel object during an incremental build. Verify this
through an incremental target-toolchain rebuild and the resulting embedded
binary; production-source substring checks are not accepted as a substitute
for compile, link, or behavior evidence.

For Combine V2 schedule, workspace, Host validation, launch ABI, and public ABI,
run:

```bash
bash scripts/validate_combine_v2_host.sh
```

Set `TILEXR_CMAKE_BIN` when CMake is not on `PATH`, and
`TILEXR_COMBINE_V2_HOST_BUILD_DIR` to keep the build outside the source tree.
These tests verify Credit routing and Host contracts, but do not validate
Ascend C compilation, MTE ordering on hardware, or multi-rank correctness.

Before a multi-rank MoonEP hardware run, execute the official HCCL Test on the
same devices and data path. Check the per-rank correctness output as well as the
process status: HCCL operation failures can still exit with status 0. Stop the
TileXR run when the matching HCCL baseline reports an interface or correctness
failure.

Run the Host suite separately:

```bash
cmake -S tests/moonep -B tests/moonep/build-host
cmake --build tests/moonep/build-host -j"$(nproc)"
ctest --test-dir tests/moonep/build-host --output-on-failure
python -m pytest tests/moonep/python -q
```

Inspect the installed public library:

```bash
nm -D install/lib*/libtilexr-moonep.so.1 | \
  grep -E 'TileXRMoonEp(Planning|Dispatch|PrefetchWeight|Combine|ReduceGrad)V1'
readelf -d install/lib*/libtilexr-moonep.so.1 | \
  grep -E 'NEEDED|RPATH|RUNPATH'
```

Expected:

- all five V1 symbols are exported;
- planner, dispatch, prefetch-weight, combine, and reduce-grad Host libraries are NEEDED;
- RPATH is `$ORIGIN` and contains no CANN `devlib` directory;
- Host and Python fake-runtime tests pass without device execution.

Do not describe these checks as NPU correctness. When runtime testing is authorized,
run the installed demo first on one rank, then on the intended physical A5 ranks. For
the validated four-card shape used on 2026-08-04:

```bash
ASCEND_RT_VISIBLE_DEVICES=4,5,6,7 \
  bash install/bin/tilexr_moonep_flow_run_a5.sh 4 64 4 32 512 4
```

Require every rank to report Planning 0, Dispatch 2000, PrefetchWeight 4000, Combine
3000, and ReduceGrad 5000, plus `native MoonEP flow smoke passed`. Then run the
separate precision and msprof gates documented in
`docs/moonep/DISPATCH_COMBINE.md`.

### MoonEP Python Correctness Reference

The CPU suite checks five-stage contracts, deterministic fixtures, the upstream
Planning oracle, downstream reference semantics, fault localization, CLI mode routing,
and report aggregation:

```bash
python -m pytest tests/moonep/python -q
```

This does not prove Torch-NPU or HCCL execution. On a CANN 9.1 host with four visible
A5/Ascend950 devices, run the independent reference separately:

```bash
python tools/moonep/run_benchmark.py \
  --mode reference \
  --cases tools/moonep/cases/correctness.json \
  --world-size 4 --physical-device-count 4 \
  --output-dir temp/moonep-reference
```

Require all five stage artifacts to pass on every rank. `performance_valid` must remain
false. Differential execution defaults to the built-in TileXR adapter. Run one rank
first against the installed libraries:

```bash
ASCEND_RT_VISIBLE_DEVICES=4 python -m tools.moonep.launcher \
  --mode correctness \
  --cases tools/moonep/cases/correctness.json \
  --case-ids planning-no-dedup \
  --world-size 1 --physical-device-count 1 \
  --install-prefix "$TILEXR_INSTALL_PREFIX" \
  --output-dir temp/moonep-correctness-1r
```

Then validate the same-node four-rank path:

```bash
ASCEND_RT_VISIBLE_DEVICES=4,5,6,7 python -m tools.moonep.launcher \
  --mode correctness \
  --cases tools/moonep/cases/correctness.json \
  --case-ids planning-no-dedup \
  --world-size 4 --physical-device-count 4 \
  --install-prefix "$TILEXR_INSTALL_PREFIX" \
  --output-dir temp/moonep-correctness-4r
```

The adapter rejects missing/stub stages, mismatched dimensions or device/rank metadata,
cross-node topology before Planning, and repeated destination ranks before the current
URMA Dispatch. External adapters remain available through
`--candidate-backend MODULE:FACTORY`. These runs are correctness-only and must retain
`performance_valid=false`.

## Run Host Checks

```bash
cd /path/to/TileXR/tests/udma
./install/bin/test_tilexr_udma_transport_layout
./install/bin/test_tilexr_udma_registry

source ../../scripts/common_env.sh
export LD_LIBRARY_PATH="$PWD/install/lib:../../install/lib:${LD_LIBRARY_PATH:-}"
RANK=0 RANK_SIZE=1 ./install/bin/test_tilexr_udma
```

Expected:

- `TileXR UDMA transport layout checks passed`
- `TileXR UDMA JSON checks passed`
- `TileXR UDMA registry checks passed`
- `test_tilexr_udma` exits with `Failed: 0`

## Run UDMA Data-Plane Demos

Run only on A5 / Ascend950 / 950 hardware:

```bash
cd /path/to/TileXR/tests/udma
bash demo/run_tilexr_udma_demo.sh 0 2 16 2 0
bash demo/run_tilexr_udma_demo.sh 1 2 16 2 0
```

Expected:

- every rank exits 0;
- every rank log includes `TileXR UDMA demo success`;
- every rank log includes `TileXRUDMARegister success`;
- no rank log includes `DATA MISMATCH`, signal mismatch text, or `ERROR`.

Quick log check:

```bash
latest=$(ls -td logs/tilexr_udma_demo_* | head -n1)
grep -R "TileXR UDMA demo success" "$latest"
grep -R "TileXRUDMARegister success" "$latest"
grep -R "DATA MISMATCH\\|expected non-local signals\\|TileXR UDMA demo failed\\|ERROR" "$latest" || true
```

The final grep should print nothing for a clean run.

## Common Failures

| Symptom | Likely Cause | Action |
| --- | --- | --- |
| Missing CANN headers | `common_env.sh` not sourced or CANN path mismatch | Source the environment and confirm CANN 9.1.0 layout |
| Cannot find `ascend_hal` | `devlib` path missing | Use the current top-level CMake configuration |
| Demo target skipped | `bisheng` unavailable | Install/compiler configure `bisheng`, or run host-only tests |
| UDMA disabled in demo | Unsupported hardware or HCCP/RA runtime unavailable | Use A5 / Ascend950 / 950 and check CANN driver/runtime libraries |
| shmem appears in `ldd libtile-comm.so` | Unexpected dependency regression | Inspect `src/comm/CMakeLists.txt` and source includes |

## Report Template

```text
Machine:
NPU model:
NPU count:
CANN version:
Driver version:
TileXR commit:

Core build:
ldd shmem check:
UDMA host tests:
UDMA all-gather demo:
UDMA put-signal demo:
Log directory:

Errors or warnings:
```
