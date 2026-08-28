#!/usr/bin/env bash
set -euo pipefail

usage() {
    cat <<'EOF'
Usage: run_model_node.sh [options]

This non-interactive node runner is normally invoked by run_model.sh.

Required options:
  --backend tilexr|native
  --seq-length COUNT --hidden-size COUNT --moe-router-topk COUNT
  --node-count COUNT --node-rank RANK --master-addr ADDRESS --master-port PORT
  --devices-per-node COUNT --tilexr-home PATH --model-root PATH
  --install-prefix PATH --cann-env PATH --conda-sh PATH --conda-env NAME
  --native-env PATH --tokenizer-path PATH --data-path PATH --run-tag TAG

Optional: --profile --stage-barrier --hccl-inter-hccs-disable true|false
          --route-capture-dir PATH --route-capture-id ID
          --route-capture-skip-calls COUNT --route-capture-calls COUNT
          --performance-capture-dir PATH --performance-capture-id ID
          --performance-capture-skip-operators COUNT
          --performance-capture-operators COUNT
          --rank-table-file PATH --timeout SECONDS
          --udma-rootinfo-path PATH
Internal cleanup: --stop --backend BACKEND --node-rank RANK
                  --tilexr-home PATH --run-tag TAG
EOF
}

backend=
sequence_length=4096
hidden_size=7168
router_topk=8
route_capture_dir=""
route_capture_id=""
route_capture_skip_calls=60
route_capture_calls=10
performance_capture_dir=""
performance_capture_id=""
performance_capture_skip_operators=330
performance_capture_operators=55
node_count=
node_rank=
master_addr=
master_port=
devices_per_node=
tilexr_home=
model_root=
install_prefix=
cann_env=
conda_sh=
conda_env=
native_env=
tokenizer_path=
data_path=
run_tag=
timeout_sec=900
profile=0
stage_barrier=0
hccl_inter_hccs_disable=""
rank_table_file=""
udma_rootinfo_path=""
stop=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        --backend) backend=${2:?--backend requires a value}; shift 2 ;;
        --seq-length) sequence_length=${2:?--seq-length requires a value}; shift 2 ;;
        --hidden-size) hidden_size=${2:?--hidden-size requires a value}; shift 2 ;;
        --moe-router-topk) router_topk=${2:?--moe-router-topk requires a value}; shift 2 ;;
        --route-capture-dir)
            route_capture_dir=${2:?--route-capture-dir requires a value}
            shift 2
            ;;
        --route-capture-id)
            route_capture_id=${2:?--route-capture-id requires a value}
            shift 2
            ;;
        --route-capture-skip-calls)
            route_capture_skip_calls=${2:?--route-capture-skip-calls requires a value}
            shift 2
            ;;
        --route-capture-calls)
            route_capture_calls=${2:?--route-capture-calls requires a value}
            shift 2
            ;;
        --performance-capture-dir)
            performance_capture_dir=${2:?--performance-capture-dir requires a value}
            shift 2
            ;;
        --performance-capture-id)
            performance_capture_id=${2:?--performance-capture-id requires a value}
            shift 2
            ;;
        --performance-capture-skip-operators)
            performance_capture_skip_operators=${2:?--performance-capture-skip-operators requires a value}
            shift 2
            ;;
        --performance-capture-operators)
            performance_capture_operators=${2:?--performance-capture-operators requires a value}
            shift 2
            ;;
        --node-count) node_count=${2:?--node-count requires a value}; shift 2 ;;
        --node-rank) node_rank=${2:?--node-rank requires a value}; shift 2 ;;
        --master-addr) master_addr=${2:?--master-addr requires a value}; shift 2 ;;
        --master-port) master_port=${2:?--master-port requires a value}; shift 2 ;;
        --devices-per-node) devices_per_node=${2:?--devices-per-node requires a value}; shift 2 ;;
        --tilexr-home) tilexr_home=${2:?--tilexr-home requires a value}; shift 2 ;;
        --model-root) model_root=${2:?--model-root requires a value}; shift 2 ;;
        --install-prefix) install_prefix=${2:?--install-prefix requires a value}; shift 2 ;;
        --cann-env) cann_env=${2:?--cann-env requires a value}; shift 2 ;;
        --conda-sh) conda_sh=${2:?--conda-sh requires a value}; shift 2 ;;
        --conda-env) conda_env=${2:?--conda-env requires a value}; shift 2 ;;
        --native-env) native_env=${2:?--native-env requires a value}; shift 2 ;;
        --tokenizer-path) tokenizer_path=${2:?--tokenizer-path requires a value}; shift 2 ;;
        --data-path) data_path=${2:?--data-path requires a value}; shift 2 ;;
        --run-tag) run_tag=${2:?--run-tag requires a value}; shift 2 ;;
        --timeout) timeout_sec=${2:?--timeout requires a value}; shift 2 ;;
        --profile) profile=1; shift ;;
        --stage-barrier) stage_barrier=1; shift ;;
        --hccl-inter-hccs-disable)
            hccl_inter_hccs_disable=${2:?--hccl-inter-hccs-disable requires a value}
            shift 2
            ;;
        --rank-table-file) rank_table_file=${2:?--rank-table-file requires a value}; shift 2 ;;
        --udma-rootinfo-path)
            udma_rootinfo_path=${2:?--udma-rootinfo-path requires a value}
            shift 2
            ;;
        --stop) stop=1; shift ;;
        -h|--help) usage; exit 0 ;;
        *) printf 'Unknown argument: %s\n' "$1" >&2; usage >&2; exit 2 ;;
    esac
done

case "${hccl_inter_hccs_disable}" in
    "") unset HCCL_INTER_HCCS_DISABLE ;;
    true|false) export HCCL_INTER_HCCS_DISABLE=${hccl_inter_hccs_disable} ;;
    *) printf 'Invalid HCCL inter-HCCS value: %s\n' "${hccl_inter_hccs_disable}" >&2; exit 2 ;;
esac
if [[ -n "${rank_table_file}" ]]; then
    [[ -f "${rank_table_file}" ]] || { echo "Rank table not found: ${rank_table_file}" >&2; exit 2; }
    export RANK_TABLE_FILE=${rank_table_file}
else
    unset RANK_TABLE_FILE
fi
if [[ -n "${udma_rootinfo_path}" ]]; then
    [[ -f "${udma_rootinfo_path}" ]] || {
        echo "UDMA RootInfo not found: ${udma_rootinfo_path}" >&2
        exit 2
    }
    export TILEXR_UDMA_ROOTINFO_PATH=${udma_rootinfo_path}
else
    unset TILEXR_UDMA_ROOTINFO_PATH
fi

case "${backend}" in tilexr|native) ;; *) printf 'Invalid backend: %s\n' "${backend}" >&2; exit 2 ;; esac
if [[ -z "${tilexr_home}" || -z "${run_tag}" || -z "${node_rank}" ]]; then
    echo "--tilexr-home, --run-tag, and --node-rank are required" >&2
    exit 2
fi
if [[ ! "${run_tag}" =~ ^[A-Za-z0-9_.-]+$ ]] || [[ ! "${node_rank}" =~ ^[0-9]+$ ]]; then
    echo "unsafe run tag or node rank" >&2
    exit 2
fi

output=${tilexr_home}/run/moonep/mindspeed/${run_tag}/${backend}/node_${node_rank}
stop_existing_run() {
    local model_pid runner_pid
    if [[ -f "${output}/model.pid" ]]; then
        model_pid=$(<"${output}/model.pid")
        if [[ "${model_pid}" =~ ^[0-9]+$ ]] && kill -0 "${model_pid}" 2>/dev/null; then
            kill -- -"${model_pid}" 2>/dev/null || true
            sleep 2
            kill -KILL -- -"${model_pid}" 2>/dev/null || true
        fi
    fi
    if [[ -f "${output}/runner.pid" ]]; then
        runner_pid=$(<"${output}/runner.pid")
        if [[ "${runner_pid}" =~ ^[0-9]+$ ]] && kill -0 "${runner_pid}" 2>/dev/null; then
            kill "${runner_pid}" 2>/dev/null || true
        fi
    fi
}
if [[ "${stop}" -eq 1 ]]; then
    stop_existing_run
    exit 0
fi

required_values=(
    node_count master_addr master_port devices_per_node model_root install_prefix
    cann_env conda_sh conda_env native_env tokenizer_path data_path
)
for variable in "${required_values[@]}"; do
    if [[ -z "${!variable:-}" ]]; then
        printf 'Missing required option for %s\n' "${variable}" >&2
        exit 2
    fi
done
for variable in node_count node_rank master_port devices_per_node timeout_sec \
    sequence_length hidden_size router_topk route_capture_skip_calls \
    route_capture_calls performance_capture_skip_operators \
    performance_capture_operators; do
    if [[ ! "${!variable}" =~ ^[0-9]+$ ]]; then
        printf '%s must be an integer\n' "${variable}" >&2
        exit 2
    fi
done
if (( node_count < 1 || node_rank >= node_count || devices_per_node < 1 || \
      master_port < 1 || master_port > 65535 || timeout_sec < 1 )); then
    echo "invalid distributed launcher dimensions" >&2
    exit 2
fi
if (( sequence_length < 1 || hidden_size < 1 || hidden_size % 128 != 0 || \
      router_topk < 1 || router_topk > 32 || route_capture_skip_calls < 0 || \
      route_capture_calls < 1 || performance_capture_skip_operators < 0 || \
      performance_capture_operators < 1 )); then
    echo "invalid model shape or route capture dimensions" >&2
    exit 2
fi

pass_TILEXR_MOONEP_DUMP_DFX_ON_ERROR=${TILEXR_MOONEP_DUMP_DFX_ON_ERROR:-}
pass_TILEXR_MOONEP_DISPATCH_GROUP_WIDTH=${TILEXR_MOONEP_DISPATCH_GROUP_WIDTH:-}
pass_TILEXR_MOONEP_DISPATCH_PEER_MODE=${TILEXR_MOONEP_DISPATCH_PEER_MODE:-}
pass_TILEXR_MOONEP_FLAG_DUMP_DIR=${TILEXR_MOONEP_FLAG_DUMP_DIR:-}
pass_TILEXR_MOONEP_FLAG_DUMP_MODE=${TILEXR_MOONEP_FLAG_DUMP_MODE:-}
pass_TILEXR_MOONEP_PLANNER_WAIT_ITERATIONS=${TILEXR_MOONEP_PLANNER_WAIT_ITERATIONS:-}
if [[ -n "${performance_capture_dir}" || -n "${performance_capture_id}" ]]; then
    if [[ "${backend}" != "tilexr" || -z "${performance_capture_dir}" ||
          -z "${performance_capture_id}" ||
          ! "${performance_capture_id}" =~ ^[A-Za-z0-9_.-]+$ ]]; then
        echo "performance capture requires TileXR, a directory, and a safe capture ID" >&2
        exit 2
    fi
fi
if [[ -n "${route_capture_dir}" || -n "${route_capture_id}" ]]; then
    if [[ "${backend}" != "tilexr" || -z "${route_capture_dir}" ||
          -z "${route_capture_id}" ||
          ! "${route_capture_id}" =~ ^[A-Za-z0-9_.-]+$ ]]; then
        echo "route capture requires TileXR, a directory, and a safe capture ID" >&2
        exit 2
    fi
fi
for path in "${cann_env}" "${conda_sh}" "${native_env}"; do
    [[ -f "${path}" ]] || { printf 'Required file not found: %s\n' "${path}" >&2; exit 1; }
done
for path in "${model_root}/MindSpeed" "${model_root}/MindSpeed-LLM" \
    "${model_root}/shmem/src/python" "${tokenizer_path}"; do
    [[ -e "${path}" ]] || { printf 'Required path not found: %s\n' "${path}" >&2; exit 1; }
done

mkdir -p "${output}"
printf '%s\n' "$$" >"${output}/runner.pid"
model_pid=
cleanup_model() {
    if [[ -n "${model_pid}" ]] && kill -0 "${model_pid}" 2>/dev/null; then
        kill -- -"${model_pid}" 2>/dev/null || true
        sleep 2
        kill -KILL -- -"${model_pid}" 2>/dev/null || true
        wait "${model_pid}" 2>/dev/null || true
    fi
}
finish_runner() {
    if [[ -f "${output}/runner.pid" ]] && [[ "$(<"${output}/runner.pid")" == "$$" ]]; then
        rm -f "${output}/runner.pid" "${output}/model.pid"
    fi
}
trap 'cleanup_model; finish_runner' EXIT
trap 'exit 130' INT
trap 'exit 143' TERM HUP
exec > >(tee "${output}/controller.log") 2>&1

set +u
# Vendor and conda environment scripts commonly append optional variables.
# shellcheck disable=SC1090
source "${cann_env}"
# shellcheck disable=SC1090
source "${conda_sh}"
conda activate "${conda_env}"
# shellcheck disable=SC1090
source "${native_env}"
set -u

idle_probe=${tilexr_home}/tools/moonep/mindspeed/probe_idle.sh
[[ -f "${idle_probe}" ]] || { echo "Idle probe not found: ${idle_probe}" >&2; exit 1; }
for gate in 1 2; do
    idle=$(bash "${idle_probe}" --devices "${devices_per_node}" \
        --log "${output}/npu_gate_${gate}.log")
    if [[ "${idle}" -ne "${devices_per_node}" ]]; then
        printf 'exit_code=90\nreason=npu_busy\ngate=%s\nidle=%s\n' "${gate}" "${idle}" \
            | tee "${output}/result.txt"
        exit 90
    fi
    [[ "${gate}" -eq 1 ]] && sleep 5
done

shmem_python=${model_root}/shmem/src/python
shmem_backend=${shmem_python}/shmem/backends/950
mindspeed_home=${model_root}/MindSpeed
mindspeed_llm_home=${model_root}/MindSpeed-LLM
export PYTHONPATH="${mindspeed_home}:${shmem_python}:${mindspeed_llm_home}${PYTHONPATH:+:${PYTHONPATH}}"
export LD_LIBRARY_PATH="${shmem_backend}:${LD_LIBRARY_PATH:-}"
[[ -f /usr/lib64/libstdc++.so.6 ]] && export LD_PRELOAD=/usr/lib64/libstdc++.so.6

interface=${MODEL_RUNNER_SOCKET_IFNAME:-}
if [[ -z "${interface}" && "${node_count}" -gt 1 ]] && \
    ip -o -4 addr show dev data0.3001 2>/dev/null | grep -q .; then
    interface=data0.3001
fi
if [[ -z "${interface}" ]]; then
    interface=$(ip route get "${master_addr}" 2>/dev/null | awk '{for (i=1; i<=NF; ++i) if ($i == "dev") {print $(i+1); exit}}')
fi
if [[ -z "${interface}" || "${interface}" == lo ]]; then
    interface=$(ip -4 -brief address | awk '$1 != "lo" && $3 != "" {print $1; exit}')
fi
[[ -n "${interface}" ]] || { echo "Unable to determine communication interface" >&2; exit 1; }

export HCCL_HOST_SOCKET_PORT_RANGE=auto
export HCCL_NPU_SOCKET_PORT_RANGE=${MODEL_RUNNER_HCCL_NPU_SOCKET_PORT_RANGE:-47000-47100}
export HCCL_SOCKET_IFNAME=${interface}
export GLOO_SOCKET_IFNAME=${interface}
export HCCL_BUFFSIZE=200
export HCCL_XN_RES_NUM=2000
export HCCL_DISABLE_NHR=1
export HCCL_DFS_CONFIG=task_exception:off
export HCCL_CONNECT_TIMEOUT=120
export HCCL_EXEC_TIMEOUT=120
export CUDA_DEVICE_MAX_CONNECTIONS=1
export TASK_QUEUE_ENABLE=2
export PYTORCH_NPU_ALLOC_CONF=expandable_segments:True
export STREAMS_PER_DEVICE=32
if [[ "${devices_per_node}" -eq 8 ]]; then
    export CPU_AFFINITY_CONF=${CPU_AFFINITY_CONF:-1,npu0:192-215,npu1:216-239,npu2:0-23,npu3:24-47,npu4:48-71,npu5:72-95,npu6:240-263,npu7:264-287}
fi

unset TILEXR_MOONEP_TRACE_STAGES TILEXR_MOONEP_TRACE_PLANNER_MAGIC
unset TILEXR_MOONEP_DEBUG_SYNC_COMBINE TILEXR_MOONEP_DEBUG_SYNC_DISPATCH_STATUS
unset TILEXR_MOONEP_DEBUG_PREFETCH_UDMA TILEXR_MOONEP_DUMP_DFX_ON_ERROR
unset TILEXR_MOONEP_FLAG_DUMP_DIR TILEXR_MOONEP_FLAG_DUMP_MODE
unset TILEXR_MINDSPEED_TRACE TILEXR_MINDSPEED_PLAN_DUMP_DIR
unset MOONEP_MINDSPEED_STAGE_BARRIER TILEXR_MINDSPEED_STAGE_BARRIER
unset TILEXR_MINDSPEED_FORCE_DUMMY_UDMA ASCEND_LAUNCH_BLOCKING
unset PROFILING_MODE PROFILING_OPTIONS ASCEND_MOONEP_DISPATCH_ENABLE_DFX
unset ASCEND_MOONEP_DISPATCH_ENABLE_TRACE ASCEND_MOONEP_DISPATCH_TRACE
unset TILEXR_MINDSPEED_FINITE_CHECK
[[ -n "${pass_TILEXR_MOONEP_DUMP_DFX_ON_ERROR}" ]] && \
    export TILEXR_MOONEP_DUMP_DFX_ON_ERROR=${pass_TILEXR_MOONEP_DUMP_DFX_ON_ERROR}
[[ -n "${pass_TILEXR_MOONEP_FLAG_DUMP_DIR}" ]] && \
    export TILEXR_MOONEP_FLAG_DUMP_DIR=${pass_TILEXR_MOONEP_FLAG_DUMP_DIR}
[[ -n "${pass_TILEXR_MOONEP_FLAG_DUMP_MODE}" ]] && \
    export TILEXR_MOONEP_FLAG_DUMP_MODE=${pass_TILEXR_MOONEP_FLAG_DUMP_MODE}
[[ -n "${pass_TILEXR_MOONEP_PLANNER_WAIT_ITERATIONS}" ]] && \
    export TILEXR_MOONEP_PLANNER_WAIT_ITERATIONS=${pass_TILEXR_MOONEP_PLANNER_WAIT_ITERATIONS}
unset TILEXR_MINDSPEED_ROUTE_CAPTURE_DIR TILEXR_MINDSPEED_ROUTE_CAPTURE_ID
unset TILEXR_MINDSPEED_ROUTE_CAPTURE_SKIP_CALLS TILEXR_MINDSPEED_ROUTE_CAPTURE_CALLS
if [[ -n "${route_capture_dir}" ]]; then
    mkdir -p "${route_capture_dir}"
    export TILEXR_MINDSPEED_ROUTE_CAPTURE_DIR=${route_capture_dir}
    export TILEXR_MINDSPEED_ROUTE_CAPTURE_ID=${route_capture_id}
    export TILEXR_MINDSPEED_ROUTE_CAPTURE_SKIP_CALLS=${route_capture_skip_calls}
    export TILEXR_MINDSPEED_ROUTE_CAPTURE_CALLS=${route_capture_calls}
fi
unset TILEXR_MOONEP_PERF_CAPTURE_DIR TILEXR_MOONEP_PERF_CAPTURE_ID
unset TILEXR_MOONEP_PERF_CAPTURE_SKIP_OPERATORS
unset TILEXR_MOONEP_PERF_CAPTURE_OPERATORS
if [[ -n "${performance_capture_dir}" ]]; then
    mkdir -p "${performance_capture_dir}"
    export TILEXR_MOONEP_PERF_CAPTURE_DIR=${performance_capture_dir}
    export TILEXR_MOONEP_PERF_CAPTURE_ID=${performance_capture_id}
    export TILEXR_MOONEP_PERF_CAPTURE_SKIP_OPERATORS=${performance_capture_skip_operators}
    export TILEXR_MOONEP_PERF_CAPTURE_OPERATORS=${performance_capture_operators}
fi

world_size=$((node_count * devices_per_node))
global_batch_size=${world_size}
ep_size=${world_size}
base_expert_count=32
expert_count=$((((base_expert_count + ep_size - 1) / ep_size) * ep_size))
backend_args=()
install -m 0644 "${tilexr_home}/tools/moonep/mindspeed/mindspeed_stage_barrier.py" \
    "${mindspeed_home}/mindspeed/core/transformer/moe/mindspeed_stage_barrier.py"
export MOONEP_MINDSPEED_STAGE_BARRIER=${stage_barrier}
if [[ "${backend}" == tilexr ]]; then
    MINDSPEED_HOME=${mindspeed_home} TILEXR_HOME=${tilexr_home} \
        TILEXR_INSTALL_PREFIX=${install_prefix} \
        bash "${tilexr_home}/tools/moonep/mindspeed/preflight_adapter.sh"
    export PYTHONPATH="${mindspeed_home}:${tilexr_home}/integrations/moonep_torch:${shmem_python}:${shmem_backend}:${mindspeed_llm_home}:${tilexr_home}${PYTHONPATH:+:${PYTHONPATH}}"
    export LD_LIBRARY_PATH="${install_prefix}/lib64:${shmem_backend}:${LD_LIBRARY_PATH:-}"
    export TILEXR_INSTALL_PREFIX=${install_prefix}
    export TILEXR_UDMA_QP_ROUTE_SPEC=port_count:6,port_count:2
    export TILEXR_ENABLE_CREDIT_IPC=${TILEXR_ENABLE_CREDIT_IPC:-1}
    export TILEXR_UDMA_ATTACH_EXISTING_RA=1
    if [[ -n "${pass_TILEXR_MOONEP_DISPATCH_PEER_MODE}" ]]; then
        case "${pass_TILEXR_MOONEP_DISPATCH_PEER_MODE}" in
            legacy)
                echo "TILEXR_MOONEP_DISPATCH_PEER_MODE=legacy is prohibited" >&2
                exit 2
                ;;
            group|group_credit)
                export TILEXR_MOONEP_DISPATCH_PEER_MODE=${pass_TILEXR_MOONEP_DISPATCH_PEER_MODE}
                export TILEXR_MOONEP_DISPATCH_GROUP_WIDTH=${pass_TILEXR_MOONEP_DISPATCH_GROUP_WIDTH:-16}
                ;;
            *)
                echo "Invalid TILEXR_MOONEP_DISPATCH_PEER_MODE=${pass_TILEXR_MOONEP_DISPATCH_PEER_MODE}" >&2
                exit 2
                ;;
        esac
    else
        export TILEXR_MOONEP_DISPATCH_PEER_MODE=group_credit
        export TILEXR_MOONEP_DISPATCH_GROUP_WIDTH=16
    fi
    export TILEXR_MOONEP_COMBINE_VERSION=2
    export TILEXR_MOONEP_UDMA_ARENA_RESERVE_BYTES=${TILEXR_MOONEP_UDMA_ARENA_RESERVE_BYTES:-$((768 * 1024 * 1024))}
    unset TILEXR_MOONEP_DISPATCH_TRANSPORT
    export TILEXR_COMM_ID="${master_addr}:$((master_port + 10000))"
    backend_args+=(
        --moonep-token-padding 1
        --moonep-backend-factory
        mindspeed.core.transformer.moe.tilexr_mindspeed_adapter:create_tilexr_moonep_backend
    )
else
    if [[ "${stage_barrier}" -eq 1 ]]; then
        backend_args+=(
            --moonep-backend-factory
            mindspeed.core.transformer.moe.mindspeed_stage_barrier:create_native_barrier_backend
        )
    fi
fi

profile_output=${output}/profiling
profile_args=()
if [[ "${profile}" -eq 1 ]]; then
    mkdir -p "${profile_output}"
    profile_args+=(
        --profile --profile-step-start 6 --profile-step-end 7
        --profile-with-cpu --profile-ranks -1 --profile-level level1
        --profile-export-type text --profile-save-path "${profile_output}"
    )
fi

rank_table_sha256=disabled
if [[ -n "${rank_table_file}" ]]; then
    rank_table_sha256=$(sha256sum "${rank_table_file}" | awk '{print $1}')
fi
udma_rootinfo_sha256=disabled
if [[ -n "${udma_rootinfo_path}" ]]; then
    udma_rootinfo_sha256=$(sha256sum "${udma_rootinfo_path}" | awk '{print $1}')
fi

{
    printf 'backend=%s\nrun_tag=%s\nnode_rank=%s\nworld_size=%s\nrank_per_dev=1\n' \
        "${backend}" "${run_tag}" "${node_rank}" "${world_size}"
    printf 'ep_size=%s\nprofile=%s\nstage_barrier=%s\ninterface=%s\n' \
        "${ep_size}" "${profile}" "${stage_barrier}" "${interface}"
    printf 'rank_table_file=%s\nrank_table_sha256=%s\n' \
        "${rank_table_file:-disabled}" "${rank_table_sha256}"
    printf 'udma_rootinfo_path=%s\nudma_rootinfo_sha256=%s\n' \
        "${udma_rootinfo_path:-disabled}" "${udma_rootinfo_sha256}"
    printf 'shape=S%s/K%s/H%s layers=4 experts=%s token_padding=1 iterations=8\n' \
        "${sequence_length}" "${router_topk}" "${hidden_size}" "${expert_count}"
    printf 'finite_check=%s flag_dump=disabled plan_dump=disabled\n' \
        "${TILEXR_MINDSPEED_FINITE_CHECK:-disabled}"
    env | grep -E '^(TILEXR_|MOONEP_|ASCEND_MOONEP_|HCCL_|GLOO_SOCKET)' | sort
} >"${output}/provenance.log"

cd "${mindspeed_llm_home}"
set +e
setsid timeout --signal=TERM --kill-after=20s "${timeout_sec}s" \
python -m torch.distributed.launch \
    --nproc_per_node "${devices_per_node}" \
    --nnodes "${node_count}" \
    --node_rank "${node_rank}" \
    --master_addr "${master_addr}" \
    --master_port "${master_port}" \
    pretrain_gpt.py \
    --te-gmm-mode performance \
    --transformer-impl transformer_engine \
    --disable-gloo-group \
    --no-check-for-nan-in-loss-and-grad \
    --spec mindspeed_llm.tasks.models.spec.deepseek_spec layer_spec \
    --gemm-gradient-accumulation-fusion \
    --manual-gc --manual-gc-interval 50 \
    --use-distributed-optimizer --use-flash-attn --use-mcore-models \
    --tensor-model-parallel-size 1 \
    --pipeline-model-parallel-size 1 \
    --expert-model-parallel-size "${ep_size}" \
    --expert-tensor-parallel-size 1 \
    --sequence-parallel \
    --context-parallel-size 1 \
    --context-parallel-algo ulysses_cp_algo \
    --num-layers 4 \
    --hidden-size "${hidden_size}" \
    --ffn-hidden-size 18432 \
    --num-attention-heads 128 \
    --tokenizer-type PretrainedFromHF \
    --tokenizer-name-or-path "${tokenizer_path}" \
    --seq-length "${sequence_length}" \
    --max-position-embeddings 163840 \
    --micro-batch-size 1 \
    --global-batch-size "${global_batch_size}" \
    --make-vocab-size-divisible-by 1 \
    --lr 1.0e-5 --train-iters 8 --lr-decay-style cosine \
    --untie-embeddings-and-output-weights --disable-bias-linear \
    --attention-dropout 0.0 --hidden-dropout 0.0 --init-method-std 0.02 \
    --position-embedding-type rope --normalization RMSNorm \
    --use-fused-rotary-pos-emb --use-rotary-position-embeddings \
    --use-fused-swiglu --use-fused-rmsnorm --swiglu \
    --no-masked-softmax-fusion --attention-softmax-in-fp32 \
    --min-lr 1.0e-7 --weight-decay 1e-2 --lr-warmup-iters 0 \
    --clip-grad 1.0 --adam-beta1 0.9 --adam-beta2 0.999 \
    --initial-loss-scale 65536 \
    --vocab-size 129280 --padded-vocab-size 129280 \
    --rotary-base 10000 --norm-epsilon 1e-6 \
    --no-load-optim --no-load-rng --bf16 --distributed-timeout-minutes 2 \
    --data-path "${data_path}" \
    --split 100,0,0 \
    --log-interval 1 --save-interval 20000 --eval-interval 20000 --eval-iters 0 \
    --no-save-optim --no-save-rng --no-shared-storage --exit-interval 8 \
    --multi-latent-attention --qk-pos-emb-head-dim 64 --qk-head-dim 128 \
    --q-lora-rank 1536 --kv-lora-rank 512 --v-head-dim 128 \
    --qk-layernorm --mla-mm-split --mla-fa-without-pad \
    --moe-grouped-gemm --moe-token-dispatcher-type flex \
    --first-k-dense-replace 0 --moe-enable-moonep --moe-layer-freq 1 \
    --moe-shared-expert-intermediate-size 2048 --num-experts "${expert_count}" \
    --moe-router-topk "${router_topk}" --moe-ffn-hidden-size 2048 \
    --moe-router-load-balancing-type seq_aux_loss \
    --moe-router-num-groups 8 --moe-router-group-topk 4 \
    --moe-router-topk-scaling-factor 2.5 --moe-aux-loss-coeff 0.0001 \
    --norm-topk-prob --moe-router-score-function sigmoid \
    --moe-router-enable-expert-bias --moe-router-dtype fp32 \
    --mtp-num-layers 1 --mtp-loss-scaling-factor 0.3 \
    --mtp-mem-efficient-logits --recompute-activation-function \
    --recompute-mla-up-proj --swap-optimizer --swap-optimizer-times 16 \
    --beta-fast 32 --beta-slow 1 --rope-scaling-factor 40 \
    --rope-scaling-mscale 1.0 --rope-scaling-mscale-all-dim 1.0 \
    --rope-scaling-original-max-position-embeddings 4096 \
    --rope-scaling-type yarn \
    --moonep-full-vmm-mode performance --moonep-zero-copy-recompute \
    "${profile_args[@]}" \
    "${backend_args[@]}" \
    --distributed-backend nccl &
model_pid=$!
printf '%s\n' "${model_pid}" >"${output}/model.pid"
wait "${model_pid}"
status=$?
set -e
model_pid=

iterations=$(grep -Ec 'iteration[[:space:]]+[0-9]+/[[:space:]]*[0-9]+' "${output}/controller.log" || true)
last_iteration=$(grep -E 'iteration[[:space:]]+[0-9]+/[[:space:]]*[0-9]+' "${output}/controller.log" | tail -1 || true)
skipped=$(grep -Ec 'number of skipped iterations:[[:space:]]+[1-9]' "${output}/controller.log" || true)
nan=$(grep -Ec 'number of nan iterations:[[:space:]]+[1-9]' "${output}/controller.log" || true)
nonfinite_grad=$(grep -Eic 'grad norm:[[:space:]]*(-?inf|nan)' \
    "${output}/controller.log" || true)
finite_final_loss=$(grep -Ec \
    'iteration[[:space:]]+8/[[:space:]]*8.*lm loss:[[:space:]]*[0-9]' \
    "${output}/controller.log" || true)
profile_done=$(find "${profile_output}" -type f -name analyse.done 2>/dev/null | wc -l || true)
post_idle=$(bash "${idle_probe}" --devices "${devices_per_node}" \
    --log "${output}/npu_after.log")
if [[ "${status}" -eq 0 && ( "${skipped}" -ne 0 || "${nan}" -ne 0 ) ]]; then
    status=92
fi
if [[ "${status}" -eq 0 && "${node_count}" -eq 1 && \
      ( "${nonfinite_grad}" -ne 0 || "${finite_final_loss}" -eq 0 ) ]]; then
    status=93
fi
if [[ "${status}" -eq 0 && "${profile}" -eq 1 && \
      "${profile_done}" -ne "${devices_per_node}" ]]; then
    status=94
fi
printf 'exit_code=%s\niterations=%s\nlast_iteration=%s\nskipped_nonzero=%s\nnan_nonzero=%s\nnonfinite_grad=%s\nfinite_final_loss=%s\nprofile_done=%s\npost_idle=%s\ncompleted=%s\n' \
    "${status}" "${iterations}" "${last_iteration}" "${skipped}" "${nan}" \
    "${nonfinite_grad}" "${finite_final_loss}" "${profile_done}" "${post_idle}" \
    "$(date '+%F %T %z')" | tee "${output}/result.txt"
exit "${status}"
