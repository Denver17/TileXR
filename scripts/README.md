# TileXR Scripts

This directory contains utility scripts for building, testing, and managing the TileXR project.

## Environment Setup

### `common_env.sh`
**Purpose**: Core environment configuration script that must be sourced before any build or test operations.

**Usage**:
```bash
source scripts/common_env.sh
```

**What it does**:
- Sets `TILEXR_HOME`, `TILEXR_CANN_HOME`, `TILEXR_TEMP_HOME`
- Detects CPU architecture (`TILEXR_OS_ARCH`)
- Detects SOC name and device count
- Configures CANN version (default: 9.1.0)
- Sets up PATH and LD_LIBRARY_PATH for CANN toolkit

**Environment variables**:
- `TILEXR_CANN_VER`: CANN version (9.1.0, 9.0.0-beta.1, 8.5.0)
- `TILEXR_SOC_NAME`: Detected chip name (Ascend910B, etc.)
- `TILEXR_HOME`: Repository root directory

### `common_util.sh`
**Purpose**: Utility functions used by other scripts.

**Functions**:
- `soc_name`: Detect NPU chip type
- `ops_name`: Get operator package name
- `device_count`: Count available NPU devices

## CANN Installation

### `cann_download_install.sh`
**Purpose**: Download and install CANN toolkit from Huawei OBS.

**Usage**:
```bash
bash scripts/cann_download_install.sh
```

**What it does**:
- Downloads CANN toolkit, kernels, and nnae packages
- Installs to `${TILEXR_CANN_HOME}`
- Configures environment variables

**Requirements**: Root user, internet connection

### `cann_local_install.sh`
**Purpose**: Install CANN from local packages (if already downloaded).

**Usage**:
```bash
bash scripts/cann_local_install.sh
```

## Dependency Building

### `build_tilexr_relay.py`

Generate ABI-v1 TileRelay content-addressed pure AICore ELF/Plan pairs using the
separately built, pinned TileLang Ascend compiler and CANN 9.1.
This opt-in tool does not build dependencies or modify the system environment.

```bash
python scripts/build_tilexr_relay.py --operator allgather_gemm \
  --rows 1024 --n 4096 --k 4096 --blocks 16 \
  --compiler-revision 87e6d1c2f0a9d2ff6a9013d9a1202325c4a619b1 \
  --output build/relay-kernels
```

Use `--operator gemm_allreduce` for GEMM followed by AllReduce SUM. See
[TileLang integration](../docs/TILELANG_INTEGRATION.md) for installation and
operator contracts. CMake consumes prebuilt artifacts when
`TILEXR_RELAY_KERNEL_DIR` is supplied; the optional Host library build never
invokes the Python compiler. The ABI-v0 builder is historical only.

### `download_open_source_deps.sh`
**Purpose**: Download fixed-version source archives used by `prepare.sh`.

**Usage**:
```bash
bash scripts/download_open_source_deps.sh
bash scripts/download_open_source_deps.sh --check
```

**What it does**:
- Downloads archives into `3rdparty/open_source/`
- Verifies every archive with SHA256
- Reuses valid existing archives

## Testing

### Pull-request CI

The CI operator runbook is [docs/CI.md](../docs/CI.md). Primary entrypoints are:

- `scripts/ci/host_checks.sh`: Ubuntu-compatible fast host validation.
  Its tracked-shell syntax scan invokes `bash -n` separately for every script;
  passing several paths to one Bash invocation checks only the first script
  and treats the rest as positional arguments.
- `scripts/ci/control/gate.py`: sealed `blue` build, queue, hardware, cleanup,
  and result controller; invoke it through the trusted workflow.
- `scripts/ci/provision/{account,cann,control,runner,verify}.sh`: idempotent
  `blue` provisioning and acceptance checks; each supports `--dry-run`.

### `test_build.sh`
**Purpose**: Build HCCL test suite.

**Usage**:
```bash
bash scripts/test_build.sh
```

### `test_allreduce.sh`
**Purpose**: Run AllReduce collective communication test.

**Usage**:
```bash
bash scripts/test_allreduce.sh
```

**What it does**:
- Launches multi-rank test via mpirun
- Tests AllReduce operation across NPU devices

### `run_moonep.sh`
**Purpose**: Run the MoonEP benchmark, reference, or correctness flow with a requested
logical rank count.

**Usage**:
```bash
bash scripts/run_moonep.sh --mode correctness --rank-size 4
```

The equivalent short form is `bash scripts/run_moonep.sh -m correctness -r 4`.

For a single-rank run, the MoonEP wrapper forces `TILEXR_ENABLE_UDMA=0` because
TileXR does not initialize UDMA for a single-rank communicator. Multi-rank runs
preserve the caller's selection; when the variable is unset, TileXR uses its
default UDMA-enabled behavior.

All modes default to the fixed-padding single-route `planning-no-dedup` case supported
by the current URMA Dispatch. Reference runs can opt into the hand-checkable
`manual-small` case (`S=2,K=1,E=2,H=8,Hf=4,B=2,P=1`) with
`--case-id 1` or `--case-id manual-small`. Case numbers are one-based positions in
`tools/moonep/cases/correctness.json`; results always use the canonical string ID.
The usage table lists the intended `rank_size` and `rank_per_dev` topology together
with `S`, `K`, `E`, `H`, `Hf`, `B`, and fixed `P=1` for every case.

Without a device option, the script selects physical NPUs starting at 0, so a
four-rank run uses devices `0,1,2,3`. Select a different set without configuring the
shell environment:

```bash
bash scripts/run_moonep.sh --mode reference --rank-size 4 \
  --case-id planning-4rank-topk-4 \
  --visible-devices 4,5,6,7 \
  --hccl-npu-socket-port-range 47200-47300
```

The HCCL NPU socket range defaults to `47000-47100` inside the script. The CLI values
take precedence; existing `ASCEND_RT_VISIBLE_DEVICES` and
`HCCL_NPU_SOCKET_PORT_RANGE` remain supported only as compatibility fallbacks.

`reference` and `correctness` runs save complete inputs and outputs for every MoonEP
stage by default and print the first eight values of each tensor. Override the preview
length with `--tensor-preview-elements COUNT`, or disable snapshots with
`--no-dump-stage-tensors`. `benchmark` runs never enable snapshots because device-to-CPU
copies would invalidate performance measurements. Benchmark mode defaults to
`--warmup 5 --iterations 20`. Both options accept zero, but their sum must be at least
one. Warmup iterations are excluded from `samples.jsonl` and all performance statistics;
reported means are arithmetic means over the measured iterations. With `--iterations 0`,
the warmup-only run succeeds and the six-stage performance fields are `N/A`. The script
automatically prints the aggregated six-stage table at the end.

Each binary `input.pt`/`output.pt` has a matching `input.txt`/`output.txt` that includes
the field path, shape, dtype, original device, and every value without ellipsis. JSON
files retain compact metadata and terminal previews.

Add `--generate-flowcharts` to a `reference` or `correctness` run to generate a
left-to-right diagram for all six stage boundaries:

```bash
bash scripts/run_moonep.sh --mode reference --rank-size 2 \
  --case-id manual-2rank-topk-2 \
  --generate-flowcharts
```

The option is disabled by default and requires the default tensor snapshots. It reads
the reference snapshots even in correctness mode, then writes numbered Mermaid, SVG,
and 2x PNG files under `<result>/<case_id>/flowcharts/`, from `1_planning-*` through
`6_reduce-grad-*`. Run `bash scripts/prepare.sh` first to install the PyPI Mermaid CLI
and browser. Flowchart export is rejected in benchmark mode so rendering and snapshot
work cannot affect performance measurements.

The script supports npm Mermaid CLI with Puppeteer and the Python Mermaid CLI with
Playwright. The selected CLI must also have a browser for the host architecture; for
the Playwright variant, install it once with `python -m playwright install chromium`.
The script performs a minimal render before launching NPU work and fails with this
guidance when the browser is missing or incompatible.

Use the two-rank manual case to inspect uneven Planner input load without duplicate
token destinations:

```bash
bash scripts/run_moonep.sh --mode reference --rank-size 2 \
  --case-id manual-2rank-imbalanced
```

The global expert counts are `[2,2,1,1]`, so the initial owner loads are `[4,2]`.
Planner can exercise remote expert placement while each token has only one route.

Use the compact multi-route case to retain `K > 1` coverage without repeated token
destinations:

```bash
bash scripts/run_moonep.sh --mode reference --rank-size 2 \
  --case-id manual-2rank-topk-2
```

Its `unique_destinations` routing sends each token's two TopK routes to different owner
ranks. `planning-4rank-topk-4`, `planning-8rank-topk-8`, and
`planning-16rank-topk-16` extend the same invariant to `K=4`, `K=8`, and `K=16`.
The paired `planning-8rank-single-route` and `planning-16rank-single-route` cases share
`S=8,K=1,E=16,H=8,Hf=4,P=1`, isolating rank/collective scaling from TopK scaling.
Their required capacities are `B=2` and `B=1` respectively because `B=E/R`.
Every runner case uses `P=1`, and no runner case selects a duplicate-destination
routing pattern.

Run the full-device cases on eight visible NPUs:

```bash
bash scripts/run_moonep.sh --mode reference --rank-size 8 \
  --case-id 8 --visible-devices 0,1,2,3,4,5,6,7

bash scripts/run_moonep.sh --mode reference --rank-size 16 \
  --case-id 9 --visible-devices 0,1,2,3,4,5,6,7

bash scripts/run_moonep.sh --mode reference --rank-size 8 \
  --case-id 10 --visible-devices 0,1,2,3,4,5,6,7

bash scripts/run_moonep.sh --mode reference --rank-size 16 \
  --case-id 11 --visible-devices 0,1,2,3,4,5,6,7
```

The 16-rank launch uses `ranks_per_device=2`; modulo assignment binds logical ranks
`d` and `d+8` to physical device `d`. Oversubscribed runs are functional validation,
not valid performance measurements. Reference/correctness mode uses Gloo with CPU
staging for this layout because HCCL requires unique local device IDs; one-rank-per-NPU
runs continue to use HCCL. Canonical IDs `planning-8rank-topk-8`,
`planning-16rank-topk-16`, `planning-8rank-single-route`, and
`planning-16rank-single-route` remain accepted.

IDs `12` and `13` extend the single-route matrix to 64 and 128 ranks. They
use eight ranks per server with one rank bound to each NPU, so ID 12 requires eight
servers and ID 13 requires sixteen. Set the same launch ID, output path, master address,
and master port on every participating node, then set the zero-based node rank locally:

```bash
export TILEXR_MOONEP_LAUNCH_ID=moonep-64r-example
export TILEXR_MOONEP_OUTPUT_DIR="$PWD/run/moonep/moonep-64r-example"

bash scripts/run_moonep.sh --mode reference --rank-size 64 --case-id 12 \
  --node-count 8 --node-rank "$NODE_RANK" \
  --master-addr "$MASTER_ADDR" --master-port 29600 \
  --visible-devices 0,1,2,3,4,5,6,7 --no-dump-stage-tensors
```

Each node writes its own global-rank directories. Merge those node-local result trees
before running aggregation. After the merged directory contains every `rank_<rank>`
directory and `node_<node-rank>_complete.json` marker, run the benchmark aggregation
without launching NPU workers:

```bash
export TILEXR_MOONEP_OUTPUT_DIR="$PWD/run/moonep/moonep-64r-example-merged"
bash scripts/run_moonep.sh --mode benchmark --rank-size 64 --case-id 12 \
  --node-count 8 --aggregate-only
```

The command validates that node rank ranges cover the complete world, aggregates all
global ranks, and prints the benchmark inputs plus a six-stage performance table as its
final terminal section. Each row joins native status and Kernel/API version on the left
with timing, algorithm bytes, and algorithm bandwidth on the right.

ID `14` / `planning-16rank-16card-single-route` is the compact two-node topology:
`rank_size=16`, `rank_per_dev=1`, two servers, and eight NPUs per server. Multi-node
`benchmark` uses the TileXR communicator and authenticated Host barrier environment;
`reference` uses `MASTER_ADDR`/`MASTER_PORT`; `correctness` requires both sets and runs
the full flow for both backends. For example, launch the following concurrently with
`NODE_RANK=0` and `1` on the two servers:

```bash
export TILEXR_MOONEP_LAUNCH_ID=moonep-16r-16card-example
export TILEXR_MOONEP_LAUNCH_SECRET=<64-hex-shared-secret>
export TILEXR_COMM_ID="$MASTER_ADDR:12001"
export TILEXR_MOONEP_BARRIER_ADDR="$MASTER_ADDR:12114"
export TILEXR_MOONEP_OUTPUT_DIR="$PWD/run/moonep/moonep-16r-16card-example"

bash scripts/run_moonep.sh --mode correctness --rank-size 16 --case-id 14 \
  --node-count 2 --node-rank "$NODE_RANK" \
  --master-addr "$MASTER_ADDR" --master-port 29600 \
  --visible-devices 0,1,2,3,4,5,6,7 --no-dump-stage-tensors
```

The script sources `common_env.sh`, uses Conda environment `ai_moe_test` by default, and writes
results to a timestamped directory under `${TILEXR_HOME}/run/moonep`. Set
`TILEXR_MOONEP_OUTPUT_DIR` to override that location. Exclude the generated `run`
directory from Mutagen synchronization. With eight available devices and no explicit
device selection, a four-rank run uses physical devices 0-3.
Running it without arguments or with `--help` prints usage.

### `run_moonep_meta_gate.py`

Run the single-node MoonEP model-order cascade gate without setting environment
variables:

```bash
cd /path/to/TileXR
python3 scripts/run_moonep_meta_gate.py
```

The gate incrementally configures, builds, and installs the current source, then runs
the checked-in replay meta for `S=4096,K=8,H=7168,EP=8` and
`S=8192,K=16,H=3584,EP=8` in both `group` and `group_credit` mode. Each replay uses
eight ranks on devices 0-7, one measured model-flow iteration, framework profiler
provenance, and the model-flow stage barrier. Successful cases print the existing
`run_moonep.sh` model-versus-replay stage performance table. The final table reports
all four PASS/FAIL results, and any failed row makes the gate exit non-zero.
Meta selection is strict: missing or incompatible checked-in meta fails the row instead
of silently falling through to a full MindSpeed model capture.

Results are retained under `run/moonep/meta-cascade-gate-<timestamp>-<pid>/`, with one
artifact directory per shape and peer mode. By default, the build and install trees
are `build-moonep-meta-gate/` and `install-moonep-meta-gate/`. For a repeated run of
an installation already built by this gate, the only optional argument is:

```bash
python3 scripts/run_moonep_meta_gate.py --skip-build
```

Use the default build path after source changes. Reusing an older install can validate
the wrong binary. By default, the gate clears inherited CANN selection variables and
requires a toolkit installed under the current checkout's `env/cann`; it never falls
back to a system CANN. For a fresh checkout, install it first with:

```bash
bash scripts/cann_download_install.sh
```

To test one explicitly selected toolkit outside the checkout, pass its root directly:

```bash
python3 scripts/run_moonep_meta_gate.py --cann /home/pkg/b101/cann-9.1.0
```

The selected root must contain `set_env.sh`. `--cann` and `--skip-build` are mutually
exclusive so every explicit toolkit test rebuilds the current source. CANN cannot be
selected through `ASCEND_HOME_PATH`, `TILEXR_CANN_HOME`, or other inherited environment
variables; those values are cleared before either the workspace default or `--cann`
path is applied. The gate uses the selected toolkit for both build and replay and starts
every replay from the TileXR source root. These checks prevent an inherited environment
or arbitrary caller working directory from selecting the wrong compiler or failing
before the model replay orchestrator starts.

This meta gate exercises the 55-stage Planning, Dispatch, Prefetch, Combine, reverse
Dispatch, and ReduceGrad order and plan reuse that single-operator tests miss; it does
not launch the full MindSpeed model and therefore is not full-model correctness
evidence. Hardware conclusions remain limited to the actual host, SoC, device set,
CANN, driver, and firmware recorded by that run.

## Utilities

### `plog_grep.sh`
**Purpose**: Filter and search device logs.

**Usage**:
```bash
bash scripts/plog_grep.sh <pattern>
```

**Examples**:
```bash
bash scripts/plog_grep.sh ERROR      # Find all errors
bash scripts/plog_grep.sh WARNING    # Find warnings
bash scripts/plog_grep.sh "AllGather" # Search for specific operation
```

**Log location**: `/var/log/npu/plog/`

### `device_connect.sh`
**Purpose**: Test NPU device connectivity.

**Usage**:
```bash
bash scripts/device_connect.sh
```

### `driver_fix.sh`
**Purpose**: Fix common NPU driver issues.

**Usage**:
```bash
bash scripts/driver_fix.sh
```

**What it does**:
- Resets NPU devices
- Clears driver state

**Requirements**: Root user

### `prepare.sh`
**Purpose**: Complete first-time setup script.

**Usage**:
```bash
bash scripts/prepare.sh
```

**What it does**:
- Checks that Python and pip are available before downloading dependencies
- Installs the PyPI `mermaid-cli` package and its Playwright Chromium browser
- Validates the installed `mmdc` interface and launches Chromium once
- Installs repo-managed local build utilities
- Installs MPICH for multi-rank tests

The Mermaid setup requires access to the configured PyPI index and Playwright browser
CDN. Installation failures stop the script before native dependency downloads begin.

**Use case**: First-time repository setup

## Typical Workflows

### First-time Setup
```bash
# 1. Install CANN
bash scripts/cann_download_install.sh

# 2. Build TileXR
source scripts/common_env.sh
mkdir -p build && cd build
cmake -DCMAKE_INSTALL_PREFIX=../install ..
make -j$(nproc) && make install
```

### Quick Setup (if packages already downloaded)
```bash
bash scripts/prepare.sh
```

### Development Workflow
```bash
# Source environment
source scripts/common_env.sh

# Run tests
bash scripts/test_allreduce.sh

# Check logs
bash scripts/plog_grep.sh ERROR
```

### Debugging
```bash
# Check device status
bash scripts/device_connect.sh

# Fix driver issues
bash scripts/driver_fix.sh

# Search logs
bash scripts/plog_grep.sh "your_search_term"
```

## Notes

The optional Relay builder is `build_tilexr_relay.py` for ABI-v1 Python operators
and content-addressed external ELF/Plan pairs. It requires the separately pinned
TileLang Ascend compiler and CANN environment, not root or package installation.
The production Host runtime never invokes it implicitly. See
[TileLang integration](../docs/TILELANG_INTEGRATION.md#tilerelay-core-python-defined-operators).

- **Root user required**: Most scripts need root access for NPU device operations
- **Source common_env.sh**: Always source before building or testing
- **CANN version**: Default is 9.1.0, configurable via `TILEXR_CANN_VER`
- **Submodules**: Ensure `git submodule update --init --recursive` has been run
