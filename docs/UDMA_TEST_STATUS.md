# UDMA Test Infrastructure Status

**Updated:** 2026-09-01
**Status:** Registered-memory and peer-memory alias data planes validated on Ascend950PR

## Summary

The current UDMA acceptance path validates TileXR-owned registered-memory communication. It no longer depends on standalone shmem tests.

The test tree verifies:

- `src/comm` does not include or link shmem;
- UDMA layout structures match the device-side expectations;
- registered-memory metadata and remote address calculations are correct;
- a TileXR communicator can expose `CommArgs`;
- AICore demo kernels can issue UDMA put and put-with-signal on A5 / Ascend950 / 950 hardware.
- an owner rank's original IPC allocation can be UDMA-registered in the opt-in
  peer-memory mode and consumed through another local rank's IPC alias.

## Peer-Memory Alias Result

On 2026-09-01, the three-rank peer-memory alias probe passed on host
`C01-A10-CPU2` (`141.61.49.223`) using Ascend950PR devices 0, 1, and 2, CANN
`9.1.T560`, and driver `25.1.rc1.b188`. Eight generations completed on every
rank. For each generation, the producer UDMA-wrote a 4 KiB payload and ready
token into the owner's registered `peerMem_[owner]`; the third rank acquired the
ready token through a distinct `peerMems[owner]` IPC mapping and verified every
payload byte.

The debugging boundary was cache visibility, not UDMA registration: CQ
completion and Host reads through both IPC aliases showed the correct token,
while repeated MTE2 token polling remained stale. DCCI followed by scalar
`GlobalTensor::GetValue` provided the required control-token acquire. This
result covers same-server UDMA-to-IPC visibility only; it does not validate an
uneven cross-node placement.

The review follow-up added an explicit cache acquire before reusing the payload
slot. The source contract check covers this guard, but the follow-up cache path
has not been rerun on Ascend950 hardware.

## Test Files

```text
tests/udma/
|-- build.sh
|-- run_tests.sh
|-- unit/
|   |-- test_tilexr_udma_transport_layout.cpp
|   `-- test_tilexr_udma_registry.cpp
|-- integration/
|   `-- test_tilexr_udma.cpp
`-- demo/
    |-- tilexr_udma_demo.cpp
    |-- tilexr_udma_demo_kernel.cpp
    |-- run_tilexr_udma_demo.sh
    |-- README.md
    `-- ASCEND_VERIFICATION.md
```

## Host-Only Checks

```bash
cd /path/to/TileXR/tests/udma
bash build.sh
./install/bin/test_tilexr_udma_transport_layout
./install/bin/test_tilexr_udma_registry
```

Expected:

```text
TileXR UDMA transport layout checks passed
TileXR UDMA registry checks passed
```

## Communicator Smoke Test

```bash
cd /path/to/TileXR/tests/udma
source ../../scripts/common_env.sh
export LD_LIBRARY_PATH="$PWD/install/lib:../../install/lib:${LD_LIBRARY_PATH:-}"
RANK=0 RANK_SIZE=1 ./install/bin/test_tilexr_udma
```

Expected:

- exit code 0;
- communicator initialization succeeds;
- `TileXRGetCommArgsHost` succeeds;
- output reports `Failed: 0`.

This is a smoke test only. It does not prove UDMA data-plane transfer.

## A5 / Ascend950 Runtime Demos

All-gather style put:

```bash
cd /path/to/TileXR/tests/udma
bash demo/run_tilexr_udma_demo.sh 0 2 16 2 0
```

Put with signal:

```bash
bash demo/run_tilexr_udma_demo.sh 1 2 16 2 0
```

Expected:

- every rank log contains `TileXR UDMA demo success`;
- every rank log contains `TileXRUDMARegister success`;
- all-gather samples include per-rank segments such as `seg0=1000 seg1=1001`;
- put-signal logs show non-local signal entries equal to `1000`;
- no log contains `DATA MISMATCH`, `expected non-local signals`, `TileXR UDMA demo failed`, or `ERROR`.

## Hardware Scope

UDMA runtime validation is only valid on A5 / Ascend950 / 950 hardware.

910B, 310P, and other non-A5 devices may still be useful for build checks or communicator smoke checks, but they must not be reported as successful UDMA runtime validation targets.

## References

- [tests/udma/README.md](../tests/udma/README.md)
- [tests/udma/QUICKSTART.md](../tests/udma/QUICKSTART.md)
- [tests/udma/demo/ASCEND_VERIFICATION.md](../tests/udma/demo/ASCEND_VERIFICATION.md)
