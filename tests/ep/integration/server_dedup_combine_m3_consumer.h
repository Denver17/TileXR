// M3 source-side oracle reconstructed from original global routes. It never
// uses the producer's actual table2, packed index or result-address helper.
struct CombineM3Validation : CombineM12Validation {
    uint64_t directRows = 0, receivedRows = 0;
    uint64_t partialRows = 0, partialReceivedRows = 0, partialMembers = 0, localMembers = 0, minMembers = 0, maxMembers = 0;

    void Initialize(const Config &c, const TileXR::CommArgs &args)
    {
        CombineM12Validation::Initialize(c, args);
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
        if (c.operatorMode == "combine" && c.checkpoint != "ep-sdedup-combine-p2-m4") shape.routeMode = TileXREpDemo::RouteMode::Balance;
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
                    const uint16_t one = shape.bf16 ? 0x3f80U : 0x3c00U;
                    values[column] = generated ? (c.checkpoint == "ep-sdedup-combine-p2-m4" ?
                        CombineM4Reference::ExpertBits(shape.bf16, producer, originalRow, column, c.operatorMode == "both") :
                        static_cast<uint16_t>(one + (originalRow * 17U + column * 3U + producer * 11U) % 113U)) :
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
        const bool m4 = c.checkpoint == "ep-sdedup-combine-p2-m4";
        const char *checked = valid ? "true" : "false";
        std::ostringstream out;
        out << dispatch.Details("dispatch", peerBytes) << combine.Details("combine", peerBytes)
            << ",\"stream_synchronizations\":" << synchronizations
            << ",\"kernel_scope\":" << Json(m4 ? (both ? "dispatch-combine-m4" : "combine-m4") :
                (both ? "dispatch-combine-m3" : "combine-m3"))
            << ",\"native_status\":" << Json(m4 ? "combine-m4" : "combine-m3")
            << ",\"combine_empty\":false,\"combine_delay_us\":0"
            << ",\"combine_observed_rounds\":" << observations
            << ",\"combine_checked_window_mask\":" << launchedWindows
            << ",\"combine_stage_validated\":" << checked << ",\"combine_control_validated\":" << checked
            << ",\"combine_control_reserved_unchanged\":" << checked
            << ",\"combine_result_validated\":" << checked << ",\"combine_result_flags_validated\":" << checked
            << ",\"combine_unwritten_results_unchanged\":" << checked
            << ",\"combine_token_num\":" << validatedTokenNum << ",\"combine_type3_rows\":" << type3Rows
            << ",\"combine_type1_rows\":" << directRows << ",\"combine_direct_received_rows\":" << receivedRows
            << ",\"combine_algorithm_bytes\":" << (type3Rows + directRows + partialRows) * c.h * 2U
            << ",\"combine_stage_copy_bytes\":" << type3Rows * c.h * 4U
            << ",\"combine_direct_payload_bytes\":" << directRows * c.h * 2U
            << ",\"combine_direct_wire_bytes\":" << directRows * ((c.h * 2U + 479U) / 480U) * 512U
            << ",\"combine_control_publish_bytes\":" << (64U * 8U + 8U * TileXREp::kEpServerDedupCombineRecvCoreNum) * 32U
            << ",\"combine_input_check\":" << Json(both ? "dispatch-final-reference" : "full-snapshot")
            << ",\"combine_inputs_unchanged\":" << (!both && valid ? "true" : "false")
            << ",\"y_out_unchanged\":" << checked << ",\"independent_expert_out_unchanged\":"
            << ((!both || c.quantMode != 0) && valid ? "true" : "false")
            << ",\"five_tables_linked\":" << (both ? "true" : "false")
            << ",\"send_counts_linked\":" << (both ? "true" : "false")
            << ",\"expert_out_source\":" << Json(!both ? "standalone_expert_out" :
                c.quantMode == 0 ? "dispatch_expand_x" : "separate_expert_out");
        if (m4) out << ",\"combine_type2_rows\":" << partialRows
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
};
