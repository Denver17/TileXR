// Executes the production device writer logic with CPU pointers and a fake
// timestamp intrinsic. Actual CANN compilation and NPU runs are separate tests.
#include <tilexr/profiling/device.h>
#include <tilexr/profiling/direct.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <type_traits>

using namespace TileXR::Profiling;
#define CHECK(expr) do { if (!(expr)) { \
    std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expr); \
    std::abort(); } } while (false)

class AlignedBuffer {
public:
    explicit AlignedBuffer(size_t bytes) : storage_(new unsigned char[bytes + 31])
    {
        data = reinterpret_cast<unsigned char *>((
            reinterpret_cast<uintptr_t>(storage_.get()) + 31U) & ~uintptr_t(31U));
        std::memset(data, 0, bytes);
    }
    unsigned char *data;
private:
    std::unique_ptr<unsigned char[]> storage_;
};

class Workspace {
public:
    explicit Workspace(uint32_t lanes = 2U, uint32_t capacity = 8U)
        : timestamps(size_t(lanes) * capacity * sizeof(uint64_t)),
          ids(size_t(lanes) * capacity * sizeof(uint32_t)),
          counts(size_t(lanes) * COUNT_STRIDE * sizeof(uint32_t)),
          dropped(size_t(lanes) * COUNT_STRIDE * sizeof(uint32_t)),
          states(size_t(lanes) * COUNT_STRIDE * sizeof(uint32_t))
    {
        view = {DEVICE_VIEW_ABI_VERSION, sizeof(DeviceView), 1U, lanes,
            capacity, capacity, COUNT_STRIDE, 0U,
            reinterpret_cast<uint64_t>(timestamps.data),
            reinterpret_cast<uint64_t>(ids.data),
            reinterpret_cast<uint64_t>(counts.data),
            reinterpret_cast<uint64_t>(dropped.data),
            reinterpret_cast<uint64_t>(states.data)};
    }
    GM_ADDR Args() { return reinterpret_cast<GM_ADDR>(&view); }
    uint32_t Count(uint32_t lane = 0) const { return Values(counts)[lane * COUNT_STRIDE]; }
    uint32_t Dropped(uint32_t lane = 0) const { return Values(dropped)[lane * COUNT_STRIDE]; }
    Completion State(uint32_t lane = 0) const
    { return static_cast<Completion>(Values(states)[lane * COUNT_STRIDE]); }
    uint32_t Id(uint32_t record, uint32_t lane = 0) const
    { return Values(ids)[lane * view.recordStride + record]; }
    uint64_t Timestamp(uint32_t record, uint32_t lane = 0) const
    { return reinterpret_cast<uint64_t *>(timestamps.data)[lane * view.recordStride + record]; }
    static uint32_t *Values(const AlignedBuffer &b) { return reinterpret_cast<uint32_t *>(b.data); }
    DeviceView view;
    AlignedBuffer timestamps, ids, counts, dropped, states;
};

static void RecorderCases()
{
    Workspace w;
    Recorder<8> sparse;
    AscendC::ProfilingTestCycle() = 0U;
    sparse.RecordMarker<0, 1>();
    sparse.RecordMarker<3, 7>();
    AscendC::ProfilingTestCycle() = 9U;
    sparse.RecordMarker<7, 8>();
    sparse.RecordMarker<7, 9>(); // Fixed slots replace; they do not append.
    sparse.Flush(w.Args(), 0);
    CHECK(w.State() == Completion::COMMITTED && w.Count() == 3 && w.Dropped() == 0);
    CHECK(w.Id(0) == 1 && w.Id(1) == 7 && w.Id(2) == 9);
    CHECK(w.Timestamp(0) == 0 && w.Timestamp(1) == 0 && w.Timestamp(2) == 9);
    Recorder<1> empty;
    empty.Flush(w.Args(), 1);
    CHECK(w.State(1) == Completion::COMMITTED && w.Count(1) == 0);
    empty.Flush(nullptr, 0);
    sparse.Flush(w.Args(), 0);
    CHECK(w.State() == Completion::INVALID && w.Count() == 3 && w.Id(0) == 1);

    Workspace overflow;
    Recorder<10> full;
    full.RecordMarker<0, 1>(); full.RecordMarker<1, 2>();
    full.RecordMarker<2, 3>(); full.RecordMarker<3, 4>();
    full.RecordMarker<4, 5>(); full.RecordMarker<5, 6>();
    full.RecordMarker<6, 7>(); full.RecordMarker<7, 8>();
    full.RecordMarker<8, 9>(); full.RecordMarker<9, 10>();
    full.Flush(overflow.Args(), 0);
    CHECK(overflow.Count() == 8 && overflow.Dropped() == 2);
    CHECK(overflow.Id(7) == 8 && overflow.State() == Completion::COMMITTED);
}

static void DirectCases()
{
    Workspace w;
    DirectWriter direct;
    direct.Publish(); direct.RecordAt(1, 0); // Uninitialized no-op.
    direct.Initialize(w.Args(), 0);
    CHECK(w.State() == Completion::WRITING);
    direct.RecordAt(7, 10);
    direct.RecordAt(8, 0); // Zero and non-monotonic backfill are valid.
    direct.RecordAt(9, static_cast<uint64_t>(-1));
    for (uint32_t i = 0; i < 9; ++i) { direct.RecordAt(10 + i, 0); }
    direct.Publish(); direct.Publish();
    CHECK(w.Count() == 8 && w.Dropped() == 4 && w.State() == Completion::COMMITTED);
    CHECK(w.Timestamp(1) == 0 && w.Timestamp(2) == static_cast<uint64_t>(-1));
    direct.RecordAt(100, 1); direct.Publish();
    CHECK(w.Count() == 8 && w.Dropped() == 4 && w.Id(0) == 7);
    direct.Initialize(w.Args(), 1); // Success -> success: cursor/dropped reset.
    direct.RecordAt(40, 2); direct.Publish();
    CHECK(w.Count(1) == 1 && w.Dropped(1) == 0 && w.Id(0, 1) == 40);
    direct.Initialize(nullptr, 1); // Success -> failure: old target is inert.
    direct.RecordAt(50, 3); direct.Publish();
    CHECK(w.Count(1) == 1 && w.Id(0, 1) == 40);
    Workspace fresh;
    direct.Initialize(fresh.Args(), 0); // Failure -> success.
    direct.Publish();
    CHECK(fresh.State() == Completion::COMMITTED && fresh.Count() == 0);
    DirectWriter duplicate;
    duplicate.Initialize(w.Args(), 0); duplicate.RecordAt(100, 1); duplicate.Publish();
    CHECK(w.State() == Completion::INVALID && w.Id(0) == 7 && w.Count() == 8);

    Workspace badId;
    direct.Initialize(badId.Args(), 0);
    direct.RecordAt(1, 0); direct.RecordAt(0, 1); direct.Publish();
    CHECK(badId.State() == Completion::INVALID && badId.Count() == 0);

    Workspace abandoned, replacement;
    direct.Initialize(abandoned.Args(), 0);
    direct.RecordAt(70, 1);
    direct.Initialize(nullptr, 0); // Failed reuse while the prior lane is WRITING.
    direct.RecordAt(71, 2); direct.Publish();
    CHECK(abandoned.State() == Completion::WRITING && abandoned.Count() == 0);
    CHECK(abandoned.Id(0) == 70 && abandoned.Id(1) == 0);
    direct.Initialize(replacement.Args(), 0); direct.RecordAt(72, 3);
    direct.Initialize(replacement.Args(), 1); direct.RecordAt(73, 4); direct.Publish();
    CHECK(replacement.State() == Completion::WRITING && replacement.Count() == 0);
    CHECK(replacement.Count(1) == 1 && replacement.Id(0, 1) == 73);
    CHECK(Detail::SaturatingAdd(0xffffffffU, 1) == 0xffffffffU);
    CHECK(Detail::SaturatingAdd(0xfffffffeU, 2) == 0xffffffffU);
    CHECK(Detail::SaturatingAdd(4, 5) == 9);
}

static void InvalidDescriptorCases()
{
    Workspace w;
    const DeviceView good = w.view;
    DirectWriter direct;
    const auto rejected = [&]() {
        direct.Initialize(w.Args(), 0); direct.RecordAt(3, 4); direct.Publish();
        CHECK(w.State() == Completion::EMPTY && w.Count() == 0 && w.Id(0) == 0);
        w.view = good;
    };
    w.view.abiVersion = 9; rejected();
    w.view.descriptorBytes = 0; rejected();
    w.view.enabled = 0; rejected();
    w.view.enabled = 2; rejected();
    w.view.reserved = 1; rejected();
    w.view.logicalStreamCount = 0; rejected();
    w.view.logicalStreamCount = 65537; rejected();
    w.view.recordsPerStream = 0; rejected();
    w.view.recordsPerStream = 7; rejected();
    w.view.recordsPerStream = 65537; rejected();
    w.view.recordStride = 7; rejected();
    w.view.recordStride = 9; rejected();
    w.view.stateStride = 0; rejected();
    w.view.stateStride = 9; rejected();
    w.view.timestampsAddress = 0; rejected();
    w.view.eventIdsAddress += 1; rejected();
    w.view.countsAddress = 0; rejected();
    w.view.droppedAddress = 0; rejected();
    w.view.statesAddress = 0; rejected();
    w.view.timestampsAddress = static_cast<uint64_t>(-32); rejected();
    direct.Initialize(w.Args(), 2); direct.Publish();
    CHECK(w.State() == Completion::EMPTY);
    direct.Initialize(reinterpret_cast<GM_ADDR>(&w.view) + 1, 0); direct.Publish();
    CHECK(w.State() == Completion::EMPTY);
    Workspace::Values(w.counts)[0] = 1;
    direct.Initialize(w.Args(), 0); direct.Publish();
    CHECK(w.State() == Completion::INVALID);

    Workspace writing;
    Workspace::Values(writing.states)[0] = uint32_t(Completion::WRITING);
    direct.Initialize(writing.Args(), 0); direct.Publish();
    CHECK(writing.State() == Completion::INVALID && writing.Count() == 0);
    Workspace dirty;
    Workspace::Values(dirty.dropped)[0] = 1;
    direct.Initialize(dirty.Args(), 0); direct.Publish();
    CHECK(dirty.State() == Completion::INVALID);

    Workspace padded(2, 16);
    padded.view.recordsPerStream = 8; // Physical stride and valid capacity differ.
    direct.Initialize(padded.Args(), 1); direct.RecordAt(7, 0); direct.Publish();
    CHECK(padded.Count(1) == 1 && padded.Id(0, 1) == 7 && padded.State() == Completion::EMPTY);
    Workspace maxCapacity(1, MAX_RECORDS_PER_STREAM);
    direct.Initialize(maxCapacity.Args(), 0); direct.RecordAt(8, 0); direct.Publish();
    CHECK(maxCapacity.Count() == 1 && maxCapacity.State() == Completion::COMMITTED);

    Workspace maxLanes(65536);
    direct.Initialize(maxLanes.Args(), 65535); direct.RecordAt(5, 0); direct.Publish();
    CHECK(maxLanes.State(65535) == Completion::COMMITTED && maxLanes.Count(65535) == 1);
    CHECK(maxLanes.State(0) == Completion::EMPTY && maxLanes.Id(0, 65535) == 5);
    direct.Initialize(maxLanes.Args(), 65536); direct.Publish();
    CHECK(maxLanes.State(0) == Completion::EMPTY);
}

int main()
{
    static_assert(std::is_standard_layout<DeviceView>::value &&
        std::is_trivial<DeviceView>::value, "DeviceView must be POD");
    RecorderCases(); DirectCases(); InvalidDescriptorCases();
    std::puts("profiling CPU device protocol tests passed (not CANN/NPU evidence)");
}
