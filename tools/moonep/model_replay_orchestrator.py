from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shlex
import subprocess
import sys
import time
import uuid
from dataclasses import dataclass, replace
from pathlib import Path
from typing import Callable, Mapping, Sequence

from .mindspeed.build_route_replay import build_route_replay, write_route_replay
from .mindspeed.collect_model_performance import collect_model_performance
from .model_flow import model_operator_order
from .model_replay_cache import (
    CacheEntry,
    CacheValidationError,
    ReplayShape,
    begin_cache_capture,
    build_cache_identity,
    cache_key,
    find_cache,
    publish_cache,
    validate_cache_entry,
)
from .model_replay_meta import (
    MetaValidationError,
    load_meta_bundle,
    materialize_meta_cache,
    write_meta_bundle,
)


RouteCapture = Callable[[Path], None]
PerformanceCapture = Callable[[Path, dict[str, object]], None]


_DEFAULT_MODEL_ROOT_CANDIDATES = (
    "/home/c30061605/ai/TileXR/run/multinode-validation/model",
)
_DEFAULT_CANN_ENV_CANDIDATES = (
    "/home/pkg/b131/cann-9.1.0/set_env.sh",
    "/home/pkg/b131/cann/set_env.sh",
)
_DEFAULT_CONDA_SH_CANDIDATES = (
    "/home/miniconda3/etc/profile.d/conda.sh",
    "/home/anaconda3/etc/profile.d/conda.sh",
)
_DEFAULT_TOKENIZER_PATH_CANDIDATES = (
    "/home/dataset/deepseek3",
)


@dataclass(frozen=True)
class PreparedReplayCache:
    entry: CacheEntry
    cache_status: str
    meta_path: Path | None = None
    meta_status: str = "not-checked"

    @property
    def route_replay_path(self) -> Path:
        return self.entry.path / "replay" / "route_replay.json"

    @property
    def model_performance_path(self) -> Path:
        return self.entry.path / "model" / "performance.json"


@dataclass(frozen=True)
class ModelRunnerSettings:
    source_root: Path
    config_path: Path
    shape: ReplayShape
    node_count: int
    timeout_seconds: int
    profiler_enabled: bool
    stage_barrier: bool = True
    bash_executable: str = "bash"
    master_port: int | None = None
    rank_table_file: Path | None = None

    def __post_init__(self) -> None:
        if self.node_count <= 0 or self.timeout_seconds <= 0:
            raise ValueError("node count and model timeout must be positive")
        if self.shape.world_size % self.node_count != 0:
            raise ValueError("world size must be divisible by node count")
        if self.master_port is not None and not 1 <= self.master_port <= 55534:
            raise ValueError("model master port must be in [1, 55534]")
        if self.rank_table_file is not None and not self.rank_table_file.is_file():
            raise ValueError(
                f"model rank table file not found: {self.rank_table_file}"
            )

    @property
    def mode(self) -> str:
        return "single" if self.node_count == 1 else "multi"

    @property
    def runner_path(self) -> Path:
        return self.source_root / "tools" / "moonep" / "mindspeed" / "run_model.sh"

    def base_command(self, *, run_tag: str) -> list[str]:
        master_port = self.master_port
        if master_port is None:
            digest = hashlib.sha256(run_tag.encode("utf-8")).digest()
            master_port = 15000 + int.from_bytes(digest[:4], "big") % 10000
        command = [
            self.bash_executable,
            str(self.runner_path),
            "--mode",
            self.mode,
            "--backend",
            "tilexr",
            "--seq-length",
            str(self.shape.tokens_per_rank),
            "--hidden-size",
            str(self.shape.hidden_size),
            "--moe-router-topk",
            str(self.shape.topk),
            "--config",
            str(self.config_path),
            "--run-tag",
            run_tag,
            "--master-port",
            str(master_port),
            "--timeout",
            str(self.timeout_seconds),
        ]
        if self.rank_table_file is not None:
            command.extend(("--rank-table-file", str(self.rank_table_file)))
        if self.stage_barrier:
            command.append("--stage-barrier")
        if self.node_count > 1:
            command.append("--collect-artifacts")
        return command

    def model_artifact_root(self, run_tag: str) -> Path:
        root = self.source_root / "run" / "moonep" / "mindspeed" / run_tag
        if self.node_count > 1:
            return root / "collected"
        return root / "tilexr" / "node_0"


def _identity_source_git_sha(identity: Mapping[str, object]) -> str:
    provenance = identity.get("provenance")
    if not isinstance(provenance, Mapping):
        raise ValueError("cache identity provenance is missing")
    value = provenance.get("tilexr_git_sha")
    if not isinstance(value, str) or not value:
        raise ValueError("cache identity TileXR git SHA is missing")
    return value


def prepare_replay_cache(
    cache_root: str | Path,
    identity: Mapping[str, object],
    shape: ReplayShape,
    *,
    capture_id: str,
    source: str = "cache",
    capture_routes: RouteCapture,
    capture_performance: PerformanceCapture,
    meta_root: str | Path | None = None,
) -> PreparedReplayCache:
    if identity.get("shape") != shape.as_dict():
        raise ValueError("cache identity shape does not match replay shape")
    if source not in ("cache", "meta", "model"):
        raise ValueError(f"invalid model replay source: {source}")
    prepared, previous, rejected_meta = _lookup_existing_replay_cache(
        cache_root,
        identity,
        shape,
        capture_id=capture_id,
        source=source,
        meta_root=meta_root,
    )
    if prepared is not None:
        return prepared

    staging = begin_cache_capture(cache_root, identity, capture_id=capture_id)
    raw_capture = staging / "capture" / "routes"
    raw_capture.mkdir(parents=True)
    capture_routes(raw_capture)

    replay = build_route_replay(
        raw_capture,
        capture_id=capture_id,
        source_git_sha=_identity_source_git_sha(identity),
        world_size=shape.world_size,
        forward_calls=shape.forward_calls,
        tokens_per_rank=shape.tokens_per_rank,
        topk=shape.topk,
        expert_count=shape.expert_count,
        hidden_size=shape.hidden_size,
        ep_size=shape.ep_size,
        ffn_hidden_size=shape.ffn_hidden_size,
        token_padding=shape.token_padding,
    )
    route_path = staging / "replay" / "route_replay.json"
    write_route_replay(route_path, replay)

    performance_path = staging / "model" / "performance.json"
    performance_path.parent.mkdir(parents=True)
    capture_performance(performance_path, replay)
    meta_path = None
    meta_status = "disabled"
    if meta_root is not None:
        with performance_path.open("r", encoding="utf-8") as handle:
            performance = json.load(handle)
        if not isinstance(performance, dict):
            raise ValueError("model performance must be a JSON object")
        bundle = write_meta_bundle(
            meta_root,
            shape,
            replay,
            performance,
            identity,
            replace=source == "model" or rejected_meta,
        )
        meta_path = bundle.path
        meta_status = "regenerated" if rejected_meta else "generated"
    entry = publish_cache(staging, identity, capture_id=capture_id)
    status = "refreshed" if previous is not None else "miss-captured"
    return PreparedReplayCache(
        entry,
        status,
        meta_path=meta_path,
        meta_status=meta_status,
    )


def _lookup_existing_replay_cache(
    cache_root: str | Path,
    identity: Mapping[str, object],
    shape: ReplayShape,
    *,
    capture_id: str,
    source: str,
    meta_root: str | Path | None,
) -> tuple[PreparedReplayCache | None, CacheEntry | None, bool]:
    previous = find_cache(cache_root, identity)
    if previous is not None and source == "cache":
        return PreparedReplayCache(previous, "hit"), previous, False

    rejected_meta = False
    if meta_root is not None and source in ("cache", "meta"):
        try:
            bundle = load_meta_bundle(meta_root, shape, identity)
        except (MetaValidationError, CacheValidationError):
            bundle = None
            rejected_meta = True
        if bundle is not None:
            entry = materialize_meta_cache(
                bundle,
                cache_root,
                identity,
                capture_id=f"{capture_id}-meta",
            )
            return (
                PreparedReplayCache(
                    entry,
                    "meta-hit",
                    meta_path=bundle.path,
                    meta_status="hit",
                ),
                previous,
                False,
            )
    return None, previous, rejected_meta


def _sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _sha256_files(paths: Sequence[Path]) -> str:
    digest = hashlib.sha256()
    for path in paths:
        digest.update(str(path.name).encode("utf-8"))
        digest.update(bytes.fromhex(_sha256_file(path)))
    return digest.hexdigest()


def _read_runner_config(path: Path) -> dict[str, str]:
    values = {}
    with path.open("r", encoding="utf-8") as handle:
        for raw_line in handle:
            line = raw_line.strip()
            if not line or line.startswith("#") or "=" not in line:
                continue
            name, raw_value = line.split("=", 1)
            if not name.startswith("MODEL_RUNNER_"):
                continue
            parsed = shlex.split(raw_value, posix=True)
            if len(parsed) != 1:
                raise ValueError(f"invalid model runner config value for {name}")
            values[name] = parsed[0]
    return values


def _existing_path(
    candidates: Sequence[str | Path],
    *,
    require_file: bool = False,
) -> Path | None:
    for raw_candidate in candidates:
        if not raw_candidate:
            continue
        candidate = Path(raw_candidate).expanduser()
        if (candidate.is_file() if require_file else candidate.exists()):
            return candidate.resolve()
    return None


def _required_existing_path(
    label: str,
    candidates: Sequence[str | Path],
    *,
    require_file: bool = False,
) -> Path:
    resolved = _existing_path(candidates, require_file=require_file)
    if resolved is None:
        rendered = ", ".join(str(candidate) for candidate in candidates if candidate)
        raise FileNotFoundError(
            f"cannot auto-generate model runner config: {label} was not found"
            f" among: {rendered}"
        )
    return resolved


def _hostname_address() -> str:
    process = subprocess.run(
        ["hostname", "-I"],
        text=True,
        capture_output=True,
        check=False,
    )
    if process.returncode == 0:
        for token in process.stdout.split():
            if token and not token.startswith("127."):
                return token
    return "127.0.0.1"


def _native_env_candidates(
    model_root: Path,
    environment: Mapping[str, str],
) -> list[str | Path]:
    candidates: list[str | Path] = [
        environment.get("MODEL_RUNNER_NATIVE_ENV", ""),
        environment.get("TILEXR_MODEL_RUNNER_NATIVE_ENV", ""),
    ]
    candidates.extend(
        sorted(
            model_root.glob("moonep-native-build-*/moonep-native.env"),
            key=lambda path: str(path),
            reverse=True,
        )
    )
    return candidates


def _write_runner_config_atomic(path: Path, values: Mapping[str, str]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    content = "# Generated by tools/moonep/model_replay_orchestrator.py\n"
    content += "# Safe to delete; run_moonep.sh will regenerate it for single-node replay.\n"
    content += "".join(
        f"{name}={shlex.quote(value)}\n"
        for name, value in sorted(values.items())
    )
    temporary = path.with_name(f".{path.name}.tmp-{os.getpid()}-{uuid.uuid4().hex}")
    with temporary.open("w", encoding="utf-8", newline="\n") as handle:
        handle.write(content)
        handle.flush()
        os.fsync(handle.fileno())
    os.replace(temporary, path)
    try:
        path.chmod(0o600)
    except OSError:
        pass


def ensure_model_runner_config(
    config_path: str | Path,
    *,
    source_root: Path,
    shape: ReplayShape,
    node_count: int,
    environment: Mapping[str, str] | None = None,
) -> Path:
    target = Path(config_path).resolve()
    if target.is_file():
        return target
    if node_count != 1:
        raise FileNotFoundError(
            f"model runner config not found: {target}; multi-node replay needs an "
            "explicit preconfigured model runner env"
        )

    env = os.environ if environment is None else environment
    model_root = _required_existing_path(
        "model root",
        (
            env.get("MODEL_RUNNER_MODEL_ROOT", ""),
            env.get("TILEXR_MODEL_RUNNER_MODEL_ROOT", ""),
            source_root / "run" / "multinode-validation" / "model",
            *_DEFAULT_MODEL_ROOT_CANDIDATES,
        ),
    )
    install_prefix = _required_existing_path(
        "TileXR install prefix",
        (
            env.get("MODEL_RUNNER_INSTALL_PREFIX", ""),
            env.get("TILEXR_INSTALL_PREFIX", ""),
            source_root / "install-clean-b131",
            source_root / "install",
        ),
    )
    cann_env = _required_existing_path(
        "CANN environment script",
        (
            env.get("MODEL_RUNNER_CANN_ENV", ""),
            env.get("TILEXR_MODEL_RUNNER_CANN_ENV", ""),
            *_DEFAULT_CANN_ENV_CANDIDATES,
            Path(env["ASCEND_HOME_PATH"]) / "set_env.sh"
            if env.get("ASCEND_HOME_PATH")
            else "",
        ),
        require_file=True,
    )
    conda_environment = (
        env.get("MODEL_RUNNER_CONDA_ENV")
        or env.get("TILEXR_MOONEP_CONDA_ENV")
        or "ai_moe_test"
    )
    conda_prefix = Path(conda_environment).expanduser()
    if conda_prefix.is_absolute():
        if not (conda_prefix / "bin" / "python").is_file():
            raise FileNotFoundError(
                "cannot auto-generate model runner config: Conda environment "
                f"Python was not found: {conda_prefix / 'bin' / 'python'}"
            )
        conda_sh: Path | None = _existing_path(
            (
                env.get("MODEL_RUNNER_CONDA_SH", ""),
                env.get("TILEXR_MODEL_RUNNER_CONDA_SH", ""),
            ),
            require_file=True,
        )
    else:
        conda_sh = _required_existing_path(
            "Conda shell script",
            (
                env.get("MODEL_RUNNER_CONDA_SH", ""),
                env.get("TILEXR_MODEL_RUNNER_CONDA_SH", ""),
                *_DEFAULT_CONDA_SH_CANDIDATES,
                Path.home() / "miniconda3" / "etc" / "profile.d" / "conda.sh",
                Path.home() / "anaconda3" / "etc" / "profile.d" / "conda.sh",
            ),
            require_file=True,
        )
    native_env = _required_existing_path(
        "native MoonEP environment script",
        _native_env_candidates(model_root, env),
        require_file=True,
    )
    tokenizer_path = _required_existing_path(
        "tokenizer path",
        (
            env.get("MODEL_RUNNER_TOKENIZER_PATH", ""),
            env.get("TILEXR_MODEL_RUNNER_TOKENIZER_PATH", ""),
            *_DEFAULT_TOKENIZER_PATH_CANDIDATES,
        ),
    )
    data_path = (
        env.get("MODEL_RUNNER_DATA_PATH")
        or env.get("TILEXR_MODEL_RUNNER_DATA_PATH")
        or str(tokenizer_path / "enwiki_text_document")
    )
    values = {
        "MODEL_RUNNER_CANN_ENV": str(cann_env),
        "MODEL_RUNNER_CONDA_ENV": conda_environment,
        "MODEL_RUNNER_CONDA_SH": "" if conda_sh is None else str(conda_sh),
        "MODEL_RUNNER_DATA_PATH": data_path,
        "MODEL_RUNNER_DEVICES_PER_NODE": str(shape.world_size),
        "MODEL_RUNNER_INSTALL_PREFIX": str(install_prefix),
        "MODEL_RUNNER_MODEL_ROOT": str(model_root),
        "MODEL_RUNNER_NATIVE_ENV": str(native_env),
        "MODEL_RUNNER_NODES": (
            env.get("MODEL_RUNNER_NODES")
            or env.get("TILEXR_MODEL_RUNNER_NODES")
            or _hostname_address()
        ),
        "MODEL_RUNNER_SSH_USER": (
            env.get("MODEL_RUNNER_SSH_USER")
            or env.get("TILEXR_MODEL_RUNNER_SSH_USER")
            or "root"
        ),
        "MODEL_RUNNER_TILEXR_HOME": str(
            Path(env.get("MODEL_RUNNER_TILEXR_HOME", str(source_root))).resolve()
        ),
        "MODEL_RUNNER_TOKENIZER_PATH": str(tokenizer_path),
    }
    _write_runner_config_atomic(target, values)
    return target


def _git_revision(path: Path) -> str:
    if not path.is_dir():
        return "unavailable"
    process = subprocess.run(
        ["git", "-C", str(path), "rev-parse", "HEAD"],
        text=True,
        capture_output=True,
        check=False,
    )
    return process.stdout.strip() if process.returncode == 0 else "unavailable"


def _file_identity(path: Path) -> str:
    return f"sha256:{_sha256_file(path)}" if path.is_file() else "unavailable"


def _npu_smi_board_identity() -> dict[str, str]:
    try:
        completed = subprocess.run(
            ["npu-smi", "info", "-t", "board", "-i", "0"],
            text=True,
            capture_output=True,
            check=False,
            timeout=10,
        )
    except (OSError, subprocess.SubprocessError):
        return {}
    if completed.returncode != 0:
        return {}

    fields: dict[str, str] = {}
    for line in completed.stdout.splitlines():
        match = re.fullmatch(r"\s*([^:]+?)\s*:\s*(.*?)\s*", line)
        if match is not None and match.group(1) in {
            "Chip Name",
            "Chip Version",
            "Firmware Version",
        }:
            fields[match.group(1)] = match.group(2)
    chip_name = fields.get("Chip Name", "")
    chip_version = fields.get("Chip Version", "")
    soc = "-".join(value for value in (chip_name, chip_version) if value)
    return {
        "firmware": fields.get("Firmware Version", ""),
        "soc": soc,
    }


def _install_artifacts_identity(install_prefix: Path) -> dict[str, str]:
    lib_dir_candidates = (install_prefix / "lib64", install_prefix / "lib")
    names = (
        "libtile-comm.so",
        "libtilexr-moonep.so",
        "libtilexr-moonep-combine-v2.so",
        "libtilexr-moonep-reduce-grad.so",
    )
    result: dict[str, str] = {}
    for name in names:
        identity = "unavailable"
        for lib_dir in lib_dir_candidates:
            path = lib_dir / name
            if path.is_file():
                identity = _file_identity(path)
                break
        result[name] = identity
    return result


def _tilexr_revision(source: Path, environment: Mapping[str, str]) -> str:
    override = environment.get("TILEXR_MODEL_REPLAY_TILEXR_GIT_SHA", "").strip()
    if not override:
        return _git_revision(source)
    if re.fullmatch(r"[0-9a-fA-F]{7,64}", override) is None:
        raise ValueError(
            "TILEXR_MODEL_REPLAY_TILEXR_GIT_SHA must be a 7-64 digit hexadecimal Git revision"
        )
    return override.lower()


def build_runtime_provenance(
    settings: ModelRunnerSettings,
    *,
    environment: Mapping[str, str] | None = None,
) -> dict[str, object]:
    env = os.environ if environment is None else environment
    config = (
        _read_runner_config(settings.config_path)
        if settings.config_path.is_file()
        else {}
    )
    model_root_value = (
        config.get("MODEL_RUNNER_MODEL_ROOT")
        or env.get("MODEL_RUNNER_MODEL_ROOT")
        or env.get("TILEXR_MODEL_RUNNER_MODEL_ROOT")
    )
    model_root = Path(model_root_value).expanduser() if model_root_value else None
    node_spec = (
        config.get("MODEL_RUNNER_NODES")
        or env.get("MODEL_RUNNER_NODES")
        or env.get("TILEXR_MODEL_RUNNER_NODES")
        or ""
    )
    hosts = node_spec.replace(",", " ").split()
    if len(hosts) < settings.node_count:
        hosts = [f"node-{index}" for index in range(settings.node_count)]
    else:
        hosts = hosts[: settings.node_count]
    devices_per_node = settings.shape.world_size // settings.node_count
    source = settings.source_root
    adapter = source / "tools" / "moonep" / "mindspeed" / "tilexr_mindspeed_adapter.py"
    runner_files = (
        source / "tools" / "moonep" / "mindspeed" / "run_model.sh",
        source / "tools" / "moonep" / "mindspeed" / "run_model_node.sh",
        source / "integrations" / "moonep_torch" / "tilexr_moonep" / "torch_api.py",
        source / "tools" / "moonep" / "model_flow.py",
        source / "tools" / "moonep" / "model_replay_cache.py",
        source / "tools" / "moonep" / "model_replay_meta.py",
        source / "tools" / "moonep" / "model_replay_orchestrator.py",
        source / "tools" / "moonep" / "mindspeed" / "build_route_replay.py",
        source / "tools" / "moonep" / "mindspeed" / "collect_model_performance.py",
    )
    cann_env_value = (
        config.get("MODEL_RUNNER_CANN_ENV")
        or env.get("MODEL_RUNNER_CANN_ENV")
        or env.get("TILEXR_MODEL_RUNNER_CANN_ENV")
    )
    if not cann_env_value and env.get("TILEXR_CANN_HOME"):
        cann_env_value = str(Path(env["TILEXR_CANN_HOME"]) / "set_env.sh")
    cann_env = Path(cann_env_value).expanduser() if cann_env_value else None
    install_prefix_value = (
        config.get("MODEL_RUNNER_INSTALL_PREFIX")
        or env.get("MODEL_RUNNER_INSTALL_PREFIX")
        or env.get("TILEXR_INSTALL_PREFIX")
    )
    install_prefix = (
        Path(install_prefix_value).expanduser() if install_prefix_value else None
    )
    driver_version = Path("/usr/local/Ascend/driver/version.info")
    firmware = env.get("TILEXR_MODEL_REPLAY_FIRMWARE_ID", "").strip()
    soc = (
        env.get("TILEXR_MODEL_REPLAY_SOC_ID", "").strip()
        or env.get("ASCEND_SOC_VERSION", "").strip()
    )
    board_identity = _npu_smi_board_identity() if not firmware or not soc else {}
    firmware = firmware or board_identity.get("firmware", "") or "unavailable"
    soc = soc or board_identity.get("soc", "") or "unavailable"
    topology: dict[str, object] = {
        "nodes": hosts,
        "node_count": settings.node_count,
        "devices_per_node": devices_per_node,
    }
    if settings.rank_table_file is not None:
        topology["rank_table_sha256"] = _sha256_file(settings.rank_table_file)
    return {
        "tilexr_git_sha": _tilexr_revision(source, env),
        "adapter_sha256": _sha256_file(adapter),
        "runner_sha256": _sha256_files(runner_files),
        "install_artifacts": (
            _install_artifacts_identity(install_prefix)
            if install_prefix is not None
            else _install_artifacts_identity(source / ".missing-install")
        ),
        "model_stack": {
            "mindspeed": (
                _git_revision(model_root / "MindSpeed")
                if model_root is not None
                else "unavailable"
            ),
            "mindspeed_llm": (
                _git_revision(model_root / "MindSpeed-LLM")
                if model_root is not None
                else "unavailable"
            ),
        },
        "backend": "tilexr",
        "execution_controls": {
            "framework_profiler": settings.profiler_enabled,
            "stage_barrier": settings.stage_barrier,
        },
        "kernel_version": {
            "combine": env.get("TILEXR_MOONEP_COMBINE_VERSION", "2"),
            "dispatch_peer_mode": (
                env.get("TILEXR_MOONEP_DISPATCH_PEER_MODE")
                or "group_credit"
            ),
            "performance_mode": (
                "framework_profiler"
                if settings.profiler_enabled
                else "lightweight_npu_event"
            ),
        },
        "cann": env.get(
            "TILEXR_MODEL_REPLAY_CANN_ID",
            _file_identity(cann_env) if cann_env is not None else "unavailable",
        ),
        "driver": env.get(
            "TILEXR_MODEL_REPLAY_DRIVER_ID", _file_identity(driver_version)
        ),
        "firmware": firmware,
        "soc": soc,
        "topology": topology,
        "rank_mapping": [
            {
                "rank": rank,
                "node": rank // devices_per_node,
                "device": rank % devices_per_node,
            }
            for rank in range(settings.shape.world_size)
        ],
    }


def _write_json_atomic(path: Path, value: object) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(f".{path.name}.tmp-{os.getpid()}-{uuid.uuid4().hex}")
    with temporary.open("w", encoding="utf-8", newline="\n") as handle:
        json.dump(value, handle, indent=2, sort_keys=True, allow_nan=False)
        handle.write("\n")
        handle.flush()
        os.fsync(handle.fileno())
    os.replace(temporary, path)


def _run_checked(command: Sequence[str]) -> None:
    subprocess.run(list(command), check=True)


def _safe_runtime_tag(value: str) -> str:
    safe = "".join(char if char.isalnum() or char in "._-" else "-" for char in value)
    return safe.strip(".-")[:80] or "model-replay"


def prepare_with_model_runner(
    cache_root: str | Path,
    identity: Mapping[str, object],
    settings: ModelRunnerSettings,
    *,
    capture_id: str,
    source: str = "cache",
    meta_root: str | Path | None = None,
    command_runner: Callable[[Sequence[str]], None] = _run_checked,
) -> PreparedReplayCache:
    tag = _safe_runtime_tag(capture_id)
    run_tag = f"{tag}-capture"
    performance_capture_root: Path | None = (
        settings.model_artifact_root(run_tag) if settings.profiler_enabled else None
    )

    def capture_routes(raw_capture: Path) -> None:
        nonlocal performance_capture_root
        command = settings.base_command(run_tag=run_tag)
        command.extend(
            (
                "--route-capture-dir",
                str(raw_capture),
                "--route-capture-id",
                capture_id,
                "--route-capture-skip-calls",
                "60",
                "--route-capture-calls",
                str(settings.shape.forward_calls),
            )
        )
        if settings.profiler_enabled:
            command.append("--profile")
        else:
            performance_capture_root = raw_capture.parent.parent / "model" / "raw"
            command.extend(
                (
                    "--performance-capture-dir",
                    str(performance_capture_root),
                    "--performance-capture-id",
                    capture_id,
                    "--performance-capture-skip-operators",
                    "330",
                    "--performance-capture-operators",
                    str(len(model_operator_order())),
                )
            )
        command_runner(command)

    def capture_performance(
        output_path: Path, route_replay: dict[str, object]
    ) -> None:
        if performance_capture_root is None:
            raise RuntimeError("combined model performance capture root is unavailable")
        performance = collect_model_performance(
            performance_capture_root,
            route_replay,
            settings.shape,
            profiler_enabled=settings.profiler_enabled,
            backend="tilexr",
            iterations=1,
        )
        _write_json_atomic(output_path, performance)

    return prepare_replay_cache(
        cache_root,
        identity,
        settings.shape,
        capture_id=capture_id,
        source=source,
        capture_routes=capture_routes,
        capture_performance=capture_performance,
        meta_root=meta_root,
    )


def wait_for_replay_cache(
    cache_root: str | Path,
    identity: Mapping[str, object],
    *,
    timeout_seconds: int,
) -> CacheEntry:
    deadline = time.monotonic() + timeout_seconds
    while True:
        entry = find_cache(cache_root, identity)
        if entry is not None:
            return entry
        if time.monotonic() >= deadline:
            raise TimeoutError(f"timed out waiting for replay cache {cache_key(identity)}")
        time.sleep(1.0)


_HOST_LOCAL_PROVENANCE_FIELDS = ("driver", "firmware", "soc")


def _shared_cache_identity(identity: Mapping[str, object]) -> dict[str, object]:
    shared = json.loads(json.dumps(dict(identity), sort_keys=True, allow_nan=False))
    provenance = shared.get("provenance")
    if isinstance(provenance, dict):
        for field in _HOST_LOCAL_PROVENANCE_FIELDS:
            provenance.pop(field, None)
    return shared


def load_published_cache_entry(
    generation: str | Path,
    cache_root: str | Path,
    follower_identity: Mapping[str, object],
) -> CacheEntry:
    root = Path(cache_root).resolve()
    path = Path(generation).resolve()
    try:
        path.relative_to(root)
    except ValueError as exc:
        raise CacheValidationError(
            f"published cache generation is outside cache root: {path}"
        ) from exc
    try:
        with (path / "manifest.json").open("r", encoding="utf-8") as handle:
            manifest = json.load(handle)
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise CacheValidationError(
            f"published cache manifest is invalid: {path}"
        ) from exc
    if not isinstance(manifest, dict) or not isinstance(manifest.get("identity"), dict):
        raise CacheValidationError("published cache manifest is missing identity")
    manifest_identity = manifest["identity"]
    manifest_key = manifest.get("cache_key")
    if not isinstance(manifest_key, str) or path.parent != root / manifest_key:
        raise CacheValidationError("published cache generation path does not match its key")
    if _shared_cache_identity(manifest_identity) != _shared_cache_identity(
        follower_identity
    ):
        raise CacheValidationError("follower cache identity mismatch")
    return validate_cache_entry(path, manifest_identity)


def distribute_cache_entry(
    entry: CacheEntry,
    settings: ModelRunnerSettings,
    *,
    command_runner: Callable[[Sequence[str]], None] = _run_checked,
) -> None:
    if settings.node_count == 1:
        return
    config = _read_runner_config(settings.config_path)
    hosts = config.get("MODEL_RUNNER_NODES", "").replace(",", " ").split()
    user = config.get("MODEL_RUNNER_SSH_USER", "root")
    if len(hosts) < settings.node_count:
        raise ValueError("model runner config does not contain every replay node")
    ssh_options = (
        "-o",
        "ConnectTimeout=15",
        "-o",
        "ServerAliveInterval=30",
        "-o",
        "ServerAliveCountMax=3",
    )
    rsync_shell = "ssh " + " ".join(shlex.quote(value) for value in ssh_options)
    for host in hosts[1 : settings.node_count]:
        target = f"{user}@{host}"
        command_runner(
            [
                "ssh",
                *ssh_options,
                target,
                f"mkdir -p -- {shlex.quote(str(entry.path.parent))}",
            ]
        )
        command_runner(
            [
                "rsync",
                "-a",
                "--protect-args",
                "-e",
                rsync_shell,
                f"{entry.path}/",
                f"{target}:{entry.path}/",
            ]
        )


def _write_result_env(
    path: Path,
    prepared: PreparedReplayCache,
    *,
    case_file: Path,
    case_id: str,
) -> None:
    values = {
        "MODEL_REPLAY_CACHE_STATUS": prepared.cache_status,
        "MODEL_REPLAY_CACHE_KEY": str(prepared.entry.manifest["cache_key"]),
        "MODEL_REPLAY_CACHE_GENERATION": str(prepared.entry.path),
        "MODEL_REPLAY_ROUTE_REPLAY": str(prepared.route_replay_path),
        "MODEL_REPLAY_MODEL_PERFORMANCE": str(prepared.model_performance_path),
        "MODEL_REPLAY_CASE_FILE": str(case_file),
        "MODEL_REPLAY_CASE_ID": case_id,
    }
    content = "".join(f"{name}={shlex.quote(value)}\n" for name, value in values.items())
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(f".{path.name}.tmp-{os.getpid()}-{uuid.uuid4().hex}")
    with temporary.open("w", encoding="utf-8", newline="\n") as handle:
        handle.write(content)
        handle.flush()
        os.fsync(handle.fileno())
    os.replace(temporary, path)


def write_dynamic_case_file(
    path: str | Path,
    shape: ReplayShape,
    *,
    warmup: int,
    iterations: int,
) -> str:
    for name, value in (("warmup", warmup), ("iterations", iterations)):
        if isinstance(value, bool) or not isinstance(value, int) or value < 0:
            raise ValueError(f"{name} must be a non-negative integer")
    if warmup + iterations < 1:
        raise ValueError("warmup + iterations must be at least 1")
    case_id = (
        f"model-replay-s{shape.tokens_per_rank}-k{shape.topk}"
        f"-h{shape.hidden_size}-ep{shape.ep_size}-r{shape.world_size}"
    )
    payload = [
        {
            "id": case_id,
            "S": shape.tokens_per_rank,
            "K": shape.topk,
            "E": shape.expert_count,
            "H": shape.hidden_size,
            "Hf": shape.ffn_hidden_size,
            "B": shape.experts_per_rank,
            "P": shape.token_padding,
            "routing": "model_replay",
            "route_distribution": "rank_shifted_uniform",
            "warmup": warmup,
            "iters": iterations,
            "correctness": True,
        }
    ]
    target = Path(path)
    target.parent.mkdir(parents=True, exist_ok=True)
    temporary = target.with_name(f".{target.name}.tmp-{os.getpid()}-{uuid.uuid4().hex}")
    with temporary.open("w", encoding="utf-8", newline="\n") as handle:
        json.dump(payload, handle, indent=2, sort_keys=True, allow_nan=False)
        handle.write("\n")
        handle.flush()
        os.fsync(handle.fileno())
    os.replace(temporary, target)
    return case_id


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Prepare and publish a cached MoonEP model replay input set"
    )
    parser.add_argument("--source-root", required=True)
    parser.add_argument("--cache-root", required=True)
    parser.add_argument("--case-file", required=True)
    parser.add_argument("--result-env-file", required=True)
    parser.add_argument("--model-runner-config", default=None)
    parser.add_argument("--s", type=int, required=True)
    parser.add_argument("--k", type=int, required=True)
    parser.add_argument("--hidden-size", type=int, required=True)
    parser.add_argument("--ep", type=int, required=True)
    parser.add_argument("--rank-size", type=int, required=True)
    parser.add_argument("--node-count", type=int, required=True)
    parser.add_argument("--node-rank", type=int, required=True)
    parser.add_argument("--warmup", type=int, required=True)
    parser.add_argument("--iterations", type=int, required=True)
    parser.add_argument("--model-timeout", type=int, default=900)
    parser.add_argument("--wait-timeout", type=int, default=3600)
    parser.add_argument("--capture-id", default=None)
    parser.add_argument(
        "--cache-generation",
        default=os.environ.get("TILEXR_MOONEP_MODEL_REPLAY_GENERATION"),
    )
    parser.add_argument(
        "--meta-root",
        default=os.environ.get("TILEXR_MOONEP_MODEL_REPLAY_META_ROOT"),
    )
    parser.add_argument(
        "--model-replay-from",
        dest="source",
        choices=("cache", "meta", "model"),
        default="cache",
    )
    profile = parser.add_mutually_exclusive_group()
    profile.add_argument("--profile", dest="profile", action="store_true")
    profile.add_argument("--no-profile", dest="profile", action="store_false")
    parser.set_defaults(profile=True)
    return parser


def _run_main(args: argparse.Namespace) -> dict[str, object]:
    source_root = Path(args.source_root).resolve()
    meta_root = (
        Path(args.meta_root).resolve()
        if args.meta_root
        else source_root / "tools" / "moonep" / "model_replay_meta"
    )
    config_path = (
        Path(args.model_runner_config).resolve()
        if args.model_runner_config
        else source_root / "run" / "moonep" / "mindspeed" / "model_runner.env"
    )
    if args.ep != args.rank_size:
        raise ValueError("EP must equal R for the current model replay runner")
    expert_count = ((32 + args.ep - 1) // args.ep) * args.ep
    shape = ReplayShape(
        tokens_per_rank=args.s,
        topk=args.k,
        hidden_size=args.hidden_size,
        ep_size=args.ep,
        world_size=args.rank_size,
        expert_count=expert_count,
    )
    if args.node_rank < 0 or args.node_rank >= args.node_count:
        raise ValueError("node rank must be in [0, node count)")
    settings = ModelRunnerSettings(
        source_root=source_root,
        config_path=config_path,
        shape=shape,
        node_count=args.node_count,
        timeout_seconds=args.model_timeout,
        profiler_enabled=args.profile,
        bash_executable=os.environ.get("TILEXR_MODEL_REPLAY_BASH", "bash"),
        rank_table_file=(
            Path(os.environ["TILEXR_MODEL_REPLAY_RANK_TABLE_FILE"]).resolve()
            if os.environ.get("TILEXR_MODEL_REPLAY_RANK_TABLE_FILE", "").strip()
            else None
        ),
    )
    provenance = build_runtime_provenance(settings)
    identity = build_cache_identity(
        shape,
        provenance=provenance,
        operator_order=model_operator_order(),
    )
    capture_id = args.capture_id or (
        f"model-replay-{time.time_ns()}-{os.getpid()}"
    )
    if args.node_rank == 0:
        prepared = None
        if args.source != "model":
            prepared, _previous, _rejected_meta = _lookup_existing_replay_cache(
                args.cache_root,
                identity,
                shape,
                capture_id=capture_id,
                source=args.source,
                meta_root=meta_root,
            )
        if prepared is None:
            config_path = ensure_model_runner_config(
                config_path,
                source_root=source_root,
                shape=shape,
                node_count=args.node_count,
            )
            settings = replace(settings, config_path=config_path)
            provenance = build_runtime_provenance(settings)
            identity = build_cache_identity(
                shape,
                provenance=provenance,
                operator_order=model_operator_order(),
            )
            prepared = prepare_with_model_runner(
                args.cache_root,
                identity,
                settings,
                capture_id=capture_id,
                source=args.source,
                meta_root=meta_root,
            )
        distribute_cache_entry(prepared.entry, settings)
    else:
        if args.source == "model" or config_path.is_file():
            config_path = ensure_model_runner_config(
                config_path,
                source_root=source_root,
                shape=shape,
                node_count=args.node_count,
            )
            settings = replace(settings, config_path=config_path)
            provenance = build_runtime_provenance(settings)
            identity = build_cache_identity(
                shape,
                provenance=provenance,
                operator_order=model_operator_order(),
            )
        if args.cache_generation:
            entry = load_published_cache_entry(
                args.cache_generation,
                args.cache_root,
                identity,
            )
        else:
            entry = wait_for_replay_cache(
                args.cache_root,
                identity,
                timeout_seconds=args.wait_timeout,
            )
        prepared = PreparedReplayCache(entry, "follower-hit")
    case_file = Path(args.case_file).resolve()
    case_id = write_dynamic_case_file(
        case_file,
        shape,
        warmup=args.warmup,
        iterations=args.iterations,
    )
    _write_result_env(
        Path(args.result_env_file).resolve(),
        prepared,
        case_file=case_file,
        case_id=case_id,
    )
    result = {
        "cache_status": prepared.cache_status,
        "cache_key": prepared.entry.manifest["cache_key"],
        "cache_generation": str(prepared.entry.path),
        "route_replay": str(prepared.route_replay_path),
        "model_performance": str(prepared.model_performance_path),
        "case_file": str(case_file),
        "case_id": case_id,
        "profiler_enabled": settings.profiler_enabled,
        "stage_barrier": settings.stage_barrier,
        "meta_status": prepared.meta_status,
        "meta_directory": (
            None if prepared.meta_path is None else str(prepared.meta_path)
        ),
    }
    if prepared.meta_path is not None:
        result["meta_files"] = [
            {
                "path": str(path),
                "size": path.stat().st_size,
                "sha256": _sha256_file(path),
                "suggested_check_in": True,
            }
            for path in sorted(prepared.meta_path.iterdir())
            if path.is_file()
        ]
    return result


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    try:
        result = _run_main(args)
    except (OSError, RuntimeError, ValueError, subprocess.SubprocessError) as exc:
        print(f"model replay preparation failed: {exc}", file=sys.stderr)
        return 1
    print(json.dumps(result, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
