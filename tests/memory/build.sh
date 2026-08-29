#!/bin/bash
#
# Build TileXR peer-memory DataCopy tests and demo.
#

set -e

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
TILEXR_ROOT="${SCRIPT_DIR}/../.."

source "${TILEXR_ROOT}/scripts/common_env.sh"

export ARCH="${TILEXR_OS_ARCH}"

echo "=========================================="
echo "  Building TileXR Memory Tests"
echo "=========================================="

BUILD_DIR="${SCRIPT_DIR}/build"
INSTALL_DIR="${SCRIPT_DIR}/install"

rm -rf "${BUILD_DIR}"
mkdir -p "${BUILD_DIR}"
mkdir -p "${INSTALL_DIR}"

cd "${BUILD_DIR}"

if command -v bisheng >/dev/null 2>&1; then
    DEMO_OPTION="-DBUILD_TILEXR_MEMORY_DEMO=ON"
else
    echo "WARN: bisheng not found; TileXR peer-memory DataCopy demo target will be skipped."
    DEMO_OPTION="-DBUILD_TILEXR_MEMORY_DEMO=OFF"
fi

cmake -DCMAKE_INSTALL_PREFIX="${INSTALL_DIR}" ${DEMO_OPTION} ..
make -j$(nproc)
make install

echo ""
echo "=========================================="
echo "  Build Complete"
echo "=========================================="
echo "Test binaries installed to: ${INSTALL_DIR}/bin"
echo ""
echo "Available tests:"
if [ -f "${INSTALL_DIR}/bin/tilexr_memory_demo" ]; then
    echo "  - tilexr_memory_demo   : TileXR peer-memory DataCopy communication demo"
else
    echo "  - tilexr_memory_demo   : skipped (requires bisheng/AICore toolchain)"
fi
echo ""
echo "Run demo with:"
echo "  bash demo/run_tilexr_memory_demo.sh 2 16"
echo "=========================================="
