#include <tilexr/profiling/device.h>
#include <tilexr/profiling/direct.h>
#include <type_traits>

static_assert(sizeof(TileXR::Profiling::DeviceView) == 72, "ABI must not depend on switch");
static_assert(std::is_trivial<TileXR::Profiling::DeviceView>::value, "POD ABI");
#if !TILEXR_PROFILE_ACTIVE
static_assert(sizeof(TileXR::Profiling::Recorder<256>) == 1, "Disabled recorder has no arrays");
static_assert(sizeof(TileXR::Profiling::DirectWriter) == 1, "Disabled writer has no state");
#endif
int main()
{
    TileXR::Profiling::Recorder<1> recorder;
    recorder.RecordMarker<0, 1>(); recorder.Flush(nullptr, 0);
    TileXR::Profiling::DirectWriter direct;
    direct.Initialize(nullptr, 0); direct.RecordAt(1, 0); direct.Publish();
}
