# vllm-ascend Collective Integration Specification

## Scope

`integrations/vllm_ascend/` provides an optional Python adapter that exposes
TileXR collectives to selected vllm-ascend paths without changing the core
runtime ABI. It is an integration shim, not a second collective implementation.

The integration owns Python-side loading, capability checks, tensor validation,
and framework adaptation. `libtilexr-collectives.so` owns the actual collective
launch and `libtile-comm.so` owns communicator state.

## Components

- `tilexr_collectives/runtime.py` loads the installed native libraries and
  translates native failures into actionable Python errors.
- `tilexr_collectives/torch_collectives.py` validates Torch/NPU tensors and
  presents the supported collective operations.
- `tilexr_collectives/vllm_adapter.py` contains framework-facing adaptation.
- `tilexr_collectives/vllm_patch.py` limits source patching to the supported
  integration boundary.
- `sitecustomize.py` provides the opt-in bootstrap used by deployment and smoke
  tests; importing unrelated Python code must not activate TileXR implicitly.

## Runtime Contract

Input tensors must be NPU-resident, contiguous where required by the native
operation, and use a supported dtype and shape. The adapter preserves the
framework stream. The vllm patch is fail-open: unsupported tensor contracts,
adapter initialization failures, missing native artifacts, and native-call
errors return control to the original vllm-ascend collective. It never copies
through CPU as a TileXR operation.

The integration reuses the application's rank identity and TileXR communicator
lifecycle. TileXR itself does not create an HCCL data path, but the original
operation selected by fallback may use the application's existing backend.
Library and Python module selection must be explicit so that a stale installed
artifact cannot be mistaken for the source under test.

## Failure Handling

- Missing libraries, symbols, unsupported tensor contracts, and adapter errors
  are retained as `last_error`/trace diagnostics when available, then fall back
  to the original operation.
- Patch/bootstrap logic is idempotent for one process and must not recursively
  wrap an already adapted operation.
- Initialization failure does not leave a partially active adapter.
- Per-operation route counters distinguish `tilexr`, `fallback`, and `error`.
- Remote deployment scripts must report the source and installed artifact
  identity used by the smoke run.

## Verification Boundary

Source-level tests cover loader, patch, and adapter behavior. The smoke scripts
exercise the installed integration on a selected environment. These checks do
not prove every vllm-ascend inference path, topology, or production workload;
claims remain scoped to the exercised operation, dtype, rank count, and host.
A TileXR-only validation must also show that every exercised operation incremented
the `tilexr` route and that no `fallback` or `error` route was used.
