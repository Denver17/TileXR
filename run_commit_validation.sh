#!/usr/bin/env bash
# Compatibility notice only: each stage must be started manually.
cat >&2 <<'EOF'
run_commit_validation.sh is retired. Run these steps separately from the first server:
  bash build_all.sh
  bash sync.sh
  bash run.sh -n 2 -e 96 -m 0 -l 3 0
Then check the cumulative output and collect performance with the same installation:
  bash run.sh -n 2 -e 96 -m 0 -l 4 0
  bash run.sh -n 2 -e 96 -m 1 -l 50 50
See docs/EP_VALIDATION_GUIDE.md. No build, sync, or test has been started.
EOF
exit 2
