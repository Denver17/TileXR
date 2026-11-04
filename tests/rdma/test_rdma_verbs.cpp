#include "rdma/tilexr_rdma_transport.h"
#include <cassert>
int main()
{
    auto provider = TileXR::MakeRdmaVerbsProvider();
    assert(provider);
    TileXR::RdmaConfig config;
    config.device = "tilexr_missing_test_device";
    assert(provider->Open(config) != TileXR::TILEXR_SUCCESS);
    assert(provider->DestroyQueues() == TileXR::TILEXR_SUCCESS);
    assert(provider->Close() == TileXR::TILEXR_SUCCESS);
    assert(provider->Close() == TileXR::TILEXR_SUCCESS);
}
