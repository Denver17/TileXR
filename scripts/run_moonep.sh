#!/usr/bin/env bash

set -eo pipefail

usage() {
    cat <<EOF
Usage: $0 -m <benchmark|reference|correctness> -r <positive-integer> [options]
       $0 --mode <benchmark|reference|correctness> --rank-size <positive-integer> [options]

Run a TileXR MoonEP case with the requested mode and logical rank count.

Options:
  -m, --mode MODE       benchmark, reference, or correctness
  -r, --rank-size SIZE  Logical rank count; at most two ranks per physical NPU
  -c, --case-id ID      Case number or canonical ID (default: 5 / planning-no-dedup)
  -v, --visible-devices LIST
                         Comma-separated physical NPU IDs (default: start at 0)
      --warmup COUNT     Untimed warmup iterations (default for benchmark: 5)
      --iterations COUNT Measured iterations (default for benchmark: 20)
      --auto-build-install
                         Build and install TileXR when TILEXR_INSTALL_PREFIX is missing
      --model-replay     Capture/cache real model inputs and replay model-order stages
      --model-replay-from SOURCE
                          Start with cache, meta, or model (default: cache); fall through in that order
      --model-replay-cache-dir PATH
                         Runtime cache root (default: run/moonep/model_replay_cache)
      --model-runner-config PATH
                         MindSpeed model runner configuration override
      --model-replay-profile
                         Enable framework profiling for the model comparison (default)
      --no-model-replay-profile
                         Capture/replay without framework profiling
      --model-replay-stage-summary-only
                         Print only the aggregated model-vs-replay stage comparison
      --s COUNT          Model tokens per rank; required by --model-replay
      --k COUNT          Model router TopK; required by --model-replay
      --hidden-size COUNT
                         Model hidden size; required by --model-replay
      --ep COUNT         Model expert parallel size; required by --model-replay
      --node-count COUNT Number of servers participating in a multi-node launch (default: 1)
      --node-rank RANK   Zero-based server rank (default: 0)
      --aggregate-only   Aggregate already merged multi-node rank artifacts; do not launch workers
      --master-addr HOST Torch distributed rendezvous host for multi-node reference/correctness
      --master-port PORT Torch distributed rendezvous port for multi-node reference/correctness
  -d, --dump-stage-tensors
                        Save complete stage inputs/outputs (default for reference/correctness)
      --no-dump-stage-tensors
                        Disable stage tensor snapshots
  -p, --tensor-preview-elements COUNT
                        Values printed per tensor (default: 8)
      --generate-flowcharts
                        Generate six numbered MMD/SVG/PNG flowcharts (default: off)
      --hccl-npu-socket-port-range START-END
                        HCCL device socket ports (default: 47000-47100)
  -h, --help            Show this usage

Available case IDs:
  1  manual-small              1-rank manual tensors (rank_size=1, rank_per_dev=1, S=2, K=1, E=2, H=8, Hf=4, B=2, P=1)
  2  manual-2rank-imbalanced   2-rank uneven load (rank_size=2, rank_per_dev=1, S=3, K=1, E=4, H=8, Hf=4, B=2, P=1)
  3  manual-2rank-topk-2       2-rank unique owners (rank_size=2, rank_per_dev=1, S=3, K=2, E=4, H=8, Hf=4, B=2, P=1)
  4  planning-small            2-rank unique owners (rank_size=2, rank_per_dev=1, S=8, K=2, E=8, H=8, Hf=4, B=4, P=1)
  5  planning-no-dedup         4-rank single route (rank_size=4, rank_per_dev=1, S=8, K=1, E=8, H=8, Hf=4, B=2, P=1)
  6  planning-4rank-topk-4     4-rank unique owners (rank_size=4, rank_per_dev=1, S=4, K=4, E=16, H=8, Hf=4, B=4, P=1)
  7  skewed-no-dup             4-rank hot expert (rank_size=4, rank_per_dev=1, S=7, K=1, E=16, H=8, Hf=4, B=4, P=1)
  8  planning-8rank-topk-8     8-rank unique owners (rank_size=8, rank_per_dev=1, S=4, K=8, E=32, H=8, Hf=4, B=4, P=1)
  9  planning-16rank-topk-16   16-rank, 8 NPUs x 2 ranks unique owners (rank_size=16, rank_per_dev=2, S=2, K=16, E=32, H=8, Hf=4, B=2, P=1)
  10 planning-8rank-single-route 8-rank single route (rank_size=8, rank_per_dev=1, S=8, K=1, E=16, H=8, Hf=4, B=2, P=1)
  11 planning-16rank-single-route 16-rank, 8 NPUs x 2 ranks single route (rank_size=16, rank_per_dev=2, S=8, K=1, E=16, H=8, Hf=4, B=1, P=1)
  12 planning-64rank-single-route 64-rank, 8 nodes x 8 NPUs single route (rank_size=64, rank_per_dev=1, S=8, K=1, E=64, H=8, Hf=4, B=1, P=1)
  13 planning-128rank-single-route 128-rank, 16 nodes x 8 NPUs single route (rank_size=128, rank_per_dev=1, S=8, K=1, E=128, H=8, Hf=4, B=1, P=1)
  14 planning-16rank-16card-single-route 16-rank, 2 nodes x 8 NPUs single route (rank_size=16, rank_per_dev=1, S=8, K=1, E=16, H=8, Hf=4, B=1, P=1)
  15 dispatch-8rank-4k-ep8-grouped-urma 8-rank Dispatch-only grouped-URMA repro (rank_size=8, rank_per_dev=1, S=4096, K=8, E=32, H=7168, Hf=2048, B=4, P=1)
  16 flow-8rank-4k-ep8-grouped-urma-plan-reuse 8-rank full flow with Combine V2 then saved-plan backward Dispatch (rank_size=8, rank_per_dev=1, S=4096, K=8, E=32, H=7168, Hf=2048, B=4, P=1)
  17 model-flow-8rank-4k-ep8-mindspeed 8-rank MindSpeed model-iteration replay (10 forward, 5 backward; rank_size=8, rank_per_dev=1, S=4096, K=8, E=32, H=7168, Hf=2048, B=4, P=1)
  18 model-flow-16rank-4k-ep16-mindspeed 16-rank, 2-node MindSpeed model-iteration replay (10 forward, 5 backward; rank_size=16, rank_per_dev=1, S=4096, K=8, E=32, H=7168, Hf=2048, B=2, P=1)
  19 model-flow-8rank-8k-k16-ep8-mindspeed 8-rank scaled MindSpeed model-iteration replay (10 forward, 5 backward; rank_size=8, rank_per_dev=1, S=8192, K=16, E=32, H=3584, Hf=2048, B=4, P=1)
  20 model-flow-16rank-8k-k16-ep16-mindspeed 16-rank, 2-node scaled MindSpeed model-iteration replay (10 forward, 5 backward; rank_size=16, rank_per_dev=1, S=8192, K=16, E=32, H=3584, Hf=2048, B=2, P=1)

Environment:
  ASCEND_RT_VISIBLE_DEVICES    Legacy fallback when --visible-devices is omitted
  HCCL_NPU_SOCKET_PORT_RANGE   Legacy fallback when the port option is omitted
  TILEXR_INSTALL_PREFIX        TileXR installation prefix
  TILEXR_MOONEP_AUTO_BUILD_INSTALL
                               Set to 1/true/yes/on to enable --auto-build-install
  TILEXR_MOONEP_BUILD_DIR      CMake build directory used by --auto-build-install
  TILEXR_MOONEP_BUILD_JOBS     CMake build parallelism (default: nproc)
  TILEXR_CANN_HOME             Explicit CANN installation root; sources set_env.sh
  TILEXR_MOONEP_CONDA_SH       Explicit Conda shell setup script
  TILEXR_MOONEP_CONDA_ENV      Conda environment name or absolute prefix (default: ai_moe_test)
  TILEXR_MOONEP_OUTPUT_DIR     Result directory (default: timestamped run/moonep directory)
  TILEXR_MOONEP_TIMEOUT_SEC    Launcher timeout in seconds (default: 600)
  TILEXR_MOONEP_LAUNCH_ID      Shared non-secret launch ID (required for multi-node)
  TILEXR_MOONEP_MODEL_REPLAY_META_ROOT
                               Checked-in replay meta root (default: tools/moonep/model_replay_meta)
  TILEXR_MOONEP_TENSOR_PREVIEW_ELEMENTS
                               Default number of values printed per tensor
EOF
}

if [[ $# -eq 0 ]]; then
    usage
    exit 0
fi

original_args=("$@")

mode=""
rank_size=""
case_id="planning-no-dedup"
case_id_explicit="false"
dump_stage_tensors=""
generate_flowcharts="false"
tensor_preview_elements="${TILEXR_MOONEP_TENSOR_PREVIEW_ELEMENTS:-8}"
visible_device_spec=""
warmup=""
iterations=""
auto_build_install="${TILEXR_MOONEP_AUTO_BUILD_INSTALL:-false}"
node_count=1
node_rank=0
aggregate_only="false"
master_addr=""
master_port=""
model_replay="false"
model_replay_from="cache"
model_replay_from_explicit="false"
model_replay_cache_dir=""
model_runner_config=""
model_runner_config_explicit="false"
model_replay_profile="true"
model_replay_stage_summary_only="false"
model_s=""
model_k=""
model_h=""
model_ep=""
hccl_npu_socket_port_range="47000-47100"
if [[ -n "${HCCL_NPU_SOCKET_PORT_RANGE:-}" ]]; then
    hccl_npu_socket_port_range="${HCCL_NPU_SOCKET_PORT_RANGE}"
fi
while [[ $# -gt 0 ]]; do
    case "$1" in
        -m|--mode)
            if [[ $# -lt 2 ]]; then
                echo "Missing value for --mode" >&2
                usage >&2
                exit 2
            fi
            mode="$2"
            shift 2
            ;;
        -r|--rank-size)
            if [[ $# -lt 2 ]]; then
                echo "Missing value for --rank-size" >&2
                usage >&2
                exit 2
            fi
            rank_size="$2"
            shift 2
            ;;
        -c|--case-id)
            if [[ $# -lt 2 ]]; then
                echo "Missing value for --case-id" >&2
                usage >&2
                exit 2
            fi
            case_id="$2"
            case_id_explicit="true"
            shift 2
            ;;
        -v|--visible-devices)
            if [[ $# -lt 2 ]]; then
                echo "Missing value for --visible-devices" >&2
                usage >&2
                exit 2
            fi
            visible_device_spec="$2"
            shift 2
            ;;
        --warmup)
            if [[ $# -lt 2 ]]; then
                echo "Missing value for --warmup" >&2
                usage >&2
                exit 2
            fi
            warmup="$2"
            shift 2
            ;;
        --iterations)
            if [[ $# -lt 2 ]]; then
                echo "Missing value for --iterations" >&2
                usage >&2
                exit 2
            fi
            iterations="$2"
            shift 2
            ;;
        --auto-build-install)
            auto_build_install="true"
            shift
            ;;
        --model-replay)
            model_replay="true"
            shift
            ;;
        --model-replay-from)
            if [[ $# -lt 2 ]]; then
                echo "Missing value for --model-replay-from" >&2
                exit 2
            fi
            model_replay_from="$2"
            model_replay_from_explicit="true"
            shift 2
            ;;
        --model-replay-cache-dir)
            if [[ $# -lt 2 ]]; then
                echo "Missing value for --model-replay-cache-dir" >&2
                exit 2
            fi
            model_replay_cache_dir="$2"
            shift 2
            ;;
        --model-runner-config)
            if [[ $# -lt 2 ]]; then
                echo "Missing value for --model-runner-config" >&2
                exit 2
            fi
            model_runner_config="$2"
            model_runner_config_explicit="true"
            shift 2
            ;;
        --model-replay-profile)
            model_replay_profile="true"
            shift
            ;;
        --no-model-replay-profile)
            model_replay_profile="false"
            shift
            ;;
        --model-replay-stage-summary-only)
            model_replay_stage_summary_only="true"
            shift
            ;;
        --s)
            if [[ $# -lt 2 ]]; then
                echo "Missing value for --s" >&2
                exit 2
            fi
            model_s="$2"
            shift 2
            ;;
        --k)
            if [[ $# -lt 2 ]]; then
                echo "Missing value for --k" >&2
                exit 2
            fi
            model_k="$2"
            shift 2
            ;;
        --hidden-size)
            if [[ $# -lt 2 ]]; then
                echo "Missing value for --hidden-size" >&2
                exit 2
            fi
            model_h="$2"
            shift 2
            ;;
        --ep)
            if [[ $# -lt 2 ]]; then
                echo "Missing value for --ep" >&2
                exit 2
            fi
            model_ep="$2"
            shift 2
            ;;
        --node-count)
            if [[ $# -lt 2 ]]; then
                echo "Missing value for --node-count" >&2
                exit 2
            fi
            node_count="$2"
            shift 2
            ;;
        --node-rank)
            if [[ $# -lt 2 ]]; then
                echo "Missing value for --node-rank" >&2
                exit 2
            fi
            node_rank="$2"
            shift 2
            ;;
        --aggregate-only)
            aggregate_only="true"
            shift
            ;;
        --master-addr)
            if [[ $# -lt 2 ]]; then
                echo "Missing value for --master-addr" >&2
                exit 2
            fi
            master_addr="$2"
            shift 2
            ;;
        --master-port)
            if [[ $# -lt 2 ]]; then
                echo "Missing value for --master-port" >&2
                exit 2
            fi
            master_port="$2"
            shift 2
            ;;
        -d|--dump-stage-tensors)
            dump_stage_tensors="true"
            shift
            ;;
        --no-dump-stage-tensors)
            dump_stage_tensors="false"
            shift
            ;;
        -p|--tensor-preview-elements)
            if [[ $# -lt 2 ]]; then
                echo "Missing value for --tensor-preview-elements" >&2
                usage >&2
                exit 2
            fi
            tensor_preview_elements="$2"
            shift 2
            ;;
        --generate-flowcharts)
            generate_flowcharts="true"
            shift
            ;;
        --hccl-npu-socket-port-range)
            if [[ $# -lt 2 ]]; then
                echo "Missing value for --hccl-npu-socket-port-range" >&2
                usage >&2
                exit 2
            fi
            hccl_npu_socket_port_range="$2"
            shift 2
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            echo "Unknown argument: $1" >&2
            usage >&2
            exit 2
            ;;
    esac
done

case "${model_replay_from}" in
    cache|meta|model) ;;
    *)
        echo "--model-replay-from must be cache, meta, or model: ${model_replay_from}" >&2
        exit 2
        ;;
esac

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
tilexr_source_root="$(cd -- "${script_dir}/.." && pwd)"

if [[ "${model_replay}" == "true" ]]; then
    model_replay_required_sources=(
        tools/moonep/model_replay_cli.py
        tools/moonep/model_replay_orchestrator.py
        tools/moonep/model_replay_cache.py
        tools/moonep/model_replay_meta.py
        tools/moonep/model_replay_compare.py
        tools/moonep/model_flow.py
        tools/moonep/mindspeed/build_route_replay.py
        tools/moonep/mindspeed/collect_model_performance.py
        tools/moonep/mindspeed/run_model.sh
        tools/moonep/mindspeed/run_model_node.sh
        tools/moonep/mindspeed/tilexr_mindspeed_adapter.py
        integrations/moonep_torch/tilexr_moonep/torch_api.py
    )
    missing_model_replay_sources=()
    for relative_path in "${model_replay_required_sources[@]}"; do
        if [[ ! -f "${tilexr_source_root}/${relative_path}" ]]; then
            missing_model_replay_sources+=("${relative_path}")
        fi
    done
    if (( ${#missing_model_replay_sources[@]} > 0 )); then
        echo "TileXR model replay sources are incomplete in ${tilexr_source_root}:" >&2
        printf '  missing %s\n' "${missing_model_replay_sources[@]}" >&2
        echo "Sync the full model-replay-cache worktree to this deployment before running --model-replay." >&2
        exit 1
    fi
fi

if [[ -z "${mode}" || ( -z "${rank_size}" && "${model_replay}" != "true" ) ]]; then
    echo "Both --mode and --rank-size are required" >&2
    usage >&2
    exit 2
fi

if [[ "${model_replay}" == "true" ]]; then
    if [[ "${mode}" != "benchmark" ]]; then
        echo "--model-replay requires --mode benchmark" >&2
        exit 2
    fi
    if [[ "${case_id_explicit}" == "true" ]]; then
        echo "--model-replay is shape-driven and cannot be combined with --case-id" >&2
        exit 2
    fi
    interactive_args=()
    if [[ -t 0 ]]; then
        interactive_args+=(--interactive)
    fi
    if ! shape_values="$(
        PYTHONPATH="${tilexr_source_root}${PYTHONPATH:+:${PYTHONPATH}}" \
            python -m tools.moonep.model_replay_cli \
                --s "${model_s}" \
                --k "${model_k}" \
                --hidden-size "${model_h}" \
                --ep "${model_ep}" \
                --rank-size "${rank_size}" \
                "${interactive_args[@]}"
    )"; then
        exit 2
    fi
    read -r model_s model_k model_h model_ep rank_size \
        model_e model_hf model_b model_p model_forward_calls <<<"${shape_values}"
    case_id="model-replay-s${model_s}-k${model_k}-h${model_h}-ep${model_ep}-r${rank_size}"
elif [[ "${model_replay_from_explicit}" == "true" ||
        -n "${model_replay_cache_dir}" ||
        -n "${model_s}${model_k}${model_h}${model_ep}" ||
        "${model_replay_stage_summary_only}" == "true" ]]; then
    echo "model replay shape/cache options require --model-replay" >&2
    exit 2
fi

case "${mode}" in
    benchmark|reference|correctness) ;;
    *)
        echo "--mode must be benchmark, reference, or correctness: ${mode}" >&2
        usage >&2
        exit 2
        ;;
esac
if [[ ! "${case_id}" =~ ^[A-Za-z0-9][A-Za-z0-9._-]*$ ]]; then
    echo "--case-id contains invalid characters: ${case_id}" >&2
    exit 2
fi
if [[ -z "${dump_stage_tensors}" ]]; then
    if [[ "${mode}" == "benchmark" ]]; then
        dump_stage_tensors="false"
    else
        dump_stage_tensors="true"
    fi
fi
if [[ "${mode}" == "benchmark" && "${dump_stage_tensors}" == "true" ]]; then
    echo "--dump-stage-tensors cannot be used in benchmark mode" >&2
    exit 2
fi
if [[ "${mode}" == "benchmark" && "${generate_flowcharts}" == "true" ]]; then
    echo "--generate-flowcharts cannot be used in benchmark mode" >&2
    exit 2
fi
if [[ "${generate_flowcharts}" == "true" && "${dump_stage_tensors}" != "true" ]]; then
    echo "--generate-flowcharts requires stage tensor snapshots; remove --no-dump-stage-tensors" >&2
    exit 2
fi
if [[ ! "${rank_size}" =~ ^[1-9][0-9]*$ ]]; then
    echo "--rank-size must be a positive integer: ${rank_size}" >&2
    usage >&2
    exit 2
fi
if [[ ! "${node_count}" =~ ^[1-9][0-9]*$ ]]; then
    echo "--node-count must be a positive integer: ${node_count}" >&2
    exit 2
fi
if [[ ! "${node_rank}" =~ ^[0-9]+$ ]] || (( node_rank >= node_count )); then
    echo "--node-rank must be in [0, node-count): ${node_rank}" >&2
    exit 2
fi
local_rank_size="${rank_size}"
if (( node_count > 1 )); then
    if (( rank_size % node_count != 0 )); then
        echo "--rank-size must be divisible by --node-count" >&2
        exit 2
    fi
    local_rank_size=$((rank_size / node_count))
    if [[ "${aggregate_only}" != "true" && "${mode}" != "benchmark" ]] &&
       { [[ -z "${master_addr}" || ! "${master_port}" =~ ^[1-9][0-9]*$ ]] || (( master_port > 65535 )); }; then
        echo "multi-node runs require --master-addr and a valid --master-port" >&2
        exit 2
    fi
    if [[ "${aggregate_only}" != "true" &&
          "${model_replay}" != "true" &&
          -z "${TILEXR_MOONEP_LAUNCH_ID:-}" ]]; then
        echo "multi-node runs require TILEXR_MOONEP_LAUNCH_ID" >&2
        exit 2
    fi
    if [[ -z "${TILEXR_MOONEP_OUTPUT_DIR:-}" ]]; then
        echo "multi-node runs require TILEXR_MOONEP_OUTPUT_DIR" >&2
        exit 2
    fi
    if [[ "${generate_flowcharts}" == "true" ]]; then
        echo "multi-node runs do not generate flowcharts on node-local artifacts" >&2
        exit 2
    fi
fi
if [[ "${aggregate_only}" == "true" ]]; then
    if (( node_count <= 1 )); then
        echo "--aggregate-only requires --node-count greater than one" >&2
        exit 2
    fi
    if [[ "${mode}" != "benchmark" ]]; then
        echo "--aggregate-only currently reports benchmark performance only" >&2
        exit 2
    fi
fi
if [[ ! "${tensor_preview_elements}" =~ ^[1-9][0-9]*$ ]]; then
    echo "--tensor-preview-elements must be a positive integer: ${tensor_preview_elements}" >&2
    exit 2
fi
if [[ -n "${warmup}" && ! "${warmup}" =~ ^[0-9]+$ ]]; then
    echo "--warmup must be a non-negative integer: ${warmup}" >&2
    exit 2
fi
if [[ -n "${iterations}" && ! "${iterations}" =~ ^[0-9]+$ ]]; then
    echo "--iterations must be a non-negative integer: ${iterations}" >&2
    exit 2
fi
effective_warmup="${warmup:-5}"
effective_iterations="${iterations:-20}"
if (( 10#${effective_warmup} + 10#${effective_iterations} < 1 )); then
    echo "warmup + iterations must be at least 1" >&2
    exit 2
fi
if [[ ! "${hccl_npu_socket_port_range}" =~ ^([0-9]+)-([0-9]+)$ ]]; then
    echo "--hccl-npu-socket-port-range must use START-END: ${hccl_npu_socket_port_range}" >&2
    exit 2
fi
hccl_port_start="${BASH_REMATCH[1]}"
hccl_port_end="${BASH_REMATCH[2]}"
if (( 10#${hccl_port_start} < 1024 || 10#${hccl_port_end} > 65535 || 10#${hccl_port_start} > 10#${hccl_port_end} )); then
    echo "HCCL NPU socket ports must satisfy 1024 <= START <= END <= 65535: ${hccl_npu_socket_port_range}" >&2
    exit 2
fi
case "${auto_build_install}" in
    1|true|TRUE|yes|YES|on|ON)
        auto_build_install="true"
        ;;
    0|false|FALSE|no|NO|off|OFF)
        auto_build_install="false"
        ;;
    *)
        echo "TILEXR_MOONEP_AUTO_BUILD_INSTALL must be boolean-like: ${auto_build_install}" >&2
        exit 2
        ;;
esac

# shellcheck disable=SC1091
source "${script_dir}/common_env.sh"

if [[ "${rank_size}" == "1" ]]; then
    export TILEXR_ENABLE_UDMA=0
fi
export TILEXR_ENABLE_CREDIT_IPC="${TILEXR_ENABLE_CREDIT_IPC:-1}"

if [[ -n "${TILEXR_CANN_HOME:-}" ]]; then
    cann_setup="${TILEXR_CANN_HOME%/}/set_env.sh"
    if [[ ! -f "${cann_setup}" ]]; then
        echo "TILEXR_CANN_HOME set_env.sh not found: ${cann_setup}" >&2
        exit 1
    fi
    # CANN environment scripts may inspect variables that are unset.
    set +u
    source "${cann_setup}"
    set -u
fi

conda_environment=${TILEXR_MOONEP_CONDA_ENV:-ai_moe_test}
if [[ "${conda_environment}" == /* ]]; then
    if [[ ! -x "${conda_environment}/bin/python" ]]; then
        echo "Conda environment Python not found: ${conda_environment}/bin/python" >&2
        exit 1
    fi
    export CONDA_PREFIX=${conda_environment}
    export CONDA_DEFAULT_ENV=${conda_environment}
    export PATH="${conda_environment}/bin:${PATH}"
else
    if [[ -n "${TILEXR_MOONEP_CONDA_SH:-}" ]]; then
        if [[ ! -f "${TILEXR_MOONEP_CONDA_SH}" ]]; then
            echo "TILEXR_MOONEP_CONDA_SH not found: ${TILEXR_MOONEP_CONDA_SH}" >&2
            exit 1
        fi
        # Conda's shell function is required for `conda activate`.
        source "${TILEXR_MOONEP_CONDA_SH}"
    else
        for conda_setup in \
            /home/miniconda3/etc/profile.d/conda.sh \
            /home/anaconda3/etc/profile.d/conda.sh \
            "${HOME}/miniconda3/etc/profile.d/conda.sh" \
            "${HOME}/anaconda3/etc/profile.d/conda.sh"; do
            if [[ -f "${conda_setup}" ]]; then
                source "${conda_setup}"
                break
            fi
        done
    fi
    if ! command -v conda >/dev/null 2>&1; then
        echo "Conda was not found; expected /home/miniconda3 or a conda command on PATH" >&2
        exit 1
    fi
    conda activate "${conda_environment}"
fi
set -u

install_prefix="${TILEXR_INSTALL_PREFIX:-${TILEXR_HOME}/install-moonep-b131-20260805}"
timeout_sec="${TILEXR_MOONEP_TIMEOUT_SEC:-600}"
if [[ ! "${timeout_sec}" =~ ^[1-9][0-9]*$ ]]; then
    echo "TILEXR_MOONEP_TIMEOUT_SEC must be a positive integer: ${timeout_sec}" >&2
    exit 2
fi

auto_build_tilexr_install() {
    local build_dir build_jobs
    build_dir="${TILEXR_MOONEP_BUILD_DIR:-${TILEXR_HOME}/build-moonep-auto}"
    build_jobs="${TILEXR_MOONEP_BUILD_JOBS:-$(nproc)}"
    if [[ ! "${build_jobs}" =~ ^[1-9][0-9]*$ ]]; then
        echo "TILEXR_MOONEP_BUILD_JOBS must be a positive integer: ${build_jobs}" >&2
        exit 2
    fi
    if ! command -v cmake >/dev/null 2>&1; then
        echo "cmake was not found; cannot auto build/install TileXR" >&2
        exit 1
    fi
    if [[ "$(realpath -m "${build_dir}")" == "$(realpath -m "${TILEXR_HOME}")" ]]; then
        echo "TILEXR_MOONEP_BUILD_DIR must not be the source root: ${build_dir}" >&2
        exit 2
    fi
    echo "TileXR installation prefix not found: ${install_prefix}"
    echo "Auto build/install enabled. Build dir: ${build_dir}"
    cmake -S "${TILEXR_HOME}" -B "${build_dir}" \
        -DCMAKE_INSTALL_PREFIX="${install_prefix}" \
        -DTILEXR_BUILD_MOONEP=ON \
        -DTILEXR_BUILD_TESTS=OFF \
        -DBUILD_TESTING=OFF
    cmake --build "${build_dir}" -j"${build_jobs}"
    cmake --install "${build_dir}"
}
if [[ ! -d "${install_prefix}" ]]; then
    if [[ "${auto_build_install}" == "true" ]]; then
        auto_build_tilexr_install
    else
        echo "TileXR installation prefix not found: ${install_prefix}" >&2
        echo "Use --auto-build-install to build and install it automatically." >&2
        exit 1
    fi
fi

output_dir="${TILEXR_MOONEP_OUTPUT_DIR:-${TILEXR_HOME}/run/moonep/tilexr-moonep-${mode}-${rank_size}r-$(date +%Y%m%d-%H%M%S)-$$}"
case_file="${TILEXR_HOME}/tools/moonep/cases/correctness.json"
if [[ "${model_replay}" == "true" ]]; then
    if (( node_count > 1 )); then
        export TILEXR_MOONEP_DISPATCH_PEER_MODE="${TILEXR_MOONEP_DISPATCH_PEER_MODE:-group_credit}"
        export TILEXR_MOONEP_PLANNER_WAIT_ITERATIONS="${TILEXR_MOONEP_PLANNER_WAIT_ITERATIONS:-100000000}"
    fi
    model_replay_cache_dir="${model_replay_cache_dir:-${TILEXR_HOME}/run/moonep/model_replay_cache}"
    model_replay_result_env="${output_dir}/model_replay_result.env"
    model_replay_case_file="${output_dir}/model_replay_case.json"
    if [[ "${model_runner_config_explicit}" != "true" ]]; then
        if [[ -n "${TILEXR_MODEL_RUNNER_CONFIG:-}" ]]; then
            model_runner_config="${TILEXR_MODEL_RUNNER_CONFIG}"
        else
            model_runner_config="${output_dir}/generated/model_runner.env"
        fi
    fi
    model_replay_args=(
        --source-root "${TILEXR_HOME}"
        --cache-root "${model_replay_cache_dir}"
        --case-file "${model_replay_case_file}"
        --result-env-file "${model_replay_result_env}"
        --s "${model_s}"
        --k "${model_k}"
        --hidden-size "${model_h}"
        --ep "${model_ep}"
        --rank-size "${rank_size}"
        --node-count "${node_count}"
        --node-rank "${node_rank}"
        --warmup "${effective_warmup}"
        --iterations "${effective_iterations}"
    )
    model_replay_args+=(--model-runner-config "${model_runner_config}")
    model_replay_args+=(--model-replay-from "${model_replay_from}")
    if [[ "${model_replay_profile}" == "true" ]]; then
        model_replay_args+=(--profile)
    else
        model_replay_args+=(--no-profile)
    fi
    if [[ -n "${TILEXR_MOONEP_MODEL_REPLAY_GENERATION:-}" ]]; then
        model_replay_args+=(
            --cache-generation "${TILEXR_MOONEP_MODEL_REPLAY_GENERATION}"
        )
    fi
    python -m tools.moonep.model_replay_orchestrator "${model_replay_args[@]}"
    if [[ ! -f "${model_replay_result_env}" ]]; then
        echo "Model replay result environment not found: ${model_replay_result_env}" >&2
        exit 1
    fi
    # shellcheck disable=SC1090
    source "${model_replay_result_env}"
    export TILEXR_MOONEP_MODEL_REPLAY_GENERATION="${MODEL_REPLAY_CACHE_GENERATION}"
    case_file="${MODEL_REPLAY_CASE_FILE}"
    case_id="${MODEL_REPLAY_CASE_ID}"
    export TILEXR_MOONEP_MODEL_ROUTE_REPLAY="${MODEL_REPLAY_ROUTE_REPLAY}"
    export TILEXR_MOONEP_MODEL_PERFORMANCE="${MODEL_REPLAY_MODEL_PERFORMANCE}"
    echo "Model replay cache: ${MODEL_REPLAY_CACHE_STATUS} (${MODEL_REPLAY_CACHE_KEY})"
fi
if [[ ! -f "${case_file}" ]]; then
    echo "Required file not found: ${case_file}" >&2
    exit 1
fi
if [[ "${case_id}" =~ ^[0-9]+$ ]]; then
    if ! resolved_case_id="$(
        PYTHONPATH="${TILEXR_HOME}${PYTHONPATH:+:${PYTHONPATH}}" \
            python - "${case_file}" "${case_id}" <<'PY'
import sys

from tools.moonep.config import load_cases, select_cases

try:
    selected = select_cases(load_cases(sys.argv[1]), sys.argv[2])
except ValueError as error:
    print(error, file=sys.stderr)
    raise SystemExit(2)
print(selected[0].case_id)
PY
    )"; then
        exit 2
    fi
    case_id="${resolved_case_id}"
fi

benchmark_kind="flow"
dispatch_modes=()
dispatch_repro_case_id="dispatch-8rank-4k-ep8-grouped-urma"
plan_reuse_repro_case_id="flow-8rank-4k-ep8-grouped-urma-plan-reuse"
model_flow_case_id="model-flow-8rank-4k-ep8-mindspeed"
model_flow_16rank_case_id="model-flow-16rank-4k-ep16-mindspeed"
model_flow_scaled_8rank_case_id="model-flow-8rank-8k-k16-ep8-mindspeed"
model_flow_scaled_16rank_case_id="model-flow-16rank-8k-k16-ep16-mindspeed"
is_model_flow="false"
case "${case_id}" in
    "${model_flow_case_id}"|"${model_flow_16rank_case_id}"|\
    "${model_flow_scaled_8rank_case_id}"|"${model_flow_scaled_16rank_case_id}")
        is_model_flow="true"
        ;;
esac
if [[ "${model_replay}" == "true" ]]; then
    is_model_flow="true"
fi
if [[ "${case_id}" == "${dispatch_repro_case_id}" ||
      "${case_id}" == "${plan_reuse_repro_case_id}" ||
      "${is_model_flow}" == "true" ]]; then
    if [[ "${mode}" != "benchmark" ]]; then
        echo "${case_id} requires --mode benchmark" >&2
        exit 2
    fi
    if [[ "${case_id}" == "${dispatch_repro_case_id}" ||
          "${case_id}" == "${plan_reuse_repro_case_id}" ]]; then
        if [[ "${rank_size}" != "8" || "${node_count}" != "1" ]]; then
            echo "${case_id} requires --rank-size 8 on one node" >&2
            exit 2
        fi
    elif [[ "${case_id}" == "${model_flow_case_id}" ||
            "${case_id}" == "${model_flow_scaled_8rank_case_id}" ]]; then
        if [[ "${rank_size}" != "8" || "${node_count}" != "1" ]]; then
            echo "${case_id} requires --rank-size 8 on one node" >&2
            exit 2
        fi
    elif [[ "${model_replay}" == "true" ]]; then
        :
    elif [[ "${rank_size}" != "16" || "${node_count}" != "2" ]]; then
        echo "${case_id} requires --rank-size 16 on two nodes" >&2
        exit 2
    fi
    requested_dispatch_peer_mode="${TILEXR_MOONEP_DISPATCH_PEER_MODE:-}"
    requested_dispatch_group_width="${TILEXR_MOONEP_DISPATCH_GROUP_WIDTH:-}"
    unset TILEXR_MOONEP_DISPATCH_TRANSPORT
    if [[ -n "${requested_dispatch_peer_mode}" ]]; then
        case "${requested_dispatch_peer_mode}" in
            legacy)
                echo "TILEXR_MOONEP_DISPATCH_PEER_MODE=legacy is prohibited" >&2
                exit 2
                ;;
            group|group_credit)
                export TILEXR_MOONEP_DISPATCH_PEER_MODE="${requested_dispatch_peer_mode}"
                export TILEXR_MOONEP_DISPATCH_GROUP_WIDTH="${requested_dispatch_group_width:-16}"
                ;;
            *)
                echo "Invalid TILEXR_MOONEP_DISPATCH_PEER_MODE=${requested_dispatch_peer_mode}" >&2
                exit 2
                ;;
        esac
    elif [[ "${model_replay}" == "true" ]] &&
         { (( node_count > 1 )) || (( model_s * model_k > 32768 )); }; then
        export TILEXR_MOONEP_DISPATCH_PEER_MODE="group_credit"
        export TILEXR_MOONEP_DISPATCH_GROUP_WIDTH="16"
    else
        case "${case_id}" in
            "${model_flow_scaled_8rank_case_id}"|"${model_flow_scaled_16rank_case_id}")
                export TILEXR_MOONEP_DISPATCH_PEER_MODE="group_credit"
                export TILEXR_MOONEP_DISPATCH_GROUP_WIDTH="16"
                ;;
            *)
                export TILEXR_MOONEP_DISPATCH_PEER_MODE="group_credit"
                export TILEXR_MOONEP_DISPATCH_GROUP_WIDTH="16"
                ;;
        esac
    fi
fi
if [[ "${case_id}" == "${dispatch_repro_case_id}" ]]; then
    benchmark_kind="dispatch_hot_loop"
    dispatch_modes=("hidden")
elif [[ "${case_id}" == "${plan_reuse_repro_case_id}" ]]; then
    warmup="${warmup:-0}"
    iterations="${iterations:-8}"
elif [[ "${is_model_flow}" == "true" ]]; then
    benchmark_kind="model_flow"
    export TILEXR_MOONEP_UDMA_ARENA_RESERVE_BYTES="${TILEXR_MOONEP_UDMA_ARENA_RESERVE_BYTES:-$((768 * 1024 * 1024))}"
    case "${TILEXR_MOONEP_DISPATCH_PEER_MODE:-group_credit}" in
        group|group_credit)
            export TILEXR_MOONEP_PLANNER_WAIT_ITERATIONS="${TILEXR_MOONEP_PLANNER_WAIT_ITERATIONS:-100000000}"
            ;;
    esac
fi
planner_wait_iterations="${TILEXR_MOONEP_PLANNER_WAIT_ITERATIONS:-1000000}"
if [[ ! "${planner_wait_iterations}" =~ ^[1-9][0-9]*$ ]]; then
    echo "TILEXR_MOONEP_PLANNER_WAIT_ITERATIONS must be a positive integer: ${planner_wait_iterations}" >&2
    exit 2
fi
if [[ "${TILEXR_MOONEP_DIAGNOSTIC_FLOW:-0}" == "1" &&
      "${is_model_flow}" == "true" ]]; then
    benchmark_kind="flow"
fi
summary_file="${output_dir}/${case_id}/summary.json"

if [[ "${aggregate_only}" == "true" ]]; then
    cd "${TILEXR_HOME}"
    python -m tools.moonep.report \
        --aggregate-output-dir "${output_dir}" \
        --case-id "${case_id}" \
        --node-count "${node_count}" \
        --world-size "${rank_size}" \
        --mode "${mode}"
    if [[ ! -f "${summary_file}" ]]; then
        echo "Summary file not found: ${summary_file}" >&2
        exit 1
    fi
    echo "MoonEP multi-node global aggregation passed. Summary: ${summary_file}"
    python -m tools.moonep.report --summary "${summary_file}"
    exit 0
fi

if [[ "${generate_flowcharts}" == "true" ]]; then
    if ! command -v mmdc >/dev/null 2>&1; then
        echo "Mermaid CLI (mmdc) was not found; install a Mermaid CLI and browser" >&2
        exit 1
    fi
    mmdc_bin="$(command -v mmdc)"
    mmdc_help="$("${mmdc_bin}" --help 2>&1)"
    if grep -q -- '--puppeteerConfigFile' <<< "${mmdc_help}"; then
        mmdc_config_flag="--puppeteerConfigFile"
        mmdc_background_flag="--backgroundColor"
    elif grep -q -- '--playwright-config-file' <<< "${mmdc_help}"; then
        mmdc_config_flag="--playwright-config-file"
        mmdc_background_flag="--background-color"
    else
        echo "Unsupported Mermaid CLI interface: ${mmdc_bin}" >&2
        exit 1
    fi
    browser_config="${TILEXR_HOME}/tools/moonep/mermaid-browser-config.json"
    if [[ ! -f "${browser_config}" ]]; then
        echo "Mermaid browser config not found: ${browser_config}" >&2
        exit 1
    fi

    mermaid_smoke_dir="$(mktemp -d "${TMPDIR:-/tmp}/tilexr-mermaid-smoke.XXXXXX")"
    printf '%s\n' 'flowchart LR' '    A --> B' > "${mermaid_smoke_dir}/smoke.mmd"
    if ! "${mmdc_bin}" \
        "${mmdc_config_flag}" "${browser_config}" \
        --input "${mermaid_smoke_dir}/smoke.mmd" \
        --output "${mermaid_smoke_dir}/mermaid-smoke.svg" \
        "${mmdc_background_flag}" transparent \
        --quiet; then
        rm -rf -- "${mermaid_smoke_dir}"
        echo "Mermaid renderer preflight failed before NPU launch." >&2
        echo "For Playwright CLI run: python -m playwright install chromium" >&2
        echo "For npm CLI install a browser compatible with this host architecture." >&2
        exit 1
    fi
    rm -rf -- "${mermaid_smoke_dir}"
fi

if [[ -n "${visible_device_spec}" ]]; then
    export ASCEND_RT_VISIBLE_DEVICES="${visible_device_spec}"
fi
if [[ -n "${ASCEND_RT_VISIBLE_DEVICES:-}" ]]; then
    IFS=',' read -r -a visible_devices <<< "${ASCEND_RT_VISIBLE_DEVICES}"
    for device in "${visible_devices[@]}"; do
        if [[ ! "${device}" =~ ^[0-9]+$ ]]; then
            echo "ASCEND_RT_VISIBLE_DEVICES contains an invalid device: ${device}" >&2
            exit 2
        fi
    done
else
    if [[ ! "${TILEXR_ASCEND_DEV_NUM}" =~ ^[1-9][0-9]*$ ]]; then
        echo "No Ascend devices were detected by common_env.sh" >&2
        exit 1
    fi
    physical_device_count="${local_rank_size}"
    if (( physical_device_count > TILEXR_ASCEND_DEV_NUM )); then
        physical_device_count="${TILEXR_ASCEND_DEV_NUM}"
    fi
    visible_devices=()
    for ((device = 0; device < physical_device_count; device++)); do
        visible_devices+=("${device}")
    done
    export ASCEND_RT_VISIBLE_DEVICES="$(IFS=,; echo "${visible_devices[*]}")"
fi

physical_device_count="${#visible_devices[@]}"
if (( node_count > 1 )); then
    if (( local_rank_size != physical_device_count )); then
        echo "multi-node runs require exactly one rank per visible NPU" >&2
        exit 2
    fi
    ranks_per_device=1
elif (( rank_size <= physical_device_count )); then
    physical_device_count="${rank_size}"
    ranks_per_device=1
elif (( rank_size <= physical_device_count * 2 )); then
    ranks_per_device=2
else
    echo "rank_size ${rank_size} exceeds two ranks per visible NPU (${#visible_devices[@]} NPUs)" >&2
    exit 2
fi

export TILEXR_INSTALL_PREFIX="${install_prefix}"
export LD_LIBRARY_PATH="${install_prefix}/lib64:${install_prefix}/lib:${LD_LIBRARY_PATH:-}"
if [[ "${mode}" != "benchmark" ]]; then
    export HCCL_NPU_SOCKET_PORT_RANGE="${hccl_npu_socket_port_range}"
fi

echo "MoonEP mode: ${mode}"
echo "Case: ${case_id}"
echo "Ranks: ${rank_size} logical, ${physical_device_count} physical"
if (( node_count > 1 )); then
    echo "Nodes: ${node_count}, node rank: ${node_rank}, local ranks: ${local_rank_size}"
fi
echo "Devices: ${ASCEND_RT_VISIBLE_DEVICES}"
if [[ "${mode}" != "benchmark" ]]; then
    if [[ "${mode}" == "correctness" ]] || (( ranks_per_device == 2 )); then
        echo "Reference collective: Gloo (CPU staging)"
    else
        echo "Reference collective: HCCL"
        echo "HCCL NPU socket ports: ${HCCL_NPU_SOCKET_PORT_RANGE}"
    fi
fi
echo "Results: ${output_dir}"
echo "Tensor snapshots: ${dump_stage_tensors}"
if [[ "${dump_stage_tensors}" == "true" ]]; then
    echo "Tensor preview elements: ${tensor_preview_elements}"
fi
echo "Flowcharts: ${generate_flowcharts}"

model_replay_collect_multinode() {
    local node_spec index target marker deadline rsync_ssh
    local nodes=()
    local ssh_options=(
        -o ConnectTimeout=15
        -o ServerAliveInterval=30
        -o ServerAliveCountMax=3
    )
    # shellcheck disable=SC1090
    source "${model_runner_config}"
    node_spec=${MODEL_RUNNER_NODES//,/ }
    read -r -a nodes <<<"${node_spec}"
    if (( ${#nodes[@]} < node_count )); then
        echo "Model runner config does not contain ${node_count} nodes" >&2
        return 1
    fi
    rsync_ssh="ssh ${ssh_options[*]}"
    deadline=$((SECONDS + timeout_sec))
    for ((index = 1; index < node_count; index++)); do
        target=${MODEL_RUNNER_SSH_USER:-root}@${nodes[${index}]}
        marker=${output_dir}/node_${index}_complete.json
        while ! ssh "${ssh_options[@]}" "${target}" \
            "test -f $(printf '%q' "${marker}")"; do
            if (( SECONDS >= deadline )); then
                echo "Timed out waiting for ${target}:${marker}" >&2
                return 1
            fi
            sleep 2
        done
        rsync -a --protect-args -e "${rsync_ssh}" \
            "${target}:${output_dir}/${case_id}/" \
            "${output_dir}/${case_id}/"
        rsync -a --protect-args -e "${rsync_ssh}" \
            "${target}:${marker}" "${output_dir}/"
    done
    python -m tools.moonep.report \
        --aggregate-output-dir "${output_dir}" \
        --case-id "${case_id}" \
        --node-count "${node_count}" \
        --world-size "${rank_size}" \
        --mode "${mode}"
}

quote_command() {
    local quoted=()
    local value
    for value in "$@"; do
        printf -v value '%q' "${value}"
        quoted+=("${value}")
    done
    local IFS=' '
    printf '%s' "${quoted[*]}"
}

model_replay_follower_pids=()
model_replay_follower_logs=()

model_replay_prepare_shared_environment() {
    local comm_port
    if [[ -z "${master_addr}" || ! "${master_port}" =~ ^[1-9][0-9]*$ ]] ||
       (( master_port > 65535 )); then
        echo "managed multi-node model replay requires --master-addr and a valid --master-port" >&2
        return 2
    fi
    if [[ -z "${TILEXR_COMM_ID:-}" ]]; then
        comm_port=$((master_port + 10000))
        if (( comm_port > 65422 )); then
            comm_port=$((master_port - 10000))
        fi
        if (( comm_port < 1024 )); then
            echo "cannot derive a valid TileXR communicator port from --master-port ${master_port}" >&2
            return 2
        fi
        export TILEXR_COMM_ID="${master_addr}:${comm_port}"
    fi
    if [[ -z "${TILEXR_MOONEP_BARRIER_ADDR:-}" ]]; then
        TILEXR_MOONEP_BARRIER_ADDR="$(
            python - "${TILEXR_COMM_ID}" <<'PY'
import sys

from tools.moonep.rendezvous import offset_host_port

print(offset_host_port(sys.argv[1]))
PY
        )"
        export TILEXR_MOONEP_BARRIER_ADDR
    fi
    export TILEXR_MOONEP_LAUNCH_ID="${TILEXR_MOONEP_LAUNCH_ID:-model-replay-$(date +%Y%m%d-%H%M%S)-$$}"
    if [[ -z "${TILEXR_MOONEP_LAUNCH_SECRET:-}" ]]; then
        TILEXR_MOONEP_LAUNCH_SECRET="$(python - <<'PY'
import secrets

print(secrets.token_hex(32))
PY
        )"
        export TILEXR_MOONEP_LAUNCH_SECRET
    fi
}

model_replay_launch_followers() {
    local index log_path name node_spec remote_command remote_script target
    local nodes=()
    local remote_env=()
    local remote_env_names=(
        ASCEND_RT_VISIBLE_DEVICES
        HCCL_NPU_SOCKET_PORT_RANGE
        TILEXR_CANN_HOME
        TILEXR_COMM_ID
        TILEXR_ENABLE_CREDIT_IPC
        TILEXR_INSTALL_PREFIX
        TILEXR_LOG_LEVEL
        TILEXR_MODEL_REPLAY_RANK_TABLE_FILE
        TILEXR_MODEL_REPLAY_TILEXR_GIT_SHA
        TILEXR_MODEL_RUNNER_CONFIG
        TILEXR_MOONEP_AUTO_BUILD_INSTALL
        TILEXR_MOONEP_BARRIER_ADDR
        TILEXR_MOONEP_CONDA_SH
        TILEXR_MOONEP_CONDA_ENV
        TILEXR_MOONEP_DISPATCH_GROUP_WIDTH
        TILEXR_MOONEP_DISPATCH_PEER_MODE
        TILEXR_MOONEP_DUMP_DFX_ON_ERROR
        TILEXR_MOONEP_FLAG_DUMP_DIR
        TILEXR_MOONEP_FLAG_DUMP_MODE
        TILEXR_MOONEP_LAUNCH_ID
        TILEXR_MOONEP_LAUNCH_SECRET
        TILEXR_MOONEP_MODEL_REPLAY_GENERATION
        TILEXR_MOONEP_MODEL_REPLAY_META_ROOT
        TILEXR_MOONEP_OUTPUT_DIR
        TILEXR_MOONEP_PLANNER_WAIT_ITERATIONS
        TILEXR_MOONEP_TIMEOUT_SEC
        TILEXR_MOONEP_UDMA_ARENA_RESERVE_BYTES
        TILEXR_MINDSPEED_PREWARM_FRAMEWORK_OPS
        TILEXR_UDMA_ATTACH_EXISTING_RA
        TILEXR_UDMA_ROOTINFO_PATH
        TILEXR_UDMA_QP_ROUTE_SPEC
    )
    local ssh_options=(
        -o ConnectTimeout=15
        -o ServerAliveInterval=30
        -o ServerAliveCountMax=3
    )
    # shellcheck disable=SC1090
    source "${model_runner_config}"
    node_spec=${MODEL_RUNNER_NODES//,/ }
    read -r -a nodes <<<"${node_spec}"
    if (( ${#nodes[@]} < node_count )); then
        echo "Model runner config does not contain ${node_count} nodes" >&2
        return 1
    fi
    remote_script="${MODEL_RUNNER_TILEXR_HOME}/scripts/run_moonep.sh"
    for name in "${remote_env_names[@]}"; do
        if [[ -n "${!name:-}" ]]; then
            remote_env+=("${name}=${!name}")
        fi
    done
    remote_env+=(
        "TILEXR_INSTALL_PREFIX=${install_prefix}"
        "TILEXR_MODEL_RUNNER_CONFIG=${model_runner_config}"
        "TILEXR_MOONEP_MODEL_REPLAY_MANAGED_MULTINODE=0"
        "TILEXR_MOONEP_OUTPUT_DIR=${output_dir}"
        "TILEXR_MOONEP_TIMEOUT_SEC=${timeout_sec}"
    )
    mkdir -p "${output_dir}/controller"
    for ((index = 1; index < node_count; index++)); do
        target=${MODEL_RUNNER_SSH_USER:-root}@${nodes[${index}]}
        remote_command="$(quote_command env -C "${MODEL_RUNNER_TILEXR_HOME}" \
            "${remote_env[@]}" bash "${remote_script}" \
            "${original_args[@]}" --node-rank "${index}")"
        log_path="${output_dir}/controller/follower_${index}.log"
        model_replay_follower_logs+=("${log_path}")
        ssh "${ssh_options[@]}" "${target}" "${remote_command}" \
            >"${log_path}" 2>&1 &
        model_replay_follower_pids+=("$!")
    done
}

model_replay_stop_followers() {
    local pid
    for pid in "${model_replay_follower_pids[@]}"; do
        if kill -0 "${pid}" 2>/dev/null; then
            kill -TERM "${pid}" 2>/dev/null || true
        fi
    done
    for pid in "${model_replay_follower_pids[@]}"; do
        wait "${pid}" 2>/dev/null || true
    done
}

model_replay_wait_followers() {
    local failed=0 index status
    for index in "${!model_replay_follower_pids[@]}"; do
        status=0
        wait "${model_replay_follower_pids[${index}]}" || status=$?
        if [[ "${status}" -ne 0 ]]; then
            echo "Model replay follower failed with exit ${status}:" \
                "${model_replay_follower_logs[${index}]}" >&2
            failed=1
        fi
    done
    return "${failed}"
}

cd "${TILEXR_HOME}"
if (( node_count > 1 )); then
    managed_model_replay="${TILEXR_MOONEP_MODEL_REPLAY_MANAGED_MULTINODE:-1}"
    if [[ "${model_replay}" == "true" && "${node_rank}" -eq 0 &&
          "${managed_model_replay}" != "0" ]]; then
        model_replay_prepare_shared_environment
        model_replay_launch_followers
    fi
    if [[ "${mode}" != "benchmark" ]]; then
        export MASTER_ADDR="${master_addr}"
        export MASTER_PORT="${master_port}"
    fi
    distributed_args=(
        --mode "${mode}"
        --benchmark-kind "${benchmark_kind}"
        --cases "${case_file}"
        --case-ids "${case_id}"
        --node-count "${node_count}"
        --node-rank "${node_rank}"
        --local-world-size "${local_rank_size}"
        --physical-device-count "${physical_device_count}"
        --install-prefix "${install_prefix}"
        --output-dir "${output_dir}"
        --timeout-sec "${timeout_sec}"
        --wait-iterations "${planner_wait_iterations}"
    )
    if [[ "${dump_stage_tensors}" == "true" ]]; then
        distributed_args+=("--dump-stage-tensors")
        distributed_args+=("--tensor-preview-elements" "${tensor_preview_elements}")
    fi
    if [[ -n "${warmup}" ]]; then
        distributed_args+=("--warmup" "${warmup}")
    fi
    if [[ -n "${iterations}" ]]; then
        distributed_args+=("--iterations" "${iterations}")
    fi
    distributed_status=0
    python -m tools.moonep.distributed_node "${distributed_args[@]}" || distributed_status=$?
    if [[ "${distributed_status}" -ne 0 ]]; then
        model_replay_stop_followers
        exit "${distributed_status}"
    fi
    echo "Node ${node_rank} result: ${output_dir}/node_${node_rank}_complete.json"
    if [[ "${model_replay}" != "true" ]]; then
        exit 0
    fi
    if [[ "${node_rank}" -ne 0 ]]; then
        exit 0
    fi
    if (( ${#model_replay_follower_pids[@]} > 0 )); then
        if ! model_replay_wait_followers; then
            exit 1
        fi
    fi
    model_replay_collect_multinode
else
    launcher_args=(
        --mode "${mode}"
        --benchmark-kind "${benchmark_kind}"
        --cases "${case_file}"
        --case-ids "${case_id}"
        --world-size "${rank_size}"
        --physical-device-count "${physical_device_count}"
        --ranks-per-device "${ranks_per_device}"
        --install-prefix "${install_prefix}"
        --output-dir "${output_dir}"
        --timeout-sec "${timeout_sec}"
        --wait-iterations "${planner_wait_iterations}"
    )
    if [[ "${benchmark_kind}" == "dispatch_hot_loop" ]]; then
        launcher_args+=("--dispatch-modes" "${dispatch_modes[@]}")
    fi
    if [[ "${dump_stage_tensors}" == "true" ]]; then
        launcher_args+=("--dump-stage-tensors")
        launcher_args+=("--tensor-preview-elements" "${tensor_preview_elements}")
    fi
    if [[ -n "${warmup}" ]]; then
        launcher_args+=("--warmup" "${warmup}")
    fi
    if [[ -n "${iterations}" ]]; then
        launcher_args+=("--iterations" "${iterations}")
    fi
    echo "ASCEND_PROCESS_LOG_PATH: ${ASCEND_PROCESS_LOG_PATH}"
    python -m tools.moonep.launcher "${launcher_args[@]}"
fi

if [[ ! -f "${summary_file}" ]]; then
    echo "Summary file not found: ${summary_file}" >&2
    exit 1
fi

if [[ "${generate_flowcharts}" == "true" ]]; then
    flowchart_dir="${output_dir}/${case_id}/flowcharts"
    python -m tools.moonep.flowcharts \
        --case-dir "${output_dir}/${case_id}" \
        --world-size "${rank_size}" \
        --output-dir "${flowchart_dir}"
    expected_flowchart_prefixes=(
        1_planning
        2_dispatch
        3_prefetch-weight
        4_expert-forward
        5_combine
        6_reduce-grad
    )
    for prefix in "${expected_flowchart_prefixes[@]}"; do
        basename="${prefix}-${rank_size}rank-detailed-lr"
        mmd_file="${flowchart_dir}/${basename}.mmd"
        svg_file="${flowchart_dir}/${basename}.svg"
        png_file="${flowchart_dir}/${basename}.png"
        if [[ ! -s "${mmd_file}" ]]; then
            echo "Generated Mermaid source is missing or empty: ${mmd_file}" >&2
            exit 1
        fi
        "${mmdc_bin}" \
            "${mmdc_config_flag}" "${browser_config}" \
            --input "${mmd_file}" \
            --output "${svg_file}" \
            "${mmdc_background_flag}" transparent
        read -r svg_width svg_height < <(
            python - "${svg_file}" <<'PY'
import math
import sys
import xml.etree.ElementTree as ET

root = ET.parse(sys.argv[1]).getroot()
view_box = root.attrib["viewBox"].split()
if len(view_box) != 4:
    raise ValueError(f"invalid SVG viewBox: {root.attrib['viewBox']}")
# Mermaid CLI renders inside the browser's default 8px body margin on each edge.
print(
    math.ceil(float(view_box[2])) + 16,
    math.ceil(float(view_box[3])) + 16,
)
PY
        )
        "${mmdc_bin}" \
            "${mmdc_config_flag}" "${browser_config}" \
            --input "${mmd_file}" \
            --output "${png_file}" \
            "${mmdc_background_flag}" white \
            --width "${svg_width}" \
            --height "${svg_height}" \
            --scale 2
        if [[ ! -s "${svg_file}" || ! -s "${png_file}" ]]; then
            echo "Flowchart render is missing or empty: ${basename}" >&2
            exit 1
        fi
    done
    echo "Flowcharts: ${flowchart_dir}"
    echo "Flowchart files: 6 .mmd, 6 .svg, 6 .png"
fi

echo "MoonEP ${mode} passed. Summary: ${summary_file}"

if [[ "${dump_stage_tensors}" == "true" ]]; then
    tensor_dump_root="${output_dir}/${case_id}"
    preview_file="${tensor_dump_root}/rank_0/tensor_dumps/preview.log"
    if [[ ! -f "${preview_file}" ]]; then
        echo "Tensor preview file not found: ${preview_file}" >&2
        exit 1
    fi
    snapshot_count="$(find "${tensor_dump_root}" -path '*/tensor_dumps/*' -type f -name '*.pt' | wc -l | tr -d ' ')"
    manifest_count="$(find "${tensor_dump_root}" -path '*/tensor_dumps/*' -type f -name '*.json' | wc -l | tr -d ' ')"
    readable_count="$(find "${tensor_dump_root}" -path '*/tensor_dumps/*' -type f -name '*.txt' | wc -l | tr -d ' ')"
    echo "Tensor snapshots: ${tensor_dump_root}"
    echo "Snapshot files: ${snapshot_count} .pt, ${manifest_count} .json"
    echo "Readable files: ${readable_count} .txt"
    echo "Rank 0 preview: ${preview_file}"
fi

if [[ "${mode}" == "benchmark" ]]; then
    if [[ "${model_replay}" != "true" ||
          "${model_replay_stage_summary_only}" != "true" ]]; then
        python -m tools.moonep.report --summary "${summary_file}"
    fi
    if [[ "${model_replay}" == "true" ]]; then
        compare_args=(
            --model "${TILEXR_MOONEP_MODEL_PERFORMANCE}"
            --replay "${summary_file}"
        )
        if [[ "${model_replay_stage_summary_only}" == "true" ]]; then
            compare_args+=(--stage-summary-only)
        fi
        python -m tools.moonep.model_replay_compare "${compare_args[@]}"
    fi
fi
