/* Copyright (c) 2026 TileXR Project. Licensed under the Apache License, Version 2.0. */

/*
 * 功能：读取 RDMA 环境变量，校验 NIC 名称、数值范围及 AIV/doorbell 配置组合。
 * 调用关系：TileXRComm::InitRDMA 调用 LoadRdmaConfig，将结果传给 RdmaProvider 初始化。
 *
 * Purpose: Parse RDMA environment settings and validate NIC names, numeric ranges and AIV/doorbell combinations.
 * Calls: TileXRComm::InitRDMA calls LoadRdmaConfig and passes the configuration to provider initialization.
 */
#include "rdma/tilexr_rdma_transport.h"
#include <cerrno>
#include <cstdlib>
#include <limits>

namespace TileXR {
namespace {
int ReadNumber(const char* name, uint32_t low, uint32_t high, uint32_t& out,
    std::string& error)
{
    const char* text = std::getenv(name);
    if (text == nullptr) { return TILEXR_SUCCESS; }
    if (*text < '0' || *text > '9') { error = name; return TILEXR_ERROR_PARA_CHECK_FAIL; }
    char* end = nullptr;
    errno = 0;
    const unsigned long value = std::strtoul(text, &end, 10);
    if (errno != 0 || *end != '\0' || value < low || value > high) {
        error = name;
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    out = static_cast<uint32_t>(value);
    return TILEXR_SUCCESS;
}
}
int LoadRdmaConfig(RdmaConfig& config, std::string& error)
{
    config = RdmaConfig {};
    error.clear();
    const char* device = std::getenv("TILEXR_RDMA_DEVICE");
    if (device == nullptr || *device == '\0') {
        error = "TILEXR_RDMA_DEVICE must explicitly select the local NIC";
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    config.device = device;
    if (config.device.size() > 63 || config.device.find_first_not_of(
        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-.") != std::string::npos ||
        config.device == "." || config.device == "..") {
        error = "TILEXR_RDMA_DEVICE";
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    uint32_t gidIndex = std::numeric_limits<uint32_t>::max();
    struct Setting { const char* name; uint32_t low; uint32_t high; uint32_t* value; };
    const Setting settings[] = {
        {"TILEXR_RDMA_PORT", 1, 255, &config.port},
        {"TILEXR_RDMA_GID_INDEX", 0, 255, &gidIndex},
        {"TILEXR_RDMA_MTU", 256, 4096, &config.mtu},
        {"TILEXR_RDMA_TRAFFIC_CLASS", 0, 255, &config.trafficClass},
        {"TILEXR_RDMA_HOP_LIMIT", 1, 255, &config.hopLimit},
        {"TILEXR_RDMA_TIMEOUT_MS", 1, 600000, &config.timeoutMs},
    };
    for (const auto& setting : settings) {
        if (ReadNumber(setting.name, setting.low, setting.high, *setting.value, error) != TILEXR_SUCCESS) {
            return TILEXR_ERROR_PARA_CHECK_FAIL;
        }
    }
    if ((config.mtu & (config.mtu - 1)) != 0) {
        error = "TILEXR_RDMA_MTU must be 256, 512, 1024, 2048 or 4096";
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    if (gidIndex != std::numeric_limits<uint32_t>::max()) {
        config.gidIndex = static_cast<int>(gidIndex);
    }
    config.indepUb = config.device.compare(0, 5, "hrn5_") == 0;
    const char* dmaMode = std::getenv("TILEXR_RDMA_DMA_MODE");
    if (dmaMode != nullptr) {
        const std::string mode(dmaMode);
        if (mode == "indep_ub") { config.indepUb = true; }
        else if (mode == "default") { config.indepUb = false; }
        else { error = "TILEXR_RDMA_DMA_MODE"; return TILEXR_ERROR_PARA_CHECK_FAIL; }
    }
    const char* submit = std::getenv("TILEXR_RDMA_MODE");
    if (submit != nullptr) {
        const std::string mode(submit);
        if (mode == "aiv") { config.aivDirect = true; }
        else if (mode != "host") { error = "TILEXR_RDMA_MODE"; return TILEXR_ERROR_PARA_CHECK_FAIL; }
    }
    const char* doorbell = std::getenv("TILEXR_RDMA_DB_MODE");
    if (doorbell != nullptr) {
        const std::string mode(doorbell);
        if (mode == "soft") { config.doorbellMode = 1; }
        else if (mode == "hard") { config.doorbellMode = 2; }
        else { error = "TILEXR_RDMA_DB_MODE"; return TILEXR_ERROR_PARA_CHECK_FAIL; }
    }
    if (config.aivDirect && (!config.indepUb || config.doorbellMode == 0)) {
        error = "AIV RDMA requires indep_ub and explicit TILEXR_RDMA_DB_MODE=soft|hard matching firmware";
        return TILEXR_ERROR_PARA_CHECK_FAIL;
    }
    return TILEXR_SUCCESS;
}
} // namespace TileXR
