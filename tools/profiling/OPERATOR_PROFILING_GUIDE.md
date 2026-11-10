# Operator profiling

Current integration guides: [English](docs/USER_GUIDE.md) /
[简体中文](docs/USER_GUIDE.zh-CN.md). Both cover standalone installation, named
Kernel events, Host capture, offline analysis, and validation boundaries.

Use the [Add4 example](../../examples/profiling/README.md) as a deliberately minimal runnable
reference. See [API](docs/API_REFERENCE.md), [formats](docs/FORMAT.md), and
[advanced workflows](docs/ADVANCED.md) for contracts and optional capabilities.

## EP Memory and ServerDedup

Use public `<tilexr/profiling.h>` and build ordinary Memory/ServerDedup with
`-DTILEXR_EP_ENABLE_PROFILING=ON`. Ordinary Memory records two kernel body events
per lane (IDs 1/2); ServerDedup appends its existing detailed event catalog with
`DirectWriter` and publishes once. Use the matching operator mapping, including
conditional events. The two-event body mapping does not describe ServerDedup.
ElasticMemory compact retains its separate SDK stage selection; it does not use
the deleted private Memory dump format.

Before creating the communicator, the driver supplies a managed plan through
`TILEXR_PROFILING_SLOTS_FILE`. It enumerates every invocation, including unselected
warmup calls, and assigns each sampled call an independent UUID and rank output
path. The communicator owns SDK captures and the GM descriptor/lane-count tables;
kernels select a descriptor by communication magic. Those slots remain part of
the current integration. See the [EP plan contract](docs/API_REFERENCE.md#ep-invocation-slot-integration).
UniTest generates plans for its ordinary Memory flow. Detailed native ServerDedup
drivers must provide their matching plan and business mapping.

The integration defaults to eight records per lane. Configure
`TILEXR_PROFILING_RECORDS_PER_STREAM` before initialization: only decimal multiples
of eight in **8…65536** are accepted with an active plan. Start detailed captures
with **512 or more** records per lane and budget for all selected calls; verify
complete expected ranks/lanes, valid counts and zero drops. An undersized capture
is a failed diagnostic, not a successful truncated result. Standalone SDK users
still use CaptureConfig and its separate default of 4096.

Private 512B dumps, old compatibility writer/owner/encoder, forwarding headers,
reserve APIs and legacy output environment settings are removed. Rebuild Host,
all consumers of CommArgs/tiling and external kernels together after this ABI
change. Do not reuse an old binary because a build cache option matches.

For performance, build diagnostics OFF and preserve warmup, Host stage barriers
and framework/external profiler settings. The cluster runner supports
`functional` and `performance`; node scripts support `correctness` and `perf`.
Old `breakdown`/`profile` options return parameter errors and have no compatibility
alias. SDK stage measurements include instrumentation cost and cannot replace
msprof Task Duration. Business timeout checks and communication completion fences
remain necessary even when profiling is disabled.

These are integration and validation requirements, not new CANN or hardware
qualification results. Verify the target timer conversion before reporting time
units and keep each report restricted to one invocation UUID.
