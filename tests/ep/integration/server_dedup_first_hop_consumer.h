// Included inside the integration consumer's namespace. No production debug ABI.
ServerDedupReference::Shape FirstHopShape(const Config &c)
{
    return {uint32_t(c.rankSize), uint32_t(c.experts), uint32_t(c.bs), uint32_t(c.h), uint32_t(c.topK),
        c.dtype == TileXR::TILEXR_DATA_TYPE_BFP16, c.quantMode == 4, c.fp8Format == "e4m3"};
}

void SaveFirstHopInputs(const Config &c, const std::vector<uint16_t> &x,
    const std::vector<int32_t> &ids, const std::vector<float> &weights)
{
    const char *directory = std::getenv("TILEXR_VALIDATION_INPUT_DIR");
    if (directory == nullptr) return; // Direct executable callers may opt out of retaining inputs.
    const std::string stem = std::string(directory) + "/rank-" + std::to_string(c.rank);
    const auto write = [&](const char *suffix, const void *data, size_t bytes) {
        std::ofstream out(stem + suffix, std::ios::binary);
        out.write(static_cast<const char *>(data), bytes);
        Require(bool(out), "cannot retain first-hop inputs: " + stem + suffix);
    };
    write("-x.bin", x.data(), x.size() * sizeof(uint16_t));
    write("-ids.bin", ids.data(), ids.size() * sizeof(int32_t));
    write("-weights.bin", weights.data(), weights.size() * sizeof(float));
}

std::string ValidateFirstHop(const Config &c, const TileXR::CommArgs &args,
    const TileXREp::EpMemoryServerDedupLayout &layout, const std::vector<void *> &buffers,
    const std::vector<float> &originalWeights)
{
    using namespace ServerDedupReference;
    const auto shape = FirstHopShape(c);
    const uint32_t localExperts = shape.experts / shape.ranks;
    uint64_t sent = 0, received = 0;
    std::vector<uint32_t> expectedMask(shape.bs, 0), actualMask(shape.bs);
    std::vector<float> weights(originalWeights.size()), weightsOut(originalWeights.size());
    Check(aclrtMemcpy(actualMask.data(), layout.sourceMaskBytes, buffers[6], layout.sourceMaskBytes,
        ACL_MEMCPY_DEVICE_TO_HOST), "read sourceMaskOut");
    Check(aclrtMemcpy(weights.data(), layout.weightBytes, buffers[8], layout.weightBytes,
        ACL_MEMCPY_DEVICE_TO_HOST), "read original weights");
    Check(aclrtMemcpy(weightsOut.data(), layout.weightBytes, buffers[9], layout.weightBytes,
        ACL_MEMCPY_DEVICE_TO_HOST), "read source weights");
    Require(std::memcmp(weights.data(), originalWeights.data(), layout.weightBytes) == 0, "input weights modified");
    for (uint32_t source = 0; source < shape.ranks; ++source) {
        const auto ids = Ids(shape, source);
        const auto packets = FirstHops(ids, Weights(shape, source), source, shape.ranks, shape.experts, shape.k);
        std::set<uint64_t> expectedSlots;
        for (const auto &packet : packets) {
            const auto &first = packet.members[0];
            if (source == uint32_t(c.rank)) {
                ++sent;
                expectedMask[packet.token] |= uint32_t(1) << first.k;
                Require(weightsOut[packet.token * shape.k + first.k] == 1.0f,
                    "source weight must be 1.0 token=" + std::to_string(packet.token) + " k=" + std::to_string(first.k));
            }
            if (first.expert / localExperts != uint32_t(c.rank)) continue;
            const auto offset = TileXREp::TileXREpServerDedupDispatchOffset(layout.data, source, c.rank,
                first.expert % localExperts, first.q, shape.ranks, localExperts, shape.bs);
            Require(expectedSlots.insert(offset).second, "duplicate expected first-hop slot");
            std::vector<uint8_t> actual(layout.data.tokenStride);
            Check(aclrtMemcpy(actual.data(), actual.size(), args.peerMems[c.rank] + offset,
                actual.size(), ACL_MEMCPY_DEVICE_TO_HOST), "read first-hop packet");
            CheckPacket(shape, source, packet, layout.data, actual);
            ++received;
        }
        // Check all other original local route slots: neither DIRECT nor relay
        // entries have been sent in this checkpoint. Fresh communicator flags are zero.
        std::vector<uint32_t> q(shape.experts, 0);
        for (int32_t expert : ids) {
            const uint32_t originalQ = q[expert]++;
            if (uint32_t(expert) / localExperts != uint32_t(c.rank)) continue;
            const auto offset = TileXREp::TileXREpServerDedupDispatchOffset(layout.data, source, c.rank,
                expert % localExperts, originalQ, shape.ranks, localExperts, shape.bs);
            if (expectedSlots.count(offset)) continue;
            std::array<uint32_t, 8> flags {};
            Check(aclrtMemcpy(flags.data(), sizeof(flags), args.peerMems[c.rank] + offset + 480U,
                sizeof(flags), ACL_MEMCPY_DEVICE_TO_HOST), "read inactive slot flags");
            for (uint32_t flag : flags) Require(flag == 0, "unexpected DIRECT/relay slot source=" + std::to_string(source) +
                " expert=" + std::to_string(expert) + " q=" + std::to_string(originalQ));
        }
    }
    for (uint32_t token = 0; token < shape.bs; ++token)
        Require(actualMask[token] == expectedMask[token], "sourceMask mismatch token=" + std::to_string(token) +
            " expected=" + std::to_string(expectedMask[token]) + " actual=" + std::to_string(actualMask[token]));
    return ",\"kernel_scope\":\"first_hop\",\"native_status\":\"first_hop\",\"first_hop_packets\":" + std::to_string(sent) +
        ",\"first_hop_received\":" + std::to_string(received) + ",\"token_stride\":" + std::to_string(layout.data.tokenStride) +
        ",\"first_hop_bytes\":" + std::to_string(sent * layout.data.tokenStride) +
        ",\"direct_packets\":0,\"first_hop_validated\":true,\"source_outputs_validated\":true,\"weights_unchanged\":true";
}

std::string ReadFirstHopProfile(const Config &c, const TileXR::CommArgs &args)
{
    using namespace TileXREp;
    std::array<EpServerDedupProfileRecord, kEpServerDedupCoreNum * kEpServerDedupProfileSegments> records {};
    Check(aclrtMemcpy(records.data(), sizeof(records), args.peerMems[c.rank], sizeof(records), ACL_MEMCPY_DEVICE_TO_HOST),
        "read first-hop breakdown");
    const uint64_t generation = 1;
    const char *points[] = {"start", "init_end", "send_cnt_end", "send_data_end", "recv_cnt_end", "cumsum_end", "prefix_end", "end"};
    std::ostringstream out;
    out << ",\"schema_version\":" << kEpServerDedupProfileVersion << ",\"kernel_scope\":\"first_hop\",\"generation\":" << generation
        << ",\"block_dim\":" << kEpServerDedupCoreNum << ",\"records\":[";
    for (uint32_t core = 0; core < kEpServerDedupCoreNum; ++core) {
        uint64_t time[8] {};
        for (uint32_t segment = 0; segment < 2; ++segment) {
            const auto &r = records[core * 2U + segment];
            Require(r.marker == kEpServerDedupProfileMarker && r.version == kEpServerDedupProfileVersion &&
                r.recordBytes == sizeof(r) && r.rank == uint32_t(c.rank) && r.core == core &&
                r.blockDim == kEpServerDedupCoreNum && r.segment == segment && r.generation == generation,
                "invalid first-hop breakdown core=" + std::to_string(core) + " segment=" + std::to_string(segment));
            std::copy(r.time, r.time + 4, time + segment * 4U);
        }
        Require(time[0] > 0 && time[7] > time[0], "invalid first-hop breakdown duration");
        if (core != 0) out << ',';
        out << "{\"core\":" << core;
        for (uint32_t i = 0; i < 8; ++i) {
            Require(i == 0 || time[i] >= time[i - 1], "unordered first-hop breakdown timestamps");
            out << ",\"" << points[i] << "\":" << time[i];
        }
        out << '}';
    }
    return out.str() + ']';
}
