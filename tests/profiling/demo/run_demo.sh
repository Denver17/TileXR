#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/../../.." && pwd)"
BUILD_DIR="${TILEXR_ADD4_BUILD_DIR:-${ROOT_DIR}/build-profiled-add4}"
OUTPUT_DIR="${1:-${ROOT_DIR}/profiled-add4-output-$(date +%Y%m%d-%H%M%S)}"
REQUIRE_IDLE_DEVICE="${2:-0}"
BUILD_JOBS="${TILEXR_ADD4_BUILD_JOBS:-2}"

if [[ -z "${ASCEND_HOME_PATH:-}" ]]; then
    source "${ROOT_DIR}/scripts/common_env.sh"
fi
if [[ -e "${OUTPUT_DIR}" ]]; then
    echo "output path already exists: ${OUTPUT_DIR}" >&2
    exit 2
fi

mkdir -p "${OUTPUT_DIR}"
cp "${SCRIPT_DIR}/profiled_add4.mapping.json" "${OUTPUT_DIR}/capture.mapping.json"
cmake -S "${ROOT_DIR}" -B "${BUILD_DIR}" \
    -DTILEXR_BUILD_PROFILING_DEMO=ON \
    -DTILEXR_PROFILING_DEMO_SOC_TYPE=Ascend950
cmake --build "${BUILD_DIR}" --target tilexr_profiled_add4_demo -j"${BUILD_JOBS}"

export TILEXR_ADD4_DEVICE_ID="${TILEXR_ADD4_DEVICE_ID:-0}"
export TILEXR_ENABLE_IPC=0
export TILEXR_ENABLE_UDMA=0
export TILEXR_ENABLE_SDMA=0
export TILEXR_PROFILING_CAPTURE_ID=add40000-0000-4000-8000-000000000001
export TILEXR_PROFILING_RECORDS_PATH="${OUTPUT_DIR}/capture.records.bin"
if [[ "${REQUIRE_IDLE_DEVICE}" == "1" ]]; then
    NPU_STATUS="$(npu-smi info)"
    if ! grep -Fq "No running processes found in NPU ${TILEXR_ADD4_DEVICE_ID}" \
        <<< "${NPU_STATUS}"; then
        echo "device ${TILEXR_ADD4_DEVICE_ID} is not idle; refusing to launch" >&2
        exit 3
    fi
fi
"${BUILD_DIR}/tests/profiling/tilexr_profiled_add4_demo"

PYTHONPATH="${ROOT_DIR}${PYTHONPATH:+:${PYTHONPATH}}" python3 -m tools.profiling \
    --mapping "${OUTPUT_DIR}/capture.mapping.json" \
    --records "${OUTPUT_DIR}/capture.records.bin" \
    --output-dir "${OUTPUT_DIR}/report"

echo "Generated profiling artifacts:"
echo "  ${OUTPUT_DIR}/report/perfetto_trace.json"
echo "  ${OUTPUT_DIR}/report/analysis.md"
echo "  ${OUTPUT_DIR}/report/report.html"
