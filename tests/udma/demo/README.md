# TileXR UDMA Communication Demo

This demo shows TileXR-initialized UDMA communication with verbose diagnostics. UDMA means UnifiedBus DMA and this runtime path currently targets A5 / Ascend950 / 950 hardware. The host demo uses TileXR public APIs, registers ordinary `aclrtMalloc` memory with `TileXRUDMARegister`, and the AICore kernel uses `tilexr_udma.h`.

## Persistent Profile Hardware Probe

`tilexr_udma_profile_probe` is the Ascend950 stop-gate for persistent multi-MR
profiles. It registers one legacy region alongside four 2 MiB-aligned profile
regions, binds every QP to local staging and one of three remote sources, and
issues four deferred READ WQEs per batch. The per-WQE sizes are 48 KiB, 256 KiB,
1 MiB, 2 MiB, 4 MiB, 8 MiB, and 16 MiB.

The probe kernel is a pure AICore binary. The Host executable registers it with
`rtDevBinaryRegister` and `rtFunctionRegister`, then launches it with
`rtKernelLaunchWithFlagV2`.

```bash
cd tests/udma
bash build.sh
bash demo/run_tilexr_udma_profile_probe_mpi.sh \
  --hosts 141.61.53.106:1,141.61.53.110:1 \
  --comm-id 172.27.12.106:10167
```

The runner retains the combined log and JSONL timing records under
`tests/udma/logs/`. `wqe_bytes` is the size of one UDMA copy and `batch_bytes`
is the four-WQE aggregate. The launcher uses MPI when `mpirun` is available and
otherwise starts one bounded SSH process on each host; use `--launcher` to
select either mode explicitly.

## Peer-Memory Alias Hardware Probe

`tilexr_udma_peermem_probe` proves that a UDMA write to an owner rank's original
IPC allocation is immediately consumable through another local rank's
`peerMems[ownerRank]` mapping after an explicit ready-token acquire. It requires
exactly three ranks on one Ascend950/A5 server: producer, owner, and local IPC
consumer.

```bash
export MPI_HOME=/path/to/mpich
bash demo/run_tilexr_udma_peermem_probe_mpi.sh \
  <host> <host> <host:port> <tests/udma/install> 8
```

The final argument is the generation count. Success requires all three ranks to
report the requested count and `TileXR UDMA peer-memory alias probe success`.
The consumer also verifies every byte of every 4 KiB payload. This probe is a
same-server visibility test; uneven cross-node rank placement is unsupported by
the current `localRankSize` calculation.

## Build

```bash
cd /path/to/TileXR/tests/udma
bash build.sh
```

The demo target requires `bisheng`. If `bisheng` is not available, `build.sh` still builds the existing UDMA tests and reports that `tilexr_udma_demo` was skipped.

## Run

```bash
cd /path/to/TileXR/tests/udma
bash demo/run_tilexr_udma_demo.sh 0 2 16 2 0
bash demo/run_tilexr_udma_demo.sh 1 2 16 2 0
```

Arguments:

```text
run_tilexr_udma_demo.sh <test_type> <rank_size> <elements_per_rank> <npu_count> <first_npu>
```

- `test_type=0`: all-gather style UDMA put.
- `test_type=1`: UDMA put with signal.
- `rank_size`: number of local ranks to launch.
- `elements_per_rank`: `int32_t` elements in each rank segment.
- `npu_count`: number of NPUs available to this run.
- `first_npu`: first physical NPU id to use.

Each run writes per-rank logs under `tests/udma/logs/tilexr_udma_demo_*`.

For a true cross-node data-channel probe, build and install the demo on every host, then launch all ranks as one MPI job:

```bash
bash demo/run_tilexr_udma_data_channel_probe_mpi.sh \
  --hosts 141.62.19.156:4,141.62.19.108:4 \
  --rank-size 8 \
  --comm-id 141.62.19.156:10067 \
  --devices 0,1,2,3 \
  --npu-count 4 \
  --first-npu 0 \
  --test-type 0 \
  --elements 16 \
  --warmup-iters 5 \
  --iterations 20
```

Run again with `--test-type 1` to validate UDMA put-signal. Add `--require-sdma` if the test should also fail when TileXR SDMA is unavailable. This probe validates the cross-node UDMA/SDMA data-channel prerequisites independently from the EP memory and UDMA backends.

Use `--warmup-iters <N>` to run unmeasured warmup launches and `--iterations <N>` to select the measured launch count. Their defaults are `0` and `1`, respectively, which preserves the original single-launch behavior. The same values may be supplied through `TILEXR_DEMO_WARMUP_ITERS` and `TILEXR_DEMO_TIMED_ITERS`.

Performance output includes `wall_start_ns` and `wall_stop_ns` from the steady clock. When adding throughput from independently launched communicators, compare intervals from ranks on the same host and require their measured windows to overlap; adding two non-overlapping rates does not prove concurrent bandwidth.

To align independent MPI jobs, give every job the same `--perf-barrier-addr` and `--perf-barrier-size`, and assign non-overlapping participant ranges with `--perf-barrier-rank-base`. The barrier is optional and only affects performance mode.

For a reproducible two-card, eight-port-per-card throughput procedure and measured results, see [UDMA Dual-Card Bandwidth Validation](../../../docs/UDMA_DUAL_CARD_BANDWIDTH_VALIDATION.md).

Run this demo only on A5 / Ascend950 / 950 hardware. Builds or smoke tests on other Ascend chips are not valid UDMA runtime validation.

## What To Check

Each rank prints:

- process id, rank, selected device, `TILEXR_COMM_ID`, and `LD_LIBRARY_PATH`
- TileXR `CommArgs` host/device pointers
- `extraFlag`, UDMA enable bit, and `udmaInfoPtr`
- `peerMems[]` IPC pointers for comparison
- registered data and signal buffer addresses
- kernel debug words
- result samples and signal values

The host demo process does not include `shmem.h` or call shmem APIs directly. Its process-level synchronization uses a TCP barrier derived from `TILEXR_COMM_ID` with a demo-only port offset. Set `TILEXR_DEMO_BARRIER_ADDR=<ip[:port]>` when ranks on other hosts must connect to the rank-0 barrier server.

The demo exits with an error if TileXR initializes without UDMA. In that case, verify that the machine is A5 / Ascend950 / 950 hardware, the Ascend driver and CANN runtime are configured, and the TileXR runtime libraries in `LD_LIBRARY_PATH` match the build under test.

UDMA buffers are allocated with ordinary `aclrtMalloc` and registered through `TileXRUDMARegister`; TileXR IPC `peerMems[]` are printed for diagnostics but are not used as UDMA transfer targets.
