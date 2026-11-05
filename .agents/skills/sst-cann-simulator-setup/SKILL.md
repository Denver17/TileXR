---
name: sst-cann-simulator-setup
description: Use whenever the user asks to execute this skill, deploy the simulator platform, set up or verify SST+CANN, or simulate, compile, run, debug, or validate any TileXR Ascend C operator on UB_Simulator/UB_MicroBenchmark, including single-node, cross-node, UDMA, combine, dispatch, allreduce, or new operator testcases.
---

# SST CANN Simulator Setup

## Overview

Use this as the default end-to-end workflow for SST/CANN simulator hosts and TileXR operator simulation. When the user says "deploy the simulator" or "simulate this operator", execute this skill directly: gather only missing connection/path facts, prepare the environment, build a simulator-compatible object, create a self-contained testcase, run SST, and compare outputs before reporting.

If the user provides enough connection, package, or operator context, treat that as a complete request. Do not ask the user to write a detailed prompt. Ask only for missing essentials such as target host, username, authentication method, target home, package source, operator source path, entry function, testcase template, or expected-output rule.

## Quick Rules

- Install SST as the target ordinary user, not root. If root previously installed into the user's home, fix ownership or rebuild as the user.
- Keep SST Core and SST Elements versions matched unless the user explicitly asks otherwise. For the known simulator setup, use `sstcore-14.0.0` with `sstelements-14.0.0`.
- Install only requested CANN versions. The known simulator baseline is CANN 9.1.0; treat every CANN 8.x object as incompatible unless a separate version-matched build and simulation proves otherwise. Ignore unrelated MindStudio or other 8.x packages unless the user asks.
- Always save logs under the target home, for example `~/sst-build-logs` and `~/cann-install-logs`.
- Verify using commands that compile or run something, not just directory listings.
- Treat `sst` exit 0 as necessary but not sufficient for operator correctness. Inspect output bins and compare against expected values.
- Keep copied testcases self-contained: rewrite absolute `kernel_file` and `input_bin` paths after copying a testcase.
- Preserve the exact kernel ABI expected by the YAML `bin_param` area. Derive it from the kernel entry signature and existing testcase YAML, not from the operator name.
- For V310 simulator runs, prefer `dav-c310-vec` objects. If a full upstream object compiled for another arch fails with `IfuL1: Unsupported instructions`, treat it as an object/simulator compatibility issue and build a simulator-friendly split/shim object before continuing validation.
- Do not hard-code personal usernames, employee IDs, or one user's home path in reusable commands. Use `<home>`, `<user>`, `<sim-root>`, or variables such as `$SIM_ROOT` unless the user explicitly provides a concrete path for the current run.

## Deployment Workflow

1. Read `references/sst-cann-runbook.md`.
2. Confirm the deployment inputs. If target host, username, authentication, target home, or package source is missing, ask only for the missing item. If they are present, proceed without additional prompting.
3. Confirm SSH login, OS, disk, compiler tools, package list, and target ownership.
4. For SST, connect as the ordinary user and build/install into that user's home.
5. For CANN, download required packages from `https://clouddrive.huawei.com/p/b8c7099badf08d2421062e90cfdac5fb` when not already present, place them under `~/pkg`, then inspect each `.run --help`; use `--full --quiet --install-path=<target>` when supported.
6. Create environment scripts:
   - `~/sst-env.sh`
   - `~/cann-env.sh`
7. Run the smoke tests from the runbook and report exact pass/fail evidence.

## Operator-to-Simulator Handoff Flow

Use this as the PPT-derived checklist when adapting a new operator testcase to the simulator platform.

1. Compile the operator `.o` and identify the exact kernel entry function.
   - Confirm the entry name in the object and set `vec_func` to that function, not merely to the operator folder name.
   - For V310, confirm the object architecture before running SST.
2. Design kernel inputs from the operator ABI and control flow.
   - List every scalar and GM pointer argument in order.
   - For each scalar, write the intended value and which branch/check it drives.
   - For each GM pointer, write address, size, whether it needs `input_bin`, and whether the kernel writes it.
   - For nested pointers inside structs such as `CommArgs`, generate the pointed-to bins too; creating only the top-level struct bin is incomplete when the kernel dereferences internal pointers.
3. Allocate address space before writing bins.
   - Each rank has a 128GB GM address space; use `rankId * gm_size` as the rank's system VA base.
   - `gm_size` is normally `128GB` and should not be changed.
   - Prefer placing custom input/output bins in the safe GM data range from `8GB` to `120GB` within each rank, then add `rankId * gm_size`.
   - Compute every bin size up front and leave gaps/alignment so regions cannot overlap.
4. Write `workload_config.yml`; this is the main simulator driver file.
   - `chip*` describes each rank/card workload.
   - `gm_size` is per-rank GM size.
   - `sys_va_base_addr = rankId * gm_size`.
   - `param_base_addr = rankId * gm_size + 2GB` in the common layout.
   - `ub_base_addr`, `stack_base_addr`, and `bin_param` are usually inherited from the template.
   - `block_dim` maps to the kernel launch block dimension and controls visible `GetBlockIdx()/GetBlockNum()` behavior.
   - `kernel_param` must preserve ABI order. Names are labels only; `param_offset` and `param_type` determine the actual argument layout.
   - `output_param` defines debug dump regions and should include every output or workspace region needed for validation.
5. Generate parameters and bins.
   - Numeric params use `value`.
   - Address params use `value` for the GM address and optional `input_bin` when memory must be prefilled.
   - `param_offset` starts at `0` and increments by each previous argument's ABI size/type width.
   - Address-only scratch spaces may omit `input_bin`, but prefilled structs, routing tables, queues, registries, workspaces, and known inputs must provide it.
6. Run simulation from the testcase directory:
   ```bash
   cd <testcase-dir>
   source <home>/sst-env.sh
   timeout 600 sst -n 20 ./test_UBusSim.py > run.log 2>&1
   ```
   Or use the wrapper when appropriate:
   ```bash
   cd <sim-root>/UB_Simulator
   source <home>/sst-env.sh
   python UBusSim_single_run.py --testcase_file=Testcases/<exp_dir>
   ```
7. Debug with dumps, not guesses.
   - Add `output_param` regions for any GM area the kernel writes or branches on.
   - Inspect `dumps`/`bin` raw die files after SST.
   - Run `output_generator.py` only when the testcase has a valid local script; otherwise parse raw die bins directly.
   - Use kernel-side debug writes or `AscendC::printf` sparingly to prove branch entry, then remove or isolate debug-only code before final validation.
8. For performance work, keep functional validation first, then inspect trace data with `chrome://tracing/` when a `trace.json` is produced.

## Default Operator Simulation Flow

Use this for any TileXR Ascend C operator, not only dispatch or combine.

1. Identify the operator entry and ABI:
   - Locate the `extern "C" __global__ __aicore__` entry.
   - Map each YAML `kernel_param` to entry arguments by `param_offset`, `param_type`, pointer/scalar width, and `bin_param.size`.
   - Confirm `vec_func` names the intended entry exactly.
2. Choose or create a testcase template:
   - Prefer an existing similar testcase under `<sim-root>/UB_Simulator/Testcases`.
   - Copy it to a timestamped directory and rewrite every absolute path to the new testcase.
   - Keep input bins, expected bins, comparison scripts, and copied `.o` files inside the testcase whenever possible.
3. Build a simulator-compatible object:
   - First try the repo's normal build if it already targets the simulator arch.
   - For V310, verify the object has C310 ELF flags (`readelf -h <obj>` usually shows `Flags: 0x990000`).
   - If the full object is C220 (`Flags: 0x930000`) or fails at IFU fetch, compile a narrow `dav-c310-vec` object through UB_MicroBenchmark using the same entry ABI. Be explicit that this validates the simulator/testcase/data path, not the full upstream object.
4. Run SST from the testcase directory:
   ```bash
   cd <testcase-dir>
   source <home>/sst-env.sh
   timeout 600 sst -n 20 ./test_UBSim.py > run.log 2>&1
   python3 output_generator.py v310_config > output_generator.log 2>&1
   ```
   Use longer timeouts, such as `900`, for cross-node or UDMA flows.
5. Validate output:
   - Compare merged output bins against expected bins with a testcase-local Python script.
   - Also inspect raw die outputs. If die1 is correct but merged output is zero, patch or bypass the local `output_generator.py` merge rule so zero blocks do not overwrite nonzero blocks at the same address.
   - Report testcase path, command, `SST_RC`, generator/compare exit code, source path, object path, entry function, and comparison evidence.

## Combine UDMA Publish/Drain Numeric Flow

Use this when validating `tilexr_ep_combine_kernel` UDMA/simulator-direct UDMA paths, especially 2P or 4P nonzero combine cases. The decisive check is final `yOut`, not only `run.log`.

1. Build the operator object before creating fresh cases:
   ```bash
   cd <sim-root>/UB_MicroBenchmark
   source <home>/cann-env.sh 2>/dev/null || true
   bash toolsh/build_operators.sh
   ```
2. Generate or copy a self-contained publish testcase and a drain testcase under:
   ```bash
   <sim-root>/UB_Simulator/Testcases/tileXrCombine
   ```
   Keep `kernel_file`, `input_bin`, `vec_func`, and every rank-local path pointing at the testcase being run.
3. For publish:
   - Set `phase=0`.
   - Provide nonzero `expertOutGM`, `assistInfoForCombineGM`, and `epRecvCountsGM`.
   - For a simple 4P smoke with `bs=4, h=8, topK=2`, use source payload values rank0 `1.0` (`0x3c00`), rank1 `2.0` (`0x4000`), rank2 `3.0` (`0x4200`), rank3 `4.0` (`0x4400`), with two routes per token.
   - Run:
     ```bash
     cd <publish-testcase>
     source <home>/sst-env.sh
     timeout 900 sst -n 30 ./test_UBusSim.py > run.log 2>&1
     ```
4. Reconstruct drain workspace from publish raw dumps:
   - Raw simulator dump records are `8-byte little-endian GM address + 512-byte payload`.
   - Merge both `david_<rank>_die1_workspace.bin` and `david_<rank>_die2_workspace.bin` by record address. For rank-local comparisons, use local offsets when global addresses differ by rank GM base.
   - Feed the merged image to the drain testcase as `workspaceGM.input_bin`.
   - Ensure the status word at `workspaceGM + UDMAStatusOffset(totalBytes, rankSize, slotBytes)` is `uint64 0` (`TileXREp::kEpStatusOk`); otherwise drain can return early with `tilexr_ep_combine drain skipped because status is non-zero`.
5. For drain:
   - Set `phase=1`.
   - Keep `workspaceGM` non-null and pointing at the merged publish workspace.
   - Keep `CommArgs.extraFlag` UDMA bit set and `udmaRegistryPtr`/`udmaInfoPtr` non-null when the kernel requires `UDMARegistryEnabled(args)`.
   - Run:
     ```bash
     cd <drain-testcase>
     source <home>/sst-env.sh
     rm -rf bin OUTPUT dumps
     timeout 900 sst -n 30 ./test_UBusSim.py > run.log 2>&1
     grep -nE 'Simulation is complete|drain skipped|FATAL|ERROR|timeout|DBG_COMBINE' run.log | tail -160
     ```
6. Validate `yOut` numerically from raw die dumps when no reliable `output_generator.py` is present:
   - Do not treat the first `output_param.yOut.size` bytes after the 8-byte address header as authoritative.
   - Search the 512-byte payload for the expected logical `yOut` byte pattern and report the offset found. In the known `yOutGM=0x300020a00`, `bs=4`, `h=8` combine cases, the valid 64B result was at payload offset `384` in `david_<rank>_die2_yOut.bin`; payload offset `0` was all zero.
   - Cross-check with a known-passing 2P case before declaring a 4P offset suspicious.

Example raw `yOut` validator for the 4P smoke:

```bash
cd <drain-testcase>
python3 - <<'PY'
from pathlib import Path
import struct

TC = Path(".")
bs, h = 4, 8
expected_rows = [0x4000, 0x4400, 0x4600, 0x4800]  # 2.0, 4.0, 6.0, 8.0
expected = b"".join(struct.pack("<H", x) * h for x in expected_rows)

overall = True
for rank in range(4):
    best = None
    for die in (1, 2):
        p = TC / "bin" / f"david_{rank}_die{die}_yOut.bin"
        if not p.exists():
            continue
        b = p.read_bytes()
        if len(b) < 8:
            continue
        addr = struct.unpack_from("<Q", b, 0)[0]
        payload = b[8:]
        off = payload.find(expected)
        if off >= 0:
            best = (die, addr, off, payload[off:off + len(expected)])
            break
    ok = best is not None
    overall = overall and ok
    print(f"RANK {rank}: match={ok}", end="")
    if ok:
        die, addr, off, data = best
        words = [struct.unpack_from("<H", data, i)[0] for i in range(0, len(data), 2)]
        print(f" die={die} raw_addr={hex(addr)} payload_offset={off}")
        for token in range(bs):
            row = words[token * h:(token + 1) * h]
            print("  token", token, [hex(x) for x in row])
    else:
        print()
print("OVERALL_MATCH", overall)
PY
```

Expected passing evidence for the 4P nonzero smoke is `SST_RC=0`, `Simulation is complete`, no `drain skipped`/`FATAL`/`ERROR`, and `OVERALL_MATCH True` with every rank producing token rows `0x4000`, `0x4400`, `0x4600`, `0x4800`.

## TileXR Simulator Flow

Use this after the SST/CANN host is already installed.

1. Confirm expected directories:
   ```bash
   ls -ld <sim-root>/UB_Simulator <sim-root>/UB_MicroBenchmark
   find <sim-root>/UB_Simulator/Testcases -maxdepth 1 -type d | sort | tail
   ```
2. If testcase needs local copies, copy `UB_Simulator` and `UB_MicroBenchmark` under `<home>/temp`, then update CMake/YAML paths.
3. If `lib/*.tar.gz` exists in copied simulator or benchmark trees, extract it before building or running.
4. Source `<home>/sst-env.sh` before every simulator run.
5. Run testcase-local scripts from the testcase directory, not from another cwd.
6. Never rely on stale `run.status`, stale `compare.log`, or paths pointing to another user's home directory.

For testcase wrappers:

```bash
cd <sim-root>/UB_Simulator
source <home>/sst-env.sh
python UBSim_single_run.py --testcase_file=Testcases/<testcase-name>
```

## Compiling Operator Objects

Prefer UB_MicroBenchmark for simulator-friendly `.o` files for any operator:

1. Put the source folder under `<sim-root>/UB_MicroBenchmark/src/operator/<category>/<operator-folder>`.
2. Copy `<sim-root>/UB_MicroBenchmark/src/operator/make.sh` into place only if the workflow expects a local copy.
3. Run:
   ```bash
   cd <sim-root>/UB_MicroBenchmark
   source <home>/cann-env.sh 2>/dev/null || true
   bash toolsh/build_operators.sh <name-filter>
   ```
4. Check the emitted object in `<sim-root>/UB_MicroBenchmark/toolsh/kernels/`.
5. Verify entry symbols and architecture before using it in YAML:
   ```bash
   strings <kernel.o> | grep -E '<entry-name>|tilexr_|TileXR'
   readelf -sW <kernel.o> | grep '<entry-name>'
   readelf -h <kernel.o> | grep Flags
   ```

For a direct `ccec` smoke compile requested by the user, keep the user-specific paths as variables:

```bash
cd <home>/tests
source set_env.sh
<ascend-home>/x86_64-linux/ccec_compiler/bin/ccec \
  -std=c++17 -g -c -xcce -O2 test.cc -o ./smoke_aivUrma.o \
  --cce-aicore-arch=dav-c310 --cce-aicore-only \
  -mllvm -cce-aicore-function-stack-size=0x4000 \
  -mllvm -cce-aicore-addr-transform \
  -mllvm -cce-aicore-or-combine=false \
  -mllvm -instcombine-code-sinking=false \
  -Xclang -fcce-vf-vl=256 \
  --cce-auto-sync=off \
  -mllvm -cce-aicore-jump-expand=true \
  -mllvm -cce-aicore-mask-opt=false \
  -ferror-limit=0 -w -DENABLE_MOVVP -D PERF_API_NORM \
  -mllvm -api-deps-filter
test -s ./smoke_aivUrma.o
```

Use concrete paths such as `/home/<user>/Ascend/9.1.0` only when the user provided them for the current machine.

For full upstream objects:

- Inspect the actual compile command and architecture in `flags.make` or build logs.
- `-DAIV_ARCH=...` may not override a non-cache `set(AIV_ARCH ...)` in CMake. Verify the generated `--cce-aicore-arch=...` instead of assuming the option worked.
- Do not keep bisecting function bodies after an empty entry still fails with IFU unsupported. Map the PC to symbol offsets with `readelf -sW`; if the fault is inside an empty function prologue, change the object architecture/build route.

Known entries:

- Dispatch single-node: `tilexr_ep_dispatch_kernel`
- Dispatch cross-node: `tilexr_ep_dispatch_cross_node_kernel`
- Combine single-node: `tilexr_ep_combine_kernel`
- Combine cross-node: `tilexr_ep_combine_cross_node_kernel`
- AllReduce int smoke: `TileXRAllReduce_int`

For collectives such as allreduce:

- `TileXRAllReduce_int` uses argument order:
  `input, output, commArgs, len, magic, op, root, cycleCount, scale, scaleCount, offset, perfTrace`.
- A 2-rank int32 smoke can use rank0 input `[1,2,3,4]`, rank1 input `[10,20,30,40]`, and expected output `[11,22,33,44]` on both ranks.
- The full collectives CMake path may build `dav-c220-vec` objects that V310 simulator rejects before operator logic executes. The combine-style path is to compile a narrow `dav-c310-vec` split/shim object with the same ABI, then validate the testcase and data path.

## Common Mistakes

| Symptom | Likely cause | Action |
|---|---|---|
| `kernel_operator.h file not found` | `set_env.sh` points to an old CANN path | Fix `ASCEND_TOOLKIT_HOME` to `~/Ascend/latest/x86_64-linux` or `~/Ascend/9.1.0/x86_64-linux` |
| CANN `.run --version` complains about `/root/log/makeself` | `/root/log` is a file, not a directory | Back up `/root/log`, then create `/root/log/makeself` |
| `opp` install says only one operation type is supported | `--check-path` was combined with `--full` | Install OPP without `--check-path`; use `--check-path` separately if needed |
| `sst-info | head` exits 141 | `pipefail` plus SIGPIPE from `head` | Redirect to a file, then inspect with `sed` or `grep` |
| OpenMPI prints OpenFabrics warnings | Host has verbs devices not configured for this run | Set `OMPI_MCA_btl='^openib'` in `sst-env.sh` |
| `IfuL1: Unsupported instructions` | `.o` architecture or instruction sequence is unsupported by V310 simulator | Recompile with simulator-compatible flags or a narrow `dav-c310-vec` split/shim object |
| SST exits 0 but merged bins are all zero | Kernel returned early or output merge overwrote valid raw die output | Inspect raw `david_*_die*.bin`, YAML paths, null pointer params, and merge script |
| `compare.log` references another user's home path | Stale copied testcase compare path | Rewrite compare paths or run a local explicit Python comparison |
| Cross-node hangs in UDMA/CQ polling | Missing or fake UDMA queue metadata | Verify `CommArgs` UDMA fields and queue buffers; do not report as math mismatch |

## Verification Gate

Before claiming completion, show:

- SST: `sst --version`, `sst-test-core` summary, and an Elements example run.
- CANN: all installed package `version.info` values, `atc --help` without missing libraries, and a `ccec` smoke compile if `test.cc` is available.
- Ownership: `stat -c '%U:%G %n'` for the key install dirs.
- TileXR operator run: exact testcase directory, exact command, `SST_RC`, generator/compare exit code, key `run.log` lines, object path, entry function, and output comparison evidence.
