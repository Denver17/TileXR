# EP Combine Memory Profiling Design

## Status

- Date: 2026-08-23
- Status: Implemented and validated on the target 64P hardware case
- Baseline: `2026-08-23-ep-combine-memory-commercial-baseline-migration-design.md`
- Target case: 64P / 384 experts / BS 96 / H 7168 / K 6 / BF16 / no quantization

## Objective

Add compile-time profiling to EP Combine Memory by following the current MoonEP Combine V2 model:

- fixed-index absolute system-cycle time points;
- fixed-index accumulated metrics for repeated work;
- missing time points filled with the previous timestamp at record publication;
- one aligned record per AIV core;
- one GM publication after the timed Kernel work;
- Host D2H only after all timed iterations;
- Chrome Trace export for the fastest, P50, and slowest ranks.

Profiling is diagnostic. Production latency is measured with profiling disabled.

## Compatibility Boundary

The existing 19-field Kernel ABI is unchanged. Profiling does not add a Kernel argument or change communication-domain construction.

The feature is compiled only when `TILEXR_EP_COMBINE_MEMORY_ENABLE_PROFILING` is enabled. With the option disabled, profiling members, `GetSystemCycle()` calls, metric accumulation, and profile GM writes are excluded from the Kernel binary.

No synchronization is added to make an asynchronous operation easier to time. A point or metric reflects only synchronization already present in the commercial-baseline flow.

## Record ABI And Address

Each AIV writes one 512-byte record, matching `kEpMemoryDispatchDfxStride`. The record contains a marker and version, rank/core metadata, the launch block count, the 64-bit current generation, 40 timestamp/diagnostic slots, and 16 accumulated metric slots.

The profile address is fixed and does not ping-pong:

```text
profileBase = TileXREpMemoryRoundDataBase(peerMems[localRank], kEpMemoryCombineProfileBuffer)
            + kEpMemoryDispatchDfxOffset
record(core) = profileBase + core * kEpMemoryDispatchDfxStride
```

`kEpMemoryCombineProfileBuffer` is fixed to buffer 1. The fixed region is `[768 KiB, 868 KiB)` in that buffer. Dispatch Memory profiling uses the corresponding DFX region in buffer 0, so the two profilers cannot overwrite each other. Dispatch Memory workspace ends at the start of this region, cumsum state begins at its end, and Combine payload starts at 1 MiB.

`generation` validates that the fixed record belongs to the final successful launch. It is not used for address selection and the Host does not inspect both ping-pong rounds.

## Time Point Schema

All records publish the same schema and `timePointCount`. A core may skip a stage, but it does not change index meanings. At publication, an unrecorded time point is replaced with the previous time point so records remain monotonic.

| Index | Name | Boundary |
| ---: | --- | --- |
| 0 | `INIT_BEGIN` | first cycle captured in `Init` |
| 1 | `STATIC_INIT_END` | context, GM tensors, and attributes initialized |
| 2 | `REUSE_WAIT_END` | reserved compatibility point; no reuse wait in combine |
| 3 | `SEND_COUNT_SPLIT_END` | send count read and per-core split completed |
| 4 | `SEND_BUFFER_INIT_END` | sender UB reset and queues initialized |
| 5 | `ASSIST_LOAD_END` | assigned assist rows available in UB |
| 6 | `SEND_ISSUE_END` | assigned payload writes issued |
| 7 | `DISPATCH_END` | existing MTE3 drain completed |
| 8 | `RECEIVE_BUFFER_INIT_END` | receiver UB reset and buffers initialized |
| 9 | `RECEIVE_PREPARE_END` | scales and local receive state prepared |
| 10 | `FIRST_TOKEN_READY` | first assigned token passed flag validation |
| 11 | `ALL_TOKENS_READY` | last assigned token passed flag validation |
| 12 | `LOCAL_COPY_END` | receive/reduce/clear/output loop completed |
| 13 | `FINAL_BARRIER_END` | reserved compatibility point; no final retirement barrier |
| 14 | `RETIRE_BEGIN` | reserved compatibility point; no round retirement |
| 15 | `RETIRE_END` | reserved compatibility point; no round retirement |
| 16 | `FINAL_END` | final timed point before record publication |

The unused timestamp capacity stores fixed diagnostics: assigned send rows, assigned receive tokens, flag polling attempts and misses, packed blocks per token, and selected send/receive queue counts.

## Accumulated Metric Schema

Repeated work uses MoonEP-style `BeginProfileMetric` and `EndProfileMetric`. Metrics accumulate cycles in class-local state and are written only at final publication.

| Index | Name |
| ---: | --- |
| 0 | `assist_load` |
| 1 | `dispatch_input_load` |
| 2 | `dispatch_pack` |
| 3 | `dispatch_store_issue` |
| 4 | `scale_load` |
| 5 | `flag_prepare` |
| 6 | `flag_load` |
| 7 | `flag_compare_scan` |
| 8 | `payload_load` |
| 9 | `reduce_issue` |
| 10 | `flag_clear_issue` |
| 11 | `output_cast_store_issue` |

Metrics ending at an asynchronous GM store are explicitly named `issue`; profiling must not add a completion barrier.

## Collection Contract

`tests/ep/demo/tilexr_ep_dispatch_demo.cpp` owns Device-to-Host collection. Collection is enabled only when `TILEXR_EP_DEMO_COMBINE_MEMORY_PROFILE=1` and is restricted to standalone memory Combine performance runs. `TILEXR_EP_COMBINE_MEMORY_PROFILE_RUN_ID` is mandatory in that mode and accepts 1-128 letters, digits, `-`, `_`, or `.` characters.

Warmup and timed iterations are submitted exactly as in the existing performance flow. After the final stop event completes, the Host:

1. obtains the local `CommArgs`;
2. copies the fixed 200-record DFX region to Host once;
3. reads `blockDim` from core 0 and validates every active record;
4. validates marker, version, rank, core, generation, counts, and monotonic timestamps;
5. prints one `EP_COMBINE_MEMORY_SAMPLE` line per rank and one `EP_COMBINE_MEMORY_PROFILE` line per active AIV core using the schema consumed by the offline trace exporter;
6. enters the existing cross-rank barrier even when local collection fails, so one malformed record cannot strand the other ranks in the collector path.

The C++14 collector receives the region into a byte buffer and copies each record into a properly aligned local object before field access. It does not rely on pre-C++17 `std::vector` support for over-aligned element allocation.

Only the final iteration is retained because every launch overwrites the same fixed per-core record. For a fresh standalone Combine communicator, magic begins at 1, so the expected final 64-bit generation is `warmupCount + loopCount`.

Each `EP_COMBINE_MEMORY_PROFILE` line must provide the following whitespace-separated fields:

```text
run_id iteration rank world_size bs h k experts dtype quant_mode
comm_quant_mode active_mask_type core block_dim generation profile_version cycles_per_us
point_count metric_count t0..t16 m0..m11
send_rows receive_tokens flag_poll_attempts flag_poll_misses
blocks_per_token dispatch_buffer_num receive_buffer_num
```

The offline exporter is invoked after all rank logs have been collected:

```bash
python tools/ep/combine_memory_trace.py tests/ep/logs/RUN/rank_*.log \
  --world-size 64 \
  --run-id RUN \
  --bs 96 \
  --hidden-size 7168 \
  --topk 6 \
  --experts 384 \
  --dtype bf16 \
  --quant-mode 0 \
  --comm-quant-mode 0 \
  --active-mask-type 0 \
  --output-dir tests/ep/logs/RUN/traces \
  --prefix ep_combine_memory
```

The exporter rejects incomplete rank/core sets, block counts above the fixed 200-record DFX capacity, mixed generations, mixed run IDs unless `--run-id` selects one, and any record whose logged shape or mode differs from the command-line provenance. `blockDim` is validated within each rank but is not required to match across ranks: devices in one 64P domain may expose different vector-core counts. Each Chrome Trace file carries the validated provenance in `otherData`.

The unified cluster entry point
`tests/ep/cluster/run_memory_dispatch_msprof_64p.sh --op combine --mode breakdown`
builds with `TILEXR_EP_COMBINE_MEMORY_ENABLE_PROFILING=ON`, distributes the built
artifacts, runs the profile case, gathers all rank logs, and invokes the exporter.
Functional and performance modes explicitly use profiling `OFF`; attribution and
production performance therefore remain separate runs.

## Rank Selection And Chrome Trace

For each rank, Kernel duration is the largest `FINAL_END - INIT_BEGIN` across its active AIV cores. Ranks are sorted by `(duration, rank)`.

- fastest: first sorted rank;
- P50: index `ceil(0.5 * rankCount) - 1`, which is index 31 for 64P;
- slowest: last sorted rank.

The trace exporter writes three independent Chrome Trace Event JSON files. Each file contains one selected rank, one thread per active AIV core, one complete event for every adjacent time-point pair, and one instant event carrying the accumulated metrics. It intentionally does not add a full-Kernel envelope event, so phase events are not covered by a redundant parent event. Each rank is normalized to its earliest core `INIT_BEGIN`; absolute cycles are not compared across NPUs.

## Validation

Static and local validation should cover:

- record size and DFX-region static assertions;
- compile-option and source-contract checks;
- parser validation for missing ranks/cores, inconsistent schema, stale generation, and non-monotonic timestamps;
- synthetic 64P trace export and fastest/P50/slowest selection.

Target validation requires a profiling-enabled 64P run followed by a profiling-disabled A/B performance run. Profiling results attribute time but do not replace the production performance result.

On 2026-08-23, profiling `ON` and `OFF` configurations compiled successfully with CANN 9.1.T555 and 13/13 EP CTests passed. The target 64P BF16/no-quant balanced case passed on all ranks. The profiling-disabled run averaged 305.735 us per iteration; the profiling run produced generation-120 records for all ranks and exported fastest/P50/slowest traces. Ranks 10 and 48 launched 72 AIV cores while the other ranks launched 64, which established the per-rank `blockDim` validation rule above. Profiling timestamps attribute the final iteration only and do not replace the profiling-disabled performance result.
