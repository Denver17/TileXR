#include "tilexr_straggler.h"
#include "tilexr_straggler_layout.h"

int main()
{
    TileXRStragglerConfig config {};
    if (TileXRStragglerConfigDefault(&config) != TileXR::TILEXR_SUCCESS) {
        return 1;
    }
    return config.clockSyncSamples == 128u && config.clockSyncWarmupSamples == 32u &&
        config.clockSyncSamples >= TileXR::TILEXR_CLOCK_SYNC_MIN_SAMPLES &&
        sizeof(TileXR::TileXRStragglerDeviceSlot) == 192u ? 0 : 2;
}
