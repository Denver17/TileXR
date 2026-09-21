#!/usr/bin/env bash
set -eo pipefail
# --debug forwards to the read-only remote SSH/rsync diagnostic workflow.
printf '[sync] Loading environment\n'
export TILEXR_VALIDATION_ACTION=sync
# Each shell invocation loads a fresh environment. Keep sync aligned with the
# current production package when it follows build_all.sh in a new shell.
export TILEXR_VALIDATION_CHECKPOINT="${TILEXR_VALIDATION_CHECKPOINT:-ep-sdedup-combine-p2-m5}"
source "$(dirname -- "${BASH_SOURCE[0]}")/set_env.sh"
exec "$TILEXR_VALIDATION_PYTHON" -u "$TILEXR_VALIDATION_ROOT/tools/validation/workflow.py" sync "$@"
