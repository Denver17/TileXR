// Included inside the integration consumer's namespace. No production debug ABI.
ServerDedupReference::Shape SourceSendShape(const Config &c)
{
    return {uint32_t(c.rankSize), uint32_t(c.experts), uint32_t(c.bs), uint32_t(c.h), uint32_t(c.topK),
        c.dtype == TileXR::TILEXR_DATA_TYPE_BFP16, c.quantMode == 4, c.fp8Format == "e4m3", c.routeMode, c.seed};
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
