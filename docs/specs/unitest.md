# UniTest Architecture and Execution Contract

UniTest runs installed TileXR EP Memory operators, validates outputs, measures
Host batch timing, and coordinates optional profiling and remote execution.
The [user guide](../../tools/unitest/README.md) covers installation and commands;
[validation records](../validation/unitest.md) identify tested revisions and
remaining hardware qualification gaps.

## Components and ownership

| Component | Responsibility |
|---|---|
| Python Controller | Resolve configuration, validate cases, assign ranks, coordinate phases, enforce deadlines and aggregate results |
| C++14 Worker | Own one device, stream, communicator, buffers and public operator calls |
| EP Memory adapters | Prepare deterministic inputs and independent reference outputs for Dispatch, Combine and their chain |
| NodeAgent | Manage explicitly assigned local Workers through a bounded remote control channel |
| Reporting | Retain resolved inputs, numerical results, measurements, library provenance, tool outcomes and partial failures |
| TileXR production targets | Build and install Host libraries, communicator/profiling wiring and device objects |
| External analysis tools | Export official profiler data or analyze generic SDK captures |

Framework code consumes the public communicator, EP and type headers. It does
not implement communication kernels. The internal capture binding belongs to
production communicator code; the Controller supplies the capture plan and
invokes the existing analysis tools without decoding their binary records.

## Build and installed layout

`TILEXR_BUILD_UNITEST` defaults to ON in the root build. It includes core,
diagnostics and EP dependencies even when `TILEXR_BUILD_EP=OFF`. Worker links
the current `tilexr-ep` and `tile-comm` targets. `BUILD_TESTING` and
`TILEXR_BUILD_TESTS` do not control this tool. Core-only builds explicitly set
`TILEXR_BUILD_UNITEST=OFF` and disable other unneeded modules.

Standalone configuration uses `tools/unitest` as its top-level project and reads
an existing installation through `TILEXR_INSTALL_PREFIX`, optional
`TILEXR_LIBRARY_DIR`, and the selected CANN SDK. It builds no production target.
Both modes use C++14 Host code and the header-only picojson submodule.

Installation provides:

```text
bin/tilexr-unitest
libexec/tilexr-unitest/tilexr_unitest_worker
share/tilexr-unitest/{controller,cases,configs,README.md}
```

The launcher resolves its files relative to its own location. Worker has no
fixed TileXR runtime RPATH; the caller selects compatible libraries through
`LD_LIBRARY_PATH`. Host libraries resolve their installed device objects.
The complete production installation, real driver HAL and compatible CANN must
be available at runtime. SDK `devlib` stubs are for link-only checks and must
not enter runtime loader paths. Compilation alone does not establish NPU support.

## Cases and configuration

Cases use `tilexr.unitest.case.v1`. Current adapters accept `memory`, `bf16`,
`quant_mode=0`, BS 1..256, H 16..4096 divisible by 16, TopK 1..8, and 1..128
experts. Expert count divides across the global rank count with at most 32
experts per rank. TopK cannot exceed expert count. Combine and the chain also
require `BS * TopK >= experts` for the supported full-coverage workload.

Local selection accepts 2..8 distinct devices. Explicit remote placement accepts
2..128 global ranks with equal positive device counts per host. These parser
limits are not evidence of hardware qualification at every accepted scale.
EP execution currently requires A5/Ascend950; core runtime coverage does not
extend an adapter's supported hardware.

Run configurations use `tilexr.unitest.run.v1`. Explicit CLI fields override
config fields, which override defaults. Store resolved values and field sources
in `run.json` and `result.json`. Reject unsupported fields, invalid counts and
incompatible combinations before Worker launch. Changing iteration counts or
capture selection does not require recompilation.

## Execution modes and numerical checks

All ranks advance through initialization, preparation, execution, validation
and release. Any missing or failed rank fails the complete run.

- `single` executes and validates once.
- `correctness` executes cumulative segments and validates at powers-of-two
  checkpoints plus the final iteration, or at every iteration when requested.
  Stop at the first failed segment/checkpoint and preserve the last passing one.
  Equal fixed inputs mean checkpoint correctness does not prove every
  intermediate iteration or detect every stale-but-equal result.
- `perf` validates a precheck, warms up, then measures batches. Each batch has
  preparation outside timing, complete-rank readiness, repeated public API calls,
  final stream synchronization and post-batch numerical validation.

Dispatch checks payload, assist and counts against Host references. Standalone
Combine uses independent Host inputs with exactly representable BF16 reference
values. The chain binds Dispatch device payload/assist/counts directly to Combine
on the same stream, keeping borrowed buffers alive through completion. It
validates the final `combine.output`; it does not independently validate every
intermediate output. One measured chain iteration includes both public API calls.

`ep_dispatch_barrier` is recognized but returns unsupported: no applicable public
device Barrier is available. Host readiness or stream synchronization must not
be presented as that device operation.

## Host batch measurement

Worker uses `std::chrono::steady_clock` around repeated calls and their final
successful stream synchronization. `host_batch_average_us` divides elapsed time
by iterations. It includes submission overhead, device work/wait and submission
gaps; it is not Kernel Duration or a per-call latency distribution.

Retain each rank/batch sample, per-batch rank median/min/max and maximum-rank
summaries. Do not subtract timestamps across hosts or treat independently started
rank batches as an exact global makespan. Host API throughput does not establish
physical-link bandwidth. Allocation, preparation, control messages, serialization,
validation and report writing remain outside the measured batch.

## Profiling integration

Profiling is optional and numerical results remain separate from tool outcomes.
Tool failure still fails the complete requested run.

### Official msprof

Worker uses the ACLprof lifecycle; after execution, the framework invokes
`msprof --export=on --output=<raw-directory>`. It does not parse CSV/SQLite or
maintain a separate Trace generator. Only all-round selection and empty custom
options are supported.

For single/correctness, capture starts before the first execution. For perf, it
starts after precheck/warmup and stops after the final batch validation. Therefore
the capture includes intervening preparation and validation, not just measured
calls. Official export success does not establish sample completeness.

### Internal generic captures

Host and Kernel must be built and installed together with
`TILEXR_EP_ENABLE_PROFILING=ON`. Before initialization, the Controller creates
fixed-slot plans for the requested actual invocations. Production communicator
code allocates SDK captures and binds their descriptors once; EP entry points
record generic begin/end events using the invocation magic.

All-round and explicit 1-based round selection are supported. Perf selection
covers measured calls only, excluding precheck and warmup. Chain stages have
independent captures. Each invocation's UUID must agree across its participating
ranks; missing or incomplete captures and exhausted budgets fail visibly.
The configured budget defaults to 64 MiB per rank/run and is capped at 1 GiB.

A communication domain must submit serially on one execution stream, without
unplanned magic-consuming calls. Capture collection occurs while the relevant
stream/context are still valid. The existing `tilexr_profiling` tool owns record
analysis and reports. Original EP v2 stage instrumentation remains distinct;
generic begin/end does not claim full internal-stage visibility.

`cycles_per_us` and `clock_basis` are required, device-specific evidence. Nominal
or uncalibrated values do not justify accurate stage latency or cross-rank clock
alignment. Instrumentation overhead remains part of measurements with capture on.

## Remote placement and failure handling

The POSIX/WSL Controller reads SSH addresses from the maintained tssh inventory;
placement selects known hosts, devices, installed framework prefixes, environment
scripts, runtime library directories and output roots. All nodes must already
have compatible installations. No source synchronization, deployment or remote
production rebuild is performed.

Ranks follow host-major/device order. The communicator endpoint is a data-network
IPv4/port reachable from every node and bindable by rank zero. Control messages
carry run identity and sequence; NodeAgents own only their assigned Workers.
Do not infer network correctness from protocol fixtures or a single-host SSH run.

Initialization, phases and exit waits are bounded. EOF, interruption and timeout
terminate owned processes and retain available evidence; no automatic replay or
cleanup of unrelated workloads occurs. Network loss can leave remote cleanup
unconfirmed, which must be reported rather than assumed successful. Keep buffers
and communicators alive through completion; if completion is unknown, do not
classify device resources as safe to reuse.

## Validation and maintenance

Check CLI/config contracts with Host tests, and separately qualify builds,
installed execution, numerical checks, profiling and cleanup on the actual target
environment. Host fixtures do not prove NPU execution, remote data-plane transfer,
GM visibility or profiler completeness. Hardware work follows the device access
and communication-environment rules in `AGENTS.md`.

Maintain one current execution contract here and keep historical revision hashes,
machine paths and stage-specific evidence in [validation records](../validation/unitest.md).
The [earlier implementation proposal](2026-11-03-unitest-redesign.md) and
[implementation plan](../plans/2026-11-04-unitest-clean-implementation.md) are
historical context, not replacements for current CLI behavior.
