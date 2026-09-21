#!/usr/bin/env bash
set -eo pipefail
case "$#:${1:-}" in
    0:) unset TILEXR_VALIDATION_RESUME_SUMMARY ;;
    2:--resume)
        printf '[commit-validate] Failed: P2.2 cannot reuse historical P2.1 results; checkout the M1/M2 commit to resume it.\n' >&2
        exit 2 ;;
    1:-h|1:--help)
        cat <<'EOF'
Usage: bash run_commit_validation.sh
Build and validate Combine P2.2: ep-sdedup-combine-p2-m3 (M1/M2/M3; no fixed delay).
Builds isolated profiling OFF/ON packages with identical source and Host regressions.
First two configured hosts, fixed8P each: 16 serial cases, each zero warmup + one launch.
Functional/performance/breakdown each cover BF16/FP16 combine/both (12 cases).
Plus BF16/FP16 Dispatch regression and two Dispatch-quant4/Combine-nonquant connections.
Checks DIRECT payload/Flag and current module results; M4/M5 and yOut arithmetic excluded.
Single-launch performance validates acquisition only: no baseline or threshold.
Requires configured CANN9.1, real A5 devices, SSH/rsync, msprof and hostfile.
Prints the full manifest and [i/16] progress; first failure stops, no automatic retries.
Historical P2.1 --resume results cannot qualify M3; use that checkpoint to resume it.
Requires Python 3.9+, CMake, Make and g++. TILEXR_VALIDATION_JOBS controls build jobs.
Reports: run/validation/commit-*/summary.json (or TILEXR_VALIDATION_RESULT_ROOT).
Return the summary and logs before proceeding to the next checkpoint.
EOF
        exit 0 ;;
    *) printf '[commit-validate] Failed: use no arguments (see --help)\n' >&2; exit 2 ;;
esac
# This entry point follows the current delivery checkpoint, independent of a
# previous Dispatch build or deployment-specific case options.
export TILEXR_VALIDATION_CHECKPOINT=ep-sdedup-combine-p2-m3
export TILEXR_VALIDATION_BUILD_MODE=production
export TILEXR_VALIDATION_ACTION=commit-validate
export TILEXR_VALIDATION_BOUNDARY_PROBE=0
export TILEXR_VALIDATION_BREAKDOWN=0
if ! source "$(dirname -- "${BASH_SOURCE[0]}")/set_env.sh"; then
    printf '失败 COMMIT_VALIDATION_FAIL checkpoint=%s type=CONFIG_FAILED (environment setup; no build or tests started)\n' "$TILEXR_VALIDATION_CHECKPOINT" >&2
    exit 1
fi
exec "$TILEXR_VALIDATION_PYTHON" -u "$TILEXR_VALIDATION_ROOT/tools/validation/workflow.py" commit-validate
