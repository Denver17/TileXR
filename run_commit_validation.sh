#!/usr/bin/env bash
set -eo pipefail
case "$#:${1:-}" in
    0:) ;;
    1:-h|1:--help)
        cat <<'EOF'
Usage: bash run_commit_validation.sh
Build and validate the current Combine checkpoint: ep-sdedup-combine-p1-abi-host.
C1 runs three local C++ Host tests; no CANN, SSH, hostfile or NPU is required.
Requires Python 3.9+, CMake, Make and g++. TILEXR_VALIDATION_JOBS controls build jobs.
Reports: run/validation/commit-*/summary.json (or TILEXR_VALIDATION_RESULT_ROOT).
Return the summary and logs before proceeding to the next checkpoint.
EOF
        exit 0 ;;
    *) printf '[commit-validate] Failed: no arguments expected (use --help)\n' >&2; exit 2 ;;
esac
# This entry point follows the current delivery checkpoint, independent of a
# previous Dispatch build or deployment-specific case options.
export TILEXR_VALIDATION_CHECKPOINT=ep-sdedup-combine-p1-abi-host
export TILEXR_VALIDATION_BUILD_MODE=host
export TILEXR_VALIDATION_ACTION=host-validate
source "$(dirname -- "${BASH_SOURCE[0]}")/set_env.sh"
exec "$TILEXR_VALIDATION_PYTHON" -u "$TILEXR_VALIDATION_ROOT/tools/validation/workflow.py" commit-validate
