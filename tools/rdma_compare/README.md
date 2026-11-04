# Matched QP1 AIV WRITE comparison

This harness prepares matching TileXR and NDA workloads. CPU validation and
compilation do not qualify NPU data transfer or bandwidth.

The default case is one QP, one SGE, 512 WQEs, unidirectional traffic, cyclic
source/destination addresses, MTU4096, GID1, independent UB DMA, hard doorbells,
batch64, signal every batch end and at most 128 WQEs in flight. Each sample uses
fresh processes/QPs. Source and destination spans are disjoint. `streaming`
instead advances both addresses by payload+64; `reverse` swaps sender direction;
`duplex` prepares a simultaneous-traffic plan, whose directional values and sum
must be reported separately. Its execution is rejected until a device start
coordinator establishes overlap: a Host barrier cannot qualify short-packet
overlap. Initial execution modes are forward/reverse. Payload sizes use powers
of two, 512B through 1MiB.

TileXR's `RDMAPutPipeline` is opt-in and single-SGE. It reserves a stop entry,
publishes the WQE tail before the valid head using MTE3, and rings through
`st_dev` after publication. The 4608B scratch contains a 4096B descriptor batch
and separate stop/CQE/record storage. Both the control CR and task signal bits
are cleared on unsignaled descriptors. CQ consumers and WR ordinals are distinct.
The mode uses reserved state words without changing structure size; old APIs
reject a pipeline-tagged QP. A drained legacy QP may enter the pipeline, but a
fresh QP is required for matched samples. Full-call SQ/CQ no-wrap admission occurs
before posting. Errors retain outstanding ownership and poison the QP.

Build the Host library first, then:

```sh
cmake -S tools/rdma_compare -B build-rdma-compare \
  -DTILEXR_ROOT="$PWD" -DTILEXR_INSTALL="$PWD/install"
cmake --build build-rdma-compare -j4
```

`bench ... --describe` prints its effective layout before any ACL call. Actual
execution requires `--run`. `RDMA_BENCH_CPU_ONLY` blocks execution. The executable
is deliberately a single measurement per QP; repetitions belong to the harness.
It calibrates the cycle counter against ACL events before measurement, checks
full source/destination/guard contents after both ranks finish, and records
directional kernel timing separately from Host launch-to-sync times.

An optional final `NUM_SGE` argument selects 1/2/4/8/16 segments:

```sh
bench RANK DEVICE BYTES COUNT DIRECTION ADDRESS_MODE KERNEL_ELF --run NUM_SGE
```

`BYTES` is fixed **per SGE**, so each WQE sends `BYTES * NUM_SGE`. Multi-SGE
sources have 64B gaps in reverse physical order and gather into one continuous
remote span. Their source and target strides differ. Cyclic slot count is based
on the aggregate bytes per WQE. Logs retain both byte counts and both strides;
reported Gb/s uses every segment's payload, excluding gaps and descriptor bytes.
SGE1 preserves the old layout and invocation. The multi-SGE pipeline requires
20992B UB, accounts SQ space in 64B blocks, and keeps CQ identities in WR units.
Rebuild the Host and pure AIV ELF together when the launch ABI changes.
`compare.py` remains the matched single-SGE TileXR/NDA harness; multi-SGE
experiments need a controller that passes the final argument to both ranks and
checks their complete case agreement and received data.

The reference NDA build needs the opt-in matched controls described in its
`docs/nda-matched-benchmark.md`; ordinary upstream builds are incompatible.
`compare.py` passes the same count/address mode to both peers and alternates
backend order. It defaults to writing `plan.json`, with no process launches:

```sh
python3 tools/rdma_compare/compare.py \
  --tilexr build-rdma-compare/bench --kernel build-rdma-compare/bench_kernel.o \
  --nda /path/to/matched/ib_write_bw --devices 0,2 --nics nic0,nic1 \
  --payload all --direction forward --address cyclic --output comparison-plan
```

On the authorized Linux host, execution additionally requires `--execute`,
timezone-aware `--window-start/--window-end`, and `--idle-check-script`,
`--inventory`, `--guard-devices`. The inventory must have one `ssh: local`
environment covering exactly the authorized pool. The checker must report every
device idle immediately before each process pair. No workload starts with fewer
than 150 seconds left in the window. The runner never terminates other workloads.
Do not execute it during a compile-only authorization window.

Every process log, effective resource record, occupancy result, case and binary
hash is retained. Generic NDA report lines can aggregate duplex traffic: use its
per-sender kernel sample rather than treating the aggregate as a direction.
Historical Excel points belong to a different run unless their physical path,
WQE count, address pattern, direction and timing have been matched explicitly.

The matched NDA build must also acknowledge full source/destination/guard
verification and peer success on both processes after the timed kernel. A
sender timing record with `payload_verified=0` is accepted only when the later
`RDMA_BENCH_VERIFY` record confirms the complete extent, role, address mode,
direction, all verification flags and `status=PASS`. Exit status or CQ success
alone is insufficient. The runner observes its own NDA server's LISTEN socket
through `/proc` before starting the client; it never probes by connecting to
the one-shot control socket. This avoids a client startup race without
consuming the server's only accept.

If QP RTR fails before any WQE, preserve its provider return value and errno,
and inspect the selected NIC's GID and neighbor resolution before changing a
kernel. A stale or failed neighbor is an environment boundary. Scoped test
neighbor changes must retain/restore the prior entries, and neither a neighbor
entry nor RoCEv2 selection proves packets traversed an L3 router. Preserve
independent forwarding evidence when reporting routed-network qualification.

CPU checks are `ctest --test-dir build-cpu`: the new pipeline model validates
encoded descriptors, address sequences, tail publication ordering, sparse
completion identities, admission, errors and state ownership; the socket control
test exercises real two-process TCP and production transport exchanges with a
CPU provider. Neither establishes device DMA/cache/MMIO correctness. Fake ACL
fixtures must keep assertions enabled in Release because their cleanup contracts
are asserted; they must never resolve to real ACL during CPU tests.

## Multiple independent QPs

`multiqp` measures WRITE with 1/2/4/8/16 independent communicator-owned QPs in
one provider/PD. It initializes domain 1 with `TileXRCommInitRankWithDomain`,
checks device-direct capability, and collectively calls `TileXRRDMAConfigureQps`
before registering memory. Domain 0 remains the separate phase/case socket.
Set `TILEXR_ENABLE_RDMA=1`, `TILEXR_RDMA_MODE=aiv`,
`TILEXR_RDMA_DMA_MODE=indep_ub` and `TILEXR_RDMA_DB_MODE=hard` for execution.
It launches one AIV per QP in one pure AIV kernel, with the same
batch64/CQ64/max128 pipeline per QP:

```sh
multiqp RANK DEVICE BYTES COUNT_PER_QP DIRECTION QPS KERNEL_ELF --describe
multiqp RANK DEVICE BYTES COUNT_PER_QP DIRECTION QPS KERNEL_ELF --run
multiqp RANK DEVICE BYTES COUNT_PER_QP DIRECTION QPS KERNEL_ELF --run NUM_SGE
```

Directions 0 and 1 select separate unidirectional tests. This executable does
not run duplex traffic. Every QP sends `COUNT_PER_QP` WQEs, so total payload is
`QPS * COUNT_PER_QP * BYTES * NUM_SGE`. Omitting `NUM_SGE` preserves SGE1;
the optional value selects 1/2/4/8/16 entries, each containing exactly `BYTES`.
The kernel uses `RDMAWrite` / `RDMAWriteSge` for QP1 and their `MultiQp` wrappers
with the owning AIV's index for multiple QPs. `RDMAWriteOptions` carries counts,
strides, batch/in-flight limits, cyclic slots and poll budget;
`RDMAWriteScratchBytes(NUM_SGE)` sizes each AIV's private UB scratch.
The SGL path retains reversed, scattered sources, separate source/target strides
and 20992B scratch per AIV. SQ admission and
producer/completed counters use `COUNT_PER_QP * WqeBlocks(NUM_SGE)` blocks,
plus a reserved stop block; CQ counters stay in completion units (8 for 512 WR).
For example, QP16 with SGE8 and 1MiB per entry transfers 64GiB per sample.
Aggregate Gb/s uses that volume divided by the
common-clock interval from the earliest QP start to the latest QP completion.
Summing individual QP bandwidths can overstate performance when their intervals
differ. Logs retain all intervals, start skew and overlap; calibration checks
the common cycle envelope against the ACL event. Host/event time remains a
separate metric. Use actual QP queue depths for no-wrap admission.

Each QP uses an independent payload pattern and full source, destination and
guard verification. Both peers agree on cases and failures before progressing.
Each QP keeps a small, separate buffer/MR registered through `TileXRRDMARegister`.
Every MR is valid on all configured QPs: `TileXRRDMAGetDeviceViews` requires the
full host array with exactly `QPS` entries. For MR `q`, the benchmark selects
entry `q` for the kernel's view array; it never constructs endpoints, keys or
queue descriptors. One AIV owns each selected queue across every MR and stream.
Local allocation and collective registration use separate agreed phases so a
local allocation failure cannot strand the peer in registration. Cleanup
synchronizes streams, collectively calls `TileXRRDMAShutdown`, then destroys the
communicator before freeing payloads. A cleanup failure retains allocations.
SGL QP tags use a multiplier distinct from the SGE entry tag: sharing one would
let a swapped QP/entry pair cancel under XOR and hide misrouting. CPU tests check
tag uniqueness, cross-QP corruption, ordered gathering and the complete MR.
`RDMA_BENCH_CPU_ONLY` also guards this executable before ACL initialization.
Repetitions require fresh processes and QPs. Compare another implementation
only with explicit per-QP WQE counts, scheduling, address and timer semantics;
using different hosts does not constitute a controlled same-host A/B test.
