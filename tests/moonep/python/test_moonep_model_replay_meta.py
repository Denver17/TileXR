from __future__ import annotations

import base64
import copy
import hashlib
import json
import struct
import zlib
from pathlib import Path

import pytest

from tools.moonep import model_replay_meta
from tools.moonep.model_flow import model_operator_order
from tools.moonep.model_replay_cache import ReplayShape, build_cache_identity
from tools.moonep.model_replay_meta import (
    MetaValidationError,
    load_meta_bundle,
    materialize_meta_bundle,
    write_meta_bundle,
)


def _shape(
    *, expert_count: int = 32, ep_size: int = 8, world_size: int = 8
) -> ReplayShape:
    return ReplayShape(
        tokens_per_rank=4,
        topk=2,
        hidden_size=128,
        ep_size=ep_size,
        world_size=world_size,
        expert_count=expert_count,
        ffn_hidden_size=32,
        token_padding=1,
        forward_calls=10,
    )


def _provenance(world_size: int = 8) -> dict[str, object]:
    node_count = world_size // 8
    return {
        "tilexr_git_sha": "0123456789abcdef",
        "adapter_sha256": "a" * 64,
        "runner_sha256": "b" * 64,
        "install_artifacts": {"libtile-comm.so": "sha256:" + "c" * 64},
        "model_stack": {"mindspeed": "model-a", "mindspeed_llm": "model-b"},
        "backend": "tilexr",
        "execution_controls": {
            "framework_profiler": True,
            "stage_barrier": True,
        },
        "kernel_version": {
            "combine": "2",
            "dispatch_peer_mode": "group",
            "performance_mode": "framework_profiler",
        },
        "cann": "sha256:" + "d" * 64,
        "driver": "sha256:" + "e" * 64,
        "firmware": "firmware-a",
        "soc": "Ascend950PR",
        "topology": {
            "nodes": [f"141.61.49.{223 - node}" for node in range(node_count)],
            "node_count": node_count,
            "devices_per_node": 8,
        },
        "rank_mapping": [
            {"rank": rank, "node": rank // 8, "device": rank % 8}
            for rank in range(world_size)
        ],
    }


def _identity(shape: ReplayShape | None = None) -> dict[str, object]:
    return build_cache_identity(
        shape or _shape(),
        provenance=_provenance((shape or _shape()).world_size),
        operator_order=model_operator_order(),
    )


def _route_values(unique: int, shape: ReplayShape) -> list[int]:
    first = unique // (shape.expert_count - 1)
    second = unique % (shape.expert_count - 1)
    if second >= first:
        second += 1
    values = [first, second]
    for token in range(1, shape.tokens_per_rank):
        expert = (unique * 7 + token * 3) % shape.expert_count
        values.extend((expert, (expert + 1) % shape.expert_count))
    return values


def _route_replay(shape: ReplayShape | None = None) -> dict[str, object]:
    shape = shape or _shape()
    ranks: dict[str, list[dict[str, object]]] = {}
    for rank in range(shape.world_size):
        records = []
        for call in range(shape.forward_calls):
            unique = (rank * shape.forward_calls + call) // 2
            values = _route_values(unique, shape)
            raw = struct.pack(f"<{len(values)}i", *values)
            counts = [values.count(expert) for expert in range(shape.expert_count)]
            records.append(
                {
                    "call": call,
                    "source_call": call + 60,
                    "topk_shape": [shape.tokens_per_rank, shape.topk],
                    "topk_dtype": "torch.int32",
                    "topk_encoding": "int32-le-zlib-base64",
                    "topk_zlib_base64": base64.b64encode(zlib.compress(raw)).decode(
                        "ascii"
                    ),
                    "topk_sha256": hashlib.sha256(raw).hexdigest(),
                    "tokens_per_expert": counts,
                    "remote_stats": [unique % 5, unique % 3],
                    "experts_to_copy": [
                        [owner * shape.experts_per_rank + slot for slot in range(shape.experts_per_rank)]
                        for owner in range(shape.world_size)
                    ],
                    "payload_seed": rank * 100 + call,
                    "tensor_descriptors": {
                        "hidden": {
                            "shape": [shape.tokens_per_rank, shape.hidden_size],
                            "dtype": "torch.bfloat16",
                        }
                    },
                }
            )
        ranks[str(rank)] = records
    return {
        "schema_version": 1,
        "dimensions": {
            "world_size": shape.world_size,
            "tokens_per_rank": shape.tokens_per_rank,
            "topk": shape.topk,
            "expert_count": shape.expert_count,
            "forward_calls": shape.forward_calls,
        },
        "payload_contract": {
            "captured": "topk,plan_metadata,tensor_descriptors",
            "regenerated": "payload_values_from_payload_seed",
        },
        "provenance": {
            "capture_id": "model-replay-12345-6789",
            "source_git_sha": "0123456789abcdef",
            "source": "TileXR MindSpeed model route and plan capture",
        },
        "ranks": ranks,
    }


def _performance(shape: ReplayShape | None = None) -> dict[str, object]:
    shape = shape or _shape()
    order = model_operator_order()
    return {
        "schema_version": 1,
        "world_size": shape.world_size,
        "profiler_enabled": True,
        "timing_source": "torch_npu_profiler_kernel_details",
        "operator_order": order,
        "ranks": {
            str(rank): {
                "timing_artifact": f"/private/host/rank_{rank}/kernel_details.csv",
                "operators": [
                    {
                        "sequence": sequence,
                        "stage": item["stage"],
                        "phase": item["phase"],
                        "layer": item["layer"],
                        "backend": "tilexr",
                        "kernel_version": f"kernel-{item['stage']}",
                        "kernel_launches_per_call": 1,
                        "algorithm_bytes": None if item["stage"] == "planning" else 1024,
                        "latency_us": 10.0 + rank + sequence,
                        "algorithm_bandwidth_GBps": (
                            None if item["stage"] == "planning" else 0.1
                        ),
                    }
                    for sequence, item in enumerate(order)
                ],
            }
            for rank in range(shape.world_size)
        },
    }


def _runtime_topk_raw(replay: dict[str, object], rank: int, call: int) -> bytes:
    record = replay["ranks"][str(rank)][call]
    return zlib.decompress(base64.b64decode(record["topk_zlib_base64"]))


def _rewrite_meta(path: Path, update) -> None:
    payload = json.loads(path.read_text(encoding="utf-8"))
    update(payload)
    path.write_text(json.dumps(payload, sort_keys=True), encoding="utf-8")


def test_u8_zstd_round_trip_deduplicates_80_calls_to_40_routes(
    tmp_path: Path,
) -> None:
    shape = _shape()
    original = _route_replay(shape)
    bundle = write_meta_bundle(
        tmp_path,
        shape,
        original,
        _performance(shape),
        _identity(shape),
        replace=False,
    )

    assert bundle.path.name == "model-replay-s4-k2-h128-ep8-r8"
    assert {path.name for path in bundle.path.iterdir()} == {
        "meta.json",
        "routes.u8.zst",
        "performance.json",
    }
    assert bundle.meta["schema_version"] == 1
    assert bundle.meta["shape"] == {
        "S": 4,
        "K": 2,
        "H": 128,
        "EP": 8,
        "R": 8,
        "E": 32,
        "Hf": 32,
        "P": 1,
        "forward_calls": 10,
    }
    assert bundle.meta["routes"]["codec"] == "zstd"
    assert bundle.meta["routes"]["dtype"] == "uint8"
    assert bundle.meta["routes"]["compression_level"] == 3
    assert (bundle.path / "routes.u8.zst").read_bytes().startswith(b"\x28\xb5\x2f\xfd")
    assert len(bundle.meta["unique_routes"]) == 40
    assert sum(len(calls) for calls in bundle.meta["rank_calls"].values()) == 80
    meta_bytes = (bundle.path / "meta.json").read_bytes()
    assert meta_bytes.endswith(b"\n")
    assert meta_bytes.count(b"\n") == 1

    loaded = load_meta_bundle(tmp_path, shape, _identity(shape))
    assert loaded is not None
    replay, performance = materialize_meta_bundle(loaded, _identity(shape))
    for rank in range(shape.world_size):
        for call in range(shape.forward_calls):
            assert _runtime_topk_raw(replay, rank, call) == _runtime_topk_raw(
                original, rank, call
            )
    assert performance["comparison"]["classification"] == "direct baseline"

    checked_in = (bundle.path / "meta.json").read_text(encoding="utf-8")
    checked_in += (bundle.path / "performance.json").read_text(encoding="utf-8")
    assert "topk_zlib_base64" not in checked_in
    assert "141.61.49.223" not in checked_in
    assert "/private/host" not in checked_in
    assert "model-replay-12345-6789" not in checked_in


def test_dotted_firmware_version_is_not_mistaken_for_an_ip_address(
    tmp_path: Path,
) -> None:
    identity = _identity()
    identity["provenance"]["firmware"] = "9.0.0.200.200"

    bundle = write_meta_bundle(
        tmp_path,
        _shape(),
        _route_replay(),
        _performance(),
        identity,
        replace=False,
    )

    assert bundle.performance["provenance"]["firmware"] == "9.0.0.200.200"


def test_embedded_ip_address_is_rejected_from_checked_in_performance(
    tmp_path: Path,
) -> None:
    identity = _identity()
    identity["provenance"]["firmware"] = "source-host=141.61.49.223"

    with pytest.raises(MetaValidationError, match="IP address is forbidden"):
        write_meta_bundle(
            tmp_path,
            _shape(),
            _route_replay(),
            _performance(),
            identity,
            replace=False,
        )


@pytest.mark.parametrize("mode", ["checksum", "truncated"])
def test_corrupt_or_truncated_zstd_is_rejected(tmp_path: Path, mode: str) -> None:
    bundle = write_meta_bundle(
        tmp_path,
        _shape(),
        _route_replay(),
        _performance(),
        _identity(),
        replace=False,
    )
    route_path = bundle.path / "routes.u8.zst"
    payload = bytearray(route_path.read_bytes())
    if mode == "checksum":
        payload[len(payload) // 2] ^= 0x01
    else:
        del payload[-5:]
    route_path.write_bytes(payload)

    with pytest.raises(MetaValidationError, match="route blob"):
        load_meta_bundle(tmp_path, _shape(), _identity())


def test_impossible_uncompressed_size_is_rejected_before_decompression(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    bundle = write_meta_bundle(
        tmp_path,
        _shape(),
        _route_replay(),
        _performance(),
        _identity(),
        replace=False,
    )
    _rewrite_meta(
        bundle.path / "meta.json",
        lambda meta: meta["routes"].update({"uncompressed_size": 2**40}),
    )
    monkeypatch.setattr(
        model_replay_meta,
        "zstd_decompress",
        lambda *_args, **_kwargs: (_ for _ in ()).throw(
            AssertionError("invalid size must be rejected before zstd")
        ),
    )

    with pytest.raises(MetaValidationError, match="uncompressed size"):
        load_meta_bundle(tmp_path, _shape(), _identity())


@pytest.mark.parametrize("damage", ["route_id", "missing_rank", "missing_call"])
def test_invalid_route_references_are_rejected(tmp_path: Path, damage: str) -> None:
    bundle = write_meta_bundle(
        tmp_path,
        _shape(),
        _route_replay(),
        _performance(),
        _identity(),
        replace=False,
    )
    meta_path = bundle.path / "meta.json"

    def update(meta: dict[str, object]) -> None:
        if damage == "route_id":
            meta["rank_calls"]["0"][0]["route_id"] = 999
        elif damage == "missing_rank":
            del meta["rank_calls"]["7"]
        else:
            meta["rank_calls"]["0"].pop()

    _rewrite_meta(meta_path, update)

    with pytest.raises(MetaValidationError, match="route_id|rank|call"):
        load_meta_bundle(tmp_path, _shape(), _identity())


def test_expert_count_above_uint8_range_is_rejected(tmp_path: Path) -> None:
    shape = _shape(expert_count=264)
    with pytest.raises(MetaValidationError, match="E=264.*uint8"):
        write_meta_bundle(
            tmp_path,
            shape,
            _route_replay(shape),
            _performance(shape),
            _identity(shape),
            replace=False,
        )


def test_incompatible_environment_keeps_performance_as_checked_in_reference(
    tmp_path: Path,
) -> None:
    bundle = write_meta_bundle(
        tmp_path,
        _shape(),
        _route_replay(),
        _performance(),
        _identity(),
        replace=False,
    )
    changed = copy.deepcopy(_identity())
    changed["provenance"]["driver"] = "different-driver"

    loaded = load_meta_bundle(tmp_path, _shape(), changed)
    assert loaded is not None
    _, performance = materialize_meta_bundle(loaded, changed)

    assert performance["comparison"]["compatible"] is False
    assert performance["comparison"]["classification"] == "checked-in reference"


@pytest.mark.parametrize("replay_mode", ["group", "group_credit"])
def test_dispatch_peer_mode_does_not_block_route_materialization(
    tmp_path: Path, replay_mode: str
) -> None:
    captured = _identity()
    captured["provenance"]["kernel_version"]["dispatch_peer_mode"] = "legacy"
    write_meta_bundle(
        tmp_path,
        _shape(),
        _route_replay(),
        _performance(),
        captured,
        replace=False,
    )
    current = copy.deepcopy(captured)
    current["provenance"]["kernel_version"]["dispatch_peer_mode"] = replay_mode

    bundle = load_meta_bundle(tmp_path, _shape(), current)
    assert bundle is not None
    replay, performance = materialize_meta_bundle(bundle, current)

    assert len(replay["ranks"]) == _shape().world_size
    assert performance["comparison"]["classification"] == "checked-in reference"
    assert performance["comparison"]["dispatch_peer_mode"] == {
        "compatible": False,
        "model": "legacy",
        "replay": replay_mode,
    }


def test_code_build_provenance_does_not_block_route_materialization(
    tmp_path: Path,
) -> None:
    captured = _identity()
    write_meta_bundle(
        tmp_path,
        _shape(),
        _route_replay(),
        _performance(),
        captured,
        replace=False,
    )
    current = copy.deepcopy(captured)
    current["provenance"]["adapter_sha256"] = "different-adapter"
    current["provenance"]["runner_sha256"] = "different-runner"
    current["provenance"]["install_artifacts"] = {
        "libtile-comm.so": "different-comm",
        "libtilexr-moonep.so": "different-moonep",
    }
    current["provenance"]["model_stack"] = {
        "mindspeed": "different-mindspeed",
        "mindspeed_llm": "different-mindspeed-llm",
    }

    bundle = load_meta_bundle(tmp_path, _shape(), current)
    assert bundle is not None
    replay, performance = materialize_meta_bundle(bundle, current)

    assert len(replay["ranks"]) == _shape().world_size
    assert performance["comparison"]["compatible"] is False
    assert performance["comparison"]["classification"] == "checked-in reference"


def test_route_materialization_rejects_combine_kernel_changes(
    tmp_path: Path,
) -> None:
    captured = _identity()
    write_meta_bundle(
        tmp_path,
        _shape(),
        _route_replay(),
        _performance(),
        captured,
        replace=False,
    )
    current = copy.deepcopy(captured)
    current["provenance"]["kernel_version"]["combine"] = "unexpected-combine"

    with pytest.raises(MetaValidationError, match="route contract is incompatible"):
        load_meta_bundle(tmp_path, _shape(), current)


@pytest.mark.parametrize(
    ("world_size", "ep_size"),
    [(8, 8), (16, 16)],
)
def test_same_shape_meta_materializes_on_another_machine_without_recapture(
    tmp_path: Path, world_size: int, ep_size: int
) -> None:
    shape = _shape(world_size=world_size, ep_size=ep_size)
    identity = _identity(shape)
    write_meta_bundle(
        tmp_path,
        shape,
        _route_replay(shape),
        _performance(shape),
        identity,
        replace=False,
    )
    other_host = copy.deepcopy(identity)
    other_host["provenance"].update(
        {
            "driver": "different-driver",
            "firmware": "different-firmware",
            "soc": "different-soc",
            "topology": {
                "nodes": ["private-host-name"] * (world_size // 8),
                "node_count": world_size // 8,
                "devices_per_node": 8,
            },
        }
    )

    bundle = load_meta_bundle(tmp_path, shape, other_host)
    assert bundle is not None
    replay, performance = materialize_meta_bundle(bundle, other_host)

    assert len(replay["ranks"]) == world_size
    assert all(len(calls) == 10 for calls in replay["ranks"].values())
    assert performance["comparison"]["classification"] == "checked-in reference"


def test_replace_publishes_an_exact_bundle_without_stale_files(tmp_path: Path) -> None:
    first = write_meta_bundle(
        tmp_path,
        _shape(),
        _route_replay(),
        _performance(),
        _identity(),
        replace=False,
    )
    (first.path / "stale.json").write_text("{}", encoding="utf-8")

    replaced = write_meta_bundle(
        tmp_path,
        _shape(),
        _route_replay(),
        _performance(),
        _identity(),
        replace=True,
    )

    assert {path.name for path in replaced.path.iterdir()} == {
        "meta.json",
        "performance.json",
        "routes.u8.zst",
    }


def test_failed_directory_swap_restores_previous_bundle(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    first = write_meta_bundle(
        tmp_path,
        _shape(),
        _route_replay(),
        _performance(),
        _identity(),
        replace=False,
    )
    original_meta_sha256 = hashlib.sha256(
        (first.path / "meta.json").read_bytes()
    ).hexdigest()
    real_replace = model_replay_meta.os.replace
    destination = first.path
    monkeypatch.setattr(model_replay_meta, "_exchange_directories", lambda *_: False)

    def fail_staging_publish(source, target) -> None:
        if Path(source).name.startswith(f".{destination.name}.staging-"):
            raise OSError("injected staging publish failure")
        real_replace(source, target)

    monkeypatch.setattr(model_replay_meta.os, "replace", fail_staging_publish)

    with pytest.raises(OSError, match="injected staging publish failure"):
        write_meta_bundle(
            tmp_path,
            _shape(),
            _route_replay(),
            _performance(),
            _identity(),
            replace=True,
        )

    assert hashlib.sha256((destination / "meta.json").read_bytes()).hexdigest() == (
        original_meta_sha256
    )
    assert load_meta_bundle(tmp_path, _shape(), _identity()) is not None


@pytest.mark.parametrize("damage", ["missing_soc", "profiler_mismatch"])
def test_incomplete_or_inconsistent_performance_provenance_is_rejected(
    tmp_path: Path, damage: str
) -> None:
    bundle = write_meta_bundle(
        tmp_path,
        _shape(),
        _route_replay(),
        _performance(),
        _identity(),
        replace=False,
    )
    performance_path = bundle.path / "performance.json"
    performance = json.loads(performance_path.read_text(encoding="utf-8"))
    if damage == "missing_soc":
        del performance["provenance"]["soc"]
    else:
        performance["profiler_enabled"] = False
    performance_path.write_text(json.dumps(performance), encoding="utf-8")
    performance_sha256 = hashlib.sha256(performance_path.read_bytes()).hexdigest()
    _rewrite_meta(
        bundle.path / "meta.json",
        lambda meta: meta["performance"].update({"sha256": performance_sha256}),
    )

    with pytest.raises(MetaValidationError, match="provenance|profiler"):
        load_meta_bundle(tmp_path, _shape(), _identity())


def test_replace_uses_atomic_directory_exchange_when_available(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    first = write_meta_bundle(
        tmp_path,
        _shape(),
        _route_replay(),
        _performance(),
        _identity(),
        replace=False,
    )
    exchanges = []

    def exchange(left: Path, right: Path) -> bool:
        exchanges.append((left, right))
        temporary = right.with_name(f".{right.name}.test-exchange")
        model_replay_meta.os.replace(right, temporary)
        model_replay_meta.os.replace(left, right)
        model_replay_meta.os.replace(temporary, left)
        return True

    monkeypatch.setattr(model_replay_meta, "_exchange_directories", exchange)
    replaced = write_meta_bundle(
        tmp_path,
        _shape(),
        _route_replay(),
        _performance(),
        _identity(),
        replace=True,
    )

    assert exchanges and exchanges[0][1] == first.path
    assert load_meta_bundle(tmp_path, _shape(), _identity()) == replaced


@pytest.mark.parametrize(
    "case_id",
    [
        "model-replay-s4096-k8-h7168-ep8-r8",
        "model-replay-s4096-k8-h7168-ep16-r16",
        "model-replay-s8192-k16-h3584-ep8-r8",
    ],
)
def test_checked_in_bundle_passes_production_validation(case_id: str) -> None:
    meta_root = Path(__file__).resolve().parents[3] / "tools/moonep/model_replay_meta"
    bundle_path = meta_root / case_id
    meta = json.loads((bundle_path / "meta.json").read_text(encoding="utf-8"))
    performance = json.loads(
        (bundle_path / "performance.json").read_text(encoding="utf-8")
    )
    shape_values = meta["shape"]
    shape = ReplayShape(
        tokens_per_rank=shape_values["S"],
        topk=shape_values["K"],
        hidden_size=shape_values["H"],
        ep_size=shape_values["EP"],
        world_size=shape_values["R"],
        expert_count=shape_values["E"],
        ffn_hidden_size=shape_values["Hf"],
        token_padding=shape_values["P"],
        forward_calls=shape_values["forward_calls"],
    )
    contract = meta["compatibility"]["route_contract"]
    provenance = copy.deepcopy(performance["provenance"])
    for field in (
        "backend",
        "adapter_sha256",
        "runner_sha256",
        "install_artifacts",
        "model_stack",
        "kernel_version",
    ):
        provenance[field] = copy.deepcopy(contract[field])
    identity = {
        "schema_version": contract["cache_schema_version"],
        "capture_schema_version": contract["capture_schema_version"],
        "shape": copy.deepcopy(contract["shape"]),
        "operator_order": copy.deepcopy(contract["operator_order"]),
        "payload_contract": copy.deepcopy(contract["payload_contract"]),
        "provenance": provenance,
    }

    bundle = load_meta_bundle(meta_root, shape, identity)
    assert bundle is not None
    assert {path.name for path in bundle.path.iterdir()} == {
        "meta.json",
        "performance.json",
        "routes.u8.zst",
    }
    assert bundle.route_bytes
    assert max(bundle.route_bytes) < shape.expert_count
    assert len(bundle.meta["rank_calls"]) == shape.world_size
    assert sum(len(calls) for calls in bundle.meta["rank_calls"].values()) == (
        shape.world_size * shape.forward_calls
    )

    replay, materialized_performance = materialize_meta_bundle(bundle, identity)
    assert len(replay["ranks"]) == shape.world_size
    assert all(
        len(calls) == shape.forward_calls for calls in replay["ranks"].values()
    )
    assert len(materialized_performance["ranks"]) == shape.world_size
    assert all(
        len(record["operators"]) == len(model_operator_order())
        for record in materialized_performance["ranks"].values()
    )
    assert materialized_performance["comparison"]["classification"] == (
        "direct baseline"
    )

    captured_mode = contract["kernel_version"]["dispatch_peer_mode"]
    replay_mode = "group" if captured_mode != "group" else "group_credit"
    cross_mode_identity = copy.deepcopy(identity)
    cross_mode_identity["provenance"]["kernel_version"][
        "dispatch_peer_mode"
    ] = replay_mode
    cross_mode_bundle = load_meta_bundle(meta_root, shape, cross_mode_identity)
    assert cross_mode_bundle is not None
    _, cross_mode_performance = materialize_meta_bundle(
        cross_mode_bundle, cross_mode_identity
    )
    assert cross_mode_performance["comparison"]["classification"] == (
        "checked-in reference"
    )
    assert cross_mode_performance["comparison"]["dispatch_peer_mode"] == {
        "compatible": False,
        "model": captured_mode,
        "replay": replay_mode,
    }
