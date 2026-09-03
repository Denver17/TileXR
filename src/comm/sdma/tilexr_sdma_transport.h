/*
 * Copyright (c) 2024-2026 TileXR Project
 * Licensed under the Apache License, Version 2.0
 */

#ifndef TILEXR_SDMA_TRANSPORT_H
#define TILEXR_SDMA_TRANSPORT_H

#include <cstring>
#include <memory>

#include "comm_args.h"
#include "tilexr_sdma_types.h"

namespace TileXR {

namespace detail {

enum class SDMABackendKind : uint32_t {
    UNSUPPORTED = 0U,
    PTO = 1U,
    A5_DIRECT = 2U,
};

inline bool SDMASocHasPrefix(const char *socName, const char *prefix)
{
    return socName != nullptr &&
        std::strncmp(socName, prefix, std::strlen(prefix)) == 0;
}

inline SDMABackendKind ClassifySDMABackend(const char *socName)
{
    if (SDMASocHasPrefix(socName, "Ascend950")) {
        return SDMABackendKind::A5_DIRECT;
    }
    if (SDMASocHasPrefix(socName, "Ascend910B") ||
        SDMASocHasPrefix(socName, "Ascend910A") ||
        SDMASocHasPrefix(socName, "Ascend910_93")) {
        return SDMABackendKind::PTO;
    }
    return SDMABackendKind::UNSUPPORTED;
}

} // namespace detail

struct TileXRSDMATransportOptions {
    int devId = 0;
};

class TileXRSDMATransport {
public:
    TileXRSDMATransport();
    ~TileXRSDMATransport();
    TileXRSDMATransport(const TileXRSDMATransport&) = delete;
    TileXRSDMATransport& operator=(const TileXRSDMATransport&) = delete;

    int Init(const TileXRSDMATransportOptions& options);
    void Shutdown();

    bool IsAvailable() const;
    GM_ADDR GetWorkspaceDev() const;
    SDMAInitStatus GetLastStatus() const;

private:
    struct Impl;

    static bool EnvEnabled();

    TileXRSDMATransportOptions options_ {};
    bool available_ = false;
    GM_ADDR workspaceDev_ = nullptr;
    SDMAInitStatus lastStatus_ = SDMAInitStatus::DISABLED_BY_ENV;
    std::unique_ptr<Impl> impl_;
};

} // namespace TileXR

#endif // TILEXR_SDMA_TRANSPORT_H
