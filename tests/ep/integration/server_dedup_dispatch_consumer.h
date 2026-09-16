// Included inside the integration consumer namespace. No production debug ABI.
std::string ValidateDispatch(const Config &c, const TileXR::CommArgs &args,
    const TileXREp::EpMemoryServerDedupLayout &layout, const std::vector<void *> &buffers,
    const std::vector<float> &originalWeights, int64_t magic)
{
    using namespace ServerDedupReference;
    const auto shape = SourceSendShape(c);
    const uint64_t window = TileXREp::TileXREpServerDedupWindowOffset(magic);
    const auto rows = DispatchRows(shape, c.rank);
    Require(rows.size() <= layout.capacityRows, "reference exceeds public row capacity");
    const uint64_t hiddenStride = uint64_t(shape.h) * (shape.quantized ? 1U : 2U);
    const uint64_t scaleStride = layout.data.scaleBytes;
    std::vector<uint8_t> hidden(rows.size() * hiddenStride), scales(rows.size() * scaleStride);
    std::vector<uint32_t> assist(rows.size() * 8U), mask(shape.bs), expectedMask(shape.bs, 0U);
    std::vector<float> weights(originalWeights.size()), weightsOut(originalWeights.size());
    const auto read = [&](void *target, uint64_t bytes, const void *source, const char *name) {
        if (bytes) Check(aclrtMemcpy(target, bytes, source, bytes, ACL_MEMCPY_DEVICE_TO_HOST), name);
    };
    read(hidden.data(), hidden.size(), buffers[2], "read dispatch hidden");
    read(scales.data(), scales.size(), buffers[10], "read dispatch scales");
    read(assist.data(), assist.size() * sizeof(uint32_t), buffers[5], "read dispatch assistEx");
    read(mask.data(), layout.sourceMaskBytes, buffers[6], "read dispatch sourceMask");
    read(weights.data(), layout.weightBytes, buffers[8], "read input weights");
    read(weightsOut.data(), layout.weightBytes, buffers[9], "read dispatch source weights");
    ValidateOutput(std::memcmp(weights.data(), originalWeights.data(), layout.weightBytes) == 0, "input weights modified");
    uint64_t firstHopSent = 0, firstHopReceived = 0, directSent = 0, directReceived = 0;
    uint64_t relaySent = 0, relayReceived = 0, localBytes = 0, sameServerBytes = 0, crossServerBytes = 0;
    const auto traffic = [&](uint32_t destination) {
        if (destination == uint32_t(c.rank)) localBytes += layout.data.tokenStride;
        else if (destination / 8U == uint32_t(c.rank) / 8U) sameServerBytes += layout.data.tokenStride;
        else crossServerBytes += layout.data.tokenStride;
    };
    const auto packets = SourcePackets(Ids(shape, c.rank), originalWeights, c.rank, shape.ranks, shape.experts, shape.k);
    for (const auto &packet : packets) {
        const auto &first = packet.members.front();
        const bool direct = packet.members.size() == 1U;
        if (direct) ++directSent;
        else ++firstHopSent;
        traffic(first.expert / uint32_t(layout.localExpertNum));
        expectedMask[packet.token] |= uint32_t(1) << first.k;
        ValidateOutput(FloatBits(weightsOut[packet.token * shape.k + first.k]) == FloatBits(direct ? first.weight : 1.0f),
            "source weight mismatch token=" + std::to_string(packet.token) + " k=" + std::to_string(first.k));
    }
    ValidateOutput(mask == expectedMask, "sourceMask mismatch");
    std::set<uint32_t> groupIds;
    std::ostringstream links;
    bool firstLink = true;
    for (size_t index = 0; index < rows.size(); ++index) {
        const auto &row = rows[index];
        const std::string context = " row=" + std::to_string(index) + " source=" + std::to_string(row.source) +
            " token=" + std::to_string(row.token) + " expert=" + std::to_string(row.expert) + " k=" + std::to_string(row.k);
        const auto payload = Payload(shape, row.source, row.packet, layout.data);
        const auto *actualHidden = hidden.data() + index * hiddenStride;
        if (std::memcmp(actualHidden, payload.data(), hiddenStride) != 0) {
            uint64_t first = 0, mismatches = 0;
            while (actualHidden[first] == payload[first]) ++first;
            for (uint64_t byte = 0; byte < hiddenStride; ++byte)
                mismatches += actualHidden[byte] != payload[byte];
            std::ostringstream detail;
            detail << "dispatch hidden mismatch" << context << " q=" << row.q
                   << " route=" << (row.Direct() ? "direct" : row.FirstHop() ? "first-hop" : "relay")
                   << " byte=" << first << " expected=" << unsigned(payload[first])
                   << " actual=" << unsigned(actualHidden[first]) << " mismatches=" << mismatches;
            if (shape.quantized)
                detail << " scale_match=" << (std::memcmp(scales.data() + index * scaleStride,
                    payload.data() + layout.data.quantCount, scaleStride) == 0);
            const auto expectedAssist = DispatchAssist(row, assist[index * 8U + 7U]);
            detail << " assist_actual=[";
            for (uint32_t word = 0; word < 8U; ++word)
                detail << (word == 0U ? "" : ",") << assist[index * 8U + word];
            detail << "] assist_expected=[";
            for (uint32_t word = 0; word < 8U; ++word)
                detail << (word == 0U ? "" : ",") << expectedAssist[word];
            detail << ']';
            // Read only after batch synchronization. Cleared ready flags do not
            // erase payload; compare logical bytes while skipping the flag gaps.
            try {
                std::vector<uint8_t> wire(layout.data.tokenStride);
                read(wire.data(), wire.size(), args.peerMems[c.rank] + window +
                    DispatchSlot(shape, c.rank, row, layout.data), "read mismatched dispatch slot");
                bool wireHiddenMatch = true, wireOutputMatch = true;
                for (uint64_t byte = 0; byte < hiddenStride; ++byte) {
                    const auto value = wire[byte / 480U * 512U + byte % 480U];
                    wireHiddenMatch &= value == payload[byte];
                    wireOutputMatch &= value == actualHidden[byte];
                }
                detail << " wire_hidden_match=" << wireHiddenMatch
                       << " wire_output_match=" << wireOutputMatch
                       << " wire_byte=" << unsigned(wire[first / 480U * 512U + first % 480U]);
                if (shape.quantized) {
                    bool wireScaleMatch = true;
                    for (uint64_t byte = layout.data.quantCount;
                         byte < layout.data.quantCount + scaleStride; ++byte)
                        wireScaleMatch &= wire[byte / 480U * 512U + byte % 480U] == payload[byte];
                    detail << " wire_scale_match=" << wireScaleMatch;
                }
            } catch (const std::exception &error) {
                // Diagnostic readback must not replace the original mismatch.
                detail << " wire_read_error=" << error.what();
            }
            ValidateOutput(false, detail.str());
        }
        if (shape.quantized) ValidateOutput(std::memcmp(scales.data() + index * scaleStride,
            payload.data() + layout.data.quantCount, scaleStride) == 0, "dispatch scale mismatch" + context);
        const uint32_t t = assist[index * 8U + 7U];
        if (!row.Direct()) ValidateOutput(t < 8U * shape.bs, "assistEx t outside group capacity" + context);
        const auto expected = DispatchAssist(row, t);
        for (uint32_t word = 0; word < 8U; ++word)
            ValidateOutput(assist[index * 8U + word] == expected[word], "assistEx mismatch word=" + std::to_string(word) + context);
        if (row.Direct()) ++directReceived;
        else {
            if (!firstLink) links << ',';
            firstLink = false;
            links << '[' << row.source << ',' << row.token << ',' << row.packet.members.front().k << ',' << row.relay << ','
                << t << ',' << row.k << ',' << (row.FirstHop() ? int64_t(row.packet.members.size() - 1U) : -1) << ']';
            if (row.FirstHop()) {
                ++firstHopReceived;
                ValidateOutput(groupIds.insert(t).second, "duplicate first-hop group t" + context);
                const uint32_t n = uint32_t(row.packet.members.size() - 1U);
                std::vector<uint32_t> group(1U + 2U * n);
                read(group.data(), group.size() * sizeof(uint32_t),
                    static_cast<const uint8_t *>(buffers[7]) + t * layout.groupRowStrideBytes, "read valid dispatch group");
                ValidateOutput(group[0] == index, "group firstHopRow mismatch" + context);
                for (uint32_t member = 0; member < n; ++member) {
                    const auto &route = row.packet.members[member + 1U];
                    ValidateOutput(group[1U + member * 2U] == t * shape.k + route.k, "group recvSlot mismatch" + context);
                    ValidateOutput(group[2U + member * 2U] == FloatBits(route.weight), "group weight mismatch" + context);
                    ++relaySent;
                    traffic(route.expert / uint32_t(layout.localExpertNum));
                }
            } else ++relayReceived;
        }
        // After complete Dispatch every original route has been consumed. Check
        // all eight words in every Flag, including metadata-only tail blocks.
        std::vector<uint8_t> wire(layout.data.tokenStride);
        read(wire.data(), wire.size(), args.peerMems[c.rank] + window + DispatchSlot(shape, c.rank, row, layout.data),
            "read consumed dispatch slot");
        for (uint64_t block = 0; block < layout.data.blockCount; ++block) {
            for (uint32_t word = 0; word < 8U; ++word) {
                uint32_t flag;
                std::memcpy(&flag, wire.data() + block * 512U + 480U + word * 4U, sizeof(flag));
                ValidateOutput(flag == 0U, "dispatch Flag not cleared block=" + std::to_string(block) +
                    " word=" + std::to_string(word) + context);
            }
        }
    }
    uint32_t counter = UINT32_MAX;
    read(&counter, sizeof(counter), args.peerMems[c.rank] + window +
        TileXREp::TileXREpServerDedupCountLayout(shape.experts).counterOffset, "read cleared relay counter");
    ValidateOutput(counter == 0U, "relay counter not cleared");
    return ",\"last_magic\":" + std::to_string(magic) +
        ",\"window_offset\":" + std::to_string(window) + ",\"kernel_scope\":\"dispatch\",\"native_status\":\"dispatch\",\"first_hop_packets\":" + std::to_string(firstHopSent) +
        ",\"first_hop_received\":" + std::to_string(firstHopReceived) + ",\"token_stride\":" + std::to_string(layout.data.tokenStride) +
        ",\"first_hop_bytes\":" + std::to_string(firstHopSent * layout.data.tokenStride) +
        ",\"direct_packets\":" + std::to_string(directSent) + ",\"direct_received\":" + std::to_string(directReceived) +
        ",\"direct_bytes\":" + std::to_string(directSent * layout.data.tokenStride) +
        ",\"relay_packets\":" + std::to_string(relaySent) + ",\"relay_received\":" + std::to_string(relayReceived) +
        ",\"relay_bytes\":" + std::to_string(relaySent * layout.data.tokenStride) +
        ",\"data_bytes\":" + std::to_string((firstHopSent + directSent + relaySent) * layout.data.tokenStride) +
        ",\"local_data_bytes\":" + std::to_string(localBytes) + ",\"same_server_data_bytes\":" + std::to_string(sameServerBytes) +
        ",\"cross_server_data_bytes\":" + std::to_string(crossServerBytes) + ",\"relay_links\":[" + links.str() + ']' +
        ",\"first_hop_validated\":true,\"direct_validated\":true,\"relay_validated\":true,\"route_coverage_validated\":true"
        ",\"source_outputs_validated\":true,\"weights_unchanged\":true,\"dispatch_outputs_validated\":true,\"group_validated\":true"
        ",\"flags_cleared\":true,\"counter_cleared\":true";
}
