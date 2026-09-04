# Grouped AllToAll Baseline Send Restoration

> Historical implementation note. The standalone grouped API/library ownership
> described by the original work has been superseded by
> `2026-08-31-aiv-urma-grouped-alltoall-design.md`. The kernel protocol remains
> relevant, but production dispatch is now internal to `TileXRAllToAll`.

## Goal

Restore the hardware-validated grouped AllToAll send protocol in the production
the grouped operator under `src/collectives`. The supported contract remains multi-route,
single-pass, `quietBatch=1`, Credit/Receive/SDMA enabled.

## Protocol

For every non-empty route, the send core uses one selected QP and publishes:

1. one payload WRITE WQE with completion;
2. one 8-byte signal WRITE WQE with ordered completion;
3. one doorbell after both WQEs are in the SQ;
4. one quiet through the signal WQE completion frontier.

Both WQEs are assembled completely in UB and copied to the SQ with MTE3. The
doorbell is rung with `st_dev` only after both MTE3 copies complete. Direct GM
construction is unsafe here: it does not provide the publication ordering that
the device consumes, and larger peer schedules can expose that as a vector-core
exception before a device status record is visible.

The signal WQE, rather than an MTE write after quiet, makes payload visibility
and readiness one ordered transport operation. Receive cores poll the registered
signal plane. Credit remains in the communicator's dedicated IPC Credit buffer.

The caller-owned input is the payload source. A narrowly scoped profile device
helper accepts this raw local GM address while retaining the selected QP's local
registration token. Existing profile APIs keep their strict local-region checks.
Signal sources, remote payload destinations, and remote signals are all inside
the registered workspace.

## Workspace

The aligned registered workspace contains, in order:

- payload plane 0 (`bytesPerRank`);
- payload plane 1 (`bytesPerRank`);
- signal plane 0 (`rankSize * 2 routes * 512 bytes`);
- signal plane 1 (same size);
- per-send-core signal source slots;
- device status.

Invocation parity selects one payload/signal plane. Checked arithmetic rejects
overflow. Signal slots are cache-line isolated and the workspace remains
512-byte aligned.

## Routing

Peer ordering is unchanged, including the 128-rank local-8/opposite-8 schedule.
The fixed shared-QP profile maps each lane to one six-port primary QP and one
two-port secondary QP. Intra-node peers use only the primary route; cross-node
payloads follow the profile's 6:2 weights. This changes neither the
communicator's QP configuration nor its QP count.

## Validation

Focused tests cover checked layout, plane isolation, raw-local profile posting,
payload-before-signal SQ order, completion flags, one deferred doorbell frontier,
and unchanged peer coverage. Hardware validation runs trace-off first at 2x8,
8x8, and 128 ranks with warmup 5/repeat 50 and bounded timeouts, followed by a
diagnostic trace run. The comparison target is baseline `b4f03a5862b6`:
approximately 3678 us and 291.9 GB/s at 128 ranks, 1 GiB/rank.
