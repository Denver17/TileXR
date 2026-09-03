from __future__ import annotations

import os
import shutil
import subprocess
from pathlib import Path

import pytest


ROOT = Path(__file__).resolve().parents[3]
CONTROLLER = ROOT / "tools" / "moonep" / "mindspeed" / "run_model.sh"
NODE_RUNNER = ROOT / "tools" / "moonep" / "mindspeed" / "run_model_node.sh"
STAGE_BARRIER = (
    ROOT / "tools" / "moonep" / "mindspeed" / "mindspeed_stage_barrier.py"
)
IDLE_PROBE = ROOT / "tools" / "moonep" / "mindspeed" / "probe_idle.sh"
GIT_BASH = Path(r"C:\Program Files\Git\bin\bash.exe")


def bash_executable() -> str:
    candidate = shutil.which("bash")
    if candidate:
        return candidate
    if GIT_BASH.is_file():
        return str(GIT_BASH)
    pytest.skip("bash is unavailable")


def run_controller(*args: str, stdin: str = "") -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [bash_executable(), str(CONTROLLER), *args],
        cwd=ROOT,
        input=stdin,
        text=True,
        capture_output=True,
        timeout=20,
        check=False,
    )


def answers(nodes: str = "node-a node-b", tilexr_home: str = "/srv/Tile XR") -> str:
    return "\n".join(
        (
            nodes,
            "root",
            "8",
            tilexr_home,
            "/srv/model stack",
            f"{tilexr_home}/install",
            "/home/pkg/b131/cann/set_env.sh",
            "/home/miniconda3/etc/profile.d/conda.sh",
            "ai_moe_test",
            "/srv/model stack/native.env",
            "/home/dataset/deepseek3",
            "/home/dataset/deepseek3/enwiki_text_document",
        )
    ) + "\n"


def test_runner_scripts_expose_the_supported_interface_and_validated_shape() -> None:
    controller = CONTROLLER.read_text(encoding="utf-8")
    node = NODE_RUNNER.read_text(encoding="utf-8")

    for option in (
        "--mode",
        "--backend",
        "--seq-length",
        "--hidden-size",
        "--moe-router-topk",
        "--route-capture-dir",
        "--route-capture-id",
        "--route-capture-skip-calls",
        "--route-capture-calls",
        "--performance-capture-dir",
        "--performance-capture-id",
        "--performance-capture-skip-operators",
        "--performance-capture-operators",
        "--collect-artifacts",
        "--profile",
        "--stage-barrier",
        "--configure",
        "--config",
        "--dry-run",
        "--idle-wait",
        "--hccl-inter-hccs-disable",
        "--rank-table-file",
        "--udma-rootinfo-path",
    ):
        assert option in controller
    for option in (
        "--node-count",
        "--node-rank",
        "--master-addr",
        "--master-port",
        "--devices-per-node",
        "--seq-length",
        "--hidden-size",
        "--moe-router-topk",
        "--route-capture-dir",
        "--route-capture-id",
        "--performance-capture-dir",
        "--performance-capture-id",
    ):
        assert option in node

    for argument in (
        "--num-layers 4",
        '--seq-length "${sequence_length}"',
        '--hidden-size "${hidden_size}"',
        '--moe-router-topk "${router_topk}"',
        "--moonep-token-padding 1",
        "--train-iters 8",
    ):
        assert argument in node
    assert "ep_size=${world_size}" in node
    assert "base_expert_count=32" in node
    assert "expert_count=$((((base_expert_count + ep_size - 1) / ep_size) * ep_size))" in node
    assert '--num-experts "${expert_count}"' in node
    assert "ep_size=${devices_per_node}" not in node
    assert "dispatch_route_count" not in node
    assert '"${node_count}" -gt 1' in node
    assert "ip -o -4 addr show dev data0.3001" in node
    assert "interface=data0.3001" in node
    assert "TILEXR_MOONEP_DISPATCH_PEER_MODE=group_credit" in node
    assert "export TILEXR_MOONEP_DISPATCH_PEER_MODE=legacy" not in node
    assert "TILEXR_MOONEP_DISPATCH_PEER_MODE=legacy is prohibited" in node
    assert "TILEXR_MOONEP_DISPATCH_GROUP_WIDTH=16" in node
    assert "unset TILEXR_MOONEP_DISPATCH_TRANSPORT" in node
    assert (
        "HCCL_NPU_SOCKET_PORT_RANGE=${MODEL_RUNNER_HCCL_NPU_SOCKET_PORT_RANGE:-47000-47100}"
        in node
    )
    assert 'export HCCL_INTER_HCCS_DISABLE=${hccl_inter_hccs_disable}' in node
    assert 'export RANK_TABLE_FILE=${rank_table_file}' in node
    assert 'rank_table_sha256=$(sha256sum "${rank_table_file}"' in node
    assert 'export TILEXR_UDMA_ROOTINFO_PATH=${udma_rootinfo_path}' in node
    assert 'udma_rootinfo_sha256=$(sha256sum "${udma_rootinfo_path}"' in node
    barrier = STAGE_BARRIER.read_text(encoding="utf-8")
    assert "class MindSpeedBarrierMoonEPBuffer(upstream_moonep.Buffer)" in barrier
    assert barrier.count("optional_stage_barrier(torch)") == 2
    assert '"MOONEP_MINDSPEED_STAGE_BARRIER"' in barrier
    assert "distributed.barrier()" in barrier
    assert "create_native_barrier_backend.__mindspeed_capabilities__" in barrier
    assert "MOONEP_MINDSPEED_STAGE_BARRIER=${stage_barrier}" in node
    assert "mindspeed_stage_barrier:create_native_barrier_backend" in node
    assert 'idle_probe=${tilexr_home}/tools/moonep/mindspeed/probe_idle.sh' in node
    assert 'bash "${idle_probe}" --devices "${devices_per_node}"' in node
    idle_probe = IDLE_PROBE.read_text(encoding="utf-8")
    assert "timeout 15s npu-smi info" in idle_probe
    assert "checking live ownership" in idle_probe
    assert "timeout 5s npu-smi info -l" in idle_probe
    assert "fuser /dev/davinci[0-9]*" in idle_probe
    for process_name in (
        "pretrain_gpt.py",
        "torch.distributed.launch",
        "hccl_test/bin/",
        "alltoallv_test",
        "tilexr_udma_dem",
    ):
        assert process_name in idle_probe
    assert "accelerator_processes" in idle_probe
    assert "ps -eo pid=,comm=,args=" in idle_probe
    assert 'readlink -f "/proc/${pid}/exe"' in idle_probe
    assert "pgrep -fc" not in idle_probe
    assert "remote_idle_probe" in controller
    assert '"${profile_done}" -ne "${devices_per_node}"' in node
    assert 'set +u\n# Vendor and conda environment scripts' in node
    assert 'source "${native_env}"\nset -u' in node
    assert "unset TILEXR_MINDSPEED_ROUTE_CAPTURE_DIR" in node
    assert "export TILEXR_MINDSPEED_ROUTE_CAPTURE_DIR=${route_capture_dir}" in node
    assert "export TILEXR_MOONEP_PERF_CAPTURE_DIR=${performance_capture_dir}" in node
    assert "TILEXR_MINDSPEED_PREWARM_FRAMEWORK_OPS" in controller


def test_scaled_model_shape_is_forwarded_to_every_node(tmp_path: Path) -> None:
    config = tmp_path / "runner.env"
    run = run_controller(
        "--mode",
        "multi",
        "--backend",
        "tilexr",
        "--seq-length",
        "8192",
        "--hidden-size",
        "3584",
        "--moe-router-topk",
        "16",
        "--config",
        str(config),
        "--dry-run",
        stdin=answers(),
    )

    assert run.returncode == 0, run.stderr
    assert run.stdout.count("--seq-length 8192") == 2
    assert run.stdout.count("--hidden-size 3584") == 2
    assert run.stdout.count("--moe-router-topk 16") == 2


def test_route_capture_contract_is_forwarded_to_every_node(tmp_path: Path) -> None:
    config = tmp_path / "runner.env"
    run = run_controller(
        "--mode",
        "multi",
        "--backend",
        "tilexr",
        "--route-capture-dir",
        "/srv/capture dir",
        "--route-capture-id",
        "capture-1",
        "--route-capture-skip-calls",
        "60",
        "--route-capture-calls",
        "10",
        "--config",
        str(config),
        "--dry-run",
        stdin=answers(),
    )

    assert run.returncode == 0, run.stderr
    assert run.stdout.count("--route-capture-dir /srv/capture\\ dir") == 2
    assert run.stdout.count("--route-capture-id capture-1") == 2
    assert run.stdout.count("--route-capture-skip-calls 60") == 2
    assert run.stdout.count("--route-capture-calls 10") == 2


def test_lightweight_performance_capture_is_forwarded_to_every_node(
    tmp_path: Path,
) -> None:
    config = tmp_path / "runner.env"
    run = run_controller(
        "--mode",
        "multi",
        "--backend",
        "tilexr",
        "--performance-capture-dir",
        "/srv/performance dir",
        "--performance-capture-id",
        "performance-1",
        "--performance-capture-skip-operators",
        "330",
        "--performance-capture-operators",
        "55",
        "--config",
        str(config),
        "--dry-run",
        stdin=answers(),
    )

    assert run.returncode == 0, run.stderr
    assert run.stdout.count("--performance-capture-dir /srv/performance\\ dir") == 2
    assert run.stdout.count("--performance-capture-id performance-1") == 2
    assert run.stdout.count("--performance-capture-skip-operators 330") == 2
    assert run.stdout.count("--performance-capture-operators 55") == 2


def test_stage_barrier_is_supported_for_native_and_tilexr_dry_runs(tmp_path: Path) -> None:
    config = tmp_path / "runner.env"
    configured = run_controller(
        "--mode", "multi", "--backend", "native", "--stage-barrier",
        "--config", str(config), "--dry-run", stdin=answers()
    )
    assert configured.returncode == 0, configured.stderr
    assert configured.stdout.count("--stage-barrier") == 2

    tilexr = run_controller(
        "--mode", "multi", "--backend", "tilexr", "--stage-barrier",
        "--config", str(config), "--dry-run"
    )
    assert tilexr.returncode == 0, tilexr.stderr
    assert tilexr.stdout.count("--stage-barrier") == 2


def test_inter_hccs_disable_is_validated_and_forwarded(tmp_path: Path) -> None:
    config = tmp_path / "runner.env"
    configured = run_controller(
        "--mode", "multi", "--backend", "native",
        "--hccl-inter-hccs-disable", "true",
        "--config", str(config), "--dry-run", stdin=answers()
    )
    assert configured.returncode == 0, configured.stderr
    assert configured.stdout.count("--hccl-inter-hccs-disable true") == 2

    invalid = run_controller(
        "--mode", "multi", "--backend", "native",
        "--hccl-inter-hccs-disable", "1",
        "--config", str(config), "--dry-run"
    )
    assert invalid.returncode != 0


def test_rank_table_path_is_forwarded_to_every_node(tmp_path: Path) -> None:
    config = tmp_path / "runner.env"
    configured = run_controller(
        "--mode", "multi", "--backend", "native",
        "--rank-table-file", "/srv/rank table.json",
        "--config", str(config), "--dry-run", stdin=answers()
    )
    assert configured.returncode == 0, configured.stderr
    assert configured.stdout.count("--rank-table-file /srv/rank\\ table.json") == 2


def test_udma_rootinfo_path_is_forwarded_to_every_node(tmp_path: Path) -> None:
    config = tmp_path / "runner.env"
    configured = run_controller(
        "--mode", "multi", "--backend", "tilexr",
        "--udma-rootinfo-path", "/etc/hccl rootinfo.json.bak",
        "--config", str(config), "--dry-run", stdin=answers()
    )
    assert configured.returncode == 0, configured.stderr
    assert configured.stdout.count(
        "--udma-rootinfo-path /etc/hccl\\ rootinfo.json.bak"
    ) == 2


def test_first_run_prompts_and_subsequent_dry_run_reuses_cached_answers(tmp_path: Path) -> None:
    config = tmp_path / "runner.env"
    first = run_controller(
        "--mode",
        "multi",
        "--backend",
        "tilexr",
        "--config",
        str(config),
        "--dry-run",
        stdin=answers(),
    )
    assert first.returncode == 0, first.stderr
    assert config.is_file()
    assert "MODEL_RUNNER_NODES=node-a\\ node-b" in config.read_text(encoding="utf-8")
    assert "password" not in config.read_text(encoding="utf-8").lower()
    assert "node_rank=0 host=node-a" in first.stdout
    assert "node_rank=1 host=node-b" in first.stdout
    assert "--node-count 2" in first.stdout
    assert "--node-rank 1" in first.stdout
    assert "/srv/Tile\\ XR/tools/moonep/mindspeed/run_model_node.sh" in first.stdout

    reused = run_controller(
        "--mode",
        "multi",
        "--backend",
        "tilexr",
        "--config",
        str(config),
        "--dry-run",
    )
    assert reused.returncode == 0, reused.stderr
    assert reused.stdout == first.stdout
    assert "Enter" not in reused.stderr


def test_configure_is_the_only_way_to_replace_cached_answers(tmp_path: Path) -> None:
    config = tmp_path / "runner.env"
    created = run_controller(
        "--mode", "multi", "--config", str(config), "--dry-run", stdin=answers()
    )
    assert created.returncode == 0, created.stderr

    ignored_input = run_controller(
        "--mode",
        "multi",
        "--config",
        str(config),
        "--dry-run",
        stdin=answers(nodes="replacement-a replacement-b"),
    )
    assert ignored_input.returncode == 0, ignored_input.stderr
    assert "host=node-a" in ignored_input.stdout
    assert "replacement-a" not in ignored_input.stdout

    updated = run_controller(
        "--mode",
        "multi",
        "--config",
        str(config),
        "--configure",
        "--dry-run",
        stdin=answers(nodes="replacement-a replacement-b"),
    )
    assert updated.returncode == 0, updated.stderr
    assert "host=replacement-a" in updated.stdout
    assert "host=node-a" not in updated.stdout


def test_single_mode_is_local_and_multi_mode_uses_system_ssh(tmp_path: Path) -> None:
    config = tmp_path / "runner.env"
    configured = run_controller(
        "--mode", "single", "--config", str(config), "--dry-run", stdin=answers()
    )
    assert configured.returncode == 0, configured.stderr
    assert "mode=single local=1 node_rank=0" in configured.stdout
    assert "ssh " not in configured.stdout
    assert "--node-count 1" in configured.stdout

    multi = run_controller(
        "--mode", "multi", "--config", str(config), "--dry-run"
    )
    assert multi.returncode == 0, multi.stderr
    assert multi.stdout.count("ssh ") == 2
    assert "root@node-a" in multi.stdout
    assert "root@node-b" in multi.stdout
    assert "--master-addr node-a" in multi.stdout


def test_scripts_do_not_invoke_file_transfer_tools_and_define_failure_cleanup() -> None:
    controller = CONTROLLER.read_text(encoding="utf-8")
    node = NODE_RUNNER.read_text(encoding="utf-8")
    for script in (controller, node):
        assert "scp " not in script
        assert "--delete" not in script
    assert "rsync -a --protect-args" in controller
    assert "collect_model_artifacts" in controller
    assert "cleanup_remote_runs" in controller
    assert "wait_for_stable_idle" in controller
    assert 'probe_pids+=("$!")' in controller
    assert 'wait "${probe_pids[${index}]}"' in controller
    assert 'consecutive=$((consecutive + 1))' in controller
    assert 'wait "${cleanup_pid}" || true' in controller
    assert "trap 'handle_signal" in controller
    assert "remaining=$((remaining - 1))" in controller
    assert "A node reported a non-finite gradient norm" in controller
    assert "No node reported iteration 8/8 with a finite language-model loss" in controller
    assert "status=91" not in node
    assert "nonfinite_grad=$(grep -Eic" in node
    assert "finite_final_loss=$(grep -Ec" in node
    assert '"${node_count}" -eq 1' in node
    assert "status=93" in node
    assert "runner.pid" in node
    assert "kill -- -\"${model_pid}\"" in node


def test_cached_config_is_explicitly_ignored() -> None:
    ignored = subprocess.run(
        ["git", "check-ignore", "-v", "run/moonep/mindspeed/model_runner.env"],
        cwd=ROOT,
        text=True,
        capture_output=True,
        check=False,
    )
    assert ignored.returncode == 0
    assert "run/moonep/mindspeed/model_runner.env" in ignored.stdout
