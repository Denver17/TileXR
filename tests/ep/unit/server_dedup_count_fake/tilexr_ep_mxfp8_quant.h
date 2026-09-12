#pragma once
#include "kernel_operator.h"
// CPU packet tests exercise the non-quantized source path. Real MXFP8 math stays
// in the existing device implementation and is checked by its hardware golden.
namespace TileXRMxfp8Quant {
template<class T> void ComputeMaxExp(T *, uint16_t *, uint32_t) { throw std::runtime_error("MXFP8 needs device validation"); }
template<class T> void ComputeScale(uint16_t *, uint16_t *, uint16_t *, uint32_t) { throw std::runtime_error("MXFP8 needs device validation"); }
template<class X, class Y, AscendC::RoundMode A, AscendC::RoundMode B>
void ComputeFp8Data(X *, uint16_t *, int8_t *, uint32_t) { throw std::runtime_error("MXFP8 needs device validation"); }
}
