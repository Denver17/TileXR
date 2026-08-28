#!/usr/bin/env bash
set -euo pipefail

readonly NPU_COUNT=8
readonly PROBE_TIMEOUT_SECONDS=8

probe_dir=$(mktemp -d "${TMPDIR:-/tmp}/tilexr-ep-npu-probe.XXXXXX")
trap 'rm -rf -- "${probe_dir}"' EXIT

declare -a probe_pids=()
for ((device = 0; device < NPU_COUNT; ++device)); do
    timeout "${PROBE_TIMEOUT_SECONDS}" npu-smi info -t proc-mem -i "${device}" \
        >"${probe_dir}/${device}.out" 2>"${probe_dir}/${device}.err" &
    probe_pids[device]=$!
done

probe_failed=0
for ((device = 0; device < NPU_COUNT; ++device)); do
    if ! wait "${probe_pids[device]}"; then
        printf 'device=%d probe_failed\n' "${device}" >&2
        cat "${probe_dir}/${device}.err" >&2 || true
        probe_failed=1
    fi
done
if ((probe_failed)); then
    printf 'NODE_STATUS unknown host=%s\n' "$(hostname)"
    exit 4
fi

process_count=0
declare -A seen_processes=()
for ((device = 0; device < NPU_COUNT; ++device)); do
    capture="${probe_dir}/${device}.out"
    while IFS= read -r pid; do
        pid=${pid//[[:space:]]/}
        [[ -n "${pid}" ]] || continue
        ((process_count += 1))
        if [[ -n "${seen_processes[${pid}]:-}" ]]; then
            continue
        fi
        seen_processes[${pid}]=1

        name=$(sed -n -E 's/^[[:space:]]*Process name[[:space:]]*:[[:space:]]*(.*)$/\1/p' \
            "${capture}" | head -n 1)
        if [[ -z "${name}" && -r "/proc/${pid}/cmdline" ]]; then
            name=$(tr '\0' ' ' <"/proc/${pid}/cmdline")
        fi
        printf 'NPU_PROCESS pid=%s name=%s\n' "${pid}" "${name:-unknown}"
    done < <(sed -n -E 's/^[[:space:]]*Process id[[:space:]]*:[[:space:]]*([0-9]+).*$/\1/p' "${capture}")
done

if ((process_count == 0)); then
    printf 'NODE_STATUS idle host=%s\n' "$(hostname)"
    exit 0
fi
printf 'NODE_STATUS blocked host=%s process_records=%d\n' \
    "$(hostname)" "${process_count}"
exit 3
