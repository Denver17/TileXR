// Only this translation unit selects the device branch and CPU Ascend C stub.
#include <tilexr/profiling.h>

void SubmitRoundTrip(unsigned char *descriptor)
{
    using namespace TileXR::Profiling;
    Recorder<8> sparse;
    AscendC::ProfilingTestCycle() = 0;
    sparse.RecordMarker<0, 1>();
    AscendC::ProfilingTestCycle() = 10;
    sparse.RecordMarker<7, 2>();
    sparse.Flush(descriptor, 0);

    DirectWriter backfill;
    backfill.Initialize(descriptor, 1);
    backfill.RecordAt(2, 10);
    backfill.RecordAt(1, 0);
    backfill.RecordAt(3, 0);
    backfill.Publish();
    backfill.Publish();

    DirectWriter overflow;
    overflow.Initialize(descriptor, 2);
    overflow.RecordAt(1, 0);
    for (uint32_t i = 1; i <= 7; ++i) overflow.RecordAt(3, i);
    overflow.RecordAt(2, 10);
    overflow.Publish();

    Recorder<1> empty;
    empty.Flush(descriptor, 3);
    // Lane 4 never submits; collection retains an incomplete empty stream.
}
