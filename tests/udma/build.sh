#!/bin/bash
#
# 构建 UDMA 测试
#

set -e

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
TILEXR_ROOT="${SCRIPT_DIR}/../.."

# 加载环境
source "${TILEXR_ROOT}/scripts/common_env.sh"

export ARCH="${TILEXR_OS_ARCH}"

echo "=========================================="
echo "  Building UDMA Tests"
echo "=========================================="

# 创建构建目录
BUILD_DIR="${SCRIPT_DIR}/build"
INSTALL_DIR="${SCRIPT_DIR}/install"

rm -rf "${BUILD_DIR}" "${INSTALL_DIR}"
mkdir -p "${BUILD_DIR}"
mkdir -p "${INSTALL_DIR}"

cd "${BUILD_DIR}"

# 配置
case "${BUILD_TILEXR_UDMA_DEMO:-AUTO}" in
    0|OFF|off|false|FALSE)
        DEMO_OPTION="-DBUILD_TILEXR_UDMA_DEMO=OFF"
        ;;
    1|ON|on|true|TRUE)
        DEMO_OPTION="-DBUILD_TILEXR_UDMA_DEMO=ON"
        ;;
    *)
        if command -v bisheng >/dev/null 2>&1; then
            DEMO_OPTION="-DBUILD_TILEXR_UDMA_DEMO=ON"
        else
            echo "WARN: bisheng not found; TileXR UDMA communication demo target will be skipped."
            DEMO_OPTION="-DBUILD_TILEXR_UDMA_DEMO=OFF"
        fi
        ;;
esac

cmake -DCMAKE_INSTALL_PREFIX="${INSTALL_DIR}" ${DEMO_OPTION} ..

# 构建
make -j$(nproc)

# 安装
make install

echo ""
echo "=========================================="
echo "  Build Complete"
echo "=========================================="
echo "Test binaries installed to: ${INSTALL_DIR}/bin"
echo ""
echo "Available tests:"
echo "  - test_tilexr_udma_transport_layout : UDMA info layout unit tests"
echo "  - test_tilexr_udma_registry : registered-memory metadata unit tests"
echo "  - test_tilexr_udma     : TileXR integration tests"
if [ -f "${INSTALL_DIR}/bin/tilexr_udma_demo" ]; then
    echo "  - tilexr_udma_demo     : TileXR UDMA communication demo"
else
    echo "  - tilexr_udma_demo     : skipped (requires bisheng/AICore toolchain)"
fi
if [ -f "${INSTALL_DIR}/bin/tilexr_udma_profile_probe" ]; then
    echo "  - tilexr_udma_profile_probe : persistent multi-MR deferred-READ hardware probe"
fi
echo ""
echo "Run tests with:"
echo "  bash run_tests.sh"
echo "Run demo with:"
echo "  bash demo/run_tilexr_udma_demo.sh 0 2 16"
echo "Run the two-host profile probe with:"
echo "  bash demo/run_tilexr_udma_profile_probe_mpi.sh --hosts host1:1,host2:1 --comm-id host1_data_ip:10067"
echo "=========================================="
