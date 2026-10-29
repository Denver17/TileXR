// Independent global-route oracle for current complete Combine.
// Preserve low-precision partial rounding before final weighted aggregation.
struct CombineValidation {
    TileXREp::EpServerDedupCombineDataLayout data {};
    std::vector<uint8_t> initialControl;
    // Independent result oracle. Only semantically unspecified payload/Flag lanes are masked.
    std::vector<uint8_t> initialResult, expectedResult, ignoredResult;
    uint32_t launchedWindows = 0;
    uint64_t observations = 0, type3Rows = 0, validatedTokenNum = 0;
    enum : uint8_t { guard = 0x6d };

    void InitializeMemory(const Config &c, const TileXR::CommArgs &args)
    {
        const uint64_t slot = (uint64_t(c.h) * 2U + 479U) / 480U * 512U;
        Check(TileXREp::TileXREpBuildServerDedupCombineDataLayout(c.rankSize, c.bs, c.h,
            c.topK, c.experts, slot, &data), "data layout");
        initialControl.assign(TILEXR_EP_SERVER_DEDUP_SHARED_DATA_OFFSET -
            TILEXR_EP_SERVER_DEDUP_COMBINE_CONTROL_OFFSET, guard);
        // Non-first float lanes are deliberately nonzero: only each 32B slot's
        // first float contributes to the two readiness sums.
        for (uint32_t listener = 0; listener < 8U; ++listener)
            for (uint32_t core = 0; core < 64U; ++core)
                std::fill_n(initialControl.begin() + listener * 2048U + core * 32U, 4U, 0);
        for (uint32_t core = 0; core < TileXREp::kEpServerDedupCombineRecvCoreNum; ++core)
            for (uint32_t source = 0; source < 8U; ++source)
                std::fill_n(initialControl.begin() + TILEXR_EP_SERVER_DEDUP_COMBINE_SERVER_READY_OFFSET +
                    core * 512U + source * 32U, 4U, 0);
        for (uint32_t window = 0; window < 2U; ++window) {
            auto base = args.peerMems[c.rank] + window * (args.peerMemBytes / 2U);
            Check(aclrtMemcpy(base + TILEXR_EP_SERVER_DEDUP_COMBINE_CONTROL_OFFSET,
                initialControl.size(), initialControl.data(), initialControl.size(), ACL_MEMCPY_HOST_TO_DEVICE),
                "seed control ownership guards");
            // In both mode Dispatch owns window 1 and Combine owns window 0.
            // Do not seed or validate Dispatch's shared payload as Combine data.
            if (c.operatorMode == "both" && window == 1U) continue;
            Check(aclrtMemset(base + data.resultOffset, data.requiredWindowBytes - data.resultOffset,
                guard, data.requiredWindowBytes - data.resultOffset), "seed data guards");
        }
    }

    void NoteLaunch(int64_t magic)
    {
        launchedWindows |= 1U << (magic % 2);
    }

    void CheckBytes(const void *device, const std::vector<uint8_t> &expected, const std::string &name,
        const std::vector<uint8_t> *ignored = nullptr)
    {
        std::vector<uint8_t> actual(std::min<size_t>(1024U * 1024U, expected.size()));
        for (size_t offset = 0; offset < expected.size();) {
            const size_t count = std::min(actual.size(), expected.size() - offset);
            Check(aclrtMemcpy(actual.data(), actual.size(), static_cast<const uint8_t *>(device) + offset,
                count, ACL_MEMCPY_DEVICE_TO_HOST), "read Combine communication window");
            size_t mismatch = 0;
            for (; mismatch < count; ++mismatch) {
                const auto index = offset + mismatch;
                if (ignored && index < ignored->size() && (*ignored)[index]) continue;
                if (actual[mismatch] != expected[index]) break;
            }
            ValidateOutput(mismatch == count, name + " mismatch byte=" + std::to_string(offset + mismatch));
            offset += count;
        }
    }

    void ValidateMemory(const Config &c, const TileXR::CommArgs &args,
        const TileXREp::EpMemoryServerDedupCombineLayout &layout, const std::vector<void *> &buffers)
    {
        int32_t tokenNum = -1;
        Check(aclrtMemcpy(&tokenNum, sizeof(tokenNum), static_cast<int32_t *>(buffers[4]) + c.experts - 1,
            sizeof(tokenNum), ACL_MEMCPY_DEVICE_TO_HOST), "read TokenNum");
        ValidateOutput(tokenNum >= 0 && tokenNum <= 10000 && uint64_t(tokenNum) <= layout.capacityRows,
            "TokenNum outside supported range");
        std::vector<uint32_t> types(tokenNum), table2(uint64_t(tokenNum) * 3U);
        if (tokenNum) {
            if (TileXREp::EpServerDedupEnabled(c.bs)) Check(aclrtMemcpy(types.data(), types.size() * 4U, buffers[5], types.size() * 4U,
                ACL_MEMCPY_DEVICE_TO_HOST), "read token types");
            Check(aclrtMemcpy(table2.data(), table2.size() * 4U, buffers[6], table2.size() * 4U,
                ACL_MEMCPY_DEVICE_TO_HOST), "read table2");
        }
        validatedTokenNum = static_cast<uint64_t>(tokenNum);
        const bool separate = c.operatorMode == "combine" || c.quantMode != 0;
        const auto expert = static_cast<const uint8_t *>(buffers[separate ? 12U : 2U]);
        std::vector<uint8_t> expertSnapshot;
        if (tokenNum) {
            expertSnapshot.resize(uint64_t(tokenNum) * c.h * 2U);
            Check(aclrtMemcpy(expertSnapshot.data(), expertSnapshot.size(), expert, expertSnapshot.size(),
                ACL_MEMCPY_DEVICE_TO_HOST), "snapshot valid expert rows");
        }
        type3Rows = 0;
        for (auto type : types) if (type == 3U) ++type3Rows;
        for (uint32_t window = 0; window < 2U; ++window) {
            const bool used = (launchedWindows & (1U << window)) != 0;
            auto base = args.peerMems[c.rank] + window * (args.peerMemBytes / 2U);
            auto control = initialControl;
            if (used && TileXREp::EpServerDedupEnabled(c.bs)) {
                std::fill_n(control.begin(), TILEXR_EP_SERVER_DEDUP_COMBINE_PREPARE_DONE_BYTES, 0);
                std::fill_n(control.begin() + TILEXR_EP_SERVER_DEDUP_COMBINE_SERVER_READY_OFFSET,
                    TileXREp::kEpServerDedupCombineRecvCoreNum * 512U, 0);
            }
            CheckBytes(base + TILEXR_EP_SERVER_DEDUP_COMBINE_CONTROL_OFFSET, control,
                "Combine control/ownership window=" + std::to_string(window));
            if (c.operatorMode == "both" && window == 1U) continue;
            std::vector<uint8_t> expected(data.requiredWindowBytes - data.resultOffset, guard);
            const auto &results = used ? expectedResult : initialResult;
            std::copy(results.begin(), results.end(), expected.begin());
            std::set<uint32_t> slots;
            for (int32_t row = 0; used && row < tokenNum; ++row) {
                if (types[row] != 3U) continue;
                const uint32_t slot = table2[uint64_t(row) * 3U];
                ValidateOutput(slot < data.stageRows && slots.insert(slot).second,
                    "type3 stage slot is invalid or duplicated");
                auto target = expected.data() + data.stageOffset - data.resultOffset + slot * data.stageRowBytes;
                std::memcpy(target, expertSnapshot.data() + uint64_t(row) * c.h * 2U, c.h * 2U);
            }
            CheckBytes(base + data.resultOffset, expected,
                "Combine stage/result/padding window=" + std::to_string(window),
                used ? &ignoredResult : nullptr);
        }
        ++observations;
    }

    uint64_t directRows = 0, receivedRows = 0;
    uint64_t partialRows = 0, partialReceivedRows = 0, partialMembers = 0, localMembers = 0, minMembers = 0, maxMembers = 0;

    void InitializeDirect(const Config &c, const TileXR::CommArgs &args)
    {
        InitializeMemory(c, args);
        const uint64_t stride = ((uint64_t(c.h) * 2U + 479U) / 480U) * 512U;
        initialResult.assign(uint64_t(c.bs) * c.topK * stride, guard);
        // Initial creation only. No post-call helper clears a result Flag.
        for (size_t block = 0; block < initialResult.size() / 512U; ++block)
            std::fill_n(initialResult.begin() + block * 512U + 480U, 32U, 0);
        for (uint32_t window = 0; window < 2U; ++window) {
            if (c.operatorMode == "both" && window == 1U) continue;
            Check(aclrtMemcpy(args.peerMems[c.rank] + window * (args.peerMemBytes / 2U) + data.resultOffset,
                initialResult.size(), initialResult.data(), initialResult.size(), ACL_MEMCPY_HOST_TO_DEVICE),
                "initialize M3 result Flag slots");
        }
        BuildExpected(c);
    }

    void BuildExpected(const Config &c)
    {
        using namespace ServerDedupReference;
        auto shape = SourceSendShape(c);
        shape.quantized = false;
        const bool generated = c.operatorMode == "combine" || c.quantMode != 0;
        expectedResult = initialResult;
        ignoredResult.assign(expectedResult.size(), 0);
        std::set<uint64_t> written;
        directRows = receivedRows = type3Rows = 0;
        for (uint32_t producer = 0; producer < uint32_t(c.rankSize); ++producer) {
            const auto rows = DispatchRows(shape, producer);
            if (producer == uint32_t(c.rank)) validatedTokenNum = rows.size();
            for (size_t originalRow = 0; originalRow < rows.size(); ++originalRow) {
                const auto &row = rows[originalRow];
                if (producer == uint32_t(c.rank)) {
                    if (row.Direct()) ++directRows;
                    else if (!row.FirstHop()) ++type3Rows;
                }
                if (!row.Direct() || row.source != uint32_t(c.rank)) continue;
                ++receivedRows;
                const uint64_t slot = uint64_t(row.token) * c.topK + row.k;
                Require(slot < uint64_t(c.bs) * c.topK && written.insert(slot).second,
                    "M3 reference DIRECT route is out of bounds or duplicates a source slot");
                std::vector<uint16_t> values(c.h);
                for (uint32_t column = 0; column < uint32_t(c.h); ++column) {
                    values[column] = generated ? CombineM4Reference::ExpertBits(shape.bf16, producer, originalRow, column, c.operatorMode == "both") :
                        HiddenBits(shape, row.source, row.token, column);
                }
                SetResultPayload(c, slot, values);
            }
        }
    }

    void SetResultPayload(const Config &c, uint64_t slot, const std::vector<uint16_t> &values)
    {
        const uint64_t stride = ((uint64_t(c.h) * 2U + 479U) / 480U) * 512U;
        const uint64_t start = slot * stride;
        for (uint32_t column = 0; column < uint32_t(c.h); ++column) {
            const uint64_t byte = uint64_t(column) * 2U;
            std::memcpy(expectedResult.data() + start + byte / 480U * 512U + byte % 480U, &values[column], 2U);
        }
        for (uint64_t block = 0; block < stride / 512U; ++block) {
            const uint32_t ready = 0x3f800000U;
            std::memcpy(expectedResult.data() + start + block * 512U + 480U, &ready, 4U);
            // Only the first FP32 is defined. Tail payload has no business meaning.
            std::fill_n(ignoredResult.begin() + start + block * 512U + 484U, 28U, 1U);
            const uint64_t valid = std::min<uint64_t>(480U, uint64_t(c.h) * 2U - block * 480U);
            std::fill_n(ignoredResult.begin() + start + block * 512U + valid, 480U - valid, 1U);
        }
    }

    std::string Details(const Config &c, uint64_t peerBytes, const OperatorProgress &dispatch,
        const OperatorProgress &combine, uint64_t synchronizations, bool valid = true) const
    {
        const bool both = c.operatorMode == "both";
        const char *native = "combine-m5";
        const char *checked = valid ? "true" : "false";
        std::ostringstream out;
        out << dispatch.Details("dispatch", peerBytes) << combine.Details("combine", peerBytes)
            << ",\"stream_synchronizations\":" << synchronizations
            << ",\"kernel_scope\":" << Json(both ? std::string("dispatch-") + native : native)
            << ",\"native_status\":" << Json(native)
            << ",\"combine_empty\":false,\"combine_delay_us\":0"
            << ",\"combine_observed_rounds\":" << observations
            << ",\"combine_checked_window_mask\":" << launchedWindows
            << ",\"combine_stage_validated\":" << checked << ",\"combine_control_validated\":" << checked
            << ",\"combine_control_reserved_unchanged\":" << checked
            << ",\"combine_result_validated\":" << checked << ",\"combine_result_flags_validated\":" << checked
            << ",\"combine_unwritten_results_unchanged\":" << checked
            << ",\"combine_token_num\":" << validatedTokenNum << ",\"combine_type3_rows\":" << type3Rows
            << ",\"combine_type1_rows\":" << directRows << ",\"combine_direct_received_rows\":" << receivedRows
            << ",\"combine_algorithm_bytes\":" << (type3Rows + directRows + partialRows + uint64_t(c.bs)) * c.h * 2U
            << ",\"combine_stage_copy_bytes\":" << type3Rows * c.h * 4U
            << ",\"combine_direct_payload_bytes\":" << directRows * c.h * 2U
            << ",\"combine_direct_wire_bytes\":" << directRows * ((c.h * 2U + 479U) / 480U) * 512U
            << ",\"combine_control_publish_bytes\":" << (TileXREp::EpServerDedupEnabled(c.bs) ? (64U * 8U + 8U * TileXREp::kEpServerDedupCombineRecvCoreNum) * 32U : 0U)
            << ",\"combine_input_check\":" << Json(both ? "dispatch-final-reference" : "full-snapshot")
            << ",\"combine_inputs_unchanged\":" << (!both && valid ? "true" : "false")
            << ",\"independent_expert_out_unchanged\":"
            << ((!both || c.quantMode != 0) && valid ? "true" : "false")
            << ",\"five_tables_linked\":" << (both ? "true" : "false")
            << ",\"send_counts_linked\":" << (both ? "true" : "false")
            << ",\"expert_out_source\":" << Json(!both ? "standalone_expert_out" :
                c.quantMode == 0 ? "dispatch_expand_x" : "separate_expert_out");
        out << ",\"combine_receive_module\":" << Json(TileXREp::EpServerDedupEnabled(c.bs) ? "M5" : "M6")
            << ",\"combine_y_out_validated\":" << checked
            << ",\"combine_result_flags_cleared\":" << checked
            << ",\"combine_final_read_bytes\":" << (receivedRows + partialReceivedRows) * c.h * 2U
            << ",\"combine_y_out_bytes\":" << uint64_t(c.bs) * c.h * 2U
            << ",\"combine_flag_clear_bytes\":" << uint64_t(c.bs) * c.topK * ((c.h * 2U + 479U) / 480U) * 32U;
        out << ",\"combine_type2_rows\":" << partialRows
            << ",\"combine_partial_received_rows\":" << partialReceivedRows
            << ",\"combine_partial_member_rows\":" << partialMembers
            << ",\"combine_partial_local_members\":" << localMembers
            << ",\"combine_partial_min_members\":" << minMembers
            << ",\"combine_partial_max_members\":" << maxMembers
            << ",\"combine_partial_payload_bytes\":" << partialRows * c.h * 2U
            << ",\"combine_partial_wire_bytes\":" << partialRows * ((c.h * 2U + 479U) / 480U) * 512U
            << ",\"combine_partial_self_read_bytes\":" << partialRows * c.h * 2U
            << ",\"combine_partial_stage_read_bytes\":" << partialMembers * c.h * 2U
            << ",\"combine_partial_validated\":" << checked
            << ",\"combine_numeric_policy\":\"fp32-sequential-rne-bit-exact\""
            << ",\"combine_numeric_atol\":0,\"combine_numeric_rtol\":0";
        return out.str();
    }

    void InitializePartial(const Config &c, const TileXR::CommArgs &args)
    {
        using namespace CombineM4Reference;
        Require(Encode(1.0f + std::ldexp(1.0f, -8), true) == 0x3f80U &&
            Encode(1.0f + 3.0f * std::ldexp(1.0f, -8), true) == 0x3f82U &&
            Encode(1.0f + std::ldexp(1.0f, -11), false) == 0x3c00U &&
            Encode(1.0f + 3.0f * std::ldexp(1.0f, -11), false) == 0x3c02U &&
            Encode(-0.0f, true) == 0x8000U && Encode(-0.0f, false) == 0x8000U,
            "M4 oracle RNE and signed-zero contract");
        InitializeDirect(c, args);
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
    std::vector<uint8_t> expectedY;

    void Initialize(const Config &c, const TileXR::CommArgs &args)
    {
        if (TileXREp::EpServerDedupEnabled(c.bs)) InitializePartial(c, args);
        else InitializeDirect(c, args);
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
        ValidateMemory(c, args, layout, buffers);
        CheckBytes(buffers[13], expectedY, "M5 yOut after intermediate partial and final RNE");
    }
};
