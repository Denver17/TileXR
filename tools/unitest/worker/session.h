// Declares per-rank device, stream, communicator and buffer ownership.
// close() is the synchronized release path; failures are handled by Worker exit.
#pragma once
#include "buffer.h"
#include "json.h"
#include <tilexr_api.h>
#include <memory>

namespace unitest {
class Session {
public:
    Session(int rank, int world, int device);
    Buffer &buffer(size_t bytes);
    void sync();
    void close();
    Json info() const;
    const int rank, world, device;
    aclrtStream stream = nullptr;
    TileXRCommPtr comm = nullptr;
private:
    std::string soc_;
    std::vector<std::unique_ptr<Buffer>> buffers_;
};
} // namespace unitest
