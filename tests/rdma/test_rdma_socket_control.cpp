#include "rdma/tilexr_rdma_transport.h"
#include "tools/socket/tilexr_sock_exchange.h"
#include <cstdio>
#include <cstdlib>
#include <vector>

using namespace TileXR;
extern "C" int aclrtGetDevice(int32_t* device) { *device = 0; return 0; }
#define REQUIRE(x) do { if (!(x)) { std::fprintf(stderr, "%d: %s\n", __LINE__, #x); std::exit(1); } } while (0)

// This executable links no ACL or real provider. Real TCP carries the production
// transport's endpoint, MR and collective status exchanges between processes.
class CpuProvider final : public RdmaProvider {
    int rank_;
    bool queues_ = false;
    int regions_ = 0;
public:
    explicit CpuProvider(int rank) : rank_(rank) {}
    int Open(const RdmaConfig&) override { return 0; }
    int Create(int, RdmaEndpoint& out) override {
        out.qpn = rank_ + 1; out.psn = rank_ + 101; out.mtu = 4096;
        out.gid[15] = uint8_t(rank_ + 1); queues_ = true; return 0;
    }
    int Connect(int peer, const RdmaEndpoint& remote) override {
        return remote.qpn == uint32_t(peer + 1) && remote.psn == uint32_t(peer + 101) ? 0 : 1;
    }
    int Register(void* ptr, size_t bytes, RdmaMemory& out) override {
        out.address = reinterpret_cast<uintptr_t>(ptr); out.bytes = bytes;
        out.lkey = rank_ + 11; out.rkey = rank_ + 21; ++regions_; return 0;
    }
    int Deregister(RdmaMemory&) override { --regions_; return 0; }
    int Transfer(bool, int, const RdmaMemory&, uint64_t, const RdmaMemory&, uint64_t,
        uint32_t, uint32_t) override { std::abort(); } // no pretend data plane
    int DestroyQueues() override { queues_ = false; return 0; }
    int Close() override { REQUIRE(!queues_ && regions_ == 0); return 0; }
};

int main(int argc, char** argv)
{
    REQUIRE(argc == 2 && (argv[1][0] == '0' || argv[1][0] == '1') && argv[1][1] == '\0');
    const int rank = argv[1][0] - '0';
    TileXRSockExchange socket(rank, 2, 0);
    RdmaOptions options; options.rank = rank; options.rankSize = 2;
    options.allGather = [&](const void* in, size_t bytes, void* out) {
        return socket.AllGather(static_cast<const uint8_t*>(in), bytes, static_cast<uint8_t*>(out), 5000);
    };
    TileXRRDMATransport transport(std::unique_ptr<RdmaProvider>(new CpuProvider(rank)));
    REQUIRE(transport.Init(options) == 0);
    std::vector<uint8_t> memory(64 * 1024, uint8_t(rank));
    for (int round = 0; round < 3; ++round) {
        TileXRRDMAMemHandle handle = 0;
        REQUIRE(transport.Register(memory.data(), memory.size(), &handle) == 0 && handle != 0);
        REQUIRE(transport.Unregister(handle) == 0);
    }
    REQUIRE(transport.Shutdown() == 0);
    std::printf("CPU_CONTROL_PASS rank=%d socket=real_tcp provider=cpu_model npu_calls=0 mr_rounds=3\n", rank);
}
