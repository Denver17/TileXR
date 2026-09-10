#!/usr/bin/env bash
# 单次测试的简短别名；参数与 run_case.sh 相同。
exec bash "$(dirname -- "${BASH_SOURCE[0]}")/run_case.sh" "$@"
