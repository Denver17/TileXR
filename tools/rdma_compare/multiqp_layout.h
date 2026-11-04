#pragma once
#include "case_layout.h"

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
