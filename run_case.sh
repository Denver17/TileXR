#!/usr/bin/env bash
# Compatibility entry; parameter parsing and execution are owned by run.sh.
exec bash "$(dirname -- "${BASH_SOURCE[0]}")/run.sh" "$@"
