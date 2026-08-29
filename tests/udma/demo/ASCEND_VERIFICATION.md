# TileXR UDMA Demo Ascend Verification

This checklist is intended for an A5 / Ascend950 / 950 machine with real devices. The current
`vub:/home/TileXR-new` environment can compile the demo because CANN is
installed, but it does not have physical Ascend cards, so runtime UDMA behavior
must be verified elsewhere.

## Goal

Verify that the TileXR UDMA demo:

- builds against TileXR's public demo API without including `shmem.h` in the host demo source;
- initializes UDMA through TileXR's own comm transport, without linking shmem;
- registers ordinary `aclrtMalloc` device memory through `TileXRUDMARegister`;
- runs device-side UDMA put and put-signal kernels successfully;
- does not report data mismatches or signal mismatches in rank logs.

## Hardware And Environment

UDMA means UnifiedBus DMA. Run this only on A5 / Ascend950 / 950 hardware. 910B,
310P, and other non-A5 devices are not valid UDMA runtime validation targets.
The demo build path currently targets `Ascend950` by default.

Before building, check:

```bash
cd /path/to/TileXR
source scripts/common_env.sh
npu-smi info
```

Expected:

- `npu-smi info` lists usable Ascend devices.
- `ASCEND_HOME_PATH` points to the CANN toolkit.
- `TILEXR_OS_ARCH` or detected `ARCH` matches the installed CANN package.

## Build

Build TileXR `tile-comm`:

```bash
cd /path/to/TileXR
source scripts/common_env.sh
cmake -S . -B /tmp/tilexr-build-udma -DCMAKE_INSTALL_PREFIX="$PWD/install"
cmake --build /tmp/tilexr-build-udma --target tile-comm -j"$(nproc)"
cmake --install /tmp/tilexr-build-udma
```

Build the UDMA tests and demo:

```bash
cd /path/to/TileXR/tests/udma
bash build.sh
```

Expected build artifacts:

```bash
test -x install/bin/tilexr_udma_demo
test -f install/lib/libtilexr_udma_demo_kernel.so
```

## Runtime Setup

Use one process per rank. The helper script launches local ranks and writes logs
under `tests/udma/logs/`.

Important environment variables:

```bash
cd /path/to/TileXR/tests/udma
source ../../scripts/common_env.sh
export TILEXR_COMM_ID="${TILEXR_COMM_ID:-127.0.0.1:10067}"
export LD_LIBRARY_PATH="$PWD/install/lib:../../install/lib:${LD_LIBRARY_PATH:-}"
```

The demo's host-side process barrier uses `127.0.0.1` and a demo-only port offset
derived from `TILEXR_COMM_ID`. Avoid running two demo instances with the same
`TILEXR_COMM_ID` at the same time.

## Test 1: UDMA Put All-Gather

Run two ranks first:

```bash
cd /path/to/TileXR/tests/udma
bash demo/run_tilexr_udma_demo.sh 0 2 16 2 0
```

Arguments are:

```text
run_tilexr_udma_demo.sh <test_type> <rank_size> <elements_per_rank> <npu_count> <first_npu>
```

For this test:

- `test_type=0`: all-gather style UDMA put;
- `rank_size=2`: launch 2 local ranks;
- `elements_per_rank=16`: each rank contributes 16 `int32_t` values;
- `npu_count=2`, `first_npu=0`: use device 0 and device 1.

Expected:

- script exits with code 0;
- each rank log contains `TileXR UDMA demo success`;
- each rank log prints `UDMA=enabled`;
- each rank log prints `TileXRUDMARegister success`;
- result sample contains one segment per rank, for example `seg0=1000 seg1=1001`;
- no log contains `DATA MISMATCH`, `ERROR`, or `TileXR UDMA demo failed`.

Quick log check:

```bash
latest=$(ls -td logs/tilexr_udma_demo_* | head -n1)
grep -R "TileXR UDMA demo success" "$latest"
grep -R "UDMA=enabled" "$latest"
grep -R "TileXRUDMARegister success" "$latest"
grep -R "DATA MISMATCH\\|TileXR UDMA demo failed\\|ERROR" "$latest" || true
```

## Test 2: UDMA Put With Signal

Run the put-signal variant:

```bash
cd /path/to/TileXR/tests/udma
bash demo/run_tilexr_udma_demo.sh 1 2 16 2 0
```

Expected:

- script exits with code 0;
- each rank log contains `TileXR UDMA demo success`;
- each rank log prints signal values;
- for rank size 2, non-local signal entries should equal `1000`;
- no log contains `DATA MISMATCH`, `expected non-local signals`, `ERROR`, or
  `TileXR UDMA demo failed`.

Quick log check:

```bash
latest=$(ls -td logs/tilexr_udma_demo_* | head -n1)
grep -R "signal values" "$latest"
grep -R "TileXR UDMA demo success" "$latest"
grep -R "DATA MISMATCH\\|expected non-local signals\\|TileXR UDMA demo failed\\|ERROR" "$latest" || true
```

## Optional Larger Runs

If the machine has more usable devices, repeat both test types with more ranks:

```bash
cd /path/to/TileXR/tests/udma
bash demo/run_tilexr_udma_demo.sh 0 4 64 4 0
bash demo/run_tilexr_udma_demo.sh 1 4 64 4 0
```

Expected result samples for four ranks should include:

```text
seg0=1000 seg1=1001 seg2=1002 seg3=1003
```

## Notes On Barrier Semantics

The demo intentionally does not include `shmem.h` or call shmem host APIs
directly. It uses a local TCP barrier only for host process coordination.
Device completion is covered by the demo kernel's `UDMAQuiet(args, peer)` calls
and the host `aclrtSynchronizeStream(stream)` after kernel launch.

If a future test needs a TileXR public device-side barrier API, add it through
`tilexr_api.h` instead of introducing any shmem dependency into the demo.

## Report Template

Please collect:

```text
Machine:
CANN version:
Driver version:
npu-smi info summary:
TileXR commit:

Build result:
Test 1 command and result:
Test 1 log directory:
Test 2 command and result:
Test 2 log directory:
Optional larger run result:

Any ERROR lines:
Any DATA MISMATCH lines:
Any signal mismatch lines:
```
