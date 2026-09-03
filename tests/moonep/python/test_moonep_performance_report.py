from __future__ import annotations

import json
from pathlib import Path
from types import SimpleNamespace

import pytest
import torch

from tools.moonep.benchmark import algorithm_bytes, stage_execution_metadata
from tools.moonep.model_flow import model_operator_order
from tools.moonep.report import (
    FLOW_STAGE_ORDER,
    aggregate_distributed_artifacts,
    aggregate_rank_artifacts,
    format_dispatch_performance,
    format_model_flow_performance,
    format_stage_performance,
    main as report_main,
    write_json,
    write_jsonl,
)


def test_dispatch_hot_loop_summary_prints_dispatch_metrics(
    tmp_path: Path, capsys
) -> None:
    summary = {
        "benchmark_kind": "dispatch_hot_loop",
        "mode": "benchmark",
        "case": {
            "case_id": "dispatch-8rank-4k-ep8-grouped-urma",
            "tokens_per_rank": 4096,
            "topk": 8,
            "expert_count": 32,
            "hidden_size": 7168,
            "intermediate_size": 2048,
            "prefetch_slots": 4,
            "token_padding": 1,
            "dtype": "bfloat16",
            "seed": 1234,
            "routing_pattern": "unique_destinations",
            "route_distribution": "rank_shifted_uniform",
            "correctness": True,
            "warmup": 0,
            "iterations": 8,
        },
        "dispatch_modes": ["hidden"],
        "metrics_us": {
            "hidden_host": {"mean": 40.0, "p50": 35.0, "p95": 80.0},
            "hidden_kernel": {"mean": 2200.0, "p50": 2150.0, "p95": 2300.0},
        },
        "tokens_per_second_by_mode": {
            "hidden": {"mean": 14_000_000.0},
        },
    }

    table = format_dispatch_performance(summary)
    assert "MoonEP Dispatch-only performance" in table
    assert "hidden" in table
    assert "2200.000" in table
    assert "14000000.000" in table

    summary_path = tmp_path / "summary.json"
    write_json(summary_path, summary)
    assert report_main(["--summary", str(summary_path)]) == 0
    assert "MoonEP Dispatch-only performance" in capsys.readouterr().out


RAW_TIMINGS = {
    "planning": 1.0,
    "dispatch_forward": 1.0,
    "prefetch_weight": 1.0,
    "expert_forward": 1.0,
    "combine_forward": 1.0,
    "dispatch_backward": 1.0,
    "expert_backward": 1.0,
    "combine_backward": 1.0,
    "reduce_grad": 1.0,
    "end_to_end": 10.0,
}


def test_algorithm_bytes_follow_routes_and_runtime_tensor_dtypes() -> None:
    plan = SimpleNamespace(
        dst=torch.tensor([0, 8, -10], dtype=torch.int32),
        remote_stats=torch.tensor([2, 3], dtype=torch.int32),
    )
    inputs = {
        "hidden": torch.empty((2, 4), dtype=torch.bfloat16),
        "route_weights": torch.empty((2, 1), dtype=torch.float32),
        "projections": SimpleNamespace(
            gate=torch.empty((4, 2, 3), dtype=torch.bfloat16),
            up=torch.empty((4, 2, 3), dtype=torch.bfloat16),
            down=torch.empty((4, 3, 2), dtype=torch.bfloat16),
        ),
        "gradients": SimpleNamespace(
            gate=torch.empty((4, 2, 3), dtype=torch.float32),
            up=torch.empty((4, 2, 3), dtype=torch.float32),
            down=torch.empty((4, 3, 2), dtype=torch.float32),
        ),
    }
    context = SimpleNamespace(
        nv_s=8,
        planner_group_rank=0,
        planner_group_size=2,
    )

    assert algorithm_bytes(plan, inputs, context) == {
        "dispatch": 40,
        "prefetch_weight": 72,
        "combine": 40,
        "reduce_grad": 216,
    }


def _result(rank: int, world_size: int, node_count: int) -> dict[str, object]:
    capabilities = {
        "stage_mask": 31,
        "stub_mask": 0,
        "transport_performance_valid": True,
        "transport_correctness_valid": True,
        "implementations": {
            "planning": "native",
            "dispatch": "native",
            "prefetch_weight": "native",
            "combine": "native",
            "reduce_grad": "native",
        },
    }
    environment = {
        "git_sha": "0123456789abcdef",
        "git_dirty": False,
        "torch_npu": "2.10.0.post2",
    }
    return {
        "benchmark_kind": "flow",
        "status": "passed",
        "mode": "benchmark",
        "launch_id": "mock-launch",
        "rank": rank,
        "case": {
            "case_id": "flow",
            "tokens_per_rank": 4,
            "topk": 2,
            "expert_count": 8,
            "hidden_size": 16,
            "intermediate_size": 32,
            "prefetch_slots": 2,
            "token_padding": 4,
            "dtype": "bfloat16",
            "seed": 1234,
            "warmup": 2,
            "iterations": 1,
            "correctness": False,
            "routing_pattern": "balanced",
            "route_distribution": "rank_shifted_uniform",
        },
        "capabilities": capabilities,
        "environment": environment,
        "benchmark_config": {"wait_iterations": 1000000},
        "stage_execution": stage_execution_metadata(
            capabilities,
            torch_npu_version=environment["torch_npu"],
        ),
        "topology": {
            "global_rank": rank,
            "global_world_size": world_size,
            "node_rank": rank // (world_size // node_count),
            "node_count": node_count,
            "visible_devices": "0,1",
            "local_rank": rank % (world_size // node_count),
            "local_world_size": world_size // node_count,
            "planner_group_size": world_size,
            "lane_group_size": 1,
            "physical_device_count": world_size // node_count,
            "ranks_per_device": 1,
            "oversubscribed": False,
            "planner_block_dim": 64,
            "planner_block_dim_source": "distributed_cli",
            "dispatch_aiv_core_count": 64,
            "dispatch_aiv_core_count_source": "distributed_cli",
            "udma_qp_route_spec": "port_count:6,port_count:2",
            "peer_memory_cross_node": node_count > 1,
            "cross_node_validated": False,
        },
        "validation": {"passed": True},
    }


def _sample(
    iteration: int,
    *,
    planning: float,
    dispatch: tuple[float, float],
    prefetch: float,
    expert: tuple[float, float],
    combine: tuple[float, float],
    reduce_grad: float,
    algorithm_bytes: tuple[int, int, int, int],
) -> dict[str, object]:
    timings = dict(RAW_TIMINGS)
    timings.update(
        {
            "planning": planning,
            "dispatch_forward": dispatch[0],
            "dispatch_backward": dispatch[1],
            "prefetch_weight": prefetch,
            "expert_forward": expert[0],
            "expert_backward": expert[1],
            "combine_forward": combine[0],
            "combine_backward": combine[1],
            "reduce_grad": reduce_grad,
        }
    )
    return {
        "iteration": iteration,
        "timings_us": timings,
        "algorithm_bytes": dict(
            zip(
                ("dispatch", "prefetch_weight", "combine", "reduce_grad"),
                algorithm_bytes,
            )
        ),
    }


def _write_rank(
    case_dir: Path,
    rank: int,
    samples: list[dict[str, object]],
    *,
    world_size: int = 2,
    node_count: int = 1,
    case_overrides: dict[str, object] | None = None,
) -> None:
    rank_dir = case_dir / f"rank_{rank}"
    result = _result(rank, world_size, node_count)
    result["case"].update(case_overrides or {})
    write_json(rank_dir / "result.json", result)
    write_jsonl(rank_dir / "samples.jsonl", samples)


def test_flow_report_uses_stage_critical_rank_and_its_bytes(tmp_path: Path) -> None:
    case_dir = tmp_path / "flow"
    _write_rank(
        case_dir,
        0,
        [
            _sample(
                0,
                planning=1,
                dispatch=(4, 6),
                prefetch=5,
                expert=(8, 2),
                combine=(3, 17),
                reduce_grad=2,
                algorithm_bytes=(100, 1000, 200, 2000),
            )
        ],
    )
    _write_rank(
        case_dir,
        1,
        [
            _sample(
                0,
                planning=2,
                dispatch=(8, 4),
                prefetch=4,
                expert=(6, 5),
                combine=(9, 5),
                reduce_grad=3,
                algorithm_bytes=(240, 2000, 400, 4000),
            )
        ],
    )

    summary = aggregate_rank_artifacts(case_dir, world_size=2)

    assert tuple(summary["stage_performance"]) == FLOW_STAGE_ORDER
    stage_samples = summary["cross_rank_stage_samples"][0]["stages"]
    assert stage_samples["dispatch"] == {
        "critical_rank": 1,
        "duration_us": 12.0,
        "algorithm_bytes": 240,
        "algorithm_bandwidth_GBps": pytest.approx(0.02),
    }
    assert stage_samples["prefetch_weight"]["critical_rank"] == 0
    assert stage_samples["prefetch_weight"]["algorithm_bytes"] == 1000
    assert stage_samples["combine"]["critical_rank"] == 0
    assert stage_samples["combine"]["algorithm_bytes"] == 200
    assert stage_samples["reduce_grad"]["critical_rank"] == 1
    assert stage_samples["planning"]["algorithm_bytes"] is None
    assert stage_samples["expert"]["algorithm_bandwidth_GBps"] is None
    assert summary["stage_performance"]["dispatch"]["timings_us"]["p95"] == 12.0
    assert (case_dir / "stage_summary.csv").is_file()

    table = format_stage_performance(summary)
    assert table.index("MoonEP benchmark inputs") < table.index(
        "MoonEP six-stage performance"
    )
    assert "S=4  K=2  E=8  H=16  Hf=32  B=2  P=4" in table
    assert "warmup=2  iterations=1  wait_iterations=1000000" in table
    assert "visible_devices=0,1" in table
    assert "udma_qp_route_spec=port_count:6,port_count:2" in table
    assert "source_git_sha=0123456789abcdef" in table
    assert "source_dirty=false" in table
    assert "Native" in table
    assert "Kernel/API version" in table
    assert "tilexr_ep_plan_kernel (PlannerV3)" in table
    assert "tilexr_moonep_dispatch_urma_kernel (DispatchV2)" in table
    assert "tilexr_moonep_combine_v2_kernel (CombineV2)" in table
    assert "torch_npu 2.10.0.post2 (GMM+SwiGLU)" in table
    stage_csv_header = (case_dir / "stage_summary.csv").read_text(
        encoding="utf-8"
    ).splitlines()[0]
    assert stage_csv_header.startswith("stage,native,kernel_version,")
    assert [line.split()[0] for line in table.splitlines()[-6:]] == [
        "Planning",
        "Dispatch",
        "PrefetchWeight",
        "Expert",
        "Combine",
        "ReduceGrad",
    ]
    assert "N/A" in table

    tiny_bandwidth = dict(summary)
    tiny_bandwidth["stage_performance"] = {
        name: dict(values) for name, values in summary["stage_performance"].items()
    }
    tiny_bandwidth["stage_performance"]["dispatch"] = dict(
        tiny_bandwidth["stage_performance"]["dispatch"]
    )
    tiny_bandwidth["stage_performance"]["dispatch"][
        "algorithm_bandwidth_GBps"
    ] = {"mean": 0.000024226}
    assert "0.000024" in format_stage_performance(tiny_bandwidth)

    assert report_main(["--summary", str(case_dir / "summary.json")]) == 0

    rank_one_result = case_dir / "rank_1" / "result.json"
    mixed = json.loads(rank_one_result.read_text(encoding="utf-8"))
    mixed["stage_execution"]["dispatch"]["kernel_version"] = "DispatchV0"
    write_json(rank_one_result, mixed)
    with pytest.raises(ValueError, match="stage execution metadata differs"):
        aggregate_rank_artifacts(case_dir, world_size=2)


def test_model_flow_report_preserves_operator_order_and_rank_distributions(
    tmp_path: Path, capsys
) -> None:
    case_dir = tmp_path / "model-flow"
    order = model_operator_order()
    for rank in range(2):
        result = _result(rank, 2, 1)
        result["benchmark_kind"] = "model_flow"
        result["case"].update(
            {
                "case_id": "model-flow-8rank-4k-ep8-mindspeed",
                "warmup": 5,
                "iterations": 2,
                "routing_pattern": "model_replay",
            }
        )
        result["profile_timing_source"] = "torch_npu_profiler_kernel_details"
        result["route_provenance"] = {"run_id": "model-profile"}
        result["model_operator_order"] = order
        rank_dir = case_dir / f"rank_{rank}"
        write_json(rank_dir / "result.json", result)
        operators = []
        for item in order:
            sequence = int(item["sequence"])
            data_moving = item["stage"] != "planning"
            operators.append(
                {
                    **item,
                    "kernel_name": f"kernel_{item['stage']}",
                    "kernel_launches_per_call": (
                        2 if item["stage"] == "combine_backward" else 1
                    ),
                    "values_us": [
                        float(sequence + rank + 1),
                        float(sequence + rank + 3),
                    ],
                    "algorithm_bytes": [1000, 1000] if data_moving else [None, None],
                }
            )
        write_json(
            rank_dir / "model_profile.json",
            {
                "schema_version": 1,
                "timing_source": "torch_npu_profiler_kernel_details",
                "iterations": 2,
                "operators": operators,
            },
        )

    summary = aggregate_rank_artifacts(case_dir, world_size=2)

    assert summary["benchmark_kind"] == "model_flow"
    assert len(summary["model_operator_performance"]) == 55
    first = summary["model_operator_performance"][0]
    assert first["latency_us"] == {
        "median": 2.5,
        "max": 3.0,
        "min": 2.0,
    }
    assert summary["model_stage_performance"]["planning"]["calls_per_rank"] == 20
    assert summary["model_stage_performance"]["dispatch_forward"][
        "calls_per_rank"
    ] == 20
    assert (case_dir / "model_operator_summary.csv").is_file()
    assert (case_dir / "model_stage_summary.csv").is_file()

    table = format_model_flow_performance(summary)
    assert table.index("MoonEP model operator performance") < table.index(
        "MoonEP model stage summary"
    )
    assert "initial_forward" in table
    assert "recompute_backward" in table
    assert "median/max/min across rank means" in table
    assert "kernel_combine_backward" in table
    summary_path = case_dir / "summary.json"
    assert report_main(["--summary", str(summary_path)]) == 0
    assert "MoonEP model operator performance" in capsys.readouterr().out


def test_two_node_model_flow_artifacts_aggregate_all_global_ranks(tmp_path: Path) -> None:
    output_dir = tmp_path / "run"
    case_id = "model-flow-16rank-4k-ep16-mindspeed"
    case_dir = output_dir / case_id
    order = model_operator_order()
    for node_rank in range(2):
        write_json(
            output_dir / f"node_{node_rank}_complete.json",
            {
                "schema_version": 1,
                "status": "passed",
                "mode": "benchmark",
                "launch_id": "model-flow-launch",
                "case_ids": [case_id],
                "topology": {
                    "node_count": 2,
                    "node_rank": node_rank,
                    "world_size": 4,
                    "first_global_rank": node_rank * 2,
                    "local_world_size": 2,
                },
            },
        )
    for rank in range(4):
        result = _result(rank, 4, 2)
        result["launch_id"] = "model-flow-launch"
        result["benchmark_kind"] = "model_flow"
        result["case"].update(
            {
                "case_id": case_id,
                "warmup": 5,
                "iterations": 1,
                "routing_pattern": "model_replay",
            }
        )
        result["profile_timing_source"] = "torch_npu_profiler_kernel_details"
        result["route_provenance"] = {"run_id": "two-node-model"}
        result["model_operator_order"] = order
        rank_dir = case_dir / f"rank_{rank}"
        write_json(rank_dir / "result.json", result)
        write_json(
            rank_dir / "model_profile.json",
            {
                "schema_version": 1,
                "timing_source": "torch_npu_profiler_kernel_details",
                "iterations": 1,
                "operators": [
                    {
                        **item,
                        "kernel_name": f"kernel_{item['stage']}",
                        "kernel_launches_per_call": 1,
                        "values_us": [float(item["sequence"] + rank + 1)],
                        "algorithm_bytes": (
                            [None] if item["stage"] == "planning" else [2048]
                        ),
                    }
                    for item in order
                ],
            },
        )

    summaries = aggregate_distributed_artifacts(
        output_dir,
        case_ids=(case_id,),
        node_count=2,
        world_size=4,
        mode="benchmark",
    )

    summary = summaries[case_id]
    assert summary["benchmark_kind"] == "model_flow"
    assert summary["logical_world_size"] == 4
    dispatch = summary["model_stage_performance"]["dispatch_forward"]
    assert dispatch["latency_us"]["max"] > dispatch["latency_us"]["min"]
    assert dispatch["algorithm_bandwidth_GBps"]["median"] > 0


def test_flow_report_means_use_all_twenty_measured_iterations(tmp_path: Path) -> None:
    case_dir = tmp_path / "flow"
    for rank in range(2):
        samples = []
        for iteration in range(20):
            duration = float(iteration + rank + 1)
            samples.append(
                _sample(
                    iteration,
                    planning=duration,
                    dispatch=(duration, duration),
                    prefetch=duration,
                    expert=(duration, duration),
                    combine=(duration, duration),
                    reduce_grad=duration,
                    algorithm_bytes=(100 + rank, 200 + rank, 300 + rank, 400 + rank),
                )
            )
        _write_rank(
            case_dir,
            rank,
            samples,
            case_overrides={"warmup": 5, "iterations": 20},
        )

    summary = aggregate_rank_artifacts(case_dir, world_size=2)

    assert len(summary["cross_rank_stage_samples"]) == 20
    assert summary["stage_performance"]["planning"]["timings_us"]["mean"] == 11.5
    assert summary["stage_performance"]["dispatch"]["timings_us"]["mean"] == 23.0


def test_warmup_only_report_uses_na_performance_values(tmp_path: Path) -> None:
    case_dir = tmp_path / "flow"
    for rank in range(2):
        _write_rank(
            case_dir,
            rank,
            [],
            case_overrides={"warmup": 5, "iterations": 0},
        )

    summary = aggregate_rank_artifacts(case_dir, world_size=2)

    assert summary["performance_scope"] == "warmup_only"
    assert summary["cross_rank_max_samples"] == []
    assert summary["cross_rank_stage_samples"] == []
    assert summary["metrics_us"] == {}
    assert summary["tokens_per_second"] is None
    for stage in FLOW_STAGE_ORDER:
        assert summary["stage_performance"][stage] == {
            "timings_us": None,
            "algorithm_bytes": None,
            "algorithm_bandwidth_GBps": None,
        }

    table = format_stage_performance(summary)
    assert "warmup=5  iterations=0" in table
    assert all(line.count("N/A") == 5 for line in table.splitlines()[-6:])
    stage_csv = (case_dir / "stage_summary.csv").read_text(encoding="utf-8").splitlines()
    assert len(stage_csv) == 7


def test_flow_report_rejects_empty_samples_for_measured_case(tmp_path: Path) -> None:
    case_dir = tmp_path / "flow"
    for rank in range(2):
        _write_rank(case_dir, rank, [])

    with pytest.raises(
        ValueError, match="rank samples are empty but case iterations is not zero"
    ):
        aggregate_rank_artifacts(case_dir, world_size=2)


def test_zero_duration_reports_unavailable_bandwidth(tmp_path: Path) -> None:
    case_dir = tmp_path / "flow"
    _write_rank(
        case_dir,
        0,
        [
            _sample(
                0,
                planning=0,
                dispatch=(0, 0),
                prefetch=0,
                expert=(0, 0),
                combine=(0, 0),
                reduce_grad=0,
                algorithm_bytes=(64, 64, 64, 64),
            )
        ],
        world_size=1,
    )

    summary = aggregate_rank_artifacts(case_dir, world_size=1)

    assert summary["cross_rank_stage_samples"][0]["stages"]["dispatch"][
        "algorithm_bandwidth_GBps"
    ] is None
    assert summary["stage_performance"]["dispatch"][
        "algorithm_bandwidth_GBps"
    ] is None


def test_legacy_flow_samples_keep_raw_summary_compatibility(tmp_path: Path) -> None:
    case_dir = tmp_path / "flow"
    legacy_timings = {
        name: value for name, value in RAW_TIMINGS.items() if name != "planning"
    }
    for rank in range(2):
        _write_rank(
            case_dir,
            rank,
            [{"iteration": 0, "timings_us": legacy_timings}],
        )

    summary = aggregate_rank_artifacts(case_dir, world_size=2)

    assert "stage_performance" not in summary
    assert "cross_rank_stage_samples" not in summary
    assert summary["metrics_us"]["end_to_end"]["mean"] == 10.0


def test_merged_two_node_artifacts_are_aggregated_globally(tmp_path: Path) -> None:
    output_dir = tmp_path / "run"
    case_dir = output_dir / "flow"
    for node_rank in range(2):
        write_json(
            output_dir / f"node_{node_rank}_complete.json",
            {
                "schema_version": 1,
                "status": "passed",
                "mode": "benchmark",
                "launch_id": "mock-launch",
                "case_ids": ["flow"],
                "topology": {
                    "node_count": 2,
                    "node_rank": node_rank,
                    "world_size": 4,
                    "first_global_rank": node_rank * 2,
                    "local_world_size": 2,
                },
            },
        )
    for rank in range(4):
        _write_rank(
            case_dir,
            rank,
            [
                _sample(
                    0,
                    planning=rank + 1,
                    dispatch=(rank + 1, rank + 1),
                    prefetch=rank + 1,
                    expert=(rank + 1, rank + 1),
                    combine=(rank + 1, rank + 1),
                    reduce_grad=rank + 1,
                    algorithm_bytes=(100 + rank, 200 + rank, 300 + rank, 400 + rank),
                )
            ],
            world_size=4,
            node_count=2,
        )

    summaries = aggregate_distributed_artifacts(
        output_dir,
        case_ids=("flow",),
        node_count=2,
        world_size=4,
        mode="benchmark",
    )

    assert summaries["flow"]["logical_world_size"] == 4
    dispatch = summaries["flow"]["cross_rank_stage_samples"][0]["stages"][
        "dispatch"
    ]
    assert dispatch["critical_rank"] == 3
    assert dispatch["duration_us"] == 8.0
    assert dispatch["algorithm_bytes"] == 103

    stale_result_path = case_dir / "rank_3" / "result.json"
    stale_result = json.loads(stale_result_path.read_text(encoding="utf-8"))
    stale_result["launch_id"] = "stale-launch"
    write_json(stale_result_path, stale_result)
    with pytest.raises(ValueError, match="rank 3 result has invalid launch id"):
        aggregate_distributed_artifacts(
            output_dir,
            case_ids=("flow",),
            node_count=2,
            world_size=4,
            mode="benchmark",
        )


def test_merged_artifacts_reject_mixed_launches(tmp_path: Path) -> None:
    output_dir = tmp_path / "run"
    for node_rank in range(2):
        write_json(
            output_dir / f"node_{node_rank}_complete.json",
            {
                "schema_version": 1,
                "status": "passed",
                "mode": "benchmark",
                "launch_id": f"launch-{node_rank}",
                "case_ids": ["flow"],
                "topology": {
                    "node_count": 2,
                    "node_rank": node_rank,
                    "world_size": 4,
                    "first_global_rank": node_rank * 2,
                    "local_world_size": 2,
                },
            },
        )

    with pytest.raises(ValueError, match="different launch ids"):
        aggregate_distributed_artifacts(
            output_dir,
            case_ids=("flow",),
            node_count=2,
            world_size=4,
            mode="benchmark",
        )
