#!/usr/bin/env bash
set -euo pipefail

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
tilexr_root=$(cd -- "${script_dir}/../.." && pwd)
readonly rank_size=${TILEXR_EP_ROUND_TEST_RANK_SIZE:-8}
readonly loop_count=${TILEXR_EP_ROUND_TEST_LOOPS:-100}
readonly wait_seconds=${TILEXR_EP_ROUND_TEST_WAIT_SECONDS:-120}
readonly retry_seconds=${TILEXR_EP_ROUND_TEST_RETRY_SECONDS:-5}

cd -- "${tilexr_root}"
bash tests/ep/build.sh full
ctest --test-dir tests/ep/build --output-on-failure \
    -R 'test_tilexr_ep_(layout|host_validation|kernel_sources|api_sources)$'

deadline=$((SECONDS + wait_seconds))
while true; do
    set +e
    probe_output=$(bash tests/ep/cluster/probe_npu_processes.sh 2>&1)
    probe_status=$?
    set -e
    printf '%s\n' "${probe_output}"
    if ((probe_status == 0)); then
        break
    fi
    if ((probe_status != 3)); then
        echo "NPU process probe failed with status ${probe_status}" >&2
        exit "${probe_status}"
    fi
    if ((SECONDS + retry_seconds > deadline)); then
        echo "NPU remained occupied for ${wait_seconds}s" >&2
        exit 75
    fi
    sleep "${retry_seconds}"
done

run_case()
{
    local mode=$1
    local port=$2
    local perf_mode=1
    if [[ "${mode}" == "dispatch_combine" ]]; then
        perf_mode=0
    fi
    echo "EP_MEMORY_ROUND_CASE_START mode=${mode} loops=${loop_count} ranks=${rank_size}"
    TILEXR_COMM_ID="127.0.0.1:${port}" \
        TILEXR_EP_DEMO_PERF="${perf_mode}" \
        TILEXR_EP_DEMO_WARMUP=0 \
        bash tests/ep/demo/run_tilexr_ep_dispatch_demo.sh \
        "${rank_size}" "${rank_size}" 0 "${loop_count}" memory 4 8 2 "" "${mode}"
    echo "EP_MEMORY_ROUND_CASE_PASS mode=${mode} loops=${loop_count} ranks=${rank_size}"
}

run_case dispatch 11077
run_case combine 11078
run_case dispatch_combine 11079

echo "EP_MEMORY_ROUND_VALIDATION_PASS"
