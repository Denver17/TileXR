# Data-As-Flag Device Protocol Specification

## Scope

`src/include/tilexr_data_as_flag.h` provides header-only Ascend C helpers for a
payload protocol in which data blocks carry the completion value used to detect
arrival. It is intended for algorithms that can reserve flag-bearing bytes in
their data layout and provide UB scratch explicitly.

## Contract

The sender initializes scratch, copies payload into the protocol layout, writes
the payload and flag-bearing blocks, and publishes them using the caller's
ordered data-movement pipeline. The receiver checks the expected flag value and
copies only a complete batch into its destination.

The public helper groups are:

- sizing helpers for scratch, aligned values, flag bytes, and batch limits;
- `DataAsFlagInit` and payload staging helpers;
- `DataAsFlagSend`;
- batch and single-value check helpers;
- `DataAsFlagCheckAndRecv`.

The caller owns GM storage, UB tensors, expected values, retry policy, and
inter-round generation. A later round must use a value that cannot be confused
with stale data from an earlier round.

## Invariants

- Sizes and addresses satisfy the alignment required by the Ascend C data-copy
  and vector operations used by the helpers.
- Scratch capacity is validated before a batch is processed.
- A receiver never exposes a partially arrived batch as complete.
- Send and receive buffers do not overlap in a way that invalidates pipeline
  ordering.
- The protocol does not reset shared GM as a substitute for generation
  management.

## Failure and Verification

Invalid sizing returns a bounded failure result where the API provides one;
callers must not continue with an oversized batch. Header compilation and source
guards cover API shape and key ordering constraints. They do not prove remote
visibility or multi-rank progress on hardware.
