# SDMA Transport Specification

## Scope

SDMA is an opt-in, same-device GM copy capability owned by the core
communicator. It accelerates eligible local paths but is never required for
communicator initialization or correctness.

`TILEXR_ENABLE_SDMA=1` requests initialization. `TileXRSDMAAvailable` reports
the resulting capability and `TileXRGetSDMAWorkspaceDev` exposes the device
workspace needed by the device helpers.

## Ownership and Availability

The communicator creates and destroys SDMA resources. Initialization records a
capability status instead of failing the communicator when the driver, device,
symbol, channel, or workspace is unavailable. Callers select SDMA only after
checking capability and eligibility; an unavailable SDMA path preserves the
existing communication path.

SDMA is limited to same-device transfers. It is not a cross-node transport and
must not be selected from rank topology without confirming the mapped memory and
runtime capability.

## Device Contract

`src/include/tilexr_sdma.h` provides capability checks, channel-group
resolution, nonblocking copy submission, and wait. The compatibility layer in
`tilexr_sdma_compat.h` adapts the supported CANN session interface while keeping
the TileXR call sites stable.

The caller owns source and destination bounds, channel selection, event handles,
and ordering with surrounding MTE or vector work. A submitted event must be
waited before its destination is consumed or its resources are reused.

## Failure and Compatibility

- Runtime initialization or submission failures disable or reject only the
  SDMA operation; they do not corrupt communicator state.
- Compatibility code remains constrained to CANN 9.1 and the supported driver
  surface.
- Runtime RPATH must resolve the real driver HAL and must never include the
  toolkit `devlib` directory.

## Verification Boundary

Host and source tests cover enablement, symbol handling, capability propagation,
and API guards. Device tests cover eligible copies on the exercised hardware.
See `docs/SDMA_TRANSPORT.md` for commands and current evidence. Host-only checks
do not prove device submission, completion, stress behavior, or bandwidth.
