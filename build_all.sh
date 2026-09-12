#!/usr/bin/env bash
set -eo pipefail
printf '[build] Loading environment\n'
source "$(dirname -- "${BASH_SOURCE[0]}")/set_env.sh"
# Production builds select the current increment; sync/run inherit its manifest.
if [[ "$TILEXR_VALIDATION_BUILD_MODE" == production ]]; then
    export TILEXR_VALIDATION_CHECKPOINT="${TILEXR_VALIDATION_CHECKPOINT:-ep-sdedup-p1-chain-multi}"
fi
exec "$TILEXR_VALIDATION_PYTHON" -u "$TILEXR_VALIDATION_ROOT/tools/validation/workflow.py" build "$@"
