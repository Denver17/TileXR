# UDMA Dual-Card External-Port Bandwidth Validation

This guide validates whether two NPUs in one server can use their independent external UnifiedBus ports concurrently. It records the measured result from the 2026-08-07 Ascend950 run and provides a reproducible procedure for other servers.

## Scope and Result

The tested source was `origin/main` commit `0096bfc` plus only the performance-probe changes described in this patch. The test used CANN `9.1.T560` on these hosts:

```text
141.61.49.195
141.61.49.198
```

Two independent two-rank communicators ran concurrently:

| Pair | Rank 0 | Rank 1 | Rendezvous |
| --- | --- | --- | --- |
| A | `.195/NPU1` | `.198/NPU1` | `141.61.49.195:12000` |
| B | `.195/NPU6` | `.198/NPU5` | `141.61.49.195:12200` |

Each NPU has eight external ports. Each port is `400 Gbit/s`, or `50 GB/s`, so the theoretical one-card and two-card transmit ceilings are `400 GB/s` and `800 GB/s`, respectively.

The four-QP route distribution was:

```text
port_count:6,port_count:6,port_count:6,port_count:2
```

This assigns three QPs to the six-port CLOS EID and one QP to the two-port CLOS EID. The resulting 3:1 traffic ratio matches the physical 6:2 port ratio.

Measured rank-0 wall-clock transmit bandwidth:

| Run | Pair A | Pair B | Aggregate |
| --- | ---: | ---: | ---: |
| Sequential single-card baselines | `389.423871 GB/s` | `389.725146 GB/s` | `779.149017 GB/s` |
| Concurrent run 1 | `389.312659 GB/s` | `389.347310 GB/s` | `778.659969 GB/s` |
| Concurrent run 2 | `389.274471 GB/s` | `389.374444 GB/s` | `778.648915 GB/s` |
| Shared-barrier verification | `389.254555 GB/s` | `389.477226 GB/s` | `778.731781 GB/s` |

The first concurrent run reached `97.33%` of the physical `800 GB/s` one-way ceiling and retained `99.94%` of the summed sequential baseline. The result is approximately twice the average single-card bandwidth. This guide uses the two rank-0 transmit rates as a one-way metric. For a bidirectional metric, adding rank 0 and rank 1 is valid, but the matching full-duplex ceiling is also doubled: one eight-port card pair has an `800 GB/s` bidirectional ceiling, and the two card pairs used here have a `1600 GB/s` bidirectional ceiling.

The shared-barrier verification was added after review to prove that the timed windows were concurrent. Pair A used `[702728885297900, 702731643754570]` ns and Pair B used `[702728885462400, 702731642342010]` ns on the `.195` steady clock. Pair B's complete `2.756880 s` window was inside Pair A's window, giving a `100%` overlap ratio. The verified aggregate reached `97.34%` of the physical one-way ceiling and retained `99.95%` of the sequential baseline sum.

Independent physical-port sampling on `.195` measured:

| Device | Eight-port TX sum |
| --- | ---: |
| NPU1 | `391124.87 MB/s` |
| NPU6 | `391070.57 MB/s` |

The wall-clock and port-counter evidence together show that both cards retain their own eight-port external bandwidth while running concurrently.

## Prerequisites

1. Use A5 / Ascend950 / 950 hardware. Host or simulator tests do not prove UDMA transfer.
2. Install the same TileXR build on both hosts and verify that the runtime and demo artifacts have matching hashes.
3. Configure passwordless MPI/SSH access between hosts.
4. Confirm the selected NPUs are idle and all target external ports are `UP`, `400G`, and `normal`.
5. Reserve two communicator ports and their demo barrier ports. The barrier is `TILEXR_COMM_ID + 97`; this procedure uses `12000/12097` and `12200/12297`. Also reserve `13600` for the optional shared performance barrier.
6. Build the UDMA demo on both hosts:

```bash
source scripts/common_env.sh
cmake -S . -B build -DCMAKE_INSTALL_PREFIX="$PWD/install"
cmake --build build -j"$(nproc)"
cmake --install build

cd tests/udma
bash build.sh
```

The following files must exist and match across hosts:

```text
install/lib64/libtile-comm.so
tests/udma/install/bin/tilexr_udma_demo
tests/udma/install/lib/libtilexr_udma_demo_kernel.so
```

## Quick Correctness Check

Run this before a throughput test. It deliberately registers exactly `2 MiB`, verifies four QPs, validates returned data and per-QP status, and exercises unregister/re-register:

```bash
cd /path/to/TileXR/tests/udma

bash demo/run_tilexr_udma_data_channel_probe_mpi.sh \
  --hosts 141.61.49.195:1,141.61.49.198:1 \
  --rank-size 2 \
  --comm-id 141.61.49.195:12000 \
  --devices 1,1 \
  --npu-count 8 \
  --test-type 0 \
  --elements 16 \
  --registered-bytes 2097152 \
  --qp-route-spec port_count:6,port_count:6,port_count:6,port_count:2 \
  --expect-qp-count 4 \
  --warmup-iters 0 \
  --iterations 1 \
  --timeout 300
```

A passing run contains these lines on both ranks:

```text
TileXRUDMARegister success
per-QP completion status: qp0=0 qp1=0 qp2=0 qp3=0
TileXR UDMA re-register lifecycle success
TileXR UDMA demo success
```

## Performance Parameters

The measured configuration uses:

```text
QP count:               4
Bytes per QP per launch: 64 MiB
Bytes per rank per launch: 256 MiB
Registered memory:      1 GiB
Warmup launches:        20
Measured launches:      4000
```

The corresponding arguments are:

```bash
PERF_ARGS=(
  --hosts 141.61.49.195:1,141.61.49.198:1
  --rank-size 2
  --npu-count 8
  --test-type 0
  --elements 16777216
  --registered-bytes 1073741824
  --qp-route-spec port_count:6,port_count:6,port_count:6,port_count:2
  --expect-qp-count 4
  --no-reregister
  --warmup-iters 20
  --iterations 4000
  --timeout 300
)
```

The performance mode prints one machine-readable line per rank:

```text
TILEXR_UDMA_PERF rank=0 device=1 qp_count=4 bytes_per_iter=268435456 iterations=4000 elapsed_ms=... tx_GBps=... wall_start_ns=... wall_stop_ns=... wall_elapsed_ms=... wall_tx_GBps=...
```

Use `wall_tx_GBps` for cross-process aggregation. `tx_GBps` uses ACL event time and is useful for device-only comparisons.

`wall_start_ns` and `wall_stop_ns` use the monotonic steady clock. Compare these fields only between processes on the same host. In this procedure, both rank-0 processes run on `.195`, so their intervals share the same host clock.

## Sequential Baselines

Run each pair separately before the concurrent test:

```bash
cd /path/to/TileXR/tests/udma

bash demo/run_tilexr_udma_data_channel_probe_mpi.sh \
  "${PERF_ARGS[@]}" \
  --comm-id 141.61.49.195:12000 \
  --devices 1,1 >baseline_a.log 2>&1

bash demo/run_tilexr_udma_data_channel_probe_mpi.sh \
  "${PERF_ARGS[@]}" \
  --comm-id 141.61.49.195:12200 \
  --devices 6,5 >baseline_b.log 2>&1
```

Require exit code zero, `TileXR UDMA demo success` on both ranks, and `qp0..qp3=0`. Extract rank-0 bandwidth with:

```bash
grep 'TILEXR_UDMA_PERF rank=0' baseline_a.log baseline_b.log
```

## Concurrent Dual-Card Run

Launch the two communicators in parallel and preserve their individual return codes:

```bash
cd /path/to/TileXR/tests/udma

bash demo/run_tilexr_udma_data_channel_probe_mpi.sh \
  "${PERF_ARGS[@]}" \
  --comm-id 141.61.49.195:12000 \
  --devices 1,1 \
  --perf-barrier-addr 141.61.49.195:13600 \
  --perf-barrier-rank-base 0 \
  --perf-barrier-size 4 >pair_a.log 2>&1 &
pid_a=$!

bash demo/run_tilexr_udma_data_channel_probe_mpi.sh \
  "${PERF_ARGS[@]}" \
  --comm-id 141.61.49.195:12200 \
  --devices 6,5 \
  --perf-barrier-addr 141.61.49.195:13600 \
  --perf-barrier-rank-base 2 \
  --perf-barrier-size 4 >pair_b.log 2>&1 &
pid_b=$!

set +e
wait "$pid_a"; rc_a=$?
wait "$pid_b"; rc_b=$?
set -e

test "$rc_a" -eq 0
test "$rc_b" -eq 0
grep 'TILEXR_UDMA_PERF rank=0' pair_a.log pair_b.log
grep 'TileXR UDMA demo success' pair_a.log pair_b.log
```

Verify that the two rank-0 timed windows overlap before adding their rates:

```bash
python3 - pair_a.log pair_b.log <<'PY'
import sys


def read_rank0_window(path):
    with open(path, encoding="utf-8") as stream:
        for line in stream:
            if not line.startswith("TILEXR_UDMA_PERF "):
                continue
            fields = dict(token.split("=", 1) for token in line.split()[1:] if "=" in token)
            if fields.get("rank") == "0":
                return int(fields["wall_start_ns"]), int(fields["wall_stop_ns"])
    raise RuntimeError(f"rank-0 performance window not found in {path}")


start_a, stop_a = read_rank0_window(sys.argv[1])
start_b, stop_b = read_rank0_window(sys.argv[2])
overlap_ns = max(0, min(stop_a, stop_b) - max(start_a, start_b))
shorter_ns = min(stop_a - start_a, stop_b - start_b)
overlap_ratio = overlap_ns / shorter_ns if shorter_ns > 0 else 0.0
print(f"overlap_ns={overlap_ns} overlap_ratio={overlap_ratio:.6f}")
if overlap_ratio < 0.95:
    raise SystemExit("timed windows overlap by less than 95%; do not aggregate this run")
PY
```

The shared barrier maps Pair A ranks to participants `0,1` and Pair B ranks to `2,3`. Participant 0 hosts the gate and releases all four ranks together. The `95%` threshold verifies that scheduling after release did not introduce material skew. If the check fails, do not aggregate the rates; inspect process scheduling and repeat the run.

Calculate:

```text
aggregate_GBps = pair_a_rank0_wall_tx_GBps + pair_b_rank0_wall_tx_GBps
stacking_efficiency = aggregate_GBps / (baseline_a_GBps + baseline_b_GBps)
physical_efficiency = aggregate_GBps / 800
```

Repeat the concurrent run at least once. A single high result is insufficient evidence of stable stacking.

## Physical-Port Sampling

Use `hccn_tool` while the concurrent job is in its measured phase. On `.195`, the validated source-port sets were:

```text
NPU1: 1/0 1/1 1/2 1/3 1/5 1/6 0/1 0/2
NPU6: 0/0 0/1 0/2 0/3 0/4 0/5 1/1 1/2
```

Sample one device per run:

```bash
sample_dev() {
  dev=$1
  shift
  for up in "$@"; do
    u=${up%/*}
    p=${up#*/}
    (
      hccn_tool -g -bandwidth \
        -i "$dev" -u "$u" -p "$p" -time 10000 2>&1 |
        sed "s|^|[NPU=$dev PORT=$up] |"
    ) &
  done
  wait
}

sample_dev 1 1/0 1/1 1/2 1/3 1/5 1/6 0/1 0/2
```

Do not sample all 16 ports concurrently with separate `hccn_tool` processes. The diagnostic interface can contend and return artificially low values. Instead, run the same dual-card workload twice: monitor NPU1 during one run and NPU6 during the other. In both runs, confirm from `wall_tx_GBps` that the unmonitored pair was also active at full speed.

## Known Boundaries

### CQ Wrap

The UDMA completion queue depth is `16384`. A run with `warmup=20` and `iterations=20000` crossed the first CQ wrap and ended with:

```text
per-QP completion status: qp0=4294967295 qp1=4294967295 qp2=4294967295 qp3=4294967295
ERROR: invalid kernel metadata or per-QP completion status
```

The transferred data still matched, but the demo correctly failed because completion metadata was invalid. Runs with `4000` and `8000` measured iterations passed. Until CQ owner/wrap handling is fixed, keep `warmup + iterations` below the CQ depth and do not claim unlimited QP reuse from this test.

### Device Health

During the recorded run, `.195/NPU6` and `.198/NPU1` reported alarm `81AF8000` (`UB RAS State, module error`). Their external links remained `UP / 400G / normal`, and use of these cards was explicitly accepted for exploratory performance validation. Treat the measured bandwidth as valid exploration evidence, not as a clean production-health certification.

### Evidence Scope

The result proves concurrent external-port bandwidth on the tested Ascend950 topology, CANN/driver environment, route selection, and payload. It does not prove UDMA data-plane behavior on 910B, a simulator, a different CLOS layout, or an arbitrary number of QPs.

## Result Checklist

- Both sequential baselines exit zero.
- Both concurrent communicators exit zero.
- Every rank reports the expected QP count.
- Every QP completion status is zero.
- Final data and debug validation pass.
- Both concurrent rank-0 `wall_tx_GBps` values remain near their sequential baselines.
- All eight ports on each source NPU carry traffic.
- No MPI, demo, or listener process remains after the run.
- Device alarms and untested boundaries are recorded with the result.
