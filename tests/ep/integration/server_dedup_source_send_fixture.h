#ifndef TILEXR_TEST_SERVER_DEDUP_SOURCE_SEND_FIXTURE_H
#define TILEXR_TEST_SERVER_DEDUP_SOURCE_SEND_FIXTURE_H

#include <cstring>
#include <random>
#include <stdexcept>
#include <string>
#include "server_dedup_source_send_reference.h"
#include "../demo/tilexr_ep_expert_ids.h"
#include "../demo/mxfp8_golden.h"
#include "ep_memory_server_dedup_data_layout.h"

namespace ServerDedupReference {
struct Shape {
    uint32_t ranks, experts, bs, h, k;
    bool bf16, quantized, e4m3;
};
inline std::vector<int32_t> Ids(const Shape &s, uint32_t rank)
{
    return TileXREpDemo::BuildBalancedShuffleExpertIds(
        {s.bs, s.k, s.experts, s.experts / s.ranks, s.ranks, rank, 2026});
}
inline std::vector<float> Weights(const Shape &s, uint32_t rank)
{
    std::mt19937 rng(2026U + rank);
    std::uniform_int_distribution<uint32_t> sample(1U, 65535U);
    std::vector<float> result(s.bs * s.k);
    for (uint32_t token = 0; token < s.bs; ++token) {
        double sum = 0.0;
        for (uint32_t k = 0; k < s.k; ++k) sum += (result[token * s.k + k] = float(sample(rng)));
        for (uint32_t k = 0; k < s.k; ++k) result[token * s.k + k] = float(result[token * s.k + k] / sum);
    }
    return result;
}
// Finite normal BF16/FP16 values. Distinct rank/token/column patterns exercise
// routing as well as every hidden byte, without pre-quantizing the device input.
inline uint16_t HiddenBits(const Shape &s, uint32_t rank, uint32_t token, uint32_t column)
{
    uint32_t n = (rank + 1U) * 73856093U ^ (token + 1U) * 19349663U ^ (column + 1U) * 83492791U;
    n ^= n >> 13U;
    const uint32_t mantissa = s.bf16 ? 7U : 10U;
    const uint32_t bias = s.bf16 ? 127U : 15U;
    return uint16_t((n & 0x8000U) | ((bias - 3U + (n % 7U)) << mantissa) | (n & ((1U << mantissa) - 1U)));
}
inline float HiddenValue(const Shape &s, uint16_t bits)
{
    const uint32_t mantissa = s.bf16 ? 7U : 10U;
    const int exponent = int((bits & 0x7fffU) >> mantissa) - (s.bf16 ? 127 : 15);
    return std::ldexp((bits & 0x8000U ? -1.0f : 1.0f) *
        (1.0f + float(bits & ((1U << mantissa) - 1U)) / float(1U << mantissa)), exponent);
}
inline std::vector<uint16_t> Hidden(const Shape &s, uint32_t rank)
{
    std::vector<uint16_t> result(uint64_t(s.bs) * s.h);
    for (uint32_t token = 0; token < s.bs; ++token)
        for (uint32_t col = 0; col < s.h; ++col) result[uint64_t(token) * s.h + col] = HiddenBits(s, rank, token, col);
    return result;
}
template<class T> inline void Put(std::vector<uint8_t> &out, uint64_t offset, T value)
{
    std::memcpy(out.data() + offset, &value, sizeof(value));
}
inline std::vector<uint8_t> Payload(const Shape &s, uint32_t rank, const Packet &packet,
    const TileXREp::EpServerDedupDataLayout &layout)
{
    std::vector<uint8_t> result(layout.packedPayloadBytes, 0);
    std::vector<float> values(s.h);
    for (uint32_t col = 0; col < s.h; ++col) {
        const uint16_t bits = HiddenBits(s, rank, packet.token, col);
        if (s.quantized) values[col] = HiddenValue(s, bits);
        else Put(result, uint64_t(col) * 2U, bits);
    }
    if (s.quantized) TileXREpDemo::QuantizeMxfp8Row(values.data(), s.h,
        s.e4m3 ? TileXREpDemo::Mxfp8Format::E4M3 : TileXREpDemo::Mxfp8Format::E5M2,
        result.data(), result.data() + layout.quantCount);
    const auto tail = layout.tailOffset;
    Put(result, tail, rank); Put(result, tail + 4U, packet.token); Put(result, tail + 8U, packet.members[0].k);
    Put(result, tail + 12U, packet.members[0].weight);
    Put(result, tail + 16U, uint32_t(packet.members.size() - 1U));
    Put(result, tail + 20U, uint32_t(packet.members.size() - 1U));
    Put(result, tail + 24U, int32_t(-1)); Put(result, tail + 28U, int32_t(-1));
    for (uint32_t i = 1; i < packet.members.size(); ++i) {
        const auto &route = packet.members[i];
        const auto offset = tail + 32U + (i - 1U) * 16U;
        Put(result, offset, route.expert); Put(result, offset + 4U, route.q);
        Put(result, offset + 8U, route.k); Put(result, offset + 12U, route.weight);
    }
    return result;
}
inline std::vector<uint8_t> Pack(const std::vector<uint8_t> &payload)
{
    std::vector<uint8_t> result(payload.size() / 480U * 512U);
    for (uint64_t block = 0; block < payload.size() / 480U; ++block) {
        std::memcpy(result.data() + block * 512U, payload.data() + block * 480U, 480U);
        for (uint32_t word = 0; word < 8U; ++word) Put(result, block * 512U + 480U + word * 4U, uint32_t(0x3f800000U));
    }
    return result;
}
inline void CheckPacket(const Shape &s, uint32_t source, const Packet &packet,
    const TileXREp::EpServerDedupDataLayout &layout, const std::vector<uint8_t> &actual)
{
    const auto expected = Payload(s, source, packet, layout);
    const std::string context = " source=" + std::to_string(source) + " token=" + std::to_string(packet.token) +
        " k=" + std::to_string(packet.members[0].k) + " q=" + std::to_string(packet.members[0].q);
    if (actual.size() != layout.tokenStride) throw std::runtime_error("packet size" + context);
    for (uint64_t block = 0; block < layout.blockCount; ++block) {
        for (uint32_t word = 0; word < 8U; ++word) {
            uint32_t flag = 0;
            std::memcpy(&flag, actual.data() + block * 512U + 480U + word * 4U, 4U);
            if (flag != 0x3f800000U) throw std::runtime_error("packet flag" + context +
                " block=" + std::to_string(block) + " word=" + std::to_string(word));
        }
    }
    const auto compare = [&](uint64_t offset, uint64_t bytes, const char *field) {
        for (uint64_t i = offset; i < offset + bytes; ++i) {
            const auto observed = actual[i / 480U * 512U + i % 480U];
            if (observed != expected[i]) throw std::runtime_error(std::string(field) + context +
                " byte=" + std::to_string(i) + " expected=" + std::to_string(expected[i]) + " actual=" + std::to_string(observed));
        }
    };
    compare(0, uint64_t(s.h) * (s.quantized ? 1U : 2U), "packet hidden");
    if (s.quantized) compare(layout.quantCount, layout.scaleBytes, "packet scale");
    compare(layout.tailOffset, 32U + 16U * (packet.members.size() - 1U), "packet tail");
}
} // namespace ServerDedupReference
#endif
