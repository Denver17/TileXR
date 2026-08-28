from __future__ import annotations

import base64
import copy
import hashlib
import json
import shlex
import struct
import subprocess
import zlib
from pathlib import Path
from types import SimpleNamespace

import pytest

from tools.moonep import model_replay_orchestrator as replay_orchestrator
from tools.moonep.config import load_cases
from tools.moonep.model_replay_cache import (
    CacheValidationError,
    ReplayShape,
    build_cache_identity,
    find_cache,
)
from tools.moonep.model_replay_orchestrator import (
    ModelRunnerSettings,
    PreparedReplayCache,
    build_runtime_provenance,
    ensure_model_runner_config,
    load_published_cache_entry,
    prepare_replay_cache,
    prepare_with_model_runner,
    write_dynamic_case_file,
)


ROOT = Path(__file__).resolve().parents[3]


def _shape() -> ReplayShape:
    return ReplayShape(
        tokens_per_rank=2,
        topk=2,
        hidden_size=128,
        ep_size=2,
        world_size=2,
        expert_count=4,
        ffn_hidden_size=32,
        forward_calls=2,
    )


def _order() -> list[dict[str, object]]:
    return [
        {"sequence": 0, "phase": "forward", "layer": 0, "stage": "planning"},
        {
            "sequence": 1,
            "phase": "forward",
            "layer": 0,
            "stage": "dispatch_forward",
        },
    ]


def _identity() -> dict[str, object]:
    return build_cache_identity(
        _shape(),
        provenance={
            "tilexr_git_sha": "abc123",
            "adapter_sha256": "a" * 64,
            "runner_sha256": "b" * 64,
            "install_artifacts": {"libtile-comm.so": "sha256:" + "c" * 64},
            "model_stack": {"mindspeed": "test", "mindspeed_llm": "test"},
            "backend": "tilexr",
            "execution_controls": {
                "framework_profiler": True,
                "stage_barrier": True,
            },
            "kernel_version": "test-kernel",
            "cann": "b131",
            "driver": "test-driver",
            "firmware": "test-firmware",
            "soc": "Ascend950",
            "topology": {
                "nodes": ["node-a"],
                "node_count": 1,
                "devices_per_node": 2,
            },
            "rank_mapping": [
                {"rank": 0, "node": 0, "device": 0},
                {"rank": 1, "node": 0, "device": 1},
            ],
        },
        operator_order=_order(),
    )


def _capture_routes(root: Path, *, capture_id: str = "capture-1") -> None:
    for rank in range(2):
        for call in range(2):
            routes = (0, 1, 2, 3)
            raw = struct.pack("<4i", *routes)
            record = {
                "schema_version": 1,
                "complete": True,
                "rank": rank,
                "call": call,
                "source_call": call + 60,
                "capture_id": capture_id,
                "topk_shape": [2, 2],
                "topk_dtype": "torch.int32",
                "topk_encoding": "int32-le-zlib-base64",
                "topk_zlib_base64": base64.b64encode(zlib.compress(raw)).decode(
                    "ascii"
                ),
                "topk_sha256": hashlib.sha256(raw).hexdigest(),
                "tokens_per_expert": [1, 1, 1, 1],
                "remote_stats": [1, 1],
                "experts_to_copy": [[0, 1], [2, 3]],
                "payload_seed": rank * 100 + call,
                "tensor_descriptors": {
                    "hidden": {"shape": [2, 128], "dtype": "torch.bfloat16"}
                },
            }
            (root / f"rank{rank}_call{call:02d}.json").write_text(
                json.dumps(record), encoding="utf-8"
            )


def _capture_performance(path: Path, _route_replay: dict[str, object]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    ranks = {}
    for rank in range(2):
        ranks[str(rank)] = {
            "operators": [
                {
                    "sequence": sequence,
                    "stage": item["stage"],
                    "backend": "tilexr",
                    "kernel_version": "test-kernel",
                    "algorithm_bytes": None if sequence == 0 else 1024,
                    "latency_us": 10.0 + rank + sequence,
                    "algorithm_bandwidth_GBps": None if sequence == 0 else 0.1,
                }
                for sequence, item in enumerate(_order())
            ]
        }
    path.write_text(
        json.dumps(
            {
                "schema_version": 1,
                "world_size": 2,
                "profiler_enabled": True,
                "ranks": ranks,
            }
        ),
        encoding="utf-8",
    )


def _parse_runner_env(text: str) -> dict[str, str]:
    values = {}
    for raw_line in text.splitlines():
        if not raw_line or raw_line.startswith("#") or "=" not in raw_line:
            continue
        name, raw_value = raw_line.split("=", 1)
        parsed = shlex.split(raw_value, posix=True)
        assert len(parsed) == 1
        values[name] = parsed[0]
    return values


def test_prepare_cache_miss_then_hit_avoids_model_callbacks(tmp_path: Path) -> None:
    calls = []

    def routes(path: Path) -> None:
        calls.append("routes")
        _capture_routes(path)

    def performance(path: Path, replay: dict[str, object]) -> None:
        calls.append("performance")
        _capture_performance(path, replay)

    first = prepare_replay_cache(
        tmp_path,
        _identity(),
        _shape(),
        capture_id="capture-1",
        source="cache",
        capture_routes=routes,
        capture_performance=performance,
    )
    second = prepare_replay_cache(
        tmp_path,
        _identity(),
        _shape(),
        capture_id="unused",
        source="cache",
        capture_routes=routes,
        capture_performance=performance,
    )

    assert first.cache_status == "miss-captured"
    assert second.cache_status == "hit"
    assert second.entry.path == first.entry.path
    assert calls == ["routes", "performance"]
    assert first.route_replay_path.is_file()


def test_meta_hit_materializes_cross_host_cache_without_model_callbacks(
    tmp_path: Path,
) -> None:
    source_cache = tmp_path / "source-cache"
    destination_cache = tmp_path / "destination-cache"
    meta_root = tmp_path / "meta"
    source_identity = _identity()
    source_identity["provenance"]["kernel_version"] = {
        "combine": "2",
        "dispatch_peer_mode": "legacy",
        "performance_mode": "framework_profiler",
    }
    first = prepare_replay_cache(
        source_cache,
        source_identity,
        _shape(),
        capture_id="capture-1",
        source="cache",
        capture_routes=_capture_routes,
        capture_performance=_capture_performance,
        meta_root=meta_root,
    )
    meta_files = {
        path.name: hashlib.sha256(path.read_bytes()).hexdigest()
        for path in first.meta_path.iterdir()
    }
    cross_host_identity = copy.deepcopy(source_identity)
    cross_host_identity["provenance"].update(
        {
            "driver": "other-host-driver",
            "firmware": "other-host-firmware",
            "soc": "other-host-soc",
            "topology": {
                "nodes": ["private-other-host"],
                "node_count": 1,
                "devices_per_node": 2,
            },
        }
    )
    cross_host_identity["provenance"]["kernel_version"][
        "dispatch_peer_mode"
    ] = "group"

    def must_not_capture(*_args, **_kwargs):
        raise AssertionError("model capture must not run on a meta hit")

    replayed = prepare_replay_cache(
        destination_cache,
        cross_host_identity,
        _shape(),
        capture_id="meta-materialization",
        source="cache",
        capture_routes=must_not_capture,
        capture_performance=must_not_capture,
        meta_root=meta_root,
    )

    assert replayed.cache_status == "meta-hit"
    assert replayed.meta_status == "hit"
    assert replayed.entry.path.is_relative_to(destination_cache)
    assert replayed.route_replay_path.is_file()
    performance = json.loads(replayed.model_performance_path.read_text(encoding="utf-8"))
    assert performance["comparison"] == {
        "compatible": False,
        "classification": "checked-in reference",
        "current_environment_sha256": performance["comparison"][
            "current_environment_sha256"
        ],
        "captured_environment_sha256": performance["comparison"][
            "captured_environment_sha256"
        ],
        "dispatch_peer_mode": {
            "compatible": False,
            "model": "legacy",
            "replay": "group",
        },
    }
    assert meta_files == {
        path.name: hashlib.sha256(path.read_bytes()).hexdigest()
        for path in first.meta_path.iterdir()
    }


def test_runtime_cache_hit_takes_precedence_over_corrupt_meta(tmp_path: Path) -> None:
    cache_root = tmp_path / "cache"
    meta_root = tmp_path / "meta"
    first = prepare_replay_cache(
        cache_root,
        _identity(),
        _shape(),
        capture_id="capture-1",
        source="cache",
        capture_routes=_capture_routes,
        capture_performance=_capture_performance,
        meta_root=meta_root,
    )
    (first.meta_path / "routes.u8.zst").write_bytes(b"corrupt")

    second = prepare_replay_cache(
        cache_root,
        _identity(),
        _shape(),
        capture_id="unused",
        source="cache",
        capture_routes=lambda _path: (_ for _ in ()).throw(AssertionError("capture")),
        capture_performance=lambda *_args: (_ for _ in ()).throw(
            AssertionError("capture")
        ),
        meta_root=meta_root,
    )

    assert second.cache_status == "hit"
    assert second.entry.path == first.entry.path


def test_meta_source_bypasses_valid_runtime_cache_without_model(tmp_path: Path) -> None:
    cache_root = tmp_path / "cache"
    meta_root = tmp_path / "meta"
    first = prepare_replay_cache(
        cache_root,
        _identity(),
        _shape(),
        capture_id="capture-1",
        source="cache",
        capture_routes=_capture_routes,
        capture_performance=_capture_performance,
        meta_root=meta_root,
    )

    def must_not_capture(*_args, **_kwargs):
        raise AssertionError("forced meta must not launch the model")

    forced = prepare_replay_cache(
        cache_root,
        _identity(),
        _shape(),
        capture_id="forced-meta",
        source="meta",
        capture_routes=must_not_capture,
        capture_performance=must_not_capture,
        meta_root=meta_root,
    )

    assert forced.cache_status == "meta-hit"
    assert forced.entry.path != first.entry.path


def test_meta_source_falls_through_to_model_when_meta_is_missing(tmp_path: Path) -> None:
    calls = []

    def routes(path: Path) -> None:
        calls.append("routes")
        _capture_routes(path, capture_id="meta-fallback")

    def performance(path: Path, replay: dict[str, object]) -> None:
        calls.append("performance")
        _capture_performance(path, replay)

    prepared = prepare_replay_cache(
        tmp_path / "cache",
        _identity(),
        _shape(),
        capture_id="meta-fallback",
        source="meta",
        capture_routes=routes,
        capture_performance=performance,
        meta_root=tmp_path / "missing-meta",
    )

    assert prepared.cache_status == "miss-captured"
    assert calls == ["routes", "performance"]


def test_meta_source_recaptures_when_performance_semantics_are_invalid(
    tmp_path: Path,
) -> None:
    cache_root = tmp_path / "cache"
    meta_root = tmp_path / "meta"
    first = prepare_replay_cache(
        cache_root,
        _identity(),
        _shape(),
        capture_id="capture-1",
        source="cache",
        capture_routes=_capture_routes,
        capture_performance=_capture_performance,
        meta_root=meta_root,
    )
    performance_path = first.meta_path / "performance.json"
    performance = json.loads(performance_path.read_text(encoding="utf-8"))
    performance["ranks"]["0"]["operators"][0]["stage"] = "invalid-stage"
    performance_path.write_text(json.dumps(performance), encoding="utf-8")
    meta_path = first.meta_path / "meta.json"
    meta = json.loads(meta_path.read_text(encoding="utf-8"))
    meta["performance"]["sha256"] = hashlib.sha256(
        performance_path.read_bytes()
    ).hexdigest()
    meta_path.write_text(json.dumps(meta), encoding="utf-8")
    calls = []

    def routes(path: Path) -> None:
        calls.append("routes")
        _capture_routes(path, capture_id="performance-recovery")

    def capture_performance(path: Path, replay: dict[str, object]) -> None:
        calls.append("performance")
        _capture_performance(path, replay)

    recovered = prepare_replay_cache(
        tmp_path / "recovered-cache",
        _identity(),
        _shape(),
        capture_id="performance-recovery",
        source="meta",
        capture_routes=routes,
        capture_performance=capture_performance,
        meta_root=meta_root,
    )

    assert recovered.cache_status == "miss-captured"
    assert recovered.meta_status == "regenerated"
    assert calls == ["routes", "performance"]


def test_model_source_bypasses_runtime_and_meta_and_recaptures_once(tmp_path: Path) -> None:
    calls = []
    capture_ids = iter(("capture-1", "capture-2"))

    def routes(path: Path) -> None:
        calls.append("routes")
        _capture_routes(path, capture_id=next(capture_ids))

    def performance(path: Path, replay: dict[str, object]) -> None:
        calls.append("performance")
        _capture_performance(path, replay)

    cache_root = tmp_path / "cache"
    meta_root = tmp_path / "meta"
    first = prepare_replay_cache(
        cache_root,
        _identity(),
        _shape(),
        capture_id="capture-1",
        source="cache",
        capture_routes=routes,
        capture_performance=performance,
        meta_root=meta_root,
    )
    refreshed = prepare_replay_cache(
        cache_root,
        _identity(),
        _shape(),
        capture_id="capture-2",
        source="model",
        capture_routes=routes,
        capture_performance=performance,
        meta_root=meta_root,
    )

    assert refreshed.cache_status == "refreshed"
    assert refreshed.meta_status == "generated"
    assert refreshed.entry.path != first.entry.path
    assert calls == ["routes", "performance", "routes", "performance"]


def test_parser_accepts_only_the_three_model_replay_sources() -> None:
    required = [
        "--source-root",
        str(ROOT),
        "--cache-root",
        "cache",
        "--case-file",
        "case.json",
        "--result-env-file",
        "result.env",
        "--s",
        "2",
        "--k",
        "2",
        "--hidden-size",
        "128",
        "--ep",
        "2",
        "--rank-size",
        "2",
        "--node-count",
        "1",
        "--node-rank",
        "0",
        "--warmup",
        "0",
        "--iterations",
        "1",
    ]
    parser = replay_orchestrator.build_parser()

    assert parser.parse_args(required).source == "cache"
    for source in ("cache", "meta", "model"):
        assert (
            parser.parse_args([*required, "--model-replay-from", source]).source
            == source
        )
    with pytest.raises(SystemExit):
        parser.parse_args([*required, "--model-replay-from", "invalid"])


def test_model_runner_combines_route_and_performance_capture_in_one_command(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    commands: list[list[str]] = []
    sentinel = object()

    def fake_prepare(*_args, capture_routes, capture_performance, **_kwargs):
        raw_capture = tmp_path / "raw-capture"
        raw_capture.mkdir()
        capture_routes(raw_capture)
        performance_path = tmp_path / "performance.json"
        capture_performance(performance_path, {})
        assert performance_path.is_file()
        return sentinel

    monkeypatch.setattr(replay_orchestrator, "prepare_replay_cache", fake_prepare)
    monkeypatch.setattr(
        replay_orchestrator,
        "collect_model_performance",
        lambda *_args, **_kwargs: {"schema_version": 1},
    )
    settings = ModelRunnerSettings(
        source_root=ROOT,
        config_path=tmp_path / "model_runner.env",
        shape=_shape(),
        node_count=1,
        timeout_seconds=60,
        profiler_enabled=True,
    )

    result = prepare_with_model_runner(
        tmp_path / "cache",
        _identity(),
        settings,
        capture_id="capture-1",
        source="cache",
        meta_root=tmp_path / "meta",
        command_runner=lambda command: commands.append(list(command)),
    )

    assert result is sentinel
    assert len(commands) == 1
    assert "--route-capture-dir" in commands[0]
    assert "--route-capture-calls" in commands[0]
    assert "--profile" in commands[0]


def test_failed_model_source_preserves_the_previous_generation(tmp_path: Path) -> None:
    first = prepare_replay_cache(
        tmp_path,
        _identity(),
        _shape(),
        capture_id="capture-1",
        source="cache",
        capture_routes=_capture_routes,
        capture_performance=_capture_performance,
    )

    def fail(_path: Path) -> None:
        raise RuntimeError("capture failed")

    with pytest.raises(RuntimeError, match="capture failed"):
        prepare_replay_cache(
            tmp_path,
            _identity(),
            _shape(),
            capture_id="capture-2",
            source="model",
            capture_routes=fail,
            capture_performance=_capture_performance,
        )

    assert find_cache(tmp_path, _identity()).path == first.entry.path


def test_follower_uses_published_generation_across_host_driver_identity(
    tmp_path: Path,
) -> None:
    leader_identity = _identity()
    prepared = prepare_replay_cache(
        tmp_path,
        leader_identity,
        _shape(),
        capture_id="capture-1",
        source="cache",
        capture_routes=_capture_routes,
        capture_performance=_capture_performance,
    )
    follower_identity = copy.deepcopy(leader_identity)
    follower_identity["provenance"]["driver"] = "different-host-driver-file"

    entry = load_published_cache_entry(
        prepared.entry.path,
        tmp_path,
        follower_identity,
    )

    assert entry.path == prepared.entry.path


def test_follower_rejects_published_generation_with_runner_mismatch(
    tmp_path: Path,
) -> None:
    leader_identity = _identity()
    prepared = prepare_replay_cache(
        tmp_path,
        leader_identity,
        _shape(),
        capture_id="capture-1",
        source="cache",
        capture_routes=_capture_routes,
        capture_performance=_capture_performance,
    )
    follower_identity = copy.deepcopy(leader_identity)
    follower_identity["provenance"]["runner_sha256"] = "different-runner"

    with pytest.raises(CacheValidationError, match="follower cache identity mismatch"):
        load_published_cache_entry(
            prepared.entry.path,
            tmp_path,
            follower_identity,
        )


def test_dynamic_case_file_preserves_shape_and_iteration_controls(tmp_path: Path) -> None:
    path = tmp_path / "case.json"
    case_id = write_dynamic_case_file(path, _shape(), warmup=5, iterations=20)

    case = load_cases(path)[0]
    assert case.case_id == case_id == "model-replay-s2-k2-h128-ep2-r2"
    assert (
        case.tokens_per_rank,
        case.topk,
        case.hidden_size,
        case.expert_count,
        case.prefetch_slots,
        case.warmup,
        case.iterations,
        case.routing_pattern,
    ) == (2, 2, 128, 4, 2, 5, 20, "model_replay")


def test_model_runner_defaults_to_profiler_and_stage_barrier(tmp_path: Path) -> None:
    config = tmp_path / "model_runner.env"
    config.write_text("MODEL_RUNNER_NODES=node-a\n", encoding="utf-8")
    settings = ModelRunnerSettings(
        source_root=ROOT,
        config_path=config,
        shape=_shape(),
        node_count=1,
        timeout_seconds=60,
        profiler_enabled=True,
    )

    assert "--stage-barrier" in settings.base_command(run_tag="test")
    provenance = build_runtime_provenance(settings, environment={})
    assert provenance["execution_controls"] == {
        "framework_profiler": True,
        "stage_barrier": True,
    }


def test_model_runner_uses_capture_specific_master_port(tmp_path: Path) -> None:
    settings = ModelRunnerSettings(
        source_root=ROOT,
        config_path=tmp_path / "model_runner.env",
        shape=_shape(),
        node_count=1,
        timeout_seconds=60,
        profiler_enabled=True,
    )

    first = settings.base_command(run_tag="capture-one")
    repeated = settings.base_command(run_tag="capture-one")
    second = settings.base_command(run_tag="capture-two")

    first_port = int(first[first.index("--master-port") + 1])
    repeated_port = int(repeated[repeated.index("--master-port") + 1])
    second_port = int(second[second.index("--master-port") + 1])
    assert 15000 <= first_port <= 24999
    assert first_port == repeated_port
    assert first_port != second_port


def test_model_runner_accepts_an_explicit_safe_master_port(tmp_path: Path) -> None:
    settings = ModelRunnerSettings(
        source_root=ROOT,
        config_path=tmp_path / "model_runner.env",
        shape=_shape(),
        node_count=1,
        timeout_seconds=60,
        profiler_enabled=True,
        master_port=30123,
    )

    command = settings.base_command(run_tag="capture")
    assert command[command.index("--master-port") + 1] == "30123"


def test_model_runner_forwards_an_explicit_rank_table(tmp_path: Path) -> None:
    rank_table = tmp_path / "rank_table.json"
    rank_table.write_text("{}\n", encoding="utf-8")
    settings = ModelRunnerSettings(
        source_root=ROOT,
        config_path=tmp_path / "model_runner.env",
        shape=_shape(),
        node_count=1,
        timeout_seconds=60,
        profiler_enabled=True,
        rank_table_file=rank_table,
    )

    command = settings.base_command(run_tag="capture")
    assert command[command.index("--rank-table-file") + 1] == str(rank_table)


def test_model_runner_rejects_a_missing_rank_table(tmp_path: Path) -> None:
    with pytest.raises(ValueError, match="rank table file not found"):
        ModelRunnerSettings(
            source_root=ROOT,
            config_path=tmp_path / "model_runner.env",
            shape=_shape(),
            node_count=1,
            timeout_seconds=60,
            profiler_enabled=True,
            rank_table_file=tmp_path / "missing.json",
        )


@pytest.mark.parametrize("master_port", [0, 55535])
def test_model_runner_rejects_unsafe_master_port(
    tmp_path: Path, master_port: int
) -> None:
    with pytest.raises(ValueError, match="master port"):
        ModelRunnerSettings(
            source_root=ROOT,
            config_path=tmp_path / "model_runner.env",
            shape=_shape(),
            node_count=1,
            timeout_seconds=60,
            profiler_enabled=True,
            master_port=master_port,
        )


def test_runtime_provenance_respects_explicit_dispatch_peer_mode(
    tmp_path: Path,
) -> None:
    config = tmp_path / "model_runner.env"
    config.write_text("MODEL_RUNNER_NODES=node-a\n", encoding="utf-8")
    settings = ModelRunnerSettings(
        source_root=ROOT,
        config_path=config,
        shape=_shape(),
        node_count=1,
        timeout_seconds=60,
        profiler_enabled=True,
    )

    provenance = build_runtime_provenance(
        settings,
        environment={"TILEXR_MOONEP_DISPATCH_PEER_MODE": "legacy"},
    )

    assert provenance["kernel_version"]["dispatch_peer_mode"] == "legacy"


def test_runtime_provenance_hashes_only_an_explicit_rank_table(tmp_path: Path) -> None:
    config = tmp_path / "model_runner.env"
    config.write_text("MODEL_RUNNER_NODES=node-a\n", encoding="utf-8")
    rank_table = tmp_path / "rank_table.json"
    payload = b'{"version":"1.2"}\n'
    rank_table.write_bytes(payload)
    common = {
        "source_root": ROOT,
        "config_path": config,
        "shape": _shape(),
        "node_count": 1,
        "timeout_seconds": 60,
        "profiler_enabled": True,
    }
    environment = {
        "TILEXR_MODEL_REPLAY_FIRMWARE_ID": "firmware",
        "TILEXR_MODEL_REPLAY_SOC_ID": "soc",
    }

    with_rank_table = build_runtime_provenance(
        ModelRunnerSettings(**common, rank_table_file=rank_table),
        environment=environment,
    )
    without_rank_table = build_runtime_provenance(
        ModelRunnerSettings(**common), environment=environment
    )

    assert with_rank_table["topology"]["rank_table_sha256"] == hashlib.sha256(
        payload
    ).hexdigest()
    assert "rank_table_sha256" not in without_rank_table["topology"]


def test_run_main_reads_rank_table_from_environment(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    rank_table = tmp_path / "rank_table.json"
    rank_table.write_text("{}\n", encoding="utf-8")
    captured = {}

    class StopAfterSettings(Exception):
        pass

    def capture_settings(**kwargs):
        captured.update(kwargs)
        raise StopAfterSettings

    monkeypatch.setenv("TILEXR_MODEL_REPLAY_RANK_TABLE_FILE", str(rank_table))
    monkeypatch.setattr(
        replay_orchestrator,
        "ensure_model_runner_config",
        lambda path, **_kwargs: path,
    )
    monkeypatch.setattr(replay_orchestrator, "ModelRunnerSettings", capture_settings)
    args = SimpleNamespace(
        source_root=str(tmp_path),
        meta_root=None,
        model_runner_config=None,
        ep=2,
        rank_size=2,
        s=2,
        k=2,
        hidden_size=128,
        node_rank=0,
        node_count=1,
        model_timeout=60,
        profile=True,
    )

    with pytest.raises(StopAfterSettings):
        replay_orchestrator._run_main(args)

    assert captured["rank_table_file"] == rank_table.resolve()


def _run_main_args(
    tmp_path: Path,
    *,
    meta_root: Path,
    source: str,
) -> SimpleNamespace:
    return SimpleNamespace(
        source_root=str(ROOT),
        cache_root=str(tmp_path / "cache"),
        case_file=str(tmp_path / "case.json"),
        result_env_file=str(tmp_path / "result.env"),
        model_runner_config=str(tmp_path / "missing-model-runner.env"),
        meta_root=str(meta_root),
        ep=2,
        rank_size=2,
        s=2,
        k=2,
        hidden_size=128,
        node_rank=0,
        node_count=1,
        warmup=0,
        iterations=1,
        model_timeout=60,
        wait_timeout=1,
        capture_id="orchestration-order",
        cache_generation=None,
        source=source,
        profile=True,
    )


def test_run_main_meta_hit_does_not_require_model_runner_config(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    meta_root = tmp_path / "meta"
    source = prepare_replay_cache(
        tmp_path / "source-cache",
        _identity(),
        _shape(),
        capture_id="capture-1",
        source="cache",
        capture_routes=_capture_routes,
        capture_performance=_capture_performance,
    )
    monkeypatch.setattr(
        replay_orchestrator,
        "build_runtime_provenance",
        lambda _settings: copy.deepcopy(_identity()["provenance"]),
    )
    monkeypatch.setattr(
        replay_orchestrator,
        "_lookup_existing_replay_cache",
        lambda *_args, **_kwargs: (
            PreparedReplayCache(
                source.entry,
                "meta-hit",
                meta_status="hit",
            ),
            None,
            False,
        ),
    )

    def unexpected_config(*_args, **_kwargs):
        raise AssertionError("meta hit must not generate model runner config")

    monkeypatch.setattr(
        replay_orchestrator, "ensure_model_runner_config", unexpected_config
    )
    monkeypatch.setenv("TILEXR_MODEL_REPLAY_BASH", "/bin/false")

    result = replay_orchestrator._run_main(
        _run_main_args(tmp_path, meta_root=meta_root, source="meta")
    )

    assert result["cache_status"] == "meta-hit"
    assert result["meta_status"] == "hit"
    assert not (tmp_path / "missing-model-runner.env").exists()


def test_run_main_meta_miss_generates_config_only_before_single_capture(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    events: list[str] = []
    meta_root = tmp_path / "missing-meta"
    config = tmp_path / "generated-model-runner.env"
    args = _run_main_args(tmp_path, meta_root=meta_root, source="meta")
    args.model_runner_config = str(config)
    monkeypatch.setattr(
        replay_orchestrator,
        "build_runtime_provenance",
        lambda _settings: copy.deepcopy(_identity()["provenance"]),
    )
    real_load_meta_bundle = replay_orchestrator.load_meta_bundle

    def record_meta_lookup(*load_args, **load_kwargs):
        events.append("meta")
        return real_load_meta_bundle(*load_args, **load_kwargs)

    def generate_config(path, **_kwargs):
        events.append("config")
        Path(path).write_text("MODEL_RUNNER_NODES=node-a\n", encoding="utf-8")
        return Path(path)

    class CaptureStarted(Exception):
        pass

    def capture_once(*_args, **_kwargs):
        events.append("capture")
        raise CaptureStarted

    monkeypatch.setattr(replay_orchestrator, "load_meta_bundle", record_meta_lookup)
    monkeypatch.setattr(
        replay_orchestrator, "ensure_model_runner_config", generate_config
    )
    monkeypatch.setattr(replay_orchestrator, "prepare_with_model_runner", capture_once)

    with pytest.raises(CaptureStarted):
        replay_orchestrator._run_main(args)

    assert events[0:2] == ["meta", "config"]
    assert events.count("config") == 1
    assert events.count("capture") == 1


def test_runtime_provenance_allows_missing_model_runner_config(
    tmp_path: Path,
) -> None:
    settings = ModelRunnerSettings(
        source_root=ROOT,
        config_path=tmp_path / "missing-model-runner.env",
        shape=_shape(),
        node_count=1,
        timeout_seconds=60,
        profiler_enabled=True,
    )

    provenance = build_runtime_provenance(
        settings,
        environment={
            "TILEXR_MODEL_REPLAY_TILEXR_GIT_SHA": "a" * 40,
            "TILEXR_MODEL_REPLAY_FIRMWARE_ID": "firmware",
            "TILEXR_MODEL_REPLAY_SOC_ID": "soc",
            "TILEXR_MOONEP_DISPATCH_PEER_MODE": "group_credit",
        },
    )

    assert provenance["model_stack"] == {
        "mindspeed": "unavailable",
        "mindspeed_llm": "unavailable",
    }
    assert provenance["kernel_version"]["dispatch_peer_mode"] == "group_credit"


def test_runtime_provenance_hashes_real_installed_library_names(tmp_path: Path) -> None:
    lib_dir = tmp_path / "lib64"
    lib_dir.mkdir()
    expected = {}
    for name in (
        "libtile-comm.so",
        "libtilexr-moonep.so",
        "libtilexr-moonep-combine-v2.so",
        "libtilexr-moonep-reduce-grad.so",
    ):
        payload = f"installed:{name}".encode("ascii")
        (lib_dir / name).write_bytes(payload)
        expected[name] = f"sha256:{hashlib.sha256(payload).hexdigest()}"

    assert replay_orchestrator._install_artifacts_identity(tmp_path) == expected


def test_runtime_provenance_accepts_validated_git_revision_override(
    tmp_path: Path,
) -> None:
    config = tmp_path / "model_runner.env"
    config.write_text("MODEL_RUNNER_NODES=node-a\n", encoding="utf-8")
    settings = ModelRunnerSettings(
        source_root=ROOT,
        config_path=config,
        shape=_shape(),
        node_count=1,
        timeout_seconds=60,
        profiler_enabled=True,
    )
    revision = "A" * 40

    provenance = build_runtime_provenance(
        settings,
        environment={
            "TILEXR_MODEL_REPLAY_TILEXR_GIT_SHA": revision,
            "TILEXR_MODEL_REPLAY_SOC_ID": "Ascend950PR-V100",
        },
    )

    assert provenance["tilexr_git_sha"] == revision.lower()
    assert provenance["soc"] == "Ascend950PR-V100"
    with pytest.raises(ValueError, match="Git revision"):
        build_runtime_provenance(
            settings,
            environment={"TILEXR_MODEL_REPLAY_TILEXR_GIT_SHA": "not-a-revision"},
        )


def test_runtime_provenance_discovers_firmware_and_soc_from_npu_smi(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    config = tmp_path / "model_runner.env"
    config.write_text("MODEL_RUNNER_NODES=node-a\n", encoding="utf-8")
    settings = ModelRunnerSettings(
        source_root=ROOT,
        config_path=config,
        shape=_shape(),
        node_count=1,
        timeout_seconds=60,
        profiler_enabled=True,
    )
    board_output = """
        Chip Name                      : Ascend950PR
        Chip Version                   : V100
        Firmware Version               : 9.0.0.200.200
    """

    monkeypatch.setattr(
        replay_orchestrator.subprocess,
        "run",
        lambda *args, **kwargs: subprocess.CompletedProcess(
            args=args[0], returncode=0, stdout=board_output, stderr=""
        ),
    )
    provenance = build_runtime_provenance(
        settings,
        environment={"TILEXR_MODEL_REPLAY_TILEXR_GIT_SHA": "a" * 40},
    )

    assert provenance["firmware"] == "9.0.0.200.200"
    assert provenance["soc"] == "Ascend950PR-V100"


def test_runtime_provenance_board_overrides_skip_npu_probe(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    config = tmp_path / "model_runner.env"
    config.write_text("MODEL_RUNNER_NODES=node-a\n", encoding="utf-8")
    settings = ModelRunnerSettings(
        source_root=ROOT,
        config_path=config,
        shape=_shape(),
        node_count=1,
        timeout_seconds=60,
        profiler_enabled=True,
    )

    def unexpected_probe():
        raise AssertionError("npu-smi probe must not run when overrides are complete")

    monkeypatch.setattr(replay_orchestrator, "_npu_smi_board_identity", unexpected_probe)
    provenance = build_runtime_provenance(
        settings,
        environment={
            "TILEXR_MODEL_REPLAY_FIRMWARE_ID": "firmware-override",
            "TILEXR_MODEL_REPLAY_SOC_ID": "soc-override",
        },
    )

    assert provenance["firmware"] == "firmware-override"
    assert provenance["soc"] == "soc-override"


def test_single_node_missing_model_runner_config_is_generated(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    source_root = tmp_path / "TileXR"
    install_prefix = source_root / "install-clean-b131"
    model_root = tmp_path / "model"
    cann_env = tmp_path / "b131" / "cann-9.1.0" / "set_env.sh"
    stale_ascend_home = tmp_path / "stale-cann"
    conda_sh = tmp_path / "conda" / "etc" / "profile.d" / "conda.sh"
    native_env = model_root / "moonep-native-build-test" / "moonep-native.env"
    tokenizer = tmp_path / "tokenizer"
    for directory in (
        install_prefix,
        model_root / "MindSpeed",
        model_root / "MindSpeed-LLM",
        model_root / "shmem" / "src" / "python",
        tokenizer,
    ):
        directory.mkdir(parents=True)
    for file_path in (cann_env, stale_ascend_home / "set_env.sh", conda_sh, native_env):
        file_path.parent.mkdir(parents=True, exist_ok=True)
        file_path.write_text("# test\n", encoding="utf-8")
    monkeypatch.setattr(
        replay_orchestrator,
        "_DEFAULT_CANN_ENV_CANDIDATES",
        (str(cann_env),),
    )

    config = tmp_path / "run" / "generated" / "model_runner.env"
    resolved = ensure_model_runner_config(
        config,
        source_root=source_root,
        shape=_shape(),
        node_count=1,
        environment={
            "TILEXR_INSTALL_PREFIX": str(install_prefix),
            "TILEXR_MODEL_RUNNER_MODEL_ROOT": str(model_root),
            "TILEXR_MODEL_RUNNER_CONDA_SH": str(conda_sh),
            "TILEXR_MODEL_RUNNER_NATIVE_ENV": str(native_env),
            "TILEXR_MODEL_RUNNER_TOKENIZER_PATH": str(tokenizer),
            "TILEXR_MODEL_RUNNER_NODES": "node-a",
            "TILEXR_MOONEP_CONDA_ENV": "ai_moe_test",
            "ASCEND_HOME_PATH": str(stale_ascend_home),
        },
    )

    text = config.read_text(encoding="utf-8")
    values = _parse_runner_env(text)
    assert resolved == config.resolve()
    assert "Generated by tools/moonep/model_replay_orchestrator.py" in text
    assert values["MODEL_RUNNER_NODES"] == "node-a"
    assert values["MODEL_RUNNER_DEVICES_PER_NODE"] == str(_shape().world_size)
    assert values["MODEL_RUNNER_TILEXR_HOME"] == str(source_root.resolve())
    assert values["MODEL_RUNNER_INSTALL_PREFIX"] == str(install_prefix.resolve())
    assert values["MODEL_RUNNER_CANN_ENV"] == str(cann_env.resolve())
    assert values["MODEL_RUNNER_CONDA_ENV"] == "ai_moe_test"


def test_generated_runner_config_accepts_absolute_conda_prefix_without_base(
    tmp_path: Path,
) -> None:
    source_root = tmp_path / "TileXR"
    install_prefix = source_root / "install"
    model_root = tmp_path / "model"
    cann_env = tmp_path / "cann" / "set_env.sh"
    native_env = model_root / "native" / "moonep-native.env"
    tokenizer = tmp_path / "tokenizer"
    conda_prefix = tmp_path / "envs" / "ai_moe_test"
    for directory in (install_prefix, model_root, tokenizer, conda_prefix / "bin"):
        directory.mkdir(parents=True)
    for file_path in (cann_env, native_env, conda_prefix / "bin" / "python"):
        file_path.parent.mkdir(parents=True, exist_ok=True)
        file_path.write_text("# test\n", encoding="utf-8")

    config = tmp_path / "generated" / "model_runner.env"
    ensure_model_runner_config(
        config,
        source_root=source_root,
        shape=_shape(),
        node_count=1,
        environment={
            "TILEXR_INSTALL_PREFIX": str(install_prefix),
            "TILEXR_MODEL_RUNNER_MODEL_ROOT": str(model_root),
            "TILEXR_MODEL_RUNNER_CANN_ENV": str(cann_env),
            "TILEXR_MODEL_RUNNER_NATIVE_ENV": str(native_env),
            "TILEXR_MODEL_RUNNER_TOKENIZER_PATH": str(tokenizer),
            "TILEXR_MOONEP_CONDA_ENV": str(conda_prefix),
        },
    )

    values = _parse_runner_env(config.read_text(encoding="utf-8"))
    assert values["MODEL_RUNNER_CONDA_ENV"] == str(conda_prefix)
    assert values["MODEL_RUNNER_CONDA_SH"] == ""


def test_multinode_missing_model_runner_config_requires_explicit_env(
    tmp_path: Path,
) -> None:
    with pytest.raises(FileNotFoundError, match="multi-node replay needs"):
        ensure_model_runner_config(
            tmp_path / "missing.env",
            source_root=tmp_path,
            shape=_shape(),
            node_count=2,
            environment={},
        )
