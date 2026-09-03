# TileXR MoonEP Benchmark

The native five-stage API, tensor contracts, status ranges, and validation
boundaries are documented in `docs/specs/moonep-runtime.md` and
`docs/specs/moonep-combine.md`.

This benchmark runs the complete protocol with TileXR bootstrap and no HCCL. Expert
Forward has one implementation: BF16 `torch_npu.npu_grouped_matmul`,
`torch_npu.npu_swiglu`, then `torch_npu.npu_grouped_matmul`:

```text
Planning -> Dispatch -> PrefetchWeight -> expert forward -> Combine
         -> Dispatch(saved Plan) -> expert backward -> Combine(saved Plan)
         -> ReduceGrad
```

That statement applies to `--mode benchmark`. The tool also provides two untimed
correctness modes:

- `--mode reference` runs all five native stages through the independent baseline V1
  Torch reference (`torch_npu_reference_v1`) on NPU tensors, executes the shared
  Torch-NPU Expert Forward between PrefetchWeight and Combine, and uses Torch
  distributed for cross-rank data. One-rank-per-NPU launches use HCCL; oversubscribed
  two-ranks-per-NPU launches use Gloo with CPU staging because HCCL rejects repeated
  local device IDs.
- `--mode correctness` runs the reference and the built-in TileXR candidate backend after
  independently cloning every stage input, then compares every defined output and
  required/forbidden mutation before proceeding. It uses Gloo with CPU staging so the
  reference collective can coexist with TileXR registered UDMA.

The default candidate is `tools.moonep.tilexr_backend:create_backend`. It requires an
installed five-stage native TileXR library, `transport_correctness_valid=true`, exact
launcher/context dimensions, and a same-node topology. An explicit
`--candidate-backend MODULE:FACTORY` overrides the built-in adapter. External factories
receive keyword arguments `torch_module`, `dimensions`, `case`, and `args`, and must
return an object satisfying `tools.moonep.contracts.MoonEPBackend`.

## Correctness Pipeline

The backend protocol remains five native MoonEP stages. The correctness runner adds an
untimed Expert Forward checkpoint, so every rank emits six stage artifacts:

```text
Planning:
  topk_experts [S,K] int32, tokens_per_expert [E] int32
  -> dst, cu_seqlens, experts_to_copy, zero_fill_ranges, remote_stats
Dispatch:
  plan, hidden [S,H] bf16, optional route weights [S,K] fp32
  -> hidden [NvS,H], optional weights [NvS], semantic dedup plan
PrefetchWeight:
  plan, gate/up/down weights [E+B,...] bf16
  -> the same tensors updated in place
ExpertForward:
  dispatched hidden [NvS,H], cu_seqlens [E+B], prefetched weights, weights [NvS]
  -> GMM1 [H,2I], NPU SwiGLU, GMM2 [I,H], route weighting, hidden [NvS,H]
Combine:
  plan, expert output [NvS,H] bf16, optional weights [NvS] fp32
  -> hidden [S,H], optional weights [S,K]
ReduceGrad:
  plan, full grads [E+B,...] fp32, reduce buffers [R,B,...] fp32
  -> owner rows accumulated and consumed live slots cleared in place
```

Every stage checks input structure and values, output structure and values, input
immutability or required in-place effects, and rank-wide agreement. Planning fields are
exact; Dispatch compares the `cu_seqlens`-defined region and semantic dedup sets;
padding is zero; Prefetch and route weights are exact. Expert Forward uses the defined
Dispatch prefix, zero-fills the remaining `NvS` capacity, and is compared with BF16
tolerance; Combine hidden output uses the same tolerance. The undefined Dispatch tail
after the final `cu_seqlens` end is reported but not compared as semantic output.

Run the CPU/fake-backend suite without NPU hardware:

```bash
python -m pytest tests/moonep/python -q
```

## Standalone PrefetchWeight Benchmark

`prefetch_weight_benchmark.py` compares only the PrefetchWeight stage. It uses
eight physical ranks by default in the validated EP8 case: 32 experts, four
experts and four remote slots per rank, BF16 `H=7168`, and `Hf=2048`. Each slot
copies three 28 MiB projections, so one rank transfers 336 MiB per measured
iteration.

Build and install the MoonEP targets, then run TileXR with the Python module
launcher. The `torchrun` executable can contain a stale Conda shebang on shared
hosts, so it is intentionally not used here.

```bash
source scripts/common_env.sh
export TILEXR_INSTALL_PREFIX="$PWD/install"
export PYTHONPATH="$PWD/integrations/moonep_torch:$PWD:${PYTHONPATH:-}"
export TILEXR_COMM_ID=127.0.0.1:10067

python -m torch.distributed.run --standalone --nproc-per-node=8 \
  tools/moonep/prefetch_weight_benchmark.py \
  --backend tilexr \
  --install-prefix "$TILEXR_INSTALL_PREFIX" \
  --output-dir output/prefetch-weight-tilexr \
  --warmup 5 --iterations 20 --repeats 3 \
  --tilexr-commit "$(git rev-parse HEAD)"
```

Run the native MoonEP comparison with the same case and iteration counts:

```bash
python -m torch.distributed.run --standalone --nproc-per-node=8 \
  tools/moonep/prefetch_weight_benchmark.py \
  --backend native \
  --native-root /path/to/ascend-moonep \
  --output-dir output/prefetch-weight-native \
  --warmup 5 --iterations 20 --repeats 3 \
  --native-commit "$(git -C /path/to/ascend-moonep rev-parse HEAD)"
```

Both backends initialize and validate exact BF16 slot contents before timing.
Each sample uses NPU events around only the stage launch, and rank 0 aggregates
the slowest rank for every iteration. Allocation, memory registration, barriers,
correctness checks, and status reads are excluded. Raw per-rank samples and the
aggregate are retained in `rank_<rank>.json` and `summary.json`. Event readings
at or below 1 us remain in `cross_rank_max_us` for diagnosis but are excluded
from `valid_cross_rank_max_us`, P50, P99, and effective bandwidth.

The validated Ascend950 results and known experimental dead ends are recorded in
`docs/moonep/PREFETCH_WEIGHT_PERFORMANCE.md`.

## Public API NPU E2E Tool

`test_npu_e2e.py` is a standalone base test tool for the upstream-compatible
`Buffer` API. Run it directly from the repository root; it launches eight local
distributed workers with the current Python interpreter:

```bash
python tools/moonep/test_npu_e2e.py
```

Use `--nproc-per-node`, `--master-addr`, and `--master-port` to override the
single-node launcher defaults. The same file can run as a worker script under an
existing launcher:

```bash
torchrun --nproc-per-node=8 tools/moonep/test_npu_e2e.py
```

The tool requires `torch`, `torch_npu`, the `tilexr_moonep` integration package,
an installed TileXR runtime, and at least five ranks for its duplicate-free remote
PrefetchWeight route. It exercises Planning, Dispatch, PrefetchWeight, ExpertForward,
Combine, and ReduceGrad through the public API. The native integration selects
Combine V2 and consumes Planner V3's reverse `dstLocal` route map.

Run the Torch-NPU/HCCL reference on four devices:

```bash
python tools/moonep/run_benchmark.py \
  --mode reference \
  --cases tools/moonep/cases/correctness.json \
  --world-size 4 \
  --physical-device-count 4 \
  --output-dir output/moonep-reference
```

Add `--dump-stage-tensors --tensor-preview-elements 8` to a reference or correctness
run to persist every stage boundary. Each rank writes complete CPU snapshots to
`rank_<rank>/tensor_dumps/<stage>/<reference|candidate>/<input|output>.pt`, plus a
matching human-readable `.txt` file containing every value without ellipsis, plus a
JSON manifest with shape, dtype, original device, element count, and the bounded
preview. Workers also write `tensor_dumps/preview.log`; the managed launcher
echoes rank 0's preview after the case. Tensor dumping is rejected in `benchmark` mode
so snapshot copies cannot enter native performance measurements.

Run a one-rank differential case first to isolate installation, capability, and stage
status failures:

```bash
ASCEND_RT_VISIBLE_DEVICES=4 python -m tools.moonep.launcher \
  --mode correctness \
  --cases tools/moonep/cases/correctness.json \
  --case-ids planning-no-dedup \
  --world-size 1 --physical-device-count 1 \
  --install-prefix "$TILEXR_INSTALL_PREFIX" \
  --output-dir output/moonep-correctness-1r
```

Then run the same fixed-padding single-route differential case on four physical devices:

```bash
bash scripts/run_moonep.sh --mode correctness --rank-size 4
```

For `reference` and `correctness`, this script enables stage tensor snapshots by
default and reports the directory and `.pt/.json/.txt` file counts. It runs
`planning-no-dedup`; use `--case-id manual-small` for reference-only hand verification. Use
`--tensor-preview-elements COUNT` to control terminal preview length or
`--no-dump-stage-tensors` for summary-only runs.

Pass `--generate-flowcharts` to the script for an opt-in six-stage visual report. The
report module loads each rank's reference `input.pt` and `output.pt`, writes numbered
left-to-right `.mmd` sources to `<result>/<case_id>/flowcharts`, and the script renders
matching `.svg` and 2x `.png` files with Mermaid CLI. Correctness runs deliberately use
the reference role rather than candidate values. The option requires tensor dumps,
rejects benchmark mode, and runs a minimal `mmdc` browser render before launching NPU
work. Both npm/Puppeteer and Python/Playwright CLI parameter forms are supported. The
PyPI CLI and Playwright browser are installed and validated by
`bash scripts/prepare.sh`; the direct browser command remains
`python -m playwright install chromium`.

`--case-id manual-2rank-imbalanced --rank-size 2` provides a compact load-balancing
case with one route per token and no duplicate destination. Initial owner loads are
`[4,2]`, so Planner can exercise remote expert placement without repeated token routes.

`--case-id manual-2rank-topk-2 --rank-size 2` and
`--case-id planning-4rank-topk-4 --rank-size 4` retain compact multi-route coverage.
`planning-8rank-topk-8` and `planning-16rank-topk-16` extend the same coverage to all
eight physical NPUs. Their `unique_destinations` routing assigns every TopK slot of a
token to a different logical owner rank, so `K > 1` does not create duplicate token
traffic. `skewed-no-dup` keeps a single-route hot-expert case for four-rank
load-placement coverage.

`planning-8rank-single-route` and `planning-16rank-single-route` share
`S=8,K=1,E=16,H=8,Hf=4,P=1`, with the required `B=2` and `B=1` capacities. These
cases isolate rank and collective scaling from the `K=8` and `K=16` Planning paths.

For the 16-rank case, pass all eight physical NPUs and `--rank-size 16`. The launcher
sets `ranks_per_device=2` and binds ranks `d` and `d+8` to device `d`; this
oversubscribed mode is functional-only and must not be used for performance claims.
The runner assigns one-based numbers in JSON order: `--case-id 8` and `9` select the
large-rank TopK cases, while `--case-id 10` and `11` select their single-route pairs.
Canonical string IDs remain accepted and are always used for artifact directory names.

`planning-64rank-single-route` and `planning-128rank-single-route` are append-only IDs
12 and 13. They use `S=8,K=1,H=8,Hf=4,B=1,P=1` with `E=64/128`, respectively,
and run as eight ranks per node with one rank per NPU. The 64-rank case uses eight nodes;
the 128-rank case uses sixteen. `run_moonep.sh` requires the same master endpoint,
launch ID, and output path on every node and records node-local global-rank artifacts.
Merge those artifacts before aggregation.

Append-only ID 14, `planning-16rank-16card-single-route`, reuses
`S=8,K=1,E=16,H=8,Hf=4,B=1,P=1` for two nodes with eight physical NPUs each. Its
runner topology is `rank_size=16`, `rank_per_dev=1`, unlike IDs 9 and 11, which place
two logical ranks on each of eight devices. The script usage reports these two topology
values alongside every case's MoonEP parameters. Distributed `benchmark` requires the
TileXR communicator and authenticated barrier environment, `reference` requires the
Torch distributed master endpoint, and `correctness` requires both; all three use the
full six-stage flow when launched through `run_moonep.sh`.

All cases exposed by `run_moonep.sh` set token padding to 1. Duplicate-destination
routing patterns remain available to focused reference tests but are intentionally
excluded from this runner matrix.

The script selects devices starting at 0 by default and owns the HCCL NPU socket
range, so the normal command requires no environment exports. Use
`--visible-devices 4,5,6,7` or
`--hccl-npu-socket-port-range 47200-47300` when a shared host needs explicit
isolation for a one-rank-per-NPU HCCL run.

Reference and correctness modes initialize HCCL or Gloo on a launcher-reserved
rendezvous port separate from TileXR bootstrap and teardown rendezvous. Their summaries
always record `performance_valid=false`; reference collective work is never included in
native timing.

Planning, Dispatch, PrefetchWeight, Combine, and ReduceGrad are native A5 stages.
ExpertForward is not a TileXR native stage or ABI: it is the Torch-NPU compute step
between the native PrefetchWeight and Combine calls. There is no precomputed,
pure-Python, quantized, or native TileXR GMM fallback.
Reports expose native transport correctness separately from performance closure:
`transport_correctness_valid=true` and `transport_performance_valid=false`. Normal runs use
`performance_scope=native_correctness_only`; 16-rank/8-device runs use
`performance_scope=oversubscribed_functional_only`.

Optional route weights use the same replaceable ABI as hidden data:
Dispatch maps `[S,K]` weights to `[NvS]`, forward and backward both consume the
dispatched weights, and Combine maps them back to `[S,K]`. Both weight stages
are native and bit-exact. Hidden transport supports BF16 only; combine reduces
K routes in FP32 and casts once to BF16. Timings remain non-performance evidence
until the precision matrix and msprof profiling gates are completed.

## Environment

Build and install TileXR with the MoonEP options, source the CANN environment,
and make the integration package importable:

```bash
source scripts/common_env.sh
export TILEXR_INSTALL_PREFIX="$PWD/install"
export PYTHONPATH="$PWD/integrations/moonep_torch:$PWD:${PYTHONPATH:-}"
```

The launcher sets `RANK`, `WORLD_SIZE`, `LOCAL_RANK`, a shared
`TILEXR_COMM_ID`, and the planner-group variables for every child. Pass
`--comm-id <rank0-ip:unused-port>` when `127.0.0.1` is not reachable by all
ranks. The native facade, rank worker, and launcher all default the bounded
Planner ready-poll budget to 1,000,000 iterations. After each case, every rank
synchronizes its local NPU stream and joins a TCP completion rendezvous before
destroying the TileXR communicator, so peer windows remain alive until all
Planner kernels have completed. The launcher gives each run a public launch id
and authenticates each case with an HMAC secret that is never sent or written
to artifacts. Teardown is released only after every rank proves local
quiescence. Any case failure is propagated to all ranks; an unquiesced worker
keeps its communicator open for launcher-coordinated termination.

## 8 Physical NPUs

```bash
python tools/moonep/run_benchmark.py \
  --cases tools/moonep/cases/smoke.json \
  --output-dir output/moonep-8p \
  --physical-device-count 8 \
  --ranks-per-device 1 \
  --comm-id 127.0.0.1:10087
```

One rank per device defaults `TILEXR_MOONEP_PLANNER_BLOCK_DIM=64`.

## 16 Logical Ranks on 8 NPUs

```bash
python tools/moonep/run_benchmark.py \
  --cases tools/moonep/cases/smoke.json \
  --output-dir output/moonep-16r-8p \
  --physical-device-count 8 \
  --ranks-per-device 2 \
  --comm-id 127.0.0.1:10088
```

The launcher maps `logical_rank % 8`, defaults the planner block dimension to
32, and records `oversubscribed=true`. This validates logical-rank indexing and
call ordering; it is not 16-device throughput evidence. An explicit planner
block dimension must satisfy `planner_group_size <= blockDim <= 64`.
Before each Planning launch, this artificial two-process-per-device mode uses
the authenticated host rendezvous to align otherwise independently scheduled
Torch processes. The rendezvous runs before device-event timing; Planning data
and device synchronization still use TileXR peer memory.

The current direct Planner is bounded to 64 ranks per planner group. Global and
lane metadata can describe 256P/512P/1024P layouts; a 128-rank lane at 1024P is
represented by two planner groups until the hierarchical Planner is implemented.

The current MoonEP native stages are same-node IPC-only and use TileXR peer windows;
they do not select UDMA even when the core communicator exposes that capability.
Cross-node MoonEP is rejected before stage launch. Core communicator UDMA probing is
best-effort and any initialization failure leaves the IPC path available.

On Ascend950, same-node IPC whitelist selection defaults to PID-only.
`TILEXR_IPC_PID_MODE=pid` can force that mode for diagnostics.

## Artifacts

Each case contains `rank_<rank>/samples.jsonl`, `rank_<rank>/result.json`,
`summary.json`, `summary.csv`, and `stage_summary.csv`. The original raw event metrics
remain in `summary.json` and `summary.csv`. Benchmark mode defaults to five untimed
warmup iterations and 20 measured iterations. `--warmup` and `--iterations` accept
non-negative values, but their sum must be at least one. Warmup iterations are not
written to `samples.jsonl` and do not contribute to performance statistics. For each
measured iteration, the six-stage report first combines forward/backward calls inside
each rank, then selects the slowest rank for that logical stage. Mean values are the
arithmetic mean of those per-iteration critical-rank samples. Communication bytes and
algorithm bandwidth use that same critical rank. Planning and Expert report bandwidth
as `N/A`. When `iterations=0`, the run is warmup-only and every performance field is
`N/A`. Aggregation fails when
rank case, capability, stage execution, topology, sample count, timing metric metadata,
or communication byte metadata differs. After normal benchmark aggregation, the script
automatically prints the complete case and runtime inputs above one horizontal table as
the final terminal block. Its left columns identify whether
each logical stage is a TileXR native stage and the concrete Kernel/API version; timing,
algorithm bytes, and algorithm bandwidth remain joined on the right. The same native
and version fields are present in `stage_summary.csv`. The build line reports both the
source Git SHA and whether tracked source files differ from that revision.

Reference/correctness cases instead contain `rank_<rank>/stages/<case>.<stage>.json`,
optional `rank_<rank>/tensor_dumps/`, `rank_<rank>/result.json`, and `summary.json`.
A failed artifact identifies the stage,
tensor, first mismatch index, expected/actual values, error maxima, valid-region mask,
and mutation failure when applicable. No timing CSV is generated for these modes.
