#!/usr/bin/env bash
# 人工批测入口：只调用 run_case.sh，不编译、不同步。
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")"
run_case() { bash ./run_case.sh "$@"; }
run_case run_mode=performance loops=50 warmup=50 bs=128
run_case run_mode=performance loops=50 warmup=50 bs=256
run_case run_mode=performance loops=50 warmup=50 bs=512
printf '\n================================\nPASS\n================================\n\n'
