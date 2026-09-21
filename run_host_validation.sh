#!/usr/bin/env bash
set -eo pipefail
case "$#:${1:-}" in
    0:) ;;
    1:-h|1:--help)
        printf 'Usage: bash run_host_validation.sh\nRun installed Host/Python tests locally, without SSH or NPU access.\nBuild first with bash build_all.sh. Reports: run/validation/host-*/summary.json\n'
        exit 0 ;;
    *) printf '[host-validate] Failed: no arguments expected (use --help)\n' >&2; exit 2 ;;
esac
unset TILEXR_VALIDATION_CHECKPOINT TILEXR_INTERNAL_ACCEPTANCE_STAGE
export TILEXR_VALIDATION_ACTION=host-validate
source "$(dirname -- "${BASH_SOURCE[0]}")/set_env.sh"
exec "$TILEXR_VALIDATION_PYTHON" -u "$TILEXR_VALIDATION_ROOT/tools/validation/workflow.py" host-validate
