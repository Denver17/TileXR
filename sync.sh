#!/usr/bin/env bash
set -eo pipefail
# --debug forwards to the read-only remote SSH/rsync diagnostic workflow.
printf '[sync] Loading environment\n'
unset TILEXR_VALIDATION_CHECKPOINT TILEXR_INTERNAL_ACCEPTANCE_STAGE
export TILEXR_VALIDATION_ACTION=sync
source "$(dirname -- "${BASH_SOURCE[0]}")/set_env.sh"
exec "$TILEXR_VALIDATION_PYTHON" -u "$TILEXR_VALIDATION_ROOT/tools/validation/workflow.py" sync "$@"
