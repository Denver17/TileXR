/*
 * Copyright (c) 2026 TileXR Project
 * Licensed under the Apache License, Version 2.0
 */

#include "ccu/tilexr_ccu_topology.h"

#include "tilexr_types.h"

#include <cctype>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <regex>
#include <unordered_map>

namespace TileXR {
namespace {

constexpr const char* TILEXR_CCU_DIRECT_FORCE_TP_TYPE_ENV =
    "TILEXR_CCU_DIRECT_FORCE_TP_TYPE";

struct RootInfo {
    std::string topoPath;
    std::unordered_map<uint32_t, uint32_t> deviceToLocalId;
    std::unordered_map<uint32_t, std::unordered_map<std::string,
        std::array<uint8_t, TILEXR_CCU_EID_BYTES>>> portToEidByLocalId;
};

struct TopoEdge {
    uint32_t localA = 0;
    uint32_t localB = 0;
    std::vector<std::string> localAPorts;
    std::vector<std::string> localBPorts;
    bool supportsCtp = false;
};

std::string ReadTextFile(const std::string& path)
{
    std::ifstream input(path);
    if (!input.is_open()) {
        return {};
    }
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

bool ParseUint(const std::string& value, uint32_t* out)
{
    if (value.empty() || out == nullptr) {
        return false;
    }
    char* end = nullptr;
    const unsigned long parsed = std::strtoul(value.c_str(), &end, 10);
    if (end == value.c_str() || *end != '\0' || parsed > UINT32_MAX) {
        return false;
    }
    *out = static_cast<uint32_t>(parsed);
    return true;
}

std::string JsonStringField(const std::string& object, const std::string& field)
{
    const std::regex pattern("\"" + field + "\"\\s*:\\s*\"([^\"]*)\"");
    std::smatch match;
    return std::regex_search(object, match, pattern) ? match[1].str() : std::string();
}

bool JsonUintField(const std::string& object, const std::string& field, uint32_t* out)
{
    const std::regex quoted("\"" + field + "\"\\s*:\\s*\"([0-9]+)\"");
    const std::regex plain("\"" + field + "\"\\s*:\\s*([0-9]+)");
    std::smatch match;
    if (std::regex_search(object, match, quoted) || std::regex_search(object, match, plain)) {
        return ParseUint(match[1].str(), out);
    }
    return false;
}

bool ParseEidHex(const std::string& text, std::array<uint8_t, TILEXR_CCU_EID_BYTES>* eid)
{
    if (eid == nullptr || text.size() != eid->size() * 2U) {
        return false;
    }
    for (size_t i = 0; i < eid->size(); ++i) {
        const char hi = text[i * 2U];
        const char lo = text[i * 2U + 1U];
        if (!std::isxdigit(static_cast<unsigned char>(hi)) ||
            !std::isxdigit(static_cast<unsigned char>(lo))) {
            return false;
        }
        (*eid)[i] = static_cast<uint8_t>(
            std::strtoul(text.substr(i * 2U, 2U).c_str(), nullptr, 16));
    }
    return true;
}

std::vector<std::string> JsonStringArrayField(const std::string& object, const std::string& field)
{
    const std::regex arrayPattern("\"" + field + "\"\\s*:\\s*\\[([^\\]]*)\\]");
    std::smatch arrayMatch;
    if (!std::regex_search(object, arrayMatch, arrayPattern)) {
        return {};
    }
    const std::string body = arrayMatch[1].str();
    std::vector<std::string> values;
    const std::regex valuePattern("\"([^\"]*)\"");
    for (auto it = std::sregex_iterator(body.begin(), body.end(), valuePattern);
         it != std::sregex_iterator(); ++it) {
        values.push_back((*it)[1].str());
    }
    return values;
}

std::vector<std::string> ExtractObjectsWithKey(const std::string& text, const std::string& key)
{
    std::vector<std::string> objects;
    const std::string needle = "\"" + key + "\"";
    size_t pos = 0;
    while ((pos = text.find(needle, pos)) != std::string::npos) {
        const size_t begin = text.rfind('{', pos);
        if (begin == std::string::npos) {
            ++pos;
            continue;
        }
        int depth = 0;
        bool inString = false;
        bool escaped = false;
        for (size_t i = begin; i < text.size(); ++i) {
            const char ch = text[i];
            if (inString) {
                escaped = !escaped && ch == '\\';
                if (ch == '"' && !escaped) {
                    inString = false;
                } else if (ch != '\\') {
                    escaped = false;
                }
                continue;
            }
            if (ch == '"') {
                inString = true;
            } else if (ch == '{') {
                ++depth;
            } else if (ch == '}') {
                --depth;
                if (depth == 0) {
                    objects.emplace_back(text.substr(begin, i - begin + 1U));
                    pos = i + 1U;
                    break;
                }
            }
        }
        if (depth != 0) {
            break;
        }
    }
    return objects;
}

bool ParseRootInfo(const std::string& path, RootInfo* root)
{
    if (root == nullptr) {
        return false;
    }
    const std::string content = ReadTextFile(path);
    root->topoPath = JsonStringField(content, "topo_file_path");
    if (content.empty() || root->topoPath.empty()) {
        return false;
    }
    for (const auto& rankObject : ExtractObjectsWithKey(content, "device_id")) {
        uint32_t deviceId = 0;
        uint32_t localId = 0;
        if (!JsonUintField(rankObject, "device_id", &deviceId) ||
            !JsonUintField(rankObject, "local_id", &localId)) {
            continue;
        }
        root->deviceToLocalId[deviceId] = localId;
        for (const auto& addressObject : ExtractObjectsWithKey(rankObject, "addr")) {
            std::array<uint8_t, TILEXR_CCU_EID_BYTES> eid {};
            if (!ParseEidHex(JsonStringField(addressObject, "addr"), &eid)) {
                continue;
            }
            for (const auto& port : JsonStringArrayField(addressObject, "ports")) {
                root->portToEidByLocalId[localId][port] = eid;
            }
        }
    }
    return !root->deviceToLocalId.empty();
}

std::vector<TopoEdge> ParseTopoInfo(const std::string& path)
{
    const std::string content = ReadTextFile(path);
    std::vector<TopoEdge> edges;
    for (const auto& edgeObject : ExtractObjectsWithKey(content, "local_a")) {
        TopoEdge edge;
        if (!JsonUintField(edgeObject, "local_a", &edge.localA) ||
            !JsonUintField(edgeObject, "local_b", &edge.localB)) {
            continue;
        }
        edge.localAPorts = JsonStringArrayField(edgeObject, "local_a_ports");
        edge.localBPorts = JsonStringArrayField(edgeObject, "local_b_ports");
        const auto protocols = JsonStringArrayField(edgeObject, "protocols");
        edge.supportsCtp = std::find(protocols.begin(), protocols.end(), "UB_CTP") != protocols.end();
        if (!edge.localAPorts.empty() && !edge.localBPorts.empty()) {
            edges.push_back(edge);
        }
    }
    return edges;
}

bool ResolveLocalPort(
    const std::vector<TopoEdge>& edges,
    uint32_t localId,
    uint32_t peerLocalId,
    std::string* localPort,
    bool* supportsCtp)
{
    if (localPort == nullptr || supportsCtp == nullptr) {
        return false;
    }
    for (const auto& edge : edges) {
        if (edge.localA == localId && edge.localB == peerLocalId) {
            *localPort = edge.localAPorts.front();
            *supportsCtp = edge.supportsCtp;
            return true;
        }
        if (edge.localB == localId && edge.localA == peerLocalId) {
            *localPort = edge.localBPorts.front();
            *supportsCtp = edge.supportsCtp;
            return true;
        }
    }
    return false;
}

int ForcedTpType()
{
    const char* value = std::getenv(TILEXR_CCU_DIRECT_FORCE_TP_TYPE_ENV);
    if (value == nullptr) {
        return -1;
    }
    const std::string text(value);
    if (text == "rtp" || text == "RTP" || text == "0") {
        return static_cast<int>(TILEXR_CCU_HCCP_TP_TYPE_RTP);
    }
    if (text == "ctp" || text == "CTP" || text == "1") {
        return static_cast<int>(TILEXR_CCU_HCCP_TP_TYPE_CTP);
    }
    return -1;
}

} // namespace

int TileXRCcuResolvePeerEidRoutes(
    const std::string& rootInfoPath,
    uint32_t localDevicePhyId,
    const std::vector<uint32_t>& peerDevicePhyIds,
    std::vector<TileXRCcuPeerEidRoute>* routes,
    std::string* message)
{
    if (routes == nullptr) {
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    routes->clear();
    RootInfo root;
    if (!ParseRootInfo(rootInfoPath, &root)) {
        if (message != nullptr) {
            *message = "failed to parse HCCL root info";
        }
        return TILEXR_ERROR_NOT_FOUND;
    }
    const auto localIdIt = root.deviceToLocalId.find(localDevicePhyId);
    if (localIdIt == root.deviceToLocalId.end()) {
        if (message != nullptr) {
            *message = "local physical device is absent from HCCL root info";
        }
        return TILEXR_ERROR_NOT_FOUND;
    }
    const auto edges = ParseTopoInfo(root.topoPath);
    const auto eidMapIt = root.portToEidByLocalId.find(localIdIt->second);
    if (edges.empty() || eidMapIt == root.portToEidByLocalId.end()) {
        if (message != nullptr) {
            *message = "HCCL topology has no local EID routes";
        }
        return TILEXR_ERROR_NOT_FOUND;
    }
    for (const uint32_t peerDevicePhyId : peerDevicePhyIds) {
        const auto peerIdIt = root.deviceToLocalId.find(peerDevicePhyId);
        std::string localPort;
        bool supportsCtp = false;
        if (peerIdIt == root.deviceToLocalId.end() ||
            !ResolveLocalPort(edges, localIdIt->second, peerIdIt->second, &localPort, &supportsCtp)) {
            if (message != nullptr) {
                *message = "HCCL topology has no device-pair edge";
            }
            routes->clear();
            return TILEXR_ERROR_NOT_FOUND;
        }
        const auto eidIt = eidMapIt->second.find(localPort);
        if (eidIt == eidMapIt->second.end()) {
            if (message != nullptr) {
                *message = "HCCL root info has no EID for the selected local port";
            }
            routes->clear();
            return TILEXR_ERROR_NOT_FOUND;
        }
        TileXRCcuPeerEidRoute route;
        route.peerDevicePhyId = peerDevicePhyId;
        route.localEid = eidIt->second;
        route.localPort = localPort;
        const int forcedTpType = ForcedTpType();
        route.tpType = forcedTpType >= 0 ?
            static_cast<uint32_t>(forcedTpType) :
            (supportsCtp ? TILEXR_CCU_HCCP_TP_TYPE_CTP : TILEXR_CCU_HCCP_TP_TYPE_RTP);
        routes->push_back(route);
    }
    if (message != nullptr) {
        *message = "ok";
    }
    return TILEXR_SUCCESS;
}

} // namespace TileXR
