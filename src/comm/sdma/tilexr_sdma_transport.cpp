/*
 * Copyright (c) 2024-2026 TileXR Project
 * Licensed under the Apache License, Version 2.0
 */

#include "sdma/tilexr_sdma_transport.h"

#include <cstdlib>
#include <new>
#include <string>

#include "acl/acl_rt.h"
#include "sdma/tilexr_sdma_a5_backend.h"
#include "tilexr_log.h"
#include "tilexr_types.h"

#if TILEXR_HAVE_PTO_SDMA
#include "pto/npu/comm/async/sdma/sdma_workspace_manager.hpp"
#endif

namespace TileXR {

struct TileXRSDMATransport::Impl {
    std::unique_ptr<TileXRA5SDMABackend> a5Backend;
#if TILEXR_HAVE_PTO_SDMA
    pto::comm::sdma::SdmaWorkspaceManager workspaceManager;
    bool ptoInitialized = false;
#endif
};

TileXRSDMATransport::TileXRSDMATransport() = default;

TileXRSDMATransport::~TileXRSDMATransport()
{
    Shutdown();
}

bool TileXRSDMATransport::EnvEnabled()
{
    const char* value = std::getenv("TILEXR_ENABLE_SDMA");
    if (value == nullptr) {
        return false;
    }
    const std::string text(value);
    return text == "1" || text == "true" || text == "TRUE" || text == "on" || text == "ON";
}

int TileXRSDMATransport::Init(const TileXRSDMATransportOptions& options)
{
    Shutdown();
    options_ = options;
    available_ = false;
    workspaceDev_ = nullptr;

    if (!EnvEnabled()) {
        lastStatus_ = SDMAInitStatus::DISABLED_BY_ENV;
        TILEXR_LOG(INFO) << "TileXR SDMA disabled; set TILEXR_ENABLE_SDMA=1 to enable";
        return TILEXR_SUCCESS;
    }

    const char *socName = aclrtGetSocName();
    const detail::SDMABackendKind backend = detail::ClassifySDMABackend(socName);
    if (backend == detail::SDMABackendKind::UNSUPPORTED) {
        lastStatus_ = SDMAInitStatus::PTO_UNAVAILABLE;
        TILEXR_LOG(WARN) << "TileXR SDMA unsupported on SoC "
                         << (socName == nullptr ? "unknown" : socName);
        return TILEXR_SUCCESS;
    }

    impl_.reset(new (std::nothrow) Impl());
    if (impl_ == nullptr) {
        lastStatus_ = SDMAInitStatus::INIT_FAILED;
        TILEXR_LOG(WARN) << "TileXR SDMA implementation allocation failed";
        return TILEXR_SUCCESS;
    }
    if (backend == detail::SDMABackendKind::A5_DIRECT) {
        impl_->a5Backend.reset(new (std::nothrow) TileXRA5SDMABackend());
        if (impl_->a5Backend == nullptr || !impl_->a5Backend->Init(options_.devId)) {
            lastStatus_ = SDMAInitStatus::INIT_FAILED;
            TILEXR_LOG(WARN) << "TileXR A5 direct SDMA unavailable";
            impl_.reset();
            return TILEXR_SUCCESS;
        }
        workspaceDev_ = impl_->a5Backend->GetWorkspaceDev();
        if (workspaceDev_ == nullptr) {
            lastStatus_ = SDMAInitStatus::NULL_WORKSPACE;
            impl_.reset();
            return TILEXR_SUCCESS;
        }
        available_ = true;
        lastStatus_ = SDMAInitStatus::INITIALIZED;
        return TILEXR_SUCCESS;
    }

#if TILEXR_HAVE_PTO_SDMA
    if (!impl_->workspaceManager.Init()) {
        lastStatus_ = SDMAInitStatus::INIT_FAILED;
        TILEXR_LOG(WARN) << "TileXR SDMA workspace manager init failed";
        impl_.reset();
        return TILEXR_SUCCESS;
    }
    impl_->ptoInitialized = true;
    workspaceDev_ = static_cast<GM_ADDR>(impl_->workspaceManager.GetWorkspaceAddr());
    if (workspaceDev_ == nullptr) {
        lastStatus_ = SDMAInitStatus::NULL_WORKSPACE;
        TILEXR_LOG(WARN) << "TileXR SDMA workspace manager returned null workspace";
        impl_->workspaceManager.Finalize();
        impl_.reset();
        return TILEXR_SUCCESS;
    }
    available_ = true;
    lastStatus_ = SDMAInitStatus::INITIALIZED;
    TILEXR_LOG(INFO) << "TileXR SDMA initialized on dev " << options_.devId
                     << ", workspace " << static_cast<void*>(workspaceDev_);
    return TILEXR_SUCCESS;
#else
    lastStatus_ = SDMAInitStatus::PTO_UNAVAILABLE;
    TILEXR_LOG(WARN) << "TileXR SDMA PTO headers unavailable at build time";
    impl_.reset();
    return TILEXR_SUCCESS;
#endif
}

void TileXRSDMATransport::Shutdown()
{
    if (impl_ != nullptr && impl_->a5Backend != nullptr) {
        if (!impl_->a5Backend->Shutdown()) {
            TILEXR_LOG(WARN) << "TileXR A5 SDMA cleanup incomplete";
        }
        impl_->a5Backend.reset();
    }
#if TILEXR_HAVE_PTO_SDMA
    if (impl_ != nullptr && impl_->ptoInitialized) {
        impl_->workspaceManager.Finalize();
        impl_->ptoInitialized = false;
    }
#endif
    impl_.reset();
    available_ = false;
    workspaceDev_ = nullptr;
}

bool TileXRSDMATransport::IsAvailable() const
{
    return available_;
}

GM_ADDR TileXRSDMATransport::GetWorkspaceDev() const
{
    return workspaceDev_;
}

SDMAInitStatus TileXRSDMATransport::GetLastStatus() const
{
    return lastStatus_;
}

} // namespace TileXR
