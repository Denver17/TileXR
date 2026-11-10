#!/usr/bin/env bash
# 构建或启动 Combine V2 性能探针，向 tools/moonep 和 tests/moonep_combine_v2 传递运行参数。
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SOURCE_DIR="$(cd "${SCRIPT_DIR}/../.." && pwd)"
BUILD_DIR="${SOURCE_DIR}/build-combine-v2-perf"
INSTALL_DIR="${SOURCE_DIR}/install-combine-v2-perf"
CANN_PATH="${ASCEND_HOME_PATH:-/usr/local/Ascend/ascend-toolkit/latest}"
JOBS="$(nproc)"

usage() {
    cat <<'EOF'
Usage: bash tools/moonep/build_combine_v2_perf.sh [options]

Options:
  --source-dir PATH    TileXR source directory
  --build-dir PATH     CMake build directory
  --install-dir PATH   Staged runtime directory
  --cann-path PATH     CANN root containing aarch64-linux
  --jobs N             Parallel build jobs (default: nproc)
  --help               Show this help
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --source-dir) SOURCE_DIR="$2"; shift 2 ;;
        --build-dir) BUILD_DIR="$2"; shift 2 ;;
        --install-dir) INSTALL_DIR="$2"; shift 2 ;;
        --cann-path) CANN_PATH="$2"; shift 2 ;;
        --jobs) JOBS="$2"; shift 2 ;;
        --help|-h) usage; exit 0 ;;
        *) echo "unknown argument: $1" >&2; usage >&2; exit 2 ;;
    esac
done

if [[ ! "${JOBS}" =~ ^[1-9][0-9]*$ ]]; then
    echo "--jobs must be a positive integer" >&2
    exit 2
fi
if [[ ! -f "${SOURCE_DIR}/CMakeLists.txt" ]]; then
    echo "TileXR source not found: ${SOURCE_DIR}" >&2
    exit 1
fi
if [[ ! -d "${CANN_PATH}/aarch64-linux" ]]; then
    echo "CANN aarch64-linux directory not found: ${CANN_PATH}" >&2
    exit 1
fi
mkdir -p "${BUILD_DIR}" "${INSTALL_DIR}/bin" "${INSTALL_DIR}/lib64"
export ASCEND_HOME_PATH="${CANN_PATH}"
export ASCEND_DRIVER_PATH="${ASCEND_DRIVER_PATH:-/usr/local/Ascend/driver}"

cmake -S "${SOURCE_DIR}" -B "${BUILD_DIR}" \
    -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_TESTING=ON \
    -DTILEXR_BUILD_TESTS=OFF \
    -DTILEXR_BUILD_MOONEP=ON \
    -DTILEXR_BUILD_MOONEP_PLANNER=OFF \
    -DCMAKE_INSTALL_PREFIX="${INSTALL_DIR}"
cmake --build "${BUILD_DIR}" --target tilexr_moonep_combine_v2_perf \
    --parallel "${JOBS}"

benchmark="${BUILD_DIR}/tests/moonep_combine_v2/tilexr_moonep_combine_v2_perf"
comm_library="${BUILD_DIR}/src/comm/libtile-comm.so"
combine_dir="${BUILD_DIR}/src/moonep/combine_v2"
if [[ ! -x "${benchmark}" || ! -f "${comm_library}" ]]; then
    echo "expected benchmark artifacts were not built" >&2
    exit 1
fi
if ! compgen -G "${combine_dir}/libtilexr-moonep-combine-v2.so*" >/dev/null; then
    echo "Combine V2 library was not built under ${combine_dir}" >&2
    exit 1
fi

rm -f "${INSTALL_DIR}/lib64/libtile-comm.so" \
    "${INSTALL_DIR}/lib64"/libtilexr-moonep-combine-v2.so*
install -m 0755 "${benchmark}" \
    "${INSTALL_DIR}/bin/tilexr_moonep_combine_v2_perf"
install -m 0755 "${comm_library}" "${INSTALL_DIR}/lib64/libtile-comm.so"
cp -a "${combine_dir}"/libtilexr-moonep-combine-v2.so* \
    "${INSTALL_DIR}/lib64/"

runtime_ld_path="${INSTALL_DIR}/lib64:${CANN_PATH}/aarch64-linux/lib64:${CANN_PATH}/lib64:${ASCEND_DRIVER_PATH}/lib64:${ASCEND_DRIVER_PATH}/lib64/common:${ASCEND_DRIVER_PATH}/lib64/driver"
benchmark_ldd="$(LD_LIBRARY_PATH="${runtime_ld_path}:${LD_LIBRARY_PATH:-}" \
    ldd "${INSTALL_DIR}/bin/tilexr_moonep_combine_v2_perf")"
if grep -q 'not found' <<<"${benchmark_ldd}"; then
    printf '%s\n' "${benchmark_ldd}" >&2
    exit 1
fi
if grep -Eiq 'libmpi|libmpicxx' <<<"${benchmark_ldd}"; then
    echo "benchmark unexpectedly depends on MPI" >&2
    printf '%s\n' "${benchmark_ldd}" >&2
    exit 1
fi
if readelf -d "${INSTALL_DIR}/bin/tilexr_moonep_combine_v2_perf" | \
    grep -Fq "${BUILD_DIR}"; then
    echo "benchmark RPATH still references the build directory" >&2
    exit 1
fi
staged_library_dir="$(readlink -f "${INSTALL_DIR}/lib64")"
for library in libtile-comm.so libtilexr-moonep-combine-v2.so; do
    resolved_library="$(awk -v prefix="${library}" \
        'index($1, prefix) == 1 { print $3; exit }' <<<"${benchmark_ldd}")"
    if [[ -z "${resolved_library}" ||
          "$(readlink -f "${resolved_library}")" != "${staged_library_dir}"/* ]]; then
        echo "${library} was not resolved from the staged runtime" >&2
        printf '%s\n' "${benchmark_ldd}" >&2
        exit 1
    fi
done

echo "Combine V2 benchmark staged at ${INSTALL_DIR}"
(cd "${INSTALL_DIR}" && find bin lib64 -type f -print0 | sort -z | \
    xargs -0 sha256sum)
