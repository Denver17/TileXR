#!/usr/bin/env bash
# Compatibility notice only: each stage must be started manually.
cat >&2 <<'EOF'
run_commit_validation.sh is retired. Run these steps separately from the first server:
  bash build_all.sh
  bash sync.sh
  bash run.sh run_mode=functional loops=3 warmup=0
Then check the cumulative output and collect performance with the same installation:
  bash run.sh run_mode=functional loops=4 warmup=0
  bash run.sh run_mode=performance loops=50 warmup=50
See docs/EP_VALIDATION_GUIDE.md. No build, sync, or test has been started.
EOF
exit 2
