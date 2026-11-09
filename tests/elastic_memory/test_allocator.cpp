/*
 * 在 Host 上直接调用生产 allocator.h 的环形分配算术，不启动 ACL 或 NPU kernel。
 * 固定用例检查地址对齐、连续空间、绕回 padding、handle 代次、空环归一化及溢出；
 * 随机用例逐字节枚举候选位置，与分配结果交叉核对。测试自行模拟乱序退休，只有
 * 连续的安全前缀推进 reclaim；失败申请必须保留原 RingState 并清空返回值。
 * 本文件覆盖分配算术，不覆盖 Host/device mailbox 可见性或 stream 退休事件。
 */
#include "allocator.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cstdio>
#include <cstring>
#include <deque>
#include <random>
using namespace TileXR::Elastic;
namespace {
RingState Ring(uint64_t address = 4096, uint64_t capacity = 1024)
{
    RingState state{};
    state.segment = Segment{1, address, capacity};
    return state;
}
Handle Allocate(RingState& state, uint64_t bytes, uint64_t alignment, uint64_t generation,
                uint64_t expectedAddress)
{
    Handle handle{}; uint64_t address = 0;
    assert(TryAllocateRing(state, bytes, alignment, generation, &handle, &address) == Success);
    assert(address == expectedAddress && handle.segmentId == state.segment.id);
    assert(handle.offset == address - state.segment.address && handle.bytes == bytes);
    assert(handle.generation == generation && address % alignment == 0);
    return handle;
}
void ExpectFailure(RingState& state, uint64_t bytes, uint64_t alignment, uint64_t generation,
                   Error expected)
{
    const RingState before = state;
    Handle handle{1, 2, 3, 4}; uint64_t address = 9;
    assert(TryAllocateRing(state, bytes, alignment, generation, &handle, &address) == expected);
    assert(IsEmpty(handle) && address == 0 && std::memcmp(&state, &before, sizeof(state)) == 0);
}
void ZeroAndIdentity()
{
    RingState state{};
    Handle empty{1, 2, 3, 4}; uint64_t address = 9;
    const RingState before = state;
    assert(TryAllocateRing(state, 0, 64, 0, &empty, &address) == Success);
    assert(IsEmpty(empty) && address == 0 && std::memcmp(&before, &state, sizeof(state)) == 0);
    ExpectFailure(state, 1, 1, 1, Capacity);
    state = Ring();
    Handle first = Allocate(state, 1024, 1, 7, 4096);
    assert(state.tail == 1024 && state.tailOffset == 0);
    ExpectFailure(state, 1, 1, 8, Capacity);
    state.reclaim = state.tail;
    Handle replacement = Allocate(state, 1024, 1, 8, 4096);
    assert(!SameHandle(first, replacement) && SameHandle(first, first));
    assert(state.head == 0 && state.reclaim == 0 && state.tail == 1024);
    // Generation uniqueness is a Host request-ID contract, not a Device handle-table scan.
    state.reclaim = state.tail;
    Allocate(state, 1, 1, UINT64_MAX, 4096);
}
void WrapAndPrefixRetirement()
{
    RingState state = Ring();
    Allocate(state, 256, 64, 1, 4096); // A
    Allocate(state, 384, 64, 2, 4352); // B
    Allocate(state, 256, 64, 3, 4736); // C, tail=896
    // Releasing B/C cannot advance reclaim while A remains live.
    ExpectFailure(state, 256, 64, 4, Capacity);
    state.reclaim = 640; // A and B are now safe, C remains live.
    Allocate(state, 256, 64, 4, 4096); // reserve suffix padding=128, wrap
    assert(state.head == 640 && state.tail == 1280 && state.tailOffset == 256);
    Allocate(state, 384, 64, 5, 4352); // exactly fills remaining reservation window
    assert(state.tail - state.head == 1024 && state.tailOffset == 640);
    ExpectFailure(state, 1, 1, 6, Capacity);
    state.reclaim = 896; // C safe; its space can be reused without crossing live D/E.
    Allocate(state, 256, 64, 6, 4736);
    assert(state.tailOffset == 896 && state.tail - state.head == 1024);
    // A successful allocation cannot accept a regressed retirement prefix.
    state.reclaim = state.head - 1;
    ExpectFailure(state, 1, 1, 7, Invalid);
}
void BaseAlignmentAndCapacity()
{
    RingState state = Ring(4097, 1024);
    Allocate(state, 256, 256, 1, 4352); // base padding=255
    assert(state.tail == 511);
    Allocate(state, 256, 256, 2, 4608);
    Allocate(state, 128, 256, 3, 4864);
    state.reclaim = 767;
    Allocate(state, 256, 256, 4, 4352); // suffix=129, base padding=255
    assert(state.tail == 1535 && state.tailOffset == 511);
    state.reclaim = state.tail;
    Allocate(state, 1024, 1, 5, 4097); // an empty ring rebases, no old suffix lost
    state = Ring(4097, 63);
    ExpectFailure(state, 1, 64, 1, Capacity);
    state = Ring(4097, 64);
    Allocate(state, 1, 64, 1, 4160);
    assert(state.tail == 64 && state.tailOffset == 0);
}
void InvalidAndOverflow()
{
    RingState state = Ring();
    ExpectFailure(state, 1, 0, 1, Invalid);
    ExpectFailure(state, 1, 3, 1, Invalid);
    ExpectFailure(state, 1, 1, 0, Invalid);
    ExpectFailure(state, UINT64_MAX, 64, 1, Capacity);
    Handle handle{1, 2, 3, 4}; uint64_t address = 9;
    assert(TryAllocateRing(state, 1, 1, 1, nullptr, &address) == Invalid && address == 0);
    assert(TryAllocateRing(state, 1, 1, 1, &handle, nullptr) == Invalid && IsEmpty(handle));
    state.segment.address = UINT64_MAX - 16;
    ExpectFailure(state, 1, 1, 1, Invalid);
    state = Ring(UINT64_MAX - 255, 255);
    Allocate(state, 128, 128, 1, UINT64_MAX - 255);
    ExpectFailure(state, 128, 128, 2, Capacity);
    // A high alignment must not overflow its rounding addition.
    state = Ring((uint64_t(1) << 63) + 1, 1024);
    ExpectFailure(state, 1, uint64_t(1) << 63, 1, Capacity);
    state = Ring(); state.tail = 1025;
    ExpectFailure(state, 1, 1, 1, Invalid);
    state = Ring(); state.reclaim = 1;
    ExpectFailure(state, 1, 1, 1, Invalid);
    state = Ring(); state.head = 1;
    ExpectFailure(state, 1, 1, 1, Invalid);
    state = Ring(); state.tailOffset = 1024;
    ExpectFailure(state, 1, 1, 1, Invalid);
    state = Ring(); state.segment.id = 0;
    ExpectFailure(state, 1, 1, 1, Invalid);
    state = Ring(); state.segment.address = 0;
    ExpectFailure(state, 1, 1, 1, Invalid);
    state = Ring(); state.segment.bytes = 0;
    ExpectFailure(state, 1, 1, 1, Invalid);
    state = Ring(); state.head = state.reclaim = UINT64_MAX - 128;
    state.tail = UINT64_MAX - 64; state.tailOffset = 959;
    ExpectFailure(state, 65, 1, 1, Invalid); // logical end overflows despite physical capacity
    state.reclaim = state.tail;
    Allocate(state, 1024, 1, 2, 4096); // empty normalization safely avoids counter overflow
}
struct Reservation { uint64_t end; bool retired; };
void RandomizedByteOracle()
{
    std::mt19937 random(0x51a7);
    for (uint64_t base : {uint64_t(4096), uint64_t(4101)}) {
        RingState state = Ring(base, 257);
        std::deque<Reservation> reservations;
        for (uint64_t generation = 1; generation <= 10000; ++generation) {
            // Retirement can be out of order, but only the safe prefix grants credit.
            if (!reservations.empty() && random() % 2 == 0)
                reservations[random() % reservations.size()].retired = true;
            while (!reservations.empty() && reservations.front().retired) {
                state.reclaim = reservations.front().end;
                reservations.pop_front();
            }
            const uint64_t bytes = 1 + random() % 260;
            const uint64_t alignment = uint64_t(1) << (random() % 7);
            const bool empty = reservations.empty();
            const uint64_t tail = empty ? 0 : state.tail;
            const uint64_t position = empty ? 0 : state.tailOffset;
            const uint64_t head = empty ? 0 : state.reclaim;
            const uint64_t available = 257 - (tail - head);
            uint64_t distance = 0, expectedOffset = 0;
            bool found = false;
            // Enumerate every physically contiguous candidate; unlike the implementation,
            // the oracle uses division and a loop rather than suffix/wrap arithmetic.
            for (; distance <= available; ++distance) {
                const uint64_t offset = (position + distance) % 257;
                if ((base + offset) % alignment == 0 && bytes <= 257 - offset &&
                    bytes <= available - distance) {
                    expectedOffset = offset; found = true; break;
                }
            }
            if (!found) {
                ExpectFailure(state, bytes, alignment, generation, Capacity);
                continue;
            }
            Allocate(state, bytes, alignment, generation, base + expectedOffset);
            assert(state.head == head && state.tail == tail + distance + bytes);
            assert(state.tailOffset == (position + distance + bytes) % 257);
            reservations.push_back(Reservation{state.tail, false});
        }
    }
}
}
int main()
{
    ZeroAndIdentity(); WrapAndPrefixRetirement(); BaseAlignmentAndCapacity();
    InvalidAndOverflow(); RandomizedByteOracle();
    std::printf("ElasticMemory allocator behavioral tests passed (segmented ring ABI v%llu)\n",
                static_cast<unsigned long long>(kAbiVersion));
}
