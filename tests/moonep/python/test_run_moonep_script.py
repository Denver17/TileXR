from __future__ import annotations

import json
import re
from pathlib import Path


ROOT = Path(__file__).resolve().parents[3]
SCRIPT = (ROOT / "scripts" / "run_moonep.sh").read_text(encoding="utf-8")
LAUNCHER = (ROOT / "tools" / "moonep" / "launcher.py").read_text(encoding="utf-8")
DIST_NODE = (ROOT / "tools" / "moonep" / "distributed_node.py").read_text(
    encoding="utf-8"
)
MODEL_NODE = (
    ROOT / "tools" / "moonep" / "mindspeed" / "run_model_node.sh"
).read_text(encoding="utf-8")


def test_script_exposes_tensor_dump_controls() -> None:
    assert "-d|--dump-stage-tensors" in SCRIPT
    assert "--no-dump-stage-tensors" in SCRIPT
    assert "-p|--tensor-preview-elements" in SCRIPT
    assert "TILEXR_MOONEP_TENSOR_PREVIEW_ELEMENTS" in SCRIPT


def test_reference_and_correctness_default_to_tensor_dumps() -> None:
    assert 'if [[ -z "${dump_stage_tensors}" ]]' in SCRIPT
    assert 'if [[ "${mode}" == "benchmark" ]]' in SCRIPT
    assert 'dump_stage_tensors="false"' in SCRIPT
    assert 'dump_stage_tensors="true"' in SCRIPT


def test_script_forwards_dump_arguments_and_reports_artifacts() -> None:
    assert 'launcher_args+=("--dump-stage-tensors")' in SCRIPT
    assert 'launcher_args+=("--tensor-preview-elements" "${tensor_preview_elements}")' in SCRIPT
    assert 'echo "Tensor snapshots: ${tensor_dump_root}"' in SCRIPT
    assert 'echo "Snapshot files: ${snapshot_count} .pt, ${manifest_count} .json"' in SCRIPT


def test_script_exposes_validates_and_forwards_iteration_controls() -> None:
    assert "--warmup COUNT" in SCRIPT
    assert "--iterations COUNT" in SCRIPT
    assert 'warmup=""' in SCRIPT
    assert 'iterations=""' in SCRIPT
    assert 'effective_warmup="${warmup:-5}"' in SCRIPT
    assert 'effective_iterations="${iterations:-20}"' in SCRIPT
    assert "warmup + iterations must be at least 1" in SCRIPT
    assert 'distributed_args+=("--warmup" "${warmup}")' in SCRIPT
    assert 'distributed_args+=("--iterations" "${iterations}")' in SCRIPT
    assert 'launcher_args+=("--warmup" "${warmup}")' in SCRIPT
    assert 'launcher_args+=("--iterations" "${iterations}")' in SCRIPT


def test_script_exposes_opt_in_auto_build_install() -> None:
    assert "--auto-build-install" in SCRIPT
    assert 'auto_build_install="${TILEXR_MOONEP_AUTO_BUILD_INSTALL:-false}"' in SCRIPT
    assert 'auto_build_install="true"' in SCRIPT
    assert "TILEXR_MOONEP_AUTO_BUILD_INSTALL must be boolean-like" in SCRIPT
    assert 'build_dir="${TILEXR_MOONEP_BUILD_DIR:-${TILEXR_HOME}/build-moonep-auto}"' in SCRIPT
    assert 'build_jobs="${TILEXR_MOONEP_BUILD_JOBS:-$(nproc)}"' in SCRIPT
    assert "cmake was not found; cannot auto build/install TileXR" in SCRIPT
    assert "TILEXR_MOONEP_BUILD_DIR must not be the source root" in SCRIPT
    assert 'cmake -S "${TILEXR_HOME}" -B "${build_dir}"' in SCRIPT
    assert '-DTILEXR_BUILD_MOONEP=ON' in SCRIPT
    assert '-DTILEXR_BUILD_TESTS=OFF' in SCRIPT
    assert '-DBUILD_TESTING=OFF' in SCRIPT
    assert 'cmake --install "${build_dir}"' in SCRIPT
    assert "Use --auto-build-install to build and install it automatically." in SCRIPT
    assert SCRIPT.index('timeout_sec="${TILEXR_MOONEP_TIMEOUT_SEC:-600}"') < SCRIPT.index(
        "auto_build_tilexr_install"
    )


def test_script_accepts_visible_devices_and_defaults_from_device_zero() -> None:
    assert "-v|--visible-devices" in SCRIPT
    assert 'visible_device_spec="$2"' in SCRIPT
    assert 'if [[ -n "${visible_device_spec}" ]]' in SCRIPT
    assert 'for ((device = 0; device < physical_device_count; device++))' in SCRIPT
    assert 'export ASCEND_RT_VISIBLE_DEVICES="$(IFS=,; echo "${visible_devices[*]}")"' in SCRIPT
    assert "first_device=" not in SCRIPT


def test_script_owns_and_validates_hccl_npu_port_range() -> None:
    assert "--hccl-npu-socket-port-range" in SCRIPT
    assert 'hccl_npu_socket_port_range="47000-47100"' in SCRIPT
    assert 'hccl_npu_socket_port_range="$2"' in SCRIPT
    assert 'export HCCL_NPU_SOCKET_PORT_RANGE="${hccl_npu_socket_port_range}"' in SCRIPT
    assert 'echo "HCCL NPU socket ports: ${HCCL_NPU_SOCKET_PORT_RANGE}"' in SCRIPT


def test_script_reports_oversubscribed_reference_collective() -> None:
    assert 'if [[ "${mode}" == "correctness" ]] || (( ranks_per_device == 2 )); then' in SCRIPT
    assert 'echo "Reference collective: Gloo (CPU staging)"' in SCRIPT
    assert 'echo "Reference collective: HCCL"' in SCRIPT


def test_script_discovers_the_shared_anaconda_installation() -> None:
    system_miniconda = "/home/miniconda3/etc/profile.d/conda.sh"
    system_anaconda = "/home/anaconda3/etc/profile.d/conda.sh"
    home_miniconda = '"${HOME}/miniconda3/etc/profile.d/conda.sh"'

    assert system_anaconda in SCRIPT
    assert SCRIPT.index(system_miniconda) < SCRIPT.index(system_anaconda)
    assert SCRIPT.index(system_anaconda) < SCRIPT.index(home_miniconda)
    assert '[[ -n "${TILEXR_MOONEP_CONDA_SH:-}" ]]' in SCRIPT
    assert 'source "${TILEXR_MOONEP_CONDA_SH}"' in SCRIPT
    assert "TILEXR_MOONEP_CONDA_SH not found" in SCRIPT
    assert '[[ "${conda_environment}" == /* ]]' in SCRIPT
    assert 'export CONDA_PREFIX=${conda_environment}' in SCRIPT
    assert 'export PATH="${conda_environment}/bin:${PATH}"' in SCRIPT
    assert "Conda environment Python not found" in SCRIPT


def test_script_sources_explicit_cann_home_before_selecting_conda() -> None:
    cann_source = 'source "${cann_setup}"'
    conda_selection = "conda_environment=${TILEXR_MOONEP_CONDA_ENV:-ai_moe_test}"
    assert 'cann_setup="${TILEXR_CANN_HOME%/}/set_env.sh"' in SCRIPT
    assert "TILEXR_CANN_HOME set_env.sh not found" in SCRIPT
    assert cann_source in SCRIPT
    assert SCRIPT.index(cann_source) < SCRIPT.index(conda_selection)


def test_default_output_directory_is_under_project_run_moonep() -> None:
    assert (
        'output_dir="${TILEXR_MOONEP_OUTPUT_DIR:-${TILEXR_HOME}/run/moonep/'
        'tilexr-moonep-${mode}-${rank_size}r-$(date +%Y%m%d-%H%M%S)-$$}"'
        in SCRIPT
    )
    assert "/tmp/tilexr-moonep-${mode}" not in SCRIPT


def test_script_prints_final_plog_path_before_python_launch() -> None:
    log_path_report = SCRIPT.index(
        'echo "ASCEND_PROCESS_LOG_PATH: ${ASCEND_PROCESS_LOG_PATH}"'
    )
    launcher_call = SCRIPT.index("python -m tools.moonep.launcher")
    assert log_path_report < launcher_call


def test_script_disables_tilexr_udma_only_for_single_rank() -> None:
    single_rank_override = '''if [[ "${rank_size}" == "1" ]]; then
    export TILEXR_ENABLE_UDMA=0
fi'''
    assert single_rank_override in SCRIPT
    assert SCRIPT.count("export TILEXR_ENABLE_UDMA=") == 1
    assert SCRIPT.index(single_rank_override) < SCRIPT.index(
        "python -m tools.moonep.launcher"
    )


def test_script_selects_manual_small_through_case_id_only() -> None:
    assert "-c|--case-id" in SCRIPT
    assert 'case_id="$2"' in SCRIPT
    assert 'case_id="planning-no-dedup"' in SCRIPT
    assert "--manual-small" not in SCRIPT
    assert '--case-ids "${case_id}"' in SCRIPT
    assert 'summary_file="${output_dir}/${case_id}/summary.json"' in SCRIPT
    assert 'tensor_dump_root="${output_dir}/${case_id}"' in SCRIPT
    assert 'echo "Readable files: ${readable_count} .txt"' in SCRIPT


def test_usage_lists_every_available_case_id_with_a_description() -> None:
    case_path = ROOT / "tools" / "moonep" / "cases" / "correctness.json"
    raw_cases = json.loads(case_path.read_text(encoding="utf-8"))
    case_ids = [item["id"] for item in raw_cases]
    usage_cases = SCRIPT.split("Available case IDs:", 1)[1].split(
        "\n\nEnvironment:", 1
    )[0]
    for number, case_id in enumerate(case_ids, start=1):
        assert re.search(
            rf"^  {number}\s+{re.escape(case_id)}\s+\S.+$",
            usage_cases,
            re.MULTILINE,
        )


def test_usage_describes_only_fixed_padding_non_duplicate_cases() -> None:
    usage_cases = SCRIPT.split("Available case IDs:", 1)[1].split(
        "\n\nEnvironment:", 1
    )[0]
    descriptions = [line for line in usage_cases.splitlines() if line.strip()]
    assert descriptions
    assert all("P=1" in line for line in descriptions)
    assert "dedup-3" not in usage_cases
    assert "duplicate group" not in usage_cases
    assert "P=4" not in usage_cases


def test_usage_describes_rank_size_and_rank_per_device_for_every_case() -> None:
    usage_cases = SCRIPT.split("Available case IDs:", 1)[1].split(
        "\n\nEnvironment:", 1
    )[0]
    descriptions = [line for line in usage_cases.splitlines() if line.strip()]
    assert len(descriptions) == 20
    assert all("rank_size=" in line for line in descriptions)
    assert all("rank_per_dev=" in line for line in descriptions)


def test_usage_describes_eight_and_sixteen_rank_cases() -> None:
    assert re.search(
        r"^  8\s+planning-8rank-topk-8\s+8-rank .+P=1\)$",
        SCRIPT,
        re.MULTILINE,
    )
    assert re.search(
        r"^  9\s+planning-16rank-topk-16\s+16-rank, 8 NPUs x 2 ranks .+P=1\)$",
        SCRIPT,
        re.MULTILINE,
    )
    assert re.search(
        r"^  10\s+planning-8rank-single-route\s+8-rank single route .+P=1\)$",
        SCRIPT,
        re.MULTILINE,
    )
    assert re.search(
        r"^  11\s+planning-16rank-single-route\s+16-rank, 8 NPUs x 2 ranks single route .+P=1\)$",
        SCRIPT,
        re.MULTILINE,
    )
    assert re.search(
        r"^  12\s+planning-64rank-single-route\s+64-rank, 8 nodes x 8 NPUs single route .+P=1\)$",
        SCRIPT,
        re.MULTILINE,
    )
    assert re.search(
        r"^  13\s+planning-128rank-single-route\s+128-rank, 16 nodes x 8 NPUs single route .+P=1\)$",
        SCRIPT,
        re.MULTILINE,
    )
    assert re.search(
        r"^  14\s+planning-16rank-16card-single-route\s+16-rank, 2 nodes x 8 NPUs single route .+rank_size=16, rank_per_dev=1.+P=1\)$",
        SCRIPT,
        re.MULTILINE,
    )
    assert re.search(
        r"^  15\s+dispatch-8rank-4k-ep8-grouped-urma\s+8-rank Dispatch-only .+rank_size=8, rank_per_dev=1.+S=4096, K=8, E=32, H=7168, Hf=2048, B=4, P=1\)$",
        SCRIPT,
        re.MULTILINE,
    )
    assert re.search(
        r"^  16\s+flow-8rank-4k-ep8-grouped-urma-plan-reuse\s+8-rank full flow with Combine V2 then saved-plan backward Dispatch .+rank_size=8, rank_per_dev=1.+S=4096, K=8, E=32, H=7168, Hf=2048, B=4, P=1\)$",
        SCRIPT,
        re.MULTILINE,
    )
    assert re.search(
        r"^  17\s+model-flow-8rank-4k-ep8-mindspeed\s+8-rank MindSpeed model-iteration replay .+10 forward, 5 backward.+S=4096, K=8, E=32, H=7168, Hf=2048, B=4, P=1\)$",
        SCRIPT,
        re.MULTILINE,
    )


def test_case_15_defaults_to_group_credit_urma_dispatch_hot_loop() -> None:
    assert 'dispatch_repro_case_id="dispatch-8rank-4k-ep8-grouped-urma"' in SCRIPT
    assert 'benchmark_kind="dispatch_hot_loop"' in SCRIPT
    assert 'dispatch_modes=("hidden")' in SCRIPT
    assert "unset TILEXR_MOONEP_DISPATCH_TRANSPORT" in SCRIPT
    assert 'export TILEXR_MOONEP_DISPATCH_PEER_MODE="group_credit"' in SCRIPT
    assert 'export TILEXR_MOONEP_DISPATCH_GROUP_WIDTH="16"' in SCRIPT
    assert '--benchmark-kind "${benchmark_kind}"' in SCRIPT
    assert 'launcher_args+=("--dispatch-modes" "${dispatch_modes[@]}")' in SCRIPT


def test_case_16_defaults_to_group_credit_full_flow_with_plan_reuse() -> None:
    assert (
        'plan_reuse_repro_case_id="flow-8rank-4k-ep8-grouped-urma-plan-reuse"'
        in SCRIPT
    )
    assert 'elif [[ "${case_id}" == "${plan_reuse_repro_case_id}" ]]' in SCRIPT
    assert 'warmup="${warmup:-0}"' in SCRIPT
    assert 'iterations="${iterations:-8}"' in SCRIPT
    assert 'benchmark_kind="flow"' in SCRIPT
    assert "unset TILEXR_MOONEP_DISPATCH_TRANSPORT" in SCRIPT
    assert 'export TILEXR_MOONEP_DISPATCH_PEER_MODE="group_credit"' in SCRIPT
    assert 'export TILEXR_MOONEP_DISPATCH_GROUP_WIDTH="16"' in SCRIPT


def test_case_17_selects_mindspeed_model_flow_without_changing_case_16() -> None:
    assert 'model_flow_case_id="model-flow-8rank-4k-ep8-mindspeed"' in SCRIPT
    assert 'benchmark_kind="model_flow"' in SCRIPT
    assert 'TILEXR_MOONEP_UDMA_ARENA_RESERVE_BYTES' in SCRIPT
    assert '768 * 1024 * 1024' in SCRIPT
    assert (
        'export TILEXR_MOONEP_PLANNER_WAIT_ITERATIONS='
        '"${TILEXR_MOONEP_PLANNER_WAIT_ITERATIONS:-100000000}"'
        in SCRIPT
    )
    case_16 = SCRIPT.split(
        'elif [[ "${case_id}" == "${plan_reuse_repro_case_id}" ]]', 1
    )[1].split("fi", 1)[0]
    assert 'warmup="${warmup:-0}"' in case_16
    assert 'iterations="${iterations:-8}"' in case_16


def test_planner_wait_iterations_reach_local_and_distributed_launchers() -> None:
    assert (
        'planner_wait_iterations='
        '"${TILEXR_MOONEP_PLANNER_WAIT_ITERATIONS:-1000000}"'
        in SCRIPT
    )
    assert (
        "TILEXR_MOONEP_PLANNER_WAIT_ITERATIONS must be a positive integer"
        in SCRIPT
    )
    assert SCRIPT.count('--wait-iterations "${planner_wait_iterations}"') == 2


def test_case_18_through_20_select_rank_matched_model_flows() -> None:
    for number, case_id, rank_size, node_count in (
        (18, "model-flow-16rank-4k-ep16-mindspeed", 16, 2),
        (19, "model-flow-8rank-8k-k16-ep8-mindspeed", 8, 1),
        (20, "model-flow-16rank-8k-k16-ep16-mindspeed", 16, 2),
    ):
        assert re.search(
            rf"^  {number}\s+{re.escape(case_id)}\s+.+rank_size={rank_size}, "
            rf"rank_per_dev=1.+P=1\)$",
            SCRIPT,
            re.MULTILINE,
        )
    assert '--benchmark-kind "${benchmark_kind}"' in SCRIPT
    assert 'choices=("flow", "model_flow", "dispatch_hot_loop")' in (
        ROOT / "tools" / "moonep" / "distributed_node.py"
    ).read_text(encoding="utf-8")
    assert (
        '"${model_flow_scaled_8rank_case_id}"|'
        '"${model_flow_scaled_16rank_case_id}")'
        in SCRIPT
    )
    assert (
        'model_flow_scaled_16rank_case_id="model-flow-16rank-8k-k16-ep16-mindspeed"'
        in SCRIPT
    )
    assert 'export TILEXR_MOONEP_DISPATCH_PEER_MODE="group_credit"' in SCRIPT
    assert 'TILEXR_MOONEP_DIAGNOSTIC_FLOW:-0' in SCRIPT


def test_full_flow_reuses_the_plan_after_forward_combine() -> None:
    benchmark = (ROOT / "tools" / "moonep" / "benchmark.py").read_text(
        encoding="utf-8"
    )
    flow = benchmark.split("def execute_iteration(", 1)[1].split(
        "\ndef _tensor_row_bytes", 1
    )[0]
    combine = flow.index('"combine_forward"')
    backward = flow.index('"dispatch_backward"')
    reuse = flow.index('buffer.dispatch(inputs["grad_output"], plan=plan)')
    assert combine < backward < reuse

    runtime = (
        ROOT
        / "integrations"
        / "moonep_torch"
        / "tilexr_moonep"
        / "runtime.py"
    ).read_text(encoding="utf-8")
    assert "self._combine_v2_lib.TileXRMoonEpCombineStageV2Fused(" in runtime


def test_single_node_launcher_accepts_dispatch_hot_loop_controls() -> None:
    assert '"--benchmark-kind"' in LAUNCHER
    assert 'choices=("flow", "model_flow", "dispatch_hot_loop")' in LAUNCHER
    assert '"--dispatch-modes"' in LAUNCHER


def test_script_exposes_managed_multinode_three_mode_launch() -> None:
    for option in ("--node-count", "--node-rank", "--master-addr", "--master-port"):
        assert option in SCRIPT
    assert 'if (( node_count > 1 )); then' in SCRIPT
    assert 'python -m tools.moonep.distributed_node' in SCRIPT
    assert 'distributed_args+=("--dump-stage-tensors")' in SCRIPT
    assert '--mode "${mode}"' in SCRIPT
    assert '--benchmark-kind "${benchmark_kind}"' in SCRIPT
    assert "multi-node runs require --mode reference" not in SCRIPT
    assert "multi-node runs require exactly one rank per visible NPU" in SCRIPT


def test_script_exposes_model_replay_shape_and_cache_controls() -> None:
    for option in (
        "--model-replay",
        "--model-replay-from",
        "--model-replay-cache-dir",
        "--model-replay-stage-summary-only",
        "--s",
        "--k",
        "--hidden-size",
        "--ep",
    ):
        assert option in SCRIPT
    assert "python -m tools.moonep.model_replay_cli" in SCRIPT
    assert '[[ -t 0 ]]' in SCRIPT
    assert 'model_replay="false"' in SCRIPT
    assert 'model_replay_from="cache"' in SCRIPT
    assert 'case "${model_replay_from}" in' in SCRIPT
    assert 'cache|meta|model)' in SCRIPT
    assert 'model_replay_args+=(--model-replay-from "${model_replay_from}")' in SCRIPT
    assert "model_replay_required_sources=(" in SCRIPT
    assert "TileXR model replay sources are incomplete" in SCRIPT
    assert "tools/moonep/model_replay_cli.py" in SCRIPT
    assert "tools/moonep/model_replay_orchestrator.py" in SCRIPT
    assert "TILEXR_UDMA_ROOTINFO_PATH" in SCRIPT
    assert "tools/moonep/model_replay_meta.py" in SCRIPT
    assert "integrations/moonep_torch/tilexr_moonep/torch_api.py" in SCRIPT
    assert "python -m tools.moonep.model_replay_orchestrator" in SCRIPT
    assert 'source "${model_replay_result_env}"' in SCRIPT
    assert 'case_file="${MODEL_REPLAY_CASE_FILE}"' in SCRIPT
    assert 'case_id="${MODEL_REPLAY_CASE_ID}"' in SCRIPT
    assert "TILEXR_MOONEP_MODEL_ROUTE_REPLAY" in SCRIPT
    assert "TILEXR_MOONEP_MODEL_PERFORMANCE" in SCRIPT
    assert '--node-rank "${node_rank}"' in SCRIPT
    assert "model_replay_collect_multinode" in SCRIPT
    assert "rsync -a --protect-args" in SCRIPT
    assert "--delete" not in SCRIPT
    assert 'if [[ "${node_rank}" -ne 0 ]]; then' in SCRIPT
    assert "python -m tools.moonep.model_replay_compare" in SCRIPT
    assert "compare_args+=(--stage-summary-only)" in SCRIPT
    assert 'model_runner_config_explicit="false"' in SCRIPT
    assert 'model_runner_config_explicit="true"' in SCRIPT
    assert 'TILEXR_MODEL_RUNNER_CONFIG:-' in SCRIPT
    assert 'output_dir}/generated/model_runner.env' in SCRIPT
    assert "TILEXR_MOONEP_MODEL_REPLAY_META_ROOT" in SCRIPT


def test_model_replay_manages_multinode_followers_and_shared_environment() -> None:
    assert 'original_args=("$@")' in SCRIPT
    assert "model_replay_prepare_shared_environment" in SCRIPT
    assert "from tools.moonep.rendezvous import offset_host_port" in SCRIPT
    assert 'TILEXR_MOONEP_LAUNCH_SECRET="$(python -' in SCRIPT
    assert "model_replay_launch_followers" in SCRIPT
    assert 'TILEXR_MOONEP_MODEL_REPLAY_MANAGED_MULTINODE=0' in SCRIPT
    assert '"${original_args[@]}" --node-rank "${index}"' in SCRIPT
    assert "model_replay_wait_followers" in SCRIPT
    assert 'model_replay_follower_logs+=("${log_path}")' in SCRIPT
    assert "TILEXR_CANN_HOME" in SCRIPT
    assert "TILEXR_MOONEP_CONDA_SH" in SCRIPT
    assert "TILEXR_MINDSPEED_PREWARM_FRAMEWORK_OPS" in SCRIPT
    assert "TILEXR_MODEL_REPLAY_TILEXR_GIT_SHA" in SCRIPT
    assert "TILEXR_MODEL_REPLAY_RANK_TABLE_FILE" in SCRIPT
    assert "MODEL_REPLAY_CACHE_GENERATION" in SCRIPT
    assert "TILEXR_MOONEP_MODEL_REPLAY_GENERATION" in SCRIPT


def test_multinode_model_replay_defaults_model_and_replay_to_group_credit() -> None:
    preparation = SCRIPT.index("python -m tools.moonep.model_replay_orchestrator")
    default_mode = SCRIPT.index(
        'export TILEXR_MOONEP_DISPATCH_PEER_MODE="${TILEXR_MOONEP_DISPATCH_PEER_MODE:-group_credit}"'
    )
    assert default_mode < preparation
    assert (
        'export TILEXR_MOONEP_PLANNER_WAIT_ITERATIONS="${TILEXR_MOONEP_PLANNER_WAIT_ITERATIONS:-100000000}"'
        in SCRIPT
    )
    assert '{ (( node_count > 1 )) || (( model_s * model_k > 32768 )); }' in SCRIPT
    assert 'TILEXR_MOONEP_DISPATCH_PEER_MODE=legacy is prohibited' in SCRIPT


def test_single_node_native_launcher_defaults_to_two_udma_qps() -> None:
    assert 'if args.mode in ("benchmark", "correctness"):' in LAUNCHER
    assert '"TILEXR_UDMA_QP_ROUTE_SPEC", "port_count:6,port_count:2"' in LAUNCHER
    assert 'base_env.setdefault("TILEXR_ENABLE_CREDIT_IPC", "1")' in LAUNCHER
    assert "base_env.setdefault(" in LAUNCHER


def test_moonep_cascade_and_model_default_to_credit_ipc() -> None:
    assert 'export TILEXR_ENABLE_CREDIT_IPC="${TILEXR_ENABLE_CREDIT_IPC:-1}"' in SCRIPT
    assert 'base_env.setdefault("TILEXR_ENABLE_CREDIT_IPC", "1")' in DIST_NODE
    assert "export TILEXR_ENABLE_CREDIT_IPC=${TILEXR_ENABLE_CREDIT_IPC:-1}" in MODEL_NODE


def test_script_exposes_merged_multinode_global_aggregation() -> None:
    assert "--aggregate-only" in SCRIPT
    assert "python -m tools.moonep.report" in SCRIPT
    assert '--node-count "${node_count}"' in SCRIPT
    assert '--world-size "${rank_size}"' in SCRIPT


def test_six_stage_performance_table_is_the_final_terminal_section() -> None:
    table_call = 'python -m tools.moonep.report --summary "${summary_file}"'
    assert table_call in SCRIPT
    assert "python -m json.tool" not in SCRIPT
    assert SCRIPT.rindex(table_call) > SCRIPT.index('echo "Rank 0 preview: ${preview_file}"')


def test_script_resolves_numeric_case_before_constructing_artifact_paths() -> None:
    numeric_check = SCRIPT.index('if [[ "${case_id}" =~ ^[0-9]+$ ]]')
    resolver = SCRIPT.index("select_cases(load_cases(sys.argv[1]), sys.argv[2])")
    canonical_assignment = SCRIPT.index('case_id="${resolved_case_id}"')
    summary_path = SCRIPT.index('summary_file="${output_dir}/${case_id}/summary.json"')
    assert numeric_check < resolver < canonical_assignment < summary_path


def test_script_exposes_opt_in_flowchart_export() -> None:
    assert "--generate-flowcharts" in SCRIPT
    assert 'generate_flowcharts="false"' in SCRIPT
    assert 'generate_flowcharts="true"' in SCRIPT
    assert 'flowchart_dir="${output_dir}/${case_id}/flowcharts"' in SCRIPT


def test_flowcharts_require_reference_data_and_skip_benchmark_timing() -> None:
    assert 'if [[ "${mode}" == "benchmark" && "${generate_flowcharts}" == "true" ]]' in SCRIPT
    assert "--generate-flowcharts cannot be used in benchmark mode" in SCRIPT
    assert 'if [[ "${generate_flowcharts}" == "true" && "${dump_stage_tensors}" != "true" ]]' in SCRIPT
    assert "--generate-flowcharts requires stage tensor snapshots" in SCRIPT


def test_script_checks_mermaid_before_launch_and_renders_all_formats() -> None:
    dependency_check = SCRIPT.index('command -v mmdc')
    launcher_call = SCRIPT.index('python -m tools.moonep.launcher')
    conda_activation = SCRIPT.index('conda activate')
    assert conda_activation < dependency_check
    assert dependency_check < launcher_call
    assert 'mmdc_bin="$(command -v mmdc)"' in SCRIPT
    assert '"${mmdc_bin}" --help' in SCRIPT
    assert "--puppeteerConfigFile" in SCRIPT
    assert "--playwright-config-file" in SCRIPT
    assert 'browser_config="${TILEXR_HOME}/tools/moonep/mermaid-browser-config.json"' in SCRIPT
    assert 'mermaid_smoke_dir="$(mktemp -d' in SCRIPT
    assert "flowchart LR" in SCRIPT
    smoke_render = SCRIPT.index('mermaid-smoke.svg')
    assert dependency_check < smoke_render < launcher_call
    assert "python -m playwright install chromium" in SCRIPT
    assert SCRIPT.count('"${mmdc_bin}" \\') == 3
    assert 'python -m tools.moonep.flowcharts' in SCRIPT
    assert '"${mmdc_config_flag}" "${browser_config}"' in SCRIPT
    assert 'read -r svg_width svg_height' in SCRIPT
    assert 'root.attrib["viewBox"]' in SCRIPT
    assert "math.ceil(float(view_box[2])) + 16" in SCRIPT
    assert "math.ceil(float(view_box[3])) + 16" in SCRIPT
    assert '--width "${svg_width}"' in SCRIPT
    assert '--height "${svg_height}"' in SCRIPT
    assert '--scale 2' in SCRIPT
    assert 'expected_flowchart_prefixes=(' in SCRIPT
    for prefix in (
        "1_planning",
        "2_dispatch",
        "3_prefetch-weight",
        "4_expert-forward",
        "5_combine",
        "6_reduce-grad",
    ):
        assert prefix in SCRIPT
