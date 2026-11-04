#include "tilexr_rdma.h"
int main(void)
{
    TileXRRDMAInfo info = {0};
    TileXRRDMAMemHandle handle = 0;
    return sizeof(info) != 32 || sizeof(handle) != 8;
}
