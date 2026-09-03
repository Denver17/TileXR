/*
 * Copyright (c) 2026 TileXR Project
 */
#include "ccu/tilexr_ccu_backend.h"

#include "ccu/tilexr_ccu_collective_planner.h"
#include "ccu/tilexr_ccu_executor.h"
#include "ccu/tilexr_ccu_runtime_session.h"

#include <new>

namespace TileXR {

class TileXRCcuBackend::Impl {
public:
    Impl();
    int Init(const TileXRCcuBackendOptions &options);
    void Shutdown();
    bool Available() const;
    bool Supports(const TileXRCcuCollectiveRequest &request) const;
    int PrepareCollective(const TileXRCcuCollectiveRequest &request, TileXRCcuCollectivePlan *plan);
    int SubmitCollective(const TileXRCcuCollectivePlan &plan, aclrtStream stream);
#ifdef TILEXR_CCU_TESTING
    bool RuntimeInitializedForTest() const;
#endif

private:
    int EnsureInternals();

    std::unique_ptr<TileXRCcuRuntimeSession> runtimeSession_;
    std::unique_ptr<TileXRCcuCollectivePlanner> planner_;
    std::unique_ptr<TileXRCcuExecutor> executor_;
};

TileXRCcuBackend::Impl::Impl()
    : runtimeSession_(new (std::nothrow) TileXRCcuRuntimeSession()),
      planner_(new (std::nothrow) TileXRCcuCollectivePlanner()),
      executor_(new (std::nothrow) TileXRCcuExecutor())
{
}

int TileXRCcuBackend::Impl::EnsureInternals()
{
    if (runtimeSession_ == nullptr) {
        runtimeSession_.reset(new (std::nothrow) TileXRCcuRuntimeSession());
    }
    if (planner_ == nullptr) {
        planner_.reset(new (std::nothrow) TileXRCcuCollectivePlanner());
    }
    if (executor_ == nullptr) {
        executor_.reset(new (std::nothrow) TileXRCcuExecutor());
    }
    return runtimeSession_ == nullptr || planner_ == nullptr || executor_ == nullptr ?
        TILEXR_ERROR_INTERNAL :
        TILEXR_SUCCESS;
}

int TileXRCcuBackend::Impl::Init(const TileXRCcuBackendOptions &options)
{
    const int ret = EnsureInternals();
    if (ret != TILEXR_SUCCESS) {
        return ret;
    }
    if (planner_ != nullptr) {
        planner_->Reset();
    }
    return runtimeSession_->Init(options);
}

void TileXRCcuBackend::Impl::Shutdown()
{
    if (planner_ != nullptr) {
        planner_->Reset();
    }
    if (runtimeSession_ != nullptr) {
        runtimeSession_->Shutdown();
    }
}

bool TileXRCcuBackend::Impl::Available() const
{
    return runtimeSession_ != nullptr && runtimeSession_->Available();
}

bool TileXRCcuBackend::Impl::Supports(const TileXRCcuCollectiveRequest &request) const
{
    return runtimeSession_ != nullptr && planner_ != nullptr && planner_->Supports(*runtimeSession_, request);
}

int TileXRCcuBackend::Impl::PrepareCollective(
    const TileXRCcuCollectiveRequest &request,
    TileXRCcuCollectivePlan *plan)
{
    if (runtimeSession_ == nullptr || planner_ == nullptr) {
        return TILEXR_ERROR_INTERNAL;
    }
    return planner_->PrepareCollective(*runtimeSession_, request, plan);
}

int TileXRCcuBackend::Impl::SubmitCollective(const TileXRCcuCollectivePlan &plan, aclrtStream stream)
{
    if (runtimeSession_ == nullptr || executor_ == nullptr) {
        return TILEXR_ERROR_INTERNAL;
    }
    return executor_->Submit(*runtimeSession_, plan, stream);
}

#ifdef TILEXR_CCU_TESTING
bool TileXRCcuBackend::Impl::RuntimeInitializedForTest() const
{
    return Available();
}
#endif

TileXRCcuBackend::TileXRCcuBackend() : impl_(new (std::nothrow) Impl())
{
}

TileXRCcuBackend::~TileXRCcuBackend()
{
    Shutdown();
}

int TileXRCcuBackend::Init(const TileXRCcuBackendOptions &options)
{
    if (impl_ == nullptr) {
        impl_.reset(new (std::nothrow) Impl());
        if (impl_ == nullptr) {
            return TILEXR_ERROR_INTERNAL;
        }
    }
    return impl_->Init(options);
}

void TileXRCcuBackend::Shutdown()
{
    if (impl_ != nullptr) {
        impl_->Shutdown();
    }
}

bool TileXRCcuBackend::Available() const
{
    return impl_ != nullptr && impl_->Available();
}

bool TileXRCcuBackend::Supports(const TileXRCcuCollectiveRequest &request) const
{
    return impl_ != nullptr && impl_->Supports(request);
}

int TileXRCcuBackend::PrepareCollective(const TileXRCcuCollectiveRequest &request, TileXRCcuCollectivePlan *plan)
{
    if (impl_ == nullptr) {
        return TILEXR_ERROR_INTERNAL;
    }
    return impl_->PrepareCollective(request, plan);
}

int TileXRCcuBackend::SubmitCollective(const TileXRCcuCollectivePlan &plan, aclrtStream stream)
{
    if (impl_ == nullptr) {
        return TILEXR_ERROR_INTERNAL;
    }
    return impl_->SubmitCollective(plan, stream);
}

#ifdef TILEXR_CCU_TESTING
bool TileXRCcuBackend::RuntimeInitializedForTest() const
{
    return impl_ != nullptr && impl_->RuntimeInitializedForTest();
}
#endif

} // namespace TileXR
