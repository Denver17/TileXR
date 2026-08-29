#!/bin/bash
#
# 运行 UDMA 测试
#

set -e

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
TILEXR_ROOT="${SCRIPT_DIR}/../.."
INSTALL_DIR="${SCRIPT_DIR}/install"

# 加载环境
source "${TILEXR_ROOT}/scripts/common_env.sh"

# 设置 LD_LIBRARY_PATH：优先使用当前仓库刚编译安装的库，避免被 /usr/local/lib 中的旧库覆盖
export LD_LIBRARY_PATH="${INSTALL_DIR}/lib:${TILEXR_ROOT}/install/lib:/usr/local/lib:${LD_LIBRARY_PATH}"

echo "=========================================="
echo "  Running UDMA Tests"
echo "=========================================="
echo "LD_LIBRARY_PATH: ${LD_LIBRARY_PATH}"
echo ""

detect_ok_npus() {
    command -v npu-smi >/dev/null 2>&1 || return 0
    local ids=()
    for id in $(seq 0 15); do
        local health
        health=$(npu-smi info -t health -i "${id}" 2>/dev/null | awk -F: '/Health Status/ {gsub(/^[ \t]+|[ \t]+$/, "", $2); print $2; exit}')
        if [ "${health}" = "OK" ]; then
            ids+=("${id}")
        fi
    done
    local IFS=,
    echo "${ids[*]}"
}

if [ -z "${TILEXR_TEST_DEVICES:-}" ]; then
    TILEXR_TEST_DEVICES=$(detect_ok_npus)
    export TILEXR_TEST_DEVICES
fi
if [ -n "${TILEXR_TEST_DEVICES:-}" ]; then
    echo "TILEXR_TEST_DEVICES: ${TILEXR_TEST_DEVICES}"
fi

# 检查测试二进制是否存在
if [ ! -f "${INSTALL_DIR}/bin/test_tilexr_udma_transport_layout" ] ||
   [ ! -f "${INSTALL_DIR}/bin/test_tilexr_udma_registry" ] ||
   [ ! -f "${INSTALL_DIR}/bin/test_tilexr_udma" ]; then
    echo "ERROR: Test binaries not found. Please run build.sh first."
    exit 1
fi

# 测试 1: UDMA info layout 单元测试（host-only）
echo "=========================================="
echo "Test 1: TileXR UDMA Transport Layout Unit Test"
echo "=========================================="
"${INSTALL_DIR}/bin/test_tilexr_udma_transport_layout"
TEST1_RESULT=$?
echo ""

# 测试 2: TileXR UDMA registry 单元测试（host-only）
echo "=========================================="
echo "Test 2: TileXR UDMA Registry Unit Test"
echo "=========================================="
"${INSTALL_DIR}/bin/test_tilexr_udma_registry"
TEST2_RESULT=$?
echo ""

# 测试 3: TileXR 集成测试（单进程，单卡）
echo "=========================================="
echo "Test 3: TileXR Integration Tests (Single Process)"
echo "=========================================="
export RANK=0
export RANK_SIZE=1
"${INSTALL_DIR}/bin/test_tilexr_udma"
TEST3_RESULT=$?
echo ""

# 测试 4: TileXR 多进程测试（需要 mpirun）
echo "=========================================="
echo "Test 4: TileXR Multi-Process Tests (MPI)"
echo "=========================================="

# 检查是否有 mpirun
if command -v mpirun &> /dev/null; then
    # 检测可用的 NPU 数量
    NPU_COUNT=${TILEXR_ASCEND_DEV_NUM:-0}
    echo "Detected ${NPU_COUNT} NPU(s)"

    DEVICE_COUNT=0
    if [ -n "${TILEXR_TEST_DEVICES:-}" ]; then
        DEVICE_COUNT=$(echo "${TILEXR_TEST_DEVICES}" | awk -F, '{print NF}')
    fi

    if [ "${NPU_COUNT}" -ge 2 ] && [ "${DEVICE_COUNT}" -ge 2 ]; then
        echo "Running 2-rank test..."
        unset RANK
        unset RANK_SIZE
        mpirun -n 2 "${INSTALL_DIR}/bin/test_tilexr_udma"
        TEST4_RESULT=$?
    else
        echo "SKIP: Need at least 2 usable NPUs for multi-rank test"
        TEST4_RESULT=0
    fi
else
    echo "SKIP: mpirun not found, skipping multi-process tests"
    TEST4_RESULT=0
fi
echo ""

# 汇总结果
echo "=========================================="
echo "  Test Results Summary"
echo "=========================================="
echo "Test 1 (UDMA Layout):     $([ $TEST1_RESULT -eq 0 ] && echo 'PASS' || echo 'FAIL')"
echo "Test 2 (UDMA Registry):   $([ $TEST2_RESULT -eq 0 ] && echo 'PASS' || echo 'FAIL')"
echo "Test 3 (TileXR Single):   $([ $TEST3_RESULT -eq 0 ] && echo 'PASS' || echo 'FAIL')"
echo "Test 4 (TileXR Multi):    $([ $TEST4_RESULT -eq 0 ] && echo 'PASS' || echo 'SKIP/FAIL')"
echo "=========================================="

# 返回失败状态
if [ $TEST1_RESULT -ne 0 ] || [ $TEST2_RESULT -ne 0 ] ||
   [ $TEST3_RESULT -ne 0 ] || [ $TEST4_RESULT -ne 0 ]; then
    exit 1
fi

exit 0
