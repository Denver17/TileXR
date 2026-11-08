/*
 * 功能：为 multi-QP benchmark 选择循环或逐调用 slot 布局，并在预期 payload 中加入 QP 标识，区分 QP/SGE 数据。
 * 调用关系：multiqp.cpp 与 CPU 布局测试共用这些函数；布局和基础预期值来自 case_layout.h，模式来自 bench_abi.h。
 * 资源与生命周期：仅计算布局和预期值，实际 buffer 由 Host 或测试 fixture 分配。
 *
 * Purpose: Select cyclic or per-call slot layouts and add QP identity to expected payloads to distinguish QP/SGE data.
 * Calls: Shared by multiqp.cpp and CPU layout tests; case_layout.h supplies base layouts/expectations and bench_abi.h selects the mode.
 * Lifetime: Computes layouts/expected values only; Host or test fixtures allocate the buffers.
 */
#pragma once
#include "bench_abi.h"
#include "case_layout.h"

inline CaseLayout MultiQpCaseLayout(uint32_t bytes, uint32_t count, uint32_t numSge)
{
    return CaseLayout(bytes, count, kMultiQpSingleWqeCalls, numSge);
}

inline uint64_t MultiQpPattern(uint32_t qp, uint32_t numSge)
{
    // Keep SGE1 data unchanged. SGL needs a different multiplier from the
    // entry/slot/rank patterns, or swapping a QP and entry can cancel the XOR.
    return uint64_t(qp) * (numSge == 1U ? 0xD6E8FEB86659FD93ULL : 0xE7037ED1A0B428DBULL);
}

inline uint64_t ExpectedMultiQpWord(const CaseLayout& layout, uint64_t offset,
    uint32_t rank, uint32_t count, uint32_t direction, uint32_t qp)
{
    const auto expected = ExpectedCaseWord(layout, offset, rank, count, direction);
    return expected == kCaseGuardWord ? expected : expected ^ MultiQpPattern(qp, layout.sgeCount);
}
