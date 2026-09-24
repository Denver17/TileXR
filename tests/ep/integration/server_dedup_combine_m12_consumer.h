// P2.1 oracle for M1 payload and M2 ownership. No M3/M4/M5 numerical claims.

struct CombineM12Validation {
    TileXREp::EpServerDedupCombineDataLayout data {};
    std::vector<uint8_t> initialControl;
    // Optional M3 result oracle. Only semantically unspecified payload/Flag lanes are masked.
    std::vector<uint8_t> initialResult, expectedResult, ignoredResult;
    std::array<uint64_t, 2> generations {{0, 0}};
    uint32_t launchedWindows = 0;
    uint64_t observations = 0, type3Rows = 0, validatedTokenNum = 0;
    enum : uint8_t { guard = 0x6d };

    void Initialize(const Config &c, const TileXR::CommArgs &args)
    {
        const uint64_t slot = (uint64_t(c.h) * 2U + 479U) / 480U * 512U;
        Check(TileXREp::TileXREpBuildServerDedupCombineDataLayout(c.rankSize, c.bs, c.h,
            c.topK, c.experts, slot, &data), "P2.1 data layout");
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
                "seed P2.1 control ownership guards");
            // In both mode Dispatch owns window 1 and Combine owns window 0.
            // Do not seed or validate Dispatch's shared payload as Combine data.
            if (c.operatorMode == "both" && window == 1U) continue;
            Check(aclrtMemset(base + data.resultOffset, data.requiredWindowBytes - data.resultOffset,
                guard, data.requiredWindowBytes - data.resultOffset), "seed P2.1 data guards");
        }
    }

    void NoteLaunch(int64_t magic, uint64_t generation)
    {
        launchedWindows |= 1U << (magic % 2);
        generations[magic % 2] = generation;
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

    void Validate(const Config &c, const TileXR::CommArgs &args,
        const TileXREp::EpMemoryServerDedupCombineLayout &layout, const std::vector<void *> &buffers)
    {
        int32_t tokenNum = -1;
        Check(aclrtMemcpy(&tokenNum, sizeof(tokenNum), static_cast<int32_t *>(buffers[4]) + c.experts - 1,
            sizeof(tokenNum), ACL_MEMCPY_DEVICE_TO_HOST), "read P2.1 TokenNum");
        ValidateOutput(tokenNum >= 0 && tokenNum <= 10000 && uint64_t(tokenNum) <= layout.capacityRows,
            "P2.1 TokenNum outside supported range");
        std::vector<uint32_t> types(tokenNum), table2(uint64_t(tokenNum) * 3U);
        if (tokenNum) {
            if (TileXREp::kEpServerDedupEnable) Check(aclrtMemcpy(types.data(), types.size() * 4U, buffers[5], types.size() * 4U,
                ACL_MEMCPY_DEVICE_TO_HOST), "read P2.1 token types");
            Check(aclrtMemcpy(table2.data(), table2.size() * 4U, buffers[6], table2.size() * 4U,
                ACL_MEMCPY_DEVICE_TO_HOST), "read P2.1 table2");
        }
        validatedTokenNum = static_cast<uint64_t>(tokenNum);
        const bool separate = c.operatorMode == "combine" || c.quantMode != 0;
        const auto expert = static_cast<const uint8_t *>(buffers[separate ? 12U : 2U]);
        std::vector<uint8_t> expertSnapshot;
        const bool generatedOracle = c.operatorMode == "combine" && c.combineObservation == "per-round";
        if (tokenNum && !generatedOracle) {
            expertSnapshot.resize(uint64_t(tokenNum) * c.h * 2U);
            Check(aclrtMemcpy(expertSnapshot.data(), expertSnapshot.size(), expert, expertSnapshot.size(),
                ACL_MEMCPY_DEVICE_TO_HOST), "snapshot P2.1 valid expert rows");
        }
        type3Rows = 0;
        for (auto type : types) if (type == 3U) ++type3Rows;
        for (uint32_t window = 0; window < 2U; ++window) {
            const bool used = (launchedWindows & (1U << window)) != 0;
            auto base = args.peerMems[c.rank] + window * (args.peerMemBytes / 2U);
            auto control = initialControl;
            if (used && TileXREp::kEpServerDedupEnable) {
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
                    "P2.1 type3 stage slot is invalid or duplicated");
                auto target = expected.data() + data.stageOffset - data.resultOffset + slot * data.stageRowBytes;
                if (generatedOracle) {
                    const uint16_t one = c.dtype == TileXR::TILEXR_DATA_TYPE_BFP16 ? 0x3f80U : 0x3c00U;
                    for (int64_t column = 0; column < c.h; ++column) {
                        const uint16_t value = static_cast<uint16_t>(one +
                            (uint64_t(row) * 17U + column * 3U + c.rank * 11U + generations[window] * 29U) % 113U);
                        std::memcpy(target + column * 2U, &value, 2U);
                    }
                } else {
                    std::memcpy(target, expertSnapshot.data() + uint64_t(row) * c.h * 2U, c.h * 2U);
                }
            }
            CheckBytes(base + data.resultOffset, expected,
                "Combine stage/result/padding window=" + std::to_string(window),
                used ? &ignoredResult : nullptr);
        }
        ++observations;
    }

    std::string Details(const Config &c, uint64_t peerBytes, const OperatorProgress &dispatch,
        const OperatorProgress &combine, uint64_t synchronizations) const
    {
        const bool both = c.operatorMode == "both";
        std::ostringstream out;
        out << dispatch.Details("dispatch", peerBytes) << combine.Details("combine", peerBytes)
            << ",\"stream_synchronizations\":" << synchronizations
            << ",\"kernel_scope\":" << Json(both ? "dispatch-combine-m12" : "combine-m12")
            << ",\"native_status\":\"combine-m12\""
            << ",\"combine_empty\":false,\"combine_delay_us\":50"
            << ",\"combine_observed_rounds\":" << observations
            << ",\"combine_checked_window_mask\":" << launchedWindows
            << ",\"combine_stage_validated\":true,\"combine_control_validated\":true"
            << ",\"combine_control_reserved_unchanged\":true,\"combine_result_unchanged\":true"
            << ",\"combine_token_num\":" << validatedTokenNum
            << ",\"combine_type3_rows\":" << type3Rows
            << ",\"combine_algorithm_bytes\":" << type3Rows * c.h * 2U
            << ",\"combine_stage_copy_bytes\":" << type3Rows * c.h * 4U
            << ",\"combine_control_publish_bytes\":" << (TileXREp::kEpServerDedupEnable ? (64U * 8U + 8U * TileXREp::kEpServerDedupCombineRecvCoreNum) * 32U : 0U)
            << ",\"combine_input_check\":" << Json(both ? "dispatch-final-reference" : "full-snapshot")
            << ",\"combine_inputs_unchanged\":" << (both ? "false" : "true")
            << ",\"y_out_unchanged\":true,\"independent_expert_out_unchanged\":"
            << (!both || c.quantMode != 0 ? "true" : "false")
            << ",\"five_tables_linked\":" << (both ? "true" : "false")
            << ",\"send_counts_linked\":" << (both ? "true" : "false")
            << ",\"expert_out_source\":" << Json(!both ? "standalone_expert_out" :
                c.quantMode == 0 ? "dispatch_expand_x" : "separate_expert_out");
        return out.str();
    }
};
