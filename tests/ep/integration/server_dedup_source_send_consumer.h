// Included inside the integration consumer's namespace. No production debug ABI.
ServerDedupReference::Shape SourceSendShape(const Config &c)
{
    return {uint32_t(c.rankSize), uint32_t(c.experts), uint32_t(c.bs), uint32_t(c.h), uint32_t(c.topK),
        c.dtype == TileXR::TILEXR_DATA_TYPE_BFP16, c.quantMode == 4, c.fp8Format == "e4m3"};
}

void SaveSourceSendInputs(const Config &c, const std::vector<uint16_t> &x,
    const std::vector<int32_t> &ids, const std::vector<float> &weights)
{
    const char *directory = std::getenv("TILEXR_VALIDATION_INPUT_DIR");
    if (directory == nullptr) return; // Direct executable callers may opt out of retaining inputs.
    const std::string stem = std::string(directory) + "/rank-" + std::to_string(c.rank);
    const auto write = [&](const char *suffix, const void *data, size_t bytes) {
        std::ofstream out(stem + suffix, std::ios::binary);
        out.write(static_cast<const char *>(data), bytes);
        Require(bool(out), "cannot retain source-send inputs: " + stem + suffix);
    };
    write("-x.bin", x.data(), x.size() * sizeof(uint16_t));
    write("-ids.bin", ids.data(), ids.size() * sizeof(int32_t));
    write("-weights.bin", weights.data(), weights.size() * sizeof(float));
}

std::string ReadDispatchProfile(const Config &c, const TileXR::CommArgs &args)
{
    using namespace TileXREp;
    std::array<EpServerDedupProfileRecord, kEpServerDedupCoreNum * kEpServerDedupProfileSegments> records {};
    Check(aclrtMemcpy(records.data(), sizeof(records), args.peerMems[c.rank], sizeof(records), ACL_MEMCPY_DEVICE_TO_HOST),
        "read dispatch breakdown");
    const auto detailLayout = TileXREpServerDedupSendProfileLayout(args.peerMemBytes, uint32_t(c.bs), uint32_t(c.topK));
    const auto dataLayout = TileXREpServerDedupDataLayout(c.bs, c.h, c.topK, c.experts, c.quantMode == 4);
    Require(detailLayout.offset >= dataLayout.requiredBytes, "send profile overlaps data or communicator flags");
    std::vector<uint8_t> detail(detailLayout.bytes);
    Check(aclrtMemcpy(detail.data(), detail.size(), args.peerMems[c.rank] + detailLayout.offset,
        detail.size(), ACL_MEMCPY_DEVICE_TO_HOST), "read send issue breakdown");
    const uint64_t generation = uint64_t(c.warmup) + c.loops;
    const char *points[] = {"start", "init_end", "send_cnt_end", "send_data_end", "recv_cnt_end", "cumsum_end", "prefix_end", "recv_data_end", "end"};
    std::ostringstream out;
    out << ",\"schema_version\":" << kEpServerDedupProfileVersion << ",\"kernel_scope\":\"dispatch\",\"generation\":" << generation
        << ",\"block_dim\":" << kEpServerDedupCoreNum << ",\"records\":[";
    for (uint32_t core = 0; core < kEpServerDedupCoreNum; ++core) {
        uint64_t time[kEpServerDedupProfilePoints] {};
        for (uint32_t segment = 0; segment < kEpServerDedupProfileSegments; ++segment) {
            const auto &r = records[core * kEpServerDedupProfileSegments + segment];
            Require(r.marker == kEpServerDedupProfileMarker && r.version == kEpServerDedupProfileVersion &&
                r.recordBytes == sizeof(r) && r.rank == uint32_t(c.rank) && r.core == core &&
                r.blockDim == kEpServerDedupCoreNum && r.segment == segment && r.generation == generation,
                "invalid dispatch breakdown core=" + std::to_string(core) + " segment=" + std::to_string(segment));
            std::copy(r.time, r.time + 4, time + segment * 4U);
        }
        Require(time[0] > 0 && time[8] > time[0], "invalid dispatch breakdown duration");
        for (uint32_t i = 9; i < kEpServerDedupProfilePoints; ++i)
            Require(time[i] == time[8], "invalid reserved breakdown timestamp");
        if (core != 0) out << ',';
        out << "{\"core\":" << core;
        for (uint32_t i = 0; i < 9; ++i) {
            Require(i == 0 || time[i] >= time[i - 1], "unordered dispatch breakdown timestamps");
            out << ",\"" << points[i] << "\":" << time[i];
        }
        if (core < kEpServerDedupSendCoreNum) {
            EpServerDedupSendProfileHeader header {};
            const auto *base = detail.data() + core * detailLayout.coreStride;
            std::memcpy(&header, base, sizeof(header));
            const uint32_t count = c.bs / kEpServerDedupSendCoreNum + (core < c.bs % kEpServerDedupSendCoreNum);
            const uint32_t start = core * (c.bs / kEpServerDedupSendCoreNum) +
                std::min(core, uint32_t(c.bs % kEpServerDedupSendCoreNum));
            Require(header.marker == kEpServerDedupProfileMarker && header.version == kEpServerDedupProfileVersion &&
                header.recordBytes == sizeof(EpServerDedupSendProfilePacket) && header.rank == uint32_t(c.rank) &&
                header.core == core && header.generation == generation && header.packetCount <= detailLayout.capacity &&
                header.packetCount >= count && header.packetCount <= count * c.topK && header.firstHopCount <= header.packetCount,
                "invalid send issue header core=" + std::to_string(core));
            out << ",\"first_hop_count\":" << header.firstHopCount << ",\"send_packets\":[";
            uint64_t previous = time[2];
            std::set<uint64_t> routes;
            for (uint32_t packet = 0; packet < header.packetCount; ++packet) {
                EpServerDedupSendProfilePacket p {};
                std::memcpy(&p, base + sizeof(header) + packet * sizeof(p), sizeof(p));
                Require(p.issue >= previous && p.issue <= time[3] && p.token >= start && p.token < start + count &&
                    p.k < uint32_t(c.topK) && routes.insert(uint64_t(p.token) * c.topK + p.k).second,
                    "invalid send issue packet core=" + std::to_string(core) + " packet=" + std::to_string(packet));
                if (packet != 0U) out << ',';
                out << "{\"issue\":" << p.issue << ",\"token\":" << p.token << ",\"k\":" << p.k << '}';
                previous = p.issue;
            }
            out << ']';
        }
        out << '}';
    }
    return out.str() + ']';
}
