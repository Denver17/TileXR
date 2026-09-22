#pragma once
#include <cstdint>
#define TILEXR_TEST_EXTERNAL_CLOCK 1
#define TILEXR_TEST_DISTINCT_FLOAT16_TYPES 1
struct half { uint16_t bits; };
struct bfloat16_t { uint16_t bits; };
#include "../server_dedup_count_fake/kernel_operator.h"
#include <limits>
namespace AscendC {
int64_t GetSystemCycle();
// Arithmetic definitions emulate API numerical boundaries for Host tests only.
// Production still uses CANN Cast/Muls/Add, verified by the A5 cross-build.
inline float TestDecodeLow(uint16_t bits, uint32_t fractionBits, int bias)
{
    const uint32_t exponentMask = fractionBits == 10U ? 31U : 255U;
    const uint32_t exponent = (bits >> fractionBits) & exponentMask;
    const uint32_t fraction = bits & ((1U << fractionBits) - 1U);
    float value;
    if (exponent == exponentMask) value = fraction ? std::numeric_limits<float>::quiet_NaN() :
        std::numeric_limits<float>::infinity();
    else if (exponent == 0U) value = std::ldexp(float(fraction), 1 - bias - int(fractionBits));
    else value = std::ldexp(float((1U << fractionBits) + fraction), int(exponent) - bias - int(fractionBits));
    return std::copysign(value, (bits & 0x8000U) ? -1.0f : 1.0f);
}
inline uint16_t TestEncodeLow(float value, uint32_t fractionBits, int bias)
{
    const uint16_t sign = std::signbit(value) ? 0x8000U : 0U;
    const uint32_t exponentMask = fractionBits == 10U ? 31U : 255U;
    if (std::isnan(value)) return sign | uint16_t((exponentMask << fractionBits) | 1U);
    if (std::isinf(value)) return sign | uint16_t(exponentMask << fractionBits);
    const double magnitude = std::fabs(double(value));
    if (magnitude == 0.0) return sign;
    int exponent = std::ilogb(magnitude);
    if (exponent < 1 - bias) {
        const uint32_t fraction = uint32_t(std::nearbyint(std::ldexp(magnitude, bias - 1 + int(fractionBits))));
        return sign | uint16_t(fraction);
    }
    uint32_t mantissa = uint32_t(std::nearbyint(std::ldexp(magnitude, int(fractionBits) - exponent)));
    if (mantissa == (2U << fractionBits)) { mantissa >>= 1U; ++exponent; }
    if (exponent + bias >= int(exponentMask)) return sign | uint16_t(exponentMask << fractionBits);
    return sign | uint16_t((uint32_t(exponent + bias) << fractionBits) | (mantissa - (1U << fractionBits)));
}
inline float TestDecode(half value) { return TestDecodeLow(value.bits, 10U, 15); }
inline float TestDecode(bfloat16_t value) { return TestDecodeLow(value.bits, 7U, 127); }
inline void TestEncode(half &output, float value) { output.bits = TestEncodeLow(value, 10U, 15); }
inline void TestEncode(bfloat16_t &output, float value) { output.bits = TestEncodeLow(value, 7U, 127); }
template<class T> void Cast(LocalTensor<float> dst, LocalTensor<T> src, RoundMode mode, uint32_t count)
{
    if (mode != RoundMode::CAST_NONE) throw std::runtime_error("upcast requires CAST_NONE");
    for (uint32_t i = 0; i < count; ++i) dst.SetValue(i, TestDecode(src.GetValue(i)));
}
template<class T> void Cast(LocalTensor<T> dst, LocalTensor<float> src, RoundMode mode, uint32_t count)
{
    if (mode != RoundMode::CAST_RINT) throw std::runtime_error("downcast requires nearest-even CAST_RINT");
    for (uint32_t i = 0; i < count; ++i) { T value; TestEncode(value, src.GetValue(i)); dst.SetValue(i, value); }
}
inline std::function<void()> &MulsObserver()
{
    static std::function<void()> observer;
    return observer;
}
inline void Muls(LocalTensor<float> dst, LocalTensor<float> src, float scalar, uint32_t count)
{
    for (uint32_t i = 0; i < count; ++i) { volatile float value = src.GetValue(i) * scalar; dst.SetValue(i, value); }
    if (MulsObserver()) MulsObserver()();
}
inline void Add(LocalTensor<float> dst, LocalTensor<float> a, LocalTensor<float> b, uint32_t count)
{
    for (uint32_t i = 0; i < count; ++i) { volatile float value = a.GetValue(i) + b.GetValue(i); dst.SetValue(i, value); }
}
}
