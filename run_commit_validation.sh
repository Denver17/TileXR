#!/usr/bin/env bash
set -eo pipefail
case "$#:${1:-}" in
    0:) ;;
    1:-h|1:--help)
        cat <<'EOF'
Usage: bash run_commit_validation.sh
Build and validate the current Combine checkpoint: ep-sdedup-combine-p1-memory-boundary.
C7 builds one isolated profiling OFF package and runs C1-C6 plus boundary Host regressions.
It syncs to the first two configured hosts and runs three serial 16P functional cases:
8GiB dual-window boundary probes (8 points), then BF16 and FP16 real Dispatch regressions.
Requires configured CANN 9.1, real A5 devices, SSH/rsync and hostfile; no skips.
Progress is printed as [i/3]; the first failure stops the matrix.
Probe uses loops=1 warmup=0; Dispatch regressions use loops=5 warmup=2 in fresh processes.
Requires Python 3.9+, CMake, Make and g++. TILEXR_VALIDATION_JOBS controls build jobs.
Reports: run/validation/commit-*/summary.json (or TILEXR_VALIDATION_RESULT_ROOT).
Return the summary and logs before proceeding to the next checkpoint.
EOF
        exit 0 ;;
    *) printf '[commit-validate] Failed: no arguments expected (use --help)\n' >&2; exit 2 ;;
esac
# This entry point follows the current delivery checkpoint, independent of a
# previous Dispatch build or deployment-specific case options.
export TILEXR_VALIDATION_CHECKPOINT=ep-sdedup-combine-p1-memory-boundary
export TILEXR_VALIDATION_BUILD_MODE=production
export TILEXR_VALIDATION_ACTION=commit-validate
export TILEXR_VALIDATION_BOUNDARY_PROBE=0
export TILEXR_VALIDATION_BREAKDOWN=0
if ! source "$(dirname -- "${BASH_SOURCE[0]}")/set_env.sh"; then
    printf '失败 COMMIT_VALIDATION_FAIL checkpoint=%s type=CONFIG_FAILED (environment setup; no build or tests started)\n' "$TILEXR_VALIDATION_CHECKPOINT" >&2
    exit 1
fi
exec "$TILEXR_VALIDATION_PYTHON" -u "$TILEXR_VALIDATION_ROOT/tools/validation/workflow.py" commit-validate
