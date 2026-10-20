#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
: "${ASCEND_HOME_PATH:?source the target CANN environment first}"
: "${TILEXR_PROFILING_PREFIX:?set the installed independent SDK prefix}"
: "${TILEXR_PROFILING_SOC:?set the actual supported target SoC}"
: "${TILEXR_PROFILING_CYCLES_PER_US:?provide the confirmed GetSystemCycle conversion}"
: "${TILEXR_PROFILING_CLOCK_BASIS:?describe the version-matched source or calibration for that conversion}"
if [[ "${TILEXR_PROFILING_SOC}" != "Ascend950" ]]; then
    echo "Add4 currently supports explicit Ascend950 only" >&2
    exit 2
fi
OUTPUT_DIR="${1:-${PWD}/profiled-add4-output-$(date +%Y%m%d-%H%M%S)}"
BUILD_DIR="${TILEXR_ADD4_BUILD_DIR:-${PWD}/build-profiled-add4}"
BUILD_JOBS="${TILEXR_ADD4_BUILD_JOBS:-2}"
export TILEXR_ADD4_DEVICE_ID="${TILEXR_ADD4_DEVICE_ID:-0}"
python3 -c 'import tilexr_profiling' # Use the installed canonical package.
if [[ -e "${OUTPUT_DIR}" ]]; then
    echo "output path already exists: ${OUTPUT_DIR}" >&2
    exit 2
fi
mkdir -p "${OUTPUT_DIR}"
python3 - "${SCRIPT_DIR}" "${OUTPUT_DIR}" "${TILEXR_PROFILING_CYCLES_PER_US}" <<'PY'
import json
import math
import pathlib
import sys
source, output, conversion = sys.argv[1:]
cycles = float(conversion)
if not math.isfinite(cycles) or cycles <= 0:
    raise SystemExit("cycles_per_us must be finite and positive")
mapping = json.loads((pathlib.Path(source) / "profiled_add4.mapping.template.json").read_text())
mapping["cycles_per_us"] = cycles
(pathlib.Path(output) / "capture.mapping.json").write_text(json.dumps(mapping, indent=2) + "\n")
PY
cmake -S "${SCRIPT_DIR}" -B "${BUILD_DIR}" \
    -DCMAKE_PREFIX_PATH="${TILEXR_PROFILING_PREFIX}" \
    -DTILEXR_PROFILING_SOC="${TILEXR_PROFILING_SOC}"
cmake --build "${BUILD_DIR}" --target tilexr_profiled_add4_demo -j"${BUILD_JOBS}"
# Save platform/compiler observations alongside operator output. These generated
# files are runtime evidence and must not be added to the source repository.
{
    printf 'soc=%s\ndevice=%s\n' "${TILEXR_PROFILING_SOC}" "${TILEXR_ADD4_DEVICE_ID}"
    printf 'host_standard=C++14\ndevice_standard=gnu++17\nprofiling_macro=1\n'
    printf 'kernel_version=Add4-v1\nsdk_version=1.0.0\n'
    printf 'cycles_per_us=%s\nclock_basis=%s\n' "${TILEXR_PROFILING_CYCLES_PER_US}" "${TILEXR_PROFILING_CLOCK_BASIS}"
    printf 'CANN=%s\ndriver=%s\nfirmware=%s\n' \
        "${TILEXR_PROFILING_CANN_VERSION:-unknown; inspect active version.info}" \
        "${TILEXR_PROFILING_DRIVER_VERSION:-unknown; inspect npu-smi output}" \
        "${TILEXR_PROFILING_FIRMWARE_VERSION:-unknown; inspect platform inventory}"
    bisheng --version
    if command -v npu-smi >/dev/null 2>&1; then npu-smi info; fi
} > "${OUTPUT_DIR}/environment.txt" 2>&1
"${BUILD_DIR}/bin/tilexr_profiled_add4_demo" "${OUTPUT_DIR}/capture.records.bin" \
    | tee "${OUTPUT_DIR}/operator.log"
python3 -m tilexr_profiling \
    --mapping "${OUTPUT_DIR}/capture.mapping.json" \
    --records "${OUTPUT_DIR}/capture.records.bin" \
    --output-dir "${OUTPUT_DIR}/report" \
    --attribute "soc=${TILEXR_PROFILING_SOC}" \
    --attribute "kernel_version=Add4-v1" --attribute "sdk_version=1.0.0" \
    --attribute "backend=local GM" --attribute "native_status=standalone Ascend C" \
    --attribute "cann_version=${TILEXR_PROFILING_CANN_VERSION:-unknown}" \
    --attribute "driver_version=${TILEXR_PROFILING_DRIVER_VERSION:-unknown}" \
    --attribute "clock_conversion_basis=${TILEXR_PROFILING_CLOCK_BASIS}" \
    --attribute "host_standard=C++14" --attribute "device_standard=gnu++17"
echo "Reports: ${OUTPUT_DIR}/report/{perfetto_trace.json,analysis.md,report.html}"
