#!/usr/bin/env bash
# 从当前示例目录构建 Add4，调用 manifest.py 声明参与者并交给 tilexr_profiling 生成离线报告。
# Build Add4 from this directory, declare participants with manifest.py and call tilexr_profiling for offline reports.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
: "${TILEXR_PROFILING_PREFIX:?set the installed SDK prefix}"
: "${TILEXR_PROFILING_CYCLES_PER_US:?set a verified GetSystemCycle conversion}"
: "${TILEXR_PROFILING_CLOCK_BASIS:?describe the source of that conversion}"
OUTPUT_DIR="${1:?usage: run_demo.sh NEW_OUTPUT_DIRECTORY}"
BUILD_DIR="${TILEXR_ADD4_BUILD_DIR:-${PWD}/build-profiled-add4}"
mkdir "${OUTPUT_DIR}"
cmake -S "${SCRIPT_DIR}" -B "${BUILD_DIR}" \
    -DCMAKE_PREFIX_PATH="${TILEXR_PROFILING_PREFIX}" \
    -DTILEXR_PROFILING_SOC=Ascend950 -DTILEXR_ENABLE_PROFILING=ON
cmake --build "${BUILD_DIR}" -j2
python3 - "${SCRIPT_DIR}" "${OUTPUT_DIR}" "${TILEXR_PROFILING_CYCLES_PER_US}" <<'MAPPING'
import json, pathlib, sys
source, output, conversion = sys.argv[1:]
mapping = json.loads((pathlib.Path(source) / "profiled_add4.mapping.template.json").read_text())
mapping["cycles_per_us"] = float(conversion)
(pathlib.Path(output) / "capture.mapping.json").write_text(json.dumps(mapping, indent=2) + "\n")
MAPPING
python3 "${SCRIPT_DIR}/manifest.py" "${OUTPUT_DIR}/capture.mapping.json" "${OUTPUT_DIR}/manifest.json"
"${BUILD_DIR}/bin/tilexr_profiled_add4_demo" "${OUTPUT_DIR}/capture.records.bin" \
    2>&1 | tee "${OUTPUT_DIR}/operator.log"
python3 "${SCRIPT_DIR}/manifest.py" "${OUTPUT_DIR}/capture.mapping.json" "${OUTPUT_DIR}/manifest.json" \
    --records "${OUTPUT_DIR}/capture.records.bin"
python3 -m tilexr_profiling --mapping "${OUTPUT_DIR}/capture.mapping.json" \
    --records "${OUTPUT_DIR}/capture.records.bin" --manifest "${OUTPUT_DIR}/manifest.json" \
    --output-dir "${OUTPUT_DIR}/report"
