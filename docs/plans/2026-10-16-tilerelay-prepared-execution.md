# TileRelay prepared execution

Historical execution record. Prepared ABI-v1 execution remains supported;
the [ABI-v1-only design](../specs/2026-10-19-tilerelay-v1-only-design.md)
archives the specialized Relay and pipeline controls used below at
`7ccaa16768ae2b2cb0da71d47e864352721de8b0` on
`baseline/tilerelay-v0-pipeline-20261019`. See the
[current benchmark guide](../../tools/relay/README.md) for external-baseline commands.

## Scope and contract

Implement the approved Host-overhead optimization without changing the ABI-v1
device frame, communication protocol, generated operators or existing launch APIs.
See `2026-10-14-tilerelay-core.md`, `../TILELANG_INTEGRATION.md` and repository
`AGENTS.md` for the existing contract and validation boundaries.

Add an opaque `TileXRRelayExecution` with these C entry points:

- `TileXRRelayExecutionCreate(plan, comm, bindings, bindingCount, workspace,
  workspaceBytes, executionOut)` validates and freezes addresses, context/device,
  communicator mapping and registered code; it does not consume an epoch.
- `TileXRRelayExecutionLaunch(execution, stream)` checks live plan, context/device
  and communicator mapping, obtains fresh FFTS state and epoch, launches once and
  synchronizes. It never silently rebinds or falls back.
- `TileXRRelayExecutionDestroy(execution)` releases Host state only.

Plan and executions share serialization and poison state. Destroying a plan
invalidates all of its executions; their Host handles remain safely destroyable.
An attempted device launch failure poisons every execution sharing that plan.
Creation/prelaunch rejection does not poison native state. Python conservatively
poisons the parent on any native launch error, as its existing adapter does.
No use/destruction of the same public handle may race. Separate plans do not
provide communicator concurrency isolation.

All device allocations, communicator and context are borrowed and must remain
alive and address-stable until execution destruction. Contents may change between
synchronous launches. Pointer checks cannot detect freed/reused allocations or
communicators. Context reset/destruction while executions are live is unsupported.
New addresses require a new execution. No implicit async execution or ownership.

Python adds `RuntimePlan.prepare_execution(comm, prepared)` accepting an existing
`PreparedBindings` owned by that plan, returning `RuntimeExecution` with
`launch(stream)`, `destroy()` and context-manager support. Existing APIs remain
unchanged. Preparation checks parent state and binding ownership under its lock;
execution launch and destroy use that same lock.

## Work and ownership

1. Native runtime and native tests: `src/include/tilexr_relay.h`, `src/relay/`,
   `tests/relay/test_relay.cpp`, `test_c_link.c` (built as C and C++). Factor shared
   validation/launch logic; use C++14. Verify mock tests and installed consumers.
2. Python facade and contract tests: `src/tilelang/python/tilexr_relay/runtime.py`,
   package exports and `tests/relay/test_python_contracts.py`. Depend only on the
   C/Python contract above; can proceed independently of native implementation.
3. Integration owner: extend `tests/relay/hardware.py` for prepared execution and
   maintain a same-batch comparison in `examples/tilelang/compare_relay.py`.
   Build/install Release on blue, then validate AG-GEMM, GEMM-AllReduce, changing
   inputs, repeated epochs, rank skew and mixed plans on devices 0/1.
4. Simplify/review integrated changes, rerun focused regression and update the
   maintained integration/validation documentation. Commit and update existing PR.

Parallel writers have disjoint ownership; only the integration owner commits or
pushes. Do not build on the benchmark host while timing workloads.

## Evidence requirements

Mock tests cover creation failure/output null, copied frame data, no prepare epoch,
fresh launch epochs, wrong context/device/mapping, parent-before-child teardown,
shared poison and compatibility. Python tests cover ownership, states, lock and
first-error handling. Hardware checks use existing kernel ELF hashes, warmup and
repeated measurements, with correctness outside timed regions. Compare old
prepared bindings and new execution using the same Release DSO, and retain the
specialized and serial baselines. Record raw rank samples, build flags, DSO/ELF
hashes and profiler state; do not derive Host segments from cross-clock timestamps.
No UDMA, multi-node or unsupported-hardware qualification is implied.
