// Real SDK writer -> mocked ACL collection -> v1 encoder -> Python reader.
#include "mock_acl_runtime.h"
#include <fstream>

void SubmitRoundTrip(unsigned char *descriptor);

int main(int argc, char **argv)
{
    CHECK(argc == 2);
    Mock::Reset();
    AclCapture capture;
    CaptureConfig config;
    config.rank = 3;
    config.logicalStreamCount = 5;
    config.recordsPerStream = 8;
    config.captureId[0] = 1;
    CHECK(capture.Initialize(config, Mock::expectedStream));
    SubmitRoundTrip(static_cast<unsigned char *>(capture.DeviceArgs()));
    CHECK(capture.Collect());
    CHECK(capture.Snapshot()->streams.size() == 5);
    std::vector<uint8_t> bytes;
    CHECK(EncodeV1(*capture.Snapshot(), &bytes));
    // Transport encoder bytes on every Host platform. Linux's actual
    // no-overwrite publication is independently exercised by test_host.cpp.
    std::ofstream output(argv[1], std::ios::binary);
    output.write(reinterpret_cast<const char *>(bytes.data()), bytes.size());
    output.close();
    CHECK(output);
    CHECK(capture.Close());
    CHECK(Mock::allocations.empty());
}
