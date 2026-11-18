# Combine with independent completion flags

Scope: the user's requested replacement of DataAsFlag in the previously simulated
Memory Combine snapshot. Keep the existing kernel ABI and numerical operation;
validate FP16 at 32 ranks, 384 experts, BS96, H7168, TopK6 and 64 AIVs.

Reference: CANN `mc2/moe_distribute_combine_v2/op_kernel/arch22/moe_distribute_combine_v2.h`
at the user-provided GitCode URL, retrieved 2026-11-18 (SHA-256
`cf8371caa6721dfa7336e3a41dd27deea7322679aebde604ccd6de550aef928c`). Its send pipeline writes
raw payloads, drains MTE3 at each batch boundary, then publishes separate flags.

The TileXR implementation uses 32-byte-aligned contiguous payload slots followed
by a separate array of 32-byte completion records in the Combine reservation.
Keeping completion flags within that reservation avoids Dispatch's count area.
The unchanged Dispatch reservation is larger than raw payload plus one flag per
slot. Sender batches are bounded by the available UB slots. Raw FP16 uses
MTE2-to-MTE3 events and reuses a slot only after its MTE3 completes. Quantized
template variants retain their existing conversion and use the same new wire
protocol. The receiver waits for all route flags of a token, reads contiguous
payloads, combines them, and clears the flags before window reuse.

Before sending, each rank clears its current completion array. A fixed-address
initialization table in the unused state-window gap carries the launch generation;
all ranks must finish clearing before any sender may publish data or flags.
This prevents a changed BS/H/TopK from interpreting old payload bytes as ready flags.

1. Implement the kernel protocol and matching Host layout/UB accounting.
2. Update focused layout tests for raw rows, separate flags, tails, quantized
   layouts and capacity rejection. Compile the exact candidate with CANN 9.1
   for dav-c310-vec and inspect compiled variants.
3. Reuse the previous independent numeric fixture/oracle in a new simulation
   case with the archive `lib_v1.0_1113.tar.gz`. Record candidate source, binary,
   input, library and configuration hashes.
4. Run bounded SST, validate every output rank and generate native Chip0 trace.
   Check raw payload writes, independent flags and data-before-flag completion.
   A trace with failed numerics remains diagnostic, not accepted performance.

The previous DataAsFlag trial completed but failed numeric validation with a
systematic output high-byte pattern. Preserve that evidence and independently
report whether this candidate reproduces it. Simulator implementation changes
are outside this protocol change. An independent review checks the synchronization
and window lifecycle. Green runs bounded exact scripts through the existing cross-zone bridge.
