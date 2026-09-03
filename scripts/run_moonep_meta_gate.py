#!/usr/bin/env python3

from __future__ import annotations

import argparse
import os
import subprocess
import sys
import time
from pathlib import Path
from typing import Callable, Mapping, NamedTuple, Sequence, TextIO


class GateCase(NamedTuple):
    tokens_per_rank: int
    topk: int
    hidden_size: int
    peer_mode: str

    @property
    def label(self) -> str:
        return (
            f"S={self.tokens_per_rank} K={self.topk} H={self.hidden_size}"
        )

    @property
    def artifact_name(self) -> str:
        return (
            f"s{self.tokens_per_rank}-k{self.topk}-h{self.hidden_size}"
            f"-{self.peer_mode}"
        )


class GateResult(NamedTuple):
    case: GateCase
    returncode: int
    elapsed_seconds: float
    output_dir: Path


CASES = (
    GateCase(4096, 8, 7168, "group"),
    GateCase(4096, 8, 7168, "group_credit"),
    GateCase(8192, 16, 3584, "group"),
    GateCase(8192, 16, 3584, "group_credit"),
)

InvokeCase = Callable[[GateCase, list[str], Mapping[str, str]], int]


def build_replay_command(
    source_root: Path,
    case: GateCase,
    output_dir: Path,
    cache_dir: Path,
) -> list[str]:
    del output_dir
    return [
        "bash",
        str(source_root / "scripts" / "run_moonep.sh"),
        "--mode",
        "benchmark",
        "--rank-size",
        "8",
        "--visible-devices",
        "0,1,2,3,4,5,6,7",
        "--warmup",
        "0",
        "--iterations",
        "1",
        "--model-replay",
        "--model-replay-from",
        "meta",
        "--model-replay-cache-dir",
        str(cache_dir),
        "--model-replay-profile",
        "--model-replay-stage-summary-only",
        "--s",
        str(case.tokens_per_rank),
        "--k",
        str(case.topk),
        "--hidden-size",
        str(case.hidden_size),
        "--ep",
        "8",
    ]


def _invoke_subprocess(
    case: GateCase,
    command: list[str],
    environment: Mapping[str, str],
) -> int:
    del case
    return subprocess.run(
        command,
        cwd=environment.get("TILEXR_HOME"),
        env=dict(environment),
        check=False,
    ).returncode


def _case_environment(
    source_root: Path,
    output_dir: Path,
    base_environment: Mapping[str, str],
    case: GateCase,
) -> dict[str, str]:
    environment = dict(base_environment)
    environment.update(
        {
            "TILEXR_ENABLE_CREDIT_IPC": "1",
            "TILEXR_HOME": str(source_root),
            # A meta gate must fail instead of falling through to a full model capture.
            "TILEXR_MODEL_REPLAY_BASH": "/bin/false",
            "TILEXR_MOONEP_DIAGNOSTIC_FLOW": "0",
            "TILEXR_MOONEP_DISPATCH_GROUP_WIDTH": "16",
            "TILEXR_MOONEP_DISPATCH_PEER_MODE": case.peer_mode,
            "TILEXR_MOONEP_DUMP_DFX_ON_ERROR": "0",
            "TILEXR_MOONEP_MODEL_REPLAY_META_ROOT": str(
                source_root / "tools" / "moonep" / "model_replay_meta"
            ),
            "TILEXR_MOONEP_OUTPUT_DIR": str(output_dir),
            "TILEXR_MOONEP_TIMEOUT_SEC": "1800",
            "TILEXR_MOONEP_TRACE_STAGES": "0",
        }
    )
    environment.pop("TILEXR_MOONEP_FLAG_DUMP_DIR", None)
    environment.pop("TILEXR_MOONEP_FLAG_DUMP_MODE", None)
    return environment


def run_case_matrix(
    source_root: Path,
    result_root: Path,
    cache_dir: Path,
    base_environment: Mapping[str, str],
    *,
    invoke: InvokeCase = _invoke_subprocess,
    output: TextIO = sys.stdout,
) -> list[GateResult]:
    results = []
    for index, case in enumerate(CASES, start=1):
        output_dir = result_root / case.artifact_name
        command = build_replay_command(source_root, case, output_dir, cache_dir)
        environment = _case_environment(
            source_root, output_dir, base_environment, case
        )
        print(
            f"\n[{index}/{len(CASES)}] {case.label} mode={case.peer_mode}",
            file=output,
            flush=True,
        )
        started = time.monotonic()
        try:
            returncode = invoke(case, command, environment)
        except OSError as exc:
            print(f"Failed to launch case: {exc}", file=output, flush=True)
            returncode = 127
        results.append(
            GateResult(
                case=case,
                returncode=returncode,
                elapsed_seconds=time.monotonic() - started,
                output_dir=output_dir,
            )
        )
    return results


def format_result_table(results: Sequence[GateResult]) -> str:
    headers = ("Shape", "Mode", "Result", "Elapsed", "Artifacts")
    rows = [
        (
            result.case.label,
            result.case.peer_mode,
            "PASS" if result.returncode == 0 else f"FAIL({result.returncode})",
            f"{result.elapsed_seconds:.1f}s",
            str(result.output_dir),
        )
        for result in results
    ]
    widths = [
        max(len(headers[index]), *(len(row[index]) for row in rows))
        for index in range(len(headers))
    ]
    lines = [
        "  ".join(value.ljust(widths[index]) for index, value in enumerate(row))
        for row in (
            headers,
            tuple("-" * width for width in widths),
            *rows,
        )
    ]
    return "\n".join(lines)


def gate_exit_code(results: Sequence[GateResult]) -> int:
    return 0 if len(results) == len(CASES) and all(
        result.returncode == 0 for result in results
    ) else 1


def _load_environment_script(
    script: Path, inherited: Mapping[str, str]
) -> dict[str, str]:
    completed = subprocess.run(
        [
            "bash",
            "-c",
            'source "$1" >&2 && env -0',
            "moonep-meta-gate",
            str(script),
        ],
        env=dict(inherited),
        check=True,
        stdout=subprocess.PIPE,
    )
    environment = {}
    for item in completed.stdout.split(b"\0"):
        if not item or b"=" not in item:
            continue
        name, value = item.split(b"=", 1)
        environment[os.fsdecode(name)] = os.fsdecode(value)
    return environment


def _load_common_environment(
    source_root: Path, inherited: Mapping[str, str]
) -> dict[str, str]:
    return _load_environment_script(
        source_root / "scripts" / "common_env.sh", inherited
    )


def _prepare_gate_environment(
    source_root: Path,
    install_dir: Path,
    inherited: Mapping[str, str],
    *,
    cann_path: Path | None = None,
) -> dict[str, str]:
    sanitized = dict(inherited)
    for name in (
        "ASCEND_AICPU_PATH",
        "ASCEND_HOME_PATH",
        "ASCEND_OPP_PATH",
        "ASCEND_TOOLKIT_HOME",
        "TILEXR_CANN_HOME",
    ):
        sanitized.pop(name, None)

    if cann_path is not None:
        requested = cann_path.expanduser()
        try:
            resolved = requested.resolve(strict=True)
        except OSError as exc:
            raise RuntimeError(
                f"explicit CANN path does not exist: {requested}"
            ) from exc
        set_env = resolved / "set_env.sh"
        if not resolved.is_dir() or not set_env.is_file():
            raise RuntimeError(
                f"explicit CANN toolkit must contain set_env.sh: {resolved}"
            )

    environment = dict(_load_common_environment(source_root, sanitized))
    if cann_path is not None:
        environment = _load_environment_script(set_env, environment)
    else:
        cann_version = environment.get("TILEXR_CANN_VER", "9.1.0")
        local_cann_parent = source_root / "env" / "cann"
        cann_candidates = (
            local_cann_parent / "cann",
            local_cann_parent / f"cann-{cann_version}",
        )
        for candidate in cann_candidates:
            if not (candidate / "set_env.sh").is_file():
                continue
            resolved = candidate.resolve()
            try:
                resolved.relative_to(local_cann_parent.resolve())
            except ValueError:
                continue
            if not local_cann_parent.is_dir() or local_cann_parent.is_symlink():
                continue
            break
        else:
            raise RuntimeError(
                "TileXR-local CANN 9.1.0 is required under env/cann; "
                "run 'bash scripts/cann_download_install.sh' first"
            )
    environment["ASCEND_HOME_PATH"] = str(resolved)
    environment["TILEXR_CANN_HOME"] = str(resolved)
    environment["TILEXR_INSTALL_PREFIX"] = str(install_dir)
    return environment


def _build_current_source(
    source_root: Path,
    build_dir: Path,
    install_dir: Path,
    environment: Mapping[str, str],
) -> None:
    configure = [
        "cmake",
        "-S",
        str(source_root),
        "-B",
        str(build_dir),
        f"-DCMAKE_INSTALL_PREFIX={install_dir}",
        "-DTILEXR_BUILD_MOONEP=ON",
        "-DTILEXR_BUILD_TESTS=OFF",
        "-DBUILD_TESTING=OFF",
        "-DTILEXR_MOONEP_DISPATCH_ENABLE_DFX=OFF",
        "-DTILEXR_MOONEP_DISPATCH_ENABLE_PROFILING=OFF",
        "-DTILEXR_MOONEP_COMBINE_V2_ENABLE_PROFILING=OFF",
    ]
    jobs = str(max(1, os.cpu_count() or 1))
    for command in (
        configure,
        ["cmake", "--build", str(build_dir), f"-j{jobs}"],
        ["cmake", "--install", str(build_dir)],
    ):
        subprocess.run(command, env=dict(environment), check=True)


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description=(
            "Build TileXR and run the four single-node 8-rank MoonEP meta cascade cases"
        )
    )
    build_options = parser.add_mutually_exclusive_group()
    build_options.add_argument(
        "--cann",
        type=Path,
        metavar="PATH",
        help="use the explicitly selected CANN toolkit root containing set_env.sh",
    )
    build_options.add_argument(
        "--skip-build",
        action="store_true",
        help="reuse install-moonep-meta-gate from an earlier successful gate build",
    )
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    arguments = list(sys.argv[1:] if argv is None else argv)
    args = _parser().parse_args(arguments)

    source_root = Path(__file__).resolve().parents[1]
    build_dir = source_root / "build-moonep-meta-gate"
    install_dir = source_root / "install-moonep-meta-gate"
    timestamp = time.strftime("%Y%m%d-%H%M%S")
    result_root = (
        source_root
        / "run"
        / "moonep"
        / f"meta-cascade-gate-{timestamp}-{os.getpid()}"
    )
    cache_dir = result_root / "cache"

    required_paths = (
        source_root / "scripts" / "common_env.sh",
        source_root / "scripts" / "run_moonep.sh",
        source_root / "tools" / "moonep" / "model_replay_meta",
    )
    missing = [str(path) for path in required_paths if not path.exists()]
    if missing:
        print(f"required gate path is missing: {missing[0]}", file=sys.stderr)
        return 1

    try:
        environment = _prepare_gate_environment(
            source_root,
            install_dir,
            os.environ,
            cann_path=args.cann,
        )
        if args.skip_build:
            if not install_dir.is_dir():
                print(
                    f"--skip-build requires an existing install: {install_dir}",
                    file=sys.stderr,
                )
                return 1
        else:
            print(f"Building current source into {install_dir}", flush=True)
            _build_current_source(
                source_root, build_dir, install_dir, environment
            )
    except (OSError, RuntimeError, subprocess.CalledProcessError) as exc:
        print(f"MoonEP gate build failed: {exc}", file=sys.stderr)
        return 1

    result_root.mkdir(parents=True, exist_ok=False)
    print(f"Gate results: {result_root}", flush=True)
    results = run_case_matrix(
        source_root,
        result_root,
        cache_dir,
        environment,
    )
    print("\nMoonEP meta cascade gate summary")
    print(format_result_table(results))
    print(f"Result root: {result_root}")
    return gate_exit_code(results)


if __name__ == "__main__":
    raise SystemExit(main())
