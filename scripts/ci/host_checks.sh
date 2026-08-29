#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/../.." && pwd -P)"
REPO_BUILD_PARENT="${ROOT_DIR}/.ci-build"

build_root_error() {
    echo "ERROR: refusing unsafe TILEXR_CI_BUILD_ROOT: $1" >&2
    exit 2
}

if [[ "${TILEXR_CI_BUILD_ROOT+x}" == "x" ]]; then
    if [[ -z "${TILEXR_CI_BUILD_ROOT}" || "${TILEXR_CI_BUILD_ROOT}" != /* ]]; then
        build_root_error "${TILEXR_CI_BUILD_ROOT:-<empty>}"
    fi
    BUILD_ROOT_INPUT="${TILEXR_CI_BUILD_ROOT}"
else
    BUILD_ROOT_INPUT="${REPO_BUILD_PARENT}/tilexr-host-default"
fi

BUILD_ROOT="$(
    python3 -c 'import os, sys; print(os.path.normpath(sys.argv[1]))' \
        "${BUILD_ROOT_INPUT}"
)"
BUILD_PARENT="$(dirname -- "${BUILD_ROOT}")"
BUILD_BASENAME="$(basename -- "${BUILD_ROOT}")"
if [[ ! "${BUILD_BASENAME}" =~ ^\.?tilexr-host-.+ ]]; then
    build_root_error "${BUILD_ROOT}"
fi

if [[ "${BUILD_PARENT}" == "${REPO_BUILD_PARENT}" ]]; then
    if [[ -L "${REPO_BUILD_PARENT}" || \
          ( -e "${REPO_BUILD_PARENT}" && ! -d "${REPO_BUILD_PARENT}" ) ]]; then
        build_root_error "${BUILD_ROOT} (invalid repository build parent)"
    fi
    mkdir -p -- "${REPO_BUILD_PARENT}"
    BUILD_PARENT_REAL="$(cd "${REPO_BUILD_PARENT}" && pwd -P)"
    if [[ "${BUILD_PARENT_REAL}" != "${REPO_BUILD_PARENT}" ]]; then
        build_root_error "${BUILD_ROOT} (repository build parent is redirected)"
    fi
else
    RUNNER_BUILD_PARENT="${RUNNER_TEMP:-/tmp}"
    if [[ -z "${RUNNER_BUILD_PARENT}" || "${RUNNER_BUILD_PARENT}" != /* ]]; then
        build_root_error "${BUILD_ROOT} (invalid RUNNER_TEMP)"
    fi
    RUNNER_BUILD_PARENT="$(
        python3 -c 'import os, sys; print(os.path.normpath(sys.argv[1]))' \
            "${RUNNER_BUILD_PARENT}"
    )"
    if [[ "${BUILD_PARENT}" != "${RUNNER_BUILD_PARENT}" || \
          -L "${RUNNER_BUILD_PARENT}" || \
          ! -d "${RUNNER_BUILD_PARENT}" ]]; then
        build_root_error "${BUILD_ROOT} (outside an allowed build parent)"
    fi
    BUILD_PARENT_REAL="$(cd "${RUNNER_BUILD_PARENT}" && pwd -P)"
    if [[ "${BUILD_PARENT_REAL}" != "${RUNNER_BUILD_PARENT}" ]]; then
        build_root_error "${BUILD_ROOT} (runner build parent is redirected)"
    fi
fi

BUILD_ROOT="${BUILD_PARENT_REAL}/${BUILD_BASENAME}"
if [[ -L "${BUILD_ROOT}" ]]; then
    build_root_error "${BUILD_ROOT} (build root is a symbolic link)"
fi
rm -rf -- "${BUILD_ROOT}"
mkdir -p "${BUILD_ROOT}"

if ! command -v nproc >/dev/null 2>&1; then
    nproc() {
        if command -v getconf >/dev/null 2>&1; then
            getconf _NPROCESSORS_ONLN
        else
            sysctl -n hw.ncpu
        fi
    }
fi

CASE_NAMES=()
CASE_RESULTS=()
HOST_STARTED="$(date +%s)"
overall_status=0

run_case() {
    local name="$1"
    shift
    local started finished duration status result restore_errexit
    restore_errexit=0
    if [[ "$-" == *e* ]]; then
        restore_errexit=1
    fi
    started="$(date +%s)"
    set +e
    (set -e; "$@")
    status="$?"
    if [[ "${restore_errexit}" -eq 1 ]]; then
        set -e
    else
        set +e
    fi
    finished="$(date +%s)"
    duration=$((finished - started))
    result=PASS
    if [[ "${status}" -ne 0 ]]; then
        result=FAIL
    fi
    CASE_NAMES+=("${name}")
    CASE_RESULTS+=("${result}")
    printf '[host-check] %s: %s (exit=%s, duration=%ss)\n' \
        "${name}" "${result}" "${status}" "${duration}"
    return "${status}"
}

run_and_accumulate_case() {
    local case_status
    set +e
    run_case "$@"
    case_status="$?"
    set -e
    if [[ "${case_status}" -ne 0 ]]; then
        overall_status=1
    fi
    return 0
}

check_tracked_shell_syntax() {
    local relative
    local -a tracked_shell_files=()
    while IFS= read -r -d '' relative; do
        if [[ "${relative}" == *.sh ]]; then
            tracked_shell_files+=("${ROOT_DIR}/${relative}")
        fi
    done < <(
        git -C "${ROOT_DIR}" ls-files -z -- \
            scripts/ci \
            tests/comm \
            tests/ep \
            tests/memory \
            tests/sdma \
            tests/udma \
            tests/collectives
    )
    if [[ "${#tracked_shell_files[@]}" -eq 0 ]]; then
        echo "ERROR: tracked shell syntax scan found no scripts" >&2
        return 1
    fi
    bash -n "${tracked_shell_files[@]}"
}

run_ci_ctest() {
    cmake -S "${ROOT_DIR}/tests/ci" -B "${BUILD_ROOT}/ci"
    (cd "${BUILD_ROOT}/ci" && ctest --output-on-failure)
}

run_comm_host_tests() {
    cmake -S "${ROOT_DIR}/tests/comm" -B "${BUILD_ROOT}/comm" \
        -DCMAKE_INSTALL_PREFIX="${BUILD_ROOT}/comm-install"
    cmake --build "${BUILD_ROOT}/comm" --target install -j"$(nproc)"
    "${BUILD_ROOT}/comm-install/bin/test_tilexr_log"
    "${BUILD_ROOT}/comm-install/bin/test_tilexr_log_spdlog_compile"
}

run_ep_host_tests() {
    cmake -S "${ROOT_DIR}/tests/ep" -B "${BUILD_ROOT}/ep" \
        -DBUILD_TILEXR_EP_DEMO=OFF
    cmake --build "${BUILD_ROOT}/ep" -j"$(nproc)"
    (cd "${BUILD_ROOT}/ep" && ctest --output-on-failure)
}

run_data_as_flag_tests() {
    cmake -S "${ROOT_DIR}/tests/data_as_flag" -B "${BUILD_ROOT}/data-as-flag"
    cmake --build "${BUILD_ROOT}/data-as-flag" -j"$(nproc)"
    (cd "${BUILD_ROOT}/data-as-flag" && ctest --output-on-failure)
}

render_summary() {
    local finished duration passed failed failed_names index
    finished="$(date +%s)"
    duration=$((finished - HOST_STARTED))
    passed=0
    failed=0
    failed_names=
    for index in "${!CASE_NAMES[@]}"; do
        if [[ "${CASE_RESULTS[index]}" == PASS ]]; then
            passed=$((passed + 1))
        else
            failed=$((failed + 1))
            if [[ -n "${failed_names}" ]]; then
                failed_names+=", "
            fi
            failed_names+="${CASE_NAMES[index]}"
        fi
    done
    failed_names="${failed_names:-none}"

    printf '# TileXR Host Checks\n\n'
    printf -- '- PR: `%s`\n' "${TILEXR_CI_PR_NUMBER:-local}"
    printf -- '- Head SHA: `%s`\n' "${TILEXR_CI_HEAD_SHA:-local}"
    printf -- '- Base SHA: `%s`\n' "${TILEXR_CI_BASE_SHA:-local}"
    printf -- '- Merge SHA: `%s`\n' "${TILEXR_CI_EXPECTED_MERGE_SHA:-local}"
    printf -- '- Host cases: %s total, %s passed, %s failed\n' \
        "${#CASE_NAMES[@]}" "${passed}" "${failed}"
    printf -- '- Failed cases: %s\n' "${failed_names}"
    printf -- '- Duration: %s seconds\n' "${duration}"
    printf -- '- NPU-only coverage is not run by Host Checks; it runs in the queued NPU Gate on blue.\n'
}

finalize_host_checks() {
    local status="$?"
    local summary
    trap - EXIT
    set +e
    if [[ "${overall_status}" -ne 0 ]]; then
        status=1
    fi
    summary="$(render_summary)"
    printf '%s\n' "${summary}"
    if [[ -n "${GITHUB_STEP_SUMMARY:-}" ]]; then
        printf '%s\n' "${summary}" >> "${GITHUB_STEP_SUMMARY}"
        if [[ "$?" -ne 0 && "${status}" -eq 0 ]]; then
            status=1
        fi
    fi
    exit "${status}"
}

trap finalize_host_checks EXIT

run_and_accumulate_case shell-syntax check_tracked_shell_syntax
run_and_accumulate_case ci-ctest run_ci_ctest
run_and_accumulate_case comm-host run_comm_host_tests
run_and_accumulate_case ep-host run_ep_host_tests
run_and_accumulate_case data-as-flag run_data_as_flag_tests
run_and_accumulate_case collectives-vllm-patch \
    python3 -m pytest -q "${ROOT_DIR}/tests/collectives/unit/test_vllm_collectives_patch.py"
run_and_accumulate_case collectives-profile-report \
    python3 "${ROOT_DIR}/tests/collectives/unit/test_collective_profile_report.py"

exit "${overall_status}"
