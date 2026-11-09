/*
 * 将 ElasticMemory Pool 和紧凑 Memory EP 接入 Python。Dispatch 的 Frame 保持输入、
 * token 计数和 communicator；materialize 等待 Pending 完成，再用 at_npu::native::from_blob
 * 包装 hidden/assist，共享 Allocation lease。消费者 stream 登记后，Storage 释放交由
 * pool.cpp 的退休流程处理。Combine 等待完成 event；无法确认访问结束时保留相关 owner。
 * Buffer 同时只保留一个未消费的路由 handle；适配层用于无梯度推理，hidden 支持 FP16/BF16。
 */
#include <torch/extension.h>
#include <c10/core/DeviceGuard.h>
#include <torch_npu/csrc/aten/common/from_blob.h>
#include <torch_npu/csrc/aten/CustomFunctions.h>
#include <torch_npu/csrc/core/npu/NPUStream.h>
#include "tilexr_elastic_ep.h"
#include "tilexr_ep.h"
#include <algorithm>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

namespace py = pybind11;
namespace E = TileXR::Elastic;
namespace {
void Check(int code, const char* operation) {
    TORCH_CHECK(code == 0, operation, " failed: ", code);
}
c10::Device Device(int index) { return c10::Device(c10::DeviceType::PrivateUse1, index); }
void CheckTensor(const at::Tensor& tensor, int device, const char* name) {
    TORCH_CHECK(tensor.defined() && tensor.device() == Device(device), name, " must be on the pool NPU");
    TORCH_CHECK(tensor.is_contiguous(), name, " must be contiguous");
    TORCH_CHECK(at_npu::native::custom_ops::get_npu_format(tensor) == ACL_FORMAT_ND,
                name, " must use NPU ND format; explicitly format_cast it before submission");
    TORCH_CHECK(!tensor.requires_grad(), "ElasticMemory compact EP currently supports inference only: ", name,
                " requires gradients");
}
TileXR::TileXRDataType Dtype(at::ScalarType dtype) {
    TORCH_CHECK(dtype == at::kHalf || dtype == at::kBFloat16, "hidden must be float16 or bfloat16");
    return dtype == at::kHalf ? TileXR::TILEXR_DATA_TYPE_FP16 : TileXR::TILEXR_DATA_TYPE_BFP16;
}
// Retain unsafe resources for process lifetime; a failed event is not permission to free HBM.
void Quarantine(std::shared_ptr<void> owner) noexcept {
    static auto* owners = new std::vector<std::shared_ptr<void>>;
    static std::mutex mutex;
    std::lock_guard<std::mutex> lock(mutex);
    owners->push_back(std::move(owner));
}
py::dict Stats(const E::Stats& stats) {
    py::dict out;
    out["live_bytes"] = stats.liveBytes;
    out["pending_retire_bytes"] = stats.pendingRetireBytes;
    out["free_bytes"] = stats.freeBytes;
    out["committed_bytes"] = stats.committedBytes;
    out["reclaimable_segment_bytes"] = stats.reclaimableSegmentBytes;
    out["largest_free_range"] = stats.largestFreeRange;
    out["metadata_bytes"] = stats.metadataBytes;
    out["peak_committed_bytes"] = stats.peakCommittedBytes;
    out["growth_count"] = stats.growthCount;
    out["trim_count"] = stats.trimCount;
    out["failure_count"] = stats.failureCount;
    out["blocked_free_bytes"] = stats.blockedFreeBytes;
    out["padding_bytes"] = stats.paddingBytes;
    return out;
}
struct TorchPool {
    int device;
    std::shared_ptr<E::Pool> pool;
    std::mutex bindingMutex;
    bool bound = false;
    TorchPool(int index, uint64_t maximum, uint64_t cache, uint64_t segment, uint32_t pending) : device(index) {
        c10::DeviceGuard guard(Device(device));
        E::Config config;
        config.maxCommittedBytes = maximum;
        config.maxCachedFreeBytes = cache;
        config.segmentBytes = segment;
        config.maxPending = pending;
        pool = E::Pool::Create(config);
    }
};
struct BufferState {
    std::shared_ptr<TorchPool> pool;
    TileXRCommPtr comm = nullptr;
    int rank, world;
    uint64_t stagingBytes, peerWindowBytes = 0;
    std::mutex mutex;
    uint64_t nextCall = 1, activeCall = 0;
    bool poisoned = false;
    BufferState(std::shared_ptr<TorchPool> source, int r, int w, int domain, int stagingMiB)
        : pool(std::move(source)), rank(r), world(w), stagingBytes(uint64_t(stagingMiB) << 20) {
        TORCH_CHECK(world > 0 && rank >= 0 && rank < world, "invalid EP rank/world_size");
        TORCH_CHECK(stagingMiB >= 2048, "ordinary Memory EP requires at least 2048 MiB peer staging per rank");
        std::lock_guard<std::mutex> binding(pool->bindingMutex);
        TORCH_CHECK(!pool->bound, "an ElasticMemory pool is bound to one ordered communication domain");
        c10::DeviceGuard guard(Device(pool->device));
        Check(TileXRCommInitRankMemoryDomainWithSize(domain, stagingMiB, world, rank, &comm),
              "TileXRCommInitRankMemoryDomainWithSize");
        TileXR::CommArgs* arguments = nullptr;
        const int inspected = TileXRGetCommArgsHost(comm, arguments);
        if (inspected || !arguments) {
            (void)TileXRCommDestroy(comm);
            comm = nullptr;
            TORCH_CHECK(false, "cannot inspect initialized Memory communicator");
        }
        peerWindowBytes = arguments->peerMemBytes;
        pool->bound = true;
    }
    ~BufferState() {
        if (!comm) return;
        try {
            c10::DeviceGuard guard(Device(pool->device));
            (void)TileXRCommDestroy(comm);
        } catch (...) { /* Context teardown is owned by the application; never reset its device. */ }
    }
    uint64_t Begin() {
        std::lock_guard<std::mutex> lock(mutex);
        TORCH_CHECK(!poisoned, "compact EP communication domain is poisoned");
        TORCH_CHECK(!activeCall, "combine or discard the preceding compact Dispatch handle before submitting again");
        activeCall = nextCall++;
        return activeCall;
    }
    void Release(uint64_t id) {
        std::lock_guard<std::mutex> lock(mutex);
        if (activeCall == id) activeCall = 0;
    }
    void Fail() { std::lock_guard<std::mutex> lock(mutex); poisoned = true; }
};
struct Frame {
    std::shared_ptr<BufferState> buffer;
    uint64_t call;
    at::Tensor input, routes, expertCounts, sendCounts;
    int64_t bs, width, topK, experts;
    bool combined = false;
    std::mutex useMutex;
    ~Frame() { buffer->Release(call); }
};
struct TorchLease {
    std::shared_ptr<E::Allocation> allocation;
    int device;
    std::mutex mutex;
    std::vector<c10_npu::NPUStream> consumers;
    TorchLease(std::shared_ptr<E::Allocation> value, int index) : allocation(std::move(value)), device(index) {}
    ~TorchLease() {
        try {
            // Storage destruction may run on Torch's own task-queue thread. Defer flushing
            // to the native pool service, outside its lock; never wait on that queue here.
            allocation->SetRetireHook([streams = std::move(consumers), index = device] {
                c10::DeviceGuard guard(Device(index));
                for (const auto& stream : streams) (void)stream.stream(true);
            });
        } catch (...) { Quarantine(allocation); }
    }
    void RecordCurrent(bool seal) {
        if (!allocation->Bytes()) return;
        c10::DeviceGuard guard(Device(device));
        const auto stream = c10_npu::getCurrentNPUStream(device);
        aclrtStream raw = nullptr;
        try { raw = stream.stream(true); }
        catch (...) { Quarantine(allocation); throw; }
        std::lock_guard<std::mutex> lock(mutex);
        if (std::find(consumers.begin(), consumers.end(), stream) == consumers.end()) {
            TORCH_CHECK(consumers.size() < 128, "ElasticMemory consumer stream metadata is full");
            allocation->RecordStream(raw);
            consumers.push_back(stream);
        }
        if (seal) {
            aclrtEvent event = nullptr;
            Check(aclrtCreateEvent(&event), "aclrtCreateEvent consumer");
            const int recorded = aclrtRecordEvent(event, raw);
            if (recorded) {
                Quarantine(allocation);
                (void)aclrtDestroyEvent(event);
                Check(recorded, "aclrtRecordEvent consumer");
            }
            try { allocation->RecordEvent(event); }
            catch (...) { (void)aclrtDestroyEvent(event); throw; }
        }
    }
};
struct Result {
    std::shared_ptr<Frame> frame;
    std::shared_ptr<TorchLease> lease;
    at::Tensor hidden, assist;
    void RecordCurrent(bool seal) { lease->RecordCurrent(seal); }
    void Discard() {
        std::lock_guard<std::mutex> lock(frame->useMutex);
        frame->buffer->Release(frame->call);
        frame->combined = true;
    }
};
struct TorchPending {
    std::shared_ptr<E::Pending> pending;
    std::shared_ptr<Frame> frame;
    std::mutex mutex;
    std::shared_ptr<Result> result;
    std::shared_ptr<Result> Materialize() {
        std::lock_guard<std::mutex> lock(mutex);
        c10::DeviceGuard guard(Device(frame->buffer->pool->device));
        if (!result) {
            std::shared_ptr<E::Allocation> allocation;
            try { allocation = pending->Wait(); }
            catch (...) { frame->buffer->Fail(); throw; }
            auto next = std::make_shared<Result>();
            next->frame = frame;
            next->lease = std::make_shared<TorchLease>(allocation, frame->buffer->pool->device);
            const int64_t rows = static_cast<int64_t>(allocation->Rows());
            const auto options = frame->input.options().requires_grad(false);
            if (rows) {
                const uint64_t offset = E::CompactAssistOffset(rows, frame->width);
                TORCH_CHECK(offset <= allocation->Bytes() && uint64_t(rows) <= (allocation->Bytes() - offset) / 12,
                            "compact EP allocation does not contain complete hidden/assist storage");
                auto keep = next->lease;
                next->hidden = at_npu::native::from_blob(allocation->Data(), {rows, frame->width},
                    [keep](void*) {}, options, Device(keep->device));
                next->assist = at_npu::native::from_blob(static_cast<char*>(allocation->Data()) + offset,
                    {rows, 3}, [keep](void*) {}, options.dtype(at::kInt), Device(keep->device));
            } else {
                next->hidden = at::empty({0, frame->width}, options);
                next->assist = at::empty({0, 3}, options.dtype(at::kInt));
            }
            result = std::move(next);
        }
        result->RecordCurrent(false);
        return result;
    }
};
std::shared_ptr<TorchPending> Dispatch(const std::shared_ptr<BufferState>& buffer,
                                       at::Tensor hidden, at::Tensor routes, int64_t experts) {
    const int device = buffer->pool->device;
    c10::DeviceGuard guard(Device(device));
    CheckTensor(hidden, device, "hidden");
    CheckTensor(routes, device, "topk_experts");
    const auto dtype = Dtype(hidden.scalar_type());
    TORCH_CHECK(hidden.dim() == 2 && hidden.size(0) > 0 && hidden.size(1) > 0, "hidden must have shape [positive bs, h]");
    TORCH_CHECK(routes.dim() == 2 && routes.size(0) == hidden.size(0) && routes.size(1) > 0 &&
                routes.scalar_type() == at::kInt, "topk_experts must be contiguous int32[bs, top_k]");
    TORCH_CHECK(experts > 0 && experts % buffer->world == 0 && routes.size(1) <= experts,
                "num_experts must be divisible by world_size and cover top_k");
    const uint64_t call = buffer->Begin();
    auto frame = std::make_shared<Frame>();
    frame->buffer = buffer;
    frame->call = call;
    frame->input = std::move(hidden);
    frame->routes = std::move(routes);
    frame->bs = frame->input.size(0);
    frame->width = frame->input.size(1);
    frame->topK = frame->routes.size(1);
    frame->experts = experts;
    frame->expertCounts = at::zeros({experts / buffer->world}, frame->input.options().dtype(at::kLong));
    frame->sendCounts = at::zeros({experts}, frame->input.options().dtype(at::kInt));
    E::CompactParams parameters;
    parameters.comm = buffer->comm;
    parameters.x = frame->input.data_ptr();
    parameters.expertIds = frame->routes.data_ptr<int32_t>();
    parameters.expertTokenNums = frame->expertCounts.data_ptr<int64_t>();
    parameters.sendCounts = frame->sendCounts.data_ptr<int32_t>();
    parameters.bs = frame->bs;
    parameters.h = frame->width;
    parameters.topK = frame->topK;
    parameters.experts = experts;
    parameters.dtype = dtype;
    parameters.keepAlive = frame;
    // stream(true) drains Torch's CPU task queue before native ACL records input readiness.
    const auto stream = c10_npu::getCurrentNPUStream(device).stream(true);
    auto pending = std::make_shared<TorchPending>();
    pending->frame = frame;
    try { pending->pending = E::DispatchCompact(*buffer->pool->pool, parameters, stream); }
    catch (...) { buffer->Fail(); throw; }
    return pending;
}
at::Tensor Combine(const std::shared_ptr<BufferState>& buffer, const std::shared_ptr<Result>& result,
                   at::Tensor expertOutput, at::Tensor weights) {
    auto frame = result->frame;
    std::lock_guard<std::mutex> useLock(frame->useMutex);
    TORCH_CHECK(frame->buffer == buffer && !frame->combined, "invalid or consumed compact Combine handle");
    const int device = buffer->pool->device;
    c10::DeviceGuard guard(Device(device));
    CheckTensor(expertOutput, device, "expert_output");
    CheckTensor(weights, device, "topk_weights");
    TORCH_CHECK(expertOutput.sizes() == result->hidden.sizes() && expertOutput.scalar_type() == frame->input.scalar_type(),
                "expert_output must preserve the compact hidden shape and dtype");
    TORCH_CHECK(weights.scalar_type() == at::kFloat && weights.dim() == 2 &&
                weights.size(0) == frame->bs && weights.size(1) == frame->topK,
                "topk_weights must be float32[bs, top_k]");
    auto output = at::empty({frame->bs, frame->width}, frame->input.options());
    // Legacy Memory Combine requires nonnull pointers even if this rank received zero rows.
    // These bounded sentinels are never exposed as output or substituted for compact storage.
    auto emptyExpert = expertOutput.numel() ? expertOutput : at::empty({1}, expertOutput.options());
    auto emptyAssist = result->assist.numel() ? result->assist : at::empty({3}, result->assist.options());
    auto raw = c10_npu::getCurrentNPUStream(device).stream(true);
    result->RecordCurrent(false);
    // Retain every source and destination even on a partial launch / unconfirmed stop.
    struct Owners { std::shared_ptr<Result> result; at::Tensor x, weights, output, emptyX, emptyAssist; };
    auto owners = std::make_shared<Owners>(Owners{result, expertOutput, weights, output, emptyExpert, emptyAssist});
    const int launched = TileXRMoeEpCombineMemory(emptyExpert.data_ptr(), emptyAssist.data_ptr<int32_t>(),
        frame->sendCounts.data_ptr<int32_t>(), weights.data_ptr<float>(), nullptr, 0, nullptr, buffer->comm,
        frame->bs, frame->width, frame->topK, frame->experts, buffer->world, buffer->rank, 1, 0, 0, 0, 0, 0,
        frame->bs * buffer->world, output.data_ptr(), Dtype(expertOutput.scalar_type()), raw);
    aclrtEvent done = nullptr;
    const int created = aclrtCreateEvent(&done);
    const int recorded = created ? created : aclrtRecordEvent(done, raw);
    const int completed = recorded ? recorded : aclrtSynchronizeEvent(done);
    if (completed) {
        buffer->Fail();
        Quarantine(owners);
        Check(completed, "compact Combine completion");
    }
    const int destroyed = aclrtDestroyEvent(done);
    if (destroyed) { buffer->Fail(); Check(destroyed, "aclrtDestroyEvent Combine"); }
    if (launched) { buffer->Fail(); Check(launched, "TileXRMoeEpCombineMemory"); }
    frame->combined = true;
    buffer->Release(frame->call);
    return output;
}
} // namespace

PYBIND11_MODULE(TORCH_EXTENSION_NAME, module) {
    py::class_<TorchPool, std::shared_ptr<TorchPool>>(module, "Pool")
        .def(py::init<int, uint64_t, uint64_t, uint64_t, uint32_t>(), py::call_guard<py::gil_scoped_release>())
        .def("snapshot", [](TorchPool& self) { return Stats(self.pool->Snapshot()); })
        .def("collect", [](TorchPool& self) { return Stats(self.pool->Collect()); })
        .def("trim", [](TorchPool& self, bool wait) {
            E::Stats result;
            { py::gil_scoped_release unlock; result = self.pool->Trim(wait); }
            return Stats(result);
        })
        .def("close", [](TorchPool& self) {
            E::Stats result;
            { py::gil_scoped_release unlock; result = self.pool->Close(); }
            return Stats(result);
        });
    py::class_<BufferState, std::shared_ptr<BufferState>>(module, "Buffer")
        .def(py::init<std::shared_ptr<TorchPool>, int, int, int, int>(), py::call_guard<py::gil_scoped_release>())
        .def("dispatch", &Dispatch, py::call_guard<py::gil_scoped_release>())
        .def("combine", &Combine, py::call_guard<py::gil_scoped_release>())
        .def_readonly("staging_bytes", &BufferState::stagingBytes)
        .def_readonly("peer_window_bytes", &BufferState::peerWindowBytes);
    py::class_<TorchPending, std::shared_ptr<TorchPending>>(module, "Pending")
        .def("materialize", &TorchPending::Materialize, py::call_guard<py::gil_scoped_release>());
    py::class_<Result, std::shared_ptr<Result>>(module, "Result")
        .def_readonly("hidden", &Result::hidden)
        .def_readonly("assist", &Result::assist)
        .def_property_readonly("expert_counts", [](const Result& self) { return self.frame->expertCounts; })
        .def_property_readonly("send_counts", [](const Result& self) { return self.frame->sendCounts; })
        .def_property_readonly("allocation_address", [](const Result& self) {
            return reinterpret_cast<uint64_t>(self.lease->allocation->Data());
        })
        .def("record_current", &Result::RecordCurrent, py::call_guard<py::gil_scoped_release>())
        .def("discard", &Result::Discard, py::call_guard<py::gil_scoped_release>());
}
