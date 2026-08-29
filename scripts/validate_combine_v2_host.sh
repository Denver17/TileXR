#!/usr/bin/env bash
set -euo pipefail

readonly script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
readonly source_dir=$(cd "${script_dir}/.." && pwd)
readonly cmake_bin=${TILEXR_CMAKE_BIN:-cmake}
readonly build_dir=${TILEXR_COMBINE_V2_HOST_BUILD_DIR:-"${source_dir}/build-combine-v2-host"}

if [[ "${cmake_bin}" == */* ]]; then
    readonly cmake_root=$(cd "$(dirname "${cmake_bin}")/.." && pwd)
    if [[ -d "${cmake_root}/lib" ]]; then
        export LD_LIBRARY_PATH="${cmake_root}/lib:${LD_LIBRARY_PATH:-}"
    fi
fi

"${cmake_bin}" -S "${source_dir}/tests/moonep_combine_v2" -B "${build_dir}"
"${cmake_bin}" --build "${build_dir}" -j"${TILEXR_BUILD_JOBS:-16}"

readonly tests=(
    test_tilexr_moonep_combine_v2_schedule
    test_tilexr_moonep_combine_v2_layout
    test_tilexr_moonep_combine_v2_host
    test_tilexr_moonep_combine_v2_launch
    test_tilexr_moonep_combine_v2_public_abi
)

for test_name in "${tests[@]}"; do
    "${build_dir}/${test_name}"
done

printf 'Combine V2 Host validation passed (%s tests)\n' "${#tests[@]}"
