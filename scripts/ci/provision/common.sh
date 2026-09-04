#!/usr/bin/env bash

set -euo pipefail

CI_USER=tilexr-ci
CI_PRIMARY_GROUP=tilexr-ci
CI_GROUP=HwHiAiUser
CI_HOME=/home/tilexr-ci
CANN_HOME=/home/tilexr-ci/toolchains/cann/9.1.0
CANN_OWNER=root
CONTROL_VERSION=v6
CONTROL_HOME="${CI_HOME}/control/${CONTROL_VERSION}"
RUNNER_HOME=/home/tilexr-ci/actions-runner
GITHUB_PROXY=http://127.0.0.1:3128
RUNNER_NO_PROXY=localhost,127.0.0.1

DRY_RUN=0

parse_args() {
    if [[ "$#" -eq 0 ]]; then
        DRY_RUN=0
    elif [[ "$#" -eq 1 && "$1" == "--dry-run" ]]; then
        DRY_RUN=1
    else
        echo "Usage: $0 [--dry-run]" >&2
        return 2
    fi
}

run() {
    if [[ "${DRY_RUN}" == 1 ]]; then
        printf '%q ' "$@"
        printf '\n'
    else
        "$@"
    fi
}

require_root() {
    if [[ "${DRY_RUN}" != 1 && "$(id -u)" -ne 0 ]]; then
        echo "ERROR: provisioning must run as root" >&2
        return 1
    fi
}

version_at_least() {
    local actual="$1"
    local minimum="$2"
    local actual_major actual_minor actual_stage actual_number
    local minimum_major minimum_minor minimum_stage minimum_number

    [[ "${actual}" =~ ^([0-9]+)\.([0-9]+)\.((rc)([0-9]+)(\.b[0-9]+)?|([0-9]+))$ ]] || return 1
    actual_major=$((10#${BASH_REMATCH[1]}))
    actual_minor=$((10#${BASH_REMATCH[2]}))
    if [[ "${BASH_REMATCH[4]}" == rc ]]; then
        actual_stage=0
        actual_number=$((10#${BASH_REMATCH[5]}))
    else
        actual_stage=1
        actual_number=$((10#${BASH_REMATCH[7]}))
    fi
    [[ "${minimum}" =~ ^([0-9]+)\.([0-9]+)\.((rc)([0-9]+)(\.b[0-9]+)?|([0-9]+))$ ]] || return 1
    minimum_major=$((10#${BASH_REMATCH[1]}))
    minimum_minor=$((10#${BASH_REMATCH[2]}))
    if [[ "${BASH_REMATCH[4]}" == rc ]]; then
        minimum_stage=0
        minimum_number=$((10#${BASH_REMATCH[5]}))
    else
        minimum_stage=1
        minimum_number=$((10#${BASH_REMATCH[7]}))
    fi

    (( actual_major > minimum_major ||
       (actual_major == minimum_major && actual_minor > minimum_minor) ||
       (actual_major == minimum_major && actual_minor == minimum_minor &&
        (actual_stage > minimum_stage ||
         (actual_stage == minimum_stage && actual_number >= minimum_number))) ))
}

ci_primary_group_has_non_root_gid() {
    local entry group_name password group_gid members
    entry="$(getent group "${CI_PRIMARY_GROUP}")" || return 1
    IFS=: read -r group_name password group_gid members <<< "${entry}"
    [[ "${group_name}" == "${CI_PRIMARY_GROUP}" &&
       "${group_gid}" =~ ^[0-9]+$ && "${group_gid}" -gt 0 ]]
}

ci_user_ids_are_non_root() {
    local user="$1"
    local uid gid
    uid="$(id -u "${user}")" || return 1
    gid="$(id -g "${user}")" || return 1
    [[ "${uid}" =~ ^[0-9]+$ && "${gid}" =~ ^[0-9]+$ &&
       "${uid}" -gt 0 && "${gid}" -gt 0 ]]
}

ci_identity_is_bounded() {
    local user="$1"
    local primary_group primary_gid group_entry group_name password group_gid members
    local groups actual_supplementary expected_supplementary

    ci_user_ids_are_non_root "${user}" || return 1
    ci_primary_group_has_non_root_gid || return 1
    primary_group="$(id -gn "${user}")" || return 1
    primary_gid="$(id -g "${user}")" || return 1
    group_entry="$(getent group "${CI_PRIMARY_GROUP}")" || return 1
    IFS=: read -r group_name password group_gid members <<< "${group_entry}"
    [[ "${primary_group}" == "${CI_PRIMARY_GROUP}" &&
       "${primary_gid}" == "${group_gid}" ]] || return 1

    groups="$(id -nG "${user}")" || return 1
    actual_supplementary="$(
        tr ' ' '\n' <<< "${groups}" |
            awk -v primary="${CI_PRIMARY_GROUP}" 'NF && $0 != primary' |
            LC_ALL=C sort -u
    )"
    expected_supplementary="$(printf '%s\n' "${CI_GROUP}" | LC_ALL=C sort -u)"
    [[ "${actual_supplementary}" == "${expected_supplementary}" ]]
}

npu_smi_info_has_expected_devices() {
    local info="$1"
    awk '
        function trim(value) {
            sub(/^[[:space:]]+/, "", value)
            sub(/[[:space:]]+$/, "", value)
            return value
        }

        /^\|[[:space:]]*NPU[[:space:]]+Name[[:space:]]*\|[[:space:]]*Health/ {
            in_inventory = 1
            found_header = 1
            next
        }
        /^\|[[:space:]]*NPU[[:space:]]+Chip[[:space:]]*\|[[:space:]]*Process id/ {
            in_inventory = 0
            next
        }
        in_inventory && /^\|/ {
            cell_count = split($0, cells, "|")
            if (cell_count < 4) {
                next
            }
            identity = trim(cells[2])
            field_count = split(identity, fields, /[[:space:]]+/)
            if (field_count != 2 || fields[1] !~ /^[0-9]+$/) {
                next
            }
            device = fields[1] + 0
            name = fields[2]
            health = trim(cells[3])
            if (device < 0 || device > 7 || name != "910B3" || health != "OK" || seen[device]) {
                bad = 1
                next
            }
            seen[device] = 1
            count++
        }
        END {
            if (!found_header || bad || count != 8) {
                exit 1
            }
            for (device = 0; device < 8; device++) {
                if (!seen[device]) {
                    exit 1
                }
            }
        }
    ' <<< "${info}"
}

run_with_runner_registration_token() {
    local token="$1"
    local status
    shift

    unset registration_token
    export ACTIONS_RUNNER_INPUT_TOKEN="${token}"
    unset token
    if "$@"; then
        status=0
    else
        status=$?
    fi
    unset ACTIONS_RUNNER_INPUT_TOKEN
    return "${status}"
}

cann_metadata_matches() {
    local path="$1"
    local package="$2"
    local package_count version_count

    [[ -f "${path}" && ! -L "${path}" ]] || return 1
    package_count="$(grep -c '^package_name=' "${path}" || true)"
    version_count="$(grep -c '^version=' "${path}" || true)"
    [[ "${package_count}" -eq 1 && "${version_count}" -eq 1 ]] || return 1
    grep -Fx "package_name=${package}" "${path}" >/dev/null &&
        grep -Fx 'version=9.1.0' "${path}" >/dev/null
}

cann_realpath_is_contained() {
    local path="$1"
    local root_real path_real
    root_real="$(realpath "${CANN_HOME}")" || return 1
    path_real="$(realpath "${path}")" || return 1
    [[ "${path_real}" == "${root_real}" || "${path_real}" == "${root_real}/"* ]]
}

cann_tree_links_are_contained() {
    local link
    while IFS= read -r -d '' link; do
        cann_realpath_is_contained "${link}" || return 1
    done < <(find -P "${CANN_HOME}" -type l -print0)
}

cann_tree_has_expected_payload() {
    local toolkit_info="${CANN_HOME}/cann/aarch64-linux/ascend_toolkit_install.info"
    local ops_info="${CANN_HOME}/cann/aarch64-linux/ascend_ops_install.info"
    local compiler="${CANN_HOME}/cann/tools/bisheng_compiler/bin/bisheng"

    [[ -d "${CANN_HOME}" && ! -L "${CANN_HOME}" ]] || return 1
    cann_metadata_matches "${toolkit_info}" Ascend-cann-toolkit || return 1
    cann_metadata_matches "${ops_info}" Ascend-cann-910b-ops || return 1
    [[ -x "${compiler}" ]] || return 1
    cann_realpath_is_contained "${toolkit_info}" || return 1
    cann_realpath_is_contained "${ops_info}" || return 1
    cann_realpath_is_contained "${compiler}" || return 1
    cann_tree_links_are_contained
}

cann_entry_stat() {
    local path="$1"
    local entry owner group raw_mode mode

    if entry="$(stat -c '%U:%G:%a' "${path}" 2>/dev/null)"; then
        printf '%s\n' "${entry}"
        return 0
    fi
    entry="$(stat -f '%Su:%Sg:%p' "${path}")" || return 1
    IFS=: read -r owner group raw_mode <<< "${entry}"
    [[ "${raw_mode}" =~ ^[0-7]+$ ]] || return 1
    mode="$(printf '%o' "$((8#${raw_mode} & 8#7777))")"
    printf '%s:%s:%s\n' "${owner}" "${group}" "${mode}"
}

managed_directory_matches() {
    local path="$1"
    local expected_owner="$2"
    local expected_group="$3"
    local expected_mode="$4"
    local entry owner group mode

    [[ -d "${path}" && ! -L "${path}" ]] || return 1
    entry="$(cann_entry_stat "${path}")" || return 1
    IFS=: read -r owner group mode <<< "${entry}"
    [[ "${owner}" == "${expected_owner}" &&
       "${group}" == "${expected_group}" &&
       "${mode}" == "${expected_mode}" ]]
}

cann_parent_directories_are_real() {
    local path
    for path in \
        "${CI_HOME}" \
        "${CI_HOME}/toolchains" \
        "${CI_HOME}/toolchains/cann"; do
        [[ -d "${path}" && ! -L "${path}" ]] || return 1
    done
}

cann_parent_directories_are_sealed() {
    managed_directory_matches \
        "${CI_HOME}" "${CANN_OWNER}" "${CI_PRIMARY_GROUP}" 755 &&
        managed_directory_matches \
            "${CI_HOME}/toolchains" "${CANN_OWNER}" "${CI_GROUP}" 755 &&
        managed_directory_matches \
            "${CI_HOME}/toolchains/cann" "${CANN_OWNER}" "${CI_GROUP}" 755
}

cann_tree_regular_entries_are_sealed() {
    local path entry owner group mode group_bits other_bits
    while IFS= read -r -d '' path; do
        entry="$(cann_entry_stat "${path}")" || return 1
        IFS=: read -r owner group mode <<< "${entry}"
        [[ "${owner}" == "${CANN_OWNER}" && "${group}" == "${CI_GROUP}" ]] || return 1
        [[ "${mode}" =~ ^([0-7])([0-7])([0-7])$ ]] || return 1
        group_bits="${BASH_REMATCH[2]}"
        other_bits="${BASH_REMATCH[3]}"
        if [[ -d "${path}" ]]; then
            [[ "${group_bits}" == 5 && "${other_bits}" == 5 ]] || return 1
        else
            [[ "${group_bits}" =~ ^[45]$ && "${other_bits}" == "${group_bits}" ]] || return 1
            if [[ -x "${path}" && "${group_bits}" != 5 ]]; then
                return 1
            fi
        fi
    done < <(find -P "${CANN_HOME}" \( -type f -o -type d \) -print0)
}

cann_tree_symlinks_are_owned() {
    local link entry owner group mode
    while IFS= read -r -d '' link; do
        entry="$(cann_entry_stat "${link}")" || return 1
        IFS=: read -r owner group mode <<< "${entry}"
        [[ "${owner}" == "${CANN_OWNER}" && "${group}" == "${CI_GROUP}" ]] || return 1
    done < <(find -P "${CANN_HOME}" -type l -print0)
}

cann_tree_is_trusted() {
    cann_tree_has_expected_payload &&
        cann_tree_regular_entries_are_sealed &&
        cann_tree_symlinks_are_owned
}

sudo_user_has_allowed_rule() {
    local user="$1"
    local output status

    if output="$(env LC_ALL=C LANG=C sudo -n -l -U "${user}" 2>&1)"; then
        status=0
    else
        status=$?
    fi

    if [[ "${status}" -eq 0 ]] &&
        grep -Eq '^User .+ may run the following commands on .+:$' <<< "${output}"; then
        return 0
    fi
    if grep -Eq '^User .+ is not allowed to run sudo on .+[.]?$' <<< "${output}"; then
        return 1
    fi

    echo "ERROR: could not determine sudo permissions for ${user}" >&2
    printf '%s\n' "${output}" >&2
    return 2
}

runner_service_matches() {
    local service_name="$1"
    local output status user_count exec_count user_line exec_line
    local expected_exec="${RUNNER_HOME}/runsvc.sh"

    if [[ ! "${service_name}" =~ ^actions\.runner\.[A-Za-z0-9_.-]+\.service$ ]]; then
        return 1
    fi
    if output="$(env LC_ALL=C LANG=C systemctl show \
        --property=User --property=ExecStart -- "${service_name}" 2>&1)"; then
        status=0
    else
        status=$?
    fi
    if [[ "${status}" -ne 0 ]]; then
        echo "ERROR: could not inspect runner service ${service_name}" >&2
        printf '%s\n' "${output}" >&2
        return 1
    fi

    user_count="$(awk '/^User=/{count++} END{print count+0}' <<< "${output}")"
    exec_count="$(awk '/^ExecStart=/{count++} END{print count+0}' <<< "${output}")"
    [[ "${user_count}" -eq 1 && "${exec_count}" -eq 1 ]] || return 1
    user_line="$(awk '/^User=/{print}' <<< "${output}")"
    exec_line="$(awk '/^ExecStart=/{print}' <<< "${output}")"
    [[ "${user_line}" == "User=${CI_USER}" ]] || return 1
    [[ "${exec_line}" == \
        "ExecStart={ path=${expected_exec} ; argv[]=${expected_exec} ;"* ]]
}

remove_ci_ssh_entry() {
    run rm -rf -- "${CI_HOME}/.ssh"
}

seal_runner_modes() {
    run chmod 0750 "${RUNNER_HOME}"
    run find "${RUNNER_HOME}" -mindepth 1 -maxdepth 1 \
        ! -name _work ! -name _diag \
        -exec chmod -R u+rwX,g+rX,o-rwx,go-w '{}' +
    run chmod 0440 "${RUNNER_HOME}/.env"
    run chmod -R u+rwX,g+rX,o-rwx,go-w \
        "${RUNNER_HOME}/_work" "${RUNNER_HOME}/_diag"
}

if [[ "${BASH_SOURCE[0]}" == "$0" ]]; then
    parse_args "$@"
fi
