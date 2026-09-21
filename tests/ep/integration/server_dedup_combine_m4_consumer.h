// M4 source-side partial oracle: resolve contributions by original route
// identity, never production stage offsets or the actual received table3.
struct CombineM4Validation : CombineM3Validation {
    void Initialize(const Config &c, const TileXR::CommArgs &args)
    {
        using namespace CombineM4Reference;
        Require(Encode(1.0f + std::ldexp(1.0f, -8), true) == 0x3f80U &&
            Encode(1.0f + 3.0f * std::ldexp(1.0f, -8), true) == 0x3f82U &&
            Encode(1.0f + std::ldexp(1.0f, -11), false) == 0x3c00U &&
            Encode(1.0f + 3.0f * std::ldexp(1.0f, -11), false) == 0x3c02U &&
            Encode(-0.0f, true) == 0x8000U && Encode(-0.0f, false) == 0x8000U,
            "M4 oracle RNE and signed-zero contract");
        CombineM3Validation::Initialize(c, args);
        BuildPartials(c);
    }

    void BuildPartials(const Config &c)
    {
        using namespace ServerDedupReference;
        using namespace CombineM4Reference;
        auto shape = SourceSendShape(c);
        shape.quantized = false;
        const bool independentExpert = c.operatorMode == "combine" || c.quantMode != 0;
        const uint32_t localExperts = uint32_t(c.experts / c.rankSize);
        using Identity = std::tuple<uint32_t, uint32_t, uint32_t, uint32_t>;
        std::map<Identity, size_t> originalRows;
        std::vector<std::vector<DispatchRow>> allRows;
        for (uint32_t producer = 0; producer < uint32_t(c.rankSize); ++producer) {
            allRows.push_back(DispatchRows(shape, producer));
            for (size_t index = 0; index < allRows.back().size(); ++index) {
                const auto &row = allRows.back()[index];
                Require(originalRows.emplace(Identity(row.source, row.token, row.expert, row.k), index).second,
                    "M4 original route identity is unique");
            }
        }
        std::set<uint64_t> outputSlots;
        const uint64_t stride = ((uint64_t(c.h) * 2U + 479U) / 480U) * 512U;
        for (uint32_t producer = 0; producer < uint32_t(c.rankSize); ++producer) {
            for (const auto &row : allRows[producer]) {
                if (!row.FirstHop()) continue;
                const auto &members = row.packet.members;
                Require(members.size() >= 2U && members.size() <= uint64_t(c.topK), "M4 member count domain");
                if (producer == uint32_t(c.rank)) {
                    ++partialRows;
                    const uint64_t additional = members.size() - 1U;
                    partialMembers += additional;
                    minMembers = minMembers == 0U ? additional : std::min(minMembers, additional);
                    maxMembers = std::max(maxMembers, additional);
                    for (size_t member = 1U; member < members.size(); ++member)
                        if (members[member].expert / localExperts == producer) ++localMembers;
                }
                if (row.source != uint32_t(c.rank)) continue;
                ++partialReceivedRows;
                const uint64_t slot = uint64_t(row.token) * c.topK + row.k;
                Require(slot < uint64_t(c.bs) * c.topK && outputSlots.insert(slot).second &&
                    std::memcmp(expectedResult.data() + slot * stride + 480U,
                        initialResult.data() + slot * stride + 480U, 4U) == 0,
                    "M4 partial and DIRECT source slots must be disjoint");
                std::vector<size_t> memberRows;
                std::vector<float> weights;
                for (const auto &member : members) {
                    memberRows.push_back(originalRows.at(Identity(row.source, row.token, member.expert, member.k)));
                    weights.push_back(c.operatorMode == "combine" ? Weight(row.source, row.token, member.k) : member.weight);
                }
                std::vector<uint16_t> values(c.h);
                for (uint32_t column = 0; column < uint32_t(c.h); ++column) {
                    float sum = 0.0f;
                    for (size_t index = 0; index < members.size(); ++index) {
                        const uint16_t bits = independentExpert ? ExpertBits(shape.bf16,
                            members[index].expert / localExperts, memberRows[index], column, c.operatorMode == "both") :
                            HiddenBits(shape, row.source, row.token, column);
                        const float value = Decode(bits, shape.bf16);
                        if (index == 0U) {
                            // Preserve self's signed zero and separate the
                            // FP32 multiplication from every following Add.
                            volatile float product = value * weights[index];
                            sum = product;
                        } else sum = Accumulate(sum, value, weights[index]);
                    }
                    values[column] = Encode(sum, shape.bf16);
                    const uint16_t exponentMask = shape.bf16 ? 0x7f80U : 0x7c00U;
                    Require(std::isfinite(sum) && std::fpclassify(sum) != FP_SUBNORMAL &&
                        (values[column] & exponentMask) != exponentMask &&
                        ((values[column] & 0x7fffU) == 0U || (values[column] & exponentMask) != 0U),
                        "M4 fixture partial must be finite normal or zero before and after rounding");
                }
                SetResultPayload(c, slot, values);
            }
        }
    }
};
