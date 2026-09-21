// Full oracle preserves the intermediate low-precision partial from M4.
// M5 consumes only effective source slots, in original k order, then rounds
// once more for yOut. No production mask/address/reduction helper is used.
struct CombineM5Validation : CombineM4Validation {
    std::vector<uint8_t> expectedY;

    void Initialize(const Config &c, const TileXR::CommArgs &args)
    {
        CombineM4Validation::Initialize(c, args);
        BuildFinal(c);
        // This changes only the Host expectation; the kernel owns GM cleanup.
        for (size_t block = 0; block < expectedResult.size() / 512U; ++block) {
            std::fill_n(expectedResult.begin() + block * 512U + 480U, 32U, 0U);
            std::fill_n(ignoredResult.begin() + block * 512U + 480U, 32U, 0U);
        }
    }

    void BuildFinal(const Config &c)
    {
        using namespace ServerDedupReference;
        using namespace CombineM4Reference;
        auto shape = SourceSendShape(c);
        shape.quantized = false;
        std::vector<uint32_t> masks(c.bs, 0U);
        std::vector<float> weights(uint64_t(c.bs) * c.topK, 0.0f);
        for (uint32_t producer = 0; producer < uint32_t(c.rankSize); ++producer) {
            for (const auto &row : DispatchRows(shape, producer)) {
                if (row.source != uint32_t(c.rank) || (!row.Direct() && !row.FirstHop())) continue;
                Require((masks[row.token] & (1U << row.k)) == 0U, "M5 reference source slot is unique");
                masks[row.token] |= 1U << row.k;
                weights[uint64_t(row.token) * c.topK + row.k] = row.FirstHop() ? 1.0f :
                    c.operatorMode == "combine" ? Weight(row.source, row.token, row.k) : row.packet.members.front().weight;
            }
        }
        const uint64_t stride = ((uint64_t(c.h) * 2U + 479U) / 480U) * 512U;
        expectedY.resize(uint64_t(c.bs) * c.h * 2U);
        for (uint32_t token = 0; token < uint32_t(c.bs); ++token) {
            Require(masks[token] != 0U, "M5 reference mask is nonzero");
            for (uint32_t column = 0; column < uint32_t(c.h); ++column) {
                float sum = 0.0f;
                bool first = true;
                for (uint32_t k = 0; k < uint32_t(c.topK); ++k) {
                    if (!(masks[token] & (1U << k))) continue;
                    const uint64_t byte = uint64_t(column) * 2U;
                    const uint64_t offset = (uint64_t(token) * c.topK + k) * stride + byte / 480U * 512U + byte % 480U;
                    uint16_t bits;
                    std::memcpy(&bits, expectedResult.data() + offset, 2U);
                    const float value = Decode(bits, shape.bf16);
                    const float weight = weights[uint64_t(token) * c.topK + k];
                    if (first) {
                        volatile float product = value * weight;
                        sum = product;
                        first = false;
                    } else sum = Accumulate(sum, value, weight);
                }
                const uint16_t bits = Encode(sum, shape.bf16);
                const uint16_t exponentMask = shape.bf16 ? 0x7f80U : 0x7c00U;
                Require(std::isfinite(sum) && std::fpclassify(sum) != FP_SUBNORMAL &&
                    (bits & exponentMask) != exponentMask &&
                    ((bits & 0x7fffU) == 0U || (bits & exponentMask) != 0U),
                    "M5 fixture yOut must be finite normal or zero before and after rounding");
                std::memcpy(expectedY.data() + (uint64_t(token) * c.h + column) * 2U, &bits, 2U);
            }
        }
    }

    void Validate(const Config &c, const TileXR::CommArgs &args,
        const TileXREp::EpMemoryServerDedupCombineLayout &layout, const std::vector<void *> &buffers)
    {
        CombineM12Validation::Validate(c, args, layout, buffers);
        CheckBytes(buffers[13], expectedY, "M5 yOut after intermediate partial and final RNE");
    }
};
