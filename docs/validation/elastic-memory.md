# ElasticMemory implementation and A5 validation

This records the single-owner segmented-ring implementation of
[ElasticMemory design](../elastic-memory.en.md) ([中文](../elastic-memory.md)).
The development session owns implementation, independent review, execution and evidence;
the design session checks the submitted results against the design. This is **partial
implementation of the complete design**, not completion of every acceptance item.

The user-approved single-owner segmented ring and 2.5 us allocation target are
implemented and tested on CANN 9.2. The current measurements, retained boundaries
and reproduction commands are in the November 9 section below.

## Supported behavior and boundaries

| Design contract | Implementation / evidence |
|---|---|
| Device calculates actual size | Production Memory Dispatch computes receive prefixes, then allocates one hidden+assist bundle on Device. No Planner or Host size oracle is used by this entry. |
| Original kernel waits for real Host growth | M0 resident-kernel experiment and production allocator hardware tests; current kernel consumes a new HBM address without replay. |
| Failed growth terminates the call | Budget failure reaches both ranks; Host bookkeeping fault after successful HBM allocation returns Device RuntimeFailure, frees the unpublished block and does not replay. Runtime/device failures without confirmed completion quarantine ownership; no recovery/retry. |
| FIFO | One native service stream and admission queue per pool; multiple submitted allocation calls complete in order. One active routing handle per Torch buffer. |
| Initial data HBM zero | Pool starts with control metadata only (6,208 reported bytes with ring ABI v2). First nonempty call creates a segment. Zero-sized output does not allocate data. |
| Dynamic return | Hot calls reuse; larger calls grow; cache-budget collection and explicit trim invoke real segment free after lease/events finish. Live views keep stable addresses. |
| Ordinary Tensor / Storage | Installed torch_npu `from_blob`, ordinary FP16/BF16 ND tensors, no output copy; alias, registered stream, pending input owners and close-with-live-Storage tests. Inference only; `requires_grad` rejected. |
| Dispatch + Combine | Real two-rank Memory transport and existing Combine with independent routing/value oracle. UDMA direct output, multi-host, quantized/shared-expert/TP paths are outside this implementation. |
| Asynchronous API | Submit runs independently of Python GIL. **Pending::Wait/materialize waits full data completion**, not metadata-only readiness; Combine also completes before returning. Early metadata materialization is deferred. |
| Version support | Native C++14; Torch extension C++17 from installed Torch. This feature is validated on CANN **9.2.0**, A5/Ascend950DT, satisfying the user-scoped version acceptance. Other CANN versions are not promised and are not outstanding requirements. 910B, training/autograd and graph capture are not supported by this delivery. |
| Performance | Final owner allocation approximately 1.301–1.671 us; OFF complete pool Dispatch 13.905 / 92.332 us versus maximum output 19.503 / 93.645 us. Full distributions, clock uncertainty and tails are below; Host/growth are separate. |

Core limits are 32 segments, 128 outstanding reservation records (including
retired records blocked behind a live prefix), and 128 registered consumer
streams/events per lease. Exceeding metadata capacity fails explicitly. The
allocator does not relocate live segments to compact memory.
The caller must keep device context and registered streams alive through retirement.

## Device header layout and file descriptions

The Device allocator and profiling helpers are now installed as
`include/device/tilexr_elastic_memory_device.h` and
`include/device/tilexr_elastic_memory_profile.h`; consumers include them with the
`device/` prefix. Host API and shared ABI stay directly under `include/`.
CMake kernel dependencies and installation rules follow the moved source paths.
The module's file descriptions explain callers and confirmed ownership without
language labels; the repository-wide convention is in `AGENTS.md`.

Layout source **`fc8499455e22690329b49a6f85bccb8a47da2986`** changes comments,
include paths and packaging only. The complete operator measurements below are
evidence for **`d847c22956966e4e0805981ba5039fad894e047b`**; they are not new timing
measurements for the layout revision. Build and install checks use a separate
A5 CANN 9.2 tree under
`/home/h30059441/elastic-memory-dev-20261106/headers-20261109/`;
local logs are under
`/workspace/TileXR-worktrees/elastic-memory/.elastic-memory-evidence/headers-20261109/`.
Validation uses CANN OFF full build, STAGES compact-kernel build, a clean install,
and external Host/Device consumers compiled with only the installed TileXR include
root plus CANN headers. The strict C++14 allocator test and Python AST comparison
check the unaffected implementation. No NPU task or HCCL qualification was run.
`configure-off-final.log`, `build-off-final.log`, `build-stages.log`, `install.log`,
`installed-header-smoke-final.log` and `strict-allocator.log` retain the commands
or outputs; `provenance.json` pins this layout revision separately from performance.
The layout manifest contains 426 files; its SHA-256 is
`d0e1324b46d899a60e06d05acbe0a95f73a0e39060b0697bf8a473c0004bf09e`.

## Segmented-ring Device and complete-kernel measurements (November 9)

The final Device pool Dispatch rank-median aggregate is **13.905 / 92.332 us**,
compared with the same-batch maximum-output reference **19.503 / 93.645 us**.
Owner allocation is approximately **1.301–1.671 us** across the retained normal
and nonempty-wrap samples. These are two-shape measured results, not bounds for
arbitrary shapes or every latency sample.

### Allocation and Dispatch integration

The final count core already owns the final prefix and now performs the single-owner
Device allocation. Each core completes its original send/count work. Only the
leading `min(receiveStatusCount, blockDim)` output lanes receive count rows/flags;
only those lanes consume and clear those flags. Prefix/expert-map/count-output
preparation overlaps allocation. A copy lane waits for the immutable local
address descriptor immediately before output copy. The owner publishes the body,
completes MTE3, then publishes the round magic; readers observe magic before
fetching the body. Peer failure aggregation overlaps other lanes' local copies.
A peer failure patches only the final Host error, preserving the allocation handle;
Host still waits for complete kernel completion before exporting any Tensor.
No Host size Planner, precomputed output address, growth replay or retry was added.
The two preallocated reference entries retain their scheduling behavior.

Extended owner-copy regression exposed a pre-existing low-blockDim UB sizing bug:
`sumCoreBuf_`/`sumLocalBuf_` reserved `blockDim*32` bytes, while Duplicate writes
whole 256B repeats. With 2/4 cores the write exceeds the reserved scratch span; a non-multiple
of eight also needs rounding. Both buffers now reserve `ceil(blockDim/8)*256`.
The original two-core capacity-failure reproduction timed out on both ranks;
a change limited to this sizing removed that timeout and both ranks returned
Device error 2. Matching PID logs were retained; they contain no definitive
kernel-PC fault, so the exact corrupted value is not claimed. The Host UB model
now matches these two aligned buffers; the strict C++14 Host layout test passes.
The 64-core capacity is unchanged. The failure and subsequent final regression
are recorded separately.

### Final complete-kernel comparison

A5 `rack08os2`, physical 0/1 with identity mapping; CANN 9.2.0, driver
26.2.0.b020, firmware 9.2.0.101.200. Native Memory backend, BF16/topK2/four experts,
64 AIV blocks, identical deterministic routes, bs16/h128 and bs256/h4096.
`TILEXR_EP_COMPACT_PROFILING=OFF`; generic EP/Combine diagnostics OFF.
The Host AllGather stage barrier and msprof/framework configuration stay enabled
and identical across all three modes. No CPU affinity modification is used in
this final batch. Each rank has 20 warmups and 100 measured samples, no outliers
removed. Task Duration excludes Host wall and Task Wait Time; normal Device
allocation, publication and synchronization remain inside Dispatch.

All rows have backend `native_memory`, native status `PASS`. Reference Dispatch
kernel is `tilexr_ep_dispatch_memory_kernel`; pool Dispatch is
`tilexr_ep_dispatch_compact_kernel`; all Combine rows use
`tilexr_ep_combine_memory_kernel`. Kernel hashes below identify these versions.
The independent audit joined 2,880 tasks, including 2,400 measured tasks; pool
measurement growthCount is 1→1 throughout. Cross-rank median/min/max summarize
the two per-rank medians. P95 uses nearest rank `ceil(0.95*n)` over the applicable
samples; pooled P95 uses all 200 measured tasks for each stage/case. The Host
actual-size mode remains a comparison-only routing oracle, not a production Planner.

| Shape / mode | Rank | Dispatch median / min / max / P95 us | Cold us | Combine median / min / max / P95 us | Cold us |
|---|---:|---|---:|---|---:|
| small-maximum_capacity | 0 | 21.008 / 18.627 / 72.814 / 58.134 | 70.358 | 10.835 / 10.135 / 12.150 / 11.461 | 11.854 |
| small-maximum_capacity | 1 | 17.998 / 16.298 / 176.084 / 77.928 | 117.777 | 10.448 / 9.913 / 11.164 / 10.829 | 343.644 |
| small-host_actual_reference | 0 | 20.064 / 18.070 / 77.035 / 45.012 | 44.833 | 10.483 / 10.063 / 11.637 / 10.868 | 12.947 |
| small-host_actual_reference | 1 | 18.106 / 15.920 / 138.307 / 65.427 | 35.466 | 10.885 / 10.157 / 11.991 / 11.362 | 246.532 |
| small-device_pool | 0 | 16.845 / 10.649 / 36.187 / 28.580 | 82.433 | 11.139 / 10.405 / 13.352 / 12.592 | 12.324 |
| small-device_pool | 1 | 10.966 / 10.442 / 14.120 / 11.734 | 66.977 | 10.460 / 9.926 / 11.067 / 10.872 | 216.886 |
| large-maximum_capacity | 0 | 98.552 / 89.580 / 146.555 / 100.059 | 127.422 | 77.066 / 75.291 / 97.784 / 79.918 | 264.770 |
| large-maximum_capacity | 1 | 88.738 / 82.695 / 98.227 / 89.976 | 235.641 | 71.791 / 58.564 / 76.564 / 75.332 | 61.982 |
| large-host_actual_reference | 0 | 98.111 / 87.008 / 100.638 / 99.384 | 140.915 | 76.382 / 66.918 / 101.574 / 78.191 | 142.109 |
| large-host_actual_reference | 1 | 89.145 / 86.683 / 107.874 / 91.739 | 151.152 | 74.349 / 56.901 / 87.405 / 81.728 | 61.166 |
| large-device_pool | 0 | 96.175 / 77.853 / 102.234 / 98.928 | 1265.786 | 76.520 / 67.621 / 88.584 / 79.176 | 61.387 |
| large-device_pool | 1 | 88.489 / 84.290 / 114.182 / 101.574 | 1493.821 | 74.168 / 62.194 / 87.257 / 81.192 | 334.677 |

Cold is the first retained task, excluded from the warm statistics. Pool cold
tasks include the resident Host-growth wait and are not judged against the hot
2.5 us allocation target.

| Shape / mode | Stage | Rank medians: median / min / max us | Pooled P95 us | Effective output GB/s: median / min / max |
|---|---|---|---:|---|
| small-maximum_capacity | dispatch | 19.503 / 17.998 / 21.008 | 63.970 | 0.442 / 0.408 / 0.477 |
| small-maximum_capacity | combine | 10.641 / 10.448 / 10.835 | 11.245 | 0.385 / 0.378 / 0.392 |
| small-host_actual_reference | dispatch | 19.085 / 18.106 / 20.064 | 54.255 | 0.451 / 0.427 / 0.474 |
| small-host_actual_reference | combine | 10.684 / 10.483 / 10.885 | 11.340 | 0.384 / 0.376 / 0.391 |
| small-device_pool | dispatch | 13.905 / 10.966 / 16.845 | 23.523 | 0.646 / 0.509 / 0.782 |
| small-device_pool | combine | 10.800 / 10.460 / 11.139 | 12.118 | 0.380 / 0.368 / 0.392 |
| large-maximum_capacity | dispatch | 93.645 / 88.738 / 98.552 | 99.621 | 44.979 / 42.622 / 47.336 |
| large-maximum_capacity | combine | 74.428 / 71.791 / 77.066 | 78.218 | 28.212 / 27.213 / 29.212 |
| large-host_actual_reference | dispatch | 93.628 / 89.145 / 98.111 | 99.294 | 44.966 / 42.813 / 47.119 |
| large-host_actual_reference | combine | 75.365 / 74.349 / 76.382 | 80.944 | 27.831 / 27.456 / 28.207 |
| large-device_pool | dispatch | 92.332 / 88.489 / 96.175 | 99.106 | 45.572 / 43.675 / 47.468 |
| large-device_pool | combine | 75.344 / 74.168 / 76.520 | 81.192 | 27.841 / 27.406 / 28.276 |

The same-batch pool Dispatch increments over maximum output are
**-5.598 / -1.312 us**. Rank-arrival and scheduling tails remain visible in the
table; this does not guarantee a per-sample latency bound. In particular, large
rank 1 has pool P95 101.574 us versus maximum-output 89.976 us (+11.598 us): the
median result does not establish uniformly improved tail latency. No unfavorable
final sample is removed or replaced with another batch.

Effective bandwidth is logical output bytes / Task Duration, not link or HBM bus
bandwidth. Dispatch output is 8,576 / 4,200,448 B (hidden 8,192 / 4,194,304 B,
assist 384 / 6,144 B; count control 32 B separately); Combine output is
4,096 / 2,097,152 B and its logical input equals the hidden payload.

### SDK stages and remaining measurement boundaries

Four final STAGES runs use the repository `TileXR::Profiling::Recorder<64>` and
real pool service-stream capture. Collect runs only after completion so it cannot
block the Host growth service. Each shape has normal and retained-output ring
runs, each with 20 warmups and 20 measured calls per rank. The owner is final
count core 63. All 10,240 lane streams and 67,200 markers are complete without
overflow/drop. The independent parser audit checks raw intervals, mapping,
normal/wrap classification and zero measured growth. The allocator itself still
reads the compact 128B snapshot, commits one 64B ring and publishes the result.

| Run | Rank | Allocation median / min / max / P95, approximate us |
|---|---:|---|
| stages-verified-small | 0 | 1.451 / 1.441 / 1.671 / 1.670 |
| stages-verified-small | 1 | 1.431 / 1.421 / 1.448 / 1.444 |
| stages-verified-large | 0 | 1.308 / 1.302 / 1.466 / 1.322 |
| stages-verified-large | 1 | 1.321 / 1.314 / 1.426 / 1.337 |
| stages-verified-ring-small | 0 | 1.459 / 1.450 / 1.503 / 1.467 |
| stages-verified-ring-small | 1 | 1.305 / 1.301 / 1.557 / 1.396 |
| stages-verified-ring-large | 0 | 1.352 / 1.347 / 1.589 / 1.399 |
| stages-verified-ring-large | 1 | 1.311 / 1.306 / 1.454 / 1.453 |

All 160 owner intervals (120 normal / 40 nonempty wrap) are approximately
**1.301–1.671 us**, with 0 samples above 2.5 us.
The same-target empirical scale is 1000.8934146536867 ticks/us relative to
msprof Task Duration, not an independently identified GetSystemCycle frequency.
Its calibration records remain under
`optimization-20261108-resume/timer-calibration/` in the local evidence root;
this retained clock calibration is the conversion basis for the current SDK results.
No HWTS/AIV/sysClock frequency is silently substituted. Short-stage absolute
accuracy, cross-lane epochs and diagnostic disturbance remain limitations.
`output_setup` includes count waiting plus prefix/expert-map/count preparation;
`copy` now covers WaitAndFormatOutput only. These intervals overlap allocator
work on another core and must not be added as a serial path. Separate STAGES/OFF
task differences mix rank arrival with instrumentation; no subtraction correction
is used. Raw records, current mappings and official SDK HTML/Perfetto reports
are retained for every final capture.


Diagnostic perturbation check (separate STAGES/OFF batches, identical stage barrier):

| Shape | Rank | STAGES Task median us | OFF Task median us | Owner send/count median approx. us | Owner rank-gate median approx. us | Slowest-lane body median approx. us |
|---|---:|---:|---:|---:|---:|---:|
| small | 0 | 13.438 | 16.845 | 3.660 | 2.083 | 8.730 |
| small | 1 | 22.541 | 10.966 | 13.157 | 2.097 | 17.312 |
| large | 0 | 97.848 | 96.175 | 45.954 | 5.524 | 89.337 |
| large | 1 | 88.593 | 88.489 | 44.402 | 3.231 | 81.257 |

Slowest lane means the longest same-lane body interval, not an established
common-epoch completion critical path. Per-lane raw ticks and all substage
min/median/max/P95 remain in the SDK reports. These differing task distributions
do not establish negligible instrumentation cost; use only OFF for the comparison.

### Functional regression and retained failures

The final binary passes the strict C++14 allocator test, native pool/FIFO/ring,
post-HBM Host-bookkeeping fault, two-rank EP normal/zero/fail and real Torch
normal/failure tests. Torch uses 2.10.0+cpu with torch_npu 2.10.0.post7 on real NPUs,
not a mock: each rank passes 20 normal cases plus distributed failure. Seven
same-communicator transitions cover copy×count dimensions
32×2 → 2×1 → 4×1 → 4×1 → 9×2 → 32×2 → 4×1, including owner copy,
non-eight-multiple blockDim and shrinking/expanding flag layouts. Each checks all
four Dispatch outputs and Combine values, then retires ownership. Existing alias,
view across growth, delayed consumer, registered stream, dropped owners, FP16/BF16,
zero receive and close-with-live-Storage cases remain checked. Two-core zero and
capacity failure are additionally exercised directly.

FIFO still reserves a growing head request's new space: the retained native test
queues both requests before launch, observes one growth, then leader 3,872 B at
offset 0 and follower 128 B at offset 3,904. Pool tests retain one side effect per
call (14/14), nonempty wrap without growth, 864 blocked bytes, 160 padding bytes,
segment switch without growth and final trim-to-zero. Fault injection records
one allocation, one failure, one free and one side effect; poisoned domains reject
new requests. Output memory is unchanged: actual bundles 8,576 / 4,200,448 B,
committed/peak 8,639 / 4,200,511 B per rank, metadata 6,208 B. Empty segments return
on trim. The 2 GiB staging remains; Torch additionally retains 4 MiB flags and a
1 MiB guard. Output savings are not equal to total HBM savings.

Every actual launch has a fresh combined proc-mem/identity gate; busy or unknown
stops it. Retained rejected gates include the pre-overlap diagnostic and post-test
cleanup windows. PID 3470491 in the first Torch rejection matches the preceding
native failure test's plog; other ownership is not inferred without records.
No card reset, mapping change, HCCL requalification or external-task termination
was performed. The historical November 6 occupied-card run remains excluded and
its external impact remains unknown.

Two diagnostic fixture failures are retained and excluded: an old hardcoded end
event (28 versus the new 32), fixed by using the shared enum, and an invalid
`ring_kernel_perf` argument rejected before ACL initialization (correct name:
`kernel_ring_perf`). The two-core timeout and its sizing fix are also retained;
no timeout or msprof exit alone is counted as PASS.

### Memory and lifecycle accounting

Per rank, both shapes start at zero data HBM. The warm single-output comparison
allocates actual hidden+assist bytes, while retaining the existing staging:

| Quantity | bs16/h128 | bs256/h4096 |
|---|---:|---:|
| Actual output bundle B | 8,576 | 4,200,448 |
| Maximum-capacity reference B | 17,152 | 8,400,896 |
| Pool committed = peak B | 8,639 | 4,200,511 |
| Control metadata B | 6,208 | 6,208 |
| Configured staging data B | 2,147,483,648 | 2,147,483,648 |
| Pool committed after final trim B | 0 | 0 |

The 6,208 B metadata counter is 4,096 B mapped mailbox + 2,112 B Device
PoolState. It does not count Host map/deque, lease/event or service-thread object
overhead and is not a measurement of total Host control memory.

Torch additionally reports 4 MiB flags and a 1 MiB memory-domain guard; the peer
window is 2,151,677,952 B. Output reduction is **not total HBM reduction** and
does not remove staging, inputs, Torch intermediates or metadata. Ring blocked
free bytes are a subset of `freeBytes` unavailable until their prefix retires;
padding belongs to live/pending reservations. The dedicated pool fixture measures
864 B blocked and 160 B padding, then frees all segments. The retained-output SDK
fixture intentionally holds two overlapping lifetimes and reports each offset,
generation, padding and committed capacity in its JSON records; it must not be
compared with the single-output memory table as an equivalent retention pattern.

`Pending::Wait/materialize` still waits full data completion; metadata-only
materialization is not implemented. The version scope is CANN 9.2; other versions
are not promised. Inference-only, one owner per pool, no training/graph capture,
UDMA direct output or multi-host qualification are claimed.

### Final provenance and reproduction

Local evidence root:
`/workspace/TileXR-worktrees/elastic-memory/.elastic-memory-evidence/kernel-increment-20261109/`.
Remote root:
`/home/h30059441/elastic-memory-dev-20261106/kernel-increment-20261109/` on a5.
The ignored `provenance.json` pins source commit
**`d847c22956966e4e0805981ba5039fad894e047b`** on `codex/elastic-memory`,
[PR #212](https://github.com/LingquLab/TileXR/pull/212). `source-sha256.txt`
(SHA-256 `3b8bad6af21fbc44867af2cbecf154a6c1862a413769df9515a7fae0eddbccd8`)
and `verified-source-verification.log` verify the same 422 first-party build/test files
against the deployed source tree. `verified-provenance.log` and
`verified-torch-loaded-provenance.log` identify the actual build and installed libraries.
Install RPATH rewriting explains build/install Host-library hash differences.
The unchanged Torch extension is dynamically linked to the updated installed
native libraries/kernel package. The remote-root `verified-binaries.tar.gz`
preserves final OFF binaries and CMakeCache; its SHA-256 is
`579cbdfae5e4aee2f813bde64330ef5f1e64a36d9a68bc19906df114f019a513`.
STAGES run folders pin their instrumented binary hashes separately; they are not the OFF performance binaries.

Artifact paths below are relative to
`/home/h30059441/elastic-memory-dev-20261106/`, unless absolute:

| Final artifact | SHA-256 |
|---|---|
| `build/tests/elastic_memory/test_elastic_compact_ep` | `b9ebc52af641122e8caeb57ead3d495c33a214e1faf508abf8d3b99bace43ace` |
| `build/src/elastic_memory/libtilexr-elastic-memory.so` | `c6bf2d9a109a6e81e29aa1c5eb8965fb362044cae46f5a379ca535bded659ee2` |
| `install/lib64/libtilexr-elastic-memory.so` | `48a5944d5d8a53f482962d27cd5bc3c7e739f21d40177dff399808210eedf338` |
| `build/src/ep/libtilexr-ep.so` | `833d9d02dcf48456af911bf09bff61476052f770d5f6664493a0beda205e5d69` |
| `install/lib64/libtilexr-ep.so` | `b2b3ce3cd682c4d3c02b13af0f5ed6b25b29c3191d765182df1ca0dba7b0f820` |
| `install/lib64/libtile-comm.so` | `ada88ea138ed62612cf7906c85692a9d2cd5f0de142eeab17fda7106a5ebd351` |
| `install/lib64/libtilexr-diagnostics.so` | `7385814a3660f5e05cdbf992216401bbe99ebecdcd3bf7a5b8c756adddda6dc5` |
| `install/lib64/tilexr/kernels/tilexr_ep_dispatch_compact_kernel.o` | `e56a432d4fb21774f613d3641cba79030d73610d7056ea5ef7890727c18ceaa2` |
| `install/lib64/tilexr/kernels/tilexr_ep_dispatch_memory_kernel.o` | `d38560c3e1f5df38d6eb5ceb7f49446f623469a6ed06149a22239dab9353528f` |
| `install/lib64/tilexr/kernels/tilexr_ep_combine_memory_kernel.o` | `e77def8099fc6014327f31cb037060353489d5b77e3eb7d23aa7ac7392a7cc7b` |
| `build/tests/elastic_memory/elastic_pool_kernel.o` | `261d31e4b043468921d2bc2149145c9b06d344e796c32ed25527101e2ced6fdc` |
| `source/integrations/elastic_memory_torch/tilexr_elastic/_C.cpython-312-aarch64-linux-gnu.so` | `cc8f7e5452330909a44d7712561162c2c9a0593db04c607adf6bc0e5dc305c7d` |
| `/home/pkg/b070/cann-9.2.0/aarch64-linux/lib64/libascendcl.so` | `1c06b9c4a8910cfb585eab4ed0066bc09e28a0cf273dfcb15458f7cc49a78cea` |
| `/home/pkg/b070/cann-9.2.0/aarch64-linux/lib64/libruntime.so` | `945f49b024b192412d5021bc12a47c9a115064059e62940f9fc8c0f7342e82c5` |
| `/home/pkg/b070/cann-9.2.0/aarch64-linux/lib64/libruntime_common.so` | `6688f0dfaa0e02965896384ae2f30f962057fab6ad79a3c7d36ad95e4bf04597` |
| `/usr/local/Ascend/driver/lib64/driver/libascend_hal.so` | `ab759e7165f18ca68640583585650327b9ec34893fd611e9af3820b44d213450` |

`delivery-off-*` holds each final paired gate, JSON correctness/memory logs and
raw msprof CSV. `verified-summary.json` / `verified-joined-samples.json` are the
main audit; `independent-verified-off-audit/` reparses original CSV/logs.
`stages-verified-*` holds raw SDK records, mapping and per-capture official reports;
`independent-verified-stage-audit/` independently checks every lane.
`regressions-verified/` contains the post-fix functional results; earlier failing
and diagnostic cases remain under their original names. Source/ABI/CMake and
failure/lifetime ordering received independent review, followed by a final
main-agent diff review. No environment-only orchestration is committed.

Reproduction is performed by development; the design session is not asked to run
commands. Use the environment and build commands below, choose unused communication
ports, then run the fresh gate immediately before each paired launch. The retained
ignored `run-pair.sh` records the exact equivalent msprof command; its final calls
use `delivery-off-SHAPE-MODE MODE BS H 100 0`. Diagnostic calls use STAGES and
`stages-verified-SHAPE device_pool BS H 20 1`, with `kernel_ring_perf` for retained
ring calls. Set no NPU remap and no CPU affinity override. Host tests require no NPU.
Torch uses the existing `tests/test_npu.py` command with the updated transitions.

CANN 9.2 alone is the feature's validated version. Pending::Wait/materialize still
waits full data completion; metadata-only readiness remains unimplemented.
More ranks, multi-host, UDMA direct output, training/autograd and graph capture
are not established by these results.

## Environment and evidence roots

The November 9 results use `ssh a5`, host `rack08os2`, aarch64, without a compute
container. Native pool tests use physical NPU 0; paired EP/Torch tests use physical
NPUs 0 and 1, with identity mapping confirmed before each launch.

- Ascend950DT; driver `26.2.0.b020`, firmware `9.2.0.101.200`.
- Toolkit `/home/pkg/b070/cann-9.2.0`, compiler build `20260921_000125261`;
  bisheng reports clang 15.0.5 build 2026-09-18.
- CMake 3.31.10 at `/home/g00623548/test20261102/tools/cmake/bin/cmake`;
  host GCC 12, native C++14.
- Python `/home/l00984991/envs/py312_lzy/bin/python`, Torch `2.10.0+cpu`,
  torch_npu `2.10.0.post7`, CXX11 ABI enabled. Despite the Torch package suffix,
  actual tensors and kernels ran on NPU through torch_npu; no mock was used.
- Runtime resolves real driver libraries in `/usr/local/Ascend/driver/lib64/driver`
  and `/usr/local/Ascend/driver/lib64`, never toolkit `devlib` stubs.
- Source/build/install/log root: `/home/h30059441/elastic-memory-dev-20261106/`.
  Source is in `source/`, products in `build/` and `install/lib64/`.
- M0 experiment root: `/home/h30059441/elastic-memory-m0-20261106/`.
- Local evidence copy: `/workspace/TileXR-worktrees/elastic-memory/.elastic-memory-evidence/`.
  Environment-specific gate/probe scripts and raw logs are retained outside Git.

HCCL Test was **not rerun**. Existing records under
`/home/h30059441/unitest-pr199/logs/{hccl-attempt-started.txt,hccl-aiv-only.txt,hccl-exit-status.txt}`
and `unitest-8254c28b-a5-20261106/logs/qualification-reuse.json` identify the
November 3 all-eight-device AIV-only attempt. Only rank-0 correctness evidence
survives. It is not complete qualification of every rank or the current two-card
subset; that boundary remains explicit despite successful operator tests.

## Occupancy violation and repaired gate

At **2026-11-06 11:14:58 UTC (19:14:58 +08)**, the first mature pool run
(`pool-test1.log`) proceeded on device 0 although proc-mem already showed
external `VLLMWorker_DP` PID **2686943**, **85,155 MB**. The preflight observation
preceded our test in that log. The faulty gate relied on an empty `fuser` result
instead of rejecting the proc-mem occupancy. That PASS is **only a functional
observation with an occupancy violation**: excluded from formal acceptance,
performance and exclusivity evidence. No external task was killed or reset;
**impact on the external workload is unknown**. Absence of kill/reset does not
establish absence of interference. The exact external workload start/end and
performance effect were not measured.

Further launch was paused. The replacement gate refuses any target-card
external process, failed query, malformed/truncated output, or unconfirmed
logical/physical mapping. Only explicit `No process in device.` for every target
card passes; `fuser` cannot override it. It also rejects runtime device remapping.
Retained occupied output was replayed without launching NPU work:

| Fixture | Gate result |
|---|---|
| Retained VLLM proc-mem output | busy / refused |
| Nonzero query status | unknown / refused |
| NPU ID mismatch | unknown / refused |
| Truncated or empty output | unknown / refused |
| Explicit idle response | idle |

Evidence: local `idle-gate-fixtures.log`, `occupancy-raw.log`,
`mapping-and-incident.log`; remote `idle_gate.py`, each `*-preflight.json`.
Live all-device queries subsequently reported idle at 11:29 UTC. Formal tests
started only after fresh checks immediately before each launch: production pool
11:45 UTC; EP normal 11:47, zero/failure 11:50; Torch 11:56 and later final runs.
These are point-in-time availability checks, not a reservation system or a claim
that another user could not start work later.

## Resident-kernel growth gate (M0)

The retained M0 experiment establishes the running-kernel growth prerequisite;
current segmented-ring functional coverage is recorded above.

M0 `growth-volatile.log`: Device request 2,272 bytes; Host deliberately delayed
100 ms, then real HBM allocation took 26 us with `done_at_allocation=0`;
pre-allocation `side_effect=1`; every output word correct; stream sync 0.
`failure-injection.log`: Host failure 207001, Device exits with failure,
side effect remains 1, sync 0. The earlier failed poll is retained in `growth.log`:
the compiler hoisted `GetValue` outside the polling loop; volatile GM reload plus
DCCI fixed the observed hang. This is CANN 9.2 evidence only.

Read the retained originals without launching device work:

```bash
sed -n '1,180p' /home/h30059441/elastic-memory-m0-20261106/growth-volatile.log
sed -n '1,180p' /home/h30059441/elastic-memory-m0-20261106/failure-injection.log
sed -n '1,180p' /home/h30059441/elastic-memory-m0-20261106/growth.log
```

The current native pool and fault-injection commands below recheck real growth,
once-only side effects and failure propagation through the segmented-ring path.

## Reproduction commands

Choose genuinely idle devices first. Capture `npu-smi info -m` and
`npu-smi info -t proc-mem -i DEVICE` for **every** selected device immediately
before launch; stop if the query/mapping is uncertain or any external process
appears. Do not kill/reset another workload. The retained environment-specific
`idle_gate.py` implements these checks; it is not a repository deployment tool.

```bash
source /home/pkg/b070/cann-9.2.0/set_env.sh
export PATH=/home/g00623548/test20261102/tools/cmake/bin:$PATH
export TILEXR_SOC_NAME=Ascend950DT
cd /home/h30059441/elastic-memory-dev-20261106
cmake -S source -B build -DTILEXR_BUILD_ELASTIC_MEMORY=ON -DTILEXR_BUILD_EP=ON \
  -DTILEXR_BUILD_UNITEST=OFF -DBUILD_TESTING=ON -DTILEXR_BUILD_TESTS=OFF \
  -DTILEXR_EP_COMPACT_PROFILING=OFF -DTILEXR_EP_ENABLE_PROFILING=OFF \
  -DTILEXR_EP_COMBINE_MEMORY_ENABLE_PROFILING=OFF -DCMAKE_INSTALL_PREFIX="$PWD/install"
cmake --build build -j8
cmake --install build
export LD_LIBRARY_PATH="$PWD/install/lib64:/usr/local/Ascend/driver/lib64/driver:/usr/local/Ascend/driver/lib64:$LD_LIBRARY_PATH"
ctest --test-dir build -R elastic_allocator --output-on-failure
python3 idle_gate.py 0 && timeout -k 5 90 build/tests/elastic_memory/test_elastic_pool \
  0 "$PWD/build/tests/elastic_memory/elastic_pool_kernel.o"
python3 idle_gate.py 0 && env TILEXR_TEST_GROWTH_FAULT_BYTES=1048593 \
  LD_PRELOAD="$PWD/build/tests/elastic_memory/libelastic_growth_fault.so" \
  timeout -k 5 90 build/tests/elastic_memory/test_elastic_growth_fault \
  0 "$PWD/build/tests/elastic_memory/elastic_pool_kernel.o"
```

For paired EP, set `TILEXR_COMM_ID=127.0.0.1:UNUSED_PORT`,
`TILEXR_ENABLE_UDMA=0`, and launch `test_elastic_compact_ep RANK DEVICE MODE`
concurrently after the **combined** idle check. Run `normal`, `zero` and `fail`
in separate pairs, with an outer bounded timeout; verify both exit codes and
both ranks' expected PASS output. The final Torch test includes owner-copy and
changing copy/count dimensions as well as the lifecycle cases listed above.

```bash
cd source/integrations/elastic_memory_torch
export TILEXR_INSTALL_PREFIX=/home/h30059441/elastic-memory-dev-20261106/install
MAX_JOBS=4 /home/l00984991/envs/py312_lzy/bin/python setup.py build_ext --inplace
export PYTHONPATH="$PWD"
# After a combined idle gate, run the following concurrently with rank/device 0 and 1:
/home/l00984991/envs/py312_lzy/bin/python tests/test_npu.py \
  --rank RANK --device DEVICE --comm-domain UNUSED_DOMAIN
# Repeat as a fresh paired call with --failure for one-rank budget failure.
```

### Device kernel profiling command

With the environment above, build `test_elastic_compact_ep`. For each fresh
process pair, select MODE from `maximum_capacity`, `host_actual_reference`,
`device_pool` and BS/H from `16/128`, `256/4096`, choose a fresh communication port,
and repeat the combined idle gate. The actual per-rank invocation was:

```bash
cmake --build build --target test_elastic_compact_ep -j8
export TILEXR_ENABLE_UDMA=0 TILEXR_COMM_ID=127.0.0.1:UNUSED_PORT
python3 idle_gate.py 0 1 || exit 1  # never reuse an earlier snapshot
# Run concurrently for RANK=0 and RANK=1, DEVICE equal to RANK:
timeout -k 5 240 msprof --output="$OUT/rank$RANK" --task-time=on --ai-core=on \
  --aic-mode=task-based --runtime-api=on \
  --application="$PWD/build/tests/elastic_memory/test_elastic_compact_ep $RANK $RANK kernel_perf $MODE $BS $H 20 100"
# Read-only reproduction of the retained analysis; no device execution:
python3 kernel-increment-20261109/summarize_delivery_off.py
```

Verify both process exit codes, both final PASS lines, each kernel's 120-row count,
the first-20 exclusion, unchanged growth counters and full-value oracles. Do not
substitute Host wall time or add task-wait time to the profiler task duration.

### Repository SDK capture command

Use `TILEXR_EP_COMPACT_PROFILING=STAGES` for diagnostic capture. The retained
`kernel-increment-20261109/run-pair.sh` supplies `TILEXR_ELASTIC_PROFILE_DIR` and
a shared capture seed to both ranks, binds the capture to the actual service
stream, and collects after completion. After a fresh combined gate, its paired
application commands are:

```bash
# RANK/DEVICE are 0/0 and 1/1. Use the same unused communication port for the pair.
build/tests/elastic_memory/test_elastic_compact_ep RANK DEVICE kernel_perf device_pool 16 128 20 20
build/tests/elastic_memory/test_elastic_compact_ep RANK DEVICE kernel_ring_perf device_pool 16 128 20 20
# Repeat as separate fresh-gated pairs for bs=256, h=4096.
```

Rebuild and install with `TILEXR_EP_COMPACT_PROFILING=OFF` before the complete-kernel
comparison. Diagnose with complete SDK lanes/records; only the OFF named msprof
Task Duration supports the final kernel comparison.

## Maintainer lessons

- Keep hot metadata proportional to the current operation. Preserve immutable
  Host snapshots and Device commit ownership, bound all reservations including
  retired records blocked by an earlier lease, and account for padding separately.
  Rebuild native and Torch consumers together when statistics or launch ABI changes.

- When moving a public Device header, update kernel build dependencies and install
  destinations as well as includes. Compile a consumer against a fresh install;
  an old prefix can hide a missed move by retaining obsolete headers.

- A compact allocator alone does not remove integration barriers or unnecessary count
  fanout. Prove the actual consumer set before reducing flags; skipped consumers must
  have no published flag to clear. Test both dimensions changing across completed
  rounds and the owner also performing copy. Size vector scratch for the complete
  repeat span, not only the bytes eventually copied to GM; tiny and non-eight-multiple
  blockDim expose overlaps that 64-core performance tests miss.

- A full Host oracle between stages can make the earlier rank wait inside the
  next communication kernel. Use the same pre-stage rank barrier in all modes,
  keep normal in-kernel waits, and derive latency from the named profiler task.
  Verify ordinal/sample counts and growth stability before computing statistics;
  separate cross-rank median statistics from a pooled sample P95.

- proc-mem is the occupancy gate; absence of file owners is not evidence of an
  idle NPU. Fail closed on parsing/query/mapping uncertainty and repeat directly
  before the actual launch. Preserve a violation and its unknown impact even
  after compliant runs succeed.
- A mapped mailbox does not make an ordinary accessor a polling load. Inspect
  generated instructions when a reload disappears; combine volatile access with
  the required Device cache invalidation and ordered publication. This fix was
  proven on CANN 9.2; do not infer compatibility with other versions.
- Publish growth failure before cleanup that might wait on Device. HBM success
  followed by Host bookkeeping failure must still unblock the resident kernel;
  the fault test covers this boundary without a production injection switch.
- Torch task queues may still contain uses after the Python Storage reference
  disappears. Flush registered submission queues on the native retirement worker,
  outside the pool mutex, then record/query completion events. Do not synchronously
  flush from a Storage deleter that may itself execute on the Torch queue worker.
- Keep runtime waits/malloc/free outside the statistics mutex. Account for an
  in-flight producer reservation so a snapshot cannot advertise its output as
  free or reclaimable. Segment size is a preference, not a reason to reject a
  smaller request that fits the remaining committed budget.
- Build all install targets before `cmake --install`; on this aarch64 system
  GNUInstallDirs selects `lib64`. Torch setup detects this and permits an explicit
  `TILEXR_INSTALL_LIBDIR` override. Device headers and their allocator dependency
  must be installed alongside the Host API.
