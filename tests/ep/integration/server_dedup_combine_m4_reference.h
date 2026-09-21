#ifndef TILEXR_TEST_SERVER_DEDUP_COMBINE_M4_REFERENCE_H
#define TILEXR_TEST_SERVER_DEDUP_COMBINE_M4_REFERENCE_H

#include <cmath>
#include <cstdint>
#include <cstring>
#include "server_dedup_dispatch_reference.h"

namespace CombineM4Reference {
// Fixtures stay in a finite normal/zero domain. FP32 multiplication and
// addition round separately; the final communication value uses ties-to-even.
inline uint32_t RoundShift(uint32_t value, uint32_t shift)
{
    const uint32_t quotient = value >> shift;
    const uint32_t remainder = value & ((uint32_t(1) << shift) - 1U);
    const uint32_t half = uint32_t(1) << (shift - 1U);
    return quotient + (remainder > half || (remainder == half && (quotient & 1U)));
}

inline uint16_t Encode(float value, bool bf16)
{
    uint32_t raw;
    std::memcpy(&raw, &value, sizeof(raw));
    if (bf16) return static_cast<uint16_t>((raw + 0x7fffU + ((raw >> 16U) & 1U)) >> 16U);
    const uint16_t sign = static_cast<uint16_t>((raw >> 16U) & 0x8000U);
    const int exponent = int((raw >> 23U) & 255U) - 127 + 15;
    const uint32_t fraction = raw & 0x7fffffU;
    if (exponent >= 31) return sign | 0x7c00U;
    if (exponent < -10) return sign;
    if (exponent <= 0) return sign | static_cast<uint16_t>(RoundShift(fraction | 0x800000U, 14U - exponent));
    return sign | static_cast<uint16_t>((uint32_t(exponent) << 10U) + RoundShift(fraction, 13U));
}

inline float Decode(uint16_t bits, bool bf16)
{
    if (bf16) {
        const uint32_t raw = uint32_t(bits) << 16U;
        float result;
        std::memcpy(&result, &raw, sizeof(result));
        return result;
    }
    const uint32_t exponent = (bits >> 10U) & 31U, fraction = bits & 1023U;
    const float magnitude = exponent == 0U ? std::ldexp(float(fraction), -24) :
        std::ldexp(1.0f + float(fraction) / 1024.0f, int(exponent) - 15);
    return bits & 0x8000U ? -magnitude : magnitude;
}

inline uint16_t ExpertBits(bool bf16, uint32_t rank, uint64_t row, uint32_t column, bool positive = false)
{
    const uint32_t mix = uint32_t(row) * 37U + column * 13U + rank * 29U;
    const uint32_t fractionBits = bf16 ? 7U : 10U;
    const uint32_t bias = bf16 ? 127U : 15U;
    return static_cast<uint16_t>((!positive && (mix & 1U) ? 0x8000U : 0U) |
        ((bias - 1U + mix % 3U) << fractionBits) | ((mix & 127U) << (fractionBits - 7U)));
}

inline float Weight(uint32_t source, uint32_t token, uint32_t k)
{
    constexpr float values[] = {0.0f, 0.375f, -1.25f, 0.125f, 1.75f, -0.3125f, 0.5f, -0.625f};
    return values[(source * 7U + token * 3U + k) % 8U];
}

inline float Accumulate(float sum, float value, float weight)
{
    volatile float product = value * weight;
    volatile float result = sum + product;
    return result;
}
} // namespace CombineM4Reference
#endif
