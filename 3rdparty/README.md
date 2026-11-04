# Third-party source dependencies

Git submodules lock upstream source revisions. Initialize only the dependencies
needed for the selected build, or use `git submodule update --init --recursive`
for the complete tree. Do not copy upstream source into TileXR modules.

| Path | Purpose | Pinned revision |
| --- | --- | --- |
| `spdlog` | Core Host logging | recorded by the Git submodule |
| `tilelang-ascend` | Ascend Python DSL/compiler and generated GEMM launcher | `87e6d1c2f0a9d2ff6a9013d9a1202325c4a619b1` |
| `picojson` | UniTest Host JSON parsing (header-only) | v1.3.0, `25fc213cca61ea22b3c2e4db8def9927562ba5f7` |

UniTest uses picojson as a Git submodule, without building or linking an
additional library. Initialize it before configuring `tools/unitest`:

```bash
git submodule update --init 3rdparty/picojson
```

Keep the submodule at the recorded revision. Its upstream BSD-2-Clause license
is retained in `3rdparty/picojson/LICENSE` and the header.

The optional production `tilexr-relay` Host library loads offline-generated
ABI-v1 kernels. It does not require collectives, compile Python/TVM or link
CUDA/TileScale. `scripts/build_tilexr_relay.py` uses `tilelang-ascend` to generate
AllGather-GEMM and GEMM-AllReduce artifacts. The benchmark also uses that compiler
for a standalone GEMM callback into an explicitly supplied historical pipeline
DSO; the old pipeline implementation and ABI-v0 builder are archived outside the
active tree. TileScale is not a build, generation or runtime dependency; its
comparison-only checkout belongs under ignored
`reference/tilescale`, not
in this dependency list or recursive submodule initialization. Do not install
its CUDA runtime into the NPU environment. These upstream projects share the
`tilelang` import name and need separate Python environments.

Initialize the Ascend compiler source before following its build instructions:

```bash
git submodule update --init --recursive 3rdparty/tilelang-ascend
cd 3rdparty/tilelang-ascend
# In an isolated environment with matching CANN/torch/torch_npu:
bash install_ascend.sh
source set_env.sh
```

The compiler build uses its own C++ standard and applies upstream TVM patches;
TileXR remains C++14. Do not alter the pinned gitlink to commit build artifacts
or applied local TVM patches. Native TileXR compilation needs only spdlog and
the selected CANN installation. See [TileLang integration](../docs/TILELANG_INTEGRATION.md).
