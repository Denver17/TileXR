#ifndef TILEXR_EP_SERVER_DEDUP_COMBINE_DELAY_FAKE_KERNEL_OPERATOR_H
#define TILEXR_EP_SERVER_DEDUP_COMBINE_DELAY_FAKE_KERNEL_OPERATOR_H

#include <cstdint>

#define __global__
#define __aicore__
#define __gm__
#define __mix__(cube, vector)
using GM_ADDR = uint8_t *;

namespace AscendC {
// Defined by the test; every timestamp is supplied explicitly.
int64_t GetSystemCycle();
}

#endif
