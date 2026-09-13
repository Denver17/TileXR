#!/usr/bin/env bash
# 人工批测入口：只调用 run.sh，不编译、不同步。
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")"
completed_cases=0
run_case() {
    if bash ./run.sh "$@"; then
        completed_cases=$((completed_cases + 1))
    else
        local case_status=$?
        printf '\n================================\nBatch Failed\n================================\n\n'
        exit "$case_status"
    fi
}
run_case -n 2 -e 96 -m 1 -l 50 50 bs=128
run_case -n 2 -e 96 -m 1 -l 50 50 bs=256
run_case -n 2 -e 96 -m 1 -l 50 50 bs=512
if (( completed_cases > 0 )); then
    printf '\n================================\nBatch PASS cases=%s\n================================\n\n' "$completed_cases"
else
    printf '\n================================\nBatch Failed (no cases)\n================================\n\n'
    exit 1
fi
