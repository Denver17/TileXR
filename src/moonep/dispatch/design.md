# MoonEP Dispatch Operator Design

Dispatch is a same-host, direct-launch Ascend C stage for CANN 9.1 and
Ascend 910A5/Ascend950. It accepts the public V1 Plan and paired tensors:

```text
hiddenSh          BF16 [S,H]
routeWeightsSk    optional FP32 [S,K]
hiddenNvsh        BF16 [NvS,H]
routeWeightsNvs   optional FP32 [NvS]
```

The optional weight descriptors are both null or both valid. Plan validation requires
`N=S*K`, matching `R`, `E%R==0`, `1<=B<=E/R`, `NvS>=N`, `K<=32`, and bounded signed
route encoding.

Host layout reserves a 32-byte-strided peer-window row for each of `NvS` destinations.
Hidden rows are chunked so the active payload plus optional weights and fresh dedup
scratch fits `IPC_BUFF_MAX_SIZE`. Each chunk uses clear, ready, and drained magic-tagged
steps. Calls use a fresh `TileXRCommNextMagic` and never reset shared flags.

For each route, `dst` decodes as:

```text
raw = encoded >= 0 ? encoded : -encoded - 1
peer = raw / NvS
loff = raw % NvS
```

Grouped URMA route selection must compare the encoded destination against each
peer's `[peer * NvS, (peer + 1) * NvS)` range. `NvS` is not required to be a
power of two, so bit shifts and masks are not valid for general decoding.

A nonnegative route scatters the source token row. A negative route identifies a
duplicate of an earlier route from the same token to the same peer. Fresh Dispatch
exchanges parent offsets, builds group-contiguous `dupLoffs` slices in two passes, and
expands primary rows to all duplicate offsets. Saved-plan Dispatch skips the build and
does not mutate `dupGroups`, `dupLoffs`, or `dupCounts`.

The kernel is compiled as a pure AICore ELF, installed in `tilexr/kernels`,
loaded relative to the owning Host library, registered with
`rtDevBinaryRegister`/`rtFunctionRegister`, and launched by
`rtKernelLaunchWithFlagV2`. Host wrapper launches are prohibited. Success status
is 2000; invalid route, remote failure, and timeout ranges are defined in
`moonep_peer_window.h`.

Validation includes Host/layout/source tests, a K=4 multi-duplicate hardware
regression, and the runtime validation ladder documented in
`docs/BUILD_VERIFICATION.md`.
