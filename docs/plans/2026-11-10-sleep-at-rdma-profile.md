# Sleep-at followed by profiled RDMA WRITE

Approved scope: on two ranks, enqueue `TileXRClockSleepAt` followed immediately
on the same stream by a single-AIV test kernel. Each rank sends 64 distinct
4 KiB WQEs to its peer through one QP and calls `RDMAWait`. Record only kernel
entry/exit with the existing TileXR profiling SDK. Keep this in diagnostics
integration tests, without a new production API.

1. Add an external test kernel and Host helper to the existing clock E2E
   executable. Prepare registration, memory and capture before submission;
   verify source, destination, guards, status and the 64-WQE producer delta.
2. Export SDK records and a frozen drift-model sidecar. Extend the test's
   offline analyzer to render a Perfetto trace on one rank-zero cycle axis,
   retaining raw timestamps and model-relative accuracy limitations. Never
   independently zero the two ranks or manufacture Host-clock calibration.
3. Validate the exporter against different device epochs and a nonzero entry
   skew, compile Host/kernel contracts, and review resource lifetimes and QP
   ownership. Once pipeline WRITE begins, do not issue legacy clock traffic
   on that QP again.
4. Commit/publish the candidate, build in the protected environment, then run
   the approved 84 devices 0/2 test with idle checks and bounded own-process
   cleanup. Export the requested timestamp-only trace for user inspection.

Retain the same immutable candidate for build, runtime and trace provenance.
The trace establishes alignment in the frozen drift model's reference domain;
it is not an independent measurement of physical cross-device clock accuracy.
